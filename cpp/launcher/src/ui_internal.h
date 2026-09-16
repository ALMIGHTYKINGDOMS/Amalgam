#pragma once

// Internal shared declarations for the UI translation unit split.
// This header is included by ui.cpp, ui_helpers.cpp, ui_components.cpp,
// and ui_shell.cpp.  It is NOT part of the public API.

#include "ui.h"
#include "ui_state.h"
#include "ui_model.h"
#include "config.h"
#include "mods.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"

#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace aml::ui {

// ---------------------------------------------------------------------------
// Theme
// ---------------------------------------------------------------------------
struct Theme {
    ImVec4 bg;
    ImVec4 sidebar;
    ImVec4 surface;
    ImVec4 surface2;
    ImVec4 border;
    ImVec4 text;
    ImVec4 muted;
    ImVec4 brand;
    ImVec4 brand_hov;
    ImVec4 brand_dk;
    ImVec4 sel;
    ImVec4 hover;
    ImVec4 red;
    ImVec4 blue;
    ImVec4 orange;
    ImVec4 yellow;
    ImVec4 green;

    // Design tokens: spacing scale, radii, and type scale. Pages should use
    // these instead of ad-hoc ui_px() magic numbers so the rhythm is uniform.
    // All values are logical pixels and are scaled by g_ui_scale via ui_px().
    float space_1 = 4.0f;    // tightest gap
    float space_2 = 8.0f;    // between inline items
    float space_3 = 10.0f;   // default item gap
    float space_4 = 12.0f;   // card padding
    float space_5 = 18.0f;   // section gap
    float space_6 = 24.0f;   // page gap
    float radius_sm = 5.0f;  // badges, small chips
    float radius_md = 8.0f;  // cards, popups
    float radius_lg = 12.0f; // hero, large panels
};

extern Theme k;

// Scrollbar colours (defined in ui.cpp). A grab tinted from the panel surface
// is invisible on these near-black backgrounds, which hides the fact that a
// page or dialog continues below the fold, so the grab is tinted from the
// readable muted text colour instead. Called by every theme applier.
void apply_scrollbar_style();

// Typography helper (defined in ui_components.cpp). Use this everywhere a
// page header is drawn so hierarchy stays consistent.
void draw_page_header(const char* title, const char* subtitle = nullptr);

// ---------------------------------------------------------------------------
// Vector icon system (defined in ui_components.cpp)
//
// One consistent vector icon vocabulary drawn with the ImGui draw list, so
// pages share a single visual language instead of mixing unicode glyphs with
// hand-drawn nav icons. Icons are drawn at `center` with a radius of `r`
// logical px (already ui_px-scaled by the caller or via icon helpers).
// ---------------------------------------------------------------------------
enum class IconId {
    Play,        // triangle play
    Pause,       // two bars
    Stop,        // square
    Restart,     // circular arrow
    Open,        // external / folder-open
    Folder,      // folder
    Trash,       // trash can
    Refresh,     // circular arrows
    Check,       // checkmark
    Close,       // X
    Search,      // magnifier
    Download,    // down arrow into tray
    Upload,      // up arrow out of tray
    Pin,         // pin/favorite
    Heart,       // favorite heart
    Users,       // people
    User,        // single person
    Server,      // server stack
    Shield,      // shield
    Edit,        // pencil
    Duplicate,   // two overlapping squares
    Export,      // arrow out of box
    Import,      // arrow into box
    Settings,    // gear
    Bell,        // notification bell
    Info,        // info circle
    Warning,     // warning triangle
    Error,       // error circle
    Globe,       // globe / world
    Cube,        // cube / mod
    Image,       // image / screenshot
    Log,         // console / terminal
    File,        // document / file
    Cloud,       // cloud
    Lock,        // lock
    Star,        // star
    Plus,        // plus
    ChevronRight,// chevron right
    ChevronDown, // chevron down
    More,        // ellipsis
    Link,        // link
    Copy,        // copy
    Wrench,      // wrench
    Rocket,      // rocket / deploy
    Power,       // power / on-off
    Book,        // book / library
    Gift,        // gift
    Sparkle,     // sparkle / premium
};

