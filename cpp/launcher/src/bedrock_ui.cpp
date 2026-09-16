#include "ui.h"
#include "ui_internal.h"
#include "bedrock_ui.h"
#include "bedrock.h"
#include "supabase.h"
#include "storage_manager.h"
#include "net.h"
#include "mods.h"
#include "provider_config.h"

#include <windows.h>
#include <shellapi.h>
#include <commdlg.h>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <chrono>
#include <random>
#include <filesystem>
#include <atomic>
#include <mutex>
#include <thread>
#include <map>

namespace aml::ui {

BedrockStats get_bedrock_stats(UiState& /*st*/,
    const std::vector<aml::bedrock::BedrockProfile>& profiles);
std::string generate_bedrock_id();
std::string format_current_timestamp();
void draw_bedrock_addons_installed(UiState& st);
void draw_bedrock_addons_discover(UiState& st);
void draw_bedrock_addons_import(UiState& st);

struct BedrockUIState {
    int profile_tab = 0;
    std::string profile_filter;
    int profile_sort = 0;
    std::string selected_profile_id;
    bool profile_creating = false;
    bool profile_editing = false;
    aml::bedrock::BedrockProfile editing_profile;
    std::string profile_error;
    std::string profile_success;

    int world_tab = 0;
    std::string world_filter;
    int world_sort = 0;
    std::string selected_world_id;
    bool world_editing = false;
    aml::bedrock::BedrockWorldEntry editing_world;
    std::string world_error;
    std::string world_success;

    int backup_tab = 0;
    std::string backup_filter;
    int backup_sort = 0;
    std::string selected_backup_id;
    std::string selected_backup_profile_id;
    std::string backup_label;
    std::string backup_error;
    std::string backup_success;

    int addon_tab = 0;
    std::string addon_filter;
    int addon_sort = 0;
    std::string selected_addon_id;
    std::string addon_error;
    std::string addon_success;

    bool auto_backups = true;
    int backup_interval = 7;
    int max_backups = 10;
    std::string backup_directory;

    int create_wizard_step = 0;

    std::string pending_delete_id;
    std::string pending_world_profile_id;
    std::string pending_world_id;
    std::string pending_restore_id;
    std::string pending_restore_backup_id;
    std::string pending_addon_profile_id;
    std::string pending_addon_filename;
    std::string pending_addon_uuid;
    aml::bedrock::BedrockPackType pending_addon_type = aml::bedrock::BedrockPackType::Behavior;
};

static BedrockUIState& get_bedrock_ui_state() {
    static BedrockUIState state;
    return state;
}

static std::vector<aml::bedrock::BedrockProfile> cached_bedrock_profiles() {
    static std::mutex cache_mu;
    static std::vector<aml::bedrock::BedrockProfile> cache;
    static std::atomic_bool loading{false};
    static uint64_t last_refresh = 0;
    const uint64_t now = GetTickCount64();
    bool start_load = false;
    {
        std::lock_guard<std::mutex> lock(cache_mu);
        if (!loading.load() && (cache.empty() || now - last_refresh > 5000)) {
            loading = true;
            start_load = true;
        }
    }
    if (start_load) {
        std::thread([] {
            auto profiles = aml::supabase::SupabaseManager::instance().get_bedrock_profiles();
            if (profiles.empty()) {
                const std::wstring data = aml::bedrock::data_dir(nullptr);
                if (!data.empty()) {
                    aml::bedrock::BedrockProfileManager local(data + L"\\profiles");
                    profiles = local.scan(nullptr);
                }
            }
            std::lock_guard<std::mutex> lock(cache_mu);
            cache = std::move(profiles);
            last_refresh = GetTickCount64();
            loading = false;
        }).detach();
    }
    std::lock_guard<std::mutex> lock(cache_mu);
    return cache;
}

static bool safe_bedrock_component(const std::string& value) {
    if (value.empty() || value == "." || value == "..") return false;
    for (unsigned char c : value) {
        if (c < 32 || c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|') return false;
    }
    return true;
}

static bool bedrock_profile_root(const aml::bedrock::BedrockProfile& profile,
                                 std::filesystem::path& output, std::string* err) {
    if (profile.id.empty() || !safe_bedrock_component(profile.id)) {
        if (err) *err = "invalid Bedrock profile id";
        return false;
    }
    const std::wstring data = aml::bedrock::data_dir(err);
    if (data.empty()) return false;
    std::error_code ec;
    output = std::filesystem::absolute(
        std::filesystem::path(data) / L"profiles" / aml::net::to_wide(profile.id), ec);
    if (ec) {
        if (err) *err = "cannot resolve Bedrock profile root: " + ec.message();
        return false;
    }
    return true;
}

static bool bedrock_addon_paths(const aml::bedrock::BedrockProfile& profile,
                                aml::bedrock::BedrockPackType type,
                                const std::string& filename,
                                std::filesystem::path& active,
                                std::filesystem::path& disabled,
                                std::string* err) {
    if (!safe_bedrock_component(filename)) {
        if (err) *err = "installed Bedrock add-on filename is missing or invalid";
        return false;
    }
    const wchar_t* content_root = nullptr;
    if (type == aml::bedrock::BedrockPackType::Behavior) content_root = L"behavior_packs";
    else if (type == aml::bedrock::BedrockPackType::Resource) content_root = L"resource_packs";
    else {
        if (err) *err = "unsupported Bedrock add-on type";
        return false;
    }

    std::filesystem::path profile_root;
    if (!bedrock_profile_root(profile, profile_root, err)) return false;
    active = profile_root / content_root / aml::net::to_wide(filename);
    // Keep disabled packs partitioned by type so behavior and resource packs
    // with the same installed directory cannot collide.
    disabled = profile_root / L"disabled_packs" / content_root / aml::net::to_wide(filename);
    return true;
}

static bool remove_bedrock_path(const std::filesystem::path& path, bool& removed,
                                std::string* err) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        if (ec) {
            if (err) *err = "cannot inspect Bedrock content: " + ec.message();
            return false;
        }
        return true;
    }
    const uintmax_t count = std::filesystem::remove_all(path, ec);
    if (ec) {
        if (err) *err = "cannot remove Bedrock content: " + ec.message();
        return false;
    }
    if (count == 0) {
        if (err) *err = "Bedrock content was not removed";
        return false;
    }
    removed = true;
    return true;
}

static bool draw_toggle(const char* id, bool value,
                        float width = 0.0f, float height = 0.0f) {
    if (width <= 0.0f) width = ui_px(40.0f);
    if (height <= 0.0f) height = ui_px(20.0f);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float radius = height * 0.5f;
    ImVec4 bg = value ? k.green : ImVec4(0.35f, 0.35f, 0.35f, 1.0f);
    dl->AddRectFilled(p, ImVec2(p.x + width, p.y + height),
                      c32(bg), height * 0.5f);
    float knob_x = value ? (p.x + width - radius - ui_px(2.0f)) : (p.x + radius + ui_px(2.0f));
    dl->AddCircleFilled(ImVec2(knob_x, p.y + radius),
                        radius - ui_px(3.0f), c32(ImVec4(1, 1, 1, 1)));
    if (ImGui::InvisibleButton(id, ImVec2(width, height))) return !value;
    return value;
}

static void draw_step_indicator(int current, const char** labels, int count) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 start = ImGui::GetCursorScreenPos();
    const float available = std::max(ui_px(260.0f), ImGui::GetContentRegionAvail().x);
    const float circle_r = ui_px(12.0f);
    const float spacing = count > 1
        ? std::max(ui_px(92.0f), (available - circle_r * 2.0f) / static_cast<float>(count - 1))
        : 0.0f;
    const float cy = start.y + circle_r;
    for (int i = 0; i < count; ++i) {
        const float cx = start.x + circle_r + i * spacing;
        bool done = i < current;
        bool active = i == current;
        ImVec4 col = done ? k.green : active ? k.brand_hov : ImVec4(k.muted.x, k.muted.y, k.muted.z, 0.72f);
        dl->AddCircleFilled(ImVec2(cx, cy), circle_r, c32(col));
        dl->AddCircle(ImVec2(cx, cy), circle_r, c32(ImVec4(k.text.x, k.text.y, k.text.z, 0.22f)), 0, ui_px(1.0f));
        if (done) {
            ImVec2 ts = ImGui::CalcTextSize("\xe2\x9c\x93");
            dl->AddText(ImVec2(cx - ts.x * 0.5f, cy - ts.y * 0.5f),
                        c32(k.text), "\xe2\x9c\x93");
        } else {
            char num[4];
            snprintf(num, sizeof(num), "%d", i + 1);
            ImGui::PushFont(f_small);
            ImVec2 ts = ImGui::CalcTextSize(num);
            dl->AddText(ImVec2(cx - ts.x * 0.5f, cy - ts.y * 0.5f),
                        c32(k.text), num);
            ImGui::PopFont();
        }
        if (i < count - 1) {
            const float x0 = cx + circle_r + ui_px(4.0f);
            const float x1 = cx + spacing - circle_r - ui_px(4.0f);
            ImVec4 lc = i < current ? k.green : k.border;
            dl->AddLine(ImVec2(x0, cy), ImVec2(x1, cy), c32(lc), ui_px(2.0f));
        }
    }
    for (int i = 0; i < count; ++i) {
        const float cx = start.x + circle_r + i * spacing;
        ImGui::PushFont(f_small);
        ImVec2 ts = ImGui::CalcTextSize(labels[i]);
        dl->AddText(ImVec2(cx - ts.x * 0.5f, cy + circle_r + ui_px(6.0f)),
                    c32(i <= current ? k.text : k.muted), labels[i]);
        ImGui::PopFont();
    }
    ImGui::Dummy(ImVec2(0, circle_r * 2 + ui_px(38.0f)));
}

static void draw_storage_segment_bar(uint64_t behavior, uint64_t resource,
                                     uint64_t saves, uint64_t other) {
    uint64_t total = behavior + resource + saves + other;
    if (total == 0) total = 1;
    float avail = ImGui::GetContentRegionAvail().x;
    float bar_h = ui_px(10.0f);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + avail, p.y + bar_h),
                      c32(k.surface2), bar_h * 0.5f);
    float x = p.x;
    float fracs[] = {
        (float)behavior / total, (float)resource / total,
        (float)saves / total, (float)other / total
    };
    ImVec4 cols[] = { k.blue, k.orange, k.green, k.muted };
    for (int i = 0; i < 4; ++i) {
        float w = avail * fracs[i];
        if (w > 0.5f)
            dl->AddRectFilled(ImVec2(x, p.y), ImVec2(x + w, p.y + bar_h),
                              c32(cols[i]), bar_h * 0.5f);
        x += w;
    }
    ImGui::Dummy(ImVec2(avail, bar_h + ui_px(8.0f)));
}

static uint64_t calculate_directory_size(const std::wstring& dir) {
    uint64_t total = 0;
    std::error_code ec;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, ec))
        if (entry.is_regular_file(ec)) total += entry.file_size(ec);
    return total;
}

static uint64_t cached_bedrock_directory_size(const std::wstring& dir) {
    struct Entry {
        uint64_t size = 0;
        uint64_t refreshed = 0;
        bool loading = false;
    };
    static std::mutex cache_mu;
    static std::map<std::wstring, Entry> cache;
    const uint64_t now = GetTickCount64();
    bool start = false;
    {
        std::lock_guard<std::mutex> lock(cache_mu);
        auto& entry = cache[dir];
        if (!entry.loading && (entry.refreshed == 0 || now - entry.refreshed > 5000)) {
            entry.loading = true;
            start = true;
        }
    }
    if (start) {
        std::thread([dir] {
            const uint64_t size = calculate_directory_size(dir);
            std::lock_guard<std::mutex> lock(cache_mu);
            auto& entry = cache[dir];
            entry.size = size;
            entry.refreshed = GetTickCount64();
            entry.loading = false;
        }).detach();
    }
    std::lock_guard<std::mutex> lock(cache_mu);
    return cache[dir].size;
}

