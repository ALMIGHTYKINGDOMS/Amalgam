#include "client/client_core.h"
#include "client/client_hud2.h"
#include "core/player_stats.h"
#include "imgui.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <random>
#include <chrono>
#include <ctime>
#include <algorithm>
#include <deque>
#include <cctype>
#include <cstdlib>
#include <set>
#include <stdexcept>
#include <thread>
#include <mutex>
#include <atomic>

namespace aml::client {

namespace {

Theme g_theme;
ClientSettings g_settings;
ProfileInfo g_profile;
ProfileSyncStatus g_sync_status;
PerfMetrics g_perf;
std::string g_config_dir;
std::string g_game_dir;
std::string g_ipc_token;
std::once_flag g_perf_init;
double g_perf_accum = 0.0;
int g_perf_frame_count = 0;
std::vector<ModInfo> g_mods;
std::vector<ServerEntry> g_servers;
std::vector<FriendEntry> g_friends;
std::string g_mods_dir;
struct ServerProbeResult {
    std::string address;
    bool online = false;
    int ping_ms = -1;
};
std::mutex g_server_probe_mutex;
std::vector<ServerProbeResult> g_pending_server_probe_results;
bool g_server_probe_ready = false;
std::atomic_bool g_server_probe_running{false};
std::thread g_server_probe_thread;
std::once_flag g_winsock_init;
std::deque<uint64_t> g_left_clicks;
bool g_left_button_was_down = false;

// FPS history for 1% low calculation
std::vector<float> g_fps_history;
static constexpr size_t kFpsHistorySize = 120;

// CPU measurement state
ULARGE_INTEGER g_prev_idle = {};
ULARGE_INTEGER g_prev_kernel = {};
ULARGE_INTEGER g_prev_user = {};
bool g_cpu_initialized = false;

// Simple JSON-like key=value config
std::string cfg_path(const std::string& name) {
    return g_config_dir + "\\" + name;
}

void write_kv(std::ofstream& f, const std::string& key, const std::string& val) {
    f << key << "=" << val << "\n";
}

void write_kv(std::ofstream& f, const std::string& key, int val) {
    f << key << "=" << std::to_string(val) << "\n";
}

void write_kv(std::ofstream& f, const std::string& key, float val) {
    f << key << "=" << std::to_string(val) << "\n";
}

void write_kv(std::ofstream& f, const std::string& key, bool val) {
    f << key << "=" << (val ? "1" : "0") << "\n";
}

bool read_kv(const std::string& line, std::string& key, std::string& value) {
    size_t eq = line.find('=');
    if (eq == std::string::npos) return false;
    key = line.substr(0, eq);
    value = line.substr(eq + 1);
    return true;
}

std::string local_sync_time() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    char text[32]{};
    std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &local);
    return text;
}

bool load_shared_profile() {
    std::ifstream file(cfg_path("shared_profile.txt"));
    if (!file.is_open()) return false;

    ProfileInfo next;
    next.session_start = g_profile.session_start;
    next.session_status = g_profile.session_status;
    next.connected = g_profile.connected;
    std::string line;
    std::string key;
    std::string value;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#' || !read_kv(line, key, value)) continue;
        if (key == "profile_id") next.profile_id = value;
        else if (key == "profile_name") next.profile_name = value;
        else if (key == "minecraft_version") next.mc_version = value;
        else if (key == "loader") next.loader = value;
        else if (key == "loader_version") next.loader_version = value;
        else if (key == "launcher_version") next.launcher_version = value;
        else if (key == "instance_directory" && g_game_dir.empty()) g_game_dir = value;
        else if (key == "mod_count") {
            try { next.mod_count = std::max(0, std::stoi(value)); }
            catch (const std::exception&) { next.mod_count = 0; }
        }
    }
    g_profile = std::move(next);
    return !g_profile.profile_id.empty() && !g_profile.profile_name.empty();
}

struct EndpointParts {
    std::string host;
    std::string port = "25565";
};

