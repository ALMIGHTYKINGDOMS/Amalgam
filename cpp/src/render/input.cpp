#include "render/input_state.h"

#include <windows.h>

namespace aml::input {

State& state() {
    static State st{};
    return st;
}

bool key_down(int vk) {
    if (vk < 0 || vk >= 256) return false;
    return state().keys[vk];
}

void poll() {
    for (int vk = 0; vk < 256; ++vk) {
        state().keys[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
    }
}

}  // namespace aml::input