#include "client/client_hud2.h"
#include "client/client_core.h"
#include "render/input_state.h"
#include "core/tracker.h"
#include "core/player_stats.h"
#include "imgui.h"

#include <windows.h>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <mutex>
#include <cmath>
#include <cstring>
#include <chrono>
#include <ctime>

namespace aml::client {

HudManager& HudManager::instance() {
    static HudManager mgr;
    return mgr;
}

void HudManager::set_config_dir(const std::string& dir) {
    std::lock_guard<std::mutex> lock(mu_);
    config_dir_ = dir;
}

void HudManager::init() {
    std::lock_guard<std::mutex> lock(mu_);

    modules_ = {
        {"fps", "FPS", true, 10.0f, 10.0f, 1.0f, 1.0f, false, 1.0f, 0},
        {"ping", "Ping", false, 10.0f, 30.0f, 1.0f, 1.0f, false, 1.0f, 0},
        {"coordinates", "Coordinates", true, 10.0f, 50.0f, 1.0f, 1.0f, false, 1.0f, 0},
        {"cps", "CPS", false, 10.0f, 70.0f, 1.0f, 1.0f, false, 1.0f, 0},
        {"keystrokes", "Keystrokes", false, 10.0f, 90.0f, 1.0f, 1.0f, false, 1.0f, 0},
        {"armor", "Armor", false, 10.0f, 110.0f, 1.0f, 1.0f, false, 1.0f, 0},
        {"potion_effects", "Potion Effects", false, 10.0f, 130.0f, 1.0f, 1.0f, false, 1.0f, 0},
        {"ram_usage", "RAM Usage", false, 10.0f, 150.0f, 1.0f, 1.0f, false, 1.0f, 0},
        {"cpu_usage", "CPU Usage", false, 10.0f, 170.0f, 1.0f, 1.0f, false, 1.0f, 0},
        {"clock", "Clock", false, 10.0f, 190.0f, 1.0f, 1.0f, false, 1.0f, 0},
        {"biome", "Biome", false, 10.0f, 210.0f, 1.0f, 1.0f, false, 1.0f, 0},
        {"direction", "Direction", false, 10.0f, 230.0f, 1.0f, 1.0f, false, 1.0f, 0},
        {"speed", "Speed", false, 10.0f, 250.0f, 1.0f, 1.0f, false, 1.0f, 0},
        {"server_tps", "Server TPS", false, 10.0f, 270.0f, 1.0f, 1.0f, false, 1.0f, 0},
        {"player_count", "Player Count", false, 10.0f, 290.0f, 1.0f, 1.0f, false, 1.0f, 0},
        {"session_status", "Session Status", false, 10.0f, 310.0f, 1.0f, 1.0f, false, 1.0f, 0},
    };
}

std::vector<HudModule> HudManager::get_modules() const {
    std::lock_guard<std::mutex> lock(mu_);
    return modules_;
}

void HudManager::set_module_enabled(const std::string& id, bool enabled) {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& m : modules_) {
        if (m.id == id) { m.enabled = enabled; return; }
    }
}

void HudManager::set_module_position(const std::string& id, float x, float y) {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& m : modules_) {
        if (m.id == id) { m.x = x; m.y = y; return; }
    }
}

void HudManager::set_module_scale(const std::string& id, float scale) {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& m : modules_) {
        if (m.id == id) { m.scale = scale; return; }
    }
}

void HudManager::set_all_disabled() {
    for (auto& m : modules_) {
        m.enabled = false;
    }
}

