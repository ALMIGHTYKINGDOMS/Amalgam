#include "core/module.h"
#include "render/input_state.h"

#include <algorithm>
#include <cstring>
#include <atomic>
#include <cmath>
#include <mutex>

namespace aml {

namespace {

const Module k_default_modules[MOD_COUNT] = {
    {MOD_FREECAM, "Freecam", false, {0.5f, 0.15f, 0.f, 0.f, 0.f, 0.f}, module_freecam_tick},
    {MOD_FLY, "Fly", false, {5.f, 0.f, 0.f, 0.f, 0.f, 0.f}, module_fly_tick},
    {MOD_SPEED, "Speed", false, {1.6f, 0.f, 0.f, 0.f, 0.f, 0.f}, module_speed_tick},
    {MOD_NOFALL, "NoFall", false, {3.f, 0.f, 0.f, 0.f, 0.f, 0.f}, module_nofall_tick},
    {MOD_AUTOTOOL, "AutoTool", false, {0.f, 0.f, 0.f, 0.f, 0.f, 0.f}, module_autotool_tick},
    {MOD_KILLAURA, "KillAura", false, {3.5f, 8.f, 0.f, 0.f, 0.f, 0.f}, module_killaura_tick},
};
Module g_modules[MOD_COUNT] = {
    k_default_modules[0], k_default_modules[1], k_default_modules[2],
    k_default_modules[3], k_default_modules[4], k_default_modules[5],
};
std::mutex g_module_mutex;
std::atomic<bool> g_safe_mode{false};

float clamp_param(ModuleId id, int index, float value) {
    if (!std::isfinite(value)) return module_param(id, index);
    switch (id) {
        case MOD_FREECAM:
            return index == 0 ? std::clamp(value, 0.05f, 2.0f) :
                   index == 1 ? std::clamp(value, 0.1f, 4.0f) : value;
        case MOD_FLY:
            return index == 0 ? std::clamp(value, 1.0f, 20.0f) : value;
        case MOD_SPEED:
            return index == 0 ? std::clamp(value, 1.0f, 3.0f) : value;
        case MOD_NOFALL:
            return index == 0 ? std::clamp(value, 1.0f, 8.0f) : value;
        case MOD_KILLAURA:
            return index == 0 ? std::clamp(value, 1.0f, 6.0f) :
                   index == 1 ? std::clamp(value, 1.0f, 20.0f) : value;
        default:
            return value;
    }
}

}  // namespace

Module* module_get(ModuleId id) {
    for (Module& m : g_modules) {
        if (m.id == id) return &m;
    }
    return nullptr;
}

Module* modules_all() { return g_modules; }

size_t modules_count() { return MOD_COUNT; }

Module module_snapshot(size_t index) {
    std::lock_guard<std::mutex> lock(g_module_mutex);
    return index < MOD_COUNT ? g_modules[index] : Module{};
}

bool module_enabled(ModuleId id) {
    std::lock_guard<std::mutex> lock(g_module_mutex);
    Module* module = module_get(id);
    return module && module->enabled;
}

float module_param(ModuleId id, int index) {
    std::lock_guard<std::mutex> lock(g_module_mutex);
    Module* module = module_get(id);
    return module && index >= 0 && index < 6 ? module->params[index] : 0.0f;
}

void module_set_enabled(ModuleId id, bool enabled) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(g_module_mutex);
        Module* module = module_get(id);
        if (module && module->enabled != enabled) {
            module->enabled = enabled;
            changed = true;
        }
    }
    // A toggle must take effect immediately. Otherwise actions queued before
    // the user disabled a module could still execute on the next game tick.
    if (changed && !enabled) {
        if (id == MOD_FREECAM || id == MOD_AUTOTOOL || id == MOD_KILLAURA)
            modules_reset_runtime_state();
        pipeline().clear();
    }
}

void module_set_param(ModuleId id, int index, float value) {
    if (index < 0 || index >= 6) return;
    value = clamp_param(id, index, value);
    std::lock_guard<std::mutex> lock(g_module_mutex);
    Module* module = module_get(id);
    if (module) module->params[index] = value;
}

void modules_reset_defaults() {
    {
        std::lock_guard<std::mutex> lock(g_module_mutex);
        std::memcpy(g_modules, k_default_modules, sizeof(g_modules));
    }
    modules_reset_runtime_state();
    pipeline().clear();
    tick_active().store(false, std::memory_order_release);
}

void modules_set_safe_mode(bool enabled) {
    g_safe_mode.store(enabled, std::memory_order_release);
    if (enabled) {
        modules_reset_runtime_state();
        pipeline().clear();
        tick_active().store(false, std::memory_order_release);
    }
}
bool modules_safe_mode() { return g_safe_mode.load(std::memory_order_acquire); }

void modules_evaluate(const Snapshot& snap) {
    if (modules_safe_mode()) return;
    Module local[MOD_COUNT];
    {
        std::lock_guard<std::mutex> lock(g_module_mutex);
        std::memcpy(local, g_modules, sizeof(local));
    }
    uint32_t seq = action_seq().load(std::memory_order_relaxed);
    for (Module& m : local) {
        if (!m.enabled || !m.tick) continue;
        m.tick(snap, m.params, pipeline(), seq);
    }
    action_seq().store(seq, std::memory_order_relaxed);
}

}  // namespace aml
