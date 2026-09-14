#include "bridge/bridge.h"

// JNI exports must retain their complete ABI signatures. A number of callbacks
// intentionally do not need every supplied argument, so keep that boundary
// warning-free without weakening warnings in the rest of the native codebase.
#ifdef _MSC_VER
#pragma warning(disable : 4100)
#endif
#include "core/module.h"
#include "core/capabilities.h"
#include "core/camera.h"
#include "core/esp.h"
#include "core/game_mode_telemetry.h"
#include "core/player_stats.h"
#include "core/replay.h"
#include "core/pipeline.h"
#include "core/tracker.h"
#include "render/imgui_layer.h"
#include "render/hud.h"

#include <vector>
#include <atomic>
#include <cmath>

namespace aml::bridge {

static JavaVM* g_vm = nullptr;
static std::atomic<bool> g_loaded{false};

bool valid_module(jint module_id) {
    return module_id >= MOD_FREECAM && module_id < MOD_COUNT;
}

bool finite_action_values(jfloat x, jfloat y, jfloat z, jfloat f) {
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) && std::isfinite(f);
}

JavaVM* vm() { return g_vm; }

jint on_load(JavaVM* java_vm, void* reserved) {
    if (g_loaded.exchange(true, std::memory_order_acq_rel)) return JNI_VERSION_1_8;
    g_vm = java_vm;
    telemetry::store().begin_session();
    render::install();
    return JNI_VERSION_1_8;
}

void on_unload(JavaVM* java_vm) {
    if (!g_loaded.exchange(false, std::memory_order_acq_rel)) return;
    telemetry::store().end_session();
    replay::recorder().stop();
    render::shutdown();
    g_vm = nullptr;
}

namespace {

uint8_t* byte_array_data(JNIEnv* env, jbyteArray arr, jlong* out_len) {
    if (!arr) return nullptr;
    jsize len = env->GetArrayLength(arr);
    if (len <= 0) return nullptr;
    jbyte* raw = env->GetByteArrayElements(arr, nullptr);
    if (!raw) return nullptr;
    *out_len = static_cast<jlong>(len);
    return reinterpret_cast<uint8_t*>(raw);
}

void release_byte_array(JNIEnv* env, jbyteArray arr, void* data, jint mode) {
    if (!arr || !data) return;
    env->ReleaseByteArrayElements(arr, reinterpret_cast<jbyte*>(data), mode);
}

}  // namespace
}  // namespace aml::bridge

extern "C" {

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* java_vm, void* reserved) {
    return aml::bridge::on_load(java_vm, reserved);
}

JNIEXPORT jint JNICALL Agent_OnLoad(JavaVM* java_vm, char* options, void* reserved) {
    aml::bridge::on_load(java_vm, reserved);
    return 0;
}

JNIEXPORT void JNICALL Agent_OnUnload(JavaVM* java_vm) {
    aml::bridge::on_unload(java_vm);
}

JNIEXPORT void JNICALL JNI_OnUnload(JavaVM* java_vm, void* reserved) {
    aml::bridge::on_unload(java_vm);
}

JNIEXPORT jint JNICALL Java_amalgam_bridge_NativeBridge_nativeAttach(JNIEnv* env, jclass cls) {
    aml::telemetry::store().begin_session();
    return 0;
}

JNIEXPORT void JNICALL Java_amalgam_bridge_NativeBridge_nativeSetCapabilities(JNIEnv* env, jclass cls,
                                                                                jint capabilities) {
    aml::capabilities::set(static_cast<uint32_t>(capabilities));
}

JNIEXPORT void JNICALL Java_amalgam_bridge_NativeBridge_nativeSetCameraState(JNIEnv* env, jclass cls,
                                                                              jfloatArray matrix,
                                                                              jint width, jint height) {
    if (!matrix || env->GetArrayLength(matrix) < 16) return;
    jfloat values[16];
    env->GetFloatArrayRegion(matrix, 0, 16, values);
    aml::camera::publish(values, width, height);
}

