#pragma once

// Motion engine for the Amalgam launcher.
//
// All animation in the launcher flows through this one place so the "restrained
// premium" feel is consistent (120-180ms ease) and so the user's
// reduced-motion preference is honored by every animation with a single gate.
//
// Animations never delay actions: they are purely visual and always
// frame-rate independent (time based, not frame based).

#include "imgui.h"

#include <algorithm>

namespace aml::ui {

// ---------------------------------------------------------------------------
// Global gate
// ---------------------------------------------------------------------------

// Whether animations are enabled at all. Returns false when the user has
// reduced motion enabled (theme_ui reduced_motion). Cheap to call every frame.
bool motion_enabled();

// Registered by the theme UI when the user toggles Reduced Motion.
void set_reduced_motion(bool enabled);

// ---------------------------------------------------------------------------
// Frames requested by animation
//
// Easing sweeps call note_motion() while they are still moving, so the main
// loop can render at the display rate only while something is actually
// animating instead of holding a 60 Hz loop open for a window nobody is
// changing. The loop calls begin_motion_frame() before drawing and reads
// motion_in_flight() afterwards, which is why the counter is cleared per frame
// rather than left to drain on its own.
// ---------------------------------------------------------------------------

void begin_motion_frame();
void note_motion();
int motion_in_flight();

// Easing curve: cubic ease-out — fast start, gentle settle. This is the
// signature curve of the "restrained premium" feel.
float ease_out_cubic(float t);

// Linear interpolation helper between two values by t in [0,1].
inline float motion_lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

// Linear interpolation between two ImVec4 colors by t in [0,1].
inline ImVec4 motion_lerp_color(const ImVec4& a, const ImVec4& b, float t) {
    return ImVec4(motion_lerp(a.x, b.x, t), motion_lerp(a.y, b.y, t),
                  motion_lerp(a.z, b.z, t), motion_lerp(a.w, b.w, t));
}

// ---------------------------------------------------------------------------
// AnimFloat — a persistent animated value
//
// Typical usage:
//   static AnimFloat hover(0.0f);
//   hover.target(hovered ? 1.0f : 0.0f);
//   const float amount = hover.update(ImGui::GetIO().DeltaTime);
//   draw_with_alpha(amount);
// ---------------------------------------------------------------------------

struct AnimFloat {
    float current = 0.0f;
    float target_value = 0.0f;

    // How many seconds a full 0->1 sweep takes. Default 0.14s.
    float duration = 0.14f;

    void set(float value) {
        current = value;
        target_value = value;
    }

    void target(float value) {
        target_value = value;
    }

    bool settled() const {
        return (current - target_value) * (current - target_value) < 0.00001f;
    }

    // Advances the animation by delta seconds; returns the current value.
    // When reduced motion is on, snaps to the target instantly.
    float update(float delta_seconds) {
        if (!motion_enabled()) {
            current = target_value;
            return current;
        }
        if (settled()) return current;
        note_motion();
        const float t = std::clamp(delta_seconds / std::max(0.001f, duration), 0.0f, 1.0f);
        const float eased = ease_out_cubic(t);
        current = motion_lerp(current, target_value, eased);
        if (settled()) current = target_value;
        return current;
    }
};

// ---------------------------------------------------------------------------
// One-shot helpers
// ---------------------------------------------------------------------------

// Fades a popup/modal in: returns an alpha multiplier in [0,1] that starts at 0
// and approaches 1 over `duration` seconds. `started_at` is the wall time
// (ImGui::GetTime()) when the popup opened.
float popup_fade_in(float started_at, float duration = 0.15f);

}  // namespace aml::ui
