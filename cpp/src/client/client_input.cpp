#include "client/client_input.h"
#include "client/client_core.h"
#include "client/client_ui.h"
#include "render/input_state.h"

#include <windows.h>

namespace aml::client {

static bool g_prev_menu_key_held = false;

bool menu_key_pressed_this_frame() {
    int key = settings().menu_key;
    bool held = input::key_down(key);
    bool pressed = held && !g_prev_menu_key_held;
    g_prev_menu_key_held = held;
    return pressed;
}

bool is_menu_key_held() {
    return input::key_down(settings().menu_key);
}

}  // namespace aml::client
