#include "ui.h"
#include "ui_internal.h"
#include "supabase.h"
#include "mods.h"
#include "net.h"
#include "json.h"
#include "config.h"
#include "instances.h"
#include "import_pack.h"
#include "project_publishing.h"
#include "provider_config.h"
#include "version_catalog.h"

#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <map>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <functional>
#include <mutex>
#include <memory>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <wincodec.h>
#include <GL/gl.h>

namespace aml::ui {

static const config::Config& ui_config(const UiState& st) {
    static const config::Config empty{};
    return st.cfg ? *st.cfg : empty;
}

// ---------------------------------------------------------------------------
// Mod Manager UI State
// ---------------------------------------------------------------------------

struct ModMetadata;

struct ModManagerUIState {
    int current_tab = 0; // 0=installed, 1=browse, 2=updates, 3=dependencies, 4=settings
    
    // Installed mods
    std::string mod_filter;
    int mod_sort = 0; // 0=name, 1=version, 2=author, 3=size, 4=date
    bool show_disabled = true;
    bool show_incompatible = true;
    
    // Browse mods
    std::string browse_search;
    std::string browse_category;
    std::string browse_loader;
    std::string browse_game_version;
    int browse_sort = 0; // 0=relevance, 1=popularity, 2=updated, 3=name
    bool browse_search_requested = false;
    std::vector<mods::SearchResult> browse_results;
    bool browse_search_completed = false;
    std::string browse_search_error;

    // Results retained after an explicit update check so the Updates tab can
    // render and act on the real provider response.
    std::vector<mods::UpdateEntry> available_updates;
    bool updates_check_succeeded = false;
    std::wstring updates_checked_directory;
    
    // Mod details
    std::string selected_mod_id;
    std::shared_ptr<ModMetadata> selected_mod;
    bool mod_details_open = false;
    
    // Dependency graph
    bool dependency_graph_open = false;
    std::string dependency_graph_mod_id;
    
    // Settings
    bool auto_update_mods = false;
    bool check_for_updates_on_startup = true;
    bool show_beta_versions = false;
    std::string mod_storage_path;

    // First-party project publishing
    std::string publish_project_id;
    std::string publish_project_name;
    std::string publish_project_slug;
    std::string publish_description;
    std::string publish_type = "modpack";
    std::string publish_version = "1.0.0";
    std::string publish_changelog;
    std::string publish_icon_path;
    std::string publish_banner_path;
    std::string publish_gallery_path;
    std::string publish_video_path;
    std::string publish_status;

