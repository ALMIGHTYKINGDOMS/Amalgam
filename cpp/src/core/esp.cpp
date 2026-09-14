#include "core/esp.h"

#include <cmath>

namespace aml::esp {

namespace {
std::mutex g_mutex;
std::vector<ProjectedEntity> g_entities;
}

void clear() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_entities.clear();
}

void publish(const ProjectedEntity& entity) {
    if (!std::isfinite(entity.x) || !std::isfinite(entity.y) ||
        !std::isfinite(entity.depth) || entity.depth <= 0.0f) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_entities.size() >= 64) return;
    g_entities.push_back(entity);
}

std::vector<ProjectedEntity> snapshot() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_entities;
}

}  // namespace aml::esp