bool split_endpoint(const std::string& endpoint, EndpointParts& out) {
    std::string value = endpoint;
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
    size_t first = 0;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first]))) ++first;
    value = value.substr(first);
    if (value.empty()) return false;

    if (value.front() == '[') {
        const size_t close = value.find(']');
        if (close == std::string::npos || close <= 1) return false;
        out.host = value.substr(1, close - 1);
        if (close + 1 < value.size()) {
            if (value[close + 1] != ':') return false;
            out.port = value.substr(close + 2);
        }
    } else {
        const size_t colon = value.rfind(':');
        if (colon != std::string::npos && value.find(':') == colon) {
            out.host = value.substr(0, colon);
            out.port = value.substr(colon + 1);
        } else {
            out.host = value;
        }
    }

    if (out.host.empty() || out.port.empty()) return false;
    for (unsigned char c : out.port)
        if (!std::isdigit(c)) return false;
    const long port = std::strtol(out.port.c_str(), nullptr, 10);
    return port > 0 && port <= 65535;
}

bool probe_server_endpoint(const std::string& endpoint, int& ping_ms) {
    EndpointParts parts;
    if (!split_endpoint(endpoint, parts)) return false;
    std::call_once(g_winsock_init, [] {
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
    });

    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_family = AF_UNSPEC;
    addrinfo* addresses = nullptr;
    if (getaddrinfo(parts.host.c_str(), parts.port.c_str(), &hints, &addresses) != 0)
        return false;

    bool connected = false;
    int best_ping = -1;
    for (addrinfo* address = addresses; address != nullptr && !connected; address = address->ai_next) {
        SOCKET socket_handle = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (socket_handle == INVALID_SOCKET) continue;
        u_long non_blocking = 1;
        ioctlsocket(socket_handle, FIONBIO, &non_blocking);
        const auto started = std::chrono::steady_clock::now();
        int result = connect(socket_handle, address->ai_addr, static_cast<int>(address->ai_addrlen));
        if (result == SOCKET_ERROR) {
            const int error = WSAGetLastError();
            if (error == WSAEWOULDBLOCK || error == WSAEINPROGRESS || error == WSAEINVAL) {
                fd_set writable;
                fd_set exceptional;
                FD_ZERO(&writable);
                FD_ZERO(&exceptional);
                FD_SET(socket_handle, &writable);
                FD_SET(socket_handle, &exceptional);
                timeval timeout{0, 300000};
                result = select(0, nullptr, &writable, &exceptional, &timeout);
                if (result > 0 && FD_ISSET(socket_handle, &writable) && !FD_ISSET(socket_handle, &exceptional)) {
                    int socket_error = 0;
                    int socket_error_size = sizeof(socket_error);
                    getsockopt(socket_handle, SOL_SOCKET, SO_ERROR,
                               reinterpret_cast<char*>(&socket_error), &socket_error_size);
                    result = socket_error == 0 ? 0 : SOCKET_ERROR;
                } else {
                    result = SOCKET_ERROR;
                }
            }
        }
        if (result == 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
            best_ping = static_cast<int>(std::max<int64_t>(1, elapsed));
            connected = true;
        }
        closesocket(socket_handle);
    }
    freeaddrinfo(addresses);
    ping_ms = best_ping;
    return connected;
}

void apply_pending_server_probe_results() {
    std::vector<ServerProbeResult> results;
    {
        std::lock_guard<std::mutex> lock(g_server_probe_mutex);
        if (!g_server_probe_ready) return;
        results.swap(g_pending_server_probe_results);
        g_server_probe_ready = false;
    }
    for (auto& server : g_servers) {
        const auto it = std::find_if(results.begin(), results.end(),
            [&server](const ServerProbeResult& result) { return result.address == server.address; });
        if (it == results.end()) continue;
        server.online = it->online;
        server.status_checked = true;
        server.ping_ms = it->ping_ms;
    }
}

void update_sync_status(bool profile_loaded) {
    std::error_code ec;
    const bool config_ready = !g_config_dir.empty() && std::filesystem::exists(g_config_dir, ec);
    const bool game_ready = !g_game_dir.empty() && std::filesystem::exists(g_game_dir, ec);
    g_sync_status.modules_synced = game_ready && std::filesystem::exists(
        std::filesystem::path(g_game_dir) / "mods", ec);
    g_sync_status.hud_synced = config_ready;
    g_sync_status.keybinds_synced = config_ready && std::filesystem::exists(
        std::filesystem::path(g_config_dir) / "client.cfg", ec);
    g_sync_status.cosmetics_synced = config_ready && std::filesystem::exists(
        std::filesystem::path(g_config_dir) / "shared_cosmetics.txt", ec);
    g_sync_status.settings_synced = g_sync_status.keybinds_synced;
    g_sync_status.server_list_synced = config_ready && std::filesystem::exists(
        std::filesystem::path(g_config_dir) / "shared_servers.txt", ec);
    g_sync_status.friends_synced = config_ready && std::filesystem::exists(
        std::filesystem::path(g_config_dir) / "shared_friends.txt", ec);
    g_sync_status.synced = profile_loaded && g_sync_status.modules_synced &&
        g_sync_status.hud_synced && g_sync_status.keybinds_synced &&
        g_sync_status.cosmetics_synced && g_sync_status.settings_synced &&
        g_sync_status.server_list_synced && g_sync_status.friends_synced;
    g_sync_status.last_sync_time = local_sync_time();
    g_sync_status.sync_error.clear();
    if (!profile_loaded)
        g_sync_status.sync_error = "Launcher profile metadata is unavailable. Relaunch this profile from Amalgam.";
    else if (!g_sync_status.synced)
        g_sync_status.sync_error = "Some profile data is not available yet. Use Sync Now after the launcher finishes saving.";
}

}  // namespace