    // Background install/update operation state. Network work never runs on
    // the ImGui thread; only these small values are read by the renderer.
    std::mutex operation_mu;
    std::thread operation_thread;
    std::atomic_bool operation_running{false};
    std::atomic_bool operation_finished{false};
    float operation_progress = 0.0f;
    bool operation_success = false;
    std::string operation_label;
    std::string operation_error;
};

static ModManagerUIState& get_mod_manager_ui_state() {
    static ModManagerUIState state{};
    return state;
}

static std::vector<std::string> available_browse_versions(UiState& st,
                                                           const std::string& loader) {
    std::vector<std::string> result = version_catalog::profile_versions(loader);
    std::vector<std::string> live_ids;
    {
        std::lock_guard<std::mutex> lock(st.version_mu);
        live_ids.reserve(st.versions.size());
        for (const auto& entry : st.versions) {
            if (entry.type == "release") live_ids.push_back(entry.id);
        }
    }
    version_catalog::merge_live_releases(result, live_ids, loader);
    return result;
}

void load_mod_settings(const config::Config& cfg) {
    auto& s = get_mod_manager_ui_state();
    s.auto_update_mods = cfg.mod_auto_update;
    s.check_for_updates_on_startup = cfg.mod_check_on_startup;
    s.show_beta_versions = cfg.mod_show_beta;
}

void save_mod_settings(config::Config& cfg) {
    const auto& s = get_mod_manager_ui_state();
    cfg.mod_auto_update = s.auto_update_mods;
    cfg.mod_check_on_startup = s.check_for_updates_on_startup;
    cfg.mod_show_beta = s.show_beta_versions;
}

static void poll_mod_operation(UiState& st, ModManagerUIState& state) {
    if (!state.operation_finished.exchange(false)) return;
    if (state.operation_thread.joinable()) state.operation_thread.join();
    std::string label;
    std::string error;
    bool success = false;
    {
        std::lock_guard<std::mutex> lock(state.operation_mu);
        label = state.operation_label;
        error = state.operation_error;
        success = state.operation_success;
    }
    if (success)
        push_notice(st, ui_model::NoticeLevel::Success, "Operation Complete", label);
    else
        push_notice(st, ui_model::NoticeLevel::Error, "Operation Failed", error.empty() ? label : error);
}

static bool begin_mod_operation(ModManagerUIState& state, const std::string& label,
                                 std::function<bool(std::atomic_bool&, float&, std::string&)> work) {
    if (state.operation_running.load()) return false;
    if (state.operation_thread.joinable()) state.operation_thread.join();
    {
        std::lock_guard<std::mutex> lock(state.operation_mu);
        state.operation_label = label;
        state.operation_error.clear();
        state.operation_success = false;
        state.operation_progress = 0.0f;
    }
    state.operation_running = true;
    state.operation_finished = false;
    state.operation_thread = std::thread([&state, work = std::move(work)]() mutable {
        std::string error;
        float progress = 0.0f;
        const bool ok = work(state.operation_running, progress, error);
        {
            std::lock_guard<std::mutex> lock(state.operation_mu);
            state.operation_progress = ok ? 1.0f : progress;
            state.operation_error = std::move(error);
            state.operation_success = ok;
        }
        state.operation_running = false;
        state.operation_finished = true;
    });
    return true;
}

static void draw_mod_operation_status(ModManagerUIState& state) {
    if (!state.operation_running.load()) return;
    float progress = 0.0f;
    std::string label;
    {
        std::lock_guard<std::mutex> lock(state.operation_mu);
        progress = state.operation_progress;
        label = state.operation_label;
    }
    ImGui::TextColored(k.brand, "%s", label.c_str());
    ImGui::ProgressBar(progress, ImVec2(-1, ui_px(8.0f)));
    ImGui::Spacing();
}

static std::wstring mod_disabled_path(const std::wstring& path, bool disabled) {
    const std::wstring suffix = L".disabled";
    const bool already = path.size() >= suffix.size() &&
        path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0;
    if (disabled && !already) return path + suffix;
    if (!disabled && already) return path.substr(0, path.size() - suffix.size());
    return path;
}

static bool set_all_mods_enabled(const std::wstring& mods_dir, bool enabled, std::string* err) {
    if (mods_dir.empty() || !net::directory_exists(mods_dir)) {
        if (err) *err = "active profile has no mods directory";
        return false;
    }
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(mods_dir, ec)) {
        if (!entry.is_regular_file()) continue;
        const std::wstring path = entry.path().wstring();
        const bool disabled = path.size() >= 9 && path.compare(path.size() - 9, 9, L".disabled") == 0;
        const std::wstring base = disabled ? path.substr(0, path.size() - 9) : path;
        const std::wstring ext = std::filesystem::path(base).extension().wstring();
        if (_wcsicmp(ext.c_str(), L".jar") != 0 && _wcsicmp(ext.c_str(), L".zip") != 0) continue;
        const std::wstring target = mod_disabled_path(path, !enabled);
        if (target != path) {
            std::filesystem::rename(path, target, ec);
            if (ec) {
                if (err) *err = "could not rename " + net::to_utf8(path);
                return false;
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Mod Data Structures
// ---------------------------------------------------------------------------

struct ModVersion {
    std::string id;
    std::string version;
    std::string changelog;
    int64_t release_date = 0;
    std::vector<std::string> dependencies;
    std::vector<std::string> game_versions;
    std::vector<std::string> loaders;
    uint64_t download_count = 0;
    uint64_t file_size = 0;
    std::string file_url;
    bool is_beta = false;
};

struct ModMetadata {
    std::string id;
    std::string name;
    std::string description;
    std::string author;
    std::string icon_url;
    std::vector<ModVersion> versions;
    std::vector<std::string> categories;
    std::vector<std::string> tags;
    std::string source; // "modrinth", "curseforge", "local"
    std::string project_url;
    int64_t created_at = 0;
    int64_t updated_at = 0;
    uint64_t download_count = 0;
    double rating = 0.0;
    int review_count = 0;
};

struct InstalledMod {
    ModMetadata metadata{};
    ModVersion version{};
    bool is_enabled = false;
    bool is_incompatible = false;
    std::string install_path;
    int64_t install_date = 0;
};

bool has_update(const InstalledMod& mod);
ModVersion* get_latest_version(const InstalledMod& mod);
std::string truncate_text(const std::string& text, size_t max_length);
std::string format_downloads(uint64_t downloads);
uint64_t get_mod_storage_usage(const std::wstring& mods_dir);
uint64_t get_mod_storage_total();
ImTextureID load_texture(const char* url);

// ---------------------------------------------------------------------------
// Installed Mods Tab
// ---------------------------------------------------------------------------

void draw_mod_manager_installed(UiState& st) {
    auto& mod_ui = get_mod_manager_ui_state();
    poll_mod_operation(st, mod_ui);
    
    page_title("Installed Mods", "Manage your installed mods and configurations");
    
    card_begin("##mod_manager_installed_header");

    // Right-align the complete action group inside the card. SameLine(x)
    // treats x as a window-relative coordinate and clipped the final button
    // once padding/DPI scaling were applied.
    const float action_gap = ImGui::GetStyle().ItemSpacing.x;
    const float action_width = ui_px(100.0f) + ui_px(100.0f) + ui_px(120.0f) +
                               action_gap * 2.0f;
    const float action_start = ImGui::GetCursorPosX() +
        std::max(0.0f, ImGui::GetContentRegionAvail().x - action_width);
    ImGui::SetCursorPosX(action_start);
    
    // Bulk actions
    if (ghost_button("Enable All", ImVec2(ui_px(100.0f), ui_px(32.0f)))) {
        std::string err;
        if (set_all_mods_enabled(st.active_instance_dir + L"\\mods", true, &err))
            push_notice(st, ui_model::NoticeLevel::Success, "Mods Enabled", "All installed mods are enabled");
        else
            push_notice(st, ui_model::NoticeLevel::Error, "Enable Failed", err);
    }
    
    ImGui::SameLine();
    
    if (ghost_button("Disable All", ImVec2(ui_px(100.0f), ui_px(32.0f)))) {
        std::string err;
        if (set_all_mods_enabled(st.active_instance_dir + L"\\mods", false, &err))
            push_notice(st, ui_model::NoticeLevel::Success, "Mods Disabled", "All installed mods are disabled");
        else
            push_notice(st, ui_model::NoticeLevel::Error, "Disable Failed", err);
    }
    
    ImGui::SameLine();
    
    if (primary_button("+ Install Mod", ImVec2(ui_px(120.0f), ui_px(32.0f)))) {
        mod_ui.current_tab = 1; // Switch to browse tab
    }
    
    ImGui::Spacing();
    
    // Filter and sort
    ImGui::SetNextItemWidth(ui_px(240.0f));
    input_text_hint("##mod_manager_mod_filter", "Filter mods...", &mod_ui.mod_filter);
    ImGui::SameLine();
    
    ImGui::SetNextItemWidth(ui_px(160.0f));
    const char* sort_options[] = {"Name (A-Z)", "Version", "Author", "Size", "Install Date"};
    if (ImGui::BeginCombo("##mod_manager_mod_sort", sort_options[mod_ui.mod_sort])) {
        for (int i = 0; i < 5; ++i) {
            if (ImGui::Selectable(sort_options[i], mod_ui.mod_sort == i)) {
                mod_ui.mod_sort = i;
            }
        }
        ImGui::EndCombo();
    }
    
    ImGui::SameLine();
    
    ImGui::Checkbox("Show Disabled", &mod_ui.show_disabled);
    ImGui::SameLine();
    ImGui::Checkbox("Show Incompatible", &mod_ui.show_incompatible);
    
    card_end();
    
    ImGui::Spacing();
    
    // Get installed mods from the real mods/ directory
    static std::vector<InstalledMod> installed_mods_cache;
    static std::wstring installed_mods_cache_directory;
    static uint64_t installed_mods_cache_at_ms = 0;
    const uint64_t installed_mods_now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    const std::wstring installed_mods_directory = st.active_instance_dir.empty()
        ? L"" : st.active_instance_dir + L"\\mods";
    const bool refresh_installed_mods = installed_mods_cache_at_ms == 0 ||
        installed_mods_cache_directory != installed_mods_directory ||
        installed_mods_now - installed_mods_cache_at_ms > 1200;
    auto& installed_mods = installed_mods_cache;
    if (refresh_installed_mods) {
        installed_mods.clear();
    {
        std::wstring mods_dir = st.active_instance_dir.empty() ? L"" : st.active_instance_dir + L"\\mods";
        if (!mods_dir.empty() && net::directory_exists(mods_dir)) {
            std::error_code ec;
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

                InstalledMod m{};
                m.is_enabled = !disabled;
                m.install_path = net::to_utf8(entry.path().wstring());
                m.install_date = std::chrono::duration_cast<std::chrono::seconds>(
                    entry.last_write_time(ec).time_since_epoch()).count();

                // Parse name and version from filename: Name-1.20.1-1.0.0.jar
                std::string fname = std::filesystem::path(filename).stem().string();
                // Try to extract version (last segment after '-')
                auto last_dash = fname.rfind('-');
                if (last_dash != std::string::npos) {
                    m.version.version = fname.substr(last_dash + 1);
                    m.metadata.name = fname.substr(0, last_dash);
                    // Try to get cleaner name (strip MC version if present)
                    auto prev_dash = m.metadata.name.rfind('-');
                    if (prev_dash != std::string::npos) {
                        std::string maybe_ver = m.metadata.name.substr(prev_dash + 1);
                        if (!maybe_ver.empty() && maybe_ver[0] >= '1' && maybe_ver[0] <= '9')
                            m.metadata.name = m.metadata.name.substr(0, prev_dash);
                    }
                } else {
                    m.metadata.name = fname;
                    m.version.version = "unknown";
                }
                m.metadata.source = "local";
                m.metadata.id = filename;

                uint64_t sz = net::file_size(entry.path().wstring());
                m.version.file_size = sz;

                installed_mods.push_back(std::move(m));
            }
        }
    }
        installed_mods_cache_directory = installed_mods_directory;
        installed_mods_cache_at_ms = installed_mods_now;
    }
    
    if (installed_mods.empty()) {
        empty_state("No Mods Installed", "Install mods to enhance your Minecraft experience.", "M");
        return;
    }
    
    // Filter mods
    auto filtered_mods = installed_mods;
    if (!mod_ui.mod_filter.empty()) {
        std::string filter = mod_ui.mod_filter;
        std::transform(filter.begin(), filter.end(), filter.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        
        filtered_mods.erase(std::remove_if(filtered_mods.begin(), filtered_mods.end(),
            [&filter](const auto& mod) {
                std::string name = mod.metadata.name;
                std::string author = mod.metadata.author;
                std::transform(name.begin(), name.end(), name.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                std::transform(author.begin(), author.end(), author.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                return name.find(filter) == std::string::npos && 
                       author.find(filter) == std::string::npos;
            }), filtered_mods.end());
    }
    
    // Filter by enabled/disabled
    if (!mod_ui.show_disabled) {
        filtered_mods.erase(std::remove_if(filtered_mods.begin(), filtered_mods.end(),
            [](const auto& mod) { return !mod.is_enabled; }), filtered_mods.end());
    }
    
    // Filter by compatibility
    if (!mod_ui.show_incompatible) {
        filtered_mods.erase(std::remove_if(filtered_mods.begin(), filtered_mods.end(),
            [](const auto& mod) { return mod.is_incompatible; }), filtered_mods.end());
    }
    
    // Sort mods
    std::sort(filtered_mods.begin(), filtered_mods.end(),
        [&mod_ui](const auto& a, const auto& b) {
            switch (mod_ui.mod_sort) {
                case 1: return a.version.version > b.version.version; // Version (newest first)
                case 2: return a.metadata.author < b.metadata.author; // Author (A-Z)
                case 3: return a.version.file_size > b.version.file_size; // Size (largest first)
                case 4: return a.install_date > b.install_date; // Install Date (newest first)
                default: return a.metadata.name < b.metadata.name; // Name (A-Z)
            }
        });
    
    // Display mods
    for (auto& mod : filtered_mods) {
        ImGui::PushID(mod.metadata.id.c_str());
        
        card_begin(("##mod_manager_mod_" + mod.metadata.id).c_str(), ImVec2(-1, ui_px(80.0f)));
        
        ImGui::BeginGroup();
        
        // Mod icon and info
        ImGui::Image(load_texture(mod.metadata.icon_url.c_str()), ImVec2(ui_px(40.0f), ui_px(40.0f)));
        
        ImGui::SameLine();
        
        ImGui::BeginGroup();
        ImGui::Text("%s", mod.metadata.name.c_str());
        ImGui::TextColored(k.muted, "%s", mod.version.version.c_str());
        ImGui::TextColored(k.muted, "%s", mod.metadata.author.c_str());
        ImGui::EndGroup();
        
        ImGui::EndGroup();
        
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - ui_px(200.0f));
        
        ImGui::BeginGroup();
        
        // Status
        if (mod.is_incompatible) {
            ImGui::TextColored(k.red, "Incompatible");
        } else if (mod.is_enabled) {
            ImGui::TextColored(k.green, "Enabled");
        } else {
            ImGui::TextColored(k.muted, "Disabled");
        }
        
        // Toggle
        if (ImGui::Checkbox(("##mod_manager_toggle_" + mod.metadata.id).c_str(), &mod.is_enabled)) {
            const std::wstring old_path = net::to_wide(mod.install_path);
            const std::wstring new_path = mod_disabled_path(old_path, !mod.is_enabled);
            std::error_code ec;
            if (old_path != new_path) std::filesystem::rename(old_path, new_path, ec);
            if (ec) {
                mod.is_enabled = !mod.is_enabled;
                push_notice(st, ui_model::NoticeLevel::Error, "Toggle Failed", net::to_utf8(new_path));
            } else {
                mod.install_path = net::to_utf8(new_path);
            }
        }
        
        ImGui::SameLine();
        
        // More button
        if (icon_button(IconId::More, ImVec2(ui_px(30.0f), ui_px(28.0f)),
                        "More installed-mod actions")) {
            ImGui::OpenPopup(("##mod_manager_mod_menu_" + mod.metadata.id).c_str());
        }
        
        // Mod menu
        if (ImGui::BeginPopup(("##mod_manager_mod_menu_" + mod.metadata.id).c_str())) {
            if (ImGui::MenuItem("View Details")) {
                mod_ui.selected_mod_id = mod.metadata.id;
                mod_ui.selected_mod = std::make_shared<ModMetadata>(mod.metadata);
                mod_ui.mod_details_open = true;
            }
            
            ImGui::MenuItem("Update (use Updates tab)", nullptr, false, false);
            
            if (ImGui::MenuItem("View Dependencies")) {
                mod_ui.dependency_graph_mod_id = mod.metadata.id;
                mod_ui.dependency_graph_open = true;
            }
            
            if (ImGui::MenuItem("Uninstall", nullptr, false, true)) {
                std::error_code ec;
                if (std::filesystem::remove(net::to_wide(mod.install_path), ec))
                    push_notice(st, ui_model::NoticeLevel::Success, "Mod Uninstalled", mod.metadata.name);
                else
                    push_notice(st, ui_model::NoticeLevel::Error, "Uninstall Failed", ec.message());
            }
            
            ImGui::EndPopup();
        }
        
        ImGui::EndGroup();
        
        card_end();
        ImGui::PopID();
    }
}

// ---------------------------------------------------------------------------
// Browse Mods Tab
// ---------------------------------------------------------------------------

void draw_mod_manager_browse(UiState& st) {
    auto& mod_ui = get_mod_manager_ui_state();
    poll_mod_operation(st, mod_ui);
    
    page_title("Browse Mods", "Discover and install new mods");
    
    card_begin("##mod_manager_browse_header");
    
    ImGui::Spacing();
    
    // Search
    ImGui::SetNextItemWidth(ui_px(300.0f));
    input_text_hint("##mod_manager_browse_search", "Search mods...", &mod_ui.browse_search);
    ImGui::SameLine();
    
    if (primary_button("Search", ImVec2(ui_px(100.0f), ui_px(32.0f)))) {
        mod_ui.browse_search_requested = true;
    }
    
    ImGui::Spacing();
    
    // Filters
    ImGui::TextUnformatted("Filters");
    ImGui::Spacing();
    
    ImGui::TextColored(k.muted, "Category");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ui_px(160.0f));
    if (ImGui::BeginCombo("##mod_manager_browse_category", mod_ui.browse_category.empty() ? "All" : mod_ui.browse_category.c_str())) {
        // In a real implementation, this would list available categories
        if (ImGui::Selectable("All", mod_ui.browse_category.empty())) {
            mod_ui.browse_category.clear();
        }
        if (ImGui::Selectable("Adventure", mod_ui.browse_category == "Adventure")) {
            mod_ui.browse_category = "Adventure";
        }
        if (ImGui::Selectable("Technology", mod_ui.browse_category == "Technology")) {
            mod_ui.browse_category = "Technology";
        }
        if (ImGui::Selectable("Decoration", mod_ui.browse_category == "Decoration")) {
            mod_ui.browse_category = "Decoration";
        }
        ImGui::EndCombo();
    }
    
    ImGui::SameLine();
    
    ImGui::TextColored(k.muted, "Loader");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ui_px(120.0f));
    auto set_browse_loader = [&](const char* loader) {
        mod_ui.browse_loader = loader;
        const std::string filter_loader = mod_ui.browse_loader.empty() ? "auto" : mod_ui.browse_loader;
        if (!mod_ui.browse_game_version.empty() &&
            !version_catalog::supports(filter_loader, mod_ui.browse_game_version))
            mod_ui.browse_game_version.clear();
    };
    if (ImGui::BeginCombo("##mod_manager_browse_loader", mod_ui.browse_loader.empty() ? "All" : mod_ui.browse_loader.c_str())) {
        if (ImGui::Selectable("All", mod_ui.browse_loader.empty())) {
            set_browse_loader("");
        }
        if (ImGui::Selectable("Fabric", mod_ui.browse_loader == "Fabric")) {
            set_browse_loader("Fabric");
        }
        if (ImGui::Selectable("Forge", mod_ui.browse_loader == "Forge")) {
            set_browse_loader("Forge");
        }
        if (ImGui::Selectable("NeoForge", mod_ui.browse_loader == "NeoForge")) {
            set_browse_loader("NeoForge");
        }
        if (ImGui::Selectable("Quilt", mod_ui.browse_loader == "Quilt")) {
            set_browse_loader("Quilt");
        }
        ImGui::EndCombo();
    }
    
    ImGui::SameLine();
    
    ImGui::TextColored(k.muted, "Version");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ui_px(120.0f));
    const std::string browse_loader = mod_ui.browse_loader.empty() ? "auto" : mod_ui.browse_loader;
    const std::vector<std::string> browse_versions = available_browse_versions(st, browse_loader);
    if (ImGui::BeginCombo("##mod_manager_browse_version",
                          mod_ui.browse_game_version.empty() ? "All supported" : mod_ui.browse_game_version.c_str())) {
        if (ImGui::Selectable("All supported", mod_ui.browse_game_version.empty())) {
            mod_ui.browse_game_version.clear();
        }
        for (const std::string& version : browse_versions) {
            const bool selected = mod_ui.browse_game_version == version;
            if (ImGui::Selectable(version.c_str(), selected))
                mod_ui.browse_game_version = version;
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    
    ImGui::Spacing();
    
    // Sort
    ImGui::TextColored(k.muted, "Sort by");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ui_px(160.0f));
    const char* browse_sort_options[] = {"Relevance", "Popularity", "Recently Updated", "Name (A-Z)"};
    if (ImGui::BeginCombo("##mod_manager_browse_sort", browse_sort_options[mod_ui.browse_sort])) {
        for (int i = 0; i < 4; ++i) {
            if (ImGui::Selectable(browse_sort_options[i], mod_ui.browse_sort == i)) {
                mod_ui.browse_sort = i;
            }
        }
        ImGui::EndCombo();
    }
    
    card_end();
    draw_mod_operation_status(mod_ui);
    
    ImGui::Spacing();
    
    // Consume the explicit request before doing any provider work. The cached
    // provider response remains visible on subsequent frames.
    const bool search_requested = mod_ui.browse_search_requested;
    mod_ui.browse_search_requested = false;
    if (search_requested) {
        mod_ui.browse_search_completed = true;
        mod_ui.browse_search_error.clear();
        mod_ui.browse_results.clear();

        if (mod_ui.browse_search.empty()) {
            mod_ui.browse_search_error = "Enter a search term before searching.";
        } else {
            const auto& cfg = ui_config(st);
            mods::ApiCfg api_cfg = provider_config::make(cfg);

            std::string loader = mod_ui.browse_loader;
            std::transform(loader.begin(), loader.end(), loader.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            const std::string game_version = mod_ui.browse_game_version;
            std::string category = mod_ui.browse_category;
            std::transform(category.begin(), category.end(), category.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

            // The provider API accepts a Facet for the categories it exposes.
            // Decoration is not represented by the existing enum and is
            // filtered from the real result categories below instead.
            mods::Facet facet = mods::Facet::Fabric;
            if (category == "adventure") facet = mods::Facet::Adventure;
            else if (category == "technology") facet = mods::Facet::Technology;

            std::vector<mods::SearchResult> results;
            std::string err;
            if (mods::search(api_cfg, mod_ui.browse_search, loader, game_version,
                             facet, results, &err)) {
                if (category == "decoration") {
                    results.erase(std::remove_if(results.begin(), results.end(),
                        [&category](const mods::SearchResult& result) {
                            return std::none_of(result.categories.begin(), result.categories.end(),
                                [&category](const std::string& value) {
                                    std::string normalized = value;
                                    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                                        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                                    return normalized == category;
                                });
                        }), results.end());
                }

                if (mod_ui.browse_sort == 1) {
                    std::sort(results.begin(), results.end(),
                        [](const mods::SearchResult& a, const mods::SearchResult& b) {
                            if (a.downloads != b.downloads) return a.downloads > b.downloads;
                            return a.slug < b.slug;
                        });
                } else if (mod_ui.browse_sort == 2) {
                    std::sort(results.begin(), results.end(),
                        [](const mods::SearchResult& a, const mods::SearchResult& b) {
                            if (a.date_modified != b.date_modified)
                                return a.date_modified > b.date_modified;
                            return a.slug < b.slug;
                        });
                } else if (mod_ui.browse_sort == 3) {
                    const auto lowercase = [](std::string value) {
                        std::transform(value.begin(), value.end(), value.begin(),
                            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                        return value;
                    };
                    std::sort(results.begin(), results.end(),
                        [&lowercase](const mods::SearchResult& a, const mods::SearchResult& b) {
                            const std::string a_title = lowercase(a.title);
                            const std::string b_title = lowercase(b.title);
                            if (a_title != b_title) return a_title < b_title;
                            return a.slug < b.slug;
                        });
                }
                mod_ui.browse_results = std::move(results);
            } else if (err != "no results") {
                mod_ui.browse_search_error = err.empty() ? "Provider search unavailable." : err;
            }
        }
    }

    // Convert only the retained, real provider response for rendering.
    std::vector<ModMetadata> mods;
    mods.reserve(mod_ui.browse_results.size());
    for (const auto& r : mod_ui.browse_results) {
        ModMetadata m{};
        m.id = r.slug;
        m.name = r.title;
        m.description = r.description;
        m.icon_url = r.icon_url;
        m.download_count = r.downloads;
        m.source = r.source;
        if (r.source == "curseforge")
            m.project_url = "https://www.curseforge.com/minecraft/mc-mods/" + r.slug;
        else
            m.project_url = "https://modrinth.com/mod/" + r.slug;
        for (const auto& cat : r.categories) m.categories.push_back(cat);
        mods.push_back(std::move(m));
    }

    if (mods.empty()) {
        if (!mod_ui.browse_search_completed) {
            empty_state("Search Mods", "Enter a query and search the live provider catalog.", "S");
        } else if (!mod_ui.browse_search_error.empty()) {
            empty_state("Search Unavailable", mod_ui.browse_search_error.c_str(), "!");
        } else {
            empty_state("No Mods Found", "Try adjusting your search criteria.", "S");
        }
        return;
    }
    
    // Display mods
    for (auto& mod : mods) {
        ImGui::PushID(mod.id.c_str());
        
        card_begin(("##mod_manager_browse_mod_" + mod.id).c_str(), ImVec2(-1, ui_px(100.0f)));
        
        ImGui::BeginGroup();
        
        // Mod icon and info
        ImTextureID tex = load_texture(mod.icon_url.c_str());
        if (tex) {
            ImGui::Image(tex, ImVec2(ui_px(50.0f), ui_px(50.0f)));
        } else {
            ImVec2 ip = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddRectFilled(ip,
                ImVec2(ip.x + ui_px(50.0f), ip.y + ui_px(50.0f)),
                c32(k.surface2), ui_px(6.0f));
            ImVec2 ts = ImGui::CalcTextSize(mod.name.substr(0, 1).c_str());
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(ip.x + (ui_px(50.0f) - ts.x) * 0.5f, ip.y + (ui_px(50.0f) - ts.y) * 0.5f),
                c32(k.muted), mod.name.substr(0, 1).c_str());
            ImGui::Dummy(ImVec2(ui_px(50.0f), ui_px(50.0f)));
        }
        
        ImGui::SameLine();
        
        ImGui::BeginGroup();
        ImGui::Text("%s", mod.name.c_str());
        ImGui::TextColored(k.muted, "%s", mod.author.c_str());
        
        // Rating
        if (mod.rating > 0) {
            ImGui::TextColored(k.yellow, "★ %.1f", mod.rating);
            ImGui::SameLine();
            ImGui::TextColored(k.muted, "(%d)", mod.review_count);
        }
        
        // Description
        ImGui::TextWrapped("%s", truncate_text(mod.description, 100).c_str());
        
        // Tags
        for (auto& tag : mod.tags) {
            if (tag != mod.tags[0]) ImGui::SameLine();
            ImGui::TextColored(k.muted, "#%s", tag.c_str());
        }
        
        ImGui::EndGroup();
        
        ImGui::EndGroup();
        
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - ui_px(150.0f));
        
        ImGui::BeginGroup();
        
        // Download count
        ImGui::TextColored(k.muted, "%s", format_downloads(mod.download_count).c_str());
        
        // Install button
        if (primary_button("Install", ImVec2(ui_px(100.0f), ui_px(32.0f)))) {
            const auto& cfg = ui_config(st);
            mods::ApiCfg api_cfg = provider_config::make(cfg);
            std::wstring mods_dir = st.active_instance_dir.empty() ? L"" : st.active_instance_dir + L"\\mods";
            if (mods_dir.empty()) {
                push_notice(st, ui_model::NoticeLevel::Warning, "No Profile",
                            "Select a profile first to install mods");
            } else if (mod_ui.operation_running.load()) {
                push_notice(st, ui_model::NoticeLevel::Warning, "Busy", "Another mod operation is in progress");
            } else {
                const std::string label = mod.name + " installed successfully";
                const std::string game_version = st.selected_instance.minecraft_version.empty()
                    ? "1.21.1" : st.selected_instance.minecraft_version;
                begin_mod_operation(mod_ui, "Installing " + mod.name,
                    [api_cfg, slug = mod.id, source = mod.source, loader = cfg.loader,
                     mods_dir, label, game_version](std::atomic_bool& running, float& progress, std::string& err) {
                        std::vector<std::string> log;
                        const bool ok = mods::install_mod(api_cfg, slug, source, loader,
                            game_version,
                            mods_dir, log, &err,
                            [&running, &progress](uint64_t done, uint64_t total) {
                                progress = total ? static_cast<float>(done) / static_cast<float>(total) : 0.0f;
                                return running.load();
                            });
                        if (ok) err.clear();
                        return ok;
                    });
            }
        }
        
        ImGui::SameLine();
        
        // More button
        if (icon_button(IconId::More, ImVec2(ui_px(30.0f), ui_px(28.0f)),
                        "More mod actions")) {
            ImGui::OpenPopup(("##mod_manager_browse_mod_menu_" + mod.id).c_str());
        }
        
        // Mod menu
        if (ImGui::BeginPopup(("##mod_manager_browse_mod_menu_" + mod.id).c_str())) {
            if (ImGui::MenuItem("View Details")) {
                mod_ui.selected_mod_id = mod.id;
                mod_ui.selected_mod = std::make_shared<ModMetadata>(mod);
                mod_ui.mod_details_open = true;
            }
            
            if (ImGui::MenuItem("View on Website")) {
                // Open mod page in browser
                ShellExecuteA(nullptr, "open", mod.project_url.c_str(), nullptr, nullptr, SW_SHOW);
            }
            
            ImGui::EndPopup();
        }
        
        ImGui::EndGroup();
        
        card_end();
        ImGui::PopID();
    }
}

// ---------------------------------------------------------------------------
// Updates Tab
// ---------------------------------------------------------------------------

void draw_mod_manager_updates(UiState& st) {
    auto& mod_ui = get_mod_manager_ui_state();
    poll_mod_operation(st, mod_ui);
    
    page_title("Mod Updates", "Check for and install mod updates");
    
    card_begin("##mod_manager_updates_header");
    
    ImGui::TextUnformatted("Updates Available");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - ui_px(200.0f));
    
    if (primary_button("Check for Updates", ImVec2(ui_px(150.0f), ui_px(32.0f)))) {
        const auto& cfg = ui_config(st);
        mods::ApiCfg api_cfg = provider_config::make(cfg);
        std::wstring instance_dir = st.active_instance_dir;
        if (!instance_dir.empty()) {
            std::vector<mods::UpdateEntry> updates;
            std::string err;
            const std::string game_version = st.selected_instance.minecraft_version.empty()
                ? "1.21.1" : st.selected_instance.minecraft_version;
            if (mods::preview_owned(api_cfg, instance_dir, cfg.loader,
                                    game_version, updates, &err)) {
                const size_t update_count = updates.size();
                mod_ui.available_updates = std::move(updates);
                mod_ui.updates_check_succeeded = true;
                mod_ui.updates_checked_directory = instance_dir;
                if (update_count == 0)
                    push_notice(st, ui_model::NoticeLevel::Success, "Up to Date",
                                "All mods are up to date");
                else
                    push_notice(st, ui_model::NoticeLevel::Info, "Updates Found",
                                (std::to_string(update_count) + " mod(s) have updates").c_str());
            } else {
                mod_ui.available_updates.clear();
                mod_ui.updates_check_succeeded = false;
                mod_ui.updates_checked_directory.clear();
                push_notice(st, ui_model::NoticeLevel::Error, "Update Check Failed", err);
            }
        } else {
            mod_ui.available_updates.clear();
            mod_ui.updates_check_succeeded = false;
            mod_ui.updates_checked_directory.clear();
            push_notice(st, ui_model::NoticeLevel::Warning, "No Profile",
                        "Select a profile to check for updates");
        }
    }
    
    ImGui::SameLine();
    
    if (ghost_button("Update All", ImVec2(ui_px(100.0f), ui_px(32.0f)))) {
        const auto& cfg = ui_config(st);
        mods::ApiCfg api_cfg = provider_config::make(cfg);
        std::wstring instance_dir = st.active_instance_dir;
        if (!instance_dir.empty() && !mod_ui.operation_running.load()) {
            begin_mod_operation(mod_ui, "Updating all mods",
                [api_cfg, instance_dir, loader = cfg.loader,
                 game_version = st.selected_instance.minecraft_version.empty()
                    ? "1.21.1" : st.selected_instance.minecraft_version
                ](std::atomic_bool& running,
                                                                float& progress, std::string& err) {
                    std::vector<std::string> log;
                    const bool ok = mods::update_owned(api_cfg, instance_dir, loader, game_version,
                        log, &err, {}, [&running, &progress](uint64_t done, uint64_t total) {
                            progress = total ? static_cast<float>(done) / static_cast<float>(total) : 0.0f;
                            return running.load();
                        });
                    if (ok) err.clear();
                    return ok;
                });
        }
    }
    
    card_end();
    draw_mod_operation_status(mod_ui);
    
    ImGui::Spacing();
    
    const bool updates_checked = mod_ui.updates_check_succeeded &&
        !st.active_instance_dir.empty() &&
        mod_ui.updates_checked_directory == st.active_instance_dir;
    if (!updates_checked) {
        empty_state("Updates Not Checked", "Run a live update check before treating mods as current.", "?");
        return;
    }
    if (mod_ui.available_updates.empty()) {
        empty_state("All Mods Up to Date", "All your mods are up to date!", "✓");
        return;
    }
    
    // Display mods with updates
    for (const auto& update : mod_ui.available_updates) {
        ImGui::PushID(update.file.c_str());
        
        card_begin(("##mod_manager_update_" + update.file).c_str(), ImVec2(-1, ui_px(80.0f)));
        
        ImGui::BeginGroup();
        
        // Mod info
        ImGui::Text("%s", update.project.empty() ? update.file.c_str() : update.project.c_str());
        ImGui::TextColored(k.muted, "Current: %s", update.current_version.c_str());
        ImGui::TextColored(k.green, "Available: %s", update.latest_version.c_str());
        
        ImGui::EndGroup();
        
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - ui_px(200.0f));
        
        ImGui::BeginGroup();
        
        // Update button
        if (primary_button("Update", ImVec2(ui_px(100.0f), ui_px(32.0f)))) {
            const auto& cfg = ui_config(st);
            if (!st.active_instance_dir.empty() && !mod_ui.operation_running.load()) {
                const std::set<std::string> selected_files{update.file};
                begin_mod_operation(mod_ui, "Updating " + update.file,
                    [api_cfg = provider_config::make(cfg),
                     instance_dir = st.active_instance_dir, loader = cfg.loader,
                     game_version = st.selected_instance.minecraft_version.empty()
                         ? "1.21.1" : st.selected_instance.minecraft_version,
                     selected_files](std::atomic_bool& running, float& progress, std::string& err) {
                        std::vector<std::string> log;
                        const bool ok = mods::update_owned(api_cfg, instance_dir, loader,
                            game_version, log, &err, selected_files,
                            [&running, &progress](uint64_t done, uint64_t total) {
                                progress = total ? static_cast<float>(done) / static_cast<float>(total) : 0.0f;
                                return running.load();
                            });
                        if (ok) err.clear();
                        return ok;
                    });
            } else if (st.active_instance_dir.empty()) {
                push_notice(st, ui_model::NoticeLevel::Warning, "No Profile",
                            "Select a profile before updating mods");
            }
        }
        
        ImGui::SameLine();
        
        // Changelog button
        if (ghost_button("Changelog", ImVec2(ui_px(80.0f), ui_px(28.0f)))) {
            push_notice(st, ui_model::NoticeLevel::Info, "Changelog",
                        update.changelog.empty() ? "No changelog was provided by the project." : update.changelog);
        }
        
        ImGui::EndGroup();
        
        card_end();
        ImGui::PopID();
    }
}

// ---------------------------------------------------------------------------
// Dependencies Tab
// ---------------------------------------------------------------------------

void draw_mod_manager_dependencies(UiState& st) {
    auto& mod_ui = get_mod_manager_ui_state();
    
    page_title("Mod Dependencies", "View and manage mod dependencies");
    
    card_begin("##mod_manager_dependencies_header");
    ImGui::TextUnformatted("Dependencies");
    ImGui::TextColored(k.muted, "View the dependency graph for your installed mods");
    card_end();
    
    ImGui::Spacing();
    
    // Read installed mods from the active profile so the dependency view is
    // based on real files rather than a placeholder collection.
    std::vector<InstalledMod> installed_mods;
    if (!st.active_instance_dir.empty()) {
        const std::wstring mods_dir = st.active_instance_dir + L"\\mods";
        std::error_code ec;
        if (net::directory_exists(mods_dir)) {
            for (const auto& entry : std::filesystem::directory_iterator(mods_dir, ec)) {
                if (ec || !entry.is_regular_file()) continue;
                std::string filename = entry.path().filename().string();
                std::string lower = filename;
                std::transform(lower.begin(), lower.end(), lower.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                const bool disabled = lower.size() > 9 &&
                    lower.compare(lower.size() - 9, 9, ".disabled") == 0;
                if (disabled) filename.resize(filename.size() - 9);
                const std::string ext = std::filesystem::path(filename).extension().string();
                if (ext != ".jar" && ext != ".zip") continue;
                InstalledMod mod{};
                mod.metadata.id = filename;
                mod.metadata.name = std::filesystem::path(filename).stem().string();
                mod.metadata.source = "local";
                mod.version.version = "unknown";
                mod.version.file_size = net::file_size(entry.path().wstring());
                mod.install_path = net::to_utf8(entry.path().wstring());
                mod.is_enabled = !disabled;
                installed_mods.push_back(std::move(mod));
            }
        }
    }
    
    if (installed_mods.empty()) {
        empty_state("No Mods Installed", "Install mods to view dependencies.", "M");
        return;
    }
    
    // Display dependency graph
    for (auto& mod : installed_mods) {
        ImGui::PushID(mod.metadata.id.c_str());
        
        card_begin(("##mod_manager_dependency_" + mod.metadata.id).c_str(), ImVec2(-1, ui_px(100.0f)));
        
        ImGui::BeginGroup();
        
        // Mod info
        ImGui::Text("%s", mod.metadata.name.c_str());
        ImGui::TextColored(k.muted, "%s", mod.version.version.c_str());
        
        // Dependencies
        if (mod.version.dependencies.empty()) {
            ImGui::TextColored(k.green, "No dependencies");
        } else {
            ImGui::TextUnformatted("Dependencies:");
            for (auto& dep : mod.version.dependencies) {
                ImGui::TextColored(k.muted, "  - %s", dep.c_str());
            }
        }
        
        ImGui::EndGroup();
        
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - ui_px(100.0f));
        
        ImGui::BeginGroup();
        
        // View graph button
        if (ghost_button("View Graph", ImVec2(ui_px(100.0f), ui_px(28.0f)))) {
            mod_ui.dependency_graph_mod_id = mod.metadata.id;
            mod_ui.dependency_graph_open = true;
        }
        
        ImGui::EndGroup();
        
        card_end();
        ImGui::PopID();
    }
    
    // Dependency graph popup
    if (mod_ui.dependency_graph_open && !mod_ui.dependency_graph_mod_id.empty()) {
        ImGui::OpenPopup("##mod_manager_dependency_graph");
        mod_ui.dependency_graph_open = false;
    }
    
    if (ImGui::BeginPopupModal("##mod_manager_dependency_graph", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Dependency Graph");
        ImGui::Separator();
        ImGui::Spacing();
        
        const InstalledMod* selected = nullptr;
        for (const auto& mod : installed_mods) {
            if (mod.metadata.id == mod_ui.dependency_graph_mod_id) {
                selected = &mod;
                break;
            }
        }
        if (!selected) {
            ImGui::TextColored(k.muted, "The selected mod is no longer installed.");
        } else if (selected->version.dependencies.empty()) {
            ImGui::TextColored(k.green, "%s has no declared dependencies.", selected->metadata.name.c_str());
        } else {
            ImGui::Text("%s", selected->metadata.name.c_str());
            ImGui::Separator();
            for (const auto& dependency : selected->version.dependencies) {
                const bool installed = std::any_of(installed_mods.begin(), installed_mods.end(),
                    [&dependency](const InstalledMod& mod) {
                        return mod.metadata.name == dependency || mod.metadata.id == dependency;
                    });
                ImGui::TextColored(installed ? k.green : k.red, "%s %s",
                                   installed ? "OK" : "MISSING", dependency.c_str());
            }
        }
        
        ImGui::Spacing();
        
        if (primary_button("Close", ImVec2(ui_px(100.0f), ui_px(32.0f)))) {
            ImGui::CloseCurrentPopup();
        }
        
        ImGui::EndPopup();
    }
}

// ---------------------------------------------------------------------------
// Settings Tab
// ---------------------------------------------------------------------------

void draw_mod_manager_settings(UiState& st) {
    auto& mod_ui = get_mod_manager_ui_state();
    
    page_title("Mod Settings", "Configure mod management preferences");
    
    card_begin("##mod_manager_settings_general");
    ImGui::TextUnformatted("General Settings");
    ImGui::Separator();
    ImGui::Spacing();
    
    ImGui::Checkbox("Auto-update Mods", &mod_ui.auto_update_mods);
    ImGui::TextColored(k.muted, "Automatically update mods when new versions are available");
    
    ImGui::Spacing();
    
    ImGui::Checkbox("Check for Updates on Startup", &mod_ui.check_for_updates_on_startup);
    ImGui::TextColored(k.muted, "Check for mod updates when the launcher starts");
    
    ImGui::Spacing();
    
    ImGui::Checkbox("Show Beta Versions", &mod_ui.show_beta_versions);
    ImGui::TextColored(k.muted, "Show beta and alpha versions when browsing mods");
    
    card_end();
    
    ImGui::Spacing();
    
    card_begin("##mod_manager_settings_storage");
    ImGui::TextUnformatted("Storage Settings");
    ImGui::Separator();
    ImGui::Spacing();
    
    ImGui::TextUnformatted("Mod Storage Path");
    const std::string storage_path = st.active_instance_dir.empty()
        ? "Select a profile to see its mods directory"
        : net::to_utf8(st.active_instance_dir + L"\\mods");
    ImGui::TextColored(k.muted, "%s", storage_path.c_str());
    ImGui::TextColored(k.muted, "Mods are stored per profile so loader and version files stay isolated.");
    
    ImGui::Spacing();
    
    // Storage usage
    uint64_t storage_used = get_mod_storage_usage(
        st.active_instance_dir.empty() ? L"" : st.active_instance_dir + L"\\mods");
    uint64_t storage_total = get_mod_storage_total();
    
    ImGui::TextUnformatted("Storage Usage");
    ImGui::TextColored(k.muted, "%s / %s used", 
                     format_bytes(storage_used).c_str(),
                     format_bytes(storage_total).c_str());
    
    // Storage bar
    float storage_percent = storage_total == 0
        ? 0.0f
        : static_cast<float>(storage_used) / static_cast<float>(storage_total);
    ImGui::ProgressBar(storage_percent, ImVec2(-1, ui_px(8.0f)));
    
    card_end();
    
    ImGui::Spacing();
    
    card_begin("##mod_manager_settings_actions");
    ImGui::TextUnformatted("Actions");
    ImGui::Separator();
    ImGui::Spacing();
    
    if (ghost_button("Save Settings", ImVec2(ui_px(150.0f), ui_px(36.0f)))) {
        save_mod_settings(*st.cfg);
        if (config::save(st.exe_dir + L"\\launcher.json", *st.cfg)) {
            push_notice(st, ui_model::NoticeLevel::Success, "Settings Saved",
                        "Mod settings have been saved");
        } else {
            push_notice(st, ui_model::NoticeLevel::Error, "Save Failed",
                        "Could not write launcher.json");
        }
    }
    
    ImGui::SameLine();
    
    if (ghost_button("Reset to Defaults", ImVec2(ui_px(150.0f), ui_px(36.0f)))) {
        // Reset to defaults
        mod_ui.auto_update_mods = false;
        mod_ui.check_for_updates_on_startup = true;
        mod_ui.show_beta_versions = false;
    }
    
    card_end();
}

// ---------------------------------------------------------------------------
// Mod Details Popup
// ---------------------------------------------------------------------------

void draw_mod_details_popup(UiState&) {
    auto& mod_ui = get_mod_manager_ui_state();
    
    if (!mod_ui.mod_details_open || mod_ui.selected_mod_id.empty()) {
        return;
    }
    
    ImGui::OpenPopup("##mod_manager_mod_details");
    mod_ui.mod_details_open = false;
    
    const ModMetadata mod = mod_ui.selected_mod ? *mod_ui.selected_mod : ModMetadata{};
    
    if (ImGui::BeginPopupModal("##mod_manager_mod_details", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("%s", mod.name.c_str());
        ImGui::Separator();
        ImGui::Spacing();
        
        // Mod info
        ImGui::BeginGroup();
        
        ImGui::Image(load_texture(mod.icon_url.c_str()), ImVec2(ui_px(80.0f), ui_px(80.0f)));
        
        ImGui::EndGroup();
        
        ImGui::SameLine();
        ImGui::BeginGroup();
        
        ImGui::Text("Author: %s", mod.author.c_str());
        ImGui::Text("Source: %s", mod.source.c_str());
        
        // Rating
        if (mod.rating > 0) {
            ImGui::Text("Rating: %.1f/5 (%d reviews)", mod.rating, mod.review_count);
        }
        
        ImGui::Text("Downloads: %s", format_downloads(mod.download_count).c_str());
        ImGui::Text("Created: %s", format_date(mod.created_at).c_str());
        ImGui::Text("Updated: %s", format_date(mod.updated_at).c_str());
        
        ImGui::EndGroup();
        
        ImGui::Spacing();
        ImGui::TextUnformatted("Description");
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextWrapped("%s", mod.description.c_str());
        
        ImGui::Spacing();
        ImGui::TextUnformatted("Categories");
        ImGui::Separator();
        ImGui::Spacing();
        
        for (auto& category : mod.categories) {
            if (category != mod.categories[0]) ImGui::SameLine();
            ImGui::TextColored(k.muted, "#%s", category.c_str());
        }
        
        ImGui::Spacing();
        ImGui::TextUnformatted("Tags");
        ImGui::Separator();
        ImGui::Spacing();
        
        for (auto& tag : mod.tags) {
            if (tag != mod.tags[0]) ImGui::SameLine();
            ImGui::TextColored(k.muted, "#%s", tag.c_str());
        }
        
        ImGui::Spacing();
        ImGui::TextUnformatted("Versions");
        ImGui::Separator();
        ImGui::Spacing();
        
        // Version list
        for (auto& version : mod.versions) {
            ImGui::Text("%s", version.version.c_str());
            ImGui::SameLine();
            ImGui::TextColored(k.muted, "(%s)", format_date(version.release_date).c_str());
            ImGui::SameLine();
            ImGui::TextColored(k.muted, "%s", format_bytes(version.file_size).c_str());
        }
        
        ImGui::Spacing();
        
        if (primary_button("Close", ImVec2(ui_px(100.0f), ui_px(32.0f)))) {
            ImGui::CloseCurrentPopup();
        }
        
        ImGui::EndPopup();
    }
}

// ---------------------------------------------------------------------------
// Main Mod Manager Page
// ---------------------------------------------------------------------------

static bool choose_publish_file(HWND owner, const wchar_t* filter, std::string& out) {
    wchar_t path[MAX_PATH] = {};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrFilter = filter;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&dialog)) return false;
    out = net::to_utf8(path);
    return true;
}

static std::string media_mime(const std::wstring& path) {
    const std::wstring ext = std::filesystem::path(path).extension().wstring();
    if (_wcsicmp(ext.c_str(), L".png") == 0) return "image/png";
    if (_wcsicmp(ext.c_str(), L".jpg") == 0 || _wcsicmp(ext.c_str(), L".jpeg") == 0) return "image/jpeg";
    if (_wcsicmp(ext.c_str(), L".webp") == 0) return "image/webp";
    if (_wcsicmp(ext.c_str(), L".mp4") == 0) return "video/mp4";
    if (_wcsicmp(ext.c_str(), L".webm") == 0) return "video/webm";
    return "application/octet-stream";
}

static bool upload_publish_media(aml::supabase::SupabaseClient& client, const std::string& project_id,
                                 const std::string& kind, const std::string& local_path,
                                 std::string* error) {
    if (local_path.empty()) {
        if (error) *error = kind + " media file is not selected";
        return false;
    }
    const std::wstring path = net::to_wide(local_path);
    if (!net::file_exists(path)) {
        if (error) *error = "media file does not exist: " + local_path;
        return false;
    }
    std::ifstream input(std::filesystem::path(path), std::ios::binary);
    input.seekg(0, std::ios::end);
    const std::streamsize size = input.tellg();
    input.seekg(0, std::ios::beg);
    if (size <= 0 || size > 512ll * 1024ll * 1024ll) {
        if (error) *error = "media file is empty or larger than 512 MiB";
        return false;
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), size);
    const std::string filename = std::filesystem::path(path).filename().string();
    const std::string storage_path = "projects/" + project_id + "/media/" + filename;
    aml::supabase::SupabaseClient::StorageUploadOptions upload{};
    upload.bucket = "project-media";
    upload.path = storage_path;
    upload.data = std::move(bytes);
    upload.content_type = media_mime(path);
    upload.upsert = true;
    const auto uploaded = client.upload_file(upload);
    if (!uploaded.success) {
        if (error) *error = uploaded.error;
        return false;
    }
    publishing::Media media{};
    media.project_id = project_id;
    media.kind = kind;
    media.storage_path = storage_path;
    media.mime_type = upload.content_type;
    media.size_bytes = size;
    media.sha256 = net::sha256_file(path);
    media.title = filename;
    return publishing::add_media(client, media, error);
}

void draw_mod_manager_publish(UiState& st) {
    auto& ui = get_mod_manager_ui_state();
    page_title("Publish Project", "Create a project, submit it for one-time staff safety review, and publish future versions yourself.");
    draw_mod_operation_status(ui);

    aml::supabase::SupabaseClient* client = aml::supabase::SupabaseManager::instance().client();
    if (!client || !client->is_authenticated()) {
        empty_state("Sign in required", "Sign in to create and publish Amalgam projects.", "@");
        return;
    }

    card_begin("##publish_project");
    ImGui::TextUnformatted("Project Details");
    ImGui::Separator();
    ImGui::SetNextItemWidth(ui_px(360.0f));
    ImGui::InputText("Name", &ui.publish_project_name);
    ImGui::SetNextItemWidth(ui_px(360.0f));
    ImGui::InputText("Slug", &ui.publish_project_slug);
    ImGui::SetNextItemWidth(ui_px(180.0f));
    const char* types[] = {"modpack", "mod", "resourcepack", "shader", "datapack", "addon"};
    int type_idx = 0;
    for (int i = 0; i < 6; ++i) if (ui.publish_type == types[i]) type_idx = i;
    if (ImGui::Combo("Type", &type_idx, types, 6)) ui.publish_type = types[type_idx];
    ImGui::InputTextMultiline("Description", &ui.publish_description, ImVec2(-1, ui_px(80.0f)));
    ImGui::SetNextItemWidth(ui_px(180.0f));
    ImGui::InputText("Version", &ui.publish_version);
    ImGui::InputTextMultiline("Changelog", &ui.publish_changelog, ImVec2(-1, ui_px(60.0f)));
    if (ui.publish_project_id.empty()) {
        if (primary_button("Create Draft Project", ImVec2(ui_px(180.0f), ui_px(32.0f)))) {
            publishing::ProjectDraft draft{};
            draft.name = ui.publish_project_name;
            draft.slug = ui.publish_project_slug;
            draft.description = ui.publish_description;
            draft.type = ui.publish_type;
            publishing::Project project{};
            std::string error;
            if (draft.name.empty() || draft.slug.empty()) {
                push_notice(st, ui_model::NoticeLevel::Warning, "Missing Details", "Name and slug are required");
            } else if (publishing::create_project(*client, draft, project, &error)) {
                ui.publish_project_id = project.id;
                ui.publish_status = project.status;
                push_notice(st, ui_model::NoticeLevel::Success, "Draft Created", "Your project draft is ready");
            } else {
                push_notice(st, ui_model::NoticeLevel::Error, "Create Failed", error);
            }
        }
    } else {
        ImGui::TextColored(k.green, "Project ID: %s", ui.publish_project_id.c_str());
        ImGui::TextColored(k.muted, "Status: %s", ui.publish_status.empty() ? "draft" : ui.publish_status.c_str());
    }
    card_end();

    if (ui.publish_project_id.empty()) return;
    ImGui::Spacing();

    card_begin("##publish_media");
    ImGui::TextUnformatted("Artwork and Video");
    ImGui::TextColored(k.muted, "Upload an icon, banner, gallery image, or demonstration video for the project.");
    auto media_picker = [&](const char* label, const wchar_t* filter, std::string& path) {
        ImGui::TextUnformatted(label);
        ImGui::SameLine(ui_px(120.0f));
        ImGui::SetNextItemWidth(ui_px(420.0f));
        ImGui::InputText((std::string("##") + label).c_str(), &path);
        ImGui::SameLine();
        if (ghost_button((std::string("Browse##") + label).c_str(), ImVec2(ui_px(80.0f), ui_px(26.0f))))
            choose_publish_file(st.hwnd, filter, path);
    };
    media_picker("Icon", L"Images\0*.png;*.jpg;*.jpeg;*.webp\0\0", ui.publish_icon_path);
    media_picker("Banner", L"Images\0*.png;*.jpg;*.jpeg;*.webp\0\0", ui.publish_banner_path);
    media_picker("Gallery", L"Images\0*.png;*.jpg;*.jpeg;*.webp\0\0", ui.publish_gallery_path);
    media_picker("Video", L"Videos\0*.mp4;*.webm\0\0", ui.publish_video_path);
    if (ghost_button("Upload Media", ImVec2(ui_px(130.0f), ui_px(30.0f)))) {
        const bool has_media = !ui.publish_icon_path.empty() || !ui.publish_banner_path.empty() ||
            !ui.publish_gallery_path.empty() || !ui.publish_video_path.empty();
        if (!has_media) {
            push_notice(st, ui_model::NoticeLevel::Warning, "No Media Selected",
                        "Select at least one artwork or video file before uploading.");
        } else {
            std::string error;
            bool ok = true;
            if (!ui.publish_icon_path.empty())
                ok = upload_publish_media(*client, ui.publish_project_id, "icon",
                                          ui.publish_icon_path, &error);
            if (ok && !ui.publish_banner_path.empty())
                ok = upload_publish_media(*client, ui.publish_project_id, "banner",
                                          ui.publish_banner_path, &error);
            if (ok && !ui.publish_gallery_path.empty())
                ok = upload_publish_media(*client, ui.publish_project_id, "gallery",
                                          ui.publish_gallery_path, &error);
            if (ok && !ui.publish_video_path.empty())
                ok = upload_publish_media(*client, ui.publish_project_id, "video",
                                          ui.publish_video_path, &error);
            if (ok) {
                push_notice(st, ui_model::NoticeLevel::Success, "Media Uploaded",
                            "Project artwork and video uploaded");
            } else {
                push_notice(st, ui_model::NoticeLevel::Error, "Media Upload Failed",
                            error.empty() ? "A selected media file could not be uploaded." : error);
            }
        }
    }
    card_end();
    ImGui::Spacing();

    card_begin("##publish_version");
    ImGui::TextUnformatted("Version Artifact");
    ImGui::TextColored(k.muted, "The active profile is exported as a verified .mrpack artifact.");
    if (primary_button("Create and Publish Version", ImVec2(ui_px(220.0f), ui_px(32.0f)))) {
        if (st.active_instance_dir.empty()) {
            push_notice(st, ui_model::NoticeLevel::Warning, "No Profile", "Select a profile to export");
        } else {
            instances::Instance instance{};
            std::string error;
            if (!instances::load(st.active_instance_dir, instance, &error)) {
                push_notice(st, ui_model::NoticeLevel::Error, "Profile Load Failed", error);
            } else {
                const std::wstring temp = st.active_instance_dir + L"\\.amalgam-publish-" + net::to_wide(ui.publish_version) + L".mrpack";
                if (!import_pack::export_mrpack(instance, temp, &error)) {
                    push_notice(st, ui_model::NoticeLevel::Error, "Export Failed", error);
                } else {
                    const std::string path = "projects/" + ui.publish_project_id + "/versions/" + ui.publish_version + ".mrpack";
                    aml::supabase::SupabaseClient::StorageUploadOptions upload{};
                    upload.bucket = "project-artifacts";
                    upload.path = path;
                    std::ifstream input(std::filesystem::path(temp), std::ios::binary);
                    input.seekg(0, std::ios::end);
                    const auto size = input.tellg();
                    input.seekg(0, std::ios::beg);
                    upload.data.resize(static_cast<size_t>(size));
                    input.read(reinterpret_cast<char*>(upload.data.data()), size);
                    upload.content_type = "application/zip";
                    upload.upsert = false;
                    const auto stored = client->upload_file(upload);
                    if (!stored.success) {
                        push_notice(st, ui_model::NoticeLevel::Error, "Upload Failed", stored.error);
                    } else {
                        publishing::Version draft{};
                        draft.project_id = ui.publish_project_id;
                        draft.version = ui.publish_version;
                        draft.changelog = ui.publish_changelog;
                        draft.artifact_path = path;
                        draft.artifact_size = size;
                        draft.artifact_sha256 = net::sha256_file(temp);
                        publishing::Version created{};
                        if (!publishing::create_version(*client, draft, created, &error)) {
                            push_notice(st, ui_model::NoticeLevel::Error, "Version Failed", error);
                        } else if (!publishing::publish_version(*client, ui.publish_project_id, created.id, &error)) {
                            push_notice(st, ui_model::NoticeLevel::Info, "Version Submitted", "Version created; staff approval is required before first publication");
                        } else {
                            ui.publish_status = "approved";
                            push_notice(st, ui_model::NoticeLevel::Success, "Version Published", "Approved projects can publish future versions without repeat review");
                        }
                    }
                    std::error_code ec;
                    std::filesystem::remove(temp, ec);
                }
            }
        }
    }
    if (primary_button("Submit Project for Staff Review", ImVec2(ui_px(240.0f), ui_px(32.0f)))) {
        std::string error;
        if (publishing::submit_for_review(*client, ui.publish_project_id, &error)) {
            ui.publish_status = "pending_review";
            push_notice(st, ui_model::NoticeLevel::Success, "Submitted", "Staff will review this project for safety");
        } else push_notice(st, ui_model::NoticeLevel::Error, "Submission Failed", error);
    }
    card_end();
}

void draw_mod_manager_page(UiState& st) {
    auto& mod_ui = get_mod_manager_ui_state();
    
    page_title("Mod Manager", "Advanced mod management with versioning and dependency resolution");
    
    // Mod manager tabs
    if (ImGui::BeginTabBar("##mod_manager_tabs")) {
    
    if (ImGui::BeginTabItem("Installed")) {
        mod_ui.current_tab = 0;
        ImGui::EndTabItem();
    }
    
    if (ImGui::BeginTabItem("Browse")) {
        mod_ui.current_tab = 1;
        ImGui::EndTabItem();
    }
    
    if (ImGui::BeginTabItem("Updates")) {
        mod_ui.current_tab = 2;
        ImGui::EndTabItem();
    }
    
    if (ImGui::BeginTabItem("Dependencies")) {
        mod_ui.current_tab = 3;
        ImGui::EndTabItem();
    }
    
    if (ImGui::BeginTabItem("Settings")) {
        mod_ui.current_tab = 4;
        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Publish")) {
        mod_ui.current_tab = 5;
        ImGui::EndTabItem();
    }
    
        ImGui::EndTabBar();
    }
    
    ImGui::Spacing();
    
    // Draw current tab
    switch (mod_ui.current_tab) {
        case 0:
        default:
            draw_mod_manager_installed(st);
            break;
        case 1:
            draw_mod_manager_browse(st);
            break;
        case 2:
            draw_mod_manager_updates(st);
            break;
        case 3:
            draw_mod_manager_dependencies(st);
            break;
        case 4:
            draw_mod_manager_settings(st);
            break;
        case 5:
            draw_mod_manager_publish(st);
            break;
    }
    
    // Draw mod details popup
    draw_mod_details_popup(st);
}

// ---------------------------------------------------------------------------
// Helper Functions
// ---------------------------------------------------------------------------

bool has_update(const InstalledMod& mod) {
    // Basic heuristic: if version contains a beta/alpha/rc tag, might have update
    if (mod.version.version.find("beta") != std::string::npos ||
        mod.version.version.find("alpha") != std::string::npos ||
        mod.version.version.find("rc") != std::string::npos)
        return true;
    return false;
}

ModVersion* get_latest_version(const InstalledMod& mod) {
    if (mod.metadata.versions.empty()) return nullptr;
    return const_cast<ModVersion*>(&mod.metadata.versions.back());
}

std::string truncate_text(const std::string& text, size_t max_length) {
    if (text.length() <= max_length) {
        return text;
    }
    return text.substr(0, max_length - 3) + "...";
}

std::string format_downloads(uint64_t downloads) {
    if (downloads >= 1000000) {
        return std::to_string(downloads / 1000000) + "M downloads";
    } else if (downloads >= 1000) {
        return std::to_string(downloads / 1000) + "K downloads";
    } else {
        return std::to_string(downloads) + " downloads";
    }
}

uint64_t get_mod_storage_usage(const std::wstring& mods_dir) {
    if (mods_dir.empty() || !net::directory_exists(mods_dir)) return 0;
    uint64_t total = 0;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(mods_dir, ec)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == ".jar" || ext == ".zip") total += net::file_size(entry.path().wstring());
    }
    return total;
}

