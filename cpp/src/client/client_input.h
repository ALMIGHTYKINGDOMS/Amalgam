#pragma once

namespace aml::client {

// Edge-triggered check: returns true only on the frame the menu key is first pressed.
bool menu_key_pressed_this_frame();

// True while the menu key is held down.
bool is_menu_key_held();

}  // namespace aml::client
