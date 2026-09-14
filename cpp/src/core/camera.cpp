#include "core/camera.h"

#include <cmath>
#include <cstring>

namespace aml::camera {

namespace {
std::mutex g_mutex;
Projection g_projection;
Pose g_pose;
}

void publish(const float* matrix, int width, int height) {
    if (!matrix || width <= 0 || height <= 0) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    std::memcpy(g_projection.view_projection.data(), matrix, sizeof(float) * 16);
    g_projection.width = width;
    g_projection.height = height;
    g_projection.valid = true;
}

Projection snapshot() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_projection;
}

void publish_pose(double x, double y, double z, float yaw, float pitch, int width, int height) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_pose.x = x;
    g_pose.y = y;
    g_pose.z = z;
    g_pose.yaw = yaw;
    g_pose.pitch = pitch;
    g_pose.width = width;
    g_pose.height = height;
    g_pose.valid = width > 0 && height > 0;
}

Pose pose() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_pose;
}

void clear() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_projection = {};
    g_pose = {};
}

bool project_world(double x, double y, double z, const Pose& camera,
                   float& screen_x, float& screen_y, float& depth) {
    if (!camera.valid || camera.width <= 0 || camera.height <= 0) return false;
    constexpr float pi = 3.14159265358979323846f;
    constexpr float vertical_fov = 70.0f * pi / 180.0f;
    const float yaw = camera.yaw * pi / 180.0f;
    const float pitch = camera.pitch * pi / 180.0f;
    const float dx = static_cast<float>(x - camera.x);
    const float dy = static_cast<float>(y - camera.y);
    const float dz = static_cast<float>(z - camera.z);
    const float right = dx * std::cos(yaw) + dz * std::sin(yaw);
    const float forward_horizontal = -dx * std::sin(yaw) + dz * std::cos(yaw);
    const float forward = forward_horizontal * std::cos(pitch) + dy * std::sin(pitch);
    const float up = dy * std::cos(pitch) - forward_horizontal * std::sin(pitch);
    if (!std::isfinite(forward) || forward <= 0.001f) return false;
    const float aspect = static_cast<float>(camera.width) / static_cast<float>(camera.height);
    const float vertical_tan = std::tan(vertical_fov * 0.5f);
    const float horizontal_tan = vertical_tan * aspect;
    const float ndc_x = right / (forward * horizontal_tan);
    const float ndc_y = up / (forward * vertical_tan);
    screen_x = (ndc_x * 0.5f + 0.5f) * static_cast<float>(camera.width);
    screen_y = (1.0f - (ndc_y * 0.5f + 0.5f)) * static_cast<float>(camera.height);
    depth = forward;
    return std::isfinite(screen_x) && std::isfinite(screen_y) && std::isfinite(depth);
}

}  // namespace aml::camera
