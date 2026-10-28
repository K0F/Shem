/* =============================================================================
 * shem_jni.c - JNI Bridge for Shem on Android
 * Author: Jáchym Pešek
 * =============================================================================
 */

#include <jni.h>
#include "shem_engine.h"
#include "shem_udp.h"
#include <android/log.h>
#include <stdlib.h>
#include <string.h>

#define TAG "ShemJNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

JNIEXPORT jboolean JNICALL
Java_org_kof_shem_MainActivity_nativeInit(JNIEnv *env, jobject thiz, jstring username) {
    (void)thiz;
    const char *user = username ? (*env)->GetStringUTFChars(env, username, NULL) : "Jachym";
    bool ok = shem_engine_init(user);
    if (username) (*env)->ReleaseStringUTFChars(env, username, user);
    return ok;
}

JNIEXPORT jstring JNICALL
Java_org_kof_shem_MainActivity_nativeEvaluate(JNIEnv *env, jobject thiz, jstring code) {
    (void)thiz;
    if (!code) return (*env)->NewStringUTF(env, "-> nil");
    const char *code_str = (*env)->GetStringUTFChars(env, code, NULL);

    char res_buf[1024];
    shem_engine_evaluate(code_str, res_buf, sizeof(res_buf));

    (*env)->ReleaseStringUTFChars(env, code, code_str);
    return (*env)->NewStringUTF(env, res_buf);
}

JNIEXPORT void JNICALL
Java_org_kof_shem_MainActivity_nativeSetLang(JNIEnv *env, jobject thiz, jboolean running) {
    (void)env; (void)thiz;
    shem_engine_set_lang_state(running);
}

JNIEXPORT void JNICALL
Java_org_kof_shem_MainActivity_nativeSetServer(JNIEnv *env, jobject thiz, jboolean running) {
    (void)env; (void)thiz;
    shem_engine_set_server_state(running);
}

JNIEXPORT void JNICALL
Java_org_kof_shem_MainActivity_nativeSetBpm(JNIEnv *env, jobject thiz, jfloat bpm) {
    (void)env; (void)thiz;
    shem_engine_set_bpm(bpm);
}

JNIEXPORT void JNICALL
Java_org_kof_shem_MainActivity_nativeGetStatus(JNIEnv *env, jobject thiz, jfloatArray outStatus) {
    (void)thiz;
    if (!outStatus) return;
    EngineStatus st;
    shem_engine_get_status(&st);

    jfloat buf[7];
    buf[0] = st.lang_running ? 1.0f : 0.0f;
    buf[1] = st.server_running ? 1.0f : 0.0f;
    buf[2] = st.bpm;
    buf[3] = (jfloat)st.beat;
    buf[4] = st.cpu_percent;
    buf[5] = (jfloat)st.active_synths;
    buf[6] = st.peak_db;

    (*env)->SetFloatArrayRegion(env, outStatus, 0, 7, buf);
}

JNIEXPORT jint JNICALL
Java_org_kof_shem_MainActivity_nativeGetNodeCount(JNIEnv *env, jobject thiz) {
    (void)env; (void)thiz;
    return shem_engine_get_node_count();
}

JNIEXPORT jstring JNICALL
Java_org_kof_shem_MainActivity_nativeGetNodeName(JNIEnv *env, jobject thiz, jint index) {
    (void)thiz;
    SynthNode node;
    if (shem_engine_get_node(index, &node)) {
        return (*env)->NewStringUTF(env, node.name);
    }
    return (*env)->NewStringUTF(env, "");
}

JNIEXPORT void JNICALL
Java_org_kof_shem_MainActivity_nativeGetNodeInfo(JNIEnv *env, jobject thiz, jint index, jfloatArray outInfo) {
    (void)thiz;
    if (!outInfo) return;
    SynthNode node;
    if (shem_engine_get_node(index, &node)) {
        jfloat buf[6];
        buf[0] = (jfloat)node.state;
        buf[1] = node.volume;
        buf[2] = node.freq;
        buf[3] = node.cutoff;
        buf[4] = node.resonance;
        buf[5] = node.pan;
        (*env)->SetFloatArrayRegion(env, outInfo, 0, 6, buf);
    }
}

JNIEXPORT void JNICALL
Java_org_kof_shem_MainActivity_nativeSetNodeState(JNIEnv *env, jobject thiz, jint index, jint state) {
    (void)env; (void)thiz;
    shem_engine_set_node_state(index, (NodePlayState)state);
}

JNIEXPORT void JNICALL
Java_org_kof_shem_MainActivity_nativeSetNodeVolume(JNIEnv *env, jobject thiz, jint index, jfloat vol) {
    (void)env; (void)thiz;
    shem_engine_set_node_volume(index, vol);
}

JNIEXPORT void JNICALL
Java_org_kof_shem_MainActivity_nativeSetEnvPoint(JNIEnv *env, jobject thiz, jint nodeIdx, jint ptIdx, jfloat time, jfloat level) {
    (void)env; (void)thiz;
    shem_engine_set_node_env_point(nodeIdx, ptIdx, time, level);
}

JNIEXPORT jint JNICALL
Java_org_kof_shem_MainActivity_nativeGetEnvPoints(JNIEnv *env, jobject thiz, jint nodeIdx, jfloatArray outTimes, jfloatArray outLevels) {
    (void)thiz;
    if (!outTimes || !outLevels) return 0;
    float times[MAX_ENV_POINTS];
    float levels[MAX_ENV_POINTS];
    int count = shem_engine_get_node_env(nodeIdx, times, levels, MAX_ENV_POINTS);
    if (count > 0) {
        (*env)->SetFloatArrayRegion(env, outTimes, 0, count, times);
        (*env)->SetFloatArrayRegion(env, outLevels, 0, count, levels);
    }
    return count;
}

JNIEXPORT void JNICALL
Java_org_kof_shem_MainActivity_nativeSetEnvPreset(JNIEnv *env, jobject thiz, jint nodeIdx, jint presetType) {
    (void)env; (void)thiz;
    shem_engine_set_node_env_preset(nodeIdx, presetType);
}

JNIEXPORT jstring JNICALL
Java_org_kof_shem_MainActivity_nativePopUdpMessage(JNIEnv *env, jobject thiz) {
    (void)thiz;
    char msg[1024];
    if (shem_udp_pop_incoming(msg, sizeof(msg))) {
        return (*env)->NewStringUTF(env, msg);
    }
    return NULL;
}

JNIEXPORT void JNICALL
Java_org_kof_shem_MainActivity_nativeClose(JNIEnv *env, jobject thiz) {
    (void)env; (void)thiz;
    shem_engine_close();
}
