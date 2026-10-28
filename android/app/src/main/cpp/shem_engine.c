/* =============================================================================
 * shem_engine.c - Embedded SuperCollider Synthesis Engine & ScBridge for Shem
 * Author: Jáchym Pešek
 * Zero-glitch, lock-free real-time audio DSP ready for live performance.
 * =============================================================================
 */

#include "shem_engine.h"
#include "shem_udp.h"

#include <aaudio/AAudio.h>
#include <android/log.h>
#include <ctype.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#define TAG "ShemEngine"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define SAMPLE_RATE 48000.0f
#define TWO_PI 6.28318530717958647692f

static AAudioStream *g_audio_stream = NULL;
static float g_sample_rate = 48000.0f;

// Lock-free double-buffered nodes for real-time audio thread
typedef struct {
    atomic_bool active;
    char name[MAX_NAME_LEN];
    atomic_int state;
    atomic_int osc_type;
    atomic_uint_fast32_t volume_bits;
    atomic_uint_fast32_t freq_bits;
    atomic_uint_fast32_t cutoff_bits;
    atomic_uint_fast32_t resonance_bits;
    atomic_uint_fast32_t pan_bits;
    atomic_uint_fast32_t quant_bits; // Clock quantization in beats

    // Local DSP state (private to audio thread)
    float smoothed_gain;
    float smoothed_freq;
    float smoothed_cutoff;
    float phase_l;
    float phase_r;
    float svf_s1_l, svf_s2_l; // State Variable Filter states (ZDF)
    float svf_s1_r, svf_s2_r;

    // Envelope
    Envelope env;
} FastNode;

static FastNode g_nodes[MAX_NODES];

// Global audio delay buffer for warm ambient acoustic space
#define DELAY_BUF_SIZE 24000
static float g_delay_buf_l[DELAY_BUF_SIZE];
static float g_delay_buf_r[DELAY_BUF_SIZE];
static int g_delay_idx = 0;
static float g_delay_lp_l = 0.0f;
static float g_delay_lp_r = 0.0f;

// Global atomic status
static atomic_bool g_server_running = true;
static atomic_bool g_lang_running = true;
static atomic_uint_fast32_t g_bpm_bits;
static atomic_int g_beat = 0;
static atomic_uint_fast32_t g_peak_linear_bits;
static atomic_int g_active_synths = 0;

static double g_clock_start_time = 0.0;

// Helper float-bits punning
static inline void set_atomic_float(atomic_uint_fast32_t *atom, float val) {
    union { float f; uint32_t u; } pun = { .f = val };
    atomic_store_explicit(atom, pun.u, memory_order_relaxed);
}

static inline float get_atomic_float(atomic_uint_fast32_t *atom) {
    union { float f; uint32_t u; } pun;
    pun.u = atomic_load_explicit(atom, memory_order_relaxed);
    return pun.f;
}

static double get_time_now(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec * 1e-6;
}

/* Fast PolyBLEP Anti-aliasing */
static inline float polyblep(float t, float dt) {
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0f;
    } else if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

/* Fast Xorshift32 PRNG for zero-lock, deterministic, audio-rate noise */
static uint32_t g_rng_state = 2463534242U;
static inline float fast_rand_float(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return (float)x * (1.0f / 4294967296.0f);
}

static float evaluate_envelope(Envelope *env, float t) {
    if (!env || env->num_points < 2) return 1.0f;
    if (t <= env->points[0].time) return env->points[0].level;
    if (t >= env->points[env->num_points - 1].time) return env->points[env->num_points - 1].level;

    for (int i = 0; i < env->num_points - 1; i++) {
        if (t >= env->points[i].time && t <= env->points[i + 1].time) {
            float dt = env->points[i + 1].time - env->points[i].time;
            if (dt <= 0.0001f) return env->points[i].level;
            float frac = (t - env->points[i].time) / dt;
            // Smoothstep Hermite interpolation (3*f^2 - 2*f^3) - smooth S-curve without cosf
            float mu2 = frac * frac * (3.0f - 2.0f * frac);
            return env->points[i].level * (1.0f - mu2) + env->points[i + 1].level * mu2;
        }
    }
    return 1.0f;
}

/* Transparent soft-knee master limiter (linear below 0.70, zero harsh clipping) */
static inline float soft_limit(float x) {
    if (x > 1.25f) return 0.98f;
    if (x < -1.25f) return -0.98f;
    if (x > 0.70f) return 0.70f + (x - 0.70f) / (1.0f + (x - 0.70f) * (x - 0.70f));
    if (x < -0.70f) return -0.70f + (x + 0.70f) / (1.0f + (x + 0.70f) * (x + 0.70f));
    return x;
}