static std::wstring bedrock_profile_art(const UiState& st,
                                        const aml::bedrock::BedrockProfile& profile) {
    if (!profile.banner_path.empty()) {
        std::wstring path = aml::net::to_wide(profile.banner_path);
        if (std::filesystem::exists(path)) return path;
        const std::wstring data = aml::bedrock::data_dir(nullptr);
        if (!data.empty()) {
            path = data + L"\\" + aml::net::to_wide(profile.banner_path);
            if (std::filesystem::exists(path)) return path;
        }
    }
    static const wchar_t* covers[] = {
        L"amalgam-cover-portal.png", L"amalgam-cover-forge.png", L"amalgam-cover-sky.png"
    };
    const size_t index = std::hash<std::string>{}(profile.id.empty() ? profile.name : profile.id) % 3;
    return st.exe_dir + L"\\branding\\" + covers[index];
}

static std::wstring bundled_bedrock_client_package(const UiState& st) {
    const std::wstring candidates[] = {
        st.exe_dir + L"\\bedrock\\AmalgamBedrockClient.mcaddon",
        st.exe_dir + L"\\..\\bedrock\\AmalgamBedrockClient.mcaddon",
        // Legacy beta builds used a versioned filename; keep for migration.
        st.exe_dir + L"\\bedrock\\AmalgamBedrockClient-3.0.0-beta.3.mcaddon"
    };
    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec)) return candidate;
    }
    return {};
}

static bool install_bundled_bedrock_client(UiState& st, const std::wstring& archive,
                                           const std::vector<aml::bedrock::BedrockProfile>& profiles,
                                           const std::string& selected_profile_id) {
    const aml::bedrock::BedrockProfile* target = nullptr;
    for (const auto& profile : profiles) {
        if (profile.id == selected_profile_id) { target = &profile; break; }
    }
    if (!target && !profiles.empty()) target = &profiles.front();
    if (!target) {
        push_notice(st, ui_model::NoticeLevel::Warning, "No Bedrock Profile",
                    "Create a Bedrock profile before installing the Amalgam Client");
        return false;
    }
    std::string error;
    auto result = aml::bedrock::import_addon(archive, *target, &error);
    if (!result.success) {
        push_notice(st, ui_model::NoticeLevel::Error, "Amalgam Client Install Failed",
                    error.empty() ? result.error : error);
        return false;
    }
    push_notice(st, ui_model::NoticeLevel::Success, "Amalgam Bedrock Client Installed",
                "Behavior and resource packs were added to the selected profile");
    return true;
}

static bool profile_has_amalgam_bedrock_client(
    const std::vector<aml::bedrock::BedrockProfile>& profiles) {
    for (const auto& profile : profiles) {
        for (const auto& pack : profile.packs) {
            if (pack.uuid == "7f1a1b32-7c09-4d65-9e3a-2abca9b5a201" ||
                pack.uuid == "5c1f0c38-6a62-43bb-b7c6-8d835d50a202" ||
                pack.name == "Amalgam Bedrock Behavior Pack" ||
                pack.name == "Amalgam Bedrock Resource Pack")
                return true;
        }
    }
    return false;
}

