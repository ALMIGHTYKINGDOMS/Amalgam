#include "render/hud.h"

#include "core/module.h"
#include "core/esp.h"
#include "core/diagnostics_outbox.h"
#include "core/game_mode_telemetry.h"
#include "core/replay.h"
#include "core/tracker.h"
#include "render/input_state.h"
#include "imgui.h"
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_set>

namespace aml::hud {

namespace {

struct HudColors {
    ImU32 esp_friend = IM_COL32(80, 255, 120, 220);
    ImU32 esp_hostile = IM_COL32(255, 95, 95, 220);
    ImU32 esp_neutral = IM_COL32(255, 180, 70, 220);
    ImU32 esp_text = IM_COL32(220, 220, 220, 200);
    ImU32 health_high = IM_COL32(50, 230, 80, 230);
    ImU32 health_mid = IM_COL32(230, 210, 50, 230);
    ImU32 health_low = IM_COL32(230, 50, 50, 230);
    ImU32 health_bg = IM_COL32(30, 30, 30, 160);
    ImU32 radar_bg = IM_COL32(12, 16, 22, 220);
    ImU32 radar_border = IM_COL32(120, 130, 145, 180);
    ImU32 radar_crosshair = IM_COL32(100, 110, 125, 90);
    ImU32 radar_player_self = IM_COL32(80, 220, 130, 255);
    ImU32 radar_player = IM_COL32(255, 95, 95, 255);
    ImU32 radar_hostile = IM_COL32(255, 180, 70, 255);
    ImU32 radar_text = IM_COL32(255, 255, 255, 200);
};

constexpr HudColors g_colors{};

struct RadarConstants {
    float window_w = 190.0f;
    float window_h = 210.0f;
    float center_x = 95.0f;
    float center_y = 105.0f;
    float radius = 72.0f;
    float text_cursor_x = 8.0f;
    float text_cursor_y = 178.0f;
    float player_triangle_size = 7.0f;
    float player_triangle_base = 6.0f;
};

constexpr RadarConstants g_radar{};

std::mutex g_mutex;
std::string g_path;
std::vector<Element> g_elements;
float g_radar_range = 32.0f;
bool g_radar_players = true;
bool g_radar_hostiles = true;
int g_profile = 0;
int g_mode_override = 0;
int g_panel_profile = 0;
bool g_server_safe = false;
std::string g_friend_csv;
std::unordered_set<std::string> g_friends;
bool g_waypoint_key_held = false;
bool g_marker_key_held = false;
bool g_replay_key_held = false;
bool g_has_waypoint = false;
Snapshot g_waypoint{};
bool g_diagnostics_enabled = false;
std::string g_replay_status;

Element* find(const std::string& id) {
    for (auto& element : g_elements) if (element.id == id) return &element;
    return nullptr;
}

void rebuild_friends() {
    g_friends.clear();
    std::stringstream stream(g_friend_csv);
    std::string name;
    while (std::getline(stream, name, ',')) {
        if (!name.empty()) g_friends.insert(name);
    }
}

void defaults() {
    g_diagnostics_enabled = false;
    g_elements = {
        {"coordinates", "Coordinates", true, 24.0f, 24.0f, 1.0f},
        {"status", "Client status", true, 24.0f, 52.0f, 1.0f},
        {"tracker", "Tracker", true, 24.0f, 80.0f, 1.0f},
        {"keystrokes", "Keystrokes", false, 24.0f, 116.0f, 1.0f},
        {"modules", "Active modules", false, 24.0f, 190.0f, 1.0f},
        {"survival", "Survival status", false, 24.0f, 250.0f, 1.0f},
        {"radar", "Entity radar", true, 24.0f, 310.0f, 1.0f},
        {"mode", "Game mode", true, 24.0f, 540.0f, 1.0f},
        {"scoreboard", "Scoreboard", false, 24.0f, 590.0f, 1.0f},
        {"objective", "Objective and timers", false, 24.0f, 820.0f, 1.0f},
        {"teams", "Teams", false, 24.0f, 900.0f, 1.0f},
        {"stats", "Mode stats", false, 24.0f, 950.0f, 1.0f},
        {"replay", "Telemetry replay", false, 24.0f, 1010.0f, 1.0f},
        {"waypoint", "Waypoint", false, 24.0f, 1060.0f, 1.0f},
    };
}

}  // namespace

void init(const std::string& path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_path = path;
    defaults();
    std::ifstream file(path);
    if (!file.is_open()) return;
    std::string line;
    while (std::getline(file, line)) {
        std::istringstream input(line);
        std::string id;
        input >> id;
        if (id == "profile") {
            input >> g_profile;
            continue;
        }
        if (id == "mode_override") {
            input >> g_mode_override;
            telemetry::store().set_manual_mode(telemetry::mode_from_index(g_mode_override));
            continue;
        }
        if (id == "panel_profile") {
            input >> g_panel_profile;
            continue;
        }
        if (id == "diagnostics") {
            int enabled = 0;
            input >> enabled;
            g_diagnostics_enabled = enabled != 0;
            diagnostics::outbox().set_enabled(g_diagnostics_enabled);
            continue;
        }
        if (id == "safe") {
            int value = 0;
            input >> value;
            g_server_safe = value != 0;
            modules_set_safe_mode(g_server_safe);
            continue;
        }
        if (id == "radar") {
            input >> g_radar_range;
            int players = 1;
            int hostiles = 1;
            input >> players >> hostiles;
            g_radar_players = players != 0;
            g_radar_hostiles = hostiles != 0;
            continue;
        }
        if (id == "friends") {
            std::getline(input, g_friend_csv);
            if (!g_friend_csv.empty() && g_friend_csv.front() == ' ') g_friend_csv.erase(0, 1);
            rebuild_friends();
            continue;
        }
        int enabled = 1;
        Element* element = find(id);
        if (!element) {
            continue;
        }
        input >> element->x >> element->y >> element->scale >> enabled;
        element->enabled = enabled != 0;
    }
}

void save() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_path.empty()) return;
    std::ofstream file(g_path, std::ios::trunc);
    if (!file.is_open()) return;
    for (const auto& element : g_elements)
        file << element.id << ' ' << element.x << ' ' << element.y << ' ' << element.scale << ' '
             << (element.enabled ? 1 : 0) << '\n';
    file << "profile " << g_profile << '\n';
    file << "mode_override " << g_mode_override << '\n';
    file << "panel_profile " << g_panel_profile << '\n';
    file << "diagnostics " << (g_diagnostics_enabled ? 1 : 0) << '\n';
    file << "safe " << (g_server_safe ? 1 : 0) << '\n';
    file << "radar " << g_radar_range << ' ' << (g_radar_players ? 1 : 0) << ' '
         << (g_radar_hostiles ? 1 : 0) << '\n';
    file << "friends " << g_friend_csv << '\n';
}

