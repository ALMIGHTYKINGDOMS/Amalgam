#include "ui.h"
#include "ui_internal.h"
#include "services.h"
#include "server_types.h"
#include "server_providers.h"
#include "version_catalog.h"
#include "essentials_address.h"
#include "config.h"
#include "json.h"
#include "net.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commdlg.h>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <mutex>

namespace aml::ui {

// ---------------------------------------------------------------------------
// Local server persistence
// ---------------------------------------------------------------------------

static std::wstring servers_json_path() {
    return aml::net::get_local_app_data_path() + L"\\amalgam\\servers.json";
}

static void save_local_servers(const std::vector<server::ServerConfig>& servers) {
    Json arr = Json::arr();
    for (const auto& s : servers) {
        Json obj = Json::obj();
        obj["name"] = s.name;
        obj["software"] = static_cast<int>(s.software);
        obj["minecraft_version"] = s.minecraft_version;
        obj["allocated_ram_mb"] = s.allocated_ram_mb;
        obj["max_players"] = s.max_players;
        obj["port"] = s.port;
        obj["server_directory"] = s.server_directory;
        obj["stage"] = static_cast<int>(s.stage);
        arr.push(obj);
    }
    std::string err;
    aml::json_write_file(servers_json_path(), arr, &err);
}

static void load_local_servers(std::vector<server::ServerConfig>& servers) {
    Json root;
    std::string err;
    if (!aml::json_parse_file(servers_json_path(), root, &err) || !root.isArray())
        return;
    servers.clear();
    for (size_t i = 0; i < root.size(); ++i) {
        const Json& j = root[i];
        server::ServerConfig s;
        s.name = j.get("name").as_str();
        s.software = static_cast<server::ServerSoftware>(j.get("software").as_int());
        s.minecraft_version = j.get("minecraft_version").as_str("1.20.1");
        s.allocated_ram_mb = static_cast<int>(j.get("allocated_ram_mb").as_int(4096));
        s.max_players = static_cast<int>(j.get("max_players").as_int(20));
        s.port = static_cast<int>(j.get("port").as_int(25565));
        s.server_directory = j.get("server_directory").as_str();
        s.stage = static_cast<server::ServerStage>(j.get("stage").as_int());
        servers.push_back(std::move(s));
    }
}

// Local run state belongs to the process the service is supervising; the saved
// list only records a hint. Every read of "is this server running" asks here.
static bool local_server_supervised(const server::ServerConfig& sv) {
    auto* svc = aml::services::ServiceManager::instance().servers();
    return svc && svc->is_local_server_running(sv.name);
}

static server::ServerStage effective_stage(const UiState& st,
                                           const server::ServerConfig& sv) {
    // Visual-review fixtures seed a running server with no process on purpose.
    if (st.fixture_mode) return sv.stage;
    return server::reconciled_stage(sv, local_server_supervised(sv));
}

// Correct a run stage no supervised process backs, so the saved list stops
// claiming a server is up after a restart.
static void reconcile_local_server_stages(std::vector<server::ServerConfig>& servers) {
    bool changed = false;
    for (auto& sv : servers) {
        const server::ServerStage stage =
            server::reconciled_stage(sv, local_server_supervised(sv));
        if (stage != sv.stage) {
            sv.stage = stage;
            changed = true;
        }
    }
    if (changed) save_local_servers(servers);
}

// ---------------------------------------------------------------------------
// UI state singleton (codebase pattern)
// ---------------------------------------------------------------------------

struct ServerUIState {
    bool loaded = false;
    bool create_open = false;
    int selected = -1;
    int action_pending = -1;
    int mode = 0;  // 0 = Host, 1 = Connect

    // Create dialog
    std::string create_name;
    std::string create_version = version_catalog::default_server_version();
    int create_software_idx = 0;
    int create_ram = 4096;
    int create_max_players = 20;
    int create_port = 25565;
    bool create_eula_accepted = false;

    // Filters
    int filter_status = 0;
    int filter_software = 0;
    int sort_mode = 0;

    // Console (popup legacy)
    bool console_open = false;
    int console_server_idx = -1;
    std::string console_filter;
    std::vector<server::ServerConsoleEntry> console_log;
    std::string console_input;
    bool console_auto_scroll = true;

    // Properties (popup legacy)
    bool properties_open = false;
    int properties_server_idx = -1;
    std::vector<std::pair<std::string, std::string>> properties;
    bool properties_dirty = false;

    // ── Server detail view ────────────────────────────────────────
    // When detail_server_idx >= 0 we show the full-panel detail page
    // for the Host server at that index.  detail_connect_idx handles
    // the Connect address-book row instead.
    int detail_server_idx = -1;   // index into UiState::servers
    int detail_connect_idx = -1;  // index into UiState::cfg->servers
    int detail_tab = 0;           // 0=Overview 1=Console 2=Files 3=Players 4=Plugins 5=Properties 6=World

    // Detail – file browser
    std::string file_browse_path;          // current directory (empty = server root)
    std::vector<std::string> file_entries; // filenames in current dir
    std::vector<bool> file_is_dir;
    bool file_list_dirty = true;
    std::string file_preview_name;
    std::string file_preview_content;
    bool file_preview_open = false;
    bool file_delete_confirm = false;
    std::string file_delete_target;

    // Detail – players
    std::vector<server::ServerPlayer> detail_players;
    std::vector<std::string> whitelist_entries;
    std::vector<std::string> ops_entries;
    bool players_dirty = true;
    std::string player_op_input;
    std::string player_whitelist_input;

    // Detail – world
    uint64_t world_size_bytes = 0;
    std::string world_name;
    bool world_dirty = true;

    // Detail – console (embedded)
    std::string detail_console_filter;
    std::string detail_console_input;
    bool detail_console_auto_scroll = true;
    double detail_console_last_refresh = 0.0;
};

static ServerUIState& state() {
    static ServerUIState s;
    return s;
}

// ---------------------------------------------------------------------------
// Software constants
// ---------------------------------------------------------------------------

static const char* kSoftwareNames[] = {
    "Vanilla (manual)", "Paper", "Purpur", "Spigot (manual)", "Fabric", "Quilt", "Folia"
};
static const server::ServerSoftware kSoftwareValues[] = {
    server::ServerSoftware::Vanilla,
    server::ServerSoftware::Paper,
    server::ServerSoftware::Purpur,
    server::ServerSoftware::Spigot,
    server::ServerSoftware::Fabric,
    server::ServerSoftware::Quilt,
    server::ServerSoftware::Folia
};
static const int kSoftwareCount = 7;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static const char* stage_label(server::ServerStage stage) {
    switch (stage) {
        case server::ServerStage::NotInstalled: return "Not Installed";
        case server::ServerStage::Installing:   return "Installing";
        case server::ServerStage::Ready:        return "Ready";
        case server::ServerStage::Running:      return "Running";
        case server::ServerStage::Stopped:      return "Stopped";
        case server::ServerStage::Error:        return "Error";
        case server::ServerStage::Crashed:      return "Crashed";
    }
    return "Unknown";
}

static ImVec4 stage_color(server::ServerStage stage) {
    switch (stage) {
        case server::ServerStage::Running:      return k.green;
        case server::ServerStage::Ready:        return k.blue;
        case server::ServerStage::Installing:   return k.yellow;
        case server::ServerStage::Stopped:      return k.muted;
        case server::ServerStage::Error:        return k.red;
        case server::ServerStage::Crashed:      return {1.0f, 0.2f, 0.2f, 1.0f};
        case server::ServerStage::NotInstalled: return k.muted;
    }
    return k.muted;
}

static ImVec4 stage_bg_color(server::ServerStage stage) {
    ImVec4 c = stage_color(stage);
    c.w = 0.15f;
    return c;
}

static const char* software_filter_name(int idx) {
    switch (idx) {
        case 0: return "All Software";
        case 1: return "Vanilla";
        case 2: return "Paper";
        case 3: return "Purpur";
        case 4: return "Spigot";
        case 5: return "Fabric";
        case 6: return "Quilt";
        case 7: return "Folia";
    }
    return "All";
}

static const char* status_filter_name(int idx) {
    switch (idx) {
        case 0: return "All Status";
        case 1: return "Running";
        case 2: return "Ready";
        case 3: return "Stopped";
        case 4: return "Error";
    }
    return "All";
}

static bool matches_filters(const UiState& st, const server::ServerConfig& sv,
                            int filter_status, int filter_software) {
    if (filter_status > 0) {
        const server::ServerStage stage = effective_stage(st, sv);
        server::ServerStage target;
        switch (filter_status) {
            case 1: target = server::ServerStage::Running; break;
            case 2: target = server::ServerStage::Ready; break;
            case 3: target = server::ServerStage::Stopped; break;
            case 4: target = server::ServerStage::Error; break;
            default: target = stage;
        }
        if (stage != target) return false;
    }
    if (filter_software > 0) {
        int sw_idx = filter_software - 1;
        if (sw_idx < kSoftwareCount && sv.software != kSoftwareValues[sw_idx])
            return false;
    }
    return true;
}

static const char* sort_label(int mode) {
    switch (mode) {
        case 0: return "Name";
        case 1: return "Status";
        case 2: return "Version";
        case 3: return "Port";
    }
    return "Name";
}

static std::string server_properties_path(const std::string& dir) {
    return dir + "\\server.properties";
}

static void load_server_properties(const std::string& dir,
                                   std::vector<std::pair<std::string, std::string>>& props) {
    props.clear();
    std::string path = server_properties_path(dir);
    FILE* f = nullptr;
    fopen_s(&f, path.c_str(), "r");
    if (!f) return;
    char buf[1024];
    while (fgets(buf, sizeof(buf), f)) {
        std::string line(buf);
        if (!line.empty() && line.back() == '\n') line.pop_back();
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq != std::string::npos) {
            props.emplace_back(line.substr(0, eq), line.substr(eq + 1));
        }
    }
    fclose(f);
}

static void save_server_properties(const std::string& dir,
                                   const std::vector<std::pair<std::string, std::string>>& props) {
    std::string path = server_properties_path(dir);
    FILE* f = nullptr;
    fopen_s(&f, path.c_str(), "w");
    if (!f) return;
    fprintf(f, "# Server properties generated by Amalgam\n");
    for (const auto& kv : props) {
        fprintf(f, "%s=%s\n", kv.first.c_str(), kv.second.c_str());
    }
    fclose(f);
}

// ---------------------------------------------------------------------------
// Runtime provisioning
// ---------------------------------------------------------------------------

// Software whose server runtime Amalgam can download and verify automatically.
// Vanilla is resolved from Mojang's live version manifest. Spigot is still
// manual because it is built with BuildTools rather than published as a
// direct jar. Everything else is auto-provisioned from provider meta APIs.
static bool is_auto_provisioned(server::ServerSoftware software) {
    switch (software) {
        case server::ServerSoftware::Vanilla:
        case server::ServerSoftware::Paper:
        case server::ServerSoftware::Purpur:
        case server::ServerSoftware::Folia:
        case server::ServerSoftware::Fabric:
        case server::ServerSoftware::Quilt:
            return true;
        default:
            return false;
    }
}

// Downloads and verifies the server runtime jar into <dir>/server.jar.
// Returns false (with err set) when the runtime cannot be provisioned.
static bool provision_server_runtime(const server::ServerConfig& cfg, std::string* err) {
    namespace sp = aml::server_providers;
    const std::string& version = cfg.minecraft_version;
    const std::wstring jar_path =
        aml::net::to_wide(cfg.server_directory) + L"\\server.jar";

    std::string url;
    std::string sha256;

    switch (cfg.software) {
        case server::ServerSoftware::Vanilla: {
            std::string sha1;
            int64_t size = -1;
            if (!sp::vanilla_server_runtime(version, url, sha1, size, err)) return false;
            // Mojang publishes SHA-1 for the vanilla server artifact, while
            // third-party providers generally publish SHA-256.
            return net::download(net::to_wide(url), jar_path, nullptr, err, sha1, size);
        }
        case server::ServerSoftware::Paper: {
            sp::BuildInfo build;
            if (!sp::paper_latest_stable(version, build, err)) return false;
            url = build.download_url;
            sha256 = build.sha256;
            break;
        }
        case server::ServerSoftware::Folia: {
            sp::BuildInfo build;
            if (!sp::folia_latest_stable(version, build, err)) return false;
            url = build.download_url;
            sha256 = build.sha256;
            break;
        }
        case server::ServerSoftware::Purpur: {
            std::vector<sp::BuildInfo> builds;
            if (!sp::purpur_builds(version, builds, err)) return false;
            if (builds.empty()) {
                if (err) *err = "No Purpur builds for " + version;
                return false;
            }
            url = builds.back().download_url;  // newest-first
            break;
        }
        case server::ServerSoftware::Fabric: {
            std::vector<std::string> loaders;
            std::vector<sp::GameVersion> installers;
            if (!sp::fabric_loader_versions(loaders, err)) return false;
            if (!sp::fabric_installer_versions(installers, err)) return false;
            if (loaders.empty() || installers.empty()) {
                if (err) *err = "No Fabric loader/installer for " + version;
                return false;
            }
            url = sp::fabric_server_jar_url(version, loaders.front(),
                                            installers.front().version);
            break;
        }
        case server::ServerSoftware::Quilt: {
            // Quilt's loader endpoint returns loader+installer pairs in one
            // response, so a single fetch provides both parts of the URL.
            std::vector<sp::QuiltLoaderPair> pairs;
            if (!sp::quilt_loader_installer_pairs(pairs, err)) return false;
            if (pairs.empty()) {
                if (err) *err = "No Quilt loader/installer for " + version;
                return false;
            }
            url = sp::quilt_server_jar_url(version, pairs.front().loader,
                                           pairs.front().installer);
            break;
        }
        default:
            if (err) *err = "This software requires manual installation.";
            return false;
    }

    if (url.empty()) {
        if (err) *err = "No download available for this software.";
        return false;
    }
    return sp::download_runtime(url, sha256, jar_path, err);
}

// ---------------------------------------------------------------------------
// Draw helpers
// ---------------------------------------------------------------------------

static void draw_status_badge(server::ServerStage stage) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec4 sc = stage_color(stage);
    ImVec4 bg = stage_bg_color(stage);
    draw_badge(dl, pos, stage_label(stage), sc, bg);
    ImGui::Dummy(ImVec2(
        ImGui::CalcTextSize(stage_label(stage)).x + ui_px(14.0f),
        ui_px(20.0f)));
}