Theme& theme() { return g_theme; }
ClientSettings& settings() { return g_settings; }
ProfileInfo& profile() { return g_profile; }
PerfMetrics& perf() { return g_perf; }

const char* client_tab_name(ClientTab tab) {
    switch (tab) {
        case ClientTab::Modules: return "Modules";
        case ClientTab::HUD: return "HUD Editor";
        case ClientTab::Profiles: return "Profiles";
        case ClientTab::Cosmetics: return "Cosmetics";
        case ClientTab::Social: return "Social";
        case ClientTab::Servers: return "Servers";
        case ClientTab::Performance: return "Performance";
        case ClientTab::Settings: return "Settings";
        case ClientTab::Diagnostics: return "Diagnostics";
        default: return "Unknown";
    }
}

const char* module_category_name(ModuleCategory cat) {
    switch (cat) {
        case ModuleCategory::Combat: return "Combat";
        case ModuleCategory::Movement: return "Movement";
        case ModuleCategory::Player: return "Player";
        case ModuleCategory::World: return "World";
        case ModuleCategory::Render: return "Render";
        case ModuleCategory::Misc: return "Misc";
        case ModuleCategory::Favorites: return "Favorites";
        case ModuleCategory::All: return "All";
        default: return "Unknown";
    }
}

ModuleCategory module_category_from_index(int index) {
    return static_cast<ModuleCategory>(std::clamp(index, 0, static_cast<int>(ModuleCategory::CategoryCount) - 1));
}

// Module favorites tracking
static std::set<std::string> g_favorited_modules;

bool is_module_favorited(const std::string& module_name) {
    return g_favorited_modules.count(module_name) > 0;
}

void toggle_module_favorite(const std::string& module_name) {
    if (g_favorited_modules.count(module_name) > 0)
        g_favorited_modules.erase(module_name);
    else
        g_favorited_modules.insert(module_name);
}

int client_tab_index(ClientTab tab) { return static_cast<int>(tab); }
ClientTab client_tab_from_index(int i) { return static_cast<ClientTab>(i); }

bool load_client_settings(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return false;
    std::string line;
    std::string key, value;
    while (std::getline(f, line)) {
        if (!read_kv(line, key, value)) continue;
        try {
            if (key == "menu_key") g_settings.menu_key = std::stoi(value);
            else if (key == "ui_scale") g_settings.ui_scale = std::stof(value);
            else if (key == "animations") g_settings.animations = (value == "1");
            else if (key == "notifications") g_settings.notifications_enabled = (value == "1");
            else if (key == "theme_index") g_settings.theme_index = std::stoi(value);
            else if (key == "hud_editor") g_settings.hud_editor_mode = (value == "1");
            else if (key == "show_version") g_settings.show_client_version = (value == "1");
            else if (key == "auto_start") g_settings.auto_start = (value == "1");
            else if (key == "launcher_conn") g_settings.launcher_connection = (value == "1");
            else if (key == "dev_mode") g_settings.developer_mode = (value == "1");
            else if (key == "language") g_settings.language = value;
            else if (key == "show_fps") g_settings.show_fps = (value == "1");
            else if (key == "compact_mode") g_settings.compact_mode = (value == "1");
            else if (key == "auto_update") g_settings.auto_update = (value == "1");
            else if (key == "notif_position") g_settings.notifications_position = std::stoi(value);
            else if (key == "privacy_online") g_settings.privacy_show_online = (value == "1");
            else if (key == "privacy_server") g_settings.privacy_show_server = (value == "1");
            else if (key == "quick_menu_style") g_settings.quick_menu_style = std::stoi(value);
        } catch (const std::invalid_argument&) {
        } catch (const std::out_of_range&) {
        }
    }
    return true;
}