uint64_t get_mod_storage_total() {
    ULARGE_INTEGER available{}, total{}, free_bytes{};
    if (GetDiskFreeSpaceExW(nullptr, &available, &total, &free_bytes))
        return static_cast<uint64_t>(total.QuadPart);
    return 0;
}

static bool decode_icon(const std::vector<uint8_t>& bytes, std::vector<uint8_t>& rgba,
                        int& width, int& height) {
    if (bytes.empty() || bytes.size() > 16u * 1024u * 1024u) return false;
    const HRESULT init_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = SUCCEEDED(init_hr);
    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    bool ok = false;
    do {
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&factory)))) break;
        if (FAILED(factory->CreateStream(&stream))) break;
        if (FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()),
                                                 static_cast<DWORD>(bytes.size())))) break;
        if (FAILED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad,
                                                     &decoder))) break;
        if (FAILED(decoder->GetFrame(0, &frame))) break;
        UINT w = 0, h = 0;
        if (FAILED(frame->GetSize(&w, &h)) || w == 0 || h == 0 ||
            static_cast<uint64_t>(w) * static_cast<uint64_t>(h) > 4ull * 1024 * 1024) break;
        if (FAILED(factory->CreateFormatConverter(&converter))) break;
        if (FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA,
                                         WICBitmapDitherTypeNone, nullptr, 0.0,
                                         WICBitmapPaletteTypeCustom))) break;
        width = static_cast<int>(w);
        height = static_cast<int>(h);
        rgba.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
        ok = SUCCEEDED(converter->CopyPixels(nullptr, static_cast<UINT>(width * 4),
                                             static_cast<UINT>(rgba.size()), rgba.data()));
    } while (false);
    if (converter) converter->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (stream) stream->Release();
    if (factory) factory->Release();
    if (uninitialize) CoUninitialize();
    return ok;
}

