#include "core/tracker.h"

#include <chrono>
#include <cmath>
#include <iostream>
#include <thread>

namespace {

bool near(float a, float b) { return std::fabs(a - b) < 0.0001f; }

bool test_validation() {
    aml::tracker::Store s;
    aml::Snapshot snap{};
    snap.entity_count = 3;
    snap.entities[0].id = 0;   // invalid, skipped
    snap.entities[1].id = -5;  // invalid, skipped
    snap.entities[2].id = 42;
    snap.entities[2].kind = 2;
    snap.entities[2].dist_sq = 9.0f;
    s.publish(snap);
    auto list = s.entities();
    if (list.size() != 1) return false;
    return list[0].id == 42 && list[0].kind == aml::tracker::ENTITY_PLAYER &&
           near(list[0].distance_sq, 9.0f);
}

bool test_sort() {
    aml::tracker::Store s;
    aml::Snapshot snap{};
    snap.entity_count = 3;
    snap.entities[0].id = 1;
    snap.entities[0].dist_sq = 100.0f;
    snap.entities[1].id = 2;
    snap.entities[1].dist_sq = 4.0f;
    snap.entities[2].id = 3;
    snap.entities[2].dist_sq = 25.0f;
    s.publish(snap);
    auto list = s.entities();
    if (list.size() != 3) return false;
    return list[0].id == 2 && list[1].id == 3 && list[2].id == 1;
}

bool test_stale_prune() {
    aml::tracker::Store s;
    s.set_stale_timeout_ms(30);
    aml::Snapshot snap{};
    snap.entity_count = 1;
    snap.entities[0].id = 7;
    snap.entities[0].dist_sq = 1.0f;
    s.publish(snap);
    if (s.entities().size() != 1) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    return s.entities().empty();
}

bool test_v1_path() {
    aml::tracker::Store s;
    aml::Snapshot snap{};
    snap.hostile_count = 2;
    snap.hostiles[0].id = 0;  // invalid, skipped
    snap.hostiles[1].id = 11;
    snap.hostiles[1].dist_sq = 3.0f;
    s.publish(snap);
    auto list = s.entities();
    if (list.size() != 1) return false;
    return list[0].id == 11 && list[0].kind == aml::tracker::ENTITY_HOSTILE &&
           near(list[0].distance_sq, 3.0f);
}

}  // namespace

int main() {
    struct {
        const char* name;
        bool (*fn)();
    } tests[] = {{"validation", test_validation}, {"sort", test_sort},
                 {"stale_prune", test_stale_prune}, {"v1_path", test_v1_path}};
    bool ok = true;
    for (const auto& t : tests) {
        if (!t.fn()) {
            std::cerr << "FAILED: " << t.name << "\n";
            ok = false;
        }
    }
    return ok ? 0 : 1;
}