// Draws a vector icon centered at `center` with the given radius (logical px,
// caller should ui_px() it). Color is used directly.
void draw_icon(IconId icon, const ImVec2& center, float radius, ImU32 color);

// Convenience: draws icon at the current cursor position (top-left aligned to
// the given size) and advances the cursor by the size. Returns true if clicked.
bool icon_button(IconId icon, const ImVec2& size, const char* tooltip = nullptr,
                 const ImVec4& color = ImVec4(-1,-1,-1,-1), bool disabled = false);
extern ImFont* f_body;
extern ImFont* f_bold;
extern ImFont* f_title;
extern ImFont* f_h2;
extern ImFont* f_small;
extern ImFont* f_mono;
extern float g_ui_scale;

inline float ui_px(float value) { return value * g_ui_scale; }
inline ImU32 c32(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }

// ---------------------------------------------------------------------------
// Formatting helpers (defined in ui_helpers.cpp)
// ---------------------------------------------------------------------------
std::string format_bytes(uint64_t bytes);
std::string format_download_count(int64_t downloads);
std::string format_rate(double bytes_per_second);
std::string format_eta(double seconds);
std::string format_elapsed(uint64_t start, uint64_t end = 0);
const char* type_text(const std::string& type);
ImVec4 type_color(const std::string& type);
size_t next_utf8_boundary(const std::string& text, size_t offset);
std::string elide_to_width(const std::string& text, float max_width);
std::string compact_provider_text(const std::string& source, size_t maximum_bytes = 0);
std::string humanize_error(const std::string& source);
std::string project_source_text(const mods::SearchResult& project, const mods::ModInfo& info);

// ---------------------------------------------------------------------------
// Component helpers (defined in ui_components.cpp)
// ---------------------------------------------------------------------------
float auto_item_width(float preferred, float minimum = 0.0f, float reserved = 0.0f);
bool input_text(const char* label, std::string* value);
bool input_text_hint(const char* label, const char* hint, std::string* value);
// Draws an empty state, optionally with one action button. Returns true on the
// frame that action was pressed, so the caller wires it to real behaviour.
bool empty_state(const char* title, const char* message, const char* icon = nullptr,
                 const char* action_label = nullptr);
// Dear ImGui resolves a popup id against the id stack of the window that opens
// it, and every card body is a child window. A control drawn inside a card
// therefore cannot open a modal its page begins by name. Such controls call
// request_popup(); the page's own window drains it with open_requested_popup()
// once per frame, so the id matches the BeginPopupModal the page runs.
void request_popup(const char* label);
void open_requested_popup();
// Premium empty state with a layered geometric brand illustration, consistent
// title/message/CTA. Used everywhere so no page is left as blank space.
void illustrated_empty_state(IconId icon, const char* title, const char* message,
                             const char* action_label = nullptr,
                             void (*on_action)(UiState&) = nullptr,
                             UiState* action_state = nullptr);
bool input_secret(const char* label, std::string* value);
bool show_open_addon(UiState& st, std::string& target);
void draw_badge(ImDrawList* dl, const ImVec2& pos, const char* text,
                 const ImVec4& fg, const ImVec4& bg);
void draw_badge_vert(ImDrawList* dl, const ImVec2& pos, const char* text,
                 const ImVec4& fg, const ImVec4& bg);
void draw_local_image(UiState& st, const std::wstring& path, const ImVec2& pos,
                      const ImVec2& size, ImU32 fallback,
                      ui_model::ImageFit fit = ui_model::ImageFit::Cover);
void draw_project_image(UiState& st, const std::string& url, const ImVec2& pos,
                        const ImVec2& size, ImU32 fallback);
void draw_brand_mark(ImDrawList* dl, const ImVec2& center, float scale);
void draw_page_emblem(UiState& st, const char* asset_name);
std::string format_date(int64_t timestamp);
void page_title(const char* title, const char* subtitle = nullptr);
bool primary_button(const char* label, const ImVec2& size = ImVec2(0, 0), bool loading = false, bool disabled = false);
bool ghost_button(const char* label, const ImVec2& size = ImVec2(0, 0), bool disabled = false);
void card_begin(const char* id, const ImVec2& size = ImVec2(0, 0), bool hoverable = false);
void card_end(bool was_hoverable = false);
void progress_bar(float progress, const ImVec2& size = ImVec2(0, 0),
                  const char* overlay_text = nullptr, const ImVec4* color = nullptr);
