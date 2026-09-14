#pragma once

#include "core/protocol.h"

#include <cstdint>
#include <mutex>
#include <vector>

namespace aml::tracker {

enum EntityKind : uint8_t { ENTITY_UNKNOWN = 0, ENTITY_HOSTILE = 1, ENTITY_PLAYER = 2 };

struct Entity {
    int32_t id = 0;
    EntityKind kind = ENTITY_UNKNOWN;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float distance_sq = 0.0f;
    float health = -1.0f;
    uint32_t flags = 0;
    std::string name;
    std::string team;
    uint64_t tick = 0;
    uint64_t last_seen_ms = 0;
};

class Store {
public:
    void publish(const Snapshot& snapshot);
    void clear();
    Snapshot snapshot() const;
    std::vector<Entity> entities() const;
    uint64_t tick() const;

    void set_stale_timeout_ms(uint64_t ms);
    uint64_t stale_timeout_ms() const;

private:
    mutable std::mutex mutex_;
    Snapshot snapshot_{};
    std::vector<Entity> entities_;
    uint64_t tick_ = 0;
    uint64_t stale_timeout_ms_ = 3000;
};

Store& store();

}  // namespace aml::tracker
