#pragma once

#include "client/client_core.h"
#include "imgui.h"

namespace aml::client {

// ---------------------------------------------------------------------------
// Client UI — Main menu framework
// ---------------------------------------------------------------------------

class ClientUI {
public:
    static ClientUI& instance();

    void init();
    void shutdown();

    void render(float dt);

    bool is_menu_open() const { return menu_open_; }
    void toggle_menu();
    void open_menu();
    void close_menu();
    void toggle_quick_menu();
    bool is_quick_menu_open() const { return quick_menu_open_; }

    ClientTab active_tab() const { return active_tab_; }
    void set_active_tab(ClientTab tab);

private:
    ClientUI() = default;

    void render_dashboard();
    void render_modules_page();
    void render_mods_page();
    void render_hud_page();
    void render_profiles_page();
    void render_cosmetics_page();
    void render_essentials_page();
    void render_performance_page();
    void render_servers_page();
    void render_screenshots_page();
    void render_settings_page();
    void render_diagnostics_page();
    void render_startup_splash(float dt);

    bool menu_open_ = false;
    bool quick_menu_open_ = false;
    ClientTab active_tab_ = ClientTab::Dashboard;
    bool startup_splash_visible_ = false;
    float startup_splash_elapsed_ = 0.0f;
};

// ---------------------------------------------------------------------------
// Utility rendering helpers
// ---------------------------------------------------------------------------

bool client_primary_button(const char* label, const ImVec2& size = ImVec2(0, 0));
bool client_secondary_button(const char* label, const ImVec2& size = ImVec2(0, 0));
bool client_success_button(const char* label, const ImVec2& size = ImVec2(0, 0));
bool client_warning_button(const char* label, const ImVec2& size = ImVec2(0, 0));
bool client_error_button(const char* label, const ImVec2& size = ImVec2(0, 0));
void client_text(const char* fmt, ...);
void client_text_disabled(const char* fmt, ...);
void client_separator();
bool client_card_begin(const char* title);
void client_card_end();

}  // namespace aml::client