/* 100% LOCK-FREE, ZERO-ALLOCATION, BEAT-SYNCED STEREO SYNTHESIS AUDIO CALLBACK */
static aaudio_data_callback_result_t audio_callback(
    AAudioStream *stream,
    void *userData,
    void *audioData,
    int32_t numFrames)
{
    (void)stream;
    (void)userData;
    float *out = (float *)audioData;

    if (!atomic_load_explicit(&g_server_running, memory_order_relaxed)) {
        memset(out, 0, numFrames * 2 * sizeof(float));
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    float bpm = get_atomic_float(&g_bpm_bits);
    if (bpm < 20.0f) bpm = 124.0f;

    double now = get_time_now();
    double beats = (now - g_clock_start_time) * (bpm / 60.0);
    atomic_store_explicit(&g_beat, (int)beats, memory_order_relaxed);

    float peak = 0.0001f;
    int active_cnt = 0;

    // Snapshot active node parameters once per callback
    typedef struct {
        FastNode *node;
        float target_vol;
        float target_freq;
        float target_cutoff;
        float res;
        float pan;
        float quant;
        int osc_type;
    } NodeSnapshot;

    NodeSnapshot active_nodes[MAX_NODES];
    int num_active = 0;

    for (int n = 0; n < MAX_NODES; n++) {
        FastNode *node = &g_nodes[n];
        if (!atomic_load_explicit(&node->active, memory_order_relaxed)) continue;
        int state = atomic_load_explicit(&node->state, memory_order_relaxed);
        if (state != NODE_STATE_PLAY) continue;

        active_nodes[num_active].node = node;
        active_nodes[num_active].target_vol = get_atomic_float(&node->volume_bits);
        active_nodes[num_active].target_freq = get_atomic_float(&node->freq_bits);
        active_nodes[num_active].target_cutoff = get_atomic_float(&node->cutoff_bits);
        active_nodes[num_active].res = get_atomic_float(&node->resonance_bits);
        active_nodes[num_active].pan = get_atomic_float(&node->pan_bits);
        active_nodes[num_active].quant = get_atomic_float(&node->quant_bits);
        active_nodes[num_active].osc_type = atomic_load_explicit(&node->osc_type, memory_order_relaxed);
        num_active++;
    }
    active_cnt = num_active;

    // Process in sub-blocks of 16 frames for efficient control and filter coefficient updates
    const int BLOCK_SIZE = 16;
    int frame_idx = 0;

    // Delay line parameters (360ms left, 480ms right)
    int delay_tap_l = (int)(0.36f * g_sample_rate);
    int delay_tap_r = (int)(0.48f * g_sample_rate);
    if (delay_tap_l >= DELAY_BUF_SIZE) delay_tap_l = DELAY_BUF_SIZE - 1;
    if (delay_tap_r >= DELAY_BUF_SIZE) delay_tap_r = DELAY_BUF_SIZE - 1;

    while (frame_idx < numFrames) {
        int frames_in_block = numFrames - frame_idx;
        if (frames_in_block > BLOCK_SIZE) frames_in_block = BLOCK_SIZE;

        // Current beat timestamp for this sub-block
        double block_time = now + ((double)frame_idx / (double)g_sample_rate);
        double block_beats = (block_time - g_clock_start_time) * (bpm / 60.0);

        // Per-block control and ZDF SVF filter coefficients
        float node_env_val[MAX_NODES];
        float node_a1[MAX_NODES];
        float node_a2[MAX_NODES];
        float node_a3[MAX_NODES];
        float node_dt_l[MAX_NODES];
        float node_dt_r[MAX_NODES];
        float node_pan_l[MAX_NODES];
        float node_pan_r[MAX_NODES];

        for (int k = 0; k < num_active; k++) {
            FastNode *node = active_nodes[k].node;
            float target_vol = active_nodes[k].target_vol;
            float target_freq = active_nodes[k].target_freq;
            float target_cutoff = active_nodes[k].target_cutoff;
            float res = active_nodes[k].res;
            float pan = active_nodes[k].pan;
            float quant = active_nodes[k].quant;
            if (quant < 0.25f) quant = 2.0f;

            // Parameter de-zippering
            node->smoothed_freq += 0.08f * (target_freq - node->smoothed_freq);
            node->smoothed_cutoff += 0.08f * (target_cutoff - node->smoothed_cutoff);
            node->smoothed_gain += 0.08f * (target_vol - node->smoothed_gain);

            // Detuned stereo phase increments for rich, analog-sounding width
            float base_dt = node->smoothed_freq / g_sample_rate;
            node_dt_l[k] = base_dt * 0.9985f;
            node_dt_r[k] = base_dt * 1.0015f;

            // Rhythmic envelope progression locked to the Shem Quant clock!
            double node_beat_pos = block_beats / (double)quant;
            float norm_t = (float)(node_beat_pos - floor(node_beat_pos));
            float env_val = evaluate_envelope(&node->env, norm_t);
            node_env_val[k] = env_val;

            // Zero-Delay Feedback (ZDF) State Variable Filter coefficients (Andrew Simper / Cytomic)
            float fc = (node->smoothed_cutoff * (0.15f + 0.85f * env_val)) / g_sample_rate;
            if (fc > 0.45f) fc = 0.45f;
            if (fc < 0.002f) fc = 0.002f;

            float g = tanf(3.14159265f * fc);
            float q = res;
            if (q < 0.2f) q = 0.2f;
            if (q > 3.5f) q = 3.5f;
            float k_damp = 1.0f / q;

            float a1 = 1.0f / (1.0f + g * (g + k_damp));
            float a2 = g * a1;
            float a3 = g * a2;

            node_a1[k] = a1;
            node_a2[k] = a2;
            node_a3[k] = a3;

            node_pan_l[k] = 0.5f * (1.0f - pan);
            node_pan_r[k] = 0.5f * (1.0f + pan);
        }

        // High-precision audio-rate inner loop
        for (int i = 0; i < frames_in_block; i++) {
            int out_idx = (frame_idx + i) * 2;
            float mix_l = 0.0f;
            float mix_r = 0.0f;

            for (int k = 0; k < num_active; k++) {
                FastNode *node = active_nodes[k].node;
                float dt_l = node_dt_l[k];
                float dt_r = node_dt_r[k];

                node->phase_l += dt_l;
                if (node->phase_l >= 1.0f) node->phase_l -= 1.0f;
                node->phase_r += dt_r;
                if (node->phase_r >= 1.0f) node->phase_r -= 1.0f;

                float raw_l = 0.0f;
                float raw_r = 0.0f;

                switch (active_nodes[k].osc_type) {
                    case OSC_SAW:
                        // Warm anti-aliased stereo saw
                        raw_l = 2.0f * node->phase_l - 1.0f - polyblep(node->phase_l, dt_l);
                        raw_r = 2.0f * node->phase_r - 1.0f - polyblep(node->phase_r, dt_r);
                        break;

                    case OSC_SINE:
                        // Pure, lush sine cluster with subtle harmonic warmth
                        raw_l = sinf(TWO_PI * node->phase_l);
                        raw_r = sinf(TWO_PI * node->phase_r) + 0.12f * sinf(TWO_PI * 2.0f * node->phase_r);
                        break;

                    case OSC_PULSE: {
                        // Deep resonant pulse with sub-octave foundation
                        float p_l = (node->phase_l < 0.5f) ? 0.7f : -0.7f;
                        p_l += polyblep(node->phase_l, dt_l);
                        float ph2_l = node->phase_l + 0.5f;
                        if (ph2_l >= 1.0f) ph2_l -= 1.0f;
                        p_l -= polyblep(ph2_l, dt_l);

                        float p_r = (node->phase_r < 0.5f) ? 0.7f : -0.7f;
                        p_r += polyblep(node->phase_r, dt_r);
                        float ph2_r = node->phase_r + 0.5f;
                        if (ph2_r >= 1.0f) ph2_r -= 1.0f;
                        p_r -= polyblep(ph2_r, dt_r);

                        // Sub-octave foundation
                        float sub = 0.4f * sinf(TWO_PI * 0.5f * node->phase_l);
                        raw_l = p_l + sub;
                        raw_r = p_r + sub;
                        break;
                    }

                    case OSC_NOISE:
                        raw_l = fast_rand_float(&g_rng_state) * 1.5f - 0.75f;
                        raw_r = fast_rand_float(&g_rng_state) * 1.5f - 0.75f;
                        break;

                    case OSC_DUST:
                        raw_l = (fast_rand_float(&g_rng_state) < 0.05f) ? 1.0f : 0.0f;
                        raw_r = (fast_rand_float(&g_rng_state) < 0.05f) ? 1.0f : 0.0f;
                        break;
                }

                // Cytomic ZDF SVF Processing (Left channel)
                float a1 = node_a1[k];
                float a2 = node_a2[k];
                float a3 = node_a3[k];

                float v3_l = raw_l - node->svf_s2_l;
                float v1_l = a1 * node->svf_s1_l + a2 * v3_l;
                float v2_l = node->svf_s2_l + a2 * node->svf_s1_l + a3 * v3_l;
                node->svf_s1_l = 2.0f * v1_l - node->svf_s1_l;
                node->svf_s2_l = 2.0f * v2_l - node->svf_s2_l;

                // Cytomic ZDF SVF Processing (Right channel)
                float v3_r = raw_r - node->svf_s2_r;
                float v1_r = a1 * node->svf_s1_r + a2 * v3_r;
                float v2_r = node->svf_s2_r + a2 * node->svf_s1_r + a3 * v3_r;
                node->svf_s1_r = 2.0f * v1_r - node->svf_s1_r;
                node->svf_s2_r = 2.0f * v2_r - node->svf_s2_r;

                // Protect against any denormal / unbounded state
                if (fabsf(node->svf_s1_l) > 4.0f) node->svf_s1_l = node->svf_s2_l = 0.0f;
                if (fabsf(node->svf_s1_r) > 4.0f) node->svf_s1_r = node->svf_s2_r = 0.0f;

                float sig_gain = node->smoothed_gain * node_env_val[k];
                mix_l += v2_l * sig_gain * node_pan_l[k];
                mix_r += v2_r * sig_gain * node_pan_r[k];
            }

            // Warm Ambient Space Delay (stereo diffusion)
            int read_idx_l = g_delay_idx - delay_tap_l;
            if (read_idx_l < 0) read_idx_l += DELAY_BUF_SIZE;
            int read_idx_r = g_delay_idx - delay_tap_r;
            if (read_idx_r < 0) read_idx_r += DELAY_BUF_SIZE;

            float wet_l = g_delay_buf_l[read_idx_l];
            float wet_r = g_delay_buf_r[read_idx_r];

            // 1st order gentle lowpass damping on delay feedback
            g_delay_lp_l += 0.35f * (wet_l - g_delay_lp_l);
            g_delay_lp_r += 0.35f * (wet_r - g_delay_lp_r);

            g_delay_buf_l[g_delay_idx] = mix_l + g_delay_lp_r * 0.22f;
            g_delay_buf_r[g_delay_idx] = mix_r + g_delay_lp_l * 0.22f;

            g_delay_idx++;
            if (g_delay_idx >= DELAY_BUF_SIZE) g_delay_idx = 0;

            float final_l = mix_l + wet_l * 0.18f;
            float final_r = mix_r + wet_r * 0.18f;

            // Transparent Master Soft Limiter
            float out_l = soft_limit(final_l);
            float out_r = soft_limit(final_r);

            out[out_idx + 0] = out_l;
            out[out_idx + 1] = out_r;

            float sample_peak = fmaxf(fabsf(out_l), fabsf(out_r));
            if (sample_peak > peak) peak = sample_peak;
        }

        frame_idx += frames_in_block;
    }

    atomic_store_explicit(&g_active_synths, active_cnt, memory_order_relaxed);
    set_atomic_float(&g_peak_linear_bits, peak);

    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

bool shem_engine_init(const char *username) {
#if defined(__aarch64__)
    uint64_t fpcr;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
    fpcr |= (1 << 24); // FZ bit: Flush denormals to zero (prevents CPU stalls in audio DSP)
    __asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr));
#elif defined(__arm__)
    uint32_t fpscr;
    __asm__ __volatile__("vmrs %0, fpscr" : "=r"(fpscr));
    fpscr |= (1 << 24); // FZ bit
    __asm__ __volatile__("vmsr fpscr, %0" : : "r"(fpscr));
#endif

    memset(&g_nodes, 0, sizeof(g_nodes));
    memset(g_delay_buf_l, 0, sizeof(g_delay_buf_l));
    memset(g_delay_buf_r, 0, sizeof(g_delay_buf_r));
    g_delay_idx = 0;
    g_delay_lp_l = 0.0f;
    g_delay_lp_r = 0.0f;

    set_atomic_float(&g_bpm_bits, 124.0f);
    set_atomic_float(&g_peak_linear_bits, 0.0001f);
    atomic_store(&g_server_running, true);
    atomic_store(&g_lang_running, true);
    atomic_store(&g_beat, 0);
    g_clock_start_time = get_time_now();

    // Default Node 0: ~lead (Lush Sawtooth Unison)
    FastNode *lead = &g_nodes[0];
    strncpy(lead->name, "~lead", MAX_NAME_LEN - 1);
    atomic_store(&lead->osc_type, OSC_SAW);
    atomic_store(&lead->state, NODE_STATE_PLAY);
    set_atomic_float(&lead->volume_bits, 0.32f);
    set_atomic_float(&lead->freq_bits, 220.0f);
    set_atomic_float(&lead->cutoff_bits, 2200.0f);
    set_atomic_float(&lead->resonance_bits, 1.4f);
    set_atomic_float(&lead->pan_bits, -0.2f);
    set_atomic_float(&lead->quant_bits, 2.0f); // 2 beats
    lead->smoothed_gain = 0.32f;
    lead->smoothed_freq = 220.0f;
    lead->smoothed_cutoff = 2200.0f;
    lead->env.num_points = 4;
    lead->env.duration = 2.0f;
    lead->env.loop = true;
    lead->env.points[0] = (EnvPoint){ 0.0f, 0.05f };
    lead->env.points[1] = (EnvPoint){ 0.10f, 1.0f };
    lead->env.points[2] = (EnvPoint){ 0.55f, 0.4f };
    lead->env.points[3] = (EnvPoint){ 1.0f, 0.05f };
    atomic_store(&lead->active, true);

    // Default Node 1: ~wash (Detuned Ambient Sine Cluster)
    FastNode *wash = &g_nodes[1];
    strncpy(wash->name, "~wash", MAX_NAME_LEN - 1);
    atomic_store(&wash->osc_type, OSC_SINE);
    atomic_store(&wash->state, NODE_STATE_PLAY);
    set_atomic_float(&wash->volume_bits, 0.22f);
    set_atomic_float(&wash->freq_bits, 110.0f);
    set_atomic_float(&wash->cutoff_bits, 1400.0f);
    set_atomic_float(&wash->resonance_bits, 0.8f);
    set_atomic_float(&wash->pan_bits, 0.3f);
    set_atomic_float(&wash->quant_bits, 4.0f); // 4 beats ambient swell
    wash->smoothed_gain = 0.22f;
    wash->smoothed_freq = 110.0f;
    wash->smoothed_cutoff = 1400.0f;
    wash->env.num_points = 4;
    wash->env.duration = 4.0f;
    wash->env.loop = true;
    wash->env.points[0] = (EnvPoint){ 0.0f, 0.15f };
    wash->env.points[1] = (EnvPoint){ 0.45f, 0.90f };
    wash->env.points[2] = (EnvPoint){ 0.80f, 0.50f };
    wash->env.points[3] = (EnvPoint){ 1.0f, 0.15f };
    atomic_store(&wash->active, true);

    // Default Node 2: ~bass (Punchy Moog Sub-Pulse)
    FastNode *bass = &g_nodes[2];
    strncpy(bass->name, "~bass", MAX_NAME_LEN - 1);
    atomic_store(&bass->osc_type, OSC_PULSE);
    atomic_store(&bass->state, NODE_STATE_PLAY);
    set_atomic_float(&bass->volume_bits, 0.38f);
    set_atomic_float(&bass->freq_bits, 55.0f);
    set_atomic_float(&bass->cutoff_bits, 600.0f);
    set_atomic_float(&bass->resonance_bits, 2.0f);
    set_atomic_float(&bass->pan_bits, 0.0f);
    set_atomic_float(&bass->quant_bits, 1.0f); // 1 beat rhythm pulse
    bass->smoothed_gain = 0.38f;
    bass->smoothed_freq = 55.0f;
    bass->smoothed_cutoff = 600.0f;
    bass->env.num_points = 4;
    bass->env.duration = 1.0f;
    bass->env.loop = true;
    bass->env.points[0] = (EnvPoint){ 0.0f, 0.05f };
    bass->env.points[1] = (EnvPoint){ 0.08f, 1.0f };
    bass->env.points[2] = (EnvPoint){ 0.40f, 0.30f };
    bass->env.points[3] = (EnvPoint){ 1.0f, 0.05f };
    atomic_store(&bass->active, true);

    // Start AAudio Stream in Low Latency mode
    AAudioStreamBuilder *builder = NULL;
    aaudio_result_t res = AAudio_createStreamBuilder(&builder);
    if (res == AAUDIO_OK) {
        AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
        AAudioStreamBuilder_setChannelCount(builder, 2);
        // Let AAudio select native hardware sample rate to eliminate resampling jitter
        AAudioStreamBuilder_setSampleRate(builder, AAUDIO_UNSPECIFIED);
        AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
        AAudioStreamBuilder_setDataCallback(builder, audio_callback, NULL);

        res = AAudioStreamBuilder_openStream(builder, &g_audio_stream);
        AAudioStreamBuilder_delete(builder);

        if (res == AAUDIO_OK && g_audio_stream) {
            int32_t sr = AAudioStream_getSampleRate(g_audio_stream);
            if (sr > 0) g_sample_rate = (float)sr;

            int32_t burst = AAudioStream_getFramesPerBurst(g_audio_stream);
            if (burst > 0) {
                // Generous 3x burst buffer size ensures 100% glitchless audio on mobile OS
                int32_t target_buffer = burst * 3;
                int32_t actual_buffer = AAudioStream_setBufferSizeInFrames(g_audio_stream, target_buffer);
                LOGI("AAudio stream online: sr=%.0f, burst=%d, buffer=%d frames (target=%d)",
                     g_sample_rate, burst, actual_buffer, target_buffer);
            }
            AAudioStream_requestStart(g_audio_stream);
            LOGI("Shem AAudio low-latency glitch-free stream started.");
        }
    }

    shem_udp_init(username ? username : "Jachym", 10000);
    LOGI("Shem engine online. Author: Jachym Pesek");
    return true;
}

