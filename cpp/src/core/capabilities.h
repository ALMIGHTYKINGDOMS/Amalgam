#pragma once

#include <atomic>
#include <cstdint>

namespace aml::capabilities {

void set(uint32_t value);
uint32_t get();
bool has(uint32_t value);

}  // namespace aml::capabilities