void draw_breadcrumbs(const std::vector<std::string>& crumbs);
void draw_health_badge(ImDrawList* dl, const ImVec2& pos, const char* status, const ImVec4& color);
bool quick_search_dialog(std::string* selected_result);
void open_quick_search();
bool g_quick_search_open();
std::string g_quick_search_query();
void g_quick_search_set_results(const std::vector<std::string>& results);
void draw_operation_progress(const UiState::DownloadJob& job, const ImVec4* color = nullptr);
bool settings_search_filter(const char* search_query, const char* setting_name, const char* setting_description);
void draw_keyboard_shortcut(const char* shortcut, const char* description);
bool draw_theme_toggle();
bool is_dark_mode();
void set_dark_mode(bool dark);
void draw_tooltip(const char* text);
void draw_status_indicator(const ImVec4& color, const char* label);
void draw_loading_spinner(float size);
bool draw_context_menu(int* selected_id);
void open_context_menu(const ImVec2& position, const std::vector<std::pair<std::string, int>>& items);
void draw_sidebar_item(const char* label, const char* icon, bool active, bool available, const char* tooltip);
void draw_profile_card_enhanced(const char* name, const char* loader, const char* version, 
                                const char* last_played, int mod_count, bool has_update,
                                const ImVec4& health_color, const char* health_status);
void draw_sidebar_compact_item(const char* icon, bool active, const char* tooltip);
void show_toast(const char* title, const char* message, const ImVec4& color, float duration);
void draw_toasts();
void draw_search_highlight(const char* text, const char* search_query);

// ---------------------------------------------------------------------------
// Server card component (defined in ui_components.cpp)
// ---------------------------------------------------------------------------
void draw_server_card(const char* name, const char* address, const char* software,
                      const char* version, int players, int max_players,
                      const ImVec4& status_color, const char* status_text,
                      bool selected, float width);

// ---------------------------------------------------------------------------
// Confirmation dialog (defined in ui_components.cpp)
// ---------------------------------------------------------------------------
bool draw_confirmation_dialog(const char* title, const char* message,
                               const char* confirm_label, const char* cancel_label,
                               const ImVec4& confirm_color, bool* open);

// ---------------------------------------------------------------------------
// Error state (defined in ui_components.cpp)
// ---------------------------------------------------------------------------
void draw_error_state(const char* title, const char* message,
                      const char* retry_label = nullptr, bool* retry_flag = nullptr);

// ---------------------------------------------------------------------------
// Auth Wizard (defined in auth_wizard.cpp)
// ---------------------------------------------------------------------------
void show_auth_wizard_if_needed(UiState& st);
void draw_auth_wizard(UiState& st);
void draw_amalgam_login_wizard(UiState& st);
void draw_password_reset_dialog(UiState& st);
void draw_account_button_with_wizard(UiState& st);

// ---------------------------------------------------------------------------
// Account UI (defined in account_ui.cpp)
// ---------------------------------------------------------------------------
void draw_account_page(UiState& st);
void draw_account_button_enhanced(UiState& st);
void draw_account_switcher(UiState& st);

// ---------------------------------------------------------------------------
// Bedrock UI (defined in bedrock_ui.cpp)
// ---------------------------------------------------------------------------
void draw_bedrock_tab(UiState& st);
void draw_bedrock_overview(UiState& st);
void draw_bedrock_profiles(UiState& st);
void draw_bedrock_worlds(UiState& st);
void draw_bedrock_backups(UiState& st);
void draw_bedrock_addons(UiState& st);

// ---------------------------------------------------------------------------
// Admin UI (defined in admin_ui.cpp)
// ---------------------------------------------------------------------------
void draw_admin_page(UiState& st);
void draw_admin_login(UiState& st);