bool server_safe() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_server_safe;
}

void set_server_safe(bool enabled) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_server_safe = enabled;
    }
    modules_set_safe_mode(enabled);
}

void draw(const Snapshot& snapshot) {
    bool waypoint_key = input::key_down(VK_F6);
    if (waypoint_key && !g_waypoint_key_held) {
        g_waypoint = snapshot;
        g_has_waypoint = true;
    }
    g_waypoint_key_held = waypoint_key;
    bool marker_key = input::key_down(VK_F7);
    if (marker_key && !g_marker_key_held)
        telemetry::store().add_marker("manual", "F7 marker");
    g_marker_key_held = marker_key;
    bool replay_key = input::key_down(VK_F8);
    if (replay_key && !g_replay_key_held) {
        if (replay::recorder().active()) {
            replay::recorder().stop();
            g_replay_status = "Replay recording stopped";
        } else {
            std::string path = g_path.empty() ? "amalgam-session.amrl" : g_path + ".replay.amrl";
            g_replay_status = replay::recorder().start(path) ? "Replay recording started" : "Replay start failed";
        }
    }
    g_replay_key_held = replay_key;
    std::vector<Element> elements;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        elements = g_elements;
    }
    std::vector<tracker::Entity> tracked = tracker::store().entities();
    std::vector<esp::ProjectedEntity> projected = esp::snapshot();
    telemetry::State mode_state = telemetry::store().snapshot();
    ImDrawList* foreground = ImGui::GetForegroundDrawList();
    ImVec2 display = ImGui::GetIO().DisplaySize;
    for (const auto& entity : projected) {
        if (entity.depth <= 0.0f || entity.x < 0.0f || entity.y < 0.0f ||
            entity.x > display.x || entity.y > display.y)
            continue;
        float size = std::clamp(260.0f / std::max(1.0f, entity.depth), 10.0f, 90.0f);
        const tracker::Entity* tracked_entity = nullptr;
        for (const auto& te : tracked) {
            if (te.id == entity.id) { tracked_entity = &te; break; }
        }
        bool is_friend = tracked_entity && g_friends.count(tracked_entity->name);
        ImU32 color = is_friend ? g_colors.esp_friend
                     : entity.kind == 2 ? g_colors.esp_hostile
                     : g_colors.esp_neutral;
        ImVec2 box_min(entity.x - size * 0.5f, entity.y - size);
        ImVec2 box_max(entity.x + size * 0.5f, entity.y);
        foreground->AddRect(box_min, box_max, color, 2.0f, 0, 1.5f);
        if (tracked_entity) {
            std::string display_name = tracked_entity->name;
            if (display_name.size() > 12) { display_name.resize(11); display_name += "\xe2\x80\xa6"; }
            ImVec2 name_size = ImGui::CalcTextSize(display_name.c_str());
            foreground->AddText(nullptr, 0, ImVec2(entity.x - name_size.x * 0.5f, box_min.y - name_size.y - 2.0f), color, display_name.c_str());
            float health = std::clamp(tracked_entity->health, 0.0f, 20.0f);
            float health_frac = health / 20.0f;
            ImU32 health_color = health_frac > 0.50f ? g_colors.health_high
                               : health_frac > 0.25f ? g_colors.health_mid
                               : g_colors.health_low;
            float bar_x = box_max.x + 4.0f;
            float bar_w = 3.0f;
            float bar_h = size;
            float bar_top = box_min.y;
            foreground->AddRectFilled(ImVec2(bar_x, bar_top), ImVec2(bar_x + bar_w, bar_top + bar_h), g_colors.health_bg);
            foreground->AddRectFilled(ImVec2(bar_x, bar_top + bar_h * (1.0f - health_frac)), ImVec2(bar_x + bar_w, bar_top + bar_h), health_color);
            float dist = std::sqrt(tracked_entity->distance_sq);
            char dist_buf[16];
            snprintf(dist_buf, sizeof(dist_buf), "%.0fm", dist);
            ImVec2 dist_size = ImGui::CalcTextSize(dist_buf);
            foreground->AddText(nullptr, 0, ImVec2(entity.x - dist_size.x * 0.5f, box_max.y + 2.0f), g_colors.esp_text, dist_buf);
        }
    }
    for (const auto& element : elements) {
        if (!element.enabled) continue;
        ImGui::SetNextWindowPos(ImVec2(element.x, element.y), ImGuiCond_Always);
        if (element.id == "radar") ImGui::SetNextWindowSize(ImVec2(190.0f, 210.0f), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.35f);
        ImGui::Begin(("##hud-" + element.id).c_str(), nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav);
        ImGui::SetWindowFontScale(element.scale);
        if (element.id == "radar") {
            ImDrawList* draw = ImGui::GetWindowDrawList();
            ImVec2 top = ImGui::GetWindowPos();
            ImVec2 center(top.x + 95.0f, top.y + 105.0f);
            const float radius = 72.0f;
            draw->AddCircleFilled(center, radius, g_colors.radar_bg, 48);
            draw->AddCircle(center, radius, g_colors.radar_border, 48, 1.0f);
            draw->AddLine(ImVec2(center.x - radius, center.y), ImVec2(center.x + radius, center.y),
                          g_colors.radar_crosshair);
            draw->AddLine(ImVec2(center.x, center.y - radius), ImVec2(center.x, center.y + radius),
                          g_colors.radar_crosshair);
            draw->AddTriangleFilled(ImVec2(center.x, center.y - 7),
                                    ImVec2(center.x - 6, center.y + 6),
                                    ImVec2(center.x + 6, center.y + 6), g_colors.radar_player_self);
            float yaw = snapshot.yaw * 3.14159265f / 180.0f;
            for (const auto& entity : tracked) {
                if (entity.kind == tracker::ENTITY_PLAYER && !g_radar_players) continue;
                if (entity.kind == tracker::ENTITY_HOSTILE && !g_radar_hostiles) continue;
                float dx = entity.x - snapshot.px;
                float dz = entity.z - snapshot.pz;
                if ((dx == 0.0f && dz == 0.0f) || entity.distance_sq <= 0.0f) continue;
                float screen_x = dx * std::cos(yaw) + dz * std::sin(yaw);
                float forward = -dx * std::sin(yaw) + dz * std::cos(yaw);
                float distance = std::sqrt(entity.distance_sq);
                float radar_range = g_profile == 1 ? 48.0f : g_profile == 2 ? 24.0f : g_radar_range;
                float scale = std::min(1.0f, distance / radar_range);
                float px = center.x + screen_x / radar_range * radius;
                float py = center.y - forward / radar_range * radius;
                if (screen_x * screen_x + forward * forward > radar_range * radar_range) {
                    float length = std::sqrt(screen_x * screen_x + forward * forward);
                    px = center.x + screen_x / length * radius;
                    py = center.y - forward / length * radius;
                }
                bool friend_entity = entity.kind == tracker::ENTITY_PLAYER &&
                                     g_friends.find(entity.name) != g_friends.end();
                ImU32 color = entity.kind == tracker::ENTITY_PLAYER
                                  ? (friend_entity ? g_colors.radar_player_self
                                                    : g_colors.radar_player)
                                                                      : g_colors.radar_hostile;
                draw->AddCircleFilled(ImVec2(px, py), 4.0f + (1.0f - scale) * 3.0f, color, 12);
                if (!entity.name.empty() && entity.name.size() > 1) {
                    auto name = entity.name;
                    if (name.size() > 12) name = name.substr(0, 10) + "..";
                    draw->AddText(ImVec2(px + 6.0f, py - 4.0f), g_colors.radar_text, name.c_str());
                }
            }
            ImGui::SetCursorPos(ImVec2(8, 178));
            ImGui::Text("Radar %.0fm  |  %d tracked", g_radar_range, static_cast<int>(tracked.size()));
        } else if (element.id == "coordinates") {
            ImGui::Text("XYZ  %.1f  %.1f  %.1f", snapshot.px, snapshot.py, snapshot.pz);
        } else if (element.id == "status") {
            const float speed = std::sqrt(snapshot.vx * snapshot.vx +
                                          snapshot.vy * snapshot.vy +
                                          snapshot.vz * snapshot.vz);
            ImGui::Text("Speed  %.2f  |  Fall  %.1f", speed, snapshot.fall_distance);
        } else if (element.id == "tracker") {
            float nearest = 0.0f;
            int hostile_count = 0;
            if (!tracked.empty()) {
                nearest = tracked.front().distance_sq;
                for (const auto& entity : tracked) {
                    nearest = std::min(nearest, entity.distance_sq);
                    if (entity.kind == tracker::ENTITY_HOSTILE) ++hostile_count;
                }
            }
            if (snapshot.entity_count <= 0) hostile_count = snapshot.hostile_count;
            ImGui::Text("Tracked hostiles  %d", hostile_count);
             if (!tracked.empty()) {
                 ImGui::Text("Nearest  %.1f m", std::sqrt(nearest));
                 auto nearest_entity = std::min_element(tracked.begin(), tracked.end(),
                     [](const tracker::Entity& a, const tracker::Entity& b) {
                         return a.distance_sq < b.distance_sq;
                     });
                 if (nearest_entity != tracked.end() && !nearest_entity->name.empty())
                     ImGui::Text("Target  %s", nearest_entity->name.c_str());
             }
        } else if (element.id == "keystrokes") {
            ImGui::Text("W %s  A %s  S %s  D %s",
                        input::key_down('W') ? "ON" : "  ", input::key_down('A') ? "ON" : "  ",
                        input::key_down('S') ? "ON" : "  ", input::key_down('D') ? "ON" : "  ");
            ImGui::Text("Space %s  Shift %s", input::key_down(VK_SPACE) ? "ON" : "  ",
                        input::key_down(VK_LSHIFT) ? "ON" : "  ");
        } else if (element.id == "modules") {
            bool any = false;
            for (size_t i = 0; i < modules_count(); ++i) {
                Module module = module_snapshot(i);
                if (!module.enabled) continue;
                if (any) ImGui::SameLine();
                ImGui::TextUnformatted(module.name);
                any = true;
            }
            if (!any) ImGui::TextUnformatted("No active modules");
        } else if (element.id == "survival") {
            ImGui::Text("Ground %s  |  Breaking %s", snapshot.on_ground ? "YES" : "NO",
                        snapshot.breaking ? "YES" : "NO");
            ImGui::Text("Fall %.1f  |  Slot %d", snapshot.fall_distance, snapshot.selected_slot + 1);
        } else if (element.id == "mode") {
            ImGui::Text("Mode  %s  |  Confidence %d%%", telemetry::mode_name(mode_state.mode),
                        mode_state.confidence);
            if (mode_state.active) {
                ImGui::TextUnformatted(mode_state.title.empty() ? "Scoreboard" : mode_state.title.c_str());
            } else {
                ImGui::TextUnformatted("No scoreboard telemetry");
            }
        } else if (element.id == "scoreboard") {
            if (mode_state.title.empty()) {
                ImGui::TextUnformatted("No scoreboard");
            } else {
                ImGui::TextUnformatted(mode_state.title.c_str());
                for (const auto& line : mode_state.lines) ImGui::TextUnformatted(line.text.c_str());
            }
        } else if (element.id == "objective") {
            ImGui::TextUnformatted(mode_state.objective.empty() ? "No objective" : mode_state.objective.c_str());
            if (mode_state.timer_seconds >= 0)
                ImGui::Text("Timer  %02d:%02d", mode_state.timer_seconds / 60, mode_state.timer_seconds % 60);
            if (mode_state.refill_seconds >= 0)
                ImGui::Text("Refill  %02d:%02d", mode_state.refill_seconds / 60, mode_state.refill_seconds % 60);
            if (mode_state.generator_seconds >= 0)
                ImGui::Text("Generator  %02d:%02d", mode_state.generator_seconds / 60,
                            mode_state.generator_seconds % 60);
            if (mode_state.chest_seconds >= 0)
                ImGui::Text("Chest  %02d:%02d", mode_state.chest_seconds / 60,
                            mode_state.chest_seconds % 60);
            if (mode_state.border_seconds >= 0)
                ImGui::Text("Border  %02d:%02d", mode_state.border_seconds / 60, mode_state.border_seconds % 60);
            if (mode_state.teleport_seconds >= 0)
                ImGui::Text("Teleport  %02d:%02d", mode_state.teleport_seconds / 60,
                            mode_state.teleport_seconds % 60);
            if (mode_state.match_elapsed_ms > 0)
                ImGui::Text("Match  %02llu:%02llu",
                            static_cast<unsigned long long>(mode_state.match_elapsed_ms / 60000),
                            static_cast<unsigned long long>((mode_state.match_elapsed_ms / 1000) % 60));
        } else if (element.id == "teams") {
            if (mode_state.teams_alive >= 0) ImGui::Text("Teams alive  %d", mode_state.teams_alive);
            if (mode_state.beds_alive >= 0) ImGui::Text("Beds  %d", mode_state.beds_alive);
            if (!mode_state.faction.empty()) ImGui::TextUnformatted(mode_state.faction.c_str());
            if (!mode_state.claim.empty()) ImGui::TextUnformatted(mode_state.claim.c_str());
            if (mode_state.teams_alive < 0 && mode_state.beds_alive < 0 && mode_state.faction.empty() &&
                mode_state.claim.empty()) ImGui::TextUnformatted("No team data");
        } else if (element.id == "stats") {
            if (mode_state.players_alive >= 0) ImGui::Text("Players alive  %d", mode_state.players_alive);
            if (mode_state.kills >= 0) ImGui::Text("Kills  %d", mode_state.kills);
            if (mode_state.final_kills >= 0) ImGui::Text("Final kills  %d", mode_state.final_kills);
            if (mode_state.episode >= 0) ImGui::Text("Episode  %d", mode_state.episode);
            if (mode_state.round >= 0) ImGui::Text("Round  %d", mode_state.round);
            if (mode_state.crystals >= 0) ImGui::Text("Crystals  %d", mode_state.crystals);
            if (mode_state.explosions >= 0) ImGui::Text("Explosions  %d", mode_state.explosions);
            if (!mode_state.map.empty()) ImGui::TextUnformatted(mode_state.map.c_str());
            if (!mode_state.opponent.empty()) ImGui::TextUnformatted(mode_state.opponent.c_str());
            telemetry::SessionStats session = telemetry::store().session_stats();
            ImGui::Text("Session matches  %llu", static_cast<unsigned long long>(session.matches));
            ImGui::Text("Session kills  %llu", static_cast<unsigned long long>(session.total_kills));
            if (mode_state.players_alive < 0 && mode_state.kills < 0 && mode_state.final_kills < 0 &&
                mode_state.episode < 0 && mode_state.round < 0 && mode_state.map.empty() &&
                mode_state.opponent.empty()) ImGui::TextUnformatted("No profile stats");
        } else if (element.id == "replay") {
            ImGui::TextUnformatted(replay::recorder().active() ? "Recording telemetry replay" : "Replay idle");
            if (!replay::recorder().path().empty())
                ImGui::TextUnformatted(replay::recorder().path().c_str());
            if (!g_replay_status.empty()) ImGui::TextUnformatted(g_replay_status.c_str());
            ImGui::TextUnformatted("F8 starts or stops redacted telemetry replay");
        } else if (element.id == "waypoint") {
            if (!g_has_waypoint) {
                ImGui::TextUnformatted("Press F6 to set waypoint");
            } else {
                float dx = snapshot.px - g_waypoint.px;
                float dy = snapshot.py - g_waypoint.py;
                float dz = snapshot.pz - g_waypoint.pz;
                ImGui::Text("Waypoint %.1f, %.1f, %.1f", g_waypoint.px, g_waypoint.py, g_waypoint.pz);
                ImGui::Text("Distance %.1f m", std::sqrt(dx * dx + dy * dy + dz * dz));
            }
        }
        ImGui::End();
    }
}