void HudManager::apply_preset(const std::string& name) {
    std::lock_guard<std::mutex> lock(mu_);

    auto disable_all = [this]() { for (auto& m : modules_) m.enabled = false; };
    auto enable = [this](const char* id) { for (auto& m : modules_) if (m.id == id) m.enabled = true; };
    auto pos = [this](const char* id, float x, float y) { for (auto& m : modules_) if (m.id == id) { m.x = x; m.y = y; } };

    if (name == "Minimal") {
        disable_all();
        enable("fps"); enable("coordinates");
        pos("fps", 10.0f, 10.0f); pos("coordinates", 10.0f, 30.0f);
    } else if (name == "PvP") {
        disable_all();
        enable("fps"); enable("ping"); enable("keystrokes"); enable("armor"); enable("cps");
        pos("fps", 10.0f, 10.0f); pos("ping", 10.0f, 30.0f);
        pos("keystrokes", 10.0f, 50.0f); pos("armor", 10.0f, 110.0f); pos("cps", 10.0f, 130.0f);
    } else if (name == "Survival") {
        disable_all();
        enable("fps"); enable("coordinates"); enable("armor"); enable("potion_effects");
        pos("fps", 10.0f, 10.0f); pos("coordinates", 10.0f, 30.0f);
        pos("armor", 10.0f, 50.0f); pos("potion_effects", 10.0f, 70.0f);
    } else if (name == "Performance") {
        disable_all();
        enable("fps"); enable("ram_usage"); enable("cpu_usage"); enable("ping"); enable("server_tps");
        pos("fps", 10.0f, 10.0f); pos("ram_usage", 10.0f, 30.0f);
        pos("cpu_usage", 10.0f, 50.0f); pos("ping", 10.0f, 70.0f); pos("server_tps", 10.0f, 90.0f);
    } else if (name == "Streaming") {
        disable_all();
        enable("fps"); enable("session_status"); enable("player_count"); enable("clock");
        pos("fps", 10.0f, 10.0f); pos("session_status", 10.0f, 30.0f);
        pos("player_count", 10.0f, 50.0f); pos("clock", 10.0f, 70.0f);
    }
}

