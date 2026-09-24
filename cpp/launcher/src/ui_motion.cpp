#include "ui_motion.h"

#include <algorithm>

namespace aml::ui {

namespace {

// The reduced-motion preference is applied from the persisted launcher config
// through a small indirection so the motion engine does not need to depend on
// the theme module or on storage.
bool g_reduced_motion = false;

// How many easing sweeps are in flight this frame. Written and read on the UI
// thread only.
int g_motion_in_flight = 0;

}  // namespace

void begin_motion_frame() {
    g_motion_in_flight = 0;
}

void note_motion() {
    ++g_motion_in_flight;
}

int motion_in_flight() {
    return g_motion_in_flight;
}

void set_reduced_motion(bool enabled) {
    g_reduced_motion = enabled;
}

bool motion_enabled() {
    return !g_reduced_motion;
}

float ease_out_cubic(float t) {
    const float s = std::clamp(t, 0.0f, 1.0f) - 1.0f;
    return s * s * s + 1.0f;
}

float popup_fade_in(float started_at, float duration) {
    if (!motion_enabled()) return 1.0f;
    const float elapsed = std::max(0.0f, static_cast<float>(ImGui::GetTime()) - started_at);
    if (elapsed >= duration) return 1.0f;
    return ease_out_cubic(elapsed / duration);
}

}  // namespace aml::ui