static void draw_server_card(server::ServerConfig& sv, int index, UiState& st) {
    auto& s = state();
    ImGui::PushID(index);

    const server::ServerStage stage = effective_stage(st, sv);
    bool is_running = (stage == server::ServerStage::Running);
    bool is_ready = (stage == server::ServerStage::Ready ||
                     stage == server::ServerStage::Stopped ||
                     stage == server::ServerStage::NotInstalled);

    // ── Card with left status strip ──────────────────────────────────
    card_begin(("##srv_" + std::to_string(index)).c_str(), ImVec2(-1, 0));

    // Left status strip
    ImVec2 card_min = ImGui::GetCursorScreenPos();
    ImVec2 card_content_max = ImGui::GetContentRegionMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec4 strip_color = stage_color(stage);
    dl->AddRectFilled(
        card_min,
        ImVec2(card_min.x + ui_px(4.0f), card_min.y + ui_px(76.0f)),
        c32(strip_color), ui_px(2.0f));

    // A real visual anchor makes the server list scan like the launcher board
    // instead of a settings form. Per-server artwork can replace this shared
    // launcher scene later without changing the layout contract.
    const float thumbnail_size = ui_px(72.0f);
    const ImVec2 thumbnail_pos = card_min + ImVec2(ui_px(12.0f), 0.0f);
    draw_local_image(st, st.exe_dir + L"\\branding\\ai\\server-card-ai-v2.png",
                     thumbnail_pos, ImVec2(thumbnail_size, thumbnail_size),
                     c32(k.brand_dk));
    dl->AddRect(thumbnail_pos, thumbnail_pos + ImVec2(thumbnail_size, thumbnail_size),
                c32(k.border), ui_px(8.0f));
    ImGui::SetCursorScreenPos(
        ImVec2(thumbnail_pos.x + thumbnail_size + ui_px(14.0f), thumbnail_pos.y));

    // ── Header row: name + badges (clickable to open detail) ────────
    ImGui::PushFont(f_bold);
    ImGui::TextColored(k.text, "%s", sv.name.c_str());
    ImGui::PopFont();
    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    if (ImGui::IsItemClicked()) {
        s.detail_server_idx = index;
        s.detail_tab = 0;
        s.file_list_dirty = true;
        s.players_dirty = true;
        s.world_dirty = true;
    }

    ImGui::SameLine(0, ui_px(8.0f));
    draw_status_badge(stage);

    ImGui::SameLine(0, ui_px(6.0f));
    {
        ImDrawList* dl2 = ImGui::GetWindowDrawList();
        ImVec2 bp = ImGui::GetCursorScreenPos();
        ImVec4 sw_bg = k.brand;
        sw_bg.w = 0.15f;
        draw_badge(dl2, bp, server::server_software_name(sv.software), k.brand, sw_bg);
        ImGui::Dummy(ImVec2(
            ImGui::CalcTextSize(server::server_software_name(sv.software)).x + ui_px(14.0f),
            ui_px(20.0f)));
    }

    ImGui::SameLine(0, ui_px(6.0f));
    {
        ImDrawList* dl3 = ImGui::GetWindowDrawList();
        ImVec2 bp = ImGui::GetCursorScreenPos();
        ImVec4 ver_bg = k.blue;
        ver_bg.w = 0.12f;
        draw_badge(dl3, bp, sv.minecraft_version.c_str(), k.blue, ver_bg);
        ImGui::Dummy(ImVec2(
            ImGui::CalcTextSize(sv.minecraft_version.c_str()).x + ui_px(14.0f),
            ui_px(20.0f)));
    }

    // ── Info row (stat chips) ───────────────────────────────────────
    // Pin metadata to the free space beside the thumbnail. Previously this
    // depended on Dear ImGui's SameLine wrapping and could jump beneath the
    // image at compact sizes, making the server cards look broken.
    ImGui::SetCursorScreenPos(ImVec2(thumbnail_pos.x + thumbnail_size + ui_px(14.0f),
                                     thumbnail_pos.y + ui_px(34.0f)));
    auto meta_chip = [&](const char* text, const ImVec4& accent) {
        const ImVec2 bp = ImGui::GetCursorScreenPos();
        const ImVec2 sz = ImGui::CalcTextSize(text) + ImVec2(ui_px(14.0f), ui_px(8.0f));
        ImVec4 bg = accent; bg.w = 0.12f;
        dl->AddRectFilled(bp, bp + sz, c32(bg), ui_px(6.0f));
        dl->AddText(bp + ImVec2(ui_px(7.0f), ui_px(4.0f)), c32(accent), text);
        ImGui::Dummy(sz + ImVec2(0, ui_px(4.0f)));
    };
    std::string port_chip = "Port " + std::to_string(sv.port);
    meta_chip(port_chip.c_str(), k.muted);
    ImGui::SameLine(0, ui_px(6.0f));
    std::string ram_chip = "RAM " + std::to_string(sv.allocated_ram_mb) + " MB";
    meta_chip(ram_chip.c_str(), k.blue);
    ImGui::SameLine(0, ui_px(6.0f));
    std::string max_chip = "Max " + std::to_string(sv.max_players) + " players";
    meta_chip(max_chip.c_str(), k.green);

    if (!is_running && !sv.status_message.empty()) {
        ImGui::Spacing();
        const ImVec4 sc = (stage == server::ServerStage::Error) ? k.red : k.muted;
        ImGui::TextColored(sc, "%s", sv.status_message.c_str());
    }

    // Return subsequent full-width rows to the content origin and keep them
    // below the thumbnail, even when metadata wraps at a compact width.
    {
        const float content_bottom = ImGui::GetCursorScreenPos().y;
        ImGui::SetCursorScreenPos(ImVec2(card_min.x,
            std::max(content_bottom, thumbnail_pos.y + thumbnail_size + ui_px(4.0f))));
    }

    // ── Running metrics ─────────────────────────────────────────────
    if (is_running) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextColored(k.green, "\xe2\x97\x8f");
        ImGui::SameLine();
        ImGui::TextColored(k.green, "Online");
        if (!sv.status_message.empty()) {
            ImGui::SameLine(0, ui_px(12.0f));
            ImGui::TextColored(k.muted, "%s", sv.status_message.c_str());
        }

        // Inline metric bars
        auto& metrics = st.server_metrics;
        if (metrics.valid) {
            ImGui::Spacing();
            ImVec2 bar_pos = ImGui::GetCursorScreenPos();
            float bar_w = ImGui::GetContentRegionAvail().x;
            float bar_h = ui_px(5.0f);
            float bar_y = bar_pos.y;
            ImDrawList* metrics_dl = ImGui::GetWindowDrawList();

            // RAM bar
            float ram_pct = std::clamp(metrics.ram_percent / 100.0f, 0.0f, 1.0f);
            ImVec4 ram_col = ram_pct > 0.9f ? k.red : ram_pct > 0.7f ? k.orange : k.blue;
            metrics_dl->AddRectFilled(ImVec2(bar_pos.x, bar_y), ImVec2(bar_pos.x + bar_w, bar_y + bar_h), c32(k.surface2), bar_h * 0.5f);
            metrics_dl->AddRectFilled(ImVec2(bar_pos.x, bar_y), ImVec2(bar_pos.x + bar_w * ram_pct, bar_y + bar_h), c32(ram_col), bar_h * 0.5f);
            ImGui::PushFont(f_small);
            metrics_dl->AddText(ImVec2(bar_pos.x, bar_y + bar_h + ui_px(2.0f)), c32(k.muted), ("RAM " + std::to_string((int)metrics.ram_percent) + "%").c_str());
            ImGui::PopFont();

            // CPU bar
            float cpu_pct = std::clamp(metrics.cpu_percent / 100.0f, 0.0f, 1.0f);
            ImVec4 cpu_col = cpu_pct > 0.9f ? k.red : cpu_pct > 0.7f ? k.orange : k.green;
            float cpu_y = bar_y + bar_h + ui_px(16.0f);
            metrics_dl->AddRectFilled(ImVec2(bar_pos.x, cpu_y), ImVec2(bar_pos.x + bar_w, cpu_y + bar_h), c32(k.surface2), bar_h * 0.5f);
            metrics_dl->AddRectFilled(ImVec2(bar_pos.x, cpu_y), ImVec2(bar_pos.x + bar_w * cpu_pct, cpu_y + bar_h), c32(cpu_col), bar_h * 0.5f);
            ImGui::PushFont(f_small);
            metrics_dl->AddText(ImVec2(bar_pos.x, cpu_y + bar_h + ui_px(2.0f)), c32(k.muted), ("CPU " + std::to_string((int)metrics.cpu_percent) + "%").c_str());
            ImGui::PopFont();

            // TPS + Players row
            ImVec4 tps_color = metrics.tps >= 19.0f ? k.green : metrics.tps >= 15.0f ? k.yellow : k.red;
            float row3_y = cpu_y + bar_h + ui_px(16.0f);
            ImGui::PushFont(f_small);
            metrics_dl->AddText(ImVec2(bar_pos.x, row3_y), c32(tps_color), ("TPS " + std::to_string(metrics.tps)).c_str());
            metrics_dl->AddText(ImVec2(bar_pos.x + bar_w * 0.5f, row3_y), c32(k.muted), (std::to_string(metrics.players_online) + "/" + std::to_string(sv.max_players) + " online").c_str());
            ImGui::PopFont();
            ImGui::Dummy(ImVec2(0, row3_y - bar_pos.y + ui_px(20.0f)));
        }
    }

    // ── Action buttons ──────────────────────────────────────────────
    // Keep the command row visually attached to the server identity. Full
    // default Spacing calls left stopped servers looking like unfinished,
    // stretched rows at desktop widths.
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ui_px(3.0f));
    ImGui::Separator();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ui_px(3.0f));

    float bw = ui_px(80.0f);
    float bh = ui_px(28.0f);

    if (is_running) {
        if (ghost_button("Stop", ImVec2(bw, bh))) {
            auto* svc = aml::services::ServiceManager::instance().servers();
            std::string error;
            const bool ok = svc && svc->stop_local_server(sv.name, &error);
            if (ok) {
                sv.stage = server::ServerStage::Stopped;
                save_local_servers(st.servers);
            } else {
                push_notice(st, ui_model::NoticeLevel::Error, "Stop Failed",
                            error.empty() ? "The local server could not be stopped." : error);
            }
        }
        ImGui::SameLine(0, ui_px(4.0f));
        if (ghost_button("Restart", ImVec2(bw + ui_px(10.0f), bh))) {
            auto* svc = aml::services::ServiceManager::instance().servers();
            std::string error;
            const bool stopped = svc && svc->stop_local_server(sv.name, &error);
            const bool started = stopped && svc->start_local_server(sv.name, "", sv.allocated_ram_mb, &error);
            if (started) {
                sv.stage = server::ServerStage::Running;
                save_local_servers(st.servers);
            } else {
                push_notice(st, ui_model::NoticeLevel::Error, "Restart Failed",
                            error.empty() ? "The local server could not be restarted." : error);
            }
        }
    } else if (is_ready) {
        if (primary_button("Start", ImVec2(bw, bh))) {
            auto* svc = aml::services::ServiceManager::instance().servers();
            std::string error;
            const bool ok = svc && svc->start_local_server(sv.name, "", sv.allocated_ram_mb, &error);
            if (ok) {
                sv.stage = server::ServerStage::Running;
                save_local_servers(st.servers);
            } else {
                push_notice(st, ui_model::NoticeLevel::Error, "Start Failed",
                            error.empty() ? "The local server could not be started." : error);
            }
        }
    }

    // Manage button always visible
    ImGui::SameLine(0, ui_px(4.0f));
    if (ghost_button("Manage", ImVec2(bw + ui_px(20.0f), bh))) {
        s.detail_server_idx = index;
        s.detail_tab = 0;
        s.file_list_dirty = true;
        s.players_dirty = true;
        s.world_dirty = true;
    }

    if (!sv.server_directory.empty()) {
        ImGui::SameLine(0, ui_px(4.0f));
        if (ghost_button("Folder", ImVec2(bw, bh))) {
            ShellExecuteW(nullptr, L"open",
                aml::net::to_wide(sv.server_directory).c_str(),
                nullptr, nullptr, SW_SHOWNORMAL);
        }
    }

    ImGui::SameLine(0, ui_px(4.0f));
    if (ghost_button("Delete", ImVec2(bw, bh))) {
        s.action_pending = index;
        ImGui::OpenPopup("Confirm Delete");
    }

    card_end();
    ImGui::PopID();
}

// ---------------------------------------------------------------------------
// Console viewer
// ---------------------------------------------------------------------------