void HudManager::render_hud() {
    std::lock_guard<std::mutex> lock(mu_);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                              ImGuiWindowFlags_NoNav |
                              ImGuiWindowFlags_NoMove |
                              ImGuiWindowFlags_NoFocusOnAppearing |
                              ImGuiWindowFlags_NoInputs |
                              ImGuiWindowFlags_AlwaysAutoResize |
                              ImGuiWindowFlags_NoBackground;

    for (const auto& m : modules_) {
        if (!m.enabled) continue;

        ImGui::SetNextWindowPos(ImVec2(m.x, m.y), ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, m.opacity);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 3));

        char win[64];
        snprintf(win, sizeof(win), "##hud_%s", m.id.c_str());
        ImGui::Begin(win, nullptr, flags);

        ImGui::PushFont(nullptr);
        if (m.scale != 1.0f)
            ImGui::SetWindowFontScale(m.scale);
        if (m.text_size != 1.0f)
            ImGui::SetWindowFontScale(m.scale * m.text_size);

        if (m.background) {
            ImVec2 text_size = ImGui::CalcTextSize(m.name.c_str());
            const ImVec2 cursor = ImGui::GetCursorScreenPos();
            const ImVec2 panel_min(cursor.x - 4.0f, cursor.y - 2.0f);
            const ImVec2 panel_max(cursor.x + text_size.x + 10.0f,
                                   cursor.y + text_size.y + 6.0f);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(panel_min, panel_max,
                              ImGui::ColorConvertFloat4ToU32(ImVec4(theme().bg.x, theme().bg.y,
                                                                       theme().bg.z, 0.86f)), 5.0f);
            dl->AddRect(panel_min, panel_max,
                        ImGui::ColorConvertFloat4ToU32(ImVec4(theme().border.x, theme().border.y,
                                                                 theme().border.z, 0.80f)), 5.0f, 0, 1.0f);
            dl->AddRectFilled(panel_min, ImVec2(panel_min.x + 2.0f, panel_max.y),
                              ImGui::ColorConvertFloat4ToU32(ImVec4(theme().accent.x, theme().accent.y,
                                                                       theme().accent.z, 0.90f)), 2.0f);
        }

        ImVec4 col = theme().text;
        if (m.id == "fps") {
            col = perf().fps > 55.0f ? theme().success :
                  perf().fps > 30.0f ? theme().warning : theme().error;
        } else if (m.id == "ping") {
            col = perf().ping_ms < 0 ? theme().muted :
                  perf().ping_ms < 50 ? theme().success :
                  perf().ping_ms < 100 ? theme().warning : theme().error;
        }

        ImGui::PushStyleColor(ImGuiCol_Text, col);

        // Apply text alignment
        if (m.alignment == 1) {  // center
            float text_w = ImGui::CalcTextSize(m.name.c_str()).x;
            float win_w = ImGui::GetContentRegionAvail().x;
            if (win_w > text_w)
                ImGui::SetCursorPosX((win_w - text_w) * 0.5f + ImGui::GetCursorPosX());
        } else if (m.alignment == 2) {  // right
            float text_w = ImGui::CalcTextSize(m.name.c_str()).x;
            float win_w = ImGui::GetContentRegionAvail().x;
            if (win_w > text_w)
                ImGui::SetCursorPosX(win_w - text_w + ImGui::GetCursorPosX());
        }

        if (m.id == "fps") {
            char buf[32];
            snprintf(buf, sizeof(buf), "FPS: %.0f", perf().fps);
            ImGui::Text("%s", buf);
        } else if (m.id == "ping") {
            if (perf().ping_ms < 0) {
                ImGui::TextDisabled("Ping: unavailable");
            } else {
                char buf[32];
                snprintf(buf, sizeof(buf), "Ping: %dms", perf().ping_ms);
                ImGui::Text("%s", buf);
            }
        } else if (m.id == "coordinates") {
            auto snap = aml::tracker::store().snapshot();
            char buf[64];
            snprintf(buf, sizeof(buf), "XYZ: %.1f / %.1f / %.1f", snap.px, snap.py, snap.pz);
            ImGui::Text("%s", buf);
        } else if (m.id == "ram_usage") {
            char buf[64];
            snprintf(buf, sizeof(buf), "RAM: %.1f / %.1f MB",
                     perf().ram_mb, perf().ram_max_mb);
            ImGui::Text("%s", buf);
        } else if (m.id == "cpu_usage") {
            char buf[32];
            snprintf(buf, sizeof(buf), "CPU: %.1f%%", perf().cpu_percent);
            ImGui::Text("%s", buf);
        } else if (m.id == "server_tps") {
            if (perf().tps < 0.0f) {
                ImGui::TextDisabled("TPS: unavailable");
            } else {
                char buf[32];
                snprintf(buf, sizeof(buf), "TPS: %.1f", perf().tps);
                ImGui::Text("%s", buf);
            }
        } else if (m.id == "session_status") {
            ImGui::Text("%s", profile().session_status.empty() ? "Unavailable" : profile().session_status.c_str());
        } else if (m.id == "armor") {
            const auto state = aml::player_stats::store().snapshot();
            if (!state.has_data) {
                ImGui::TextDisabled("Armor: unavailable");
            } else {
                ImGui::Text("Armor: %d/20", state.armor_points);
            }
        } else if (m.id == "potion_effects") {
            const auto state = aml::player_stats::store().snapshot();
            if (!state.has_data) {
                ImGui::TextDisabled("Effects: unavailable");
            } else if (state.effects.empty()) {
                ImGui::Text("Effects: none");
            } else {
                ImGui::Text("Effects: %zu active", state.effects.size());
            }
        } else if (m.id == "keystrokes") {
            bool w = (GetAsyncKeyState('W') & 0x8000) != 0;
            bool a = (GetAsyncKeyState('A') & 0x8000) != 0;
            bool s = (GetAsyncKeyState('S') & 0x8000) != 0;
            bool d = (GetAsyncKeyState('D') & 0x8000) != 0;
            ImGui::TextColored(w ? theme().accent : theme().muted, "W");
            ImGui::SameLine();
            ImGui::TextColored(a ? theme().accent : theme().muted, "A");
            ImGui::SameLine();
            ImGui::TextColored(s ? theme().accent : theme().muted, "S");
            ImGui::SameLine();
            ImGui::TextColored(d ? theme().accent : theme().muted, "D");
        } else if (m.id == "direction") {
            auto snap = aml::tracker::store().snapshot();
            float yaw = snap.yaw;
            while (yaw < 0) yaw += 360.0f;
            while (yaw >= 360) yaw -= 360.0f;
            const char* dir = "S";
            if (yaw >= 337.5f || yaw < 22.5f) dir = "S";
            else if (yaw < 67.5f) dir = "SW";
            else if (yaw < 112.5f) dir = "W";
            else if (yaw < 157.5f) dir = "NW";
            else if (yaw < 202.5f) dir = "N";
            else if (yaw < 247.5f) dir = "NE";
            else if (yaw < 292.5f) dir = "E";
            else dir = "SE";
            char buf[32];
            snprintf(buf, sizeof(buf), "Dir: %s (%.0f)", dir, yaw);
            ImGui::Text("%s", buf);
        } else if (m.id == "speed") {
            auto snap = aml::tracker::store().snapshot();
            float spd = std::sqrt(snap.vx * snap.vx + snap.vy * snap.vy + snap.vz * snap.vz);
            char buf[32];
            snprintf(buf, sizeof(buf), "Speed: %.1f m/s", spd);
            ImGui::Text("%s", buf);
        } else if (m.id == "biome") {
            const auto state = aml::player_stats::store().snapshot();
            if (!state.has_data || state.biome[0] == '\0') {
                ImGui::TextDisabled("Biome: unavailable");
            } else {
                ImGui::Text("Biome: %s", state.biome);
            }
        } else if (m.id == "clock") {
            auto now = std::chrono::system_clock::now();
            auto tt = std::chrono::system_clock::to_time_t(now);
            char time_buf[16];
            std::strftime(time_buf, sizeof(time_buf), "%H:%M", std::localtime(&tt));
            ImGui::Text("%s", time_buf);
        } else if (m.id == "player_count") {
            int count = 0;
            for (const auto& e : aml::tracker::store().entities()) {
                if (e.kind == aml::tracker::ENTITY_PLAYER) count++;
            }
            char buf[32];
            snprintf(buf, sizeof(buf), "Players: %d", count);
            ImGui::Text("%s", buf);
        } else {
            ImGui::Text("%s", m.name.c_str());
        }

        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::End();
        ImGui::PopStyleVar(2);
    }
}