bool save_client_settings(const std::string& path) {
    std::ofstream f(path, std::ios::trunc);
    if (!f.is_open()) return false;
    write_kv(f, "menu_key", g_settings.menu_key);
    write_kv(f, "ui_scale", g_settings.ui_scale);
    write_kv(f, "animations", g_settings.animations);
    write_kv(f, "notifications", g_settings.notifications_enabled);
    write_kv(f, "theme_index", g_settings.theme_index);
    write_kv(f, "hud_editor", g_settings.hud_editor_mode);
    write_kv(f, "show_version", g_settings.show_client_version);
    write_kv(f, "auto_start", g_settings.auto_start);
    write_kv(f, "launcher_conn", g_settings.launcher_connection);
    write_kv(f, "dev_mode", g_settings.developer_mode);
    write_kv(f, "language", g_settings.language);
    write_kv(f, "show_fps", g_settings.show_fps);
    write_kv(f, "compact_mode", g_settings.compact_mode);
    write_kv(f, "auto_update", g_settings.auto_update);
    write_kv(f, "notif_position", g_settings.notifications_position);
    write_kv(f, "privacy_online", g_settings.privacy_show_online);
    write_kv(f, "privacy_server", g_settings.privacy_show_server);
    write_kv(f, "quick_menu_style", g_settings.quick_menu_style);
    return f.good();
}

bool client_init(const std::string& config_dir, const std::string& game_dir) {
    g_config_dir = config_dir;
    g_game_dir = game_dir;
    if (g_game_dir.empty()) {
        std::error_code ec;
        g_game_dir = std::filesystem::current_path(ec).string();
        if (ec) g_game_dir.clear();
    }
    HudManager::instance().set_config_dir(config_dir);
    g_profile = {};
    g_sync_status = {};
    g_perf = {};
    g_perf.ping_ms = -1;
    g_perf.tps = -1.0f;
    g_fps_history.clear();
    g_left_clicks.clear();
    g_left_button_was_down = false;
    create_directories(std::filesystem::path(config_dir));

    const bool had_settings = load_client_settings(cfg_path("client.cfg"));
    if (!had_settings) save_client_settings(cfg_path("client.cfg"));

    // Generate IPC token
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dis;
    char token[33];
    snprintf(token, sizeof(token), "%016llX%016llX",
             static_cast<unsigned long long>(dis(gen)),
             static_cast<unsigned long long>(dis(gen)));
    g_ipc_token = token;

    g_profile.session_start = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    g_profile.session_status = "Waiting for game data";
    refresh_client_data();
    refresh_server_status();

    return true;
}

void client_shutdown() {
    if (g_server_probe_thread.joinable())
        g_server_probe_thread.join();
    g_server_probe_running = false;
    save_client_settings(cfg_path("client.cfg"));
}