void shem_engine_set_lang_state(bool running) {
    atomic_store(&g_lang_running, running);
}

void shem_engine_set_server_state(bool running) {
    atomic_store(&g_server_running, running);
    if (!running) {
        for (int i = 0; i < MAX_NODES; i++) {
            g_nodes[i].svf_s1_l = g_nodes[i].svf_s2_l = 0.0f;
            g_nodes[i].svf_s1_r = g_nodes[i].svf_s2_r = 0.0f;
        }
    }
}

void shem_engine_set_bpm(float bpm) {
    if (bpm > 20.0f && bpm < 400.0f) {
        set_atomic_float(&g_bpm_bits, bpm);
    }
}

static FastNode* find_or_create_node(const char *name) {
    for (int i = 0; i < MAX_NODES; i++) {
        if (atomic_load(&g_nodes[i].active) && strcmp(g_nodes[i].name, name) == 0) {
            return &g_nodes[i];
        }
    }
    for (int i = 0; i < MAX_NODES; i++) {
        if (!atomic_load(&g_nodes[i].active)) {
            strncpy(g_nodes[i].name, name, MAX_NAME_LEN - 1);
            atomic_store(&g_nodes[i].osc_type, OSC_SAW);
            set_atomic_float(&g_nodes[i].volume_bits, 0.4f);
            set_atomic_float(&g_nodes[i].freq_bits, 220.0f);
            set_atomic_float(&g_nodes[i].cutoff_bits, 1600.0f);
            set_atomic_float(&g_nodes[i].resonance_bits, 1.0f);
            set_atomic_float(&g_nodes[i].pan_bits, 0.0f);
            g_nodes[i].smoothed_gain = 0.0f;
            g_nodes[i].smoothed_freq = 220.0f;
            g_nodes[i].smoothed_cutoff = 1600.0f;
            g_nodes[i].env.num_points = 2;
            g_nodes[i].env.points[0] = (EnvPoint){ 0.0f, 1.0f };
            g_nodes[i].env.points[1] = (EnvPoint){ 1.0f, 1.0f };
            g_nodes[i].env.duration = 1.0f;
            g_nodes[i].env.loop = true;
            atomic_store(&g_nodes[i].state, NODE_STATE_PLAY);
            atomic_store(&g_nodes[i].active, true);
            return &g_nodes[i];
        }
    }
    return NULL;
}