static bool import_bedrock_file(UiState& st, bool world) {
    wchar_t path[MAX_PATH] = {};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = st.hwnd;
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrFilter = world
        ? L"Bedrock Worlds\0*.mcworld\0All files\0*.*\0\0"
        : L"Bedrock Add-ons\0*.mcpack;*.mcaddon\0All files\0*.*\0\0";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&dialog)) return false;

    auto profiles = cached_bedrock_profiles();
    auto& ui = get_bedrock_ui_state();
    const aml::bedrock::BedrockProfile* target = nullptr;
    for (const auto& profile : profiles) {
        if (profile.id == ui.selected_profile_id) { target = &profile; break; }
    }
    if (!target && !profiles.empty()) target = &profiles.front();
    if (!target) {
        push_notice(st, ui_model::NoticeLevel::Warning, "No Bedrock Profile",
                    "Create a Bedrock profile before importing content");
        return false;
    }

    std::string error;
    const std::wstring source(path);
    bool ok = false;
    if (world) {
        const std::string name = std::filesystem::path(source).stem().string();
        ok = aml::bedrock::import_world_file(source, *target, name, &error);
    } else {
        auto result = aml::bedrock::import_addon(source, *target, &error);
        ok = result.success;
        if (!ok && error.empty()) error = result.error;
    }
    if (ok)
        push_notice(st, ui_model::NoticeLevel::Success, world ? "World Imported" : "Add-on Imported",
                    std::filesystem::path(source).filename().string());
    else
        push_notice(st, ui_model::NoticeLevel::Error, "Import Failed", error);
    return ok;
}
void draw_bedrock_overview(UiState& st) {
    auto& bedrock_ui = get_bedrock_ui_state();
    draw_page_emblem(st, "bedrock-emblem-ai.png");
    page_title("Bedrock", "Manage your Bedrock Edition profiles, worlds, and add-ons.");

    bool installed = aml::bedrock::installed();
    if (!installed) {
        card_begin("##bedrock_not_installed", ImVec2(-1, ui_px(260.0f)));
        ImVec2 panel = ImGui::GetCursorScreenPos();
        float content_w = ImGui::GetContentRegionAvail().x;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 icon_center(panel.x + content_w * 0.5f, panel.y + ui_px(58.0f));
        dl->AddCircleFilled(icon_center, ui_px(30.0f), c32(k.brand_dk));
        dl->AddRectFilled(ImVec2(icon_center.x - ui_px(17.0f), icon_center.y - ui_px(17.0f)),
                          ImVec2(icon_center.x + ui_px(17.0f), icon_center.y + ui_px(17.0f)),
                          c32(k.green), ui_px(4.0f));
        dl->AddRectFilled(ImVec2(icon_center.x - ui_px(17.0f), icon_center.y - ui_px(17.0f)),
                          ImVec2(icon_center.x + ui_px(17.0f), icon_center.y - ui_px(8.0f)),
                          c32(k.green), ui_px(3.0f));
        ImGui::Dummy(ImVec2(content_w, ui_px(104.0f)));
        ImGui::SetCursorPosX((content_w - ImGui::CalcTextSize("Minecraft for Windows isn't installed").x) * 0.5f);
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("Minecraft for Windows isn't installed");
        ImGui::PopFont();
        ImGui::SetCursorPosX((content_w - ImGui::CalcTextSize("Install Bedrock Edition to manage profiles, worlds, and add-ons here.").x) * 0.5f);
        ImGui::TextColored(k.muted, "Install Bedrock Edition to manage profiles, worlds, and add-ons here.");
        ImGui::Spacing();
        float button_w = ui_px(190.0f);
        ImGui::SetCursorPosX((content_w - button_w * 2.0f - ui_px(8.0f)) * 0.5f);
        if (primary_button("Install Bedrock", ImVec2(button_w, ui_px(34.0f))))
            ShellExecuteA(nullptr, "open", "ms-windows-store://pdp/?ProductId=9nblggh4ggsh",
                          nullptr, nullptr, SW_SHOWNORMAL);
        ImGui::SameLine(0, ui_px(8.0f));
        if (ghost_button("Check Again", ImVec2(button_w, ui_px(34.0f))))
            aml::bedrock::detect(nullptr);
        ImGui::Spacing();
        ImGui::SetCursorPosX((content_w - ImGui::CalcTextSize("After installation: open Bedrock once, then click Check Again.").x) * 0.5f);
        ImGui::TextColored(k.muted, "After installation: open Bedrock once, then click Check Again.");
        card_end();
        return;
    }

    // ── Detection Banner ──────────────────────────────────────────────────
    card_begin("##bedrock_detect", ImVec2(-1, ui_px(100.0f)));
        {
            ImVec2 cp = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(
            cp, ImVec2(cp.x + ui_px(4.0f), cp.y + ui_px(100.0f)),
            c32(k.green), ui_px(2.0f));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ui_px(12.0f));
        ImVec2 icon_pos = ImGui::GetCursorScreenPos();
        const ImVec2 icon_size(ui_px(48.0f), ui_px(48.0f));
        ImGui::Dummy(icon_size);
        const ImVec2 icon_end = icon_pos + icon_size;
        ImGui::GetWindowDrawList()->AddRectFilled(icon_pos, icon_end,
            c32(ImVec4(0.15f, 0.55f, 0.15f, 1.0f)), ui_px(8.0f));
        {
            ImVec2 ctr(icon_pos.x + ui_px(24.0f), icon_pos.y + ui_px(24.0f));
            float r = ui_px(13.0f);
            ImGui::GetWindowDrawList()->AddRectFilled(
                ImVec2(ctr.x - r, ctr.y - r), ImVec2(ctr.x + r, ctr.y + r),
                c32(ImVec4(0.35f, 0.25f, 0.15f, 1.0f)), ui_px(3.0f));
            ImGui::GetWindowDrawList()->AddRectFilled(
                ImVec2(ctr.x - r, ctr.y - r), ImVec2(ctr.x + r, ctr.y - ui_px(4.0f)),
                c32(ImVec4(0.25f, 0.65f, 0.25f, 1.0f)), ui_px(2.0f));
        }
        ImGui::SameLine();
        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, cp.y + ui_px(10.0f)));
        ImGui::PushFont(f_title);
        ImGui::TextUnformatted("Minecraft for Windows");
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::TextColored(k.green, "\xe2\x9c\x93 Detected");
        const auto data_path = aml::bedrock::detect();
        ImGui::TextColored(k.muted, "Edition: Bedrock for Windows  |  Package: Microsoft Store");
        if (!data_path.empty()) {
            const std::string path_text = aml::net::to_utf8(data_path);
            ImGui::TextColored(k.green, "Game data connected");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", path_text.c_str());
        } else {
            ImGui::TextColored(k.yellow, "Open Bedrock once to initialize game data");
        }
        const float btn_w = ui_px(160.0f);
        const float redetect_w = ui_px(100.0f);
        const float more_w = ui_px(38.0f);
        const float action_gap = ImGui::GetStyle().ItemSpacing.x;
        const float action_w = btn_w + redetect_w + more_w + action_gap * 2.0f;
        const float content_right = ImGui::GetWindowPos().x +
                                    ImGui::GetWindowContentRegionMax().x;
        ImGui::SetCursorScreenPos(ImVec2(content_right - action_w, cp.y + ui_px(14.0f)));
        if (primary_button("PLAY BEDROCK", ImVec2(btn_w, ui_px(36.0f)))) {
            std::string err;
            if (aml::bedrock::launch(&err))
                push_notice(st, ui_model::NoticeLevel::Success, "Launched", "Bedrock is launching");
            else
                push_notice(st, ui_model::NoticeLevel::Error, "Launch Failed", err);
        }
        ImGui::SameLine(0, action_gap);
        if (ghost_button("Re-detect", ImVec2(redetect_w, ui_px(36.0f)))) {
            std::string err;
            aml::bedrock::detect(&err);
        }
        ImGui::SameLine(0, action_gap);
        if (ghost_button("...", ImVec2(more_w, ui_px(36.0f))))
            push_notice(st, ui_model::NoticeLevel::Info, "Bedrock Details",
                        "Advanced diagnostics are available from Settings > Diagnostics");
    }
    card_end();
    ImGui::Spacing();

    // ── Amalgam Bedrock Client ───────────────────────────────────────────
    {
        const std::wstring client_package = bundled_bedrock_client_package(st);
        const auto profiles = cached_bedrock_profiles();
        const bool client_installed = profile_has_amalgam_bedrock_client(profiles);
        card_begin("##amalgam_bedrock_client", ImVec2(-1, ui_px(112.0f)));
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("Amalgam Bedrock Client");
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::TextColored(k.brand, "BETA");
        ImGui::TextColored(k.muted, "Supported .mcaddon companion: HUD, settings, diagnostics, and safe in-game menu.");
        if (client_package.empty())
            ImGui::TextColored(k.yellow, "Package not bundled with this build");
        else if (client_installed)
            ImGui::TextColored(k.green, "Installed in a Bedrock profile · ready to repair");
        else
            ImGui::TextColored(k.green, "Package ready to install");
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - ui_px(124.0f));
        if (!client_package.empty() && primary_button("Install / Repair", ImVec2(ui_px(116.0f), ui_px(32.0f))))
            install_bundled_bedrock_client(st, client_package, profiles, bedrock_ui.selected_profile_id);
        if (ImGui::IsItemHovered() && client_package.empty())
            ImGui::SetTooltip("This launcher build does not contain the Bedrock Client package yet.");
        card_end();
        ImGui::Spacing();
    }

    // ── Main: Profiles (left) + Quick Actions (right) ────────────────────
    float sidebar_w = ui_px(200.0f);
    float main_w = ImGui::GetContentRegionAvail().x - sidebar_w - ui_px(12.0f);
    ImGui::Columns(2, "##bedrock_main", false);
    ImGui::SetColumnWidth(0, main_w);
    ImGui::SetColumnWidth(1, sidebar_w);

    // LEFT: Profiles
    {
        auto profiles = cached_bedrock_profiles();
        ImGui::TextUnformatted("My Bedrock Profiles");
        ImGui::SameLine(main_w - ui_px(120.0f));
        ImGui::SetNextItemWidth(ui_px(110.0f));
        const char* sort_items[] = {"Last Played", "Name", "Date Created"};
        ImGui::Combo("##sort", &bedrock_ui.profile_sort, sort_items, 3);
        ImGui::SameLine();
        ghost_button("Grid", ImVec2(ui_px(48.0f), ui_px(26.0f)));
        ImGui::SameLine();
        ghost_button("List", ImVec2(ui_px(48.0f), ui_px(26.0f)));
        ImGui::Spacing();
        if (profiles.empty()) {
            // Make first run feel intentional: a single rich launch card uses
            // the real Bedrock scene shipped with the client rather than
            // leaving two thirds of the main canvas empty.
            card_begin("##bp_first_profile", ImVec2(main_w, ui_px(226.0f)));
            const ImVec2 hero = ImGui::GetCursorScreenPos();
            const ImVec2 hero_size(ImGui::GetContentRegionAvail().x, ui_px(194.0f));
            draw_local_image(st, st.exe_dir + L"\\branding\\ai\\profile-cover-bedrock-ai.png",
                             hero, hero_size, c32(k.brand_dk), ui_model::ImageFit::Cover);
            ImGui::GetWindowDrawList()->AddRectFilledMultiColor(
                hero, hero + hero_size,
                c32(ImVec4(k.sidebar.x, k.sidebar.y, k.sidebar.z, 0.92f)),
                c32(ImVec4(k.sidebar.x, k.sidebar.y, k.sidebar.z, 0.32f)),
                c32(ImVec4(k.sidebar.x, k.sidebar.y, k.sidebar.z, 0.28f)),
                c32(ImVec4(k.sidebar.x, k.sidebar.y, k.sidebar.z, 0.90f)));
            ImGui::SetCursorScreenPos(hero + ImVec2(ui_px(24.0f), ui_px(28.0f)));
            ImGui::PushFont(f_title);
            ImGui::TextUnformatted("Build your Bedrock library");
            ImGui::PopFont();
            ImGui::TextColored(k.muted, "Create a clean profile, then add worlds and approved Bedrock content.");
            ImGui::Spacing();
            if (primary_button("Create first profile", ImVec2(ui_px(176.0f), ui_px(34.0f)))) {
                bedrock_ui.profile_creating = true;
                bedrock_ui.editing_profile = aml::bedrock::BedrockProfile();
            }
            ImGui::SetCursorScreenPos(ImVec2(hero.x, hero.y + hero_size.y));
            ImGui::Dummy(ImVec2(0.0f, ui_px(2.0f)));
            card_end();
        } else {
        float card_w = (main_w - ui_px(16.0f)) / 3.0f;
        int col = 0;
        for (const auto& p : profiles) {
            if (col > 0) ImGui::SameLine(0, ui_px(8.0f));
            card_begin(("##bp_" + p.id).c_str(), ImVec2(card_w, ui_px(200.0f)));
            ImVec2 bp = ImGui::GetCursorScreenPos();
            ImVec2 bs(card_w - ui_px(16.0f), ui_px(80.0f));
            std::wstring banner = bedrock_profile_art(st, p);
            draw_local_image(st, banner, bp, bs, c32(k.surface));
            ImGui::GetWindowDrawList()->AddRectFilled(
                ImVec2(bp.x, bp.y + bs.y - ui_px(26.0f)),
                ImVec2(bp.x + bs.x, bp.y + bs.y), c32(ImVec4(0, 0, 0, 0.35f)));
            ImGui::Dummy(bs);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ui_px(4.0f));
            ImVec2 icon_pos = ImGui::GetCursorScreenPos();
            std::wstring icon = p.icon_path.empty() ? L"" : aml::net::to_wide(p.icon_path);
            if (!icon.empty()) {
                draw_local_image(st, icon, icon_pos,
                                 ImVec2(ui_px(32.0f), ui_px(32.0f)), c32(k.brand_dk));
                ImGui::Dummy(ImVec2(ui_px(32.0f), ui_px(32.0f)));
                ImGui::SameLine();
            }
            ImGui::PushFont(f_bold);
            ImGui::TextWrapped("%s", p.name.c_str());
            ImGui::PopFont();
            if (p.favorite) {
                ImGui::SameLine();
                ImGui::TextColored(k.brand, "\xe2\x98\x85");
            }
            ImGui::TextColored(k.muted, "%d Add-ons \xc2\xb7 %d Worlds",
                static_cast<int>(p.packs.size()), static_cast<int>(p.worlds.size()));
            if (!p.last_played.empty())
                ImGui::TextColored(k.green, "Last Played: %s", p.last_played.c_str());
            else
                ImGui::TextColored(k.muted, "Never played");
            ImGui::Spacing();
            if (primary_button(("PLAY##" + p.id).c_str(),
                               ImVec2(card_w - ui_px(40.0f), ui_px(30.0f)))) {
                bedrock_ui.selected_profile_id = p.id;
                std::string err;
                if (aml::bedrock::launch(&err))
                    push_notice(st, ui_model::NoticeLevel::Success, "Launched", p.name);
                else
                    push_notice(st, ui_model::NoticeLevel::Error, "Launch Failed", err);
            }
            ImGui::SameLine();
            if (ghost_button("...", ImVec2(ui_px(28.0f), ui_px(30.0f)))) {
                bedrock_ui.selected_profile_id = p.id;
                bedrock_ui.profile_editing = true;
                bedrock_ui.editing_profile = p;
            }
            card_end();
            col++;
            if (col >= 3) col = 0;
        }
        // Create New Profile card
        if (col > 0) ImGui::SameLine(0, ui_px(8.0f));
        card_begin("##bp_new", ImVec2(card_w, ui_px(200.0f)));
        {
            ImVec2 ncp(ImGui::GetCursorScreenPos());
            ImVec2 ctr(ncp.x + card_w * 0.5f, ncp.y + ui_px(70.0f));
            float r = ui_px(24.0f);
            ImGui::GetWindowDrawList()->AddCircleFilled(ctr, r, c32(k.brand));
            ImVec2 ts = ImGui::CalcTextSize("+");
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(ctr.x - ts.x * 0.5f, ctr.y - ts.y * 0.5f), c32(k.text), "+");
            ImGui::SetCursorScreenPos(ImVec2(ncp.x + ui_px(12.0f), ncp.y + ui_px(102.0f)));
            ImGui::PushFont(f_bold);
            ImGui::TextUnformatted("Create New Profile");
            ImGui::PopFont();
            ImGui::TextColored(k.muted, "Set up a new Bedrock profile");
            ImGui::TextColored(k.muted, "with your own content.");
            ImGui::Spacing();
            if (ghost_button(("Create Profile##" + std::to_string(col)).c_str(),
                             ImVec2(card_w - ui_px(40.0f), ui_px(28.0f)))) {
                bedrock_ui.profile_creating = true;
                bedrock_ui.editing_profile = aml::bedrock::BedrockProfile();
            }
        }
        card_end();
        }
    }

    ImGui::NextColumn();

    // RIGHT: Quick Actions
    {
        card_begin("##bedrock_quick_actions", ImVec2(-1, ui_px(330.0f)));
        ImGui::TextUnformatted("Quick Actions");
        ImGui::Spacing();
            auto qa = [&](const char* label, const char* sub) {
            float w = ImGui::GetContentRegionAvail().x;
            float h = ui_px(48.0f);
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton(("##qa_" + std::string(label)).c_str(), ImVec2(w, h));
            const bool clicked = ImGui::IsItemClicked();
            if (ImGui::IsItemHovered())
                ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + w, p.y + h),
                    c32(k.hover), ui_px(8.0f));
            ImGui::GetWindowDrawList()->AddRectFilled(
                ImVec2(p.x + ui_px(4.0f), p.y + ui_px(8.0f)),
                ImVec2(p.x + ui_px(36.0f), p.y + h - ui_px(8.0f)),
                c32(k.brand), ui_px(6.0f));
            ImVec2 ic(p.x + ui_px(20.0f), p.y + h * 0.5f);
            ImVec2 ti = ImGui::CalcTextSize("+");
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(ic.x - ti.x * 0.5f, ic.y - ti.y * 0.5f), c32(k.text), "+");
            ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + ui_px(44.0f), p.y + ui_px(8.0f)),
                c32(k.text), label);
            ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + ui_px(44.0f), p.y + ui_px(26.0f)),
                c32(k.muted), sub);
            ImGui::Dummy(ImVec2(w, h));
            if (clicked) {
                if (std::string(label) == "Import Add-on") import_bedrock_file(st, false);
                else if (std::string(label) == "Import World") import_bedrock_file(st, true);
                else if (std::string(label) == "Open Bedrock Folder") {
                    const std::wstring folder = aml::bedrock::data_dir(nullptr);
                    if (!folder.empty()) ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                } else if (std::string(label) == "Browse Content") {
                    bedrock_ui.profile_tab = 4;
                    bedrock_ui.addon_tab = 1;
                } else if (std::string(label) == "Manage Backups") {
                    bedrock_ui.profile_tab = 3;
                }
            }
        };
        qa("Import Add-on", ".mcpack / .mcaddon");
        ImGui::Spacing();
        qa("Import World", ".mcworld");
        ImGui::Spacing();
        qa("Browse Content", "Discover Bedrock add-ons");
        ImGui::Spacing();
        qa("Manage Backups", "View and restore backups");
        ImGui::Spacing();
        qa("Open Bedrock Folder", "Open Minecraft data folder");
        card_end();
    }

    ImGui::Columns(1);
    ImGui::Spacing();

    // ── Bottom Row ────────────────────────────────────────────────────────
    float col_w = (ImGui::GetContentRegionAvail().x - ui_px(24.0f)) / 4.0f;
    ImGui::Columns(4, "##bedrock_bottom", false);
    ImGui::SetColumnWidth(0, col_w);
    ImGui::SetColumnWidth(1, col_w);
    ImGui::SetColumnWidth(2, col_w);
    ImGui::SetColumnWidth(3, col_w);

    // Recent Activity
    card_begin("##bedrock_activity");
    ImGui::TextUnformatted("Recent Activity");
    ImGui::Separator();
    ImGui::Spacing();
    {
        auto profiles = cached_bedrock_profiles();
        std::vector<aml::bedrock::BedrockProfile> sorted = profiles;
        std::sort(sorted.begin(), sorted.end(),
            [](const auto& a, const auto& b) { return a.last_played_ts > b.last_played_ts; });
        int shown = 0;
        for (const auto& p : sorted) {
            if (shown >= 4 || p.last_played_ts <= 0) continue;
            ImGui::TextColored(k.green, "\xe2\x9c\x93");
            ImGui::SameLine();
            ImGui::Text("Played %s", p.name.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%s", format_date(p.last_played_ts).c_str());
            shown++;
        }
        if (shown == 0) ImGui::TextColored(k.muted, "No recent activity");
    }
    card_end();

    ImGui::NextColumn();

    // Storage Usage
    card_begin("##bedrock_storage2");
    ImGui::TextUnformatted("Storage Usage");
    ImGui::Separator();
    ImGui::Spacing();
    {
        uint64_t beh = 0, res = 0, sav = 0;
        // Measure managed profile directories.
        for (const auto& profile : cached_bedrock_profiles()) {
            std::filesystem::path profile_root;
            if (!bedrock_profile_root(profile, profile_root, nullptr)) continue;
            const std::wstring behavior = (profile_root / L"behavior_packs").wstring();
            const std::wstring resource = (profile_root / L"resource_packs").wstring();
            const std::wstring saves = (profile_root / L"saves").wstring();
            if (aml::net::directory_exists(behavior)) beh += cached_bedrock_directory_size(behavior);
            if (aml::net::directory_exists(resource)) res += cached_bedrock_directory_size(resource);
            if (aml::net::directory_exists(saves)) sav += cached_bedrock_directory_size(saves);
        }
        // When there are no managed profiles, measure the vanilla com.mojang
        // data directories directly so storage never shows 0 while content
        // actually exists on disk.
        bool measured_vanilla = false;
        if (cached_bedrock_profiles().empty()) {
            std::string dir_err;
            const std::wstring data = aml::bedrock::data_dir(&dir_err);
            if (!data.empty()) {
                const std::wstring behavior = data + L"\\behavior_packs";
                const std::wstring resource = data + L"\\resource_packs";
                // Bedrock stores worlds under minecraftWorlds, not saves.
                const std::wstring worlds = data + L"\\minecraftWorlds";
                if (aml::net::directory_exists(behavior)) beh += cached_bedrock_directory_size(behavior);
                if (aml::net::directory_exists(resource)) res += cached_bedrock_directory_size(resource);
                if (aml::net::directory_exists(worlds)) sav += cached_bedrock_directory_size(worlds);
                measured_vanilla = beh > 0 || res > 0 || sav > 0;
            }
        }
        uint64_t total = beh + res + sav;
        ImGui::PushFont(f_title);
        ImGui::Text("%s", format_bytes(total).c_str());
        ImGui::PopFont();
        ImGui::TextColored(k.muted, measured_vanilla
            ? "Total used by Bedrock on this PC"
            : "Total used by Bedrock profiles");
        ImGui::Spacing();
        draw_storage_segment_bar(beh, res, sav, 0);
        float tf = total > 0 ? static_cast<float>(total) : 1.0f;
        auto sl = [](const ImVec4& c, const char* l, uint64_t b, float pct) {
            ImGui::TextColored(c, "\xe2\x96\x88"); ImGui::SameLine(ui_px(20.0f));
            ImGui::Text("%s", l); ImGui::SameLine(ui_px(100.0f));
            ImGui::TextDisabled("%s", format_bytes(b).c_str()); ImGui::SameLine(ui_px(140.0f));
            ImGui::TextDisabled("%.0f%%", pct);
        };
        sl(k.blue, "Worlds", sav, sav * 100.0f / tf);
        sl(k.green, "Behavior Packs", beh, beh * 100.0f / tf);
        sl(k.orange, "Resource Packs", res, res * 100.0f / tf);
    }
    card_end();

    ImGui::NextColumn();

    // Updates Available
    card_begin("##bedrock_updates");
    ImGui::TextUnformatted("Updates Available");
    ImGui::Separator();
    ImGui::Spacing();
    {
        auto profiles = cached_bedrock_profiles();
        int total_packs = 0;
        for (const auto& p : profiles)
            total_packs += static_cast<int>(p.packs.size());
        if (total_packs == 0) {
            ImGui::TextColored(k.muted, "No add-ons installed");
        } else {
            ImGui::TextColored(k.muted, "%d add-on(s) installed", total_packs);
            ImGui::Spacing();
            ImGui::TextColored(k.muted, "Add-on update status: Not checked");
        }
    }
    card_end();

    ImGui::NextColumn();

    // No authoritative Bedrock news API is configured.
    card_begin("##bedrock_news");
    ImGui::TextUnformatted("Bedrock News");
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextColored(k.muted, "No authoritative Bedrock news is available.");
    ImGui::TextDisabled("News is not provided by the detected Windows package API.");
    card_end();

    ImGui::Columns(1);
}
void draw_bedrock_profiles(UiState& st) {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    auto& bedrock_ui = get_bedrock_ui_state();
    page_title("Bedrock Profiles", "Manage your Minecraft Bedrock profiles");
    if (!aml::bedrock::installed()) {
        empty_state("Bedrock Not Installed",
                    "Minecraft Bedrock Edition is not installed on this system.", "B");
        return;
    }
    auto profiles = cached_bedrock_profiles();
    card_begin("##bedrock_profiles_header");
    ImGui::TextUnformatted("Profiles");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - ui_px(200.0f));
    if (primary_button("+ Create Profile", ImVec2(ui_px(150.0f), ui_px(32.0f)))) {
        bedrock_ui.profile_creating = true;
        bedrock_ui.profile_tab = 1;
        bedrock_ui.create_wizard_step = 0;
    }
    ImGui::Spacing();
    ImGui::SetNextItemWidth(ui_px(240.0f));
    input_text_hint("##bedrock_profile_filter", "Filter profiles...", &bedrock_ui.profile_filter);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ui_px(160.0f));
    if (ImGui::BeginCombo("##bedrock_profile_sort",
        bedrock_ui.profile_sort == 0 ? "Name (A-Z)" :
        bedrock_ui.profile_sort == 1 ? "Recently Created" : "Recently Played")) {
        if (ImGui::Selectable("Name (A-Z)", bedrock_ui.profile_sort == 0)) bedrock_ui.profile_sort = 0;
        if (ImGui::Selectable("Recently Created", bedrock_ui.profile_sort == 1)) bedrock_ui.profile_sort = 1;
        if (ImGui::Selectable("Recently Played", bedrock_ui.profile_sort == 2)) bedrock_ui.profile_sort = 2;
        ImGui::EndCombo();
    }
    card_end();
    ImGui::Spacing();
    if (profiles.empty()) {
        empty_state("No Bedrock profiles yet",
                    "Bedrock profiles will appear here once you create them.", "P");
        return;
    }
    auto filtered = profiles;
    if (!bedrock_ui.profile_filter.empty()) {
        std::string f = bedrock_ui.profile_filter;
        std::transform(f.begin(), f.end(), f.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        filtered.erase(std::remove_if(filtered.begin(), filtered.end(),
            [&f](const auto& p) {
                std::string n = p.name;
                std::transform(n.begin(), n.end(), n.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                return n.find(f) == std::string::npos;
            }), filtered.end());
    }
    std::sort(filtered.begin(), filtered.end(),
        [&bedrock_ui](const auto& a, const auto& b) {
            switch (bedrock_ui.profile_sort) {
                case 1: return a.created > b.created;
                case 2: return a.last_played_ts > b.last_played_ts;
                default: return a.name < b.name;
            }
        });
    if (filtered.empty() && !profiles.empty()) {
        empty_state("No matching profiles",
                    "Try adjusting your search or filter criteria.", "P");
        return;
    }
    if (ImGui::BeginPopupModal("Delete Profile?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Are you sure you want to delete this profile?");
        ImGui::TextColored(k.muted, "This action cannot be undone.");
        ImGui::Spacing();
        if (primary_button("Delete", ImVec2(ui_px(100.0f), ui_px(32.0f)))) {
            if (supabase.delete_bedrock_profile(bedrock_ui.pending_delete_id))
                push_notice(st, ui_model::NoticeLevel::Success, "Profile Deleted", "Profile deleted successfully");
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ghost_button("Cancel", ImVec2(ui_px(100.0f), ui_px(32.0f))))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    for (auto& profile : filtered) {
        ImGui::PushID(profile.id.c_str());
        card_begin(("##bedrock_profile_" + profile.id).c_str(), ImVec2(-1, ui_px(96.0f)));
        ImVec2 cp = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(cp, ImVec2(cp.x + ui_px(4.0f), cp.y + ui_px(96.0f)),
            c32(profile.favorite ? ImVec4(1.0f, 0.85f, 0.0f, 1.0f) : k.brand), ui_px(2.0f));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ui_px(14.0f));
        ImGui::PushFont(f_bold);
        ImGui::TextUnformatted(profile.name.c_str());
        ImGui::PopFont();
        ImGui::SameLine();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 bp = ImGui::GetCursorScreenPos();
        draw_badge(dl, bp, profile.minecraft_version.c_str(), k.text, k.blue);
        ImGui::Dummy(ImVec2(ImGui::CalcTextSize(profile.minecraft_version.c_str()).x + 14.0f, ui_px(20.0f)));
        // Stat chips: worlds / addons / last played
        {
            ImDrawList* dl2 = ImGui::GetWindowDrawList();
            auto chip = [&](const char* text, const ImVec4& col) {
                ImVec2 chip_pos = ImGui::GetCursorScreenPos();
                const float chip_w = ImGui::CalcTextSize(text).x + ui_px(14.0f);
                ImVec4 bg = col; bg.w = 0.12f;
                dl2->AddRectFilled(chip_pos, ImVec2(chip_pos.x + chip_w, chip_pos.y + ui_px(20.0f)),
                                   c32(bg), ui_px(10.0f));
                dl2->AddText(ImVec2(chip_pos.x + ui_px(7.0f), chip_pos.y + ui_px(3.0f)), c32(col), text);
                ImGui::Dummy(ImVec2(chip_w, ui_px(20.0f)));
            };
            chip((std::to_string(profile.worlds.size()) + " worlds").c_str(), k.blue);
            ImGui::SameLine(0, ui_px(6.0f));
            chip((std::to_string(profile.packs.size()) + " addons").c_str(), k.green);
            ImGui::SameLine(0, ui_px(6.0f));
            chip((std::string("Played ") +
                  (profile.last_played_ts > 0 ? format_date(profile.last_played_ts) : "Never")).c_str(),
                 k.muted);
        }
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ui_px(80.0f));
        if (icon_button(profile.favorite ? IconId::Star : IconId::Star,
                        ImVec2(ui_px(30.0f), ui_px(30.0f)),
                        profile.favorite ? "Remove from favorites" : "Add to favorites",
                        profile.favorite ? k.brand_hov : k.muted)) {
            profile.favorite = !profile.favorite;
            supabase.update_bedrock_profile(profile);
        }
        ImGui::SameLine();
        if (icon_button(IconId::More, ImVec2(ui_px(30.0f), ui_px(30.0f)),
                        "More profile actions"))
            ImGui::OpenPopup(("##pmenu_" + profile.id).c_str());
        if (ImGui::BeginPopup(("##pmenu_" + profile.id).c_str())) {
            if (ImGui::MenuItem("Edit")) {
                bedrock_ui.profile_editing = true;
                bedrock_ui.editing_profile = profile;
                bedrock_ui.profile_tab = 2;
            }
            if (ImGui::MenuItem("Duplicate")) {
                auto np = profile;
                np.id.clear();
                np.name += " (Copy)";
                if (!supabase.create_bedrock_profile(np).id.empty())
                    push_notice(st, ui_model::NoticeLevel::Success, "Profile Duplicated", "Profile duplicated");
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Delete", nullptr, false, !profile.favorite)) {
                bedrock_ui.pending_delete_id = profile.id;
                request_popup("Delete Profile?");
            }
            ImGui::EndPopup();
        }
        card_end();
        ImGui::PopID();
    }
}
void draw_bedrock_create_profile(UiState& /*st*/) {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    auto& bedrock_ui = get_bedrock_ui_state();
    page_title("Create Bedrock Profile", "Build an isolated Bedrock profile without changing your existing installation.");
    const char* step_labels[] = {"Name", "Version", "Create"};
    card_begin("##bedrock_create_hero", ImVec2(-1, ui_px(86.0f)));
    const ImVec2 hero_origin = ImGui::GetCursorScreenPos();
    ImDrawList* hero_draw = ImGui::GetWindowDrawList();
    hero_draw->AddCircleFilled(hero_origin + ImVec2(ui_px(28.0f), ui_px(28.0f)), ui_px(24.0f),
                               c32(ImVec4(k.brand_dk.x, k.brand_dk.y, k.brand_dk.z, 0.92f)));
    draw_icon(IconId::Cube, hero_origin + ImVec2(ui_px(28.0f), ui_px(28.0f)), ui_px(13.0f), c32(k.brand_hov));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ui_px(62.0f));
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted("Bedrock profile setup");
    ImGui::PopFont();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ui_px(62.0f));
    ImGui::TextColored(k.muted, "Step %d of 3  •  Reversible, local, and ready for the launcher", bedrock_ui.create_wizard_step + 1);
    card_end();
    ImGui::Spacing();
    draw_step_indicator(bedrock_ui.create_wizard_step, step_labels, 3);
    ImGui::Spacing();
    card_begin("##bedrock_create_profile");
    if (bedrock_ui.create_wizard_step == 0) {
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("Name your profile");
        ImGui::PopFont();
        ImGui::TextColored(k.muted, "Use a name that makes this world easy to find in your library.");
        ImGui::Spacing();
        ImGui::PushFont(f_bold);
        ImGui::TextUnformatted("Profile name");
        ImGui::PopFont();
        ImGui::SetNextItemWidth(-1);
        input_text_hint("##create_profile_name", "For example: Survival Realm", &bedrock_ui.editing_profile.name);
        if (bedrock_ui.editing_profile.name.empty())
            ImGui::TextColored(k.muted, "A short, unique name works best.");
        else
            ImGui::TextColored(k.green, "Ready: %s", bedrock_ui.editing_profile.name.c_str());
    } else if (bedrock_ui.create_wizard_step == 1) {
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("Confirm Bedrock version");
        ImGui::PopFont();
        ImGui::TextColored(k.muted, "The launcher will use the locally detected Bedrock installation when it is available.");
        ImGui::Spacing();
        ImGui::TextColored(k.muted, "MINECRAFT BEDROCK VERSION");
        const char* version = bedrock_ui.editing_profile.minecraft_version.empty()
            ? "Unavailable: no authoritative Bedrock version API is configured."
            : bedrock_ui.editing_profile.minecraft_version.c_str();
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted(version);
        ImGui::PopFont();
        ImGui::Spacing();
        ImGui::TextColored(k.muted, "You can still create the profile; Amalgam will check the installation before launch.");
    } else if (bedrock_ui.create_wizard_step == 2) {
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("Review and create");
        ImGui::PopFont();
        ImGui::TextColored(k.muted, "Nothing in an existing profile will be replaced.");
        ImGui::Spacing();
        card_begin("##bedrock_profile_review", ImVec2(-1, 0));
        ImGui::TextColored(k.muted, "PROFILE NAME");
        ImGui::PushFont(f_bold); ImGui::TextUnformatted(bedrock_ui.editing_profile.name.c_str()); ImGui::PopFont();
        ImGui::Spacing();
        ImGui::TextColored(k.muted, "BEDROCK VERSION");
        ImGui::PushFont(f_bold);
        ImGui::TextUnformatted(bedrock_ui.editing_profile.minecraft_version.empty()
            ? "Detect before launch" : bedrock_ui.editing_profile.minecraft_version.c_str());
        ImGui::PopFont();
        card_end();
    }
    if (!bedrock_ui.profile_error.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(k.red, "PROFILE SETUP NEEDS ATTENTION");
        ImGui::TextWrapped("%s", bedrock_ui.profile_error.c_str());
    }
    if (!bedrock_ui.profile_success.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(k.green, "PROFILE READY");
        ImGui::TextWrapped("%s", bedrock_ui.profile_success.c_str());
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (bedrock_ui.create_wizard_step == 0) {
        if (ghost_button("Cancel", ImVec2(ui_px(100.0f), ui_px(36.0f)))) {
            bedrock_ui.profile_creating = false;
            bedrock_ui.profile_error.clear();
            bedrock_ui.editing_profile = aml::bedrock::BedrockProfile();
            bedrock_ui.create_wizard_step = 0;
        }
        ImGui::SameLine();
        if (primary_button("Continue", ImVec2(ui_px(132.0f), ui_px(36.0f)))) {
            if (bedrock_ui.editing_profile.name.empty())
                bedrock_ui.profile_error = "Profile name is required";
            else { bedrock_ui.profile_error.clear(); bedrock_ui.create_wizard_step = 1; }
        }
    } else if (bedrock_ui.create_wizard_step == 1) {
        if (ghost_button("Back", ImVec2(ui_px(100.0f), ui_px(36.0f))))
            bedrock_ui.create_wizard_step = 0;
        ImGui::SameLine();
        if (ghost_button("Cancel", ImVec2(ui_px(100.0f), ui_px(36.0f)))) {
            bedrock_ui.profile_creating = false;
            bedrock_ui.editing_profile = aml::bedrock::BedrockProfile();
            bedrock_ui.create_wizard_step = 0;
        }
        ImGui::SameLine();
        if (primary_button("Review profile", ImVec2(ui_px(150.0f), ui_px(36.0f)))) {
            bedrock_ui.create_wizard_step = 2;
        }
    } else if (bedrock_ui.create_wizard_step == 2) {
        if (ghost_button("Back", ImVec2(ui_px(100.0f), ui_px(36.0f))))
            bedrock_ui.create_wizard_step = 1;
        ImGui::SameLine();
        if (ghost_button("Cancel", ImVec2(ui_px(100.0f), ui_px(36.0f)))) {
            bedrock_ui.profile_creating = false;
            bedrock_ui.editing_profile = aml::bedrock::BedrockProfile();
            bedrock_ui.create_wizard_step = 0;
        }
        ImGui::SameLine();
        if (primary_button("Create Bedrock profile", ImVec2(ui_px(190.0f), ui_px(36.0f)))) {
            bedrock_ui.editing_profile.id = generate_bedrock_id();
            bedrock_ui.editing_profile.created = format_current_timestamp();
            bedrock_ui.editing_profile.last_played = bedrock_ui.editing_profile.created;
            bedrock_ui.editing_profile.last_played_ts = std::time(nullptr);
            if (bedrock_ui.editing_profile.minecraft_version.empty())
                bedrock_ui.editing_profile.minecraft_version = "Unavailable";
            auto created = supabase.create_bedrock_profile(bedrock_ui.editing_profile);
            if (!created.id.empty()) {
                bedrock_ui.profile_success = "Profile created successfully!";
                bedrock_ui.profile_creating = false;
                bedrock_ui.editing_profile = aml::bedrock::BedrockProfile();
                bedrock_ui.create_wizard_step = 0;
            } else {
                bedrock_ui.profile_error = "We could not create this Bedrock profile. Check the launcher connection, then try again.";
            }
        }
    }
    card_end();
}