void client_tick(float dt) {
    apply_pending_server_probe_results();
    const bool left_button_down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    if (left_button_down && !g_left_button_was_down)
        g_left_clicks.push_back(GetTickCount64());
    g_left_button_was_down = left_button_down;
    const uint64_t click_cutoff = GetTickCount64() - 1000;
    while (!g_left_clicks.empty() && g_left_clicks.front() < click_cutoff)
        g_left_clicks.pop_front();

    // Update performance metrics (timed, not every frame)
    g_perf_accum += dt;
    g_perf_frame_count++;

    if (g_perf_accum >= 0.5) {
        g_perf.fps = static_cast<float>(
            static_cast<double>(g_perf_frame_count) / g_perf_accum);
        g_perf.frame_time_ms = static_cast<float>(g_perf_accum * 1000.0 / g_perf_frame_count);

        // Track FPS history for 1% low
        g_fps_history.push_back(g_perf.fps);
        if (g_fps_history.size() > kFpsHistorySize) {
            g_fps_history.erase(g_fps_history.begin());
        }

        // Calculate 1% low
        if (g_fps_history.size() > 10) {
            std::vector<float> sorted_fps = g_fps_history;
            std::sort(sorted_fps.begin(), sorted_fps.end());
            size_t cutoff = sorted_fps.size() / 100;
            if (cutoff < 1) cutoff = 1;
            g_perf.fps_1pct_low = sorted_fps[cutoff];
        }

        // Memory usage
        MEMORYSTATUSEX statex;
        statex.dwLength = sizeof(statex);
        if (GlobalMemoryStatusEx(&statex)) {
            g_perf.ram_max_mb = static_cast<float>(statex.ullTotalPhys / (1024 * 1024));
            g_perf.ram_mb = g_perf.ram_max_mb * (1.0f - static_cast<float>(statex.ullAvailPhys) / statex.ullTotalPhys);
        }

        const auto player_state = aml::player_stats::store().snapshot();
        g_perf.ping_ms = player_state.has_data && player_state.ping_ms > 0
            ? player_state.ping_ms : -1;
        g_profile.connected = player_state.has_data;
        g_profile.session_status = player_state.has_data ? "In game" : "Waiting for game data";

        // CPU usage via GetSystemTimes
        FILETIME idle_time, kernel_time, user_time;
        if (GetSystemTimes(&idle_time, &kernel_time, &user_time)) {
            ULARGE_INTEGER idle, kernel, user;
            idle.LowPart = idle_time.dwLowDateTime;
            idle.HighPart = idle_time.dwHighDateTime;
            kernel.LowPart = kernel_time.dwLowDateTime;
            kernel.HighPart = kernel_time.dwHighDateTime;
            user.LowPart = user_time.dwLowDateTime;
            user.HighPart = user_time.dwHighDateTime;

            if (g_cpu_initialized) {
                ULONGLONG idle_diff = idle.QuadPart - g_prev_idle.QuadPart;
                ULONGLONG kernel_diff = kernel.QuadPart - g_prev_kernel.QuadPart;
                ULONGLONG user_diff = user.QuadPart - g_prev_user.QuadPart;
                ULONGLONG total = kernel_diff + user_diff;
                if (total > 0) {
                    g_perf.cpu_percent = static_cast<float>(100.0 * (total - idle_diff) / total);
                }
            }
            g_prev_idle = idle;
            g_prev_kernel = kernel;
            g_prev_user = user;
            g_cpu_initialized = true;
        }

        g_perf_accum = 0.0;
        g_perf_frame_count = 0;
    }
}

int clicks_per_second() {
    const uint64_t cutoff = GetTickCount64() - 1000;
    while (!g_left_clicks.empty() && g_left_clicks.front() < cutoff)
        g_left_clicks.pop_front();
    return static_cast<int>(g_left_clicks.size());
}

std::string ipc_token() { return g_ipc_token; }
void set_ipc_token(const std::string& token) { g_ipc_token = token; }
std::string config_dir() { return g_config_dir; }
std::string game_dir() { return g_game_dir; }

std::vector<ModInfo>& mods() { return g_mods; }
std::vector<ServerEntry>& servers() { return g_servers; }
std::vector<FriendEntry>& friends() { return g_friends; }

void scan_mods(const std::string& mods_dir) {
    g_mods.clear();
    std::error_code ec;
    if (!std::filesystem::exists(mods_dir, ec)) return;

    for (const auto& entry : std::filesystem::directory_iterator(mods_dir, ec)) {
        if (!entry.is_regular_file()) continue;
        std::string filename = entry.path().filename().string();
        std::string lower_filename = filename;
        std::transform(lower_filename.begin(), lower_filename.end(), lower_filename.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const bool disabled = lower_filename.size() > 9 &&
            lower_filename.compare(lower_filename.size() - 9, 9, ".disabled") == 0;
        if (disabled) filename.resize(filename.size() - 9);

        auto ext = std::filesystem::path(filename).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext != ".jar" && ext != ".zip") continue;

        ModInfo mod;
        mod.filename = filename;
        mod.name = mod.filename;
        mod.enabled = !disabled;

        // Try to strip version from filename (e.g. "Sodium-0.5.8.jar" -> "Sodium")
        auto dash_pos = mod.name.rfind('-');
        if (dash_pos != std::string::npos) {
            std::string ver = mod.name.substr(dash_pos + 1);
            // Remove extension from version
            auto dot_pos = ver.rfind('.');
            if (dot_pos != std::string::npos) ver = ver.substr(0, dot_pos);
            mod.version = ver;
            mod.name = mod.name.substr(0, dash_pos);
        }

        g_mods.push_back(std::move(mod));
    }
}