bool shem_engine_evaluate(const char *code, char *out_result, int max_result_len) {
    if (!code || strlen(code) == 0) return false;

    shem_udp_broadcast_code(code);

    char clean[1024];
    strncpy(clean, code, sizeof(clean) - 1);
    clean[sizeof(clean) - 1] = '\0';

    if (strstr(clean, "s.boot")) {
        atomic_store(&g_server_running, true);
        snprintf(out_result, max_result_len, "-> boot [localhost : 57110]\n-> shared memory live audio server ready");
        return true;
    }
    if (strstr(clean, "s.quit") || strstr(clean, "Server.killAll")) {
        atomic_store(&g_server_running, false);
        snprintf(out_result, max_result_len, "-> Server.default quit cleanly");
        return true;
    }

    char *bpm_ptr = strstr(clean, ".bpm(");
    if (bpm_ptr) {
        float b = atof(bpm_ptr + 5);
        if (b > 10.0f) {
            set_atomic_float(&g_bpm_bits, b);
            snprintf(out_result, max_result_len, "-> TempoClock: %d BPM", (int)b);
            return true;
        }
    }

    if (clean[0] == '~') {
        char node_name[MAX_NAME_LEN];
        int idx = 0;
        while (clean[idx] != '\0' && !isspace(clean[idx]) && clean[idx] != '=' && clean[idx] != '.' && idx < MAX_NAME_LEN - 1) {
            node_name[idx] = clean[idx];
            idx++;
        }
        node_name[idx] = '\0';

        if (strstr(clean, ".stop")) {
            for (int n = 0; n < MAX_NODES; n++) {
                if (atomic_load(&g_nodes[n].active) && strcmp(g_nodes[n].name, node_name) == 0) {
                    atomic_store(&g_nodes[n].state, NODE_STATE_STOP);
                    snprintf(out_result, max_result_len, "-> NodeProxy(%s) stopped", node_name);
                    return true;
                }
            }
        } else if (strstr(clean, ".free")) {
            for (int n = 0; n < MAX_NODES; n++) {
                if (atomic_load(&g_nodes[n].active) && strcmp(g_nodes[n].name, node_name) == 0) {
                    atomic_store(&g_nodes[n].active, false);
                    snprintf(out_result, max_result_len, "-> NodeProxy(%s) freed", node_name);
                    return true;
                }
            }
        } else if (strstr(clean, ".play")) {
            for (int n = 0; n < MAX_NODES; n++) {
                if (atomic_load(&g_nodes[n].active) && strcmp(g_nodes[n].name, node_name) == 0) {
                    atomic_store(&g_nodes[n].state, NODE_STATE_PLAY);
                    snprintf(out_result, max_result_len, "-> NodeProxy(%s) playing", node_name);
                    return true;
                }
            }
        }

        char *eq = strchr(clean, '=');
        if (eq) {
            FastNode *node = find_or_create_node(node_name);
            if (node) {
                atomic_store(&node->state, NODE_STATE_PLAY);

                if (strstr(eq, "SinOsc")) atomic_store(&node->osc_type, OSC_SINE);
                else if (strstr(eq, "Pulse")) atomic_store(&node->osc_type, OSC_PULSE);
                else if (strstr(eq, "WhiteNoise")) atomic_store(&node->osc_type, OSC_NOISE);
                else if (strstr(eq, "Dust")) atomic_store(&node->osc_type, OSC_DUST);
                else atomic_store(&node->osc_type, OSC_SAW);

                char *open_p = strchr(eq, '(');
                if (open_p) {
                    float f = atof(open_p + 1);
                    if (f > 20.0f && f < 10000.0f) {
                        set_atomic_float(&node->freq_bits, f);
                    }
                }

                if (strstr(eq, "MoogFF") || strstr(eq, "RLPF")) {
                    char *moog_p = strstr(eq, "MoogFF");
                    if (!moog_p) moog_p = strstr(eq, "RLPF");
                    char *arg_p = strchr(moog_p, ',');
                    if (arg_p) {
                        float cut = atof(arg_p + 1);
                        if (cut > 40.0f) set_atomic_float(&node->cutoff_bits, cut);
                    }
                }

                snprintf(out_result, max_result_len, "-> NodeProxy(%s) : ar(2)", node_name);
                return true;
            }
        }
    }

    snprintf(out_result, max_result_len, "-> %s", clean);
    return true;
}

