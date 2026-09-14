#include "core/camera.h"

#include <cmath>
#include <iostream>

int main() {
    aml::camera::Pose camera;
    camera.valid = true;
    camera.width = 1920;
    camera.height = 1080;
    camera.yaw = 0.0f;
    camera.pitch = 0.0f;
    float x = 0.0f;
    float y = 0.0f;
    float depth = 0.0f;
    if (!aml::camera::project_world(0.0, 0.0, 10.0, camera, x, y, depth) ||
        std::fabs(x - 960.0f) > 1.0f || std::fabs(y - 540.0f) > 1.0f || depth <= 0.0f) {
        std::cerr << "center projection failed\n";
        return 1;
    }
    if (aml::camera::project_world(0.0, 0.0, -1.0, camera, x, y, depth)) {
        std::cerr << "behind-camera projection was accepted\n";
        return 1;
    }
    return 0;
}