void draw_bedrock_edit_profile(UiState& /*st*/) {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    auto& bedrock_ui = get_bedrock_ui_state();
    page_title("Edit Bedrock Profile", "Edit your Minecraft Bedrock profile");
    card_begin("##bedrock_edit_profile");
    ImGui::TextUnformatted("Profile Information");
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextUnformatted("Profile Name");
    ImGui::SetNextItemWidth(ui_px(360.0f));
    ImGui::InputText("##edit_profile_name", &bedrock_ui.editing_profile.name);
    ImGui::Spacing();
    ImGui::TextUnformatted("Minecraft Version");
    const char* version = bedrock_ui.editing_profile.minecraft_version.empty()
        ? "Unavailable: no authoritative Bedrock version API is configured."
        : bedrock_ui.editing_profile.minecraft_version.c_str();
    ImGui::TextColored(k.muted, "%s", version);
    ImGui::Spacing();
    ImGui::Checkbox("Favorite", &bedrock_ui.editing_profile.favorite);
    ImGui::Spacing();
    if (!bedrock_ui.profile_error.empty()) {
        ImGui::TextColored(k.red, "%s", bedrock_ui.profile_error.c_str());
        ImGui::Spacing();
    }
    if (!bedrock_ui.profile_success.empty()) {
        ImGui::TextColored(k.green, "%s", bedrock_ui.profile_success.c_str());
        ImGui::Spacing();
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (ghost_button("Cancel", ImVec2(ui_px(100.0f), ui_px(34.0f)))) {
        bedrock_ui.profile_editing = false;
        bedrock_ui.profile_error.clear();
        bedrock_ui.profile_success.clear();
        bedrock_ui.editing_profile = aml::bedrock::BedrockProfile();
    }
    ImGui::SameLine();
    if (primary_button("Save Changes", ImVec2(ui_px(140.0f), ui_px(34.0f)))) {
        if (bedrock_ui.editing_profile.name.empty())
            bedrock_ui.profile_error = "Profile name is required";
        else {
            bedrock_ui.profile_error.clear();
            bedrock_ui.editing_profile.updated_at = format_current_timestamp();
            if (supabase.update_bedrock_profile(bedrock_ui.editing_profile)) {
                bedrock_ui.profile_success = "Profile updated successfully!";
                bedrock_ui.profile_editing = false;
                bedrock_ui.editing_profile = aml::bedrock::BedrockProfile();
            } else {
                bedrock_ui.profile_error = "Failed to update profile";
            }
        }
    }
    card_end();
}
void draw_bedrock_worlds(UiState& st) {
    auto& bedrock_ui = get_bedrock_ui_state();
    page_title("Bedrock Worlds", "Manage your Minecraft Bedrock worlds");
    if (!aml::bedrock::installed()) {
        empty_state("Bedrock Not Installed",
                    "Minecraft Bedrock Edition is not installed on this system.", "B");
        return;
    }
    std::vector<aml::bedrock::BedrockWorldEntry> all_worlds;
    auto profiles = cached_bedrock_profiles();
    for (const auto& profile : profiles) {
        for (const auto& world : profile.worlds) {
            auto wc = world;
            wc.profile_id = profile.id;
            wc.profile_name = profile.name;
            all_worlds.push_back(wc);
        }
    }
    card_begin("##bedrock_worlds_header");
    ImGui::TextUnformatted("Worlds");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - ui_px(200.0f));
    ImGui::TextColored(k.muted, "Create worlds in Bedrock");
    ImGui::Spacing();
    ImGui::SetNextItemWidth(ui_px(240.0f));
    input_text_hint("##bedrock_world_filter", "Filter worlds...", &bedrock_ui.world_filter);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ui_px(160.0f));
    if (ImGui::BeginCombo("##bedrock_world_sort",
        bedrock_ui.world_sort == 0 ? "Name (A-Z)" :
        bedrock_ui.world_sort == 1 ? "Recently Created" : "Recently Played")) {
        if (ImGui::Selectable("Name (A-Z)", bedrock_ui.world_sort == 0)) bedrock_ui.world_sort = 0;
        if (ImGui::Selectable("Recently Created", bedrock_ui.world_sort == 1)) bedrock_ui.world_sort = 1;
        if (ImGui::Selectable("Recently Played", bedrock_ui.world_sort == 2)) bedrock_ui.world_sort = 2;
        ImGui::EndCombo();
    }
    card_end();
    ImGui::Spacing();
    if (all_worlds.empty()) {
        empty_state("No Bedrock worlds yet",
                    "Worlds will appear here once you create them in the game.", "W");
        return;
    }
    auto filtered = all_worlds;
    if (!bedrock_ui.world_filter.empty()) {
        std::string f = bedrock_ui.world_filter;
        std::transform(f.begin(), f.end(), f.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        filtered.erase(std::remove_if(filtered.begin(), filtered.end(),
            [&f](const auto& w) {
                std::string n = w.name;
                std::transform(n.begin(), n.end(), n.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                return n.find(f) == std::string::npos;
            }), filtered.end());
    }
    std::sort(filtered.begin(), filtered.end(),
        [&bedrock_ui](const auto& a, const auto& b) {
            switch (bedrock_ui.world_sort) {
                case 1: return a.folder > b.folder;
                case 2: return a.last_played > b.last_played;
                default: return a.name < b.name;
            }
        });
    if (filtered.empty() && !all_worlds.empty()) {
        empty_state("No matching worlds",
                    "Try adjusting your search or filter criteria.", "W");
        return;
    }
    if (ImGui::BeginPopupModal("Delete World?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Are you sure you want to delete this world?");
        ImGui::TextColored(k.muted, "This action cannot be undone.");
        ImGui::Spacing();
        if (primary_button("Delete", ImVec2(ui_px(100.0f), ui_px(32.0f)))) {
            aml::bedrock::BedrockProfile target;
            for (const auto& p : profiles) {
                if (p.id == bedrock_ui.pending_world_profile_id) {
                    target = p;
                    break;
                }
            }
            std::string werr;
            std::filesystem::path profile_root;
            bool deleted = false;
            if (target.id.empty()) {
                werr = "selected Bedrock profile is no longer available";
            } else if (bedrock_profile_root(target, profile_root, &werr) &&
                       safe_bedrock_component(bedrock_ui.pending_world_id)) {
                const std::filesystem::path world_dir = profile_root / L"saves" /
                    aml::net::to_wide(bedrock_ui.pending_world_id);
                std::error_code ec;
                if (!std::filesystem::exists(world_dir, ec)) {
                    werr = ec ? "cannot inspect Bedrock world: " + ec.message()
                              : "Bedrock world not found: " + world_dir.string();
                } else {
                    const uintmax_t count = std::filesystem::remove_all(world_dir, ec);
                    if (ec) werr = "cannot delete Bedrock world: " + ec.message();
                    else if (count == 0) werr = "Bedrock world was not deleted";
                    else deleted = true;
                }
            } else if (werr.empty()) {
                werr = "invalid Bedrock world folder";
            }
            if (deleted)
                push_notice(st, ui_model::NoticeLevel::Success, "World Deleted", "World deleted successfully");
            else
                push_notice(st, ui_model::NoticeLevel::Error, "Delete Failed", werr);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ghost_button("Cancel", ImVec2(ui_px(100.0f), ui_px(32.0f))))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    for (auto& world : filtered) {
        ImGui::PushID(world.folder.c_str());
        card_begin(("##bedrock_world_" + world.folder).c_str(), ImVec2(-1, ui_px(88.0f)));
        ImVec2 wp = ImGui::GetCursorScreenPos();
        ImVec4 icon_colors[] = { k.blue, k.green, k.orange, k.brand_dk };
        int ci = std::hash<std::string>{}(world.folder) % 4;
        ImGui::GetWindowDrawList()->AddRectFilled(wp, ImVec2(wp.x + ui_px(56.0f), wp.y + ui_px(56.0f)),
                                                  c32(icon_colors[ci]), ui_px(8.0f));
        ImGui::GetWindowDrawList()->AddText(ImVec2(wp.x + ui_px(20.0f), wp.y + ui_px(18.0f)),
                                            c32(k.text), "W");
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ui_px(68.0f));
        ImGui::PushFont(f_bold);
        ImGui::TextUnformatted(world.name.c_str());
        ImGui::PopFont();
        ImGui::SameLine();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 dbp = ImGui::GetCursorScreenPos();
        ImVec4 dim_col = k.muted;
        if (world.dimension == "overworld") dim_col = k.green;
        else if (world.dimension == "nether") dim_col = k.orange;
        else if (world.dimension == "the_end") dim_col = k.blue;
        draw_badge(dl, dbp, world.dimension.c_str(), k.text, dim_col);
        ImGui::Dummy(ImVec2(ImGui::CalcTextSize(world.dimension.c_str()).x + 14.0f, ui_px(20.0f)));
        ImGui::TextColored(k.muted, "%s  |  %s  |  %s",
                          world.profile_name.c_str(), format_bytes(world.size_bytes).c_str(),
                          world.last_played.empty() ? "Never played" : world.last_played.c_str());
        float btn_w = ui_px(80.0f);
        float total_btns = btn_w * 2 + ui_px(20.0f);
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - total_btns - ui_px(10.0f));
        if (ghost_button("Backup", ImVec2(btn_w, ui_px(28.0f)))) {
            aml::bedrock::BedrockProfile target;
            for (const auto& p : profiles) if (p.id == world.profile_id) { target = p; break; }
            std::wstring out_path;
            std::string berr;
            if (aml::bedrock::backup_world(target, world.folder, out_path, &berr))
                push_notice(st, ui_model::NoticeLevel::Success, "Backup Created", "World backup created");
            else
                push_notice(st, ui_model::NoticeLevel::Error, "Backup Failed", berr);
        }
        ImGui::SameLine();
        if (ghost_button("Delete", ImVec2(btn_w, ui_px(28.0f)))) {
            bedrock_ui.pending_world_profile_id = world.profile_id;
            bedrock_ui.pending_world_id = world.folder;
            request_popup("Delete World?");
        }
        card_end();
        ImGui::PopID();
    }
}
void draw_bedrock_backups(UiState& st) {
    (void)aml::storage::StorageManager::instance();
    auto& bedrock_ui = get_bedrock_ui_state();
    page_title("Bedrock Backups", "Manage your Minecraft Bedrock world backups");
    if (!aml::bedrock::installed()) {
        empty_state("Bedrock Not Installed",
                    "Minecraft Bedrock Edition is not installed on this system.", "B");
        return;
    }
    card_begin("##bedrock_backups_header");
    ImGui::TextUnformatted("Backups");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - ui_px(200.0f));
    ImGui::TextColored(k.muted, "Use a world's Backup action");
    ImGui::Spacing();
    ImGui::SetNextItemWidth(ui_px(240.0f));
    input_text_hint("##bedrock_backup_filter", "Filter backups...", &bedrock_ui.backup_filter);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ui_px(160.0f));
    if (ImGui::BeginCombo("##bedrock_backup_sort",
        bedrock_ui.backup_sort == 0 ? "Newest First" :
        bedrock_ui.backup_sort == 1 ? "Oldest First" : "Largest First")) {
        if (ImGui::Selectable("Newest First", bedrock_ui.backup_sort == 0)) bedrock_ui.backup_sort = 0;
        if (ImGui::Selectable("Oldest First", bedrock_ui.backup_sort == 1)) bedrock_ui.backup_sort = 1;
        if (ImGui::Selectable("Largest First", bedrock_ui.backup_sort == 2)) bedrock_ui.backup_sort = 2;
        ImGui::EndCombo();
    }
    card_end();
    ImGui::Spacing();
    std::vector<aml::bedrock::BedrockBackupEntry> backups;
    auto profiles = cached_bedrock_profiles();
    for (const auto& profile : profiles) {
        std::string berr;
        auto pb = aml::bedrock::list_backups(profile, &berr);
        for (auto& b : pb) { b.profile_id = profile.id; backups.push_back(b); }
    }
    if (backups.empty()) {
        empty_state("No Bedrock backups yet",
                    "Backups will appear here once you create them.", "B");
        return;
    }
    auto filtered = backups;
    if (!bedrock_ui.backup_filter.empty()) {
        std::string f = bedrock_ui.backup_filter;
        std::transform(f.begin(), f.end(), f.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        filtered.erase(std::remove_if(filtered.begin(), filtered.end(),
            [&f](const auto& b) {
                std::string l = b.label;
                std::transform(l.begin(), l.end(), l.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                return l.find(f) == std::string::npos;
            }), filtered.end());
    }
    std::sort(filtered.begin(), filtered.end(),
        [&bedrock_ui](const auto& a, const auto& b) {
            switch (bedrock_ui.backup_sort) {
                case 1: return a.timestamp < b.timestamp;
                case 2: return a.size_bytes < b.size_bytes;
                default: return a.timestamp > b.timestamp;
            }
        });
    if (filtered.empty() && !backups.empty()) {
        empty_state("No matching backups",
                    "Try adjusting your search or filter criteria.", "B");
        return;
    }
    if (ImGui::BeginPopupModal("Restore Backup?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Restore this backup? The current world data will be replaced.");
        ImGui::TextColored(k.muted, "This action cannot be undone.");
        ImGui::Spacing();
        if (primary_button("Restore", ImVec2(ui_px(100.0f), ui_px(32.0f)))) {
            std::string rerr;
            aml::bedrock::BedrockProfile target;
            for (const auto& p : profiles) {
                if (p.id == bedrock_ui.pending_restore_id) {
                    target = p;
                    break;
                }
            }
            bool found = false;
            for (const auto& b : backups) {
                if (b.profile_id == bedrock_ui.pending_restore_id &&
                    b.id == bedrock_ui.pending_restore_backup_id) {
                    found = true;
                    if (aml::bedrock::restore_backup(target, b, &rerr))
                        push_notice(st, ui_model::NoticeLevel::Success, "Backup Restored", "World backup restored");
                    else
                        push_notice(st, ui_model::NoticeLevel::Error, "Restore Failed", rerr);
                    break;
                }
            }
            if (!found)
                push_notice(st, ui_model::NoticeLevel::Error, "Restore Failed", "Selected backup is no longer available");
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ghost_button("Cancel", ImVec2(ui_px(100.0f), ui_px(32.0f))))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("Delete Backup?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Are you sure you want to delete this backup?");
        ImGui::TextColored(k.muted, "This action cannot be undone.");
        ImGui::Spacing();
        if (primary_button("Delete", ImVec2(ui_px(100.0f), ui_px(32.0f)))) {
            std::string derr;
            aml::bedrock::BedrockProfile target;
            for (const auto& p : profiles) {
                if (p.id == bedrock_ui.selected_backup_profile_id) {
                    target = p;
                    break;
                }
            }
            bool found = false;
            for (const auto& b : backups) {
                if (b.profile_id == bedrock_ui.selected_backup_profile_id &&
                    b.id == bedrock_ui.selected_backup_id) {
                    found = true;
                    if (aml::bedrock::delete_backup(target, b, &derr))
                        push_notice(st, ui_model::NoticeLevel::Success, "Backup Deleted", "World backup deleted");
                    else
                        push_notice(st, ui_model::NoticeLevel::Error, "Delete Failed", derr);
                    break;
                }
            }
            if (!found)
                push_notice(st, ui_model::NoticeLevel::Error, "Delete Failed", "Selected backup is no longer available");
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ghost_button("Cancel", ImVec2(ui_px(100.0f), ui_px(32.0f))))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    for (auto& backup : filtered) {
        ImGui::PushID(backup.id.c_str());
        card_begin(("##bedrock_backup_" + backup.id).c_str(), ImVec2(-1, ui_px(78.0f)));
        ImVec2 bp = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddCircleFilled(
            ImVec2(bp.x + ui_px(8.0f), bp.y + ui_px(30.0f)), ui_px(5.0f), c32(k.green));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ui_px(20.0f));
        ImGui::PushFont(f_bold);
        ImGui::TextUnformatted(backup.label.empty() ? "Untitled Backup" : backup.label.c_str());
        ImGui::PopFont();
        ImGui::TextColored(k.muted, "World: %s  |  %s  |  %s",
                          backup.world_name.c_str(), backup.timestamp.c_str(),
                          format_bytes(backup.size_bytes).c_str());
        float btn_w = ui_px(80.0f);
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - btn_w * 2 - ui_px(20.0f));
        if (ghost_button("Restore", ImVec2(btn_w, ui_px(28.0f)))) {
            bedrock_ui.pending_restore_id = backup.profile_id;
            bedrock_ui.pending_restore_backup_id = backup.id;
            request_popup("Restore Backup?");
        }
        ImGui::SameLine();
        if (ghost_button("Delete", ImVec2(btn_w, ui_px(28.0f)))) {
            bedrock_ui.selected_backup_profile_id = backup.profile_id;
            bedrock_ui.selected_backup_id = backup.id;
            request_popup("Delete Backup?");
        }
        card_end();
        ImGui::PopID();
    }
}
void draw_bedrock_addons(UiState& st) {
    auto& bedrock_ui = get_bedrock_ui_state();
    page_title("Bedrock Addons", "Manage your Minecraft Bedrock addons");
    if (!aml::bedrock::installed()) {
        empty_state("Bedrock Not Installed",
                    "Minecraft Bedrock Edition is not installed on this system.", "B");
        return;
    }
    const char* addon_tabs[] = {"Installed", "Discover", "Import"};
    const float addon_gap = ui_px(6.0f);
    for (int i = 0; i < 3; ++i) {
        if (i) ImGui::SameLine(0, addon_gap);
        const bool active = bedrock_ui.addon_tab == i;
        const ImVec2 label_size = ImGui::CalcTextSize(addon_tabs[i]);
        const float pad_x = ui_px(14.0f);
        const float tab_h = ui_px(32.0f);
        const ImVec2 tab_size(label_size.x + pad_x * 2.0f, tab_h);
        const ImVec2 tab_min = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(tab_min, tab_min + tab_size,
                          c32(active ? k.surface2 : ImVec4(0, 0, 0, 0)), ui_px(8.0f));
        if (active)
            dl->AddRect(tab_min, tab_min + tab_size, c32(k.brand), ui_px(8.0f),
                        0, ui_px(1.5f));
        ImGui::InvisibleButton((std::string("##bedrock_addon_tab_") + std::to_string(i)).c_str(), tab_size);
        if (ImGui::IsItemHovered() && !active)
            dl->AddRect(tab_min, tab_min + tab_size, c32(k.border), ui_px(8.0f),
                        0, ui_px(1.0f));
        if (ImGui::IsItemClicked()) bedrock_ui.addon_tab = i;
        ImGui::PushFont(active ? f_bold : f_body);
        dl->AddText(tab_min + ImVec2(pad_x, (tab_h - ImGui::GetTextLineHeight()) * 0.5f),
                    c32(active ? k.text : k.muted), addon_tabs[i]);
        ImGui::PopFont();
    }
    ImGui::Spacing();
    switch (bedrock_ui.addon_tab) {
        case 1: draw_bedrock_addons_discover(st); break;
        case 2: draw_bedrock_addons_import(st); break;
        default: draw_bedrock_addons_installed(st); break;
    }
}