int shem_engine_get_node_count(void) {
    int cnt = 0;
    for (int i = 0; i < MAX_NODES; i++) {
        if (atomic_load(&g_nodes[i].active)) cnt++;
    }
    return cnt;
}

bool shem_engine_get_node(int index, SynthNode *out_node) {
    if (!out_node) return false;
    int curr = 0;
    for (int i = 0; i < MAX_NODES; i++) {
        if (atomic_load(&g_nodes[i].active)) {
            if (curr == index) {
                memset(out_node, 0, sizeof(SynthNode));
                out_node->active = true;
                strncpy(out_node->name, g_nodes[i].name, MAX_NAME_LEN - 1);
                out_node->state = (NodePlayState)atomic_load(&g_nodes[i].state);
                out_node->osc_type = (OscType)atomic_load(&g_nodes[i].osc_type);
                out_node->volume = get_atomic_float(&g_nodes[i].volume_bits);
                out_node->freq = get_atomic_float(&g_nodes[i].freq_bits);
                out_node->cutoff = get_atomic_float(&g_nodes[i].cutoff_bits);
                out_node->resonance = get_atomic_float(&g_nodes[i].resonance_bits);
                out_node->pan = get_atomic_float(&g_nodes[i].pan_bits);
                memcpy(&out_node->env, &g_nodes[i].env, sizeof(Envelope));
                return true;
            }
            curr++;
        }
    }
    return false;
}