void HudManager::render_editor() {
    ImGui::Text("HUD Editor");
    ImGui::Separator();

    static int editor_tab = 0;
    ImGui::RadioButton("Editor Mode", &editor_tab, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Module List", &editor_tab, 1);
    ImGui::SameLine();
    {
        bool prev = preview_mode_;
        ImGui::Checkbox("Preview", &preview_mode_);
        if (preview_mode_ != prev) {}
    }

    ImGui::Spacing();

    if (editor_tab == 0) {
        render_editor_canvas();
    } else {
        render_editor_module_list();
    }
}

void HudManager::render_editor_canvas() {
    ImGui::TextDisabled("Drag modules to reposition. Snap guides appear near edges.");
    ImGui::Spacing();

    ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
    ImVec2 canvas_size = ImGui::GetContentRegionAvail();

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(canvas_pos,
        ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y),
        ImGui::ColorConvertFloat4ToU32(theme().bg), 8.0f);
    draw_list->AddRectFilledMultiColor(
        ImVec2(canvas_pos.x + 1.0f, canvas_pos.y + 1.0f),
        ImVec2(canvas_pos.x + canvas_size.x - 1.0f, canvas_pos.y + canvas_size.y - 1.0f),
        ImGui::ColorConvertFloat4ToU32(ImVec4(theme().header_bg.x, theme().header_bg.y, theme().header_bg.z, 0.42f)),
        ImGui::ColorConvertFloat4ToU32(ImVec4(theme().bg.x, theme().bg.y, theme().bg.z, 0.20f)),
        ImGui::ColorConvertFloat4ToU32(ImVec4(theme().bg.x, theme().bg.y, theme().bg.z, 0.20f)),
        ImGui::ColorConvertFloat4ToU32(ImVec4(theme().header_bg.x, theme().header_bg.y, theme().header_bg.z, 0.42f)));
    draw_list->AddRect(canvas_pos,
        ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y),
        ImGui::ColorConvertFloat4ToU32(theme().border), 8.0f, 0, 1.0f);

    ImGui::InvisibleButton("##canvas", canvas_size);

    float snap_dist = 50.0f;
    float display_w = ImGui::GetIO().DisplaySize.x;
    float display_h = ImGui::GetIO().DisplaySize.y;

    for (float s = snap_dist; s < display_w; s += snap_dist) {
        draw_list->AddLine(ImVec2(canvas_pos.x + s, canvas_pos.y),
                           ImVec2(canvas_pos.x + s, canvas_pos.y + canvas_size.y),
                           IM_COL32(60, 60, 80, 60), 1.0f);
    }
    for (float s = snap_dist; s < display_h; s += snap_dist) {
        draw_list->AddLine(ImVec2(canvas_pos.x, canvas_pos.y + s),
                           ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + s),
                           IM_COL32(60, 60, 80, 60), 1.0f);
    }

    dragging_ = false;
    drag_index_ = -1;

    for (int i = 0; i < static_cast<int>(modules_.size()); ++i) {
        auto& m = modules_[i];

        ImVec2 module_pos(canvas_pos.x + m.x, canvas_pos.y + m.y);

        char label[64];
        snprintf(label, sizeof(label), "== %s", m.name.c_str());

        ImVec2 text_size = ImGui::CalcTextSize(label);
        ImVec2 item_size(text_size.x + 16, text_size.y + 8);

        ImVec2 item_min = module_pos;
        ImVec2 item_max(module_pos.x + item_size.x, module_pos.y + item_size.y);

        draw_list->AddRectFilled(item_min, item_max,
            m.enabled ? ImGui::ColorConvertFloat4ToU32(theme().panel)
                      : ImGui::ColorConvertFloat4ToU32(ImVec4(theme().panel.x * 0.7f, theme().panel.y * 0.7f, theme().panel.z * 0.7f, 0.6f)), 4.0f);
        draw_list->AddRect(item_min, item_max,
            m.enabled ? ImGui::ColorConvertFloat4ToU32(theme().accent)
                      : ImGui::ColorConvertFloat4ToU32(ImVec4(theme().border.x * 0.8f, theme().border.y * 0.8f, theme().border.z * 0.8f, 0.6f)), 4.0f, 0, 1.0f);

        draw_list->AddText(ImVec2(item_min.x + 8, item_min.y + 4),
            m.enabled ? ImGui::ColorConvertFloat4ToU32(theme().text)
                      : ImGui::ColorConvertFloat4ToU32(theme().muted), label);

        char coord_buf[48];
        snprintf(coord_buf, sizeof(coord_buf), "%.0f, %.0f", m.x, m.y);
        draw_list->AddText(ImVec2(item_max.x + 6, item_min.y + 4),
            ImGui::ColorConvertFloat4ToU32(theme().muted), coord_buf);

        ImGui::SetCursorScreenPos(item_min);
        ImGui::InvisibleButton(m.id.c_str(), item_size);

        if (ImGui::IsItemActive()) {
            ImVec2 delta = ImGui::GetIO().MouseDelta;
            m.x += delta.x;
            m.y += delta.y;
            dragging_ = true;
            drag_index_ = i;

            if (m.x < snap_dist) m.x = 0;
            if (m.y < snap_dist) m.y = 0;
            if (fabs(m.x - display_w + item_size.x) < snap_dist)
                m.x = display_w - item_size.x;
            if (fabs(m.y - display_h + item_size.y) < snap_dist)
                m.y = display_h - item_size.y;

            if (m.x < 0) m.x = 0;
            if (m.y < 0) m.y = 0;

            char snap_buf[64];
            snprintf(snap_buf, sizeof(snap_buf), "%s [%.0f, %.0f]",
                m.name.c_str(), m.x, m.y);
            draw_list->AddText(ImVec2(canvas_pos.x + 8, canvas_pos.y + canvas_size.y - 20),
                ImGui::ColorConvertFloat4ToU32(theme().muted), snap_buf);
        }
    }

    ImGui::SetCursorScreenPos(canvas_pos);
}