void draw_bedrock_addons_installed(UiState& st) {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    auto& bedrock_ui = get_bedrock_ui_state();
    std::vector<aml::bedrock::BedrockPackEntry> all_addons;
    auto profiles = cached_bedrock_profiles();
    for (const auto& profile : profiles) {
        for (const auto& pack : profile.packs) {
            auto pc = pack;
            pc.profile_id = profile.id;
            pc.profile_name = profile.name;
            all_addons.push_back(pc);
        }
    }
    card_begin("##bedrock_addons_installed_header");
    ImGui::TextUnformatted("Installed Addons");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - ui_px(200.0f));
    if (primary_button("+ Import Addon", ImVec2(ui_px(150.0f), ui_px(32.0f))))
        import_bedrock_file(st, false);
    ImGui::Spacing();
    ImGui::SetNextItemWidth(ui_px(240.0f));
    input_text_hint("##bedrock_addon_filter", "Filter addons...", &bedrock_ui.addon_filter);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ui_px(160.0f));
    const char* sort_options[] = {"Name (A-Z)", "Type", "Size"};
    if (ImGui::BeginCombo("##bedrock_addon_sort", sort_options[bedrock_ui.addon_sort])) {
        for (int i = 0; i < 3; ++i)
            if (ImGui::Selectable(sort_options[i], bedrock_ui.addon_sort == i))
                bedrock_ui.addon_sort = i;
        ImGui::EndCombo();
    }
    card_end();
    ImGui::Spacing();
    if (all_addons.empty()) {
        empty_state("No Bedrock addons yet", "Addons will appear here once you import them.", "A");
        return;
    }
    auto filtered = all_addons;
    if (!bedrock_ui.addon_filter.empty()) {
        std::string f = bedrock_ui.addon_filter;
        std::transform(f.begin(), f.end(), f.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        filtered.erase(std::remove_if(filtered.begin(), filtered.end(),
            [&f](const auto& a) {
                std::string n = a.name;
                std::transform(n.begin(), n.end(), n.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                return n.find(f) == std::string::npos;
            }), filtered.end());
    }
    std::sort(filtered.begin(), filtered.end(),
        [&bedrock_ui](const auto& a, const auto& b) {
            switch (bedrock_ui.addon_sort) {
                case 1: return a.type < b.type;
                case 2: return a.size_bytes < b.size_bytes;
                default: return a.name < b.name;
            }
        });
    if (filtered.empty() && !all_addons.empty()) {
        empty_state("No matching addons",
                    "Try adjusting your search or filter criteria.", "A");
        return;
    }
    if (ImGui::BeginPopupModal("Delete Addon?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Are you sure you want to delete this addon?");
        ImGui::TextColored(k.muted, "This will remove all associated files.");
        ImGui::Spacing();
        if (primary_button("Delete", ImVec2(ui_px(100.0f), ui_px(32.0f)))) {
            aml::bedrock::BedrockProfile target;
            for (const auto& profile : profiles) {
                if (profile.id == bedrock_ui.pending_addon_profile_id) {
                    target = profile;
                    break;
                }
            }
            std::string delete_error;
            bool deleted = false;
            std::filesystem::path active_path;
            std::filesystem::path disabled_path;
            if (target.id.empty()) {
                delete_error = "selected Bedrock profile is no longer available";
            } else if (bedrock_addon_paths(target, bedrock_ui.pending_addon_type,
                                           bedrock_ui.pending_addon_filename,
                                           active_path, disabled_path, &delete_error)) {
                if (remove_bedrock_path(active_path, deleted, &delete_error) &&
                    remove_bedrock_path(disabled_path, deleted, &delete_error)) {
                    if (!deleted) delete_error = "installed Bedrock add-on was not found";
                }
            }

            bool metadata_saved = true;
            if (deleted && delete_error.empty() && supabase.is_authenticated()) {
                auto profile_it = std::find_if(profiles.begin(), profiles.end(),
                    [&bedrock_ui](const auto& profile) {
                        return profile.id == bedrock_ui.pending_addon_profile_id;
                    });
                if (profile_it == profiles.end()) {
                    metadata_saved = false;
                } else {
                    const auto pack_it = std::find_if(profile_it->packs.begin(), profile_it->packs.end(),
                        [&bedrock_ui](const auto& pack) {
                            return pack.type == bedrock_ui.pending_addon_type &&
                                   pack.filename == bedrock_ui.pending_addon_filename &&
                                   (bedrock_ui.pending_addon_uuid.empty() ||
                                    pack.uuid == bedrock_ui.pending_addon_uuid);
                        });
                    if (pack_it == profile_it->packs.end()) {
                        metadata_saved = false;
                    } else {
                        profile_it->packs.erase(pack_it);
                        metadata_saved = supabase.update_bedrock_profile(*profile_it);
                    }
                }
                if (!metadata_saved && delete_error.empty())
                    delete_error = "add-on files were removed, but profile metadata could not be saved";
            }

            if (deleted && delete_error.empty() && metadata_saved)
                push_notice(st, ui_model::NoticeLevel::Success, "Addon Deleted", "Addon deleted successfully");
            else
                push_notice(st, ui_model::NoticeLevel::Error, "Delete Failed",
                            delete_error.empty() ? "add-on deletion failed" : delete_error);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ghost_button("Cancel", ImVec2(ui_px(100.0f), ui_px(32.0f))))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    for (auto& addon : filtered) {
        ImGui::PushID(addon.uuid.c_str());
        card_begin(("##bedrock_addon_" + addon.uuid).c_str(), ImVec2(-1, ui_px(72.0f)));
        ImVec2 ap = ImGui::GetCursorScreenPos();
        ImVec4 type_col = addon.type == aml::bedrock::BedrockPackType::Behavior ? k.blue : k.orange;
        ImGui::GetWindowDrawList()->AddRectFilled(ap, ImVec2(ap.x + ui_px(4.0f), ap.y + ui_px(72.0f)),
                                                  c32(type_col), ui_px(2.0f));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ui_px(14.0f));
        ImGui::PushFont(f_bold);
        ImGui::TextUnformatted(addon.name.c_str());
        ImGui::PopFont();
        ImGui::SameLine();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 tbp = ImGui::GetCursorScreenPos();
        draw_badge(dl, tbp,
                   addon.type == aml::bedrock::BedrockPackType::Behavior ? "BP" : "RP",
                   k.text, type_col);
        ImGui::Dummy(ImVec2(ui_px(34.0f), ui_px(20.0f)));
        ImGui::TextColored(k.muted, "v%s  |  %s  |  %s  |  %s",
                          addon.version.c_str(),
                          addon.author.empty() ? "Unknown Author" : addon.author.c_str(),
                          format_bytes(addon.size_bytes).c_str(),
                          addon.profile_name.c_str());
        float toggle_w = ui_px(44.0f);
        float del_w = ui_px(70.0f);
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
            ImGui::GetContentRegionAvail().x - toggle_w - del_w - ui_px(30.0f));
        ImGui::PushID("toggle");
        bool nv = draw_toggle("##toggle", addon.enabled, toggle_w, ui_px(22.0f));
        if (nv != addon.enabled) {
            const bool was_enabled = addon.enabled;
            std::string toggle_error;
            std::filesystem::path active_path;
            std::filesystem::path disabled_path;
            bool moved = false;
            const auto profile_it = std::find_if(profiles.begin(), profiles.end(),
                [&addon](const auto& profile) { return profile.id == addon.profile_id; });
            if (profile_it == profiles.end()) {
                toggle_error = "selected Bedrock profile is no longer available";
            } else if (bedrock_addon_paths(*profile_it, addon.type, addon.filename,
                                            active_path, disabled_path, &toggle_error)) {
                const std::filesystem::path from = was_enabled ? active_path : disabled_path;
                const std::filesystem::path to = was_enabled ? disabled_path : active_path;
                std::error_code ec;
                if (!std::filesystem::exists(from, ec)) {
                    toggle_error = ec ? "cannot inspect Bedrock add-on: " + ec.message()
                                      : "installed Bedrock add-on directory not found: " + from.string();
                } else if (std::filesystem::exists(to, ec)) {
                    toggle_error = ec ? "cannot inspect Bedrock add-on destination: " + ec.message()
                                      : "Bedrock add-on destination already exists: " + to.string();
                } else {
                    std::filesystem::create_directories(to.parent_path(), ec);
                    if (ec) {
                        toggle_error = "cannot create Bedrock add-on directory: " + ec.message();
                    } else {
                        std::filesystem::rename(from, to, ec);
                        if (ec) {
                            toggle_error = "cannot change Bedrock add-on state: " + ec.message();
                        } else {
                            moved = true;
                        }
                    }
                }

                if (moved && supabase.is_authenticated()) {
                    auto metadata_profile_it = std::find_if(profiles.begin(), profiles.end(),
                        [&addon](const auto& profile) { return profile.id == addon.profile_id; });
                    bool metadata_saved = false;
                    if (metadata_profile_it != profiles.end()) {
                        auto pack_it = std::find_if(metadata_profile_it->packs.begin(), metadata_profile_it->packs.end(),
                            [&addon](const auto& pack) {
                                return pack.type == addon.type && pack.filename == addon.filename &&
                                       (addon.uuid.empty() || pack.uuid == addon.uuid);
                            });
                        if (pack_it != metadata_profile_it->packs.end()) {
                            pack_it->enabled = nv;
                            metadata_saved = supabase.update_bedrock_profile(*metadata_profile_it);
                        }
                    }
                    if (!metadata_saved) {
                        std::error_code rollback_ec;
                        std::filesystem::rename(to, from, rollback_ec);
                        toggle_error = "add-on state changed, but profile metadata could not be saved";
                        if (rollback_ec) toggle_error += "; rollback failed: " + rollback_ec.message();
                        moved = false;
                    }
                }
            }
            if (moved) {
                addon.enabled = nv;
                push_notice(st, ui_model::NoticeLevel::Success, "Addon Updated",
                            nv ? "Add-on enabled" : "Add-on disabled");
            } else {
                addon.enabled = was_enabled;
                push_notice(st, ui_model::NoticeLevel::Error, "Addon Update Failed",
                            toggle_error.empty() ? "add-on state was not changed" : toggle_error);
            }
        }
        ImGui::PopID();
        ImGui::SameLine();
        ImGui::PushID("del");
        if (ghost_button("Delete", ImVec2(del_w, ui_px(26.0f)))) {
            bedrock_ui.pending_addon_profile_id = addon.profile_id;
            bedrock_ui.pending_addon_filename = addon.filename;
            bedrock_ui.pending_addon_uuid = addon.uuid;
            bedrock_ui.pending_addon_type = addon.type;
            request_popup("Delete Addon?");
        }
        ImGui::PopID();
        card_end();
        ImGui::PopID();
    }
}
void draw_bedrock_addons_discover(UiState& st) {
    auto& bedrock_ui = get_bedrock_ui_state();
    static std::vector<aml::mods::SearchResult> results;
    static std::string searched_query;
    static std::string search_error;

    card_begin("##bedrock_addons_discover");
    ImGui::TextUnformatted("Discover Addons");
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextWrapped("Browse Bedrock add-ons from CurseForge. Modrinth does not currently publish native .mcpack/.mcaddon projects.");
    ImGui::Spacing();
    ImGui::SetNextItemWidth(ui_px(300.0f));
    input_text_hint("##bedrock_addon_search", "Search addons...", &bedrock_ui.addon_filter);
    ImGui::SameLine();
    if (primary_button("Search", ImVec2(ui_px(100.0f), ui_px(32.0f)))) {
        results.clear();
        search_error.clear();
        searched_query = bedrock_ui.addon_filter;
        if (!st.cfg) {
            search_error = "Launcher configuration is unavailable.";
        } else {
            aml::mods::ApiCfg cfg = aml::provider_config::make(*st.cfg);
            if (!aml::mods::curseforge_available(cfg)) {
                search_error = aml::mods::curseforge_proxy_configured(cfg)
                    ? "Sign in to your Amalgam account to browse CurseForge add-ons."
                    : "Connect Amalgam online services or add a personal CurseForge API key.";
            } else {
            std::vector<aml::mods::SearchResult> fetched;
            if (!aml::mods::search(cfg, bedrock_ui.addon_filter, "", "",
                                   aml::mods::Facet::BedrockAddon, fetched, &search_error)) {
                if (search_error.empty()) search_error = "CurseForge search failed.";
            } else {
                results = std::move(fetched);
            }
            }
        }
    }
    ImGui::Spacing();
    if (!search_error.empty()) {
        ImGui::TextColored(k.red, "%s", search_error.c_str());
    } else if (results.empty()) {
        ImGui::TextColored(k.muted, searched_query.empty()
            ? "Search CurseForge to find Bedrock add-ons."
            : "No Bedrock add-ons found.");
    } else {
        ImGui::TextColored(k.muted, "%d CurseForge result(s)", static_cast<int>(results.size()));
        ImGui::Spacing();
        for (const auto& addon : results) {
            ImGui::PushID(addon.slug.c_str());
            card_begin(("##bedrock_discover_" + addon.slug).c_str(), ImVec2(-1, ui_px(82.0f)));
            ImGui::PushFont(f_bold);
            ImGui::TextUnformatted(addon.title.c_str());
            ImGui::PopFont();
            ImGui::TextColored(k.muted, "%s", addon.description.c_str());
            ImGui::TextColored(k.muted, "%lld downloads", static_cast<long long>(addon.downloads));
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - ui_px(110.0f));
            if (primary_button("Install", ImVec2(ui_px(90.0f), ui_px(28.0f)))) {
                std::vector<aml::bedrock::BedrockProfile> profiles =
                    aml::supabase::SupabaseManager::instance().get_bedrock_profiles();
                const aml::bedrock::BedrockProfile* target = nullptr;
                for (const auto& p : profiles) {
                    if (p.id == bedrock_ui.selected_profile_id) { target = &p; break; }
                }
                if (!target && !profiles.empty()) target = &profiles.front();
                if (!target) {
                    push_notice(st, ui_model::NoticeLevel::Warning, "No Profile", "Create a Bedrock profile first");
                } else if (!st.cfg ||
                           !aml::mods::curseforge_available(aml::provider_config::make(*st.cfg))) {
                    const aml::mods::ApiCfg cfg = st.cfg
                        ? aml::provider_config::make(*st.cfg) : aml::mods::ApiCfg{};
                    push_notice(st, ui_model::NoticeLevel::Error, "CurseForge Unavailable",
                                aml::mods::curseforge_proxy_configured(cfg)
                                    ? "Sign in to your Amalgam account to use CurseForge"
                                    : "Connect Amalgam online services or add a personal API key");
                } else {
                    aml::mods::ApiCfg cfg = aml::provider_config::make(*st.cfg);
                    aml::mods::ModInfo info;
                    std::string err;
                    if (!aml::mods::project_files(cfg, addon.slug, "curseforge", info, &err)) {
                        push_notice(st, ui_model::NoticeLevel::Error, "Addon Lookup Failed", err);
                    } else {
                        const std::string file_id = aml::mods::pick_file(info, "", "");
                        auto file_it = std::find_if(info.files.begin(), info.files.end(),
                            [&file_id](const auto& f) { return f.id == file_id; });
                        if (file_it == info.files.end()) {
                            push_notice(st, ui_model::NoticeLevel::Error, "No Download", "CurseForge has no downloadable add-on file");
                        } else if (!aml::mods::resolve_download_url(cfg, addon.slug, "curseforge", *file_it, &err)) {
                            push_notice(st, ui_model::NoticeLevel::Error, "Download URL Failed", err);
                        } else {
                            std::wstring cache = net::get_local_app_data_path() + L"\\Amalgam\\bedrock-cache";
                            net::mkdirs(cache);
                            std::wstring archive = cache + L"\\" + net::to_wide(file_it->filename);
                            if (!net::download(net::to_wide(file_it->url), archive, {}, &err,
                                               file_it->sha1, file_it->size)) {
                                push_notice(st, ui_model::NoticeLevel::Error, "Download Failed", err);
                            } else {
                                auto imported = aml::bedrock::import_addon(archive, *target, &err);
                                if (imported.success)
                                    push_notice(st, ui_model::NoticeLevel::Success, "Addon Installed", addon.title);
                                else
                                    push_notice(st, ui_model::NoticeLevel::Error, "Import Failed", err.empty() ? imported.error : err);
                                std::error_code ec;
                                std::filesystem::remove(archive, ec);
                            }
                        }
                    }
                }
            }
            card_end();
            ImGui::PopID();
            ImGui::Spacing();
        }
    }
    card_end();
}

