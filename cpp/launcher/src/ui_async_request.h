#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace aml::ui {

// A small hand-off object for UI-triggered background work.  The request owns
// no thread: callers keep workers in UiState so launcher shutdown joins them
// before UiState is destroyed.  The monotonically increasing generation means
// an older worker can never overwrite a newer request's result.
struct AsyncUiRequestResult {
    bool success = false;
    bool warning = false;
    std::string title;
    std::string detail;
    std::string payload_a;
    std::string payload_b;
    std::string payload_c;
    int64_t number_a = 0;
    // A compact, typed hand-off for local UI snapshots that need more than
    // one scalar (for example, a cache-usage breakdown).  Keeping it in the
    // request result avoids parsing display strings on the render thread.
    std::vector<int64_t> numbers;
    std::vector<std::string> items;
};

struct AsyncUiRequestState {
    std::atomic_bool working{false};
    std::atomic_uint64_t generation{0};
    mutable std::mutex mu;
    std::string action;
    uint64_t result_generation = 0;
    bool has_result = false;
    bool result_delivered = false;
    AsyncUiRequestResult result;
};

struct AsyncUiRequestSnapshot {
    bool working = false;
    uint64_t generation = 0;
    std::string action;
    uint64_t result_generation = 0;
    bool has_result = false;
    bool result_delivered = false;
    AsyncUiRequestResult result;
};

inline bool begin_async_ui_request(AsyncUiRequestState& state,
                                   const std::string& action,
                                   uint64_t* request_generation = nullptr) {
    bool expected = false;
    if (!state.working.compare_exchange_strong(expected, true)) return false;

    const uint64_t generation = state.generation.fetch_add(1) + 1;
    {
        std::lock_guard<std::mutex> lock(state.mu);
        state.action = action;
        state.result_generation = 0;
        state.has_result = false;
        state.result_delivered = false;
        state.result = {};
    }
    if (request_generation) *request_generation = generation;
    return true;
}

inline void complete_async_ui_request(AsyncUiRequestState& state,
                                      const std::string& action,
                                      uint64_t request_generation,
                                      AsyncUiRequestResult result) {
    std::lock_guard<std::mutex> lock(state.mu);
    // A caller may invalidate a request during teardown or replace it after a
    // future cancellation path.  Keep the prior result private in that case.
    if (state.generation.load() != request_generation ||
        state.action != action) {
        // Invalidation intentionally keeps the lane occupied until the
        // underlying worker returns: an HTTP call cannot be safely pretended
        // away, and a second stateful mutation must not race it.  Release that
        // action-less lane when the obsolete worker finally returns so retry
        // remains possible.
        if (state.action.empty() && state.working.load()) {
            state.working = false;
        }
        return;
    }
    state.result_generation = request_generation;
    state.result = std::move(result);
    state.has_result = true;
    state.result_delivered = false;
    state.working = false;
}

inline AsyncUiRequestSnapshot snapshot_async_ui_request(
    const AsyncUiRequestState& state) {
    AsyncUiRequestSnapshot snapshot;
    snapshot.working = state.working.load();
    snapshot.generation = state.generation.load();
    std::lock_guard<std::mutex> lock(state.mu);
    snapshot.action = state.action;
    snapshot.result_generation = state.result_generation;
    snapshot.has_result = state.has_result;
    snapshot.result_delivered = state.result_delivered;
    snapshot.result = state.result;
    return snapshot;
}

// A result remains owned by its originating surface until that surface has
// consumed it.  Starting another request during that short interval would
// overwrite a response that may contain identity-scoped form state, so it is
// part of the serialized lane just as much as an actively running worker.
inline bool async_ui_request_is_reserved(const AsyncUiRequestState& state) {
    const AsyncUiRequestSnapshot snapshot = snapshot_async_ui_request(state);
    return snapshot.working || (snapshot.has_result && !snapshot.result_delivered);
}

inline bool take_async_ui_request_result(AsyncUiRequestState& state,
                                         AsyncUiRequestSnapshot* completed) {
    std::lock_guard<std::mutex> lock(state.mu);
    if (!state.has_result || state.result_delivered) return false;
    state.result_delivered = true;
    if (completed) {
        completed->working = state.working.load();
        completed->generation = state.generation.load();
        completed->action = state.action;
        completed->result_generation = state.result_generation;
        completed->has_result = state.has_result;
        completed->result_delivered = true;
        completed->result = state.result;
    }
    return true;
}

// Marks a running operation obsolete without pretending the underlying HTTP
// call was cancelled.  It is intentionally only for teardown/future explicit
// cancellation paths; normal UI actions serialize one mutation at a time.
inline void invalidate_async_ui_request(AsyncUiRequestState& state) {
    std::lock_guard<std::mutex> lock(state.mu);
    // Advance the generation while holding the same mutex completion uses.
    // Otherwise a worker could observe the new generation before `action` is
    // cleared, reject its result, and leave the serialized lane occupied.
    state.generation.fetch_add(1);
    state.action.clear();
    state.has_result = false;
    state.result_delivered = false;
    state.result = {};
}

}  // namespace aml::ui