void draw_editor() {
    bool should_save = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!ImGui::TreeNode("HUD layout")) return;
        for (auto& element : g_elements) {
            ImGui::PushID(element.id.c_str());
            ImGui::Checkbox("##enabled", &element.enabled);
            ImGui::SameLine();
            ImGui::TextUnformatted(element.name.c_str());
            ImGui::DragFloat("X", &element.x, 1.0f, 0.0f, 10000.0f);
            ImGui::DragFloat("Y", &element.y, 1.0f, 1.0f, 10000.0f);
            ImGui::SliderFloat("Scale", &element.scale, 0.5f, 2.5f);
            ImGui::PopID();
        }
        ImGui::Separator();
        ImGui::TextUnformatted("Radar profile");
        int previous_profile = g_profile;
        ImGui::Combo("Mode", &g_profile, "Survival\0PvP\0Practice\0", 3);
        if (previous_profile != g_profile) {
            if (g_profile == 0) {
                g_radar_range = 32.0f;
                g_radar_players = false;
                g_radar_hostiles = true;
            } else if (g_profile == 1) {
                g_radar_range = 48.0f;
                g_radar_players = true;
                g_radar_hostiles = false;
            } else {
                g_radar_range = 24.0f;
                g_radar_players = true;
                g_radar_hostiles = true;
            }
        }
        int previous_mode_override = g_mode_override;
        const char* telemetry_modes = "Auto\0Unknown\0BedWars\0SkyWars\0UHC\0Crystal PvP\0SMP/Factions\0Practice\0";
        ImGui::Combo("Telemetry mode", &g_mode_override, telemetry_modes);
        if (previous_mode_override != g_mode_override)
            telemetry::store().set_manual_mode(telemetry::mode_from_index(g_mode_override));
        if (ImGui::Button("Export telemetry report")) {
            const std::string report_path = g_path.empty() ? "amalgam-telemetry.csv" : g_path + ".telemetry.csv";
            telemetry::store().export_report(report_path);
        }
        bool previous_diagnostics = g_diagnostics_enabled;
        ImGui::Checkbox("Opt-in local diagnostics", &g_diagnostics_enabled);
        if (previous_diagnostics != g_diagnostics_enabled) {
            if (g_diagnostics_enabled && !diagnostics::outbox().set_enabled(true))
                g_diagnostics_enabled = false;
            else if (!g_diagnostics_enabled)
                diagnostics::outbox().set_enabled(false);
        }
        ImGui::Text("Pending local summaries: %llu",
                    static_cast<unsigned long long>(diagnostics::outbox().pending()));
        if (ImGui::Button("Export match data")) {
            std::string base = g_path.empty() ? "amalgam" : g_path;
            telemetry::store().export_match_summaries(base + ".matches.csv");
            telemetry::store().export_markers(base + ".markers.csv");
            telemetry::store().export_session_stats(base + ".session.csv");
            telemetry::store().export_report(base + ".telemetry.csv");
            if (g_diagnostics_enabled) {
                std::wstring path(base.begin(), base.end());
                path += L".diagnostics.csv";
                diagnostics::outbox().export_csv(path);
            }
        }
        if (ImGui::Button(replay::recorder().active() ? "Stop telemetry replay" : "Start telemetry replay")) {
            if (replay::recorder().active()) {
                replay::recorder().stop();
                g_replay_status = "Replay recording stopped";
            } else {
                std::string path = g_path.empty() ? "amalgam-session.amrl" : g_path + ".replay.amrl";
                g_replay_status = replay::recorder().start(path) ? "Replay recording started" : "Replay start failed";
            }
        }
        if (ImGui::Button("Validate telemetry replay")) {
            replay::Player player;
            std::string path = replay::recorder().path();
            if (path.empty()) path = g_path.empty() ? "amalgam-session.amrl" : g_path + ".replay.amrl";
            int records = 0;
            replay::Record record;
            if (player.open(path)) {
                while (player.next(record)) ++records;
                g_replay_status = "Replay records: " + std::to_string(records);
            } else {
                g_replay_status = "Replay file unavailable";
            }
        }
        if (ImGui::Button("Delete local diagnostics")) diagnostics::outbox().delete_all();
        bool previous_safe = g_server_safe;
        ImGui::Checkbox("Server-safe mode", &g_server_safe);
        if (previous_safe != g_server_safe) modules_set_safe_mode(g_server_safe);
        if (!previous_safe && g_server_safe) {
            for (size_t i = 0; i < modules_count(); ++i)
                module_set_enabled(module_snapshot(i).id, false);
        }
        ImGui::SliderFloat("Radar range", &g_radar_range, 8.0f, 128.0f);
        ImGui::Checkbox("Show players", &g_radar_players);
        ImGui::Checkbox("Show hostiles", &g_radar_hostiles);
        char friends[512]{};
        strncpy_s(friends, g_friend_csv.c_str(), _TRUNCATE);
        if (ImGui::InputText("Friends (comma separated)", friends, sizeof(friends))) {
            g_friend_csv = friends;
            rebuild_friends();
        }
        should_save = ImGui::Button("Save HUD layout");
        ImGui::TreePop();
    }
    if (should_save) save();
}