void draw_bedrock_addons_import(UiState& st) {
    auto& bedrock_ui = get_bedrock_ui_state();
    card_begin("##bedrock_addons_import");
    ImGui::TextUnformatted("Import Addon");
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextWrapped("Import a Bedrock addon (.mcpack or .mcaddon) to use in your profiles.");
    ImGui::Spacing();
    if (ghost_button("Select Addon File", ImVec2(ui_px(180.0f), ui_px(36.0f))))
        import_bedrock_file(st, false);
    ImGui::Spacing();
    if (!bedrock_ui.addon_error.empty()) {
        ImGui::TextColored(k.red, "%s", bedrock_ui.addon_error.c_str());
        ImGui::Spacing();
    }
    if (!bedrock_ui.addon_success.empty()) {
        ImGui::TextColored(k.green, "%s", bedrock_ui.addon_success.c_str());
        ImGui::Spacing();
    }
    card_end();
}

void draw_bedrock_tab(UiState& st) {
    auto& bedrock_ui = get_bedrock_ui_state();
    if (bedrock_ui.profile_creating) {
        draw_bedrock_create_profile(st);
        return;
    }
    if (bedrock_ui.profile_editing) {
        draw_bedrock_edit_profile(st);
        return;
    }
    const char* bedrock_tabs[] = {"Overview", "Profiles", "Worlds", "Backups", "Add-ons"};
    const float tab_gap = ui_px(6.0f);
    for (int i = 0; i < 5; ++i) {
        if (i) ImGui::SameLine(0, tab_gap);
        const bool active = bedrock_ui.profile_tab == i;
        const ImVec2 label_size = ImGui::CalcTextSize(bedrock_tabs[i]);
        const float pad_x = ui_px(14.0f);
        const float tab_h = ui_px(34.0f);
        const ImVec2 tab_size(label_size.x + pad_x * 2.0f, tab_h);
        const ImVec2 tab_min = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(tab_min, tab_min + tab_size,
                          c32(active ? k.surface2 : ImVec4(0, 0, 0, 0)), ui_px(8.0f));
        if (active)
            dl->AddRect(tab_min, tab_min + tab_size, c32(k.brand), ui_px(8.0f),
                        0, ui_px(1.5f));
        ImGui::InvisibleButton((std::string("##bedrock_tab_") + std::to_string(i)).c_str(), tab_size);
        if (ImGui::IsItemHovered() && !active)
            dl->AddRect(tab_min, tab_min + tab_size, c32(k.border), ui_px(8.0f),
                        0, ui_px(1.0f));
        if (ImGui::IsItemClicked()) bedrock_ui.profile_tab = i;
        ImGui::PushFont(active ? f_bold : f_body);
        dl->AddText(tab_min + ImVec2(pad_x, (tab_h - ImGui::GetTextLineHeight()) * 0.5f),
                    c32(active ? k.text : k.muted), bedrock_tabs[i]);
        ImGui::PopFont();
    }
    ImGui::Spacing();
    switch (bedrock_ui.profile_tab) {
        case 1: draw_bedrock_profiles(st); break;
        case 2: draw_bedrock_worlds(st); break;
        case 3: draw_bedrock_backups(st); break;
        case 4: draw_bedrock_addons(st); break;
        default:
            bedrock_ui.world_tab = 0;
            bedrock_ui.backup_tab = 0;
            bedrock_ui.addon_tab = 0;
            draw_bedrock_overview(st);
            break;
    }
}