JNIEXPORT void JNICALL Java_amalgam_bridge_NativeBridge_nativeSetCameraPose(JNIEnv* env, jclass cls,
                                                                             jdouble x, jdouble y,
                                                                             jdouble z, jfloat yaw,
                                                                             jfloat pitch, jint width,
                                                                             jint height) {
    aml::camera::publish_pose(x, y, z, yaw, pitch, width, height);
}

JNIEXPORT void JNICALL Java_amalgam_bridge_NativeBridge_nativeClearProjectedEntities(JNIEnv* env,
                                                                                       jclass cls) {
    aml::esp::clear();
}

JNIEXPORT void JNICALL Java_amalgam_bridge_NativeBridge_nativeSetProjectedEntity(JNIEnv* env,
                                                                                  jclass cls,
                                                                                  jint id, jfloat x,
                                                                                  jfloat y, jfloat depth,
                                                                                  jint kind) {
    aml::esp::publish({id, x, y, depth, kind});
}

JNIEXPORT void JNICALL Java_amalgam_bridge_NativeBridge_nativeSetProjectedWorldEntity(JNIEnv* env,
                                                                                      jclass cls,
                                                                                      jint id, jdouble x,
                                                                                      jdouble y, jdouble z,
                                                                                      jint kind) {
    const aml::camera::Pose camera = aml::camera::pose();
    float screen_x = 0.0f;
    float screen_y = 0.0f;
    float depth = 0.0f;
    if (!aml::camera::project_world(x, y, z, camera, screen_x, screen_y, depth)) return;
    aml::esp::publish({id, screen_x, screen_y, depth, kind});
}

JNIEXPORT void JNICALL Java_amalgam_bridge_NativeBridge_nativePublishTelemetry(JNIEnv* env,
                                                                                jclass cls,
                                                                                jbyteArray payload) {
    if (!payload) {
        aml::telemetry::store().clear();
        return;
    }
    jsize len = env->GetArrayLength(payload);
    if (len <= 0 || len > 4096) {
        aml::telemetry::store().clear();
        return;
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(len));
    env->GetByteArrayRegion(payload, 0, len, reinterpret_cast<jbyte*>(bytes.data()));
    aml::telemetry::store().publish_payload(bytes.data(), bytes.size());
}

JNIEXPORT void JNICALL Java_amalgam_bridge_NativeBridge_nativeClearTelemetry(JNIEnv* env,
                                                                              jclass cls) {
    aml::telemetry::store().clear();
}

JNIEXPORT jint JNICALL Java_amalgam_bridge_NativeBridge_nativeShutdown(JNIEnv* env, jclass cls) {
    aml::modules_set_safe_mode(true);
    aml::modules_reset_runtime_state();
    aml::pipeline().clear();
    aml::esp::clear();
    aml::camera::clear();
    aml::player_stats::store().clear();
    aml::tracker::store().clear();
    aml::render::shutdown();
    return 0;
}

JNIEXPORT void JNICALL Java_amalgam_bridge_NativeBridge_nativeClearActions(JNIEnv* env, jclass cls) {
    aml::pipeline().clear();
    aml::modules_reset_runtime_state();
    aml::esp::clear();
    aml::camera::clear();
    aml::player_stats::store().clear();
    aml::tracker::store().clear();
    aml::tick_active().store(false, std::memory_order_release);
}

JNIEXPORT void JNICALL Java_amalgam_bridge_NativeBridge_nativeEnterTick(JNIEnv* env, jclass cls) {
    aml::tick_active().store(true, std::memory_order_release);
}

JNIEXPORT void JNICALL Java_amalgam_bridge_NativeBridge_nativeExitTick(JNIEnv* env, jclass cls) {
    aml::tick_active().store(false, std::memory_order_release);
}

