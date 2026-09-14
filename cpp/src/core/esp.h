#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

namespace aml::esp {

struct ProjectedEntity {
    int32_t id = 0;
    float x = 0.0f;
    float y = 0.0f;
    float depth = 0.0f;
    int kind = 0;
};

void clear();
void publish(const ProjectedEntity& entity);
std::vector<ProjectedEntity> snapshot();

}  // namespace aml::esp
