#include "core/tracker.h"

#include <algorithm>
#include <chrono>

namespace aml::tracker {

namespace {

uint64_t now_ms() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

}  // namespace

void Store::publish(const Snapshot& snapshot) {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_ = snapshot;
    ++tick_;
    entities_.clear();
    uint64_t seen = now_ms();
    if (snapshot.entity_count > 0) {
        entities_.reserve(static_cast<size_t>(snapshot.entity_count));
        for (int i = 0; i < snapshot.entity_count; ++i) {
            // Validation: reject ids that cannot be a real entity (0 / negative).
            if (snapshot.entities[i].id <= 0) continue;
            Entity entity;
            entity.id = snapshot.entities[i].id;
            entity.kind = snapshot.entities[i].kind == 2 ? ENTITY_PLAYER : ENTITY_HOSTILE;
            entity.x = snapshot.entities[i].x;
            entity.y = snapshot.entities[i].y;
            entity.z = snapshot.entities[i].z;
            entity.distance_sq = snapshot.entities[i].dist_sq;
            entity.health = snapshot.entities[i].health;
            entity.flags = snapshot.entities[i].flags;
            entity.name = snapshot.entities[i].name;
            entity.team = snapshot.entities[i].team;
            entity.tick = tick_;
            entity.last_seen_ms = seen;
            entities_.push_back(entity);
        }
        return;
    }
    entities_.reserve(static_cast<size_t>(snapshot.hostile_count));
    for (int i = 0; i < snapshot.hostile_count; ++i) {
        if (snapshot.hostiles[i].id <= 0) continue;
        Entity entity;
        entity.id = snapshot.hostiles[i].id;
        entity.kind = ENTITY_HOSTILE;
        entity.distance_sq = snapshot.hostiles[i].dist_sq;
        entity.tick = tick_;
        entity.last_seen_ms = seen;
        entities_.push_back(entity);
    }
}

void Store::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_ = Snapshot{};
    entities_.clear();
    tick_ = 0;
}

Snapshot Store::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

std::vector<Entity> Store::entities() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Entity> out;
    uint64_t cutoff = now_ms() > stale_timeout_ms_ ? now_ms() - stale_timeout_ms_ : 0;
    for (const Entity& e : entities_) {
        if (e.last_seen_ms < cutoff) continue;
        out.push_back(e);
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const Entity& a, const Entity& b) { return a.distance_sq < b.distance_sq; });
    return out;
}

uint64_t Store::tick() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tick_;
}

void Store::set_stale_timeout_ms(uint64_t ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    stale_timeout_ms_ = ms;
}

uint64_t Store::stale_timeout_ms() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stale_timeout_ms_;
}

Store& store() {
    static Store instance;
    return instance;
}

}  // namespace aml::tracker