const char* panel_profile_name(int profile) {
    static const char* names[] = {"Auto", "Survival", "PvP", "Practice", "BedWars",
                                  "SkyWars", "UHC", "Crystal PvP", "SMP/Factions", "Modded"};
    return profile >= 0 && profile < 10 ? names[profile] : names[0];
}

int panel_profile() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_panel_profile;
}

void set_panel_profile(int profile) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_panel_profile = std::clamp(profile, 0, 9);
    switch (g_panel_profile) {
        case 1: g_profile = 0; g_mode_override = 1; break; // Survival
        case 2: g_profile = 1; g_mode_override = 1; break; // PvP
        case 3: g_profile = 2; g_mode_override = 7; break; // Practice
        case 4: g_profile = 2; g_mode_override = 2; break; // BedWars
        case 5: g_profile = 2; g_mode_override = 3; break; // SkyWars
        case 6: g_profile = 0; g_mode_override = 4; break; // UHC
        case 7: g_profile = 1; g_mode_override = 5; break; // Crystal PvP
        case 8: g_profile = 0; g_mode_override = 6; break; // SMP/Factions
        case 9: g_profile = 0; g_mode_override = 1; break; // Modded
        default: g_profile = 0; g_mode_override = 0; break; // Auto
    }
    telemetry::store().set_manual_mode(telemetry::mode_from_index(g_mode_override));
}

std::vector<Element> snapshot_elements() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_elements;
}

}  // namespace aml::hud