bool set_mod_enabled(const std::string& filename, bool enabled) {
    if (g_mods_dir.empty() || filename.empty()) return false;
    const std::filesystem::path source = std::filesystem::path(g_mods_dir) /
        (enabled ? filename + ".disabled" : filename);
    const std::filesystem::path target = std::filesystem::path(g_mods_dir) /
        (enabled ? filename : filename + ".disabled");
    std::error_code ec;
    if (!std::filesystem::exists(source, ec) || std::filesystem::exists(target, ec)) return false;
    std::filesystem::rename(source, target, ec);
    if (ec) return false;
    scan_mods(g_mods_dir);
    return true;
}

void refresh_client_data() {
    const bool profile_loaded = load_shared_profile();
    if (!g_game_dir.empty()) g_mods_dir = (std::filesystem::path(g_game_dir) / "mods").string();
    if (!g_mods_dir.empty()) scan_mods(g_mods_dir);
    g_profile.mod_count = static_cast<int>(g_mods.size());
    if (!g_config_dir.empty()) {
        load_servers(g_config_dir);
        load_friends(g_config_dir);
        load_cosmetics(g_config_dir);
    }
    update_sync_status(profile_loaded);
}

void refresh_server_status() {
    if (g_server_probe_running.exchange(true)) return;

    std::vector<std::string> endpoints;
    endpoints.reserve(g_servers.size());
    for (auto& server : g_servers) {
        if (!server.address.empty()) endpoints.push_back(server.address);
        server.online = false;
        server.status_checked = false;
        server.ping_ms = -1;
    }

    if (endpoints.empty()) {
        g_server_probe_running = false;
        return;
    }

    // A completed worker may still own the thread object. Join it before
    // replacing it, while keeping the UI thread out of network I/O.
    if (g_server_probe_thread.joinable())
        g_server_probe_thread.join();

    g_server_probe_thread = std::thread([endpoints = std::move(endpoints)] {
        std::vector<ServerProbeResult> results;
        results.reserve(endpoints.size());
        for (const auto& endpoint : endpoints) {
            int ping = -1;
            const bool online = probe_server_endpoint(endpoint, ping);
            results.push_back(ServerProbeResult{endpoint, online, ping});
        }
        {
            std::lock_guard<std::mutex> lock(g_server_probe_mutex);
            g_pending_server_probe_results = std::move(results);
            g_server_probe_ready = true;
        }
        g_server_probe_running = false;
    });
}

void load_servers(const std::string& config_dir) {
    g_servers.clear();
    std::string path = config_dir + "\\shared_servers.txt";
    std::ifstream f(path);
    if (!f.is_open()) return;

    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        ServerEntry s;
        auto pipe = line.find('|');
        s.name = line.substr(0, pipe);
        if (pipe != std::string::npos) {
            s.address = line.substr(pipe + 1);
        }
        s.port = 0;  // port is embedded in the launcher's host:port address
        s.online = false;
        s.status_checked = false;
        s.ping_ms = -1;
        g_servers.push_back(std::move(s));
    }
}

void load_friends(const std::string& config_dir) {
    g_friends.clear();
    std::string path = config_dir + "\\shared_friends.txt";
    std::ifstream file(path);
    if (!file.is_open()) return;

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        FriendEntry friend_entry;
        auto pipe = line.find('|');
        friend_entry.name = line.substr(0, pipe);
        if (pipe != std::string::npos) {
            friend_entry.status = line.substr(pipe + 1);
        }
        g_friends.push_back(std::move(friend_entry));
    }
}

void load_cosmetics(const std::string& config_dir) {
    auto& state = cosmetics::cosmetics_state();
    state.items.clear();
    std::string path = config_dir + "\\shared_cosmetics.txt";
    std::ifstream f(path);
    if (!f.is_open()) return;

    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        cosmetics::CosmeticItem item;
        size_t pos = 0;
        auto next_field = [&]() {
            size_t q = line.find('|', pos);
            std::string tok = (q == std::string::npos) ? line.substr(pos) : line.substr(pos, q - pos);
            pos = (q == std::string::npos) ? line.size() : q + 1;
            return tok;
        };
        item.id = next_field();
        item.name = next_field();
        item.category = next_field();
        item.rarity = std::atoi(next_field().c_str());
        item.owned = std::atoi(next_field().c_str()) != 0;
        item.equipped = std::atoi(next_field().c_str()) != 0;
        item.description = next_field();
        state.items.push_back(std::move(item));
    }

    // Launcher entitlements define ownership; the player's active selection is
    // profile-local and survives launcher refreshes in a separate tiny file.
    std::ifstream equipped_file(config_dir + "\\equipped_cosmetics.txt");
    if (equipped_file.is_open()) {
        for (auto& item : state.items) item.equipped = false;
        std::set<std::string> equipped_ids;
        while (std::getline(equipped_file, line)) {
            if (!line.empty() && line[0] != '#') equipped_ids.insert(line);
        }
        for (auto& item : state.items)
            item.equipped = item.owned && equipped_ids.count(item.id) != 0;
    }
}