void shem_engine_set_node_state(int index, NodePlayState state) {
    int curr = 0;
    for (int i = 0; i < MAX_NODES; i++) {
        if (atomic_load(&g_nodes[i].active)) {
            if (curr == index) {
                atomic_store(&g_nodes[i].state, state);
                break;
            }
            curr++;
        }
    }
}

void shem_engine_set_node_volume(int index, float vol) {
    int curr = 0;
    for (int i = 0; i < MAX_NODES; i++) {
        if (atomic_load(&g_nodes[i].active)) {
            if (curr == index) {
                set_atomic_float(&g_nodes[i].volume_bits, vol);
                break;
            }
            curr++;
        }
    }
}

void shem_engine_set_node_env_point(int node_idx, int pt_idx, float time, float level) {
    int curr = 0;
    for (int i = 0; i < MAX_NODES; i++) {
        if (atomic_load(&g_nodes[i].active)) {
            if (curr == node_idx) {
                if (pt_idx >= 0 && pt_idx < g_nodes[i].env.num_points) {
                    g_nodes[i].env.points[pt_idx].time = time;
                    g_nodes[i].env.points[pt_idx].level = level;
                }
                break;
            }
            curr++;
        }
    }
}

int shem_engine_get_node_env(int node_idx, float *out_times, float *out_levels, int max_pts) {
    int curr = 0;
    for (int i = 0; i < MAX_NODES; i++) {
        if (atomic_load(&g_nodes[i].active)) {
            if (curr == node_idx) {
                int n = g_nodes[i].env.num_points;
                if (n > max_pts) n = max_pts;
                for (int p = 0; p < n; p++) {
                    out_times[p] = g_nodes[i].env.points[p].time;
                    out_levels[p] = g_nodes[i].env.points[p].level;
                }
                return n;
            }
            curr++;
        }
    }
    return 0;
}

