#pragma once

#include <string>
#include <vector>
#include <mutex>

namespace aml::client {

struct HudModule {
    std::string id;
    std::string name;
    bool enabled = false;
    float x = 0.0f;
    float y = 0.0f;
    float scale = 1.0f;
    float opacity = 1.0f;
    bool background = false;
    float text_size = 1.0f;
    int alignment = 0;  // 0=left, 1=center, 2=right
};

class HudManager {
public:
    static HudManager& instance();

    void init();
    void set_config_dir(const std::string& dir);
    void render_hud();
    void render_editor();

    std::vector<HudModule> get_modules() const;
    void set_module_enabled(const std::string& id, bool enabled);
    void set_module_position(const std::string& id, float x, float y);
    void set_module_scale(const std::string& id, float scale);

    void save_layout(const std::string& name);
    void load_layout(const std::string& name);
    void reset_layout();
    std::vector<std::string> get_presets() const;

    void apply_preset(const std::string& name);
    void set_all_disabled();

    bool is_editor_active() const { return editor_active_; }
    void set_editor_active(bool active) { editor_active_ = active; }

    bool is_dragging() const { return dragging_; }
    bool is_preview_mode() const { return preview_mode_; }
    void set_preview_mode(bool on) { preview_mode_ = on; }

private:
    HudManager() = default;

    void render_editor_canvas();
    void render_editor_module_list();

    mutable std::mutex mu_;
    std::vector<HudModule> modules_;
    bool editor_active_ = false;
    bool preview_mode_ = false;
    bool dragging_ = false;
    int drag_index_ = -1;
    std::string current_layout_ = "default";
    std::string config_dir_;
};

}  // namespace aml::client