// ---------------------------------------------------------------------------
// Cosmetics System
// ---------------------------------------------------------------------------

namespace cosmetics {

static CosmeticsState g_cosmetics_state;

CosmeticsState& cosmetics_state() { return g_cosmetics_state; }

std::vector<CosmeticItem> get_cosmetics_by_category(const std::string& category) {
    std::vector<CosmeticItem> result;
    for (const auto& item : g_cosmetics_state.items) {
        if (item.category == category) {
            result.push_back(item);
        }
    }
    return result;
}

static void save_equipped_cosmetics() {
    if (g_config_dir.empty()) return;
    const std::filesystem::path target =
        std::filesystem::path(g_config_dir) / "equipped_cosmetics.txt";
    const std::filesystem::path temporary = target.string() + ".tmp";
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) return;
    file << "# Profile-local equipped cosmetic ids\n";
    for (const auto& item : g_cosmetics_state.items) {
        if (item.owned && item.equipped) file << item.id << "\n";
    }
    file.flush();
    const bool wrote = file.good();
    file.close();
    if (wrote && !MoveFileExA(temporary.string().c_str(), target.string().c_str(),
                              MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        DeleteFileA(temporary.string().c_str());
}

void equip_cosmetic(const std::string& item_id) {
    std::string category;
    for (const auto& item : g_cosmetics_state.items) {
        if (item.id == item_id && item.owned) {
            category = item.category;
            break;
        }
    }
    if (category.empty()) return;
    for (auto& item : g_cosmetics_state.items) {
        if (item.category == category) item.equipped = item.id == item_id;
    }
    save_equipped_cosmetics();
}

void unequip_cosmetic(const std::string& item_id) {
    for (auto& item : g_cosmetics_state.items) {
        if (item.id == item_id) {
            item.equipped = false;
        }
    }
    save_equipped_cosmetics();
}

}  // namespace cosmetics

// ---------------------------------------------------------------------------
// Profile Sync Status
// ---------------------------------------------------------------------------

ProfileSyncStatus& sync_status() { return g_sync_status; }

// ---------------------------------------------------------------------------
// Module Category Helpers
// ---------------------------------------------------------------------------

std::vector<std::string> get_modules_by_category(ModuleCategory cat) {
    std::vector<std::string> result;
    auto& mod_list = mods();
    for (const auto& m : mod_list) {
        // Simple category assignment based on module name
        if (cat == ModuleCategory::All || cat == ModuleCategory::Favorites) {
            if (cat == ModuleCategory::Favorites && !is_module_favorited(m.name)) continue;
            result.push_back(m.name);
            continue;
        }
        // Map module names to categories
        std::string lower_name = m.name;
        std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        bool matches = false;
        switch (cat) {
            case ModuleCategory::Combat:
                matches = lower_name.find("killaura") != std::string::npos ||
                         lower_name.find("combat") != std::string::npos;
                break;
            case ModuleCategory::Movement:
                matches = lower_name.find("fly") != std::string::npos ||
                         lower_name.find("speed") != std::string::npos ||
                         lower_name.find("freecam") != std::string::npos;
                break;
            case ModuleCategory::Player:
                matches = lower_name.find("autotool") != std::string::npos ||
                         lower_name.find("nofall") != std::string::npos;
                break;
            case ModuleCategory::World:
                matches = lower_name.find("world") != std::string::npos ||
                         lower_name.find("esp") != std::string::npos;
                break;
            case ModuleCategory::Render:
                matches = lower_name.find("render") != std::string::npos ||
                         lower_name.find("hud") != std::string::npos ||
                         lower_name.find("overlay") != std::string::npos;
                break;
            case ModuleCategory::Misc:
            default:
                matches = true;
                break;
        }
        if (matches) {
            result.push_back(m.name);
        }
    }
    return result;
}

}  // namespace aml::client
