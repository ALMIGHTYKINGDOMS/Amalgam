#pragma once

namespace aml::input {

struct State {
    bool keys[256];
};

State& state();

bool key_down(int vk);

void poll();

}  // namespace aml::input