namespace {

struct PendingIcon {
    std::vector<uint8_t> rgba;
    int width = 0;
    int height = 0;
};

GLuint upload_icon_texture(const PendingIcon& icon) {
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, icon.width, icon.height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, icon.rgba.data());
    return texture;
}

}  // namespace

// Mod icons are fetched over the network. The synchronous fetch below used to
// block the render loop for one HTTP round trip per uncached icon, freezing
// the Mod Manager page. Fetch and decode on a worker thread, then upload the
// texture to GL on the render thread the next frame the icon is drawn.
ImTextureID load_texture(const char* url) {
    static std::mutex cache_mu;
    static std::unordered_map<std::string, ImTextureID> cache;
    static std::unordered_map<std::string, PendingIcon> pending;
    static std::unordered_set<std::string> attempted;
    // A catalog page can expose dozens of icons at once. Keep image work
    // bounded so scrolling a large result set cannot create an unbounded
    // detached-thread burst or saturate the network connection.
    static std::atomic<int> active_requests{0};
    constexpr int kMaxConcurrentRequests = 4;
    if (!url || !*url) return static_cast<ImTextureID>(0);
    const std::string key(url);
    std::lock_guard<std::mutex> lock(cache_mu);
    auto cached = cache.find(key);
    if (cached != cache.end()) return cached->second;

    // Decoded on a worker, uploaded here (the render thread) when ready.
    auto ready = pending.find(key);
    if (ready != pending.end()) {
        PendingIcon icon = std::move(ready->second);
        pending.erase(ready);
        const ImTextureID id = static_cast<ImTextureID>(upload_icon_texture(icon));
        cache.emplace(key, id);
        return id;
    }

    if (attempted.find(key) != attempted.end()) return static_cast<ImTextureID>(0);
    if (active_requests.load(std::memory_order_relaxed) >= kMaxConcurrentRequests)
        return static_cast<ImTextureID>(0);
    attempted.insert(key);
    active_requests.fetch_add(1, std::memory_order_relaxed);

    std::thread([key]() {
        std::vector<uint8_t> bytes;
        std::vector<uint8_t> rgba;
        int width = 0, height = 0;
        std::string error;
        if (net::get(net::to_wide(key), bytes, &error) &&
            decode_icon(bytes, rgba, width, height)) {
            PendingIcon icon;
            icon.rgba = std::move(rgba);
            icon.width = width;
            icon.height = height;
            std::lock_guard<std::mutex> lock(cache_mu);
            if (cache.find(key) == cache.end() && pending.find(key) == pending.end())
                pending.emplace(key, std::move(icon));
        }
        active_requests.fetch_sub(1, std::memory_order_relaxed);
    }).detach();
    return static_cast<ImTextureID>(0);
}

}  // namespace aml::ui
