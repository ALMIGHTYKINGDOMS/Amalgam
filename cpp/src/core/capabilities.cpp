#include "core/capabilities.h"

namespace aml::capabilities {

namespace {
std::atomic<uint32_t> g_value{0};
}

void set(uint32_t value) { g_value.store(value, std::memory_order_release); }
uint32_t get() { return g_value.load(std::memory_order_acquire); }
bool has(uint32_t value) { return (get() & value) == value; }

}  // namespace aml::capabilities
