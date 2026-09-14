#pragma once

#include "imgui.h"

#include <string>
#include <vector>
#include <functional>
#include <cstdint>
#include <mutex>

namespace aml::client {

// ---------------------------------------------------------------------------
// Theme
// ---------------------------------------------------------------------------

struct Theme {
    // Mirrors the launcher palette so the in-game client feels like the same
    // premium product instead of a separate overlay skinned with generic gray.
    ImVec4 bg{0.018f, 0.027f, 0.050f, 0.97f};
    ImVec4 panel{0.038f, 0.058f, 0.094f, 0.98f};
    ImVec4 border{0.120f, 0.170f, 0.250f, 0.95f};
    ImVec4 text{0.93f, 0.95f, 0.99f, 1.0f};
    ImVec4 muted{0.55f, 0.64f, 0.76f, 1.0f};
    ImVec4 accent{0.50f, 0.22f, 0.92f, 1.0f};
    ImVec4 accent_hover{0.72f, 0.43f, 1.0f, 1.0f};
    ImVec4 accent_dark{0.25f, 0.08f, 0.54f, 1.0f};
    ImVec4 success{0.20f, 0.80f, 0.40f, 1.0f};
    ImVec4 warning{0.95f, 0.70f, 0.15f, 1.0f};
    ImVec4 error{0.90f, 0.20f, 0.20f, 1.0f};
    ImVec4 header_bg{0.028f, 0.044f, 0.076f, 0.98f};
    ImVec4 tab_active{0.50f, 0.22f, 0.92f, 1.0f};
    ImVec4 tab_inactive{0.070f, 0.105f, 0.165f, 0.82f};
};

Theme& theme();

// ---------------------------------------------------------------------------
// Client Tab IDs
// ---------------------------------------------------------------------------

enum class ClientTab {
    Dashboard = 0,
    Modules,
    HUD,
    Profiles,
    Cosmetics,
    Social,
    Servers,
    Performance,
    Settings,
    Diagnostics,
    TabCount
};

// Module categories matching the reference design
enum class ModuleCategory {
    Combat,
    Movement,
    Player,
    World,
    Render,
    Misc,
    Favorites,
    All,
    CategoryCount
};

const char* module_category_name(ModuleCategory cat);
ModuleCategory module_category_from_index(int index);

const char* client_tab_name(ClientTab tab);
int client_tab_index(ClientTab tab);
ClientTab client_tab_from_index(int index);

// Module category helpers
std::vector<std::string> get_modules_by_category(ModuleCategory cat);
bool is_module_favorited(const std::string& module_name);
void toggle_module_favorite(const std::string& module_name);

// ---------------------------------------------------------------------------
// Client Settings
// ---------------------------------------------------------------------------

struct ClientSettings {
    int menu_key = 0x2D;  // VK_INSERT
    float ui_scale = 1.0f;
    bool animations = true;
    bool notifications_enabled = true;
    int theme_index = 0;  // 0 = dark purple
    bool hud_editor_mode = false;
    bool show_client_version = true;
    bool auto_start = false;
    bool launcher_connection = true;
    bool developer_mode = false;
    // Extended settings matching reference
    std::string language = "English";
    bool show_fps = true;
    bool compact_mode = false;
    bool auto_update = true;
    int notifications_position = 0;  // 0 = top-right, 1 = bottom-right, 2 = top-left
    bool privacy_show_online = true;
    bool privacy_show_server = true;
    int quick_menu_style = 0;  // 0 = sidebar, 1 = radial
};

// Cosmetics system
namespace cosmetics {
struct CosmeticItem {
    std::string id;
    std::string name;
    std::string description;
    std::string category;  // "cape", "badge", "nameplate", "emote"
    bool owned = false;
    bool equipped = false;
    int rarity = 0;  // 0 = common, 1 = rare, 2 = epic, 3 = legendary
};

struct CosmeticsState {
    std::vector<CosmeticItem> items;
    int selected_category = 0;  // 0 = capes, 1 = badges, 2 = nameplates, 3 = emotes
    std::string selected_item;
    bool inventory_open = false;
};

CosmeticsState& cosmetics_state();
std::vector<CosmeticItem> get_cosmetics_by_category(const std::string& category);
void equip_cosmetic(const std::string& item_id);
void unequip_cosmetic(const std::string& item_id);
}  // namespace cosmetics

ClientSettings& settings();
bool load_client_settings(const std::string& path);
bool save_client_settings(const std::string& path);

// ---------------------------------------------------------------------------
// Profile Info (synced from launcher)
// ---------------------------------------------------------------------------

struct ProfileInfo {
    std::string profile_id;
    std::string profile_name;
    std::string mc_version;
    std::string loader;
    std::string loader_version;
    int mod_count = 0;
    bool connected = false;
    std::string launcher_version;
    int64_t session_start = 0;
    std::string session_status;  // "Active", "In Menu", "Idle"
};

ProfileInfo& profile();

// ---------------------------------------------------------------------------
// Mod Info (scanned from instance mods/ directory)
// ---------------------------------------------------------------------------

struct ModInfo {
    std::string filename;
    std::string name;
    std::string version;
    bool enabled = true;
    bool has_update = false;
};

std::vector<ModInfo>& mods();
void scan_mods(const std::string& mods_dir);
bool set_mod_enabled(const std::string& filename, bool enabled);
void refresh_client_data();
void refresh_server_status();

// ---------------------------------------------------------------------------
// Server Entry (from launcher server list)
// ---------------------------------------------------------------------------

struct ServerEntry {
    std::string name;
    std::string address;
    int port = 25565;
    bool online = false;
    bool status_checked = false;
    int ping_ms = -1;
};

std::vector<ServerEntry>& servers();
void load_servers(const std::string& config_dir);

// ---------------------------------------------------------------------------
// Friend Entry (from launcher essentials)
// ---------------------------------------------------------------------------

struct FriendEntry {
    std::string name;
    std::string status;
    std::string uuid;
};

std::vector<FriendEntry>& friends();
void load_friends(const std::string& config_dir);
void load_cosmetics(const std::string& config_dir);

// ---------------------------------------------------------------------------
// Performance Metrics
// ---------------------------------------------------------------------------

struct PerfMetrics {
    float fps = 0.0f;
    float fps_1pct_low = 0.0f;
    float frame_time_ms = 0.0f;
    float ram_mb = 0.0f;
    float ram_max_mb = 0.0f;
    float cpu_percent = 0.0f;
    int ping_ms = -1;
    float tps = -1.0f;
    // Extended metrics
    float gpu_percent = 0.0f;
    float download_speed = 0.0f;
    float upload_speed = 0.0f;
    int render_distance = 12;
    std::string server_address;
    std::string server_name;
    int player_count = 0;
    int max_players = 0;
    int64_t session_start_time = 0;
};

// Profile sync status
struct ProfileSyncStatus {
    bool synced = false;
    std::string last_sync_time;
    bool modules_synced = false;
    bool hud_synced = false;
    bool keybinds_synced = false;
    bool cosmetics_synced = false;
    bool settings_synced = false;
    bool server_list_synced = false;
    bool friends_synced = false;
    std::string sync_error;
};

ProfileSyncStatus& sync_status();

PerfMetrics& perf();
int clicks_per_second();

// ---------------------------------------------------------------------------
// Core Init/Shutdown
// ---------------------------------------------------------------------------

bool client_init(const std::string& config_dir, const std::string& game_dir = {});
void client_shutdown();
void client_tick(float dt);

// ---------------------------------------------------------------------------
// IPC Token (for launcher bridge)
// ---------------------------------------------------------------------------

std::string ipc_token();
void set_ipc_token(const std::string& token);

std::string config_dir();
std::string game_dir();

}  // namespace aml::client