static void draw_console_panel(UiState& st) {
    auto& s = state();
    if (!s.console_open || s.console_server_idx < 0 ||
        s.console_server_idx >= static_cast<int>(st.servers.size())) {
        return;
    }

    auto& sv = st.servers[s.console_server_idx];

    ImGui::SetNextWindowSize(ImVec2(ui_px(600), ui_px(400)), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::Begin("Server Console", &s.console_open,
                     ImGuiWindowFlags_NoSavedSettings)) {
        // Header
        ImGui::PushFont(f_h2);
        ImGui::TextColored(k.text, "Console: %s", sv.name.c_str());
        ImGui::PopFont();
        ImGui::SameLine(0, ui_px(12.0f));
        draw_status_badge(effective_stage(st, sv));

        ImGui::Spacing();

        // Filter
        ImGui::SetNextItemWidth(ui_px(200.0f));
        input_text_hint("##console_filter", "Filter logs...", &s.console_filter);

        ImGui::SameLine(0, ui_px(8.0f));
        if (ghost_button("Copy", ImVec2(ui_px(60.0f), ui_px(24.0f)))) {
            std::string copy_text;
            for (const auto& entry : s.console_log)
                copy_text += "[" + entry.timestamp + "] " + entry.message + "\n";
            if (!copy_text.empty()) ImGui::SetClipboardText(copy_text.c_str());
        }
        ImGui::SameLine(0, ui_px(4.0f));
        if (ghost_button("Clear", ImVec2(ui_px(60.0f), ui_px(24.0f)))) {
            s.console_log.clear();
        }
        ImGui::SameLine(0, ui_px(4.0f));
        if (ghost_button(s.console_auto_scroll ? "Auto-scroll ON" : "Auto-scroll OFF",
                         ImVec2(ui_px(110.0f), ui_px(24.0f)))) {
            s.console_auto_scroll = !s.console_auto_scroll;
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Log area
        float footer_h = ImGui::GetFrameHeightWithSpacing() + ui_px(4.0f);
        if (ImGui::BeginChild("##console_log",
                ImVec2(-1, -(footer_h)), ImGuiChildFlags_Borders)) {
            for (const auto& entry : s.console_log) {
                if (!s.console_filter.empty() &&
                    entry.message.find(s.console_filter) == std::string::npos)
                    continue;

                // Color code by level
                ImVec4 log_color = k.muted;
                if (entry.message.find("ERROR") != std::string::npos ||
                    entry.message.find("SEVERE") != std::string::npos)
                    log_color = k.red;
                else if (entry.message.find("WARN") != std::string::npos)
                    log_color = k.yellow;
                else if (entry.message.find("INFO") != std::string::npos)
                    log_color = k.green;

                ImGui::TextColored(k.muted, "[%s]", entry.timestamp.c_str());
                ImGui::SameLine();
                ImGui::TextColored(log_color, "%s", entry.message.c_str());
            }

            if (s.console_auto_scroll &&
                ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
                ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();

        ImGui::Spacing();

        // Command input
        ImGui::SetNextItemWidth(-ui_px(80.0f) - ui_px(4.0f));
        if (ImGui::InputText("##console_cmd", &s.console_input,
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            if (!s.console_input.empty()) {
                // Echo the command
                server::ServerConsoleEntry cmd_entry;
                cmd_entry.timestamp = ">";
                cmd_entry.message = s.console_input;
                s.console_log.push_back(cmd_entry);

                // Send to server
                auto* svc = aml::services::ServiceManager::instance().servers();
                if (svc) {
                    std::string response;
                    svc->send_command(sv.name, s.console_input, &response);
                    if (!response.empty()) {
                        server::ServerConsoleEntry resp_entry;
                        resp_entry.timestamp = "<";
                        resp_entry.message = response;
                        s.console_log.push_back(resp_entry);
                    }
                }
                s.console_input.clear();
            }
            ImGui::SetKeyboardFocusHere(-1);
        }
        ImGui::SameLine();
        if (primary_button("Send", ImVec2(ui_px(76.0f), ImGui::GetFrameHeight()))) {
            if (!s.console_input.empty()) {
                server::ServerConsoleEntry cmd_entry;
                cmd_entry.timestamp = ">";
                cmd_entry.message = s.console_input;
                s.console_log.push_back(cmd_entry);

                auto* svc = aml::services::ServiceManager::instance().servers();
                if (svc) {
                    std::string response;
                    svc->send_command(sv.name, s.console_input, &response);
                    if (!response.empty()) {
                        server::ServerConsoleEntry resp_entry;
                        resp_entry.timestamp = "<";
                        resp_entry.message = response;
                        s.console_log.push_back(resp_entry);
                    }
                }
                s.console_input.clear();
            }
        }
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Properties editor
// ---------------------------------------------------------------------------

static void draw_properties_panel(UiState& st) {
    auto& s = state();
    if (!s.properties_open || s.properties_server_idx < 0 ||
        s.properties_server_idx >= static_cast<int>(st.servers.size())) {
        return;
    }

    auto& sv = st.servers[s.properties_server_idx];

    ImGui::SetNextWindowSize(ImVec2(ui_px(500), ui_px(450)), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::Begin("Server Properties", &s.properties_open,
                     ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::PushFont(f_h2);
        ImGui::TextColored(k.text, "Properties: %s", sv.name.c_str());
        ImGui::PopFont();

        ImGui::Spacing();

        if (s.properties.empty()) {
            empty_state("No server.properties found",
                        "Start the server once to generate the properties file.");
        } else {
            ImGui::TextColored(k.muted, "%d properties", (int)s.properties.size());
            ImGui::Spacing();

            float footer_h = ImGui::GetFrameHeightWithSpacing() + ui_px(8.0f);
            if (ImGui::BeginChild("##props_list",
                    ImVec2(-1, -(footer_h)), ImGuiChildFlags_Borders)) {
                for (size_t i = 0; i < s.properties.size(); ++i) {
                    auto& [key, val] = s.properties[i];
                    ImGui::PushID(static_cast<int>(i));

                    ImGui::SetNextItemWidth(ui_px(180.0f));
                    ImGui::TextColored(k.brand, "%s", key.c_str());
                    ImGui::SameLine(ui_px(184.0f));
                    ImGui::SetNextItemWidth(-1);

                    // Special handling for known boolean properties
                    bool is_bool = (key == "online-mode" || key == "pvp" ||
                                    key == "enable-command-block" || key == "spawn-monsters" ||
                                    key == "spawn-animals" || key == "spawn-npcs" ||
                                    key == "white-list" || key == "spawn-protection" &&
                                    val == "0" || val == "1" ||
                                    key == "enforce-whitelist");
                    if (is_bool) {
                        bool bval = (val == "true");
                        if (ImGui::Checkbox("##val", &bval)) {
                            val = bval ? "true" : "false";
                            s.properties_dirty = true;
                        }
                    } else {
                        char buf[512];
                        strncpy(buf, val.c_str(), sizeof(buf) - 1);
                        buf[sizeof(buf) - 1] = '\0';
                        if (ImGui::InputText("##val", buf, sizeof(buf))) {
                            val = buf;
                            s.properties_dirty = true;
                        }
                    }

                    ImGui::PopID();
                }
            }
            ImGui::EndChild();

            ImGui::Spacing();
            if (s.properties_dirty) {
                if (primary_button("Save Properties",
                                   ImVec2(ui_px(130.0f), ui_px(30.0f)))) {
                    save_server_properties(sv.server_directory, s.properties);
                    s.properties_dirty = false;
                }
                ImGui::SameLine();
            }
            if (ghost_button("Reload", ImVec2(ui_px(80.0f), ui_px(30.0f)))) {
                load_server_properties(sv.server_directory, s.properties);
                s.properties_dirty = false;
            }
        }
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Server file browser helpers
// ---------------------------------------------------------------------------

static void scan_server_directory(ServerUIState& s, const std::string& dir) {
    s.file_entries.clear();
    s.file_is_dir.clear();
    namespace fs = std::filesystem;
    std::error_code ec;
    std::string full = dir;
    if (!s.file_browse_path.empty()) {
        full += "\\" + s.file_browse_path;
    }
    for (auto& entry : fs::directory_iterator(aml::net::to_wide(full), ec)) {
        const auto name_u8 = entry.path().filename().u8string();
        s.file_entries.emplace_back(reinterpret_cast<const char*>(name_u8.c_str()));
        std::error_code tec;
        s.file_is_dir.push_back(entry.is_directory(tec));
    }
    // Sort: directories first, then alphabetical
    for (size_t i = 0; i < s.file_entries.size(); ++i) {
        for (size_t j = i + 1; j < s.file_entries.size(); ++j) {
            bool swap_needed = false;
            if (s.file_is_dir[i] && !s.file_is_dir[j]) continue;
            if (!s.file_is_dir[i] && s.file_is_dir[j]) swap_needed = true;
            else if (s.file_entries[i] > s.file_entries[j]) swap_needed = true;
            if (swap_needed) {
                std::string tmp_name = s.file_entries[i];
                s.file_entries[i] = s.file_entries[j];
                s.file_entries[j] = tmp_name;
                bool tmp_dir = s.file_is_dir[i];
                s.file_is_dir[i] = s.file_is_dir[j];
                s.file_is_dir[j] = tmp_dir;
            }
        }
    }
}

static std::string file_full_path(ServerUIState& s, const std::string& base_dir,
                                   const std::string& filename) {
    std::string path = base_dir;
    if (!s.file_browse_path.empty()) path += "\\" + s.file_browse_path;
    if (!filename.empty()) path += "\\" + filename;
    return path;
}

// Read a text file into a string (truncated to 64 KB for display).
static std::string read_text_file(const std::string& path, size_t max_bytes = 65536) {
    FILE* f = nullptr;
    fopen_s(&f, path.c_str(), "rb");
    if (!f) return "(unable to open file)";
    std::string out;
    out.resize(max_bytes);
    size_t n = fread(out.data(), 1, max_bytes, f);
    fclose(f);
    out.resize(n);
    return out;
}

// Format file size nicely.
static std::string file_size_label(uint64_t bytes) {
    if (bytes < 1024) return std::to_string(bytes) + " B";
    if (bytes < 1024 * 1024)
        return std::to_string(bytes / 1024) + "." +
               std::to_string((bytes % 1024) / 102) + " KB";
    if (bytes < 1024ULL * 1024 * 1024)
        return std::to_string(bytes / (1024 * 1024)) + "." +
               std::to_string((bytes % (1024 * 1024)) / (1024 * 102)) + " MB";
    return std::to_string(bytes / (1024ULL * 1024 * 1024)) + "." +
           std::to_string((bytes % (1024ULL * 1024 * 1024)) / (1024ULL * 1024 * 102)) + " GB";
}

// ---------------------------------------------------------------------------
// Server detail – file browser tab
// ---------------------------------------------------------------------------

static void draw_server_files_tab(ServerUIState& s, const server::ServerConfig& sv) {
    if (s.file_list_dirty) {
        scan_server_directory(s, sv.server_directory);
        s.file_list_dirty = false;
    }

    // Breadcrumb path
    {
        ImGui::PushFont(f_small);
        ImGui::TextColored(k.muted, "Directory:");
        ImGui::SameLine();
        std::string display_path = sv.server_directory;
        if (!s.file_browse_path.empty()) display_path += "\\" + s.file_browse_path;
        ImGui::TextColored(k.text, "%s", display_path.c_str());
        ImGui::PopFont();
    }

    // Navigation controls
    ImGui::Spacing();
    if (!s.file_browse_path.empty()) {
        if (ghost_button("< Back", ImVec2(ui_px(80.0f), ui_px(26.0f)))) {
            auto pos = s.file_browse_path.rfind('\\');
            if (pos != std::string::npos)
                s.file_browse_path = s.file_browse_path.substr(0, pos);
            else
                s.file_browse_path.clear();
            s.file_list_dirty = true;
        }
        ImGui::SameLine(0, ui_px(8.0f));
    }
    if (ghost_button("Refresh", ImVec2(ui_px(80.0f), ui_px(26.0f)))) {
        s.file_list_dirty = true;
    }

    ImGui::Spacing();

    // File list
    float footer_h = ImGui::GetFrameHeightWithSpacing() + ui_px(4.0f);
    if (ImGui::BeginChild("##server_files_list",
            ImVec2(-1, -(footer_h + ui_px(28.0f))), ImGuiChildFlags_Borders)) {
        if (s.file_entries.empty()) {
            empty_state("Empty directory", "No files found in this folder.");
        } else {
            for (size_t i = 0; i < s.file_entries.size(); ++i) {
                const auto& name = s.file_entries[i];
                bool is_dir = s.file_is_dir[i];
                ImGui::PushID(static_cast<int>(i));

                // Icon (vector)
                const ImVec2 icon_center = ImGui::GetCursorScreenPos() +
                                           ImVec2(ui_px(8.0f), ui_px(9.0f));
                if (is_dir) {
                    draw_icon(IconId::Folder, icon_center, ui_px(7.0f), c32(k.brand));
                } else {
                    // Color by extension
                    auto dot = name.rfind('.');
                    ImVec4 ext_color = k.text;
                    if (dot != std::string::npos) {
                        auto ext = name.substr(dot + 1);
                        if (ext == "jar" || ext == "zip") ext_color = k.brand;
                        else if (ext == "properties" || ext == "yml" || ext == "yaml" || ext == "toml") ext_color = k.green;
                        else if (ext == "log" || ext == "txt") ext_color = k.muted;
                        else if (ext == "json") ext_color = k.yellow;
                    }
                    draw_icon(IconId::File, icon_center, ui_px(7.0f), c32(ext_color));
                }
                ImGui::Dummy(ImVec2(ui_px(16.0f), 0));
                ImGui::SameLine();

                // Clickable name
                ImGui::TextColored(is_dir ? k.brand : k.text, "%s", name.c_str());

                // Right-click context
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                    ImGui::OpenPopup("##file_ctx");
                }

                // Double-click to open
                if (ImGui::IsItemHovered()) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    if (ImGui::IsMouseDoubleClicked(0)) {
                        if (is_dir) {
                            if (!s.file_browse_path.empty())
                                s.file_browse_path += "\\" + name;
                            else
                                s.file_browse_path = name;
                            s.file_list_dirty = true;
                        } else {
                            s.file_preview_name = name;
                            s.file_preview_content = read_text_file(
                                file_full_path(s, sv.server_directory, name));
                            s.file_preview_open = true;
                        }
                    }
                }

                // Context menu
                if (ImGui::BeginPopup("##file_ctx")) {
                    if (is_dir) {
                        if (ImGui::MenuItem("Open")) {
                            if (!s.file_browse_path.empty())
                                s.file_browse_path += "\\" + name;
                            else
                                s.file_browse_path = name;
                            s.file_list_dirty = true;
                        }
                    } else {
                        if (ImGui::MenuItem("Preview")) {
                            s.file_preview_name = name;
                            s.file_preview_content = read_text_file(
                                file_full_path(s, sv.server_directory, name));
                            s.file_preview_open = true;
                        }
                        if (ImGui::MenuItem("Open in Explorer")) {
                            ShellExecuteW(nullptr, L"open",
                                aml::net::to_wide(file_full_path(s, sv.server_directory, name)).c_str(),
                                nullptr, nullptr, SW_SHOWNORMAL);
                        }
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Delete")) {
                        s.file_delete_target = name;
                        s.file_delete_confirm = true;
                    }
                    ImGui::EndPopup();
                }

                ImGui::PopID();
            }
        }
    }
    ImGui::EndChild();

    ImGui::Spacing();
    ImGui::TextColored(k.muted, "%d items", (int)s.file_entries.size());

    // Delete confirmation
    if (s.file_delete_confirm) {
        ImGui::OpenPopup("##confirm_file_delete");
        s.file_delete_confirm = false;  // open once
    }
    if (ImGui::BeginPopupModal("##confirm_file_delete", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Delete \"%s\"?", s.file_delete_target.c_str());
        ImGui::TextColored(k.muted, "This cannot be undone.");
        ImGui::Spacing();
        if (primary_button("Delete", ImVec2(ui_px(80.0f), ui_px(28.0f)))) {
            namespace fs = std::filesystem;
            std::string path = file_full_path(s, sv.server_directory, s.file_delete_target);
            std::error_code ec;
            if (fs::is_directory(aml::net::to_wide(path), ec))
                fs::remove_all(aml::net::to_wide(path), ec);
            else
                fs::remove(aml::net::to_wide(path), ec);
            s.file_list_dirty = true;
            s.file_delete_target.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ghost_button("Cancel", ImVec2(ui_px(70.0f), ui_px(28.0f))))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // File preview window
    if (s.file_preview_open) {
        ImGui::SetNextWindowSize(ImVec2(ui_px(560), ui_px(420)), ImGuiCond_Appearing);
        if (ImGui::Begin(("File: " + s.file_preview_name).c_str(), &s.file_preview_open,
                         ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::PushFont(f_mono);
            float footer_h2 = ImGui::GetFrameHeightWithSpacing() + ui_px(4.0f);
            if (ImGui::BeginChild("##file_preview_content",
                    ImVec2(-1, -(footer_h2)), ImGuiChildFlags_Borders)) {
                ImGui::TextWrapped("%s", s.file_preview_content.c_str());
            }
            ImGui::EndChild();
            ImGui::PopFont();

            ImGui::Spacing();
            if (ghost_button("Open in Explorer", ImVec2(ui_px(130.0f), ui_px(26.0f)))) {
                ShellExecuteW(nullptr, L"open",
                    aml::net::to_wide(file_full_path(s, sv.server_directory, s.file_preview_name)).c_str(),
                    nullptr, nullptr, SW_SHOWNORMAL);
            }
        }
        ImGui::End();
    }
}

// ---------------------------------------------------------------------------
// Server detail – players tab
// ---------------------------------------------------------------------------

static void draw_server_players_tab(ServerUIState& s, const server::ServerConfig& sv, UiState& st) {
    ImGui::PushFont(f_h2);
    ImGui::TextColored(k.text, "Online Players");
    ImGui::PopFont();
    ImGui::Spacing();

    if (effective_stage(st, sv) != server::ServerStage::Running) {
        empty_state("Server not running", "Start the server to see online players.");
    } else if (s.detail_players.empty()) {
        empty_state("No players online", "Waiting for players to join.");
    } else {
        card_begin("##players_list", ImVec2(-1, 0));
        for (size_t i = 0; i < s.detail_players.size(); ++i) {
            auto& p = s.detail_players[i];
            ImGui::PushID(static_cast<int>(i));
            const float row_h = ui_px(52.0f);
            const ImVec2 row_min = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(row_min, row_min + ImVec2(ImGui::GetContentRegionAvail().x, row_h),
                              c32(k.surface), ui_px(8.0f));

            // Player avatar dot + name
            dl->AddCircleFilled(row_min + ImVec2(ui_px(18.0f), row_h * 0.5f), ui_px(10.0f),
                                c32(k.brand_dk));
            const char initial = p.name.empty() ? '?' : p.name[0];
            char init_str[2] = { initial, '\0' };
            const ImVec2 its = ImGui::CalcTextSize(init_str);
            dl->AddText(row_min + ImVec2(ui_px(18.0f) - its.x * 0.5f,
                                         row_h * 0.5f - its.y * 0.5f),
                        c32(k.text), init_str);
            ImGui::PushFont(f_bold);
            dl->AddText(row_min + ImVec2(ui_px(38.0f), (row_h - ImGui::GetTextLineHeight()) * 0.5f),
                        c32(k.text), p.name.c_str());
            ImGui::PopFont();

            // Ping badge
            ImVec4 ping_col = p.ping_ms < 100 ? k.green : p.ping_ms < 200 ? k.yellow : k.red;
            const ImVec2 ping_pos = row_min + ImVec2(ui_px(220.0f), (row_h - ui_px(20.0f)) * 0.5f);
            const ImVec2 ping_sz = ImGui::CalcTextSize(p.ping_str.c_str()) + ImVec2(ui_px(12.0f), ui_px(4.0f));
            ImVec4 ping_bg = ping_col; ping_bg.w = 0.12f;
            dl->AddRectFilled(ping_pos, ping_pos + ping_sz, c32(ping_bg), ui_px(5.0f));
            dl->AddText(ping_pos + ImVec2(ui_px(6.0f), ui_px(2.0f)), c32(ping_col),
                        p.ping_str.c_str());

            // Actions
            ImGui::SameLine(ImGui::GetCursorPosX() +
                            std::max(0.0f, ImGui::GetContentRegionAvail().x - ui_px(140.0f)));
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (row_h - ui_px(26.0f)) * 0.5f);
            if (ghost_button("Kick", ImVec2(ui_px(62.0f), ui_px(26.0f)))) {
                auto* svc = aml::services::ServiceManager::instance().servers();
                if (svc) svc->send_command(sv.name, "kick " + p.name, nullptr);
            }
            ImGui::SameLine(0, ui_px(4.0f));
            if (ghost_button("Ban", ImVec2(ui_px(62.0f), ui_px(26.0f)))) {
                auto* svc = aml::services::ServiceManager::instance().servers();
                if (svc) svc->send_command(sv.name, "ban " + p.name, nullptr);
            }
            ImGui::Dummy(ImVec2(0, row_h));
            ImGui::PopID();
        }
        card_end();
    }

    ImGui::Spacing();
    ImGui::Spacing();

    // Whitelist / OP management
    ImGui::PushFont(f_h2);
    ImGui::TextColored(k.text, "Whitelist");
    ImGui::PopFont();
    ImGui::Spacing();

    ImGui::SetNextItemWidth(ui_px(220.0f));
    ImGui::InputText("##wl_input", &s.player_whitelist_input);
    ImGui::SameLine();
    if (primary_button("Add to Whitelist", ImVec2(ui_px(130.0f), ui_px(26.0f))) &&
        !s.player_whitelist_input.empty()) {
        auto* svc = aml::services::ServiceManager::instance().servers();
        if (svc) {
            svc->send_command(sv.name, "whitelist add " + s.player_whitelist_input, nullptr);
            s.player_whitelist_input.clear();
        }
    }
    ImGui::Spacing();

    if (!s.whitelist_entries.empty()) {
        card_begin("##whitelist", ImVec2(-1, 0));
        for (size_t i = 0; i < s.whitelist_entries.size(); ++i) {
            ImGui::PushID(static_cast<int>(i + 1000));
            ImGui::TextColored(k.text, "%s", s.whitelist_entries[i].c_str());
            ImGui::SameLine(0, ui_px(12.0f));
            if (ghost_button("Remove", ImVec2(ui_px(70.0f), ui_px(22.0f)))) {
                auto* svc = aml::services::ServiceManager::instance().servers();
                if (svc) svc->send_command(sv.name,
                    "whitelist remove " + s.whitelist_entries[i], nullptr);
            }
            ImGui::PopID();
        }
        card_end();
    }

    ImGui::Spacing();
    ImGui::Spacing();

    // Operators
    ImGui::PushFont(f_h2);
    ImGui::TextColored(k.text, "Operators (OPs)");
    ImGui::PopFont();
    ImGui::Spacing();

    ImGui::SetNextItemWidth(ui_px(220.0f));
    ImGui::InputText("##op_input", &s.player_op_input);
    ImGui::SameLine();
    if (primary_button("Make OP", ImVec2(ui_px(100.0f), ui_px(26.0f))) &&
        !s.player_op_input.empty()) {
        auto* svc = aml::services::ServiceManager::instance().servers();
        if (svc) {
            svc->send_command(sv.name, "op " + s.player_op_input, nullptr);
            s.player_op_input.clear();
        }
    }
    ImGui::Spacing();

    if (!s.ops_entries.empty()) {
        card_begin("##ops_list", ImVec2(-1, 0));
        for (size_t i = 0; i < s.ops_entries.size(); ++i) {
            ImGui::PushID(static_cast<int>(i + 2000));
            ImGui::TextColored(k.brand, "%s", s.ops_entries[i].c_str());
            ImGui::SameLine(0, ui_px(12.0f));
            if (ghost_button("De-op", ImVec2(ui_px(70.0f), ui_px(22.0f)))) {
                auto* svc = aml::services::ServiceManager::instance().servers();
                if (svc) svc->send_command(sv.name,
                    "deop " + s.ops_entries[i], nullptr);
            }
            ImGui::PopID();
        }
        card_end();
    }
}

// ---------------------------------------------------------------------------
// Server detail – world tab
// ---------------------------------------------------------------------------

static void draw_server_world_tab(ServerUIState& s, const server::ServerConfig& sv, UiState& st) {
    ImGui::PushFont(f_h2);
    ImGui::TextColored(k.text, "World");
    ImGui::PopFont();
    ImGui::Spacing();

    if (sv.server_directory.empty()) {
        empty_state("No server directory", "Set a server directory to manage the world.");
        return;
    }

    // World info card
    card_begin("##world_info", ImVec2(-1, 0));
    {
        // Scan world folder
        namespace fs = std::filesystem;
        std::string world_dir = sv.server_directory + "\\world";
        std::error_code ec;
        bool exists = fs::exists(aml::net::to_wide(world_dir), ec);

        if (!exists) {
            // Also check level-name from server.properties
            std::string level_name = "world";
            {
                FILE* f = nullptr;
                std::string prop_path = sv.server_directory + "\\server.properties";
                fopen_s(&f, prop_path.c_str(), "r");
                if (f) {
                    char buf[256];
                    while (fgets(buf, sizeof(buf), f)) {
                        std::string line(buf);
                        if (line.find("level-name=") == 0) {
                            level_name = line.substr(11);
                            if (!level_name.empty() && level_name.back() == '\n')
                                level_name.pop_back();
                        }
                    }
                    fclose(f);
                }
                world_dir = sv.server_directory + "\\" + level_name;
                exists = fs::exists(aml::net::to_wide(world_dir), ec);
            }
        }

        if (!exists) {
            ImGui::TextColored(k.muted, "World folder not found.");
            ImGui::TextColored(k.muted, "Start the server once to generate the world.");
        } else {
            // Calculate world size
            if (s.world_dirty || s.world_size_bytes == 0) {
                s.world_size_bytes = 0;
                std::error_code size_ec;
                for (auto& entry : fs::recursive_directory_iterator(
                        aml::net::to_wide(world_dir), size_ec)) {
                    if (entry.is_regular_file(size_ec))
                        s.world_size_bytes += entry.file_size(size_ec);
                }
                s.world_dirty = false;
            }

            ImGui::TextColored(k.text, "World Size:");
            ImGui::SameLine();
            ImGui::TextColored(k.muted, "%s", file_size_label(s.world_size_bytes).c_str());

            ImGui::Spacing();

            // Quick actions
            float bw = ui_px(100.0f);
            float bh = ui_px(28.0f);

            if (effective_stage(st, sv) == server::ServerStage::Running) {
                if (ghost_button("Save-All", ImVec2(bw, bh))) {
                    auto* svc = aml::services::ServiceManager::instance().servers();
                    if (svc) svc->send_command(sv.name, "save-all", nullptr);
                }
                ImGui::SameLine(0, ui_px(4.0f));
            }

            if (ghost_button("Open Folder", ImVec2(bw + ui_px(20.0f), bh))) {
                ShellExecuteW(nullptr, L"open",
                    aml::net::to_wide(world_dir).c_str(),
                    nullptr, nullptr, SW_SHOWNORMAL);
            }
        }
    }
    card_end();

    ImGui::Spacing();
    ImGui::Spacing();

    // World backup section
    ImGui::PushFont(f_h2);
    ImGui::TextColored(k.text, "Backups");
    ImGui::PopFont();
    ImGui::Spacing();

    if (ghost_button("Create Backup", ImVec2(ui_px(120.0f), ui_px(30.0f)))) {
        auto* svc = aml::services::ServiceManager::instance().servers();
        if (svc) {
            std::string backup_id;
            std::string backup_err;
            svc->create_backup(sv.name, sv.name + "_manual", &backup_id, &backup_err);
            if (!backup_err.empty()) {
                // Show inline error
                ImGui::TextColored(k.red, "Backup failed: %s", backup_err.c_str());
            }
        }
    }

    ImGui::Spacing();

    // List backups from the services layer
    {
        auto* svc = aml::services::ServiceManager::instance().servers();
        if (svc) {
            std::string berr;
            auto backups = svc->list_backups(sv.name, &berr);
            if (backups.empty()) {
                empty_state("No backups yet", "Create a backup above to protect your world.");
            } else {
                card_begin("##backups_list", ImVec2(-1, 0));
                for (size_t i = 0; i < backups.size(); ++i) {
                    ImGui::PushID(static_cast<int>(i + 3000));
                    auto& b = backups[i];
                    auto name_it = b.find("name");
                    auto id_it = b.find("id");
                    auto time_it = b.find("created_at");
                    std::string bname = name_it != b.end() ? name_it->second : "Backup";
                    std::string bid = id_it != b.end() ? id_it->second : "";
                    std::string btime = time_it != b.end() ? time_it->second : "";

                    ImGui::TextColored(k.text, "%s", bname.c_str());
                    if (!btime.empty()) {
                        ImGui::SameLine();
                        ImGui::TextColored(k.muted, "(%s)", btime.c_str());
                    }
                    ImGui::SameLine(0, ui_px(12.0f));
                    if (ghost_button("Restore", ImVec2(ui_px(70.0f), ui_px(22.0f)))) {
                        auto* svc2 = aml::services::ServiceManager::instance().servers();
                        if (svc2) svc2->restore_backup(sv.name, bid, nullptr);
                    }
                    ImGui::PopID();
                }
                card_end();
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Server detail – console tab (embedded)
// ---------------------------------------------------------------------------

static void draw_server_detail_console(ServerUIState& s, const server::ServerConfig& sv, UiState& st) {
    if (ImGui::GetTime() - s.detail_console_last_refresh >= 1.0) {
        s.detail_console_last_refresh = ImGui::GetTime();
        auto* service = aml::services::ServiceManager::instance().servers();
        if (service) {
            const auto live_entries = service->get_console_logs(sv.name, 500, nullptr);
            if (!live_entries.empty()) {
                s.console_log.clear();
                s.console_log.reserve(live_entries.size());
                for (const auto& live : live_entries) {
                    server::ServerConsoleEntry entry;
                    const std::time_t raw_time = static_cast<std::time_t>(live.timestamp);
                    std::tm local_time{};
                    char time_text[16]{};
                    if (localtime_s(&local_time, &raw_time) == 0)
                        std::strftime(time_text, sizeof(time_text), "%H:%M:%S", &local_time);
                    entry.timestamp = time_text[0] ? time_text : "live";
                    entry.message = live.message;
                    s.console_log.push_back(std::move(entry));
                }
            }
        }
    }

    // Header
    ImGui::PushFont(f_h2);
    ImGui::TextColored(k.text, "Console: %s", sv.name.c_str());
    ImGui::PopFont();
    ImGui::SameLine(0, ui_px(12.0f));
    draw_status_badge(effective_stage(st, sv));

    ImGui::Spacing();

    // Controls row
    ImGui::SetNextItemWidth(ui_px(200.0f));
    input_text_hint("##dconsole_filter", "Filter logs...", &s.detail_console_filter);

    ImGui::SameLine(0, ui_px(8.0f));
    if (ghost_button("Copy", ImVec2(ui_px(60.0f), ui_px(24.0f)))) {
        std::string copy_text;
        for (const auto& entry : s.console_log)
            copy_text += "[" + entry.timestamp + "] " + entry.message + "\n";
        if (!copy_text.empty()) ImGui::SetClipboardText(copy_text.c_str());
    }
    ImGui::SameLine(0, ui_px(4.0f));
    if (ghost_button("Clear", ImVec2(ui_px(60.0f), ui_px(24.0f)))) {
        s.console_log.clear();
    }
    ImGui::SameLine(0, ui_px(4.0f));
    if (ghost_button(s.detail_console_auto_scroll ? "Auto-scroll ON" : "Auto-scroll OFF",
                     ImVec2(ui_px(110.0f), ui_px(24.0f)))) {
        s.detail_console_auto_scroll = !s.detail_console_auto_scroll;
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Log area
    float footer_h = ImGui::GetFrameHeightWithSpacing() + ui_px(4.0f);
    if (ImGui::BeginChild("##dconsole_log",
            ImVec2(-1, -(footer_h)), ImGuiChildFlags_Borders)) {
        for (const auto& entry : s.console_log) {
            if (!s.detail_console_filter.empty() &&
                entry.message.find(s.detail_console_filter) == std::string::npos)
                continue;

            ImVec4 log_color = k.muted;
            if (entry.message.find("ERROR") != std::string::npos ||
                entry.message.find("SEVERE") != std::string::npos)
                log_color = k.red;
            else if (entry.message.find("WARN") != std::string::npos)
                log_color = k.yellow;
            else if (entry.message.find("INFO") != std::string::npos)
                log_color = k.green;

            ImGui::TextColored(k.muted, "[%s]", entry.timestamp.c_str());
            ImGui::SameLine();
            ImGui::TextColored(log_color, "%s", entry.message.c_str());
        }

        if (s.detail_console_auto_scroll &&
            ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
            ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();

    ImGui::Spacing();

    auto submit_command = [&]() {
        if (s.detail_console_input.empty()) return;
        const std::string command = s.detail_console_input;
        server::ServerConsoleEntry cmd_entry;
        cmd_entry.timestamp = ">";
        cmd_entry.message = command;
        s.console_log.push_back(std::move(cmd_entry));

        auto* service = aml::services::ServiceManager::instance().servers();
        std::string response;
        std::string command_error;
        const bool sent = service &&
            service->send_command(sv.name, command, &response, &command_error);
        server::ServerConsoleEntry result_entry;
        result_entry.timestamp = sent ? "<" : "!";
        result_entry.message = sent
            ? (response.empty() ? "Command sent" : response)
            : (command_error.empty() ? "Command could not be sent" : command_error);
        s.console_log.push_back(std::move(result_entry));
        s.detail_console_input.clear();
    };

    // Command input. Reserve the exact button width and current style gap so
    // the Send action remains inside the content region at every DPI scale.
    const float send_width = ui_px(76.0f);
    const float send_gap = ImGui::GetStyle().ItemSpacing.x;
    const float command_width = std::max(ui_px(120.0f),
        ImGui::GetContentRegionAvail().x - send_width - send_gap);
    ImGui::SetNextItemWidth(command_width);
    if (ImGui::InputText("##dconsole_cmd", &s.detail_console_input,
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
        submit_command();
        ImGui::SetKeyboardFocusHere(-1);
    }
    ImGui::SameLine(0, send_gap);
    if (primary_button("Send", ImVec2(send_width, ImGui::GetFrameHeight()))) {
        submit_command();
    }
}

// ---------------------------------------------------------------------------
// Server detail – overview tab
// ---------------------------------------------------------------------------

static void draw_server_overview_tab(ServerUIState& s, server::ServerConfig& sv, UiState& st) {
    const server::ServerStage stage = effective_stage(st, sv);
    bool is_running = (stage == server::ServerStage::Running);
    bool is_ready = (stage == server::ServerStage::Ready ||
                     stage == server::ServerStage::Stopped ||
                     stage == server::ServerStage::NotInstalled);

    // ── Status hero card ────────────────────────────────────────────
    card_begin("##detail_hero", ImVec2(-1, 0));
    {
        const ImVec2 hero_art_pos = ImGui::GetCursorScreenPos();
        const float hero_art_height = ui_px(132.0f);
        const ImVec2 hero_art_size(ImGui::GetContentRegionAvail().x, hero_art_height);
        draw_local_image(st, st.exe_dir + L"\\branding\\ai\\server-card-ai-v2.png",
                         hero_art_pos, hero_art_size, c32(k.brand_dk));
        ImGui::GetWindowDrawList()->AddRectFilledMultiColor(
            hero_art_pos, hero_art_pos + hero_art_size,
            c32(ImVec4(k.bg.x, k.bg.y, k.bg.z, 0.94f)),
            c32(ImVec4(k.bg.x, k.bg.y, k.bg.z, 0.42f)),
            c32(ImVec4(k.bg.x, k.bg.y, k.bg.z, 0.52f)),
            c32(ImVec4(k.bg.x, k.bg.y, k.bg.z, 0.94f)));
        ImGui::GetWindowDrawList()->AddRect(hero_art_pos, hero_art_pos + hero_art_size,
                                            c32(k.border), ui_px(10.0f));

        // Left status strip
        ImVec2 card_min = ImGui::GetCursorScreenPos();
        ImVec4 strip_color = stage_color(stage);
        ImGui::GetWindowDrawList()->AddRectFilled(
            card_min,
            ImVec2(card_min.x + ui_px(5.0f), card_min.y + ui_px(90.0f)),
            c32(strip_color), ui_px(2.0f));

        ImGui::Dummy(ImVec2(ui_px(10.0f), 0));
        ImGui::SameLine();

        // Name + badges
        ImGui::PushFont(f_title);
        ImGui::TextUnformatted(sv.name.c_str());
        ImGui::PopFont();
        ImGui::SameLine(0, ui_px(12.0f));
        draw_status_badge(stage);
        ImGui::SameLine(0, ui_px(8.0f));
        {
            ImDrawList* dl2 = ImGui::GetWindowDrawList();
            ImVec2 bp = ImGui::GetCursorScreenPos();
            ImVec4 sw_bg = k.brand;
            sw_bg.w = 0.15f;
            draw_badge(dl2, bp, server::server_software_name(sv.software), k.brand, sw_bg);
            ImGui::Dummy(ImVec2(
                ImGui::CalcTextSize(server::server_software_name(sv.software)).x + ui_px(14.0f),
                ui_px(20.0f)));
        }
        ImGui::SameLine(0, ui_px(8.0f));
        {
            ImDrawList* dl3 = ImGui::GetWindowDrawList();
            ImVec2 bp = ImGui::GetCursorScreenPos();
            ImVec4 ver_bg = k.blue;
            ver_bg.w = 0.12f;
            draw_badge(dl3, bp, sv.minecraft_version.c_str(), k.blue, ver_bg);
            ImGui::Dummy(ImVec2(
                ImGui::CalcTextSize(sv.minecraft_version.c_str()).x + ui_px(14.0f),
                ui_px(20.0f)));
        }

        ImGui::Spacing();

        // Config summary (stat chips)
        ImDrawList* dl = ImGui::GetWindowDrawList();
        auto meta_chip = [&](const char* text, const ImVec4& accent) {
            const ImVec2 bp = ImGui::GetCursorScreenPos();
            const ImVec2 sz = ImGui::CalcTextSize(text) + ImVec2(ui_px(14.0f), ui_px(8.0f));
            ImVec4 bg = accent; bg.w = 0.12f;
            dl->AddRectFilled(bp, bp + sz, c32(bg), ui_px(6.0f));
            dl->AddText(bp + ImVec2(ui_px(7.0f), ui_px(4.0f)), c32(accent), text);
            ImGui::Dummy(sz + ImVec2(0, ui_px(4.0f)));
        };
        std::string port_chip = "Port " + std::to_string(sv.port);
        meta_chip(port_chip.c_str(), k.muted);
        ImGui::SameLine(0, ui_px(6.0f));
        std::string ram_chip = "RAM " + std::to_string(sv.allocated_ram_mb) + " MB";
        meta_chip(ram_chip.c_str(), k.blue);
        ImGui::SameLine(0, ui_px(6.0f));
        std::string max_chip = "Max " + std::to_string(sv.max_players) + " players";
        meta_chip(max_chip.c_str(), k.green);

        if (!sv.status_message.empty()) {
            ImGui::Spacing();
            const ImVec4 sc = (stage == server::ServerStage::Error) ? k.red : k.muted;
            ImGui::TextColored(sc, "%s", sv.status_message.c_str());
        }

        const float content_bottom = ImGui::GetCursorScreenPos().y;
        if (content_bottom < hero_art_pos.y + hero_art_height) {
            ImGui::SetCursorScreenPos(ImVec2(hero_art_pos.x,
                                             hero_art_pos.y + hero_art_height));
            ImGui::Dummy(ImVec2(0, 0));
        }
    }
    card_end();

    ImGui::Spacing();

    // ── Quick actions row ───────────────────────────────────────────
    {
        float bw = ui_px(100.0f);
        float bh = ui_px(32.0f);

        if (is_running) {
            if (primary_button("Stop", ImVec2(bw + ui_px(10.0f), bh))) {
                auto* svc = aml::services::ServiceManager::instance().servers();
                std::string error;
                const bool ok = svc && svc->stop_local_server(sv.name, &error);
                if (ok) {
                    sv.stage = server::ServerStage::Stopped;
                    save_local_servers(st.servers);
                } else {
                    push_notice(st, ui_model::NoticeLevel::Error, "Stop Failed",
                                error.empty() ? "The local server could not be stopped." : error);
                }
            }
            ImGui::SameLine(0, ui_px(6.0f));
            if (ghost_button("Restart", ImVec2(bw + ui_px(10.0f), bh))) {
                auto* svc = aml::services::ServiceManager::instance().servers();
                std::string error;
                const bool stopped = svc && svc->stop_local_server(sv.name, &error);
                const bool started = stopped && svc->start_local_server(sv.name, "", sv.allocated_ram_mb, &error);
                if (started) {
                    sv.stage = server::ServerStage::Running;
                    save_local_servers(st.servers);
                } else {
                    push_notice(st, ui_model::NoticeLevel::Error, "Restart Failed",
                                error.empty() ? "The local server could not be restarted." : error);
                }
            }
        } else if (is_ready) {
            if (primary_button("Start", ImVec2(bw + ui_px(10.0f), bh))) {
                auto* svc = aml::services::ServiceManager::instance().servers();
                std::string error;
                const bool ok = svc && svc->start_local_server(sv.name, "", sv.allocated_ram_mb, &error);
                if (ok) {
                    sv.stage = server::ServerStage::Running;
                    save_local_servers(st.servers);
                } else {
                    push_notice(st, ui_model::NoticeLevel::Error, "Start Failed",
                                error.empty() ? "The local server could not be started." : error);
                }
            }
            ImGui::SameLine(0, ui_px(6.0f));
            if (ghost_button("Open Folder", ImVec2(bw + ui_px(20.0f), bh))) {
                ShellExecuteW(nullptr, L"open",
                    aml::net::to_wide(sv.server_directory).c_str(),
                    nullptr, nullptr, SW_SHOWNORMAL);
            }
        }

        ImGui::SameLine(0, ui_px(6.0f));
        if (ghost_button("Delete Server", ImVec2(bw + ui_px(10.0f), bh))) {
            s.action_pending = s.detail_server_idx;
            ImGui::OpenPopup("Confirm Delete");
        }
    }

    ImGui::Spacing();

    // ── Performance metrics (if running) ────────────────────────────
    if (is_running) {
        card_begin("##detail_metrics", ImVec2(-1, 0));
        ImGui::PushFont(f_h2);
        ImGui::TextColored(k.text, "Live Performance");
        ImGui::PopFont();
        ImGui::Spacing();

        auto& metrics = st.server_metrics;
        if (!metrics.valid) {
            empty_state("Live metrics unavailable",
                        "Metrics will appear when this server reports an authoritative health snapshot.");
        } else {

        // Stat cards row
        float card_w = (ImGui::GetContentRegionAvail().x - ui_px(36.0f)) / 4.0f;

        float max_h = 0;
        float cpu_pct = metrics.cpu_percent / 100.0f;
        ImVec4 cpu_color = cpu_pct > 0.9f ? k.red : cpu_pct > 0.7f ? k.orange : k.green;
        max_h = std::max(max_h, draw_stat_card("CPU", (std::to_string((int)metrics.cpu_percent) + "%").c_str(), cpu_pct, cpu_color, card_w));
        ImGui::SameLine(0, ui_px(12.0f));

        float ram_pct = metrics.ram_percent / 100.0f;
        ImVec4 ram_color = ram_pct > 0.9f ? k.red : ram_pct > 0.7f ? k.orange : k.blue;
        max_h = std::max(max_h, draw_stat_card("RAM", (std::to_string(metrics.ram_mb) + " MB").c_str(), ram_pct, ram_color, card_w));
        ImGui::SameLine(0, ui_px(12.0f));

        ImVec4 tps_col = metrics.tps >= 19.0f ? k.green : metrics.tps >= 15.0f ? k.yellow : k.red;
        float tps_pct = metrics.tps / 20.0f;
        char tps_value[32]{};
        std::snprintf(tps_value, sizeof(tps_value), "%.1f", metrics.tps);
        max_h = std::max(max_h, draw_stat_card("TPS", tps_value, tps_pct, tps_col, card_w));
        ImGui::SameLine(0, ui_px(12.0f));

        float pl_pct = (float)metrics.players_online / (float)std::max(1, sv.max_players);
        max_h = std::max(max_h, draw_stat_card("Players", (std::to_string(metrics.players_online) + " / " + std::to_string(sv.max_players)).c_str(), pl_pct, k.brand, card_w));
        }

        card_end();
    }
}

// ---------------------------------------------------------------------------
// Server detail – plugins tab
// ---------------------------------------------------------------------------

static void draw_server_plugins_tab(ServerUIState&, const server::ServerConfig& sv) {
    ImGui::PushFont(f_h2);
    ImGui::TextColored(k.text, "Plugins & Mods");
    ImGui::PopFont();
    ImGui::Spacing();

    namespace fs = std::filesystem;
    bool has_plugins = false;
    bool has_mods = false;
    std::string plugins_dir = sv.server_directory + "\\plugins";
    std::string mods_dir = sv.server_directory + "\\mods";

    std::error_code ec;
    has_plugins = fs::exists(aml::net::to_wide(plugins_dir), ec);
    has_mods = fs::exists(aml::net::to_wide(mods_dir), ec);

    if (!has_plugins && !has_mods) {
        empty_state("No plugins or mods directory",
                    "Create a 'plugins' or 'mods' folder in your server directory to add them.");
        ImGui::Spacing();
        if (ghost_button("Open Server Folder", ImVec2(ui_px(140.0f), ui_px(28.0f)))) {
            ShellExecuteW(nullptr, L"open",
                aml::net::to_wide(sv.server_directory).c_str(),
                nullptr, nullptr, SW_SHOWNORMAL);
        }
        return;
    }

    if (has_plugins) {
        ImGui::PushFont(f_h2);
        ImGui::TextColored(k.text, "Plugins");
        ImGui::PopFont();
        ImGui::Spacing();

        std::vector<std::string> jars;
        for (auto& entry : fs::directory_iterator(aml::net::to_wide(plugins_dir), ec)) {
            if (entry.is_regular_file(ec)) {
                auto ext = entry.path().extension().u8string();
                std::string ext_s(reinterpret_cast<const char*>(ext.c_str()));
                if (ext_s == ".jar") {
                    auto fname = entry.path().filename().u8string();
                    jars.emplace_back(reinterpret_cast<const char*>(fname.c_str()));
                }
            }
        }
        std::sort(jars.begin(), jars.end());

        if (jars.empty()) {
            empty_state("No plugins installed", "Drop .jar files into the plugins folder.");
        } else {
            card_begin("##plugins_list", ImVec2(-1, 0));
            for (size_t i = 0; i < jars.size(); ++i) {
                ImGui::PushID(static_cast<int>(i + 4000));
                const ImVec2 icon_pos = ImGui::GetCursorScreenPos();
                draw_icon(IconId::Cube,
                          icon_pos + ImVec2(ui_px(8.0f), ui_px(8.0f)),
                          ui_px(6.0f), c32(k.brand));
                ImGui::Dummy(ImVec2(ui_px(16.0f), ui_px(16.0f)));
                ImGui::SameLine(0, ui_px(4.0f));
                ImGui::TextColored(k.text, "%s", jars[i].c_str());
                ImGui::PopID();
            }
            card_end();
            ImGui::TextColored(k.muted, "%d plugins", (int)jars.size());
        }
        ImGui::Spacing();
    }

    if (has_mods) {
        ImGui::PushFont(f_h2);
        ImGui::TextColored(k.text, "Mods");
        ImGui::PopFont();
        ImGui::Spacing();

        std::vector<std::string> jars;
        for (auto& entry : fs::directory_iterator(aml::net::to_wide(mods_dir), ec)) {
            if (entry.is_regular_file(ec)) {
                auto ext = entry.path().extension().u8string();
                std::string ext_s(reinterpret_cast<const char*>(ext.c_str()));
                if (ext_s == ".jar") {
                    auto fname = entry.path().filename().u8string();
                    jars.emplace_back(reinterpret_cast<const char*>(fname.c_str()));
                }
            }
        }
        std::sort(jars.begin(), jars.end());

        if (jars.empty()) {
            empty_state("No mods installed", "Drop .jar files into the mods folder.");
        } else {
            card_begin("##mods_list", ImVec2(-1, 0));
            for (size_t i = 0; i < jars.size(); ++i) {
                ImGui::PushID(static_cast<int>(i + 5000));
                const ImVec2 icon_pos = ImGui::GetCursorScreenPos();
                draw_icon(IconId::Cube,
                          icon_pos + ImVec2(ui_px(8.0f), ui_px(8.0f)),
                          ui_px(6.0f), c32(k.brand));
                ImGui::Dummy(ImVec2(ui_px(16.0f), ui_px(16.0f)));
                ImGui::SameLine(0, ui_px(4.0f));
                ImGui::TextColored(k.text, "%s", jars[i].c_str());
                ImGui::PopID();
            }
            card_end();
            ImGui::TextColored(k.muted, "%d mods", (int)jars.size());
        }
    }
}

// ---------------------------------------------------------------------------
// Server detail – properties tab (embedded)
// ---------------------------------------------------------------------------

static void draw_server_properties_tab(ServerUIState& s, const server::ServerConfig& sv) {
    ImGui::PushFont(f_h2);
    ImGui::TextColored(k.text, "server.properties");
    ImGui::PopFont();
    ImGui::Spacing();

    // Load properties if needed
    if (s.properties.empty() || s.properties_server_idx != s.detail_server_idx) {
        load_server_properties(sv.server_directory, s.properties);
        s.properties_server_idx = s.detail_server_idx;
        s.properties_dirty = false;
    }

    if (s.properties.empty()) {
        empty_state("No server.properties found",
                    "Start the server once to generate the properties file.");
        return;
    }

    ImGui::TextColored(k.muted, "%d properties", (int)s.properties.size());
    ImGui::Spacing();

    float footer_h = ImGui::GetFrameHeightWithSpacing() + ui_px(8.0f);
    if (ImGui::BeginChild("##detail_props_list",
            ImVec2(-1, -(footer_h)), ImGuiChildFlags_Borders)) {
        for (size_t i = 0; i < s.properties.size(); ++i) {
            auto& [key, val] = s.properties[i];
            ImGui::PushID(static_cast<int>(i + 6000));

            ImGui::TextColored(k.brand, "%s", key.c_str());
            ImGui::SameLine(ui_px(220.0f));
            ImGui::SetNextItemWidth(-1);

            bool is_bool = (key == "online-mode" || key == "pvp" ||
                            key == "enable-command-block" || key == "spawn-monsters" ||
                            key == "spawn-animals" || key == "spawn-npcs" ||
                            key == "white-list" || key == "enforce-whitelist" ||
                            key == "allow-flight" || key == "allow-nether" ||
                            key == "spawn-protection");
            if (is_bool) {
                bool bval = (val == "true");
                if (ImGui::Checkbox("##val", &bval)) {
                    val = bval ? "true" : "false";
                    s.properties_dirty = true;
                }
            } else {
                char buf[512];
                strncpy(buf, val.c_str(), sizeof(buf) - 1);
                buf[sizeof(buf) - 1] = '\0';
                if (ImGui::InputText("##val", buf, sizeof(buf))) {
                    val = buf;
                    s.properties_dirty = true;
                }
            }

            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    ImGui::Spacing();
    if (s.properties_dirty) {
        if (primary_button("Save Properties",
                           ImVec2(ui_px(130.0f), ui_px(30.0f)))) {
            save_server_properties(sv.server_directory, s.properties);
            s.properties_dirty = false;
        }
        ImGui::SameLine();
    }
    if (ghost_button("Reload", ImVec2(ui_px(80.0f), ui_px(30.0f)))) {
        load_server_properties(sv.server_directory, s.properties);
        s.properties_dirty = false;
    }
}

// ---------------------------------------------------------------------------
// Server detail – Connect server version (for external servers)
// ---------------------------------------------------------------------------

static void draw_server_detail_connect(UiState& st) {
    auto& s = state();
    if (s.detail_connect_idx < 0 || s.detail_connect_idx >= static_cast<int>(st.cfg->servers.size()))
        return;

    const auto& server = st.cfg->servers[s.detail_connect_idx];
    static const char* kTypeLabels[] = {"Java", "Bedrock", "Modded (Java)", "Bedrock Modded"};
    int type_idx = (server.type >= 0 && server.type < 4) ? server.type : 0;

    // Breadcrumb
    if (ghost_button("< Servers", ImVec2(ui_px(120.0f), ui_px(28.0f)))) {
        s.detail_connect_idx = -1;
    }
    ImGui::Spacing();

    // Hero card
    card_begin("##connect_detail_hero", ImVec2(-1, 0));
    ImGui::PushFont(f_title);
    ImGui::TextUnformatted(server.name.c_str());
    ImGui::PopFont();
    ImGui::Spacing();

    ImGui::TextColored(k.muted, "Address:");
    ImGui::SameLine();
    ImGui::TextColored(k.text, "%s", net::to_utf8(server.address).c_str());

    ImGui::TextColored(k.muted, "Type:");
    ImGui::SameLine();
    ImGui::TextColored(k.brand, "%s", kTypeLabels[type_idx]);

    if (!server.profile.empty()) {
        ImGui::TextColored(k.muted, "Profile:");
        ImGui::SameLine();
        ImGui::TextColored(k.brand, "%s", server.profile.c_str());
    }

    ImGui::Spacing();
    ImGui::Spacing();

    // Actions
    float bw = ui_px(100.0f);
    float bh = ui_px(32.0f);

    if (primary_button("Play", ImVec2(bw + ui_px(10.0f), bh)) && !st.running) {
        if (!server.profile.empty()) {
            st.selected = server.profile;
            st.active_instance_dir.clear();
            for (auto& inst : st.instance_list) {
                if (inst.id == server.profile) {
                    st.active_instance_dir = inst.directory;
                    break;
                }
            }
        }
        if (!st.active_instance_dir.empty() && !st.selected.empty()) {
            st.pending_instance_dir = st.active_instance_dir;
            st.pending_launch = true;
            st.pending_id = st.selected;
            st.pending_server = net::to_utf8(server.address);
        }
    }
    ImGui::SameLine(0, ui_px(6.0f));
    if (ghost_button("Edit", ImVec2(bw, bh))) {
        s.detail_connect_idx = -1;
        st.selected_server = s.detail_connect_idx;
        st.server_name = server.name;
        st.ui_server = net::to_utf8(server.address);
        st.server_edit_type = server.type;
        st.server_edit_profile = server.profile;
    }
    ImGui::SameLine(0, ui_px(6.0f));
    if (ghost_button("Copy Address", ImVec2(bw + ui_px(10.0f), bh))) {
        ImGui::SetClipboardText(net::to_utf8(server.address).c_str());
    }

    card_end();
}

// ---------------------------------------------------------------------------
// Main server detail panel
// ---------------------------------------------------------------------------

static void draw_server_detail(UiState& st) {
    auto& s = state();

    // ── Connect mode detail ───────────────────────────────────────
    if (s.detail_connect_idx >= 0) {
        draw_server_detail_connect(st);
        return;
    }

    // ── Host mode detail ──────────────────────────────────────────
    if (s.detail_server_idx < 0 || s.detail_server_idx >= static_cast<int>(st.servers.size()))
        return;

    server::ServerConfig& sv = st.servers[s.detail_server_idx];
    s.detail_tab = std::clamp(s.detail_tab, 0, 6);

    // Seed the console log from the UiState-level log so both the fixture and
    // any live console feed share one source of truth.
    if (s.console_log.empty() && !st.server_console_log.empty()) {
        s.console_log = st.server_console_log;
    }

    // Breadcrumb
    if (ghost_button("< Servers", ImVec2(ui_px(120.0f), ui_px(28.0f)))) {
        s.detail_server_idx = -1;
        return;
    }
    ImGui::Spacing();

    // ── Tab bar ───────────────────────────────────────────────────
    static const char* kTabs[] = {
        "Overview", "Console", "Files", "Players",
        "Plugins", "Properties", "World"
    };
    static const int kTabCount = 7;

    // Premium pill tabs (matching the design-system tab language)
    const float tab_gap = ui_px(6.0f);
    const float tab_h = ui_px(32.0f);
    for (int i = 0; i < kTabCount; ++i) {
        const bool active = (s.detail_tab == i);
        const char* label = kTabs[i];
        const ImVec2 label_sz = ImGui::CalcTextSize(label);
        const float pad_x = ui_px(14.0f);
        const ImVec2 tab_size(label_sz.x + pad_x * 2.0f, tab_h);
        const float content_right = ImGui::GetCursorScreenPos().x +
                                    ImGui::GetContentRegionAvail().x;
        if (i > 0) {
            const float next_x = ImGui::GetCursorScreenPos().x + tab_gap + tab_size.x;
            if (next_x > content_right) {
                ImGui::NewLine();
                ImGui::Spacing();
            } else {
                ImGui::SameLine(0, tab_gap);
            }
        }
        const ImVec2 tab_min = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(tab_min, tab_min + tab_size,
                          c32(active ? k.surface2 : ImVec4(0, 0, 0, 0)),
                          ui_px(8.0f));
        if (active)
            dl->AddRect(tab_min, tab_min + tab_size, c32(k.brand), ui_px(8.0f),
                        0, ui_px(1.5f));
        ImGui::InvisibleButton((std::string("##srv_tab_") + std::to_string(i)).c_str(),
                               tab_size);
        if (ImGui::IsItemHovered() && !active)
            dl->AddRect(tab_min, tab_min + tab_size, c32(k.border), ui_px(8.0f),
                        0, ui_px(1.0f));
        if (ImGui::IsItemClicked()) s.detail_tab = i;
        ImGui::PushFont(active ? f_bold : f_body);
        dl->AddText(tab_min + ImVec2(pad_x, (tab_h - ImGui::GetTextLineHeight()) * 0.5f),
                    c32(active ? k.text : k.muted), label);
        ImGui::PopFont();
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    ImGui::Spacing();

    // ── Tab content ───────────────────────────────────────────────
    switch (s.detail_tab) {
        case 0: draw_server_overview_tab(s, sv, st); break;
        case 1: draw_server_detail_console(s, sv, st); break;
        case 2: draw_server_files_tab(s, sv); break;
        case 3: draw_server_players_tab(s, sv, st); break;
        case 4: draw_server_plugins_tab(s, sv); break;
        case 5: draw_server_properties_tab(s, sv); break;
        case 6: draw_server_world_tab(s, sv, st); break;
    }
}

// ---------------------------------------------------------------------------
// Create dialog
// ---------------------------------------------------------------------------

static void draw_create_dialog(UiState& st) {
    auto& s = state();
    if (!s.create_open) return;

    std::vector<std::string> supported_versions = version_catalog::server_versions();
    // The static list gives the dialog a useful first paint. Once the Mojang
    // manifest is available, merge every released version into the same
    // selector so a newly published vanilla server does not wait for a
    // launcher update.
    {
        std::lock_guard<std::mutex> lock(st.version_mu);
        for (const auto& manifest_entry : st.versions) {
            if (manifest_entry.type != "release" ||
                !version_catalog::supports_server(manifest_entry.id)) continue;
            if (std::find(supported_versions.begin(), supported_versions.end(), manifest_entry.id) ==
                supported_versions.end())
                supported_versions.push_back(manifest_entry.id);
        }
    }
    if (!version_catalog::supports_server(s.create_version)) {
        s.create_version = version_catalog::default_server_version();
    }

    ImGui::OpenPopup("Create Server");
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(
        std::min(ui_px(660.0f), ImGui::GetMainViewport()->WorkSize.x - ui_px(40.0f)),
        std::min(ui_px(690.0f), ImGui::GetMainViewport()->WorkSize.y - ui_px(40.0f))),
        ImGuiCond_Appearing);

    if (ImGui::BeginPopupModal("Create Server", &s.create_open,
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {

        card_begin("##create_server_hero", ImVec2(-1, ui_px(88.0f)));
        const ImVec2 hero_origin = ImGui::GetCursorScreenPos();
        ImDrawList* hero_draw = ImGui::GetWindowDrawList();
        hero_draw->AddCircleFilled(hero_origin + ImVec2(ui_px(29.0f), ui_px(29.0f)), ui_px(24.0f),
                                   c32(ImVec4(k.brand_dk.x, k.brand_dk.y, k.brand_dk.z, 0.92f)));
        draw_icon(IconId::Server, hero_origin + ImVec2(ui_px(29.0f), ui_px(29.0f)), ui_px(13.0f), c32(k.brand_hov));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ui_px(64.0f));
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("Create a local server");
        ImGui::PopFont();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ui_px(64.0f));
        ImGui::TextColored(k.muted, "Choose a runtime, capacity, and protected server settings. You can edit all of this later.");
        card_end();
        ImGui::Spacing();

        card_begin("##create_server_settings", ImVec2(-1, 0));
        ImGui::TextColored(k.muted, "SERVER BASICS");
        ImGui::PushFont(f_bold);
        ImGui::TextUnformatted("Server name");
        ImGui::PopFont();
        ImGui::SetNextItemWidth(-1);
        input_text_hint("##cs_name", "For example: Survival Realm", &s.create_name);

        ImGui::Spacing();
        ImGui::PushFont(f_bold);
        ImGui::TextUnformatted("Minecraft version");
        ImGui::PopFont();
        ImGui::TextColored(k.muted,
                           "Choose a tested server target. Amalgam verifies the provider build before downloading.");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##cs_ver", s.create_version.c_str())) {
            for (const std::string& version : supported_versions) {
                const bool selected = s.create_version == version;
                if (ImGui::Selectable(version.c_str(), selected)) {
                    s.create_version = version;
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        ImGui::Spacing();
        ImGui::PushFont(f_bold);
        ImGui::TextUnformatted("Server software");
        ImGui::PopFont();
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##cs_sw", kSoftwareNames[s.create_software_idx])) {
            for (int i = 0; i < kSoftwareCount; ++i) {
                if (ImGui::Selectable(kSoftwareNames[i], s.create_software_idx == i))
                    s.create_software_idx = i;
            }
            ImGui::EndCombo();
        }

        ImGui::Spacing();
        ImGui::PushFont(f_bold);
        ImGui::Text("Memory allocation: %d MB", s.create_ram);
        ImGui::PopFont();
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderInt("##cs_ram", &s.create_ram, 512, 16384, "%d MB");

        ImGui::Spacing();

        // Two-column layout for port and players
        ImGui::Columns(2, nullptr, false);
        ImGui::SetColumnWidth(0, ui_px(200.0f));
        ImGui::Text("Max Players");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputInt("##cs_mp", &s.create_max_players, 1, 10);
        s.create_max_players = std::clamp(s.create_max_players, 1, 200);

        ImGui::NextColumn();
        ImGui::Text("Port");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputInt("##cs_port", &s.create_port, 1, 100);
        s.create_port = std::clamp(s.create_port, 1, 65535);
        ImGui::Columns(1);
        card_end();

        ImGui::Spacing();
        card_begin("##create_server_eula", ImVec2(-1, 0));
        ImGui::TextColored(k.muted, "MINECRAFT EULA");
        ImGui::Checkbox("I have read and agree to the Minecraft EULA",
                        &s.create_eula_accepted);
        ImGui::SameLine();
        if (ImGui::SmallButton("Read EULA")) {
            ShellExecuteW(nullptr, L"open", L"https://aka.ms/MinecraftEULA",
                          nullptr, nullptr, SW_SHOWNORMAL);
        }
        ImGui::TextColored(k.muted,
            "Amalgam writes eula=true only after this explicit confirmation.");
        card_end();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const bool safe_name = !s.create_name.empty() && s.create_name != "." &&
            s.create_name != ".." &&
            s.create_name.find_first_of("\\/:*?\"<>|") == std::string::npos;
        bool duplicate_name = false;
        for (const auto& existing : st.servers) {
            std::string left = existing.name;
            std::string right = s.create_name;
            std::transform(left.begin(), left.end(), left.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            std::transform(right.begin(), right.end(), right.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (!right.empty() && left == right) { duplicate_name = true; break; }
        }
        if (!safe_name)
            ImGui::TextColored(k.red, "Use a simple server name without path characters.");
        else if (duplicate_name)
            ImGui::TextColored(k.red, "A server with this name already exists.");
        else if (!s.create_eula_accepted)
            ImGui::TextColored(k.yellow, "EULA acceptance is required before creation.");
        const bool valid = safe_name && !duplicate_name && s.create_eula_accepted &&
            version_catalog::supports_server(s.create_version);
        if (!valid) ImGui::BeginDisabled();
        if (primary_button("Create", ImVec2(ui_px(100.0f), ui_px(32.0f)))) {
            server::ServerConfig cfg;
            cfg.name = s.create_name;
            cfg.software = kSoftwareValues[s.create_software_idx];
            cfg.minecraft_version = s.create_version;
            cfg.allocated_ram_mb = s.create_ram;
            cfg.max_players = s.create_max_players;
            cfg.port = s.create_port;
            cfg.stage = server::ServerStage::Installing;

            std::wstring data = aml::net::get_local_app_data_path() + L"\\amalgam\\servers\\";
            std::wstring dir = data + aml::net::to_wide(s.create_name);
            std::filesystem::create_directories(dir);
            cfg.server_directory = aml::net::to_utf8(dir);

            bool config_written = true;
            {
                std::ofstream eula(std::filesystem::path(dir) / "eula.txt",
                                   std::ios::trunc);
                eula << "eula=true\n";
                config_written = static_cast<bool>(eula);
            }
            {
                std::ofstream properties(std::filesystem::path(dir) / "server.properties",
                                         std::ios::trunc);
                properties << "server-name=" << cfg.name << "\n"
                           << "motd=" << cfg.name << " - managed by Amalgam\n"
                           << "server-port=" << cfg.port << "\n"
                           << "max-players=" << cfg.max_players << "\n"
                           << "online-mode=true\n"
                           << "view-distance=10\n"
                           << "simulation-distance=10\n";
                config_written = config_written && static_cast<bool>(properties);
            }

            if (!config_written) {
                cfg.stage = server::ServerStage::Error;
                cfg.status_message = "Failed to write EULA or server.properties";
            } else if (is_auto_provisioned(cfg.software)) {
                std::string prov_err;
                if (provision_server_runtime(cfg, &prov_err)) {
                    cfg.stage = server::ServerStage::Ready;
                } else {
                    cfg.stage = server::ServerStage::Error;
                    cfg.status_message = prov_err.empty() ? "Failed to install runtime" : prov_err;
                }
            } else {
                cfg.stage = server::ServerStage::Ready;
                cfg.status_message = "Runtime must be installed manually";
            }

            st.servers.push_back(cfg);
            save_local_servers(st.servers);

            s.create_open = false;
            s.create_name.clear();
            s.create_ram = 4096;
            s.create_max_players = 20;
            s.create_port = 25565;
            s.create_eula_accepted = false;
            ImGui::CloseCurrentPopup();
        }
        if (!valid) ImGui::EndDisabled();

        ImGui::SameLine();
        if (ghost_button("Cancel", ImVec2(ui_px(90.0f), ui_px(32.0f)))) {
            s.create_open = false;
            s.create_name.clear();
            s.create_eula_accepted = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    if (!s.create_open) {
        s.create_name.clear();
        s.create_eula_accepted = false;
    }
}

// ---------------------------------------------------------------------------
// Fixture helper: lets the snapshot runner open the cloud tab directly.
// ---------------------------------------------------------------------------

void set_fixture_server_mode(int mode) {
    state().mode = mode;
}

void set_fixture_server_detail(int server_index, int tab) {
    auto& s = state();
    s.mode = 0;
    s.detail_server_idx = std::max(0, server_index);
    s.detail_connect_idx = -1;
    s.detail_tab = std::clamp(tab, 0, 6);
    s.file_list_dirty = true;
    s.players_dirty = true;
    s.world_dirty = true;
}

// ---------------------------------------------------------------------------
// Main entry point
// ---------------------------------------------------------------------------

void draw_server_manager(UiState& st) {
    auto& s = state();
    if (!s.loaded) {
        // In fixture mode the visual seed already populated st.servers.
        if (!st.fixture_mode || st.servers.empty()) {
            load_local_servers(st.servers);
            reconcile_local_server_stages(st.servers);
        }
        s.loaded = true;
    }

    // ── Detail view (local server) ──────────────────────────────────
    if (s.detail_server_idx >= 0) {
        page_title("Servers", nullptr);
        draw_breadcrumbs({"Home", "Servers", "Local"});
        draw_server_detail(st);
        // Overlays
        draw_create_dialog(st);
        draw_console_panel(st);
        draw_properties_panel(st);
        return;
    }

    draw_page_emblem(st, "server-emblem-ai.png");
    page_title("Servers", "Run local servers here — hosted servers are managed on the website.");
    draw_breadcrumbs({"Home", "Servers"});

    // Branded scene keeps this operational page visually connected to the
    // launcher while the controls remain real and readable on top.
    {
        const float art_h = ui_px(116.0f);
        const ImVec2 art_pos = ImGui::GetCursorScreenPos();
        const ImVec2 art_size(ImGui::GetContentRegionAvail().x, art_h);
        draw_local_image(st, st.exe_dir + L"\\branding\\ai\\launcher-servers-ai.png",
                         art_pos, art_size, c32(k.brand_dk));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(art_pos, art_pos + art_size,
                          c32(ImVec4(0.02f, 0.01f, 0.06f, 0.32f)), ui_px(12.0f));
        dl->AddRect(art_pos, art_pos + art_size,
                    c32(ImVec4(k.brand.x, k.brand.y, k.brand.z, 0.35f)), ui_px(12.0f),
                    0, ui_px(1.0f));
        ImGui::SetCursorScreenPos(art_pos + ImVec2(ui_px(18.0f), ui_px(16.0f)));
        ImGui::PushFont(f_title);
        ImGui::TextColored(k.text, "Build your world");
        ImGui::PopFont();
        ImGui::TextColored(k.muted, "Host locally, connect external servers, and keep every world organized.");
        ImGui::SetCursorScreenPos(ImVec2(art_pos.x, art_pos.y + art_h + ui_px(12.0f)));
    }

    // ── Mode switch: LOCAL / AMALGAM CLOUD (premium pill tabs) ────
    const char* modes[] = {"LOCAL", "AMALGAM CLOUD"};
    {
        const float tab_gap = ui_px(6.0f);
        const float tab_h = ui_px(34.0f);
        for (int i = 0; i < 2; ++i) {
            if (i) ImGui::SameLine(0, tab_gap);
            const bool active = s.mode == i;
            const char* label = modes[i];
            const ImVec2 label_sz = ImGui::CalcTextSize(label);
            const float pad_x = ui_px(16.0f);
            const ImVec2 tab_size(label_sz.x + pad_x * 2.0f, tab_h);
            const ImVec2 tab_min = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(tab_min, tab_min + tab_size,
                              c32(active ? k.surface2 : ImVec4(0, 0, 0, 0)),
                              ui_px(8.0f));
            if (active)
                dl->AddRect(tab_min, tab_min + tab_size, c32(k.brand), ui_px(8.0f),
                            0, ui_px(1.5f));
            ImGui::InvisibleButton((std::string("##mode_") + std::to_string(i)).c_str(),
                                   tab_size);
            if (ImGui::IsItemHovered() && !active)
                dl->AddRect(tab_min, tab_min + tab_size, c32(k.border), ui_px(8.0f),
                            0, ui_px(1.0f));
            if (ImGui::IsItemClicked()) s.mode = i;
            ImGui::PushFont(active ? f_bold : f_body);
            dl->AddText(tab_min + ImVec2(pad_x, (tab_h - ImGui::GetTextLineHeight()) * 0.5f),
                        c32(active ? k.text : k.muted), label);
            ImGui::PopFont();
            if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
        ImGui::Separator();
        ImGui::Spacing();
    }

    // ── Cloud tab → delegate to cloud_ui.cpp ──────────────────────
    if (s.mode == 1) {
        draw_cloud_page(st);
        return;
    }

    // ══════════════════════════════════════════════════════════════
    // LOCAL SERVERS
    // ══════════════════════════════════════════════════════════════

    // Header
    ImGui::PushFont(f_h2);
    ImGui::TextColored(k.text, "LOCAL SERVERS");
    ImGui::PopFont();
    ImGui::TextColored(k.muted, "Run and manage Minecraft servers directly from this PC.");
    ImGui::Spacing();

    // Toolbar
    if (primary_button("+ Create Local Server", ImVec2(ui_px(170.0f), ui_px(32.0f)))) {
        s.create_open = true;
    }
    ImGui::SameLine(0, ui_px(8.0f));
    if (ghost_button("Import Server", ImVec2(ui_px(120.0f), ui_px(32.0f)))) {
        wchar_t path_buf[MAX_PATH] = {0};
        BROWSEINFOW bi{};
        bi.hwndOwner = st.hwnd;
        bi.lpszTitle = L"Select an existing Minecraft server folder to import.";
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
        if (pidl) {
            if (SHGetPathFromIDListW(pidl, path_buf)) {
                std::wstring folder(path_buf);
                std::string dir_utf8 = aml::net::to_utf8(folder);
                // Read server.properties to detect version, port, software.
                server::ServerConfig cfg;
                cfg.server_directory = dir_utf8;
                cfg.stage = server::ServerStage::Ready;
                cfg.allocated_ram_mb = 4096;
                cfg.max_players = 20;
                cfg.port = 25565;
                cfg.minecraft_version = "1.20.1";
                cfg.software = server::ServerSoftware::Vanilla;
                // Parse server.properties if present.
                std::wstring props_path = folder + L"\\server.properties";
                std::ifstream props(props_path);
                if (props.is_open()) {
                    std::string line;
                    while (std::getline(props, line)) {
                        auto eq = line.find('=');
                        if (eq == std::string::npos) continue;
                        std::string key = line.substr(0, eq);
                        std::string val = line.substr(eq + 1);
                        // Trim whitespace.
                        auto trim = [](std::string& s) {
                            while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
                            size_t start = s.find_first_not_of(" \t");
                            if (start != std::string::npos) s = s.substr(start);
                        };
                        trim(key); trim(val);
                        if (key == "server-port") cfg.port = std::clamp(std::stoi(val.empty() ? "25565" : val), 1, 65535);
                        else if (key == "max-players") cfg.max_players = std::clamp(std::stoi(val.empty() ? "20" : val), 1, 200);
                        else if (key == "server-name" || key == "motd") {
                            if (!val.empty() && cfg.name.empty()) cfg.name = val;
                        }
                    }
                }
                // Detect software from jar files in the directory.
                for (const auto& entry : std::filesystem::directory_iterator(folder)) {
                    if (!entry.is_regular_file()) continue;
                    const auto& p = entry.path();
                    const auto ext = p.extension().wstring();
                    const auto stem = p.stem().wstring();
                    auto to_lower = [](std::wstring s) {
                        for (auto& c : s) c = (wchar_t)towlower((wint_t)c);
                        return s;
                    };
                    std::wstring le = to_lower(stem);
                    std::wstring le_ext = to_lower(ext);
                    if (le_ext != L".jar") continue;
                    if (le.find(L"paper") != std::wstring::npos) cfg.software = server::ServerSoftware::Paper;
                    else if (le.find(L"purpur") != std::wstring::npos) cfg.software = server::ServerSoftware::Paper;
                    else if (le.find(L"forge") != std::wstring::npos || le.find(L"neoforge") != std::wstring::npos) cfg.software = server::ServerSoftware::Vanilla;
                    else if (le.find(L"fabric") != std::wstring::npos) cfg.software = server::ServerSoftware::Vanilla;
                    else if (le.find(L"velocity") != std::wstring::npos) cfg.software = server::ServerSoftware::Vanilla;
                    else if (le.find(L"waterfall") != std::wstring::npos) cfg.software = server::ServerSoftware::Vanilla;
                }
                // Use folder name if no name was found.
                if (cfg.name.empty()) {
                    auto folder_name = std::filesystem::path(folder).filename().wstring();
                    cfg.name = aml::net::to_utf8(folder_name);
                }
                // Check for duplicate.
                bool duplicate = false;
                for (const auto& existing : st.servers) {
                    if (existing.server_directory == dir_utf8) { duplicate = true; break; }
                }
                if (!duplicate) {
                    st.servers.push_back(cfg);
                    save_local_servers(st.servers);
                }
            }
            CoTaskMemFree(pidl);
        }
    }

    // Keep the filters on a dedicated row once the content column becomes
    // narrow. This prevents the create/import actions and combo boxes from
    // competing for the same horizontal space on smaller windows.
    if (ImGui::GetContentRegionAvail().x < ui_px(760.0f)) {
        ImGui::NewLine();
        ImGui::Spacing();
    } else {
        ImGui::SameLine(0, ui_px(12.0f));
    }

    // Filter bar
    {
        const char* status_name = status_filter_name(s.filter_status);
        ImGui::SetNextItemWidth(ui_px(110.0f));
        if (ImGui::BeginCombo("##srv_status_f", status_name)) {
            for (int i = 0; i < 5; ++i) {
                if (ImGui::Selectable(status_filter_name(i), s.filter_status == i))
                    s.filter_status = i;
            }
            ImGui::EndCombo();
        }
    }
    ImGui::SameLine(0, ui_px(4.0f));
    {
        const char* sw_name = software_filter_name(s.filter_software);
        ImGui::SetNextItemWidth(ui_px(110.0f));
        if (ImGui::BeginCombo("##srv_sw_f", sw_name)) {
            for (int i = 0; i <= kSoftwareCount; ++i) {
                if (ImGui::Selectable(software_filter_name(i), s.filter_software == i))
                    s.filter_software = i;
            }
            ImGui::EndCombo();
        }
    }
    ImGui::SameLine(0, ui_px(4.0f));
    {
        ImGui::SetNextItemWidth(ui_px(130.0f));
        if (ImGui::BeginCombo("##srv_sort", ("Sort: " + std::string(sort_label(s.sort_mode))).c_str())) {
            for (int i = 0; i < 4; ++i) {
                if (ImGui::Selectable(sort_label(i), s.sort_mode == i))
                    s.sort_mode = i;
            }
            ImGui::EndCombo();
        }
    }

    ImGui::SameLine(0, ui_px(12.0f));
    int shown = 0;
    for (const auto& sv : st.servers) {
        if (matches_filters(st, sv, s.filter_status, s.filter_software)) ++shown;
    }
    ImGui::TextColored(k.muted, "%d / %d servers", shown, (int)st.servers.size());

    ImGui::Spacing();

    // ── Server list ────────────────────────────────────────────────
    if (st.servers.empty()) {
        ImGui::Spacing();
        card_begin("##srv_empty", ImVec2(-1, ui_px(200.0f)));
        illustrated_empty_state(IconId::Server, "NO SERVERS YET",
                                "Create a local server to host Minecraft on this PC, or deploy a 24/7 server on the website.");
        ImGui::SetCursorPosX(
            ImGui::GetCursorPosX() +
            (ImGui::GetContentRegionAvail().x - ui_px(360.0f)) * 0.5f);
        if (primary_button("Create Your First Server",
                           ImVec2(ui_px(200.0f), ui_px(36.0f)))) {
            s.create_open = true;
        }
        ImGui::SameLine(0, ui_px(8.0f));
        if (ghost_button("Cloud Hosting",
                         ImVec2(ui_px(130.0f), ui_px(36.0f)))) {
            s.mode = 1;
        }
        card_end();
    } else {
        // Collect filtered indices for sorting
        std::vector<int> indices;
        for (int i = 0; i < static_cast<int>(st.servers.size()); ++i) {
            if (matches_filters(st, st.servers[i], s.filter_status, s.filter_software))
                indices.push_back(i);
        }

        // Sort
        std::sort(indices.begin(), indices.end(), [&](int a, int b) {
            const auto& sa = st.servers[a];
            const auto& sb = st.servers[b];
            switch (s.sort_mode) {
                case 1: return static_cast<int>(effective_stage(st, sa)) <
                               static_cast<int>(effective_stage(st, sb));
                case 2: return sa.minecraft_version < sb.minecraft_version;
                case 3: return sa.port < sb.port;
                default: return sa.name < sb.name;
            }
        });

        for (int idx : indices) {
            draw_server_card(st.servers[idx], idx, st);
            ImGui::Spacing();
        }

        if (shown == 0) {
            card_begin("##srv_no_match", ImVec2(-1, ui_px(80.0f)));
            empty_state("No matching servers", "Adjust your filters to see more servers.");
            card_end();
        }
    }

    // ── Delete confirmation ────────────────────────────────────────
    if (s.action_pending >= 0 && s.action_pending < static_cast<int>(st.servers.size())) {
        ImGui::OpenPopup("Confirm Delete");
    }
    if (ImGui::BeginPopupModal("Confirm Delete", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Delete server \"%s\"?",
            s.action_pending >= 0 && s.action_pending < static_cast<int>(st.servers.size())
                ? st.servers[s.action_pending].name.c_str() : "");
        ImGui::TextColored(k.muted, "This cannot be undone.");
        ImGui::Spacing();
        if (primary_button("Delete", ImVec2(ui_px(90.0f), ui_px(32.0f))) &&
            s.action_pending >= 0 && s.action_pending < static_cast<int>(st.servers.size())) {
            st.servers.erase(st.servers.begin() + s.action_pending);
            save_local_servers(st.servers);
            s.action_pending = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ghost_button("Cancel", ImVec2(ui_px(80.0f), ui_px(32.0f)))) {
            s.action_pending = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // ── Saved external servers (moved from old Connect tab) ────────
    if (!st.cfg->servers.empty()) {
        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::PushFont(f_h2);
        ImGui::TextColored(k.text, "SAVED SERVERS");
        ImGui::PopFont();
        ImGui::TextColored(k.muted, "External Minecraft server addresses for quick-connect.");
        ImGui::Spacing();

        for (size_t i = 0; i < st.cfg->servers.size(); ++i) {
            const auto& server = st.cfg->servers[i];
            ImGui::PushID(static_cast<int>(i + 9000));
            card_begin("##saved_srv", ImVec2(-1, 0));

            static const char* kTypeLabels[] = {"Java", "Bedrock", "Modded", "Bedrock Modded"};
            ImVec4 kTypeBadgeColors[] = {k.blue, k.green, k.brand, ImVec4(0.4f, 0.6f, 0.8f, 1.0f)};
            int type_idx = (server.type >= 0 && server.type < 4) ? server.type : 0;

            // Server icon tile
            const ImVec2 icon_pos = ImGui::GetCursorScreenPos();
            const float icon_r = ui_px(14.0f);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(icon_pos, icon_pos + ImVec2(icon_r * 2.0f, icon_r * 2.0f),
                              c32(k.surface), ui_px(7.0f));
            dl->AddRect(icon_pos, icon_pos + ImVec2(icon_r * 2.0f, icon_r * 2.0f),
                         c32(kTypeBadgeColors[type_idx]), ui_px(7.0f), 0, ui_px(1.0f));
            draw_icon(IconId::Server, icon_pos + ImVec2(icon_r, icon_r), ui_px(8.0f),
                      c32(kTypeBadgeColors[type_idx]));
            ImGui::Dummy(ImVec2(icon_r * 2.0f, icon_r * 2.0f));
            ImGui::SameLine(0, ui_px(10.0f));

            // Name + address
            ImGui::BeginGroup();
            ImGui::PushFont(f_bold);
            ImGui::TextColored(k.text, "%s", server.name.c_str());
            ImGui::PopFont();
            ImGui::TextColored(k.muted, "%s", net::to_utf8(server.address).c_str());
            ImGui::EndGroup();

            // Type badge + profile chip
            ImGui::SameLine(0, ui_px(10.0f));
            {
                const ImVec2 bp = ImGui::GetCursorScreenPos();
                const ImVec2 badge_sz = ImGui::CalcTextSize(kTypeLabels[type_idx]) +
                                        ImVec2(ui_px(12.0f), ui_px(6.0f));
                ImVec4 bg = kTypeBadgeColors[type_idx];
                bg.w = 0.15f;
                dl->AddRectFilled(bp, bp + badge_sz, c32(bg), ui_px(5.0f));
                dl->AddText(bp + ImVec2(ui_px(6.0f), ui_px(3.0f)),
                            c32(kTypeBadgeColors[type_idx]), kTypeLabels[type_idx]);
                ImGui::Dummy(badge_sz + ImVec2(0, ui_px(6.0f)));
            }
            if (!server.profile.empty()) {
                ImGui::SameLine(0, ui_px(6.0f));
                const ImVec2 bp = ImGui::GetCursorScreenPos();
                const ImVec2 badge_sz = ImGui::CalcTextSize("Profile") +
                                        ImVec2(ui_px(12.0f), ui_px(6.0f));
                dl->AddRectFilled(bp, bp + badge_sz, c32(ImVec4(k.brand.x, k.brand.y, k.brand.z, 0.15f)),
                                  ui_px(5.0f));
                dl->AddText(bp + ImVec2(ui_px(6.0f), ui_px(3.0f)), c32(k.brand), "Profile");
                ImGui::Dummy(badge_sz + ImVec2(0, ui_px(6.0f)));
            }

            // Actions
            ImGui::SameLine(ImGui::GetCursorPosX() +
                            std::max(0.0f, ImGui::GetContentRegionAvail().x - ui_px(230.0f)));
            if (ghost_button("Connect", ImVec2(ui_px(70.0f), ui_px(26.0f)))) {
                if (!server.profile.empty()) {
                    st.selected = server.profile;
                    st.active_instance_dir.clear();
                    for (auto& inst : st.instance_list) {
                        if (inst.id == server.profile) {
                            st.active_instance_dir = inst.directory;
                            break;
                        }
                    }
                }
                if (!st.active_instance_dir.empty() && !st.selected.empty()) {
                    st.pending_instance_dir = st.active_instance_dir;
                    st.pending_launch = true;
                    st.pending_id = st.selected;
                    st.pending_server = net::to_utf8(server.address);
                }
            }
            ImGui::SameLine(0, ui_px(4.0f));
            if (ghost_button("Copy", ImVec2(ui_px(60.0f), ui_px(24.0f)))) {
                ImGui::SetClipboardText(net::to_utf8(server.address).c_str());
            }
            ImGui::SameLine(0, ui_px(4.0f));
            if (ghost_button("Remove", ImVec2(ui_px(70.0f), ui_px(24.0f)))) {
                st.cfg->servers.erase(st.cfg->servers.begin() + i);
                config::save(st.exe_dir + L"\\launcher.json", *st.cfg);
                ImGui::PopID();
                break;  // index invalidated
            }

            card_end();
            ImGui::PopID();
        }
    }

    // ── Overlays ───────────────────────────────────────────────────
    draw_create_dialog(st);
    draw_console_panel(st);
    draw_properties_panel(st);
}

}  // namespace aml::ui
