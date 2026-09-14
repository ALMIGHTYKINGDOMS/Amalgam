#include "core/module.h"
#include "render/input_state.h"

#include <windows.h>

#include <cstring>
#include <cmath>
#include <iostream>

namespace {

aml::Action one_action(aml::Pipeline& pipeline) {
    uint8_t bytes[aml::ACTION_BYTES]{};
    if (pipeline.drain(bytes, sizeof(bytes)) != aml::ACTION_BYTES) return {};
    aml::Action action{};
    std::memcpy(&action, bytes, sizeof(action));
    return action;
}

void reset_input() { std::memset(aml::input::state().keys, 0, sizeof(aml::input::state().keys)); }

bool test_fly() {
    reset_input();
    aml::input::state().keys['W'] = true;
    aml::input::state().keys[VK_SPACE] = true;
    aml::Snapshot snapshot{};
    snapshot.yaw = 0.0f;
    float params[6] = {5.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    aml::Pipeline pipeline;
    uint32_t sequence = 0;
    aml::module_fly_tick(snapshot, params, pipeline, sequence);
    aml::Action action = one_action(pipeline);
    return action.kind == aml::ACT_SET_VELOCITY && action.module_id == aml::MOD_FLY &&
           action.z > 0.24f && action.y > 0.24f && sequence == 1;
}

bool test_speed_and_nofall() {
    reset_input();
    aml::input::state().keys['W'] = true;
    aml::Snapshot snapshot{};
    snapshot.on_ground = true;
    snapshot.vx = 0.1f;
    snapshot.vz = 0.1f;
    float speed_params[6] = {1.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    aml::Pipeline pipeline;
    uint32_t sequence = 0;
    aml::module_speed_tick(snapshot, speed_params, pipeline, sequence);
    aml::Action speed = one_action(pipeline);
    if (speed.kind != aml::ACT_SET_VELOCITY || speed.module_id != aml::MOD_SPEED) return false;

    snapshot.on_ground = false;
    snapshot.fall_distance = 4.0f;
    float nofall_params[6] = {2.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    aml::module_nofall_tick(snapshot, nofall_params, pipeline, sequence);
    aml::Action nofall = one_action(pipeline);
    return nofall.kind == aml::ACT_MOVE_ON_GROUND && nofall.module_id == aml::MOD_NOFALL && nofall.f == 1.0f;
}

bool test_autotool_and_freecam() {
    reset_input();
    aml::Snapshot snapshot{};
    snapshot.breaking = true;
    float params[6]{};
    aml::Pipeline pipeline;
    uint32_t sequence = 0;
    for (int i = 0; i < 4; ++i) aml::module_autotool_tick(snapshot, params, pipeline, sequence);
    aml::Action tool = one_action(pipeline);
    if (tool.kind != aml::ACT_SWAP_SLOT || tool.module_id != aml::MOD_AUTOTOOL) return false;

    reset_input();
    aml::modules_reset_runtime_state();
    aml::input::state().keys[VK_RIGHT] = true;
    aml::input::state().keys['D'] = true;
    float freecam_params[6] = {1.0f, 2.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    aml::module_freecam_tick(snapshot, freecam_params, pipeline, sequence);
    aml::Action camera = one_action(pipeline);
    if (camera.kind != aml::ACT_SET_POS || camera.module_id != aml::MOD_FREECAM) return false;
    aml::module_freecam_tick(snapshot, freecam_params, pipeline, sequence);
    aml::Action next_camera = one_action(pipeline);
    return next_camera.kind == aml::ACT_SET_POS && next_camera.module_id == aml::MOD_FREECAM &&
           next_camera.x > camera.x + 0.5f;
}

bool test_v2_hostile_targeting() {
    reset_input();
    aml::Snapshot snapshot{};
    snapshot.entity_count = 2;
    snapshot.entities[0].id = 11;
    snapshot.entities[0].kind = 2;
    snapshot.entities[0].dist_sq = 1.0f;
    snapshot.entities[1].id = 22;
    snapshot.entities[1].kind = 1;
    snapshot.entities[1].dist_sq = 4.0f;
    float params[6] = {10.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    aml::Pipeline pipeline;
    uint32_t sequence = 0;
    aml::module_killaura_tick(snapshot, params, pipeline, sequence);
    aml::Action action = one_action(pipeline);
    return action.kind == aml::ACT_ATTACK && action.module_id == aml::MOD_KILLAURA &&
           action.target_id == 22;
}

bool test_safe_mode() {
    reset_input();
    aml::input::state().keys['W'] = true;
    aml::pipeline().clear();
    aml::module_set_enabled(aml::MOD_FLY, true);
    aml::modules_set_safe_mode(true);
    aml::Snapshot snapshot{};
    aml::modules_evaluate(snapshot);
    bool blocked = aml::pipeline().pending() == 0;
    aml::modules_set_safe_mode(false);
    aml::module_set_enabled(aml::MOD_FLY, false);
    return blocked;
}

bool test_toggle_guards_and_reset() {
    reset_input();
    aml::modules_reset_defaults();
    if (aml::module_enabled(aml::MOD_FLY)) return false;

    aml::module_set_param(aml::MOD_FLY, 0, 999.0f);
    if (aml::module_param(aml::MOD_FLY, 0) != 20.0f) return false;
    aml::module_set_param(aml::MOD_FLY, 0, -10.0f);
    if (aml::module_param(aml::MOD_FLY, 0) != 1.0f) return false;
    aml::module_set_param(aml::MOD_FLY, 0, NAN);
    if (aml::module_param(aml::MOD_FLY, 0) != 1.0f) return false;

    aml::input::state().keys['W'] = true;
    aml::module_set_enabled(aml::MOD_FLY, true);
    aml::modules_evaluate(aml::Snapshot{});
    if (aml::pipeline().pending() == 0) return false;
    aml::module_set_enabled(aml::MOD_FLY, false);
    if (aml::pipeline().pending() != 0) return false;

    aml::modules_reset_defaults();
    return !aml::module_enabled(aml::MOD_FLY) &&
           aml::module_param(aml::MOD_FLY, 0) == 5.0f &&
           aml::pipeline().pending() == 0;
}

bool test_action_contract() {
    return aml::action_matches_module(aml::MOD_FREECAM, aml::ACT_SET_POS) &&
           aml::action_matches_module(aml::MOD_FLY, aml::ACT_SET_VELOCITY) &&
           aml::action_matches_module(aml::MOD_SPEED, aml::ACT_SET_VELOCITY) &&
           aml::action_matches_module(aml::MOD_NOFALL, aml::ACT_MOVE_ON_GROUND) &&
           aml::action_matches_module(aml::MOD_AUTOTOOL, aml::ACT_SWAP_SLOT) &&
           aml::action_matches_module(aml::MOD_AUTOTOOL, aml::ACT_SELECT_SLOT) &&
           aml::action_matches_module(aml::MOD_KILLAURA, aml::ACT_ATTACK) &&
           !aml::action_matches_module(aml::MOD_KILLAURA, aml::ACT_SET_POS) &&
           !aml::action_matches_module(aml::MOD_FREECAM, aml::ACT_ATTACK);
}

}  // namespace

int main() {
    struct Test {
        const char* name;
        bool (*run)();
    } tests[] = {{"fly", test_fly}, {"speed_nofall", test_speed_and_nofall},
                 {"autotool_freecam", test_autotool_and_freecam},
                 {"v2_hostile_targeting", test_v2_hostile_targeting},
                 {"safe_mode", test_safe_mode},
                 {"toggle_guards_reset", test_toggle_guards_and_reset},
                 {"action_contract", test_action_contract}};
    bool ok = true;
    for (const auto& test : tests) {
        if (!test.run()) {
            std::cerr << "FAILED: " << test.name << "\n";
            ok = false;
        }
    }
    return ok ? 0 : 1;
}