void HudManager::render_editor_module_list() {
    for (auto& m : modules_) {
        ImGui::PushID(m.id.c_str());
        ImGui::Checkbox("##en", &m.enabled);
        ImGui::SameLine();
        ImGui::Text("%s", m.name.c_str());
        ImGui::SameLine(160);
        ImGui::SetNextItemWidth(80);
        ImGui::DragFloat("##x", &m.x, 1.0f, 0.0f, 4096.0f, "X:%.0f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        ImGui::DragFloat("##y", &m.y, 1.0f, 0.0f, 4096.0f, "Y:%.0f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60);
        ImGui::DragFloat("##s", &m.scale, 0.05f, 0.2f, 4.0f, "%.1fx");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60);
        ImGui::DragFloat("##op", &m.opacity, 0.05f, 0.0f, 1.0f, "%.0%%");
        ImGui::SameLine();
        ImGui::Checkbox("BG", &m.background);
        ImGui::PopID();
    }

    ImGui::Spacing();
    if (ImGui::Button("Reset Layout")) reset_layout();
    ImGui::SameLine();
    if (ImGui::Button("Save Default")) save_layout("default");
}

void HudManager::save_layout(const std::string& name) {
    std::lock_guard<std::mutex> lock(mu_);
    current_layout_ = name;
    std::string path = config_dir_ + "\\hud_" + name + ".cfg";
    std::ofstream f(path, std::ios::trunc);
    if (!f.is_open()) return;
    for (const auto& m : modules_) {
        f << m.id << "=" << (m.enabled ? 1 : 0) << "|"
          << m.x << "|" << m.y << "|" << m.scale << "|"
          << m.opacity << "|" << (m.background ? 1 : 0) << "|"
          << m.text_size << "|" << m.alignment << "\n";
    }
}

