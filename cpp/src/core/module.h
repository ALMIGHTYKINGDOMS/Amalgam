#pragma once

#include "core/pipeline.h"
#include "core/protocol.h"

#include <cstddef>

namespace aml {

typedef void (*ModuleTickFn)(const Snapshot& snap, const float* params, Pipeline& pipe, uint32_t& seq);

struct Module {
    ModuleId id;
    const char* name;
    bool enabled;
    float params[6];
    ModuleTickFn tick;
};

Module* module_get(ModuleId id);
Module* modules_all();
size_t modules_count();
Module module_snapshot(size_t index);
bool module_enabled(ModuleId id);
float module_param(ModuleId id, int index);
void module_set_enabled(ModuleId id, bool enabled);
void module_set_param(ModuleId id, int index, float value);
void modules_reset_defaults();
void modules_reset_runtime_state();
void modules_set_safe_mode(bool enabled);
bool modules_safe_mode();
void modules_evaluate(const Snapshot& snap);

void module_freecam_tick(const Snapshot& snap, const float* params, Pipeline& pipe, uint32_t& seq);
void module_fly_tick(const Snapshot& snap, const float* params, Pipeline& pipe, uint32_t& seq);
void module_speed_tick(const Snapshot& snap, const float* params, Pipeline& pipe, uint32_t& seq);
void module_nofall_tick(const Snapshot& snap, const float* params, Pipeline& pipe, uint32_t& seq);
void module_autotool_tick(const Snapshot& snap, const float* params, Pipeline& pipe, uint32_t& seq);
void module_killaura_tick(const Snapshot& snap, const float* params, Pipeline& pipe, uint32_t& seq);

}
