#include "core/module.h"
#include "core/protocol.h"
#include "render/input_state.h"

#include <cmath>
#include <mutex>

namespace aml {

namespace {

constexpr float PI = 3.14159265358979323846f;

struct FreecamRuntime {
    bool initialized = false;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float yaw = 0.0f;
    float pitch = 0.0f;
};

std::mutex g_runtime_mutex;
FreecamRuntime g_freecam;
int g_autotool_counter = 0;
int g_killaura_counter = 0;

void move_dir(float yaw_rad, bool w, bool s, bool a, bool d, float& mx, float& mz) {
    float fx = -std::sin(yaw_rad);
    float fz = std::cos(yaw_rad);
    float rx = std::cos(yaw_rad);
    float rz = std::sin(yaw_rad);
    mx = (w ? fx : 0.f) + (s ? -fx : 0.f) + (d ? rx : 0.f) + (a ? -rx : 0.f);
    mz = (w ? fz : 0.f) + (s ? -fz : 0.f) + (d ? rz : 0.f) + (a ? -rz : 0.f);
    float len = std::sqrt(mx * mx + mz * mz);
    if (len > 0.f) {
        mx /= len;
        mz /= len;
    }
}

}  // namespace

void module_freecam_tick(const Snapshot& snap, const float* p, Pipeline& pipe, uint32_t& seq) {
    input::State& st = input::state();

    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    if (!g_freecam.initialized) {
        g_freecam.initialized = true;
        g_freecam.x = snap.px;
        g_freecam.y = snap.py;
        g_freecam.z = snap.pz;
        g_freecam.yaw = snap.yaw;
        g_freecam.pitch = snap.pitch;
    }
    float rate = p[1];
    if (st.keys[39]) g_freecam.yaw += rate;
    if (st.keys[37]) g_freecam.yaw -= rate;
    if (st.keys[38]) g_freecam.pitch += rate;
    if (st.keys[40]) g_freecam.pitch -= rate;
    if (g_freecam.pitch > 89.f) g_freecam.pitch = 89.f;
    if (g_freecam.pitch < -89.f) g_freecam.pitch = -89.f;
    if (g_freecam.yaw > 180.f) g_freecam.yaw -= 360.f;
    if (g_freecam.yaw < -180.f) g_freecam.yaw += 360.f;

    float mx, mz;
    move_dir(g_freecam.yaw * PI / 180.f, st.keys[87], st.keys[83], st.keys[65], st.keys[68], mx, mz);
    float speed = p[0];
    g_freecam.x += mx * speed;
    g_freecam.y += (st.keys[32] ? speed : 0.f) + (st.keys[160] ? -speed : 0.f);
    g_freecam.z += mz * speed;

    Action a = make_action(++seq, MOD_FREECAM, ACT_SET_POS);
    a.x = g_freecam.x;
    a.y = g_freecam.y;
    a.z = g_freecam.z;
    a.f = g_freecam.yaw;
    a.int_a = static_cast<int32_t>(g_freecam.pitch * 1000.f);
    pipe.push(a);
}

void modules_reset_runtime_state() {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    g_freecam = {};
    g_autotool_counter = 0;
    g_killaura_counter = 0;
}

void module_fly_tick(const Snapshot& snap, const float* p, Pipeline& pipe, uint32_t& seq) {
    input::State& st = input::state();
    float mx, mz;
    move_dir(snap.yaw * PI / 180.f, st.keys[87], st.keys[83], st.keys[65], st.keys[68], mx, mz);
    float per_tick = p[0] / 20.f;
    float vy = 0.f;
    if (st.keys[32]) vy = per_tick;
    if (st.keys[160]) vy = -per_tick;
    if (st.keys[162]) vy = 0.f;
    Action a = make_action(++seq, MOD_FLY, ACT_SET_VELOCITY);
    a.x = mx * per_tick;
    a.y = vy;
    a.z = mz * per_tick;
    pipe.push(a);
}

void module_speed_tick(const Snapshot& snap, const float* p, Pipeline& pipe, uint32_t& seq) {
    input::State& st = input::state();
    if (!snap.on_ground) return;
    float mx, mz;
    move_dir(snap.yaw * PI / 180.f, st.keys[87], st.keys[83], st.keys[65], st.keys[68], mx, mz);
    if (mx == 0.f && mz == 0.f) return;
    float base = std::sqrt(snap.vx * snap.vx + snap.vz * snap.vz);
    if (base < 0.05f) base = 0.1f;
    Action a = make_action(++seq, MOD_SPEED, ACT_SET_VELOCITY);
    a.x = mx * base * p[0];
    a.y = snap.vy;
    a.z = mz * base * p[0];
    pipe.push(a);
}

void module_nofall_tick(const Snapshot& snap, const float* p, Pipeline& pipe, uint32_t& seq) {
    if (snap.on_ground) return;
    if (snap.fall_distance < p[0]) return;
    Action a = make_action(++seq, MOD_NOFALL, ACT_MOVE_ON_GROUND);
    a.f = 1.f;
    pipe.push(a);
}

void module_autotool_tick(const Snapshot& snap, const float* p, Pipeline& pipe, uint32_t& seq) {
    (void)p; // Reserved module-parameter slot; Auto Tool has no tunable values yet.
    if (!snap.breaking) return;
    {
        std::lock_guard<std::mutex> lock(g_runtime_mutex);
        if (++g_autotool_counter % 4 != 0) return;
    }
    Action a = make_action(++seq, MOD_AUTOTOOL, ACT_SWAP_SLOT);
    a.int_a = -1;
    a.int_b = -1;
    pipe.push(a);
}

void module_killaura_tick(const Snapshot& snap, const float* p, Pipeline& pipe, uint32_t& seq) {
    if (snap.hurt_time > 0.f) return;
    {
        std::lock_guard<std::mutex> lock(g_runtime_mutex);
        if (++g_killaura_counter < static_cast<int>(p[1])) return;
        g_killaura_counter = 0;
    }
    int best = -1;
    float best_d = p[0] * p[0];
    if (snap.entity_count > 0) {
        for (int k = 0; k < snap.entity_count; ++k) {
            // V2 entity kind 1 is a hostile; kind 2 is a player and is not a
            // valid target for this module.
            if (snap.entities[k].kind == 1 && snap.entities[k].dist_sq < best_d) {
                best_d = snap.entities[k].dist_sq;
                best = snap.entities[k].id;
            }
        }
    } else {
        for (int k = 0; k < snap.hostile_count; ++k) {
            if (snap.hostiles[k].dist_sq < best_d) {
                best_d = snap.hostiles[k].dist_sq;
                best = snap.hostiles[k].id;
            }
        }
    }
    if (best < 0) return;
    Action a = make_action(++seq, MOD_KILLAURA, ACT_ATTACK);
    a.target_id = best;
    pipe.push(a);
}

}  // namespace aml