void HudManager::load_layout(const std::string& name) {
    std::lock_guard<std::mutex> lock(mu_);
    current_layout_ = name;
    std::string path = config_dir_ + "\\hud_" + name + ".cfg";
    std::ifstream f(path);
    if (!f.is_open()) return;
    std::string line;
    while (std::getline(f, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string id = line.substr(0, eq);
        for (auto& m : modules_) {
            if (m.id == id) {
                int en = 0, bg = 0, al = 0;
                float x = 0, y = 0, s = 1, o = 1, ts = 1;
                sscanf(line.c_str() + eq + 1, "%d|%f|%f|%f|%f|%d|%f|%d",
                       &en, &x, &y, &s, &o, &bg, &ts, &al);
                m.enabled = en != 0;
                m.x = x; m.y = y; m.scale = s; m.opacity = o;
                m.background = bg != 0; m.text_size = ts; m.alignment = al;
                break;
            }
        }
    }
}

void HudManager::reset_layout() {
    std::lock_guard<std::mutex> lock(mu_);
    float y = 10.0f;
    for (auto& m : modules_) {
        m.x = 10.0f;
        m.y = y;
        m.scale = 1.0f;
        m.opacity = 1.0f;
        m.background = false;
        m.text_size = 1.0f;
        m.alignment = 0;
        y += 20.0f;
    }
}

std::vector<std::string> HudManager::get_presets() const {
    return {"Minimal", "PvP", "Survival", "Performance", "Streaming", "Custom"};
}

}  // namespace aml::client
