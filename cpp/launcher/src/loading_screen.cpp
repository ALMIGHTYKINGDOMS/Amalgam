#include "ui_internal.h"
#include "loading_screen.h"

#include <algorithm>
#include <cmath>

namespace aml::ui {

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------

LoadingScreen& LoadingScreen::instance() {
    static LoadingScreen s;
    return s;
}

LoadingScreen::LoadingScreen() = default;

// ---------------------------------------------------------------------------
// Animation name
// ---------------------------------------------------------------------------

const char* LoadingScreen::animation_name(LoadingAnimation anim) {
    switch (anim) {
        case LoadingAnimation::Spinner:         return "Spinner";
        case LoadingAnimation::Pulse:           return "Pulse";
        case LoadingAnimation::Wave:            return "Wave";
        case LoadingAnimation::ProgressBar:     return "Progress Bar";
        case LoadingAnimation::MinecraftBlocks: return "Minecraft Blocks";
        case LoadingAnimation::DiamondGrid:     return "Diamond Grid";
        case LoadingAnimation::Ripple:          return "Ripple";
        case LoadingAnimation::TypingDots:      return "Typing Dots";
        case LoadingAnimation::ParticleBurst:   return "Particle Burst";
        case LoadingAnimation::GradientSweep:   return "Gradient Sweep";
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------
// Show / hide
// ---------------------------------------------------------------------------

void LoadingScreen::show(LoadingAnimation anim, LoadingSize size,
                         const std::string& title, const std::string& subtitle) {
    state_.visible = true;
    state_.animation = anim;
    state_.size = size;
    state_.title = title;
    state_.subtitle = subtitle;
    state_.status_text.clear();
    state_.progress = -1.0f;
    state_.elapsed = 0.0f;
    state_.steps.clear();
    state_.current_step = -1;
    state_.cancellable = false;
    state_.on_cancel = nullptr;
}

void LoadingScreen::show_fullscreen(const std::string& title,
                                    const std::string& subtitle,
                                    LoadingAnimation anim) {
    show(anim, LoadingSize::FullScreen, title, subtitle);
}

void LoadingScreen::hide() {
    state_.visible = false;
    state_.elapsed = 0.0f;
    state_.steps.clear();
    state_.current_step = -1;
}

bool LoadingScreen::is_visible() const {
    return state_.visible;
}

// ---------------------------------------------------------------------------
// Progress
// ---------------------------------------------------------------------------

void LoadingScreen::set_progress(float progress) {
    state_.progress = std::max(-1.0f, std::min(1.0f, progress));
}

void LoadingScreen::set_status(const std::string& status) {
    state_.status_text = status;
}

void LoadingScreen::set_title(const std::string& title) {
    state_.title = title;
}

void LoadingScreen::set_subtitle(const std::string& subtitle) {
    state_.subtitle = subtitle;
}

// ---------------------------------------------------------------------------
// Steps
// ---------------------------------------------------------------------------

void LoadingScreen::begin_steps(const std::vector<std::string>& step_labels) {
    state_.steps.clear();
    state_.current_step = -1;
    for (auto& label : step_labels) {
        LoadingScreenState::Step s;
        s.label = label;
        s.completed = false;
        s.active = false;
        s.failed = false;
        state_.steps.push_back(s);
    }
    // Auto-activate first step
    if (!state_.steps.empty()) {
        state_.current_step = 0;
        state_.steps[0].active = true;
    }
}

void LoadingScreen::advance_step(const std::string& status) {
    if (state_.current_step < 0 || state_.current_step >= (int)state_.steps.size()) return;
    state_.steps[state_.current_step].active = false;
    state_.steps[state_.current_step].completed = true;
    state_.current_step++;
    if (state_.current_step < (int)state_.steps.size()) {
        state_.steps[state_.current_step].active = true;
    }
    if (!status.empty()) state_.status_text = status;
}

void LoadingScreen::fail_step(const std::string& error) {
    if (state_.current_step < 0 || state_.current_step >= (int)state_.steps.size()) return;
    state_.steps[state_.current_step].active = false;
    state_.steps[state_.current_step].failed = true;
    if (!error.empty()) state_.status_text = error;
}

void LoadingScreen::complete_all_steps() {
    for (auto& s : state_.steps) {
        s.completed = true;
        s.active = false;
        s.failed = false;
    }
    state_.current_step = (int)state_.steps.size();
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

void LoadingScreen::set_animation(LoadingAnimation anim) {
    state_.animation = anim;
}

void LoadingScreen::set_size(LoadingSize size) {
    state_.size = size;
}

void LoadingScreen::set_animation_speed(float speed) {
    state_.animation_speed = std::max(0.1f, speed);
}

void LoadingScreen::set_cancellable(bool cancellable, std::function<void()> on_cancel) {
    state_.cancellable = cancellable;
    state_.on_cancel = std::move(on_cancel);
}

// ---------------------------------------------------------------------------
// Render entry point
// ---------------------------------------------------------------------------

void LoadingScreen::render(float delta_time) {
    if (!state_.visible) return;
    state_.elapsed += delta_time * state_.animation_speed;

    if (state_.size == LoadingSize::FullScreen) {
        render_fullscreen_overlay();
    } else {
        render_centered_widget();
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static float anim_radius(LoadingSize size) {
    switch (size) {
        case LoadingSize::Small:      return ui_px(24.0f);
        case LoadingSize::Medium:     return ui_px(40.0f);
        case LoadingSize::Large:      return ui_px(60.0f);
        case LoadingSize::FullScreen: return ui_px(60.0f);
    }
    return ui_px(40.0f);
}

static float bar_width(LoadingSize size) {
    switch (size) {
        case LoadingSize::Small:      return ui_px(120.0f);
        case LoadingSize::Medium:     return ui_px(180.0f);
        case LoadingSize::Large:      return ui_px(260.0f);
        case LoadingSize::FullScreen: return ui_px(300.0f);
    }
    return ui_px(180.0f);
}

// Ease in-out cubic
static float ease_in_out(float t) {
    return t < 0.5f ? 4.0f * t * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) / 2.0f;
}

// ---------------------------------------------------------------------------
// Animation: Spinner
// ---------------------------------------------------------------------------

void LoadingScreen::render_spinner(ImDrawList* dl, const ImVec2& center,
                                   float radius, float t) {
    const int dots = 8;
    const float base_angle = t * 6.283185f; // full rotation per second
    for (int i = 0; i < dots; i++) {
        float phase = (float)i / (float)dots;
        float angle = base_angle + phase * 6.283185f;
        float opacity = 0.15f + 0.85f * phase;
        float r = radius * 0.7f;
        ImVec2 pos(center.x + std::cos(angle) * r, center.y + std::sin(angle) * r);
        float dot_r = ui_px(2.0f) + ui_px(1.5f) * phase;
        ImVec4 col = k.brand;
        col.w = opacity;
        dl->AddCircleFilled(pos, dot_r, c32(col));
    }
}

// ---------------------------------------------------------------------------
// Animation: Pulse
// ---------------------------------------------------------------------------

void LoadingScreen::render_pulse(ImDrawList* dl, const ImVec2& center,
                                 float radius, float t) {
    float cycle = std::fmod(t, 1.0f);
    float scale = 0.3f + 0.7f * ease_in_out(cycle);
    float opacity = 0.2f + 0.8f * (1.0f - cycle);

    ImVec4 col = k.brand;
    col.w = opacity;
    float r = radius * scale;
    dl->AddCircleFilled(center, r, c32(col));

    // Inner ring
    ImVec4 col2 = k.brand;
    col2.w = opacity * 0.5f;
    dl->AddCircle(center, r * 0.6f, c32(col2), 0, ui_px(1.5f));
}

// ---------------------------------------------------------------------------
// Animation: Wave
// ---------------------------------------------------------------------------

void LoadingScreen::render_wave(ImDrawList* dl, const ImVec2& center,
                                float radius, float t) {
    const int bars = 5;
    float bar_w = ui_px(4.0f);
    float gap = ui_px(3.0f);
    float total_w = bars * bar_w + (bars - 1) * gap;
    float start_x = center.x - total_w * 0.5f;

    for (int i = 0; i < bars; i++) {
        float phase = std::fmod(t * 3.0f + (float)i * 0.3f, 2.0f);
        float h;
        if (phase < 1.0f)
            h = radius * 0.3f + radius * 0.7f * ease_in_out(phase);
        else
            h = radius * 1.0f - radius * 0.7f * ease_in_out(phase - 1.0f);

        float x = start_x + i * (bar_w + gap);
        ImVec2 top(x + bar_w * 0.5f, center.y - h * 0.5f);
        ImVec2 bot(x + bar_w * 0.5f, center.y + h * 0.5f);

        ImVec4 col = k.brand;
        col.w = 0.7f + 0.3f * std::sin(t * 3.0f + (float)i);
        dl->AddLine(top, bot, c32(col), bar_w);
    }
}

// ---------------------------------------------------------------------------
// Animation: Progress Bar
// ---------------------------------------------------------------------------

void LoadingScreen::render_progress_bar(ImDrawList* dl, const ImVec2& center,
                                        float width, float t) {
    float h = ui_px(6.0f);
    float r = h * 0.5f;
    ImVec2 p0(center.x - width * 0.5f, center.y - h * 0.5f);
    ImVec2 p1(center.x + width * 0.5f, center.y + h * 0.5f);

    // Background track
    ImVec4 bg = k.surface;
    bg.w = 0.6f;
    dl->AddRectFilled(p0, p1, c32(bg), r);

    // Fill
    float fill;
    if (state_.progress >= 0.0f) {
        fill = state_.progress;
    } else {
        // Indeterminate: bouncing fill
        float cycle = std::fmod(t, 2.0f);
        fill = cycle < 1.0f ? ease_in_out(cycle) : 2.0f - ease_in_out(cycle);
        fill = 0.2f + fill * 0.5f;
    }

    float fill_w = width * fill;
    ImVec2 fp0 = p0;
    ImVec2 fp1(p0.x + fill_w, p1.y);

    // Gradient-like: use brand color with slight variation
    ImVec4 col = k.brand;
    dl->AddRectFilled(fp0, fp1, c32(col), r);

    // Moving highlight
    float sweep = std::fmod(t * 1.5f, 1.5f) - 0.25f;
    float hl_x = p0.x + fill_w * sweep;
    ImVec4 hl = {1.0f, 1.0f, 1.0f, 0.35f};
    dl->AddRectFilled(ImVec2(hl_x - ui_px(12.0f), p0.y),
                      ImVec2(hl_x + ui_px(12.0f), p1.y), c32(hl), r);
}

// ---------------------------------------------------------------------------
// Animation: Minecraft Blocks
// ---------------------------------------------------------------------------

void LoadingScreen::render_minecraft_blocks(ImDrawList* dl, const ImVec2& center,
                                            float radius, float t) {
    struct Block {
        float speed;
        float offset_x;
        float angle;
        float size;
        ImVec4 color;
    };

    static const Block blocks[] = {
        { 1.0f, -0.6f, 0.0f, 1.0f, {0.45f, 0.75f, 0.35f, 1.0f} }, // grass green
        { 1.4f,  0.3f, 1.2f, 0.8f, {0.55f, 0.40f, 0.25f, 1.0f} }, // dirt brown
        { 0.8f, -0.2f, 2.5f, 0.9f, {0.60f, 0.60f, 0.60f, 1.0f} }, // stone gray
        { 1.2f,  0.5f, 4.0f, 0.7f, {0.85f, 0.75f, 0.20f, 1.0f} }, // gold
        { 1.6f,  0.0f, 5.5f, 0.6f, {0.30f, 0.55f, 0.85f, 1.0f} }, // lapis blue
        { 1.1f, -0.4f, 3.3f, 0.75f,{0.80f, 0.25f, 0.20f, 1.0f} }, // redstone
    };

    const int count = 6;
    for (int i = 0; i < count; i++) {
        auto& b = blocks[i];
        float cycle = std::fmod(t * b.speed + b.offset_x, 3.0f);
        float life = cycle / 3.0f; // 0..1

        float y = center.y + radius * 0.4f - life * radius * 1.2f;
        float x = center.x + std::sin(b.angle + t * b.speed) * radius * 0.4f;
        float sz = ui_px(8.0f) * b.size * (1.0f - life * 0.3f);

        float alpha = (life < 0.1f) ? life / 0.1f
                    : (life > 0.7f) ? (1.0f - life) / 0.3f
                    : 1.0f;
        alpha = std::max(0.0f, std::min(1.0f, alpha));

        ImVec4 col = b.color;
        col.w = alpha;

        // Draw blocky square
        ImVec2 bp0(x - sz * 0.5f, y - sz * 0.5f);
        ImVec2 bp1(x + sz * 0.5f, y + sz * 0.5f);
        dl->AddRectFilled(bp0, bp1, c32(col));

        // Subtle highlight edge (top-left)
        ImVec4 hi = {1.0f, 1.0f, 1.0f, alpha * 0.25f};
        dl->AddRect(bp0, bp1, c32(hi), 0.0f, 0, ui_px(1.0f));
    }
}

// ---------------------------------------------------------------------------
// Animation: Diamond Grid
// ---------------------------------------------------------------------------

void LoadingScreen::render_diamond_grid(ImDrawList* dl, const ImVec2& center,
                                        float radius, float t) {
    const int rows = 3, cols = 3;
    float spacing = radius * 0.45f;

    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            int idx = r * cols + c;
            float delay = idx * 0.12f;
            float local_t = std::fmod(t + delay, 2.0f);
            float scale = (local_t < 1.0f)
                ? 0.3f + 0.7f * ease_in_out(local_t)
                : 1.0f - 0.7f * ease_in_out(local_t - 1.0f);

            float alpha = 0.3f + 0.7f * scale;
            float sz = ui_px(7.0f) * scale;

            float ox = (c - 1) * spacing;
            float oy = (r - 1) * spacing;
            ImVec2 cpos(center.x + ox, center.y + oy);

            // Diamond = rotated square via 4 vertices
            ImVec2 v0(cpos.x, cpos.y - sz);
            ImVec2 v1(cpos.x + sz, cpos.y);
            ImVec2 v2(cpos.x, cpos.y + sz);
            ImVec2 v3(cpos.x - sz, cpos.y);

            ImVec4 col = k.brand;
            col.w = alpha;
            dl->AddQuadFilled(v0, v1, v2, v3, c32(col));
        }
    }
}

// ---------------------------------------------------------------------------
// Animation: Ripple
// ---------------------------------------------------------------------------

void LoadingScreen::render_ripple(ImDrawList* dl, const ImVec2& center,
                                  float radius, float t) {
    const int rings = 3;
    for (int i = 0; i < rings; i++) {
        float delay = i * 0.4f;
        float cycle = std::fmod(t * 0.8f + delay, 2.0f);
        float r = radius * 0.15f + radius * 0.85f * ease_in_out(std::min(cycle, 1.0f));
        float alpha = 1.0f - std::min(cycle / 2.0f, 1.0f);
        alpha = std::max(0.0f, alpha);

        ImVec4 col = k.brand;
        col.w = alpha * 0.7f;
        dl->AddCircle(center, r, c32(col), 64, ui_px(2.0f));
    }
}

// ---------------------------------------------------------------------------
// Animation: Typing Dots
// ---------------------------------------------------------------------------

void LoadingScreen::render_typing_dots(ImDrawList* dl, const ImVec2& center,
                                       float radius, float t) {
    const int dots = 3;
    float spacing = radius * 0.5f;
    float dot_r = ui_px(4.5f);

    for (int i = 0; i < dots; i++) {
        float phase = std::fmod(t * 3.0f + (float)i * 0.4f, 2.0f);
        float bounce;
        if (phase < 1.0f)
            bounce = -ease_in_out(phase) * radius * 0.3f;
        else
            bounce = -ease_in_out(2.0f - phase) * radius * 0.3f;

        float x = center.x + ((float)i - 1.0f) * spacing;
        float y = center.y + bounce;

        float alpha = 0.4f + 0.6f * (1.0f + std::sin(t * 3.0f + (float)i)) * 0.5f;
        ImVec4 col = k.brand;
        col.w = alpha;
        dl->AddCircleFilled(ImVec2(x, y), dot_r, c32(col));
    }
}

// ---------------------------------------------------------------------------
// Animation: Particle Burst
// ---------------------------------------------------------------------------

void LoadingScreen::render_particle_burst(ImDrawList* dl, const ImVec2& center,
                                          float radius, float t) {
    const int count = 14;
    float cycle = std::fmod(t, 1.5f);
    float progress = cycle / 1.5f;

    for (int i = 0; i < count; i++) {
        float angle = (float)i / (float)count * 6.283185f;
        float dist = radius * ease_in_out(progress) * (0.6f + 0.4f * std::sin((float)i * 2.3f));
        float alpha = 1.0f - progress;
        alpha = std::max(0.0f, alpha * alpha);

        float x = center.x + std::cos(angle) * dist;
        float y = center.y + std::sin(angle) * dist;
        float sz = ui_px(2.0f) + ui_px(1.0f) * (1.0f - progress);

        ImVec4 col = k.brand;
        col.w = alpha;
        dl->AddCircleFilled(ImVec2(x, y), sz, c32(col));
    }
}

// ---------------------------------------------------------------------------
// Animation: Gradient Sweep
// ---------------------------------------------------------------------------

void LoadingScreen::render_gradient_sweep(ImDrawList* dl, const ImVec2& center,
                                          float radius, float t) {
    // Rotating arc with fading trail
    float sweep_angle = t * 3.0f; // radians per second
    int segments = 48;
    float trail = 2.5f; // trail length in radians

    for (int i = 0; i < segments; i++) {
        float frac = (float)i / (float)segments;
        float angle = sweep_angle - trail * frac;
        float alpha = (1.0f - frac) * 0.8f;
        float r = radius * (0.5f + 0.5f * (1.0f - frac));

        float x = center.x + std::cos(angle) * r;
        float y = center.y + std::sin(angle) * r;

        ImVec4 col = k.brand;
        col.w = alpha;
        float sz = ui_px(2.5f) * (1.0f - frac * 0.5f);
        dl->AddCircleFilled(ImVec2(x, y), sz, c32(col));
    }

    // Bright leading dot
    float lx = center.x + std::cos(sweep_angle) * radius;
    float ly = center.y + std::sin(sweep_angle) * radius;
    ImVec4 lead = {1.0f, 1.0f, 1.0f, 0.9f};
    dl->AddCircleFilled(ImVec2(lx, ly), ui_px(3.5f), c32(lead));
}

// ---------------------------------------------------------------------------
// Fullscreen overlay
// ---------------------------------------------------------------------------

void LoadingScreen::render_fullscreen_overlay() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 vp_min = vp->Pos;
    ImVec2 vp_max(vp_min.x + vp->Size.x, vp_min.y + vp->Size.y);

    ImGui::SetNextWindowPos(vp_min);
    ImGui::SetNextWindowSize(vp->Size);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration
                           | ImGuiWindowFlags_NoBackground
                           | ImGuiWindowFlags_NoInputs
                           | ImGuiWindowFlags_NoFocusOnAppearing
                           | ImGuiWindowFlags_NoNav
                           | ImGuiWindowFlags_NoBringToFrontOnFocus
                           | ImGuiWindowFlags_NoScrollWithMouse;

    ImGui::Begin("##LoadingOverlay", nullptr, flags);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Original Minecraft-themed loading scene with a dark veil so the real
    // progress state remains readable. The hero art is optional at runtime
    // and falls back to the established loading scene for minimal packages.
    dl->AddRectFilled(vp_min, vp_max, IM_COL32(0, 0, 0, 255));
    if (app) {
        draw_local_image(*app,
                         app->exe_dir + L"\\branding\\ai\\amalgam-loading-hero-ai.png",
                         vp_min, vp->Size, IM_COL32(5, 3, 13, 255),
                         ui_model::ImageFit::Cover);
        dl->AddRectFilled(vp_min, vp_max, IM_COL32(0, 0, 0, 128));
    }

    // Center widget background
    float widget_w = ui_px(380.0f);
    float anim_h = anim_radius(state_.size) * 2.0f + ui_px(40.0f);
    float text_h = ui_px(30.0f);
    float status_h = state_.status_text.empty() ? 0.0f : ui_px(20.0f);
    float steps_h = state_.steps.empty() ? 0.0f : (float)state_.steps.size() * ui_px(22.0f) + ui_px(8.0f);
    float bar_h = (state_.size == LoadingSize::FullScreen && state_.animation != LoadingAnimation::ProgressBar)
                  ? bar_width(state_.size) : 0.0f;
    float cancel_h = state_.cancellable ? ui_px(36.0f) : 0.0f;

    float total_h = anim_h + text_h + status_h + steps_h + bar_h + cancel_h + ui_px(40.0f);
    float widget_h = total_h;

    ImVec2 widget_center(vp_min.x + vp->Size.x * 0.5f, vp_min.y + vp->Size.y * 0.5f);
    ImVec2 w0(widget_center.x - widget_w * 0.5f, widget_center.y - widget_h * 0.5f);
    ImVec2 w1(widget_center.x + widget_w * 0.5f, widget_center.y + widget_h * 0.5f);

    // Widget background with rounded corners
    ImVec4 surface_bg = k.surface;
    surface_bg.w = 0.92f;
    dl->AddRectFilled(w0, w1, c32(surface_bg), ui_px(12.0f));
    ImVec4 border_col = k.border;
    border_col.w = 0.4f;
    dl->AddRect(w0, w1, c32(border_col), ui_px(12.0f), 0, ui_px(1.0f));
    dl->PushClipRect(w0, w1, true);

    // Layout
    float y = w0.y + ui_px(20.0f);
    float center_x = widget_center.x;

    // Animation
    float r = anim_radius(state_.size);
    ImVec2 anim_center(center_x, y + r + ui_px(10.0f));
    switch (state_.animation) {
        case LoadingAnimation::Spinner:         render_spinner(dl, anim_center, r, state_.elapsed); break;
        case LoadingAnimation::Pulse:           render_pulse(dl, anim_center, r, state_.elapsed); break;
        case LoadingAnimation::Wave:            render_wave(dl, anim_center, r, state_.elapsed); break;
        case LoadingAnimation::ProgressBar:     render_progress_bar(dl, anim_center, bar_width(state_.size), state_.elapsed); break;
        case LoadingAnimation::MinecraftBlocks: render_minecraft_blocks(dl, anim_center, r, state_.elapsed); break;
        case LoadingAnimation::DiamondGrid:     render_diamond_grid(dl, anim_center, r, state_.elapsed); break;
        case LoadingAnimation::Ripple:          render_ripple(dl, anim_center, r, state_.elapsed); break;
        case LoadingAnimation::TypingDots:      render_typing_dots(dl, anim_center, r, state_.elapsed); break;
        case LoadingAnimation::ParticleBurst:   render_particle_burst(dl, anim_center, r, state_.elapsed); break;
        case LoadingAnimation::GradientSweep:   render_gradient_sweep(dl, anim_center, r, state_.elapsed); break;
    }
    y += anim_h;

    // Title
    if (!state_.title.empty()) {
        ImVec4 tc = k.text;
        dl->AddText(f_bold, ui_px(16.0f), ImVec2(center_x - ImGui::CalcTextSize(state_.title.c_str()).x * 0.5f, y),
                    c32(tc), state_.title.c_str());
        y += text_h;
    }

    // Subtitle
    if (!state_.subtitle.empty()) {
        ImVec4 mc = k.muted;
        auto sz = ImGui::CalcTextSize(state_.subtitle.c_str());
        dl->AddText(f_body, ui_px(12.0f), ImVec2(center_x - sz.x * 0.5f, y),
                    c32(mc), state_.subtitle.c_str());
        y += ui_px(18.0f);
    }

    // Status text
    if (!state_.status_text.empty()) {
        ImVec4 mc = k.muted;
        auto sz = ImGui::CalcTextSize(state_.status_text.c_str());
        dl->AddText(f_small, ui_px(11.0f), ImVec2(center_x - sz.x * 0.5f, y),
                    c32(mc), state_.status_text.c_str());
        y += status_h;
    }

    // Progress bar (if fullscreen and not already animated)
    if (bar_h > 0.0f) {
        float bw = bar_width(state_.size);
        render_progress_bar(dl, ImVec2(center_x, y + ui_px(4.0f)), bw, state_.elapsed);
        y += bar_h;
    }

    // Steps list
    if (!state_.steps.empty()) {
        y += ui_px(4.0f);
        for (auto& step : state_.steps) {
            ImVec4 col;
            const char* icon;
            if (step.failed) {
                col = k.red;
                icon = "X";
            } else if (step.completed) {
                col = k.green;
                icon = "+";
            } else if (step.active) {
                col = k.brand;
                icon = ">";
            } else {
                col = k.muted;
                icon = "-";
            }
            char line[256];
            snprintf(line, sizeof(line), "%s %s", icon, step.label.c_str());
            dl->AddText(f_small, ui_px(11.0f), ImVec2(w0.x + ui_px(24.0f), y), c32(col), line);
            y += ui_px(22.0f);
        }
    }

    // Cancel button
    if (state_.cancellable) {
        y += ui_px(4.0f);
        float btn_w = ui_px(100.0f);
        float btn_h = ui_px(28.0f);
        ImVec2 btn0(center_x - btn_w * 0.5f, y);
        ImVec2 btn1(center_x + btn_w * 0.5f, y + btn_h);

        ImGuiIO& io = ImGui::GetIO();
        bool hovered = ImGui::IsMouseHoveringRect(btn0, btn1);

        ImVec4 btn_bg = hovered ? k.red : k.surface2;
        dl->AddRectFilled(btn0, btn1, c32(btn_bg), ui_px(6.0f));
        ImVec4 btn_tc = {1.0f, 1.0f, 1.0f, 1.0f};
        auto t_sz = ImGui::CalcTextSize("Cancel");
        dl->AddText(f_body, ui_px(12.0f),
                    ImVec2(center_x - t_sz.x * 0.5f, y + (btn_h - t_sz.y) * 0.5f),
                    c32(btn_tc), "Cancel");

        if (hovered && io.MouseClicked[0] && state_.on_cancel) {
            state_.on_cancel();
        }
    }

    dl->PopClipRect();
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Centered widget (non-fullscreen)
// ---------------------------------------------------------------------------

void LoadingScreen::render_centered_widget() {
    float r = anim_radius(state_.size);
    float widget_w = ui_px(260.0f);
    float anim_h = r * 2.0f + ui_px(16.0f);
    float text_h = ui_px(22.0f);
    float status_h = state_.status_text.empty() ? 0.0f : ui_px(18.0f);
    float bar_h = (state_.animation == LoadingAnimation::ProgressBar) ? ui_px(20.0f) : 0.0f;
    float total_h = anim_h + text_h + status_h + bar_h + ui_px(24.0f);

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 vp_min = vp->Pos;

    // Overlay window
    ImGui::SetNextWindowPos(vp_min);
    ImGui::SetNextWindowSize(vp->Size);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration
                           | ImGuiWindowFlags_NoBackground
                           | ImGuiWindowFlags_NoInputs
                           | ImGuiWindowFlags_NoFocusOnAppearing
                           | ImGuiWindowFlags_NoNav
                           | ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::Begin("##LoadingWidget", nullptr, flags);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Semi-transparent background covering full screen
    dl->AddRectFilled(vp_min, ImVec2(vp_min.x + vp->Size.x, vp_min.y + vp->Size.y),
                      IM_COL32(0, 0, 0, 120));

    // Widget box
    ImVec2 widget_center(vp_min.x + vp->Size.x * 0.5f, vp_min.y + vp->Size.y * 0.5f);
    ImVec2 w0(widget_center.x - widget_w * 0.5f, widget_center.y - total_h * 0.5f);
    ImVec2 w1(widget_center.x + widget_w * 0.5f, widget_center.y + total_h * 0.5f);

    ImVec4 surface_bg = k.surface;
    surface_bg.w = 0.94f;
    dl->AddRectFilled(w0, w1, c32(surface_bg), ui_px(10.0f));
    ImVec4 border_col = k.border;
    border_col.w = 0.3f;
    dl->AddRect(w0, w1, c32(border_col), ui_px(10.0f), 0, ui_px(1.0f));

    // Animation
    float y = w0.y + ui_px(12.0f);
    ImVec2 anim_center(widget_center.x, y + r + ui_px(4.0f));
    switch (state_.animation) {
        case LoadingAnimation::Spinner:         render_spinner(dl, anim_center, r, state_.elapsed); break;
        case LoadingAnimation::Pulse:           render_pulse(dl, anim_center, r, state_.elapsed); break;
        case LoadingAnimation::Wave:            render_wave(dl, anim_center, r, state_.elapsed); break;
        case LoadingAnimation::ProgressBar:     render_progress_bar(dl, anim_center, widget_w - ui_px(40.0f), state_.elapsed); break;
        case LoadingAnimation::MinecraftBlocks: render_minecraft_blocks(dl, anim_center, r, state_.elapsed); break;
        case LoadingAnimation::DiamondGrid:     render_diamond_grid(dl, anim_center, r, state_.elapsed); break;
        case LoadingAnimation::Ripple:          render_ripple(dl, anim_center, r, state_.elapsed); break;
        case LoadingAnimation::TypingDots:      render_typing_dots(dl, anim_center, r, state_.elapsed); break;
        case LoadingAnimation::ParticleBurst:   render_particle_burst(dl, anim_center, r, state_.elapsed); break;
        case LoadingAnimation::GradientSweep:   render_gradient_sweep(dl, anim_center, r, state_.elapsed); break;
    }
    y += anim_h;

    // Title
    if (!state_.title.empty()) {
        ImVec4 tc = k.text;
        auto sz = ImGui::CalcTextSize(state_.title.c_str());
        dl->AddText(f_bold, ui_px(14.0f), ImVec2(widget_center.x - sz.x * 0.5f, y), c32(tc), state_.title.c_str());
        y += text_h;
    }

    // Subtitle
    if (!state_.subtitle.empty()) {
        ImVec4 mc = k.muted;
        auto sz = ImGui::CalcTextSize(state_.subtitle.c_str());
        dl->AddText(f_body, ui_px(11.0f), ImVec2(widget_center.x - sz.x * 0.5f, y), c32(mc), state_.subtitle.c_str());
        y += ui_px(16.0f);
    }

    // Status
    if (!state_.status_text.empty()) {
        ImVec4 mc = k.muted;
        auto sz = ImGui::CalcTextSize(state_.status_text.c_str());
        dl->AddText(f_small, ui_px(10.0f), ImVec2(widget_center.x - sz.x * 0.5f, y), c32(mc), state_.status_text.c_str());
        y += status_h;
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Convenience functions
// ---------------------------------------------------------------------------

void show_loading(const std::string& title, const std::string& subtitle) {
    LoadingScreen::instance().show(LoadingAnimation::Spinner, LoadingSize::Medium, title, subtitle);
}

void hide_loading() {
    LoadingScreen::instance().hide();
}

void show_progress_loading(const std::string& title, int total_steps) {
    auto& ls = LoadingScreen::instance();
    ls.show(LoadingAnimation::ProgressBar, LoadingSize::Medium, title);
    std::vector<std::string> labels;
    for (int i = 0; i < total_steps; i++) labels.push_back("Step " + std::to_string(i + 1));
    ls.begin_steps(labels);
}

void advance_loading(const std::string& status) {
    LoadingScreen::instance().advance_step(status);
}

void fail_loading(const std::string& error) {
    LoadingScreen::instance().fail_step(error);
}

void show_splash_screen(const std::string& version) {
    auto& ls = LoadingScreen::instance();
    ls.show_fullscreen("AMALGAM LAUNCHER",
                       version.empty() ? "Your world. Your mods. Your adventure."
                                       : ("Your world. Your mods. Your adventure.  v" + version),
                       LoadingAnimation::GradientSweep);
    ls.begin_steps({"Load configuration", "Prepare content catalog",
                    "Check Minecraft runtime", "Ready for your next world"});
    ls.set_status("Preparing your next world...");
}

void hide_splash_screen() {
    LoadingScreen::instance().hide();
}

}  // namespace aml::ui
