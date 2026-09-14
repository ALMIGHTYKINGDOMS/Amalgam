#pragma once

#include "core/protocol.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>

namespace aml {

class Pipeline {
public:
    bool push(const Action& a);
    void clear();
    uint32_t drain(uint8_t* out, uint32_t cap);
    uint32_t pending() const;

private:
    mutable std::mutex mu_;
    std::array<Action, PIPELINE_CAPACITY> items_;
    uint32_t head_ = 0;
    uint32_t size_ = 0;
};

Pipeline& pipeline();

std::atomic<uint32_t>& action_seq();

std::atomic<bool>& tick_active();

}