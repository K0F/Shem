/* =============================================================================
 * shem_engine.h - Embedded SuperCollider Synthesis Engine & ScBridge for Shem
 * Author: Jáchym Pešek
 * =============================================================================
 */

#ifndef SHEM_ENGINE_H
#define SHEM_ENGINE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_NODES 16
#define MAX_ENV_POINTS 16
#define MAX_NAME_LEN 32

typedef enum {
    NODE_STATE_OFF,
    NODE_STATE_PLAY,
    NODE_STATE_STOP
} NodePlayState;

typedef enum {
    OSC_SAW,
    OSC_SINE,
    OSC_PULSE,
    OSC_NOISE,
    OSC_DUST
} OscType;

typedef struct {
    float time;   // 0.0 to 1.0 normalized
    float level;  // 0.0 to 1.0
} EnvPoint;

typedef struct {
    EnvPoint points[MAX_ENV_POINTS];
    int num_points;
    float duration; // seconds
    float current_time;
    bool loop;
} Envelope;

typedef struct {
    bool active;
    char name[MAX_NAME_LEN];
    NodePlayState state;
    OscType osc_type;
    float volume;
    float fade_time;
    float current_gain;

    // Synthesis parameters
    float freq;
    float target_freq;
    float cutoff;
    float target_cutoff;
    float resonance;
    float pan;
    float pulse_width;

    // Phase accumulator
    float phase;
    // 4-pole Moog ladder filter state
    float filter_s[4];

    // Envelope
    Envelope env;
} SynthNode;

typedef struct {
    bool lang_running;
    bool server_running;
    float bpm;
    int beat;
    float cpu_percent;
    int active_synths;
    int active_groups;
    float peak_db;
} EngineStatus;

bool shem_engine_init(const char *username);
void shem_engine_set_lang_state(bool running);
void shem_engine_set_server_state(bool running);
void shem_engine_set_bpm(float bpm);

// Live Code Evaluation
bool shem_engine_evaluate(const char *code, char *out_result, int max_result_len);

// Node and Envelope Control
int shem_engine_get_node_count(void);
bool shem_engine_get_node(int index, SynthNode *out_node);
void shem_engine_set_node_state(int index, NodePlayState state);
void shem_engine_set_node_volume(int index, float vol);
int shem_engine_get_node_env(int node_idx, float *out_times, float *out_levels, int max_pts);
void shem_engine_set_node_env_point(int node_idx, int pt_idx, float time, float level);
void shem_engine_set_node_env_preset(int node_idx, int preset_type);

// Status Telemetry
void shem_engine_get_status(EngineStatus *out_status);

void shem_engine_close(void);

#ifdef __cplusplus
}
#endif

#endif // SHEM_ENGINE_H
