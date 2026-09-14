#include "core/pipeline.h"

namespace aml {

static Pipeline g_pipeline;
static std::atomic<uint32_t> g_seq{1};
static std::atomic<bool> g_tick_active{false};

bool Pipeline::push(const Action& a) {
    if (!action_matches_module(a.module_id, a.kind)) return false;
    std::lock_guard<std::mutex> lock(mu_);
    if (size_ >= PIPELINE_CAPACITY) return false;
    items_[(head_ + size_) % PIPELINE_CAPACITY] = a;
    ++size_;
    return true;
}

void Pipeline::clear() {
    std::lock_guard<std::mutex> lock(mu_);
    head_ = 0;
    size_ = 0;
}

uint32_t Pipeline::drain(uint8_t* out, uint32_t cap) {
    std::lock_guard<std::mutex> lock(mu_);
    uint32_t count = size_;
    uint32_t max = cap / ACTION_BYTES;
    if (count > max) count = max;
    for (uint32_t k = 0; k < count; ++k) {
        const Action& a = items_[(head_ + k) % PIPELINE_CAPACITY];
        std::memcpy(out + k * ACTION_BYTES, &a, ACTION_BYTES);
    }
    if (count < size_) {
        head_ = (head_ + count) % PIPELINE_CAPACITY;
        size_ -= count;
    } else {
        head_ = 0;
        size_ = 0;
    }
    return count * ACTION_BYTES;
}

uint32_t Pipeline::pending() const {
    std::lock_guard<std::mutex> lock(mu_);
    return size_;
}

Pipeline& pipeline() { return g_pipeline; }

std::atomic<uint32_t>& action_seq() { return g_seq; }

std::atomic<bool>& tick_active() { return g_tick_active; }

}
