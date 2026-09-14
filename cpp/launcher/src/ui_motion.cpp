#include "ui_motion.h"

#include <algorithm>

namespace aml::ui {

namespace {

// The reduced-motion preference lives in the theme UI state. Read it through
// a small indirection so the motion engine does not need to know about the
// theme module directly. The theme_ui module registers its flag here.
bool g_reduced_motion = false;

}  // namespace

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