BedrockStats get_bedrock_stats(UiState& /*st*/,
    const std::vector<aml::bedrock::BedrockProfile>& profiles) {
    BedrockStats stats;
    for (const auto& profile : profiles) {
        stats.total_profiles++;
        if (profile.favorite) stats.favorite_profiles++;
        stats.total_worlds += static_cast<int>(profile.worlds.size());
        for (const auto& world : profile.worlds) stats.total_world_size += world.size_bytes;
    }
    for (const auto& profile : profiles) {
        std::filesystem::path profile_root;
        if (!bedrock_profile_root(profile, profile_root, nullptr)) continue;
        const std::wstring behavior = (profile_root / L"behavior_packs").wstring();
        const std::wstring resource = (profile_root / L"resource_packs").wstring();
        const std::wstring saves = (profile_root / L"saves").wstring();
        if (aml::net::directory_exists(behavior)) stats.total_world_size += calculate_directory_size(behavior);
        if (aml::net::directory_exists(resource)) stats.total_world_size += calculate_directory_size(resource);
        if (aml::net::directory_exists(saves)) stats.total_world_size += calculate_directory_size(saves);
    }
    for (const auto& profile : profiles) {
        std::string berr;
        auto backups = aml::bedrock::list_backups(profile, &berr);
        stats.total_backups += static_cast<int>(backups.size());
        for (const auto& b : backups) stats.total_backup_size += b.size_bytes;
    }
    return stats;
}

std::string generate_bedrock_id() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 15);
    const char* hex = "0123456789abcdef";
    std::string id;
    for (int i = 0; i < 16; ++i) id += hex[dis(gen)];
    return id;
}

std::string format_current_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&t), "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

}  // namespace aml::ui