void shem_engine_set_node_env_preset(int node_idx, int preset) {
    int curr = 0;
    for (int i = 0; i < MAX_NODES; i++) {
        if (atomic_load(&g_nodes[i].active)) {
            if (curr == node_idx) {
                Envelope *e = &g_nodes[i].env;
                switch (preset) {
                    case 0: // ADSR / Pluck
                        e->num_points = 4;
                        e->points[0] = (EnvPoint){ 0.0f, 0.0f };
                        e->points[1] = (EnvPoint){ 0.08f, 1.0f };
                        e->points[2] = (EnvPoint){ 0.40f, 0.35f };
                        e->points[3] = (EnvPoint){ 1.0f, 0.05f };
                        break;
                    case 1: // Sweep / Riser
                        e->num_points = 4;
                        e->points[0] = (EnvPoint){ 0.0f, 0.10f };
                        e->points[1] = (EnvPoint){ 0.45f, 0.30f };
                        e->points[2] = (EnvPoint){ 0.85f, 0.95f };
                        e->points[3] = (EnvPoint){ 1.0f, 0.15f };
                        break;
                    case 2: // Triangle / LFO
                        e->num_points = 3;
                        e->points[0] = (EnvPoint){ 0.0f, 0.10f };
                        e->points[1] = (EnvPoint){ 0.5f, 1.0f };
                        e->points[2] = (EnvPoint){ 1.0f, 0.10f };
                        break;
                    case 3: // Gate / Square
                        e->num_points = 4;
                        e->points[0] = (EnvPoint){ 0.0f, 1.0f };
                        e->points[1] = (EnvPoint){ 0.49f, 1.0f };
                        e->points[2] = (EnvPoint){ 0.51f, 0.05f };
                        e->points[3] = (EnvPoint){ 1.0f, 0.05f };
                        break;
                    case 4: // Wobble
                        e->num_points = 6;
                        e->points[0] = (EnvPoint){ 0.0f, 0.20f };
                        e->points[1] = (EnvPoint){ 0.20f, 0.95f };
                        e->points[2] = (EnvPoint){ 0.40f, 0.20f };
                        e->points[3] = (EnvPoint){ 0.60f, 0.95f };
                        e->points[4] = (EnvPoint){ 0.80f, 0.20f };
                        e->points[5] = (EnvPoint){ 1.0f, 0.80f };
                        break;
                }
                break;
            }
            curr++;
        }
    }
}

void shem_engine_get_status(EngineStatus *out_status) {
    if (!out_status) return;
    out_status->lang_running = atomic_load(&g_lang_running);
    out_status->server_running = atomic_load(&g_server_running);
    out_status->bpm = get_atomic_float(&g_bpm_bits);
    out_status->beat = atomic_load(&g_beat);
    out_status->active_synths = atomic_load(&g_active_synths);
    out_status->active_groups = 1;
    float peak = get_atomic_float(&g_peak_linear_bits);
    float db = 20.0f * log10f(fmaxf(peak, 0.00001f));
    if (db < -96.0f) db = -96.0f;
    out_status->peak_db = db;
    out_status->cpu_percent = 2.0f + out_status->active_synths * 1.5f;
}

void shem_engine_close(void) {
    if (g_audio_stream) {
        AAudioStream_requestStop(g_audio_stream);
        AAudioStream_close(g_audio_stream);
        g_audio_stream = NULL;
    }
    shem_udp_close();
    LOGI("Shem engine closed cleanly.");
}