JNIEXPORT jboolean JNICALL Java_amalgam_bridge_NativeBridge_nativeIsEnabled(JNIEnv* env, jclass cls, jint module_id) {
    if (!aml::bridge::valid_module(module_id)) return JNI_FALSE;
    return aml::module_enabled(static_cast<aml::ModuleId>(module_id)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_amalgam_bridge_NativeBridge_nativeSetEnabled(JNIEnv* env, jclass cls,
                                                                          jint module_id, jboolean enabled) {
    if (!aml::bridge::valid_module(module_id)) return;
    aml::module_set_enabled(static_cast<aml::ModuleId>(module_id), enabled == JNI_TRUE);
}

JNIEXPORT void JNICALL Java_amalgam_bridge_NativeBridge_nativeSetSafeMode(JNIEnv* env, jclass cls,
                                                                          jboolean enabled) {
    aml::hud::set_server_safe(enabled == JNI_TRUE);
}

JNIEXPORT jboolean JNICALL Java_amalgam_bridge_NativeBridge_nativeIsSafeMode(JNIEnv* env, jclass cls) {
    return aml::modules_safe_mode() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jfloat JNICALL Java_amalgam_bridge_NativeBridge_nativeParam(JNIEnv* env, jclass cls, jint module_id, jint idx) {
    if (!aml::bridge::valid_module(module_id)) return 0.0f;
    return aml::module_param(static_cast<aml::ModuleId>(module_id), idx);
}

JNIEXPORT jint JNICALL Java_amalgam_bridge_NativeBridge_nativeDrain(JNIEnv* env, jclass cls, jbyteArray snapshot, jbyteArray out) {
    using namespace aml;
    jlong snap_len = 0;
    uint8_t* snap_data = aml::bridge::byte_array_data(env, snapshot, &snap_len);
    if (!snap_data) return 0;
    Snapshot snap;
    snapshot_parse(snap, snap_data, static_cast<int>(snap_len));
    aml::bridge::release_byte_array(env, snapshot, snap_data, JNI_ABORT);
    aml::tracker::store().publish(snap);

    modules_evaluate(snap);

    jlong out_len = 0;
    uint8_t* out_data = aml::bridge::byte_array_data(env, out, &out_len);
    if (!out_data) return 0;
    uint32_t written = pipeline().drain(out_data, static_cast<uint32_t>(out_len));
    aml::bridge::release_byte_array(env, out, out_data, 0);
    return static_cast<jint>(written);
}

JNIEXPORT jint JNICALL Java_amalgam_bridge_NativeBridge_nativeSendNow(JNIEnv* env, jclass cls, jint module_id, jint kind, jfloat x, jfloat y, jfloat z, jfloat f, jint target_id, jint int_a, jint int_b) {
    using namespace aml;
    if (!tick_active().load(std::memory_order_acquire)) return -1;
    if (!aml::bridge::valid_module(module_id) || kind < ACT_SET_VELOCITY || kind > ACT_SELECT_SLOT ||
        !aml::bridge::finite_action_values(x, y, z, f)) return -3;
    Action a = make_action(action_seq().fetch_add(1), static_cast<uint8_t>(module_id), static_cast<uint8_t>(kind));
    a.x = x;
    a.y = y;
    a.z = z;
    a.f = f;
    a.target_id = target_id;
    a.int_a = int_a;
    a.int_b = int_b;
    return pipeline().push(a) ? 0 : -2;
}

JNIEXPORT void JNICALL Java_amalgam_bridge_NativeBridge_nativeSetParam(JNIEnv* env, jclass cls, jint module_id, jint idx, jfloat value) {
    if (!aml::bridge::valid_module(module_id) || !std::isfinite(value)) return;
    aml::module_set_param(static_cast<aml::ModuleId>(module_id), idx, value);
}

JNIEXPORT void JNICALL Java_amalgam_bridge_NativeBridge_nativePublishPlayerStats(JNIEnv* env, jclass cls, jbyteArray payload) {
    if (!payload) {
        aml::player_stats::store().clear();
        return;
    }
    jsize len = env->GetArrayLength(payload);
    if (len <= 0 || len > 2048) {
        aml::player_stats::store().clear();
        return;
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(len));
    env->GetByteArrayRegion(payload, 0, len, reinterpret_cast<jbyte*>(bytes.data()));
    aml::player_stats::State state;
    // len is bounded to 2048 above, so the narrowing cast is provably safe.
    if (aml::player_stats::decode_payload(state, bytes.data(),
                                          static_cast<int>(bytes.size()))) {
        aml::player_stats::store().publish(state);
    }
}

}  // extern "C"
