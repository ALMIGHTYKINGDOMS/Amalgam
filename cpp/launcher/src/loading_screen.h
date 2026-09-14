#pragma once

#include <string>
#include <vector>
#include <functional>
#include <cstdint>
#include <cmath>

// Forward declare ImGui types
struct ImVec2;
struct ImVec4;
struct ImDrawList;

namespace aml::ui {

// ---------------------------------------------------------------------------
// Loading Screen Types
// ---------------------------------------------------------------------------

enum class LoadingAnimation {
    Spinner,        // Rotating circle of dots
    Pulse,          // Pulsing circle
    Wave,           // Horizontal wave bars
    ProgressBar,    // Animated progress bar
    MinecraftBlocks, // Floating Minecraft-style blocks
    DiamondGrid,    // Rotating diamond grid
    Ripple,         // Expanding ripple circles
    TypingDots,     // Three dots typing animation
    ParticleBurst,  // Particle explosion effect
    GradientSweep,  // Rotating gradient sweep
};

enum class LoadingSize {
    Small,   // 48px
    Medium,  // 80px
    Large,   // 120px
    FullScreen, // Fills screen
};

struct LoadingScreenState {
    bool visible = false;
    float progress = -1.0f;  // -1 = indeterminate
    std::string title;
    std::string subtitle;
    std::string status_text;
    LoadingAnimation animation = LoadingAnimation::Spinner;
    LoadingSize size = LoadingSize::Medium;
    float elapsed = 0.0f;
    float animation_speed = 1.0f;
    bool cancellable = false;
    std::function<void()> on_cancel;

    // Step-based progress
    struct Step {
        std::string label;
        bool completed = false;
        bool active = false;
        bool failed = false;
    };
    std::vector<Step> steps;
    int current_step = -1;
};

// ---------------------------------------------------------------------------
// Loading Screen Manager
// ---------------------------------------------------------------------------

class LoadingScreen {
public:
    static LoadingScreen& instance();

    // Show/hide
    void show(LoadingAnimation anim = LoadingAnimation::Spinner,
              LoadingSize size = LoadingSize::Medium,
              const std::string& title = "",
              const std::string& subtitle = "");

    void show_fullscreen(const std::string& title, const std::string& subtitle = "",
                         LoadingAnimation anim = LoadingAnimation::MinecraftBlocks);

    void hide();
    bool is_visible() const;

    // Progress
    void set_progress(float progress);  // 0.0 - 1.0
    void set_status(const std::string& status);
    void set_title(const std::string& title);
    void set_subtitle(const std::string& subtitle);

    // Steps
    void begin_steps(const std::vector<std::string>& step_labels);
    void advance_step(const std::string& status = "");
    void fail_step(const std::string& error = "");
    void complete_all_steps();

    // Configuration
    void set_animation(LoadingAnimation anim);
    void set_size(LoadingSize size);
    void set_animation_speed(float speed);
    void set_cancellable(bool cancellable, std::function<void()> on_cancel = nullptr);

    // Render (call every frame)
    void render(float delta_time);

    // Utility
    static const char* animation_name(LoadingAnimation anim);

private:
    LoadingScreen();
    ~LoadingScreen() = default;
    LoadingScreen(const LoadingScreen&) = delete;
    LoadingScreen& operator=(const LoadingScreen&) = delete;

    // Animation renderers
    void render_spinner(ImDrawList* dl, const ImVec2& center, float radius, float t);
    void render_pulse(ImDrawList* dl, const ImVec2& center, float radius, float t);
    void render_wave(ImDrawList* dl, const ImVec2& center, float radius, float t);
    void render_progress_bar(ImDrawList* dl, const ImVec2& center, float width, float t);
    void render_minecraft_blocks(ImDrawList* dl, const ImVec2& center, float radius, float t);
    void render_diamond_grid(ImDrawList* dl, const ImVec2& center, float radius, float t);
    void render_ripple(ImDrawList* dl, const ImVec2& center, float radius, float t);
    void render_typing_dots(ImDrawList* dl, const ImVec2& center, float radius, float t);
    void render_particle_burst(ImDrawList* dl, const ImVec2& center, float radius, float t);
    void render_gradient_sweep(ImDrawList* dl, const ImVec2& center, float radius, float t);

    // Fullscreen overlay
    void render_fullscreen_overlay();
    void render_centered_widget();

    LoadingScreenState state_;
};

// ---------------------------------------------------------------------------
// Convenience Functions
// ---------------------------------------------------------------------------

// Quick show/hide
void show_loading(const std::string& title, const std::string& subtitle = "");
void hide_loading();

// Progress-tracked loading
void show_progress_loading(const std::string& title, int total_steps);
void advance_loading(const std::string& status = "");
void fail_loading(const std::string& error = "");

// Fullscreen splash (for app startup)
void show_splash_screen(const std::string& version = "");
void hide_splash_screen();

}  // namespace aml::ui
