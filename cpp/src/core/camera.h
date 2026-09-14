#pragma once

#include <array>
#include <mutex>

namespace aml::camera {

struct Projection {
    std::array<float, 16> view_projection{};
    int width = 0;
    int height = 0;
    bool valid = false;
};

struct Pose {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    float yaw = 0.0f;
    float pitch = 0.0f;
    int width = 0;
    int height = 0;
    bool valid = false;
};

void publish(const float* matrix, int width, int height);
Projection snapshot();
void publish_pose(double x, double y, double z, float yaw, float pitch, int width, int height);
Pose pose();
void clear();
bool project_world(double x, double y, double z, const Pose& camera,
                   float& screen_x, float& screen_y, float& depth);

}  // namespace aml::camera