// ---------------------------------------------------------------------------
// Server V2 UI (defined in server_ui.cpp)
// ---------------------------------------------------------------------------
void draw_server_manager(UiState& st);
void set_fixture_server_mode(int mode);
void set_fixture_server_detail(int server_index, int tab);

// ---------------------------------------------------------------------------
// Cloud Hosting UI (defined in cloud_ui.cpp)
// ---------------------------------------------------------------------------
void draw_cloud_page(UiState& st);

// ---------------------------------------------------------------------------
// Mod Manager UI (defined in mod_manager_ui.cpp)
// ---------------------------------------------------------------------------
void draw_mod_manager_page(UiState& st);

// ---------------------------------------------------------------------------
// Performance UI (defined in performance_ui.cpp)
// ---------------------------------------------------------------------------
void draw_performance_page(UiState& st);

// ---------------------------------------------------------------------------
// Theme UI (defined in theme_ui.cpp)
// ---------------------------------------------------------------------------
void draw_theme_page(UiState& st);
void draw_theme_selector_inline(UiState& st, config::Config& c);

// ---------------------------------------------------------------------------
// Window / layout (defined in ui_shell.cpp)
// ---------------------------------------------------------------------------
void enable_per_monitor_dpi_awareness();
float dpi_scale(UINT dpi);
float initial_dpi_scale();
float visible_window_width(HWND hwnd);
void set_next_adaptive_window(float preferred_width, float preferred_height,
                              float minimum_width = 320.0f, float minimum_height = 0.0f,
                              ImGuiCond condition = ImGuiCond_Appearing);
void set_next_adaptive_right_panel(float preferred_width, float preferred_height,
                                   float minimum_width = 280.0f);

// ---------------------------------------------------------------------------
// Navigation (defined in ui.cpp)
// ---------------------------------------------------------------------------
extern UiState* app;
void navigate_to(UiState& st, int sidebar_item, int tab, int provider_tab = -1);
void nav_indicator_reset();

// ---------------------------------------------------------------------------
// Worker threads (defined in ui.cpp)
// ---------------------------------------------------------------------------
void spawn_worker(UiState& st, std::thread worker);
void join_workers(UiState& st);

// ---------------------------------------------------------------------------
// Authentication (defined in ui.cpp)
// ---------------------------------------------------------------------------
void start_microsoft_login(UiState& st);
void draw_microsoft_login_dialog(UiState& st);
bool has_linked_account(UiState& st);
std::string player_display_name(UiState& st);

// ---------------------------------------------------------------------------
// Notices (defined in ui.cpp)
// ---------------------------------------------------------------------------
void push_notice(UiState& st, ui_model::NoticeLevel level, std::string title,
                 std::string message = {}, std::string action_label = {},
                 std::string action_id = {}, bool sticky = false);
bool save_ui_config(UiState& st);

// ---------------------------------------------------------------------------
// V3 Design System components (defined in ui_components.cpp)
// ---------------------------------------------------------------------------
void draw_skeleton_rect(const ImVec2& pos, const ImVec2& size, float rounding = 0.0f);
void draw_skeleton_circle(const ImVec2& center, float radius);
void draw_skeleton_text(const ImVec2& pos, float width, float height = 0.0f);
void draw_section_header(const char* title, const char* subtitle = nullptr);
void draw_status_dot(const ImVec2& center, const ImVec4& color, float radius = 0.0f);
void draw_divider(float padding = 0.0f);
void draw_meta_line(const char* label, const char* value);
void draw_meta_line_colored(const char* label, const char* value, const ImVec4& value_color);

// Stat card: metric label + large value + optional progress bar + optional icon.
// Returns the total height drawn so callers can align cards.
float draw_stat_card(const char* label, const char* value, float progress = -1.0f,
                     const ImVec4& accent = ImVec4(-1,-1,-1,-1),
                     float width = 0.0f);

// Circular progress indicator drawn at pos with given radius.
void draw_circle_progress(const ImVec2& center, float radius, float progress,
                          const ImVec4& track_color, const ImVec4& fill_color);

}  // namespace aml::ui
