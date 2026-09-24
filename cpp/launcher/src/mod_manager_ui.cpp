#include "ui.h"
#include "ui_internal.h"
#include "auth_wizard.h"
#include "publish_async_scope.h"
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
#include <cstdint>
#include <ctime>
#include <map>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <exception>
#include <functional>
#include <mutex>
#include <memory>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <string_view>
#include <utility>
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
struct ModManagerUIState;

using ModOperationCompletion = std::function<void(
    UiState&, ModManagerUIState&, bool success, const std::string& error)>;

struct ModOperationPresentation {
    std::string success_title = "Operation Complete";
    std::string success_detail;
    std::string failure_title = "Operation Failed";
    bool notify_success = true;
    ModOperationCompletion completion;
};

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
    bool browse_search_in_progress = false;
    std::vector<mods::SearchResult> browse_results;
    bool browse_search_completed = false;
    std::string browse_search_error;

    // Results retained after an explicit update check so the Updates tab can
    // render and act on the real provider response.
    std::vector<mods::UpdateEntry> available_updates;
    bool updates_check_succeeded = false;
    bool updates_check_in_progress = false;
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
    // Publish requests use UiState's joined account lane rather than this
    // screen's standalone mod-operation worker. These values are written only
    // by the render thread; workers receive copied inputs and a shared atomic
    // stop signal, never a reference to this form state.
    std::string publish_bound_account_identity;
    uint64_t publish_bound_session_generation = 0;
    PublishAsyncScope publish_request_scope;
    std::string publish_request_action;
    std::string publish_retry_action;
    std::shared_ptr<std::atomic_bool> publish_work_in_progress;
    std::shared_ptr<std::atomic_bool> publish_stop_requested;
    std::string publish_feedback;
    bool publish_feedback_warning = false;
    std::string publish_error;

    // Background install/update operation state. Network work never runs on
    // the ImGui thread; only these small values are read by the renderer.
    std::mutex operation_mu;
    std::thread operation_thread;
    std::atomic_bool operation_running{false};
    std::atomic_bool operation_finished{false};
    std::atomic<float> operation_progress{0.0f};
    bool operation_success = false;
    std::string operation_label;
    std::string operation_error;
    std::string operation_success_title;
    std::string operation_success_detail;
    std::string operation_failure_title;
    bool operation_notify_success = true;
    ModOperationCompletion operation_completion;

    // A mod uninstall is intentionally two-step.  Keep a profile-qualified
    // target rather than a pointer into the installed-mod cache so a refresh
    // cannot make the confirmation act on a different file.
    bool uninstall_confirmation_requested = false;
    std::wstring pending_uninstall_profile_dir;
    std::wstring pending_uninstall_path;
    std::string pending_uninstall_name;
    std::string pending_uninstall_error;
    instances::ContentFileSnapshot pending_uninstall_snapshot;
    bool uninstall_move_in_progress = false;
    bool uninstall_close_requested = false;
    bool installed_mods_refresh_requested = false;

    std::string updates_check_error;

    ~ModManagerUIState() {
        // The state is process-wide, so no UiState outlives it.  Ask a
        // cancellable provider callback to stop and join before the static
        // thread member is destroyed; otherwise a launcher close while a mod
        // request is pending would terminate the process.
        operation_running.store(false);
        if (operation_thread.joinable()) operation_thread.join();
    }
};

static ModManagerUIState& get_mod_manager_ui_state() {
    static ModManagerUIState state{};
    return state;
}

constexpr const char* kPublishCreateDraftAction = "mod-manager-publish-create-draft";
constexpr const char* kPublishUploadMediaAction = "mod-manager-publish-upload-media";
constexpr const char* kPublishCreateVersionAction = "mod-manager-publish-create-version";
constexpr const char* kPublishSubmitReviewAction = "mod-manager-publish-submit-review";

static bool is_mod_publish_action(std::string_view action) {
    return action == kPublishCreateDraftAction ||
           action == kPublishUploadMediaAction ||
           action == kPublishCreateVersionAction ||
           action == kPublishSubmitReviewAction;
}

static const char* publish_action_label(std::string_view action) {
    if (action == kPublishCreateDraftAction) return "Creating draft project";
    if (action == kPublishUploadMediaAction) return "Uploading project media";
    if (action == kPublishCreateVersionAction) return "Exporting and publishing version";
    if (action == kPublishSubmitReviewAction) return "Submitting staff review";
    return "Finishing publish request";
}

static bool publish_background_work_active(const ModManagerUIState& state) {
    return state.publish_work_in_progress && state.publish_work_in_progress->load();
}

// Ordinary mod operations and Publish both touch the selected profile. Keep
// their visible controls and their worker starts behind the same local lock.
static bool mod_manager_operation_busy(const ModManagerUIState& state) {
    return state.operation_running.load() || publish_background_work_active(state);
}

static void clear_publish_form(ModManagerUIState& state) {
    state.publish_project_id.clear();
    state.publish_project_name.clear();
    state.publish_project_slug.clear();
    state.publish_description.clear();
    state.publish_type = "modpack";
    state.publish_version = "1.0.0";
    state.publish_changelog.clear();
    state.publish_icon_path.clear();
    state.publish_banner_path.clear();
    state.publish_gallery_path.clear();
    state.publish_video_path.clear();
    state.publish_status.clear();
    state.publish_request_scope = {};
    state.publish_request_action.clear();
    state.publish_retry_action.clear();
    state.publish_feedback.clear();
    state.publish_feedback_warning = false;
    state.publish_error.clear();
}

// Project identifiers, selected files, and inline feedback are account-scoped
// UI output. A sign-out or account switch must clear them before a late worker
// can show an old account's data in the current form.
static void bind_publish_form_to_account(ModManagerUIState& state,
                                         const std::string& account_identity,
                                         uint64_t session_generation) {
    if (state.publish_bound_account_identity == account_identity &&
        state.publish_bound_session_generation == session_generation) {
        return;
    }
    if (state.publish_stop_requested) state.publish_stop_requested->store(true);
    clear_publish_form(state);
    if (!publish_background_work_active(state)) {
        state.publish_work_in_progress.reset();
        state.publish_stop_requested.reset();
    }
    state.publish_bound_account_identity = account_identity;
    state.publish_bound_session_generation = session_generation;
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
    std::string success_title;
    std::string success_detail;
    std::string failure_title;
    bool success = false;
    bool notify_success = true;
    ModOperationCompletion completion;
    {
        std::lock_guard<std::mutex> lock(state.operation_mu);
        label = state.operation_label;
        error = state.operation_error;
        success = state.operation_success;
        success_title = state.operation_success_title;
        success_detail = state.operation_success_detail;
        failure_title = state.operation_failure_title;
        notify_success = state.operation_notify_success;
        completion = std::move(state.operation_completion);
        state.operation_completion = {};
    }
    if (success) {
        if (notify_success) {
            push_notice(st, ui_model::NoticeLevel::Success,
                        success_title.empty() ? "Operation Complete" : success_title,
                        success_detail.empty() ? label : success_detail);
        }
    } else {
        push_notice(st, ui_model::NoticeLevel::Error,
                    failure_title.empty() ? "Operation Failed" : failure_title,
                    error.empty() ? label : error);
    }
    if (completion) completion(st, state, success, error);
}

static bool begin_mod_operation(ModManagerUIState& state, const std::string& label,
                                 std::function<bool(std::atomic_bool&, std::atomic<float>&,
                                                    std::string&)> work,
                                 ModOperationPresentation presentation = {}) {
    // A publish export reads the selected profile and must not overlap a
    // mod-manager mutation of that profile. Publish work is joined by UiState
    // through the account lane; ordinary mod work stays on its own lane.
    if (mod_manager_operation_busy(state)) return false;
    if (state.operation_thread.joinable()) state.operation_thread.join();
    {
        std::lock_guard<std::mutex> lock(state.operation_mu);
        state.operation_label = label;
        state.operation_error.clear();
        state.operation_success = false;
        state.operation_success_title = std::move(presentation.success_title);
        state.operation_success_detail = std::move(presentation.success_detail);
        state.operation_failure_title = std::move(presentation.failure_title);
        state.operation_notify_success = presentation.notify_success;
        state.operation_completion = std::move(presentation.completion);
    }
    state.operation_progress.store(0.0f);
    state.operation_running = true;
    state.operation_finished = false;
    state.operation_thread = std::thread([&state, work = std::move(work)]() mutable {
        std::string error;
        bool ok = false;
        try {
            ok = work(state.operation_running, state.operation_progress, error);
        } catch (const std::exception&) {
            error = "The mod operation ended unexpectedly. Please retry it.";
        } catch (...) {
            error = "The mod operation ended unexpectedly. Please retry it.";
        }
        {
            std::lock_guard<std::mutex> lock(state.operation_mu);
            if (ok) state.operation_progress.store(1.0f);
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
    const float progress = std::clamp(state.operation_progress.load(), 0.0f, 1.0f);
    std::string label;
    {
        std::lock_guard<std::mutex> lock(state.operation_mu);
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

static bool is_mod_archive_path(const std::wstring& path) {
    constexpr wchar_t kDisabledSuffix[] = L".disabled";
    constexpr size_t suffix_length = 9;
    const bool disabled = path.size() >= suffix_length &&
        path.compare(path.size() - suffix_length, suffix_length, kDisabledSuffix) == 0;
    const std::wstring archive_path = disabled ? path.substr(0, path.size() - suffix_length) : path;
    const std::wstring extension = std::filesystem::path(archive_path).extension().wstring();
    return _wcsicmp(extension.c_str(), L".jar") == 0 || _wcsicmp(extension.c_str(), L".zip") == 0;
}

static bool set_all_mods_enabled(const instances::Instance& profile, bool enabled,
                                 std::atomic_bool& running,
                                 std::atomic<float>& progress, std::string* err) {
    if (profile.directory.empty()) {
        if (err) *err = "select a profile before changing mod states";
        return false;
    }

    std::string list_error;
    const std::vector<instances::ContentEntry> content = instances::list_content(profile, &list_error);
    if (!list_error.empty()) {
        if (err) *err = "could not inspect installed mods: " + list_error;
        return false;
    }

    struct PendingChange {
        instances::ContentEntry entry;
        instances::ContentFileSnapshot snapshot;
    };
    std::vector<PendingChange> changes;
    for (const auto& entry : content) {
        if (entry.type != instances::ContentType::Mod || entry.enabled == enabled ||
            !is_mod_archive_path(entry.path)) continue;
        PendingChange change;
        change.entry = entry;
        std::string snapshot_error;
        if (!instances::capture_content_file_snapshot(profile, change.entry, change.snapshot,
                                                      &snapshot_error)) {
            if (err) *err = "could not prepare a mod state change: " + snapshot_error;
            return false;
        }
        changes.push_back(std::move(change));
    }
    if (changes.empty()) {
        progress.store(1.0f);
        return true;
    }

    std::vector<instances::ContentEntry> changed;
    const auto rollback = [&](const std::string& reason) {
        bool rollback_failed = false;
        for (auto it = changed.rbegin(); it != changed.rend(); ++it) {
            std::string rollback_error;
            if (!instances::set_content_enabled(profile, *it, !enabled, &rollback_error))
                rollback_failed = true;
        }
        if (err) {
            *err = reason;
            *err += rollback_failed
                ? "; Amalgam could not fully restore the earlier mod states. Review the installed list before retrying."
                : "; no mod state changes were kept.";
        }
        return false;
    };

    for (size_t index = 0; index < changes.size(); ++index) {
        if (!running.load()) return rollback("The mod operation was cancelled before it finished");
        const PendingChange& change = changes[index];
        std::string change_error;
        if (!instances::content_file_matches_snapshot(profile, change.entry, change.snapshot,
                                                       &change_error)) {
            return rollback("A mod changed while this action was waiting: " + change_error);
        }
        if (!instances::set_content_enabled(profile, change.entry, enabled, &change_error)) {
            return rollback("Could not change a mod state: " + change_error);
        }
        instances::ContentEntry changed_entry = change.entry;
        changed_entry.path = mod_disabled_path(change.entry.path, !enabled);
        changed_entry.filename = net::to_utf8(
            std::filesystem::path(changed_entry.path).filename().wstring());
        changed_entry.enabled = enabled;
        changed.push_back(std::move(changed_entry));
        progress.store(static_cast<float>(index + 1) / static_cast<float>(changes.size()));
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

static void clear_pending_mod_uninstall(ModManagerUIState& state) {
    state.uninstall_confirmation_requested = false;
    state.pending_uninstall_profile_dir.clear();
    state.pending_uninstall_path.clear();
    state.pending_uninstall_name.clear();
    state.pending_uninstall_error.clear();
    state.pending_uninstall_snapshot = {};
    state.uninstall_move_in_progress = false;
    state.uninstall_close_requested = false;
}

static instances::ContentEntry pending_mod_uninstall_entry(const ModManagerUIState& state) {
    instances::ContentEntry entry;
    entry.path = state.pending_uninstall_path;
    entry.filename = net::to_utf8(
        std::filesystem::path(state.pending_uninstall_path).filename().wstring());
    entry.type = instances::ContentType::Mod;
    return entry;
}

static bool request_mod_uninstall(UiState& st, ModManagerUIState& state,
                                  const InstalledMod& mod) {
    if (st.active_instance_dir.empty() || mod.install_path.empty()) {
        push_notice(st, ui_model::NoticeLevel::Warning, "No Mod Selected",
                    "Refresh the installed list and choose a mod to move to recovery.");
        return false;
    }

    instances::Instance profile;
    profile.directory = st.active_instance_dir;
    instances::ContentEntry entry;
    entry.path = net::to_wide(mod.install_path);
    entry.filename = net::to_utf8(std::filesystem::path(entry.path).filename().wstring());
    entry.type = instances::ContentType::Mod;
    instances::ContentFileSnapshot snapshot;
    std::string error;
    if (!instances::capture_content_file_snapshot(profile, entry, snapshot, &error)) {
        push_notice(st, ui_model::NoticeLevel::Error, "Cannot Move Mod to Recovery",
                    error.empty() ? "The selected mod is no longer available. Refresh and try again." : error);
        return false;
    }

    clear_pending_mod_uninstall(state);
    state.pending_uninstall_profile_dir = profile.directory;
    state.pending_uninstall_path = entry.path;
    state.pending_uninstall_name = mod.metadata.name.empty() ? mod.metadata.id : mod.metadata.name;
    state.pending_uninstall_snapshot = std::move(snapshot);
    state.uninstall_confirmation_requested = true;
    return true;
}

static void draw_mod_uninstall_confirmation(UiState& st, ModManagerUIState& state) {
    constexpr const char* kPopupId = "##mod_manager_uninstall_confirmation";
    if (state.uninstall_confirmation_requested) {
        ImGui::OpenPopup(kPopupId);
        state.uninstall_confirmation_requested = false;
    }

    ImGui::SetNextWindowSizeConstraints(ImVec2(ui_px(360.0f), 0.0f),
                                        ImVec2(ui_px(520.0f), ui_px(1000.0f)));
    if (!ImGui::BeginPopupModal(kPopupId, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

    if (state.uninstall_close_requested) {
        clear_pending_mod_uninstall(state);
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    ImGui::TextUnformatted("Move mod to recovery?");
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextWrapped("Move \"%s\" out of the active profile?", state.pending_uninstall_name.c_str());
    ImGui::PushStyleColor(ImGuiCol_Text, k.muted);
    ImGui::TextWrapped(
        "Nothing is permanently deleted. Amalgam moves the original file into this profile's local recovery folder instead.");
    ImGui::PopStyleColor();
    const bool active_profile_changed = state.pending_uninstall_profile_dir.empty() ||
        state.pending_uninstall_profile_dir != st.active_instance_dir;
    if (active_profile_changed) {
        ImGui::Spacing();
        ImGui::TextColored(k.yellow,
                           "The active profile changed. Cancel, return to the original profile, and choose the mod again.");
    }
    if (!state.pending_uninstall_error.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(k.red, "%s", state.pending_uninstall_error.c_str());
    }
    if (state.uninstall_move_in_progress) {
        ImGui::Spacing();
        ImGui::TextColored(k.brand, "Moving the selected mod to recovery…");
    }
    ImGui::Spacing();

    const bool operation_busy = state.operation_running.load();
    if (ghost_button("Cancel", ImVec2(ui_px(100.0f), ui_px(32.0f)), operation_busy)) {
        clear_pending_mod_uninstall(state);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    const char* move_label = state.uninstall_move_in_progress ? "Moving to Recovery…"
        : state.pending_uninstall_error.empty() ? "Move to Recovery" : "Retry Move to Recovery";
    const bool move_disabled = operation_busy || active_profile_changed;
    if (primary_button(move_label, ImVec2(ui_px(168.0f), ui_px(32.0f)),
                       state.uninstall_move_in_progress, move_disabled)) {
        std::string error;
        if (state.pending_uninstall_profile_dir.empty() || state.pending_uninstall_path.empty() ||
            state.pending_uninstall_snapshot.path.empty()) {
            error = "the selected mod is no longer available";
        } else if (state.pending_uninstall_profile_dir != st.active_instance_dir) {
            error = "the active profile changed; choose the mod again from its original profile";
        } else {
            instances::Instance profile;
            profile.directory = state.pending_uninstall_profile_dir;
            const instances::ContentEntry entry = pending_mod_uninstall_entry(state);
            const instances::ContentFileSnapshot snapshot = state.pending_uninstall_snapshot;
            if (!instances::content_file_matches_snapshot(profile, entry, snapshot, &error)) {
                // The user still has a visible error and an explicit cancel;
                // do not silently apply the confirmed action to a replacement.
            } else {
                ModOperationPresentation presentation;
                presentation.success_title = "Mod Moved to Recovery";
                presentation.success_detail = state.pending_uninstall_name +
                    " is now in this profile's recovery folder.";
                presentation.failure_title = "Move to Recovery Failed";
                presentation.completion = [](UiState&, ModManagerUIState& completed,
                                              bool success, const std::string& operation_error) {
                    completed.uninstall_move_in_progress = false;
                    if (success) {
                        completed.installed_mods_refresh_requested = true;
                        completed.uninstall_close_requested = true;
                    } else {
                        completed.pending_uninstall_error = operation_error.empty()
                            ? "The selected mod could not be moved. Retry or cancel this action."
                            : operation_error;
                    }
                };
                if (begin_mod_operation(
                        state, "Moving " + state.pending_uninstall_name + " to recovery",
                        [profile, entry, snapshot](std::atomic_bool& running,
                                                   std::atomic<float>& progress,
                                                   std::string& operation_error) {
                            if (!running.load()) {
                                operation_error = "The recovery move was cancelled before it started";
                                return false;
                            }
                            if (!instances::content_file_matches_snapshot(profile, entry, snapshot,
                                                                        &operation_error)) {
                                return false;
                            }
                            progress.store(0.2f);
                            if (!running.load()) {
                                operation_error = "The recovery move was cancelled before it started";
                                return false;
                            }
                            const bool moved = instances::move_content_to_trash(
                                profile, entry, nullptr, &operation_error);
                            if (moved) progress.store(1.0f);
                            return moved;
                        }, std::move(presentation))) {
                    state.uninstall_move_in_progress = true;
                    state.pending_uninstall_error.clear();
                } else {
                    error = "another mod operation is already in progress";
                }
            }
        }

        if (!error.empty()) {
            state.pending_uninstall_error = error;
            push_notice(st, ui_model::NoticeLevel::Error, "Cannot Start Recovery Move", error);
        }
    }

    ImGui::EndPopup();
}

// ---------------------------------------------------------------------------
// Installed Mods Tab
// ---------------------------------------------------------------------------

void draw_mod_manager_installed(UiState& st) {
    auto& mod_ui = get_mod_manager_ui_state();
    const bool operation_busy = mod_manager_operation_busy(mod_ui);
    
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
    if (ghost_button("Enable All", ImVec2(ui_px(100.0f), ui_px(32.0f)), operation_busy)) {
        if (st.active_instance_dir.empty()) {
            push_notice(st, ui_model::NoticeLevel::Warning, "No Profile",
                        "Select a profile before enabling mods.");
        } else {
            instances::Instance profile;
            profile.directory = st.active_instance_dir;
            ModOperationPresentation presentation;
            presentation.success_title = "Mods Enabled";
            presentation.success_detail = "All eligible mods in this profile are enabled.";
            presentation.failure_title = "Enable Mods Failed";
            presentation.completion = [](UiState&, ModManagerUIState& completed,
                                          bool success, const std::string&) {
                if (success) completed.installed_mods_refresh_requested = true;
            };
            if (!begin_mod_operation(
                    mod_ui, "Enabling mods", [profile](std::atomic_bool& running,
                                                       std::atomic<float>& progress,
                                                       std::string& error) {
                        return set_all_mods_enabled(profile, true, running, progress, &error);
                    }, std::move(presentation))) {
                push_notice(st, ui_model::NoticeLevel::Warning, "Mod Manager Busy",
                            "Another mod operation is still finishing. Try again in a moment.");
            }
        }
    }
    
    ImGui::SameLine();
    
    if (ghost_button("Disable All", ImVec2(ui_px(100.0f), ui_px(32.0f)), operation_busy)) {
        if (st.active_instance_dir.empty()) {
            push_notice(st, ui_model::NoticeLevel::Warning, "No Profile",
                        "Select a profile before disabling mods.");
        } else {
            instances::Instance profile;
            profile.directory = st.active_instance_dir;
            ModOperationPresentation presentation;
            presentation.success_title = "Mods Disabled";
            presentation.success_detail = "All eligible mods in this profile are disabled.";
            presentation.failure_title = "Disable Mods Failed";
            presentation.completion = [](UiState&, ModManagerUIState& completed,
                                          bool success, const std::string&) {
                if (success) completed.installed_mods_refresh_requested = true;
            };
            if (!begin_mod_operation(
                    mod_ui, "Disabling mods", [profile](std::atomic_bool& running,
                                                        std::atomic<float>& progress,
                                                        std::string& error) {
                        return set_all_mods_enabled(profile, false, running, progress, &error);
                    }, std::move(presentation))) {
                push_notice(st, ui_model::NoticeLevel::Warning, "Mod Manager Busy",
                            "Another mod operation is still finishing. Try again in a moment.");
            }
        }
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
    draw_mod_operation_status(mod_ui);
    
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
    const bool refresh_installed_mods = mod_ui.installed_mods_refresh_requested ||
        installed_mods_cache_at_ms == 0 ||
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
        mod_ui.installed_mods_refresh_requested = false;
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
        
        // Toggle.  The checkbox restores its previous value until the
        // profile-qualified rename has completed on the background lane, so a
        // blocked disk cannot make the rendered state claim success early.
        ImGui::BeginDisabled(operation_busy);
        const bool toggled = ImGui::Checkbox(
            ("##mod_manager_toggle_" + mod.metadata.id).c_str(), &mod.is_enabled);
        ImGui::EndDisabled();
        if (toggled) {
            const bool requested_enabled = mod.is_enabled;
            mod.is_enabled = !requested_enabled;
            instances::Instance profile;
            profile.directory = st.active_instance_dir;
            instances::ContentEntry entry;
            entry.path = net::to_wide(mod.install_path);
            entry.filename = net::to_utf8(std::filesystem::path(entry.path).filename().wstring());
            entry.type = instances::ContentType::Mod;
            instances::ContentFileSnapshot snapshot;
            std::string error;
            if (!instances::capture_content_file_snapshot(profile, entry, snapshot, &error)) {
                push_notice(st, ui_model::NoticeLevel::Error, "Mod State Change Failed",
                            error.empty() ? "Refresh the installed list and retry." : error);
            } else {
                ModOperationPresentation presentation;
                presentation.success_title = requested_enabled ? "Mod Enabled" : "Mod Disabled";
                presentation.success_detail = (mod.metadata.name.empty() ? mod.metadata.id : mod.metadata.name) +
                    (requested_enabled ? " is enabled." : " is disabled.");
                presentation.failure_title = "Mod State Change Failed";
                presentation.completion = [](UiState&, ModManagerUIState& completed,
                                              bool success, const std::string&) {
                    if (success) completed.installed_mods_refresh_requested = true;
                };
                if (!begin_mod_operation(
                        mod_ui, std::string(requested_enabled ? "Enabling " : "Disabling ") +
                            (mod.metadata.name.empty() ? mod.metadata.id : mod.metadata.name),
                        [profile, entry, snapshot, requested_enabled](std::atomic_bool& running,
                                                                       std::atomic<float>& progress,
                                                                       std::string& operation_error) {
                            if (!running.load()) {
                                operation_error = "The mod state change was cancelled before it started";
                                return false;
                            }
                            if (!instances::content_file_matches_snapshot(profile, entry, snapshot,
                                                                        &operation_error)) {
                                return false;
                            }
                            progress.store(0.2f);
                            if (!instances::set_content_enabled(profile, entry, requested_enabled,
                                                               &operation_error)) {
                                return false;
                            }
                            progress.store(1.0f);
                            return true;
                        }, std::move(presentation))) {
                    push_notice(st, ui_model::NoticeLevel::Warning, "Mod Manager Busy",
                                "Another mod operation is still finishing. Try again in a moment.");
                }
            }
        }
        
        ImGui::SameLine();
        
        // More button
        if (icon_button(IconId::More, ImVec2(ui_px(30.0f), ui_px(28.0f)),
                        "More installed-mod actions", ImVec4(-1, -1, -1, -1), operation_busy)) {
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
            
            if (ImGui::MenuItem("Move to Recovery...", nullptr, false, !operation_busy)) {
                request_mod_uninstall(st, mod_ui, mod);
                ImGui::CloseCurrentPopup();
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
    const bool operation_busy = mod_manager_operation_busy(mod_ui);
    
    page_title("Browse Mods", "Discover and install new mods");
    
    card_begin("##mod_manager_browse_header");
    
    ImGui::Spacing();
    
    // Search
    ImGui::SetNextItemWidth(ui_px(300.0f));
    input_text_hint("##mod_manager_browse_search", "Search mods...", &mod_ui.browse_search);
    ImGui::SameLine();
    
    if (primary_button(mod_ui.browse_search_in_progress ? "Searching…" : "Search",
                       ImVec2(ui_px(100.0f), ui_px(32.0f)),
                       mod_ui.browse_search_in_progress, operation_busy)) {
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
    
    // Consume the explicit request on the render thread, then do provider
    // work in the serialized background lane.  The previous real response is
    // retained until a new response succeeds, so a slow catalog never freezes
    // the page or replaces useful results with a blank screen.
    const bool search_requested = mod_ui.browse_search_requested;
    mod_ui.browse_search_requested = false;
    if (search_requested) {
        if (mod_ui.browse_search.empty()) {
            mod_ui.browse_search_completed = true;
            mod_ui.browse_search_error = "Enter a search term before searching.";
        } else if (mod_manager_operation_busy(mod_ui)) {
            push_notice(st, ui_model::NoticeLevel::Warning, "Mod Manager Busy",
                        "Another mod operation is still finishing. Try the search again in a moment.");
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
            const int sort = mod_ui.browse_sort;
            const auto result = std::make_shared<std::vector<mods::SearchResult>>();
            ModOperationPresentation presentation;
            presentation.notify_success = false;
            presentation.failure_title = "Mod Search Failed";
            presentation.completion = [result](UiState&, ModManagerUIState& completed,
                                               bool success, const std::string& operation_error) {
                completed.browse_search_in_progress = false;
                completed.browse_search_completed = true;
                if (success) {
                    completed.browse_results = std::move(*result);
                    completed.browse_search_error.clear();
                } else {
                    completed.browse_search_error = operation_error.empty()
                        ? "The mod catalog is unavailable. Retry the search."
                        : operation_error;
                }
            };
            const std::string query = mod_ui.browse_search;
            if (begin_mod_operation(
                    mod_ui, "Searching the mod catalog",
                    [api_cfg, query, loader, game_version, category, facet, sort,
                     result](std::atomic_bool& running, std::atomic<float>& progress,
                             std::string& error) {
                        if (!running.load()) {
                            error = "The search was cancelled before it started";
                            return false;
                        }
                        std::vector<mods::SearchResult> results;
                        if (!mods::search(api_cfg, query, loader, game_version, facet, results, &error)) {
                            if (error == "no results") {
                                error.clear();
                            } else {
                                return false;
                            }
                        }
                        if (category == "decoration") {
                            results.erase(std::remove_if(results.begin(), results.end(),
                                [&category](const mods::SearchResult& search_result) {
                                    return std::none_of(search_result.categories.begin(),
                                        search_result.categories.end(), [&category](const std::string& value) {
                                            std::string normalized = value;
                                            std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                                                [](unsigned char c) {
                                                    return static_cast<char>(std::tolower(c));
                                                });
                                            return normalized == category;
                                        });
                                }), results.end());
                        }
                        if (sort == 1) {
                            std::sort(results.begin(), results.end(),
                                [](const mods::SearchResult& a, const mods::SearchResult& b) {
                                    if (a.downloads != b.downloads) return a.downloads > b.downloads;
                                    return a.slug < b.slug;
                                });
                        } else if (sort == 2) {
                            std::sort(results.begin(), results.end(),
                                [](const mods::SearchResult& a, const mods::SearchResult& b) {
                                    if (a.date_modified != b.date_modified)
                                        return a.date_modified > b.date_modified;
                                    return a.slug < b.slug;
                                });
                        } else if (sort == 3) {
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
                        *result = std::move(results);
                        progress.store(1.0f);
                        return true;
                    }, std::move(presentation))) {
                mod_ui.browse_search_in_progress = true;
                mod_ui.browse_search_completed = false;
                mod_ui.browse_search_error.clear();
            } else {
                push_notice(st, ui_model::NoticeLevel::Warning, "Mod Manager Busy",
                            "Another mod operation is still finishing. Try the search again in a moment.");
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
        if (mod_ui.browse_search_in_progress) {
            empty_state("Searching Mod Catalog", "Searching the live provider catalog without blocking the launcher.", "…");
        } else if (!mod_ui.browse_search_completed) {
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
        if (primary_button("Install", ImVec2(ui_px(100.0f), ui_px(32.0f)), false, operation_busy)) {
            const auto& cfg = ui_config(st);
            mods::ApiCfg api_cfg = provider_config::make(cfg);
            std::wstring mods_dir = st.active_instance_dir.empty() ? L"" : st.active_instance_dir + L"\\mods";
            if (mods_dir.empty()) {
                push_notice(st, ui_model::NoticeLevel::Warning, "No Profile",
                            "Select a profile first to install mods");
            } else if (mod_manager_operation_busy(mod_ui)) {
                push_notice(st, ui_model::NoticeLevel::Warning, "Busy", "Another mod operation is in progress");
            } else {
                const std::string game_version = st.selected_instance.minecraft_version.empty()
                    ? "1.21.1" : st.selected_instance.minecraft_version;
                ModOperationPresentation presentation;
                presentation.success_title = "Mod Installed";
                presentation.success_detail = mod.name + " was installed for the selected profile.";
                presentation.failure_title = "Mod Install Failed";
                presentation.completion = [](UiState&, ModManagerUIState& completed,
                                              bool success, const std::string&) {
                    if (success) completed.installed_mods_refresh_requested = true;
                };
                begin_mod_operation(mod_ui, "Installing " + mod.name,
                    [api_cfg, slug = mod.id, source = mod.source, loader = cfg.loader,
                     mods_dir, game_version](std::atomic_bool& running,
                                             std::atomic<float>& progress, std::string& err) {
                        std::vector<std::string> log;
                        const bool ok = mods::install_mod(api_cfg, slug, source, loader,
                            game_version,
                            mods_dir, log, &err,
                            [&running, &progress](uint64_t done, uint64_t total) {
                                progress.store(total
                                    ? static_cast<float>(done) / static_cast<float>(total)
                                    : 0.0f);
                                return running.load();
                            });
                        if (ok) err.clear();
                        return ok;
                    }, std::move(presentation));
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
    const bool operation_busy = mod_manager_operation_busy(mod_ui);
    const bool updates_checked = mod_ui.updates_check_succeeded &&
        !st.active_instance_dir.empty() &&
        mod_ui.updates_checked_directory == st.active_instance_dir;
    
    page_title("Mod Updates", "Check for and install mod updates");
    
    card_begin("##mod_manager_updates_header");
    
    ImGui::TextUnformatted("Updates Available");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - ui_px(200.0f));
    
    const char* check_label = mod_ui.updates_check_in_progress ? "Checking…"
        : mod_ui.updates_check_error.empty() ? "Check for Updates" : "Retry Update Check";
    if (primary_button(check_label, ImVec2(ui_px(150.0f), ui_px(32.0f)),
                       mod_ui.updates_check_in_progress, operation_busy)) {
        const auto& cfg = ui_config(st);
        mods::ApiCfg api_cfg = provider_config::make(cfg);
        std::wstring instance_dir = st.active_instance_dir;
        if (!instance_dir.empty()) {
            const std::string game_version = st.selected_instance.minecraft_version.empty()
                ? "1.21.1" : st.selected_instance.minecraft_version;
            const auto updates = std::make_shared<std::vector<mods::UpdateEntry>>();
            ModOperationPresentation presentation;
            presentation.notify_success = false;
            presentation.failure_title = "Update Check Failed";
            presentation.completion = [updates, instance_dir](UiState& current,
                                                               ModManagerUIState& completed,
                                                               bool success,
                                                               const std::string& operation_error) {
                completed.updates_check_in_progress = false;
                if (current.active_instance_dir != instance_dir) {
                    completed.available_updates.clear();
                    completed.updates_check_succeeded = false;
                    completed.updates_checked_directory.clear();
                    completed.updates_check_error =
                        "The active profile changed before the check finished. Select it and retry.";
                    push_notice(current, ui_model::NoticeLevel::Warning, "Update Check Discarded",
                                completed.updates_check_error);
                    return;
                }
                if (!success) {
                    completed.available_updates.clear();
                    completed.updates_check_succeeded = false;
                    completed.updates_checked_directory.clear();
                    completed.updates_check_error = operation_error.empty()
                        ? "The update service is unavailable. Retry the check."
                        : operation_error;
                    return;
                }
                completed.available_updates = std::move(*updates);
                completed.updates_check_succeeded = true;
                completed.updates_checked_directory = instance_dir;
                completed.updates_check_error.clear();
                if (completed.available_updates.empty()) {
                    push_notice(current, ui_model::NoticeLevel::Success, "Up to Date",
                                "All checked mods are up to date.");
                } else {
                    push_notice(current, ui_model::NoticeLevel::Info, "Updates Found",
                                std::to_string(completed.available_updates.size()) +
                                    " compatible mod update(s) are ready to review.");
                }
            };
            if (begin_mod_operation(
                    mod_ui, "Checking for mod updates",
                    [api_cfg, instance_dir, loader = cfg.loader, game_version,
                     updates](std::atomic_bool& running, std::atomic<float>& progress,
                              std::string& error) {
                        if (!running.load()) {
                            error = "The update check was cancelled before it started";
                            return false;
                        }
                        std::vector<mods::UpdateEntry> found;
                        if (!mods::preview_owned(api_cfg, instance_dir, loader, game_version,
                                                 found, &error)) {
                            return false;
                        }
                        *updates = std::move(found);
                        progress.store(1.0f);
                        return true;
                    }, std::move(presentation))) {
                mod_ui.updates_check_in_progress = true;
                mod_ui.updates_check_error.clear();
            } else {
                push_notice(st, ui_model::NoticeLevel::Warning, "Mod Manager Busy",
                            "Another mod operation is still finishing. Try the update check again in a moment.");
            }
        } else {
            mod_ui.available_updates.clear();
            mod_ui.updates_check_succeeded = false;
            mod_ui.updates_checked_directory.clear();
            mod_ui.updates_check_error.clear();
            push_notice(st, ui_model::NoticeLevel::Warning, "No Profile",
                        "Select a profile to check for updates");
        }
    }
    
    ImGui::SameLine();
    
    const bool update_all_disabled = operation_busy || !updates_checked || mod_ui.available_updates.empty();
    if (ghost_button("Update All", ImVec2(ui_px(100.0f), ui_px(32.0f)), update_all_disabled)) {
        const auto& cfg = ui_config(st);
        mods::ApiCfg api_cfg = provider_config::make(cfg);
        std::wstring instance_dir = st.active_instance_dir;
        if (!instance_dir.empty() && updates_checked) {
            std::set<std::string> selected_files;
            for (const auto& update : mod_ui.available_updates) selected_files.insert(update.file);
            ModOperationPresentation presentation;
            presentation.success_title = "Mod Updates Installed";
            presentation.success_detail = "The reviewed compatible updates were installed.";
            presentation.failure_title = "Update Mods Failed";
            presentation.completion = [](UiState&, ModManagerUIState& completed,
                                          bool success, const std::string&) {
                if (success) completed.installed_mods_refresh_requested = true;
            };
            if (!begin_mod_operation(mod_ui, "Updating reviewed mods",
                [api_cfg, instance_dir, loader = cfg.loader,
                 game_version = st.selected_instance.minecraft_version.empty()
                    ? "1.21.1" : st.selected_instance.minecraft_version,
                 selected_files
                ](std::atomic_bool& running,
                  std::atomic<float>& progress, std::string& err) {
                    std::vector<std::string> log;
                    const bool ok = mods::update_owned(api_cfg, instance_dir, loader, game_version,
                        log, &err, selected_files, [&running, &progress](uint64_t done, uint64_t total) {
                            progress.store(total
                                ? static_cast<float>(done) / static_cast<float>(total)
                                : 0.0f);
                            return running.load();
                        });
                    if (ok) err.clear();
                    return ok;
                }, std::move(presentation))) {
                push_notice(st, ui_model::NoticeLevel::Warning, "Mod Manager Busy",
                            "Another mod operation is still finishing. Try again in a moment.");
            }
        }
    }
    
    card_end();
    draw_mod_operation_status(mod_ui);
    
    ImGui::Spacing();
    
    if (!updates_checked) {
        if (mod_ui.updates_check_in_progress) {
            empty_state("Checking for Updates", "Checking compatible updates without blocking the launcher.", "…");
        } else if (!mod_ui.updates_check_error.empty()) {
            empty_state("Update Check Failed", mod_ui.updates_check_error.c_str(), "!");
        } else {
            empty_state("Updates Not Checked", "Run a live update check before treating mods as current.", "?");
        }
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
        if (primary_button("Update", ImVec2(ui_px(100.0f), ui_px(32.0f)), false, operation_busy)) {
            const auto& cfg = ui_config(st);
            if (!st.active_instance_dir.empty() && updates_checked) {
                const std::set<std::string> selected_files{update.file};
                ModOperationPresentation presentation;
                presentation.success_title = "Mod Updated";
                presentation.success_detail = (update.project.empty() ? update.file : update.project) +
                    " was updated.";
                presentation.failure_title = "Mod Update Failed";
                presentation.completion = [](UiState&, ModManagerUIState& completed,
                                              bool success, const std::string&) {
                    if (success) completed.installed_mods_refresh_requested = true;
                };
                if (!begin_mod_operation(mod_ui, "Updating " + update.file,
                    [api_cfg = provider_config::make(cfg),
                     instance_dir = st.active_instance_dir, loader = cfg.loader,
                     game_version = st.selected_instance.minecraft_version.empty()
                         ? "1.21.1" : st.selected_instance.minecraft_version,
                     selected_files](std::atomic_bool& running, std::atomic<float>& progress,
                                     std::string& err) {
                        std::vector<std::string> log;
                        const bool ok = mods::update_owned(api_cfg, instance_dir, loader,
                            game_version, log, &err, selected_files,
                            [&running, &progress](uint64_t done, uint64_t total) {
                                progress.store(total
                                    ? static_cast<float>(done) / static_cast<float>(total)
                                    : 0.0f);
                                return running.load();
                            });
                        if (ok) err.clear();
                        return ok;
                    }, std::move(presentation))) {
                    push_notice(st, ui_model::NoticeLevel::Warning, "Mod Manager Busy",
                                "Another mod operation is still finishing. Try again in a moment.");
                }
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
    // A visual fixture is deliberately detached from the reviewer profile and
    // drive.  Do not calculate storage usage, inspect free disk space, or let
    // its controls write the real launcher configuration.
    if (st.fixture_mode) {
        page_title("Mod Settings", "Representative local mod-management preferences");
        ImGui::TextColored(k.brand_hov,
                           "Visual fixture: local sample state only; no profile files or disk capacity are inspected.");
        ImGui::Spacing();
        card_begin("##fixture_mod_manager_settings_general");
        ImGui::TextUnformatted("General Settings");
        ImGui::Separator();
        ImGui::BeginDisabled();
        bool enabled = true;
        ImGui::Checkbox("Auto-update Mods", &enabled);
        ImGui::Checkbox("Check for Updates on Startup", &enabled);
        bool beta = false;
        ImGui::Checkbox("Show Beta Versions", &beta);
        ImGui::EndDisabled();
        ImGui::TextColored(k.muted, "Controls are intentionally inert during visual review.");
        card_end();
        ImGui::Spacing();
        card_begin("##fixture_mod_manager_settings_storage");
        ImGui::TextUnformatted("Storage Settings");
        ImGui::Separator();
        ImGui::TextColored(k.muted, "Representative profile storage");
        ImGui::TextColored(k.text, "412 MB of 5 GB");
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, k.brand);
        ImGui::ProgressBar(0.08f, ImVec2(-1, ui_px(8.0f)));
        ImGui::PopStyleColor();
        ImGui::TextColored(k.muted, "Installed mods remain isolated per profile.");
        card_end();
        ImGui::Spacing();
        ImGui::BeginDisabled();
        ghost_button("Save Settings", ImVec2(ui_px(150.0f), ui_px(36.0f)));
        ImGui::SameLine();
        ghost_button("Reset to Defaults", ImVec2(ui_px(150.0f), ui_px(36.0f)));
        ImGui::EndDisabled();
        return;
    }
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

// The live Mod Manager retains browse results, image fetch work, publish
// fields, and detail popups in process-wide state. A fixture must not inherit
// that state from a signed-in or actively browsing launcher, so its complete
// presentation lives in this static, network-free façade.
static int fixture_mod_manager_tab(const std::string& fixture_case) {
    if (fixture_case == "mod-manager-browse" ||
        fixture_case == "mod-manager-browse-menu") return 1;
    if (fixture_case == "mod-manager-updates" ||
        fixture_case == "mod-manager-update-check-working" ||
        fixture_case == "mod-manager-update-check-error") return 2;
    if (fixture_case == "mod-manager-dependencies" ||
        fixture_case == "mod-manager-dependency-graph") return 3;
    if (fixture_case == "mod-manager-settings") return 4;
    if (fixture_case == "mod-manager-publish") return 5;
    return 0;
}

// Mirrors the live, retained operation row without constructing a ModManager
// worker or reading a profile.  It deliberately exists only in the static
// fixture façade below, so progress evidence cannot be mistaken for activity.
static void draw_fixture_mod_operation_status(const UiState& st) {
    if (st.fixture_case != "mod-manager-operation-working") return;

    card_begin("##fixture_mod_operation_working", ImVec2(-1, 0));
    ImGui::PushFont(f_bold);
    ImGui::TextColored(k.brand, "Updating reviewed mods");
    ImGui::PopFont();
    ImGui::ProgressBar(0.62f, ImVec2(-1, ui_px(8.0f)), "62%");
    ImGui::TextColored(k.muted,
                       "Preparing a representative compatible update — local visual fixture only.");
    ImGui::TextColored(k.brand_hov,
                       "No profile files, provider request, or installation worker was started.");
    card_end();
    ImGui::Spacing();
}

static void draw_fixture_mod_tab_bar(int current) {
    const char* labels[] = {"Installed", "Browse", "Updates", "Dependencies", "Settings", "Publish"};
    if (ImGui::BeginTabBar("##fixture_mod_manager_tabs")) {
        for (int i = 0; i < 6; ++i) {
            const ImGuiTabItemFlags flags = current == i
                ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            if (ImGui::BeginTabItem(labels[i], nullptr, flags)) ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

static void draw_fixture_mod_row(const char* id, const char* name, const char* detail,
                                 const char* version, const ImVec4& color) {
    card_begin(id, ImVec2(-1, 0));
    draw_icon(IconId::Cube, ImGui::GetCursorScreenPos() + ImVec2(ui_px(13.0f), ui_px(13.0f)),
              ui_px(9.0f), c32(color));
    ImGui::Dummy(ImVec2(ui_px(28.0f), ui_px(28.0f)));
    ImGui::SameLine(0, ui_px(8.0f));
    ImGui::PushFont(f_bold);
    ImGui::TextColored(k.text, "%s", name);
    ImGui::PopFont();
    ImGui::SameLine(0, ui_px(8.0f));
    ImGui::TextColored(k.muted, "%s", version);
    ImGui::TextColored(k.muted, "%s", detail);
    card_end();
    ImGui::Spacing();
}

// These overlays deliberately live inside the fixture façade rather than the
// live ModManagerUIState.  The live flows read installed content, cache remote
// images, revalidate snapshots, and can start a filesystem worker; none of
// those behaviors is appropriate for a visual-review process.
static bool begin_fixture_mod_overlay(UiState& st, const char* popup_id,
                                      std::string& dismissed_fixture) {
    if (!dismissed_fixture.empty() && dismissed_fixture != st.fixture_case)
        dismissed_fixture.clear();
    if (dismissed_fixture == st.fixture_case) return false;
    ImGui::OpenPopup(popup_id);
    return true;
}

static void draw_fixture_mod_recovery_overlay(UiState& st) {
    static std::string dismissed_fixture;
    constexpr const char* kPopupId = "##fixture_mod_manager_recovery";
    if (!begin_fixture_mod_overlay(st, kPopupId, dismissed_fixture)) return;

    const bool working = st.fixture_case == "mod-manager-move-recovery-working";
    const bool error = st.fixture_case == "mod-manager-move-recovery-error";
    ImGui::SetNextWindowSizeConstraints(ImVec2(ui_px(360.0f), 0.0f),
                                        ImVec2(ui_px(520.0f), ui_px(720.0f)));
    bool open = true;
    if (ImGui::BeginPopupModal(kPopupId, &open, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("Move mod to recovery?");
        ImGui::PopFont();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextWrapped("Move \"Sodium\" out of the sample Astral Frontier profile?");
        ImGui::TextColored(k.muted,
                           "Nothing is permanently deleted. The normal action moves the original file into the profile's local recovery folder.");
        ImGui::Spacing();
        ImGui::TextColored(k.brand,
                           "LOCAL VISUAL-QA FIXTURE — no file is inspected, moved, or queued.");
        if (working) {
            ImGui::Spacing();
            ImGui::TextColored(k.orange, "Move to recovery is in progress in this sample presentation.");
            ImGui::TextWrapped("No worker was started for this fixture; the disabled control proves the busy composition only.");
        } else if (error) {
            ImGui::Spacing();
            ImGui::TextColored(k.red, "The selected sample content changed before the move could begin.");
            ImGui::TextWrapped("Refresh the real profile and choose the mod again. No file was changed for this fixture.");
        }
        ImGui::Spacing();
        const char* action_label = working ? "Moving to Recovery…" :
            (error ? "Retry Move to Recovery" : "Move to Recovery");
        primary_button(action_label, ImVec2(ui_px(184.0f), ui_px(32.0f)), working, true);
        ImGui::SameLine();
        if (ghost_button("Close preview", ImVec2(ui_px(118.0f), ui_px(32.0f)))) {
            dismissed_fixture = st.fixture_case;
            open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (!open) dismissed_fixture = st.fixture_case;
}

static void draw_fixture_mod_details_overlay(UiState& st) {
    static std::string dismissed_fixture;
    constexpr const char* kPopupId = "##fixture_mod_manager_details";
    if (!begin_fixture_mod_overlay(st, kPopupId, dismissed_fixture)) return;

    ImGui::SetNextWindowSizeConstraints(ImVec2(ui_px(380.0f), 0.0f),
                                        ImVec2(ui_px(560.0f), ui_px(720.0f)));
    bool open = true;
    if (ImGui::BeginPopupModal(kPopupId, &open, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("Sodium");
        ImGui::PopFont();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextColored(k.brand, "LOCAL VISUAL-QA FIXTURE — static representative metadata only.");
        ImGui::TextColored(k.muted, "Author: CaffeineMC  •  Source: Modrinth");
        ImGui::TextColored(k.muted, "Fabric 1.20.1  •  Version 0.5.11");
        ImGui::Spacing();
        ImGui::PushFont(f_bold);
        ImGui::TextUnformatted("Description");
        ImGui::PopFont();
        ImGui::TextWrapped("A modern renderer focused on smooth performance and a clean video-settings experience. This preview does not fetch an icon, inspect a profile, or contact a provider.");
        ImGui::Spacing();
        ImGui::PushFont(f_bold);
        ImGui::TextUnformatted("Categories");
        ImGui::PopFont();
        ImGui::TextColored(k.muted, "#performance   #client-side   #fabric");
        ImGui::Spacing();
        ImGui::PushFont(f_bold);
        ImGui::TextUnformatted("Compatible sample releases");
        ImGui::PopFont();
        ImGui::BulletText("0.5.11  •  Minecraft 1.20.1");
        ImGui::BulletText("0.5.10  •  Minecraft 1.20.1");
        ImGui::Spacing();
        if (primary_button("Close preview", ImVec2(ui_px(118.0f), ui_px(32.0f)))) {
            dismissed_fixture = st.fixture_case;
            open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (!open) dismissed_fixture = st.fixture_case;
}

static void draw_fixture_mod_dependency_overlay(UiState& st) {
    static std::string dismissed_fixture;
    constexpr const char* kPopupId = "##fixture_mod_manager_dependency_graph";
    if (!begin_fixture_mod_overlay(st, kPopupId, dismissed_fixture)) return;

    ImGui::SetNextWindowSizeConstraints(ImVec2(ui_px(360.0f), 0.0f),
                                        ImVec2(ui_px(520.0f), ui_px(680.0f)));
    bool open = true;
    if (ImGui::BeginPopupModal(kPopupId, &open, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("Dependency Graph");
        ImGui::PopFont();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextColored(k.brand,
                           "LOCAL VISUAL-QA FIXTURE — no installed-content scan is performed.");
        ImGui::TextColored(k.text, "Sodium 0.5.11");
        ImGui::Indent(ui_px(20.0f));
        ImGui::TextColored(k.green, "OK      Fabric Loader 0.15.11");
        ImGui::TextColored(k.green, "OK      Minecraft 1.20.1");
        ImGui::TextColored(k.red, "MISSING Indium (optional compatibility layer)");
        ImGui::Unindent(ui_px(20.0f));
        ImGui::Spacing();
        ImGui::TextWrapped("The real Dependencies view checks the active profile before any action. This sample illustrates both satisfied and missing states without reading it.");
        ImGui::Spacing();
        if (primary_button("Close preview", ImVec2(ui_px(118.0f), ui_px(32.0f)))) {
            dismissed_fixture = st.fixture_case;
            open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (!open) dismissed_fixture = st.fixture_case;
}

// The live installed-mod overflow menu can launch detail/recovery workflows.
// Keep the visual route in the inert façade so a review capture cannot inherit
// a real profile or reach one of those handlers.
static void draw_fixture_mod_actions_menu(UiState& st) {
    static std::string dismissed_fixture;
    constexpr const char* kPopupId = "##fixture_mod_manager_actions_menu";
    if (!begin_fixture_mod_overlay(st, kPopupId, dismissed_fixture)) return;

    // The menu is intentionally positioned next to the final representative
    // installed-mod row. That preserves the visual relationship a reviewer
    // sees in production without creating an interactive live overflow button.
    ImVec2 popup_pos = ImGui::GetCursorScreenPos();
    popup_pos.x -= ui_px(196.0f);
    popup_pos.y -= ui_px(74.0f);
    ImGui::SetNextWindowPos(popup_pos, ImGuiCond_Appearing);
    if (ImGui::BeginPopup(kPopupId, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextDisabled("Sodium 0.5.11 — local fixture preview");
        ImGui::Separator();
        ImGui::BeginDisabled();
        ImGui::MenuItem("View Details");
        ImGui::MenuItem("Update (use Updates tab)");
        ImGui::MenuItem("View Dependencies");
        ImGui::MenuItem("Move to Recovery...");
        ImGui::EndDisabled();
        ImGui::Separator();
        if (ImGui::MenuItem("Close preview")) {
            dismissed_fixture = st.fixture_case;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// The Browse overflow includes an external-browser handoff in production.
// Keep this deterministic preview in the fixture façade with every real
// action disabled; it must never share the live browse menu or its URL data.
static void draw_fixture_mod_browse_actions_menu(UiState& st) {
    static std::string dismissed_fixture;
    constexpr const char* kPopupId = "##fixture_mod_manager_browse_actions_menu";
    if (!begin_fixture_mod_overlay(st, kPopupId, dismissed_fixture)) return;

    ImVec2 popup_pos = ImGui::GetCursorScreenPos();
    popup_pos.x -= ui_px(192.0f);
    popup_pos.y -= ui_px(92.0f);
    ImGui::SetNextWindowPos(popup_pos, ImGuiCond_Appearing);
    if (ImGui::BeginPopup(kPopupId, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextDisabled("Sodium — local fixture preview");
        ImGui::Separator();
        ImGui::BeginDisabled();
        ImGui::MenuItem("View Details");
        ImGui::MenuItem("View on Website");
        ImGui::EndDisabled();
        ImGui::Separator();
        ImGui::TextColored(k.brand_hov,
                           "External browser handoff is disabled for visual review.");
        if (ImGui::MenuItem("Close preview")) {
            dismissed_fixture = st.fixture_case;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

static void draw_fixture_mod_manager_overlay(UiState& st) {
    if (st.fixture_case == "mod-manager-move-recovery-confirm" ||
        st.fixture_case == "mod-manager-move-recovery-working" ||
        st.fixture_case == "mod-manager-move-recovery-error") {
        draw_fixture_mod_recovery_overlay(st);
    } else if (st.fixture_case == "mod-manager-details") {
        draw_fixture_mod_details_overlay(st);
    } else if (st.fixture_case == "mod-manager-dependency-graph") {
        draw_fixture_mod_dependency_overlay(st);
    } else if (st.fixture_case == "mod-manager-installed-menu") {
        draw_fixture_mod_actions_menu(st);
    } else if (st.fixture_case == "mod-manager-browse-menu") {
        draw_fixture_mod_browse_actions_menu(st);
    }
}

static void draw_fixture_mod_manager_page(UiState& st) {
    const int tab = fixture_mod_manager_tab(st.fixture_case);
    page_title("Mod Manager", "Organize a profile's mods, updates, and publishing workflow");
    ImGui::TextColored(k.brand_hov,
                       "LOCAL VISUAL FIXTURE — no profile files, account data, provider calls, or icon downloads are used.");
    ImGui::Spacing();
    draw_fixture_mod_tab_bar(tab);
    ImGui::Spacing();
    draw_fixture_mod_operation_status(st);

    if (tab == 0) {
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("Installed mods");
        ImGui::PopFont();
        ImGui::TextColored(k.muted, "3 representative entries  •  Fabric 1.20.1");
        ImGui::Spacing();
        draw_fixture_mod_row("##fixture_mod_installed_api", "Fabric API", "Core library",
                             "0.92.2+1.20.1", k.brand);
        draw_fixture_mod_row("##fixture_mod_installed_lithium", "Lithium", "Performance optimization",
                             "0.12.7", k.green);
        draw_fixture_mod_row("##fixture_mod_installed_sodium", "Sodium", "Renderer",
                             "0.5.11", k.blue);
    } else if (tab == 1) {
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("Browse mods");
        ImGui::PopFont();
        ImGui::TextColored(k.muted, "Representative catalog preview for layout review.");
        ImGui::Spacing();
        card_begin("##fixture_mod_browse_filters", ImVec2(-1, 0));
        static std::string fixture_search = "performance";
        ImGui::SetNextItemWidth(ui_px(260.0f));
        ImGui::BeginDisabled();
        ImGui::InputTextWithHint("##fixture_mod_search", "Search mods", &fixture_search);
        ImGui::EndDisabled();
        ImGui::SameLine(0, ui_px(6.0f));
        primary_button("Search", ImVec2(ui_px(92.0f), ui_px(30.0f)), false, true);
        ImGui::SameLine(0, ui_px(16.0f));
        ImGui::TextColored(k.muted, "Fabric  •  1.20.1  •  Popular");
        card_end();
        ImGui::Spacing();
        draw_fixture_mod_row("##fixture_mod_browse_sodium", "Sodium",
                             "Modern renderer with a clean video settings experience.", "by CaffeineMC", k.brand);
        draw_fixture_mod_row("##fixture_mod_browse_lithium", "Lithium",
                             "Broad game-logic optimizations for responsive play.", "by CaffeineMC", k.green);
        draw_fixture_mod_row("##fixture_mod_browse_ferrite", "FerriteCore",
                             "Reduces memory pressure without changing gameplay.", "by malte0811", k.blue);
    } else if (tab == 2) {
        const bool checking = st.fixture_case == "mod-manager-update-check-working";
        const bool check_error = st.fixture_case == "mod-manager-update-check-error";
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("Available updates");
        ImGui::PopFont();
        ImGui::TextColored(k.muted, checking
            ? "Checking representative compatible updates; no profile scan is performed."
            : "Representative update status; no profile scan is performed.");
        ImGui::Spacing();
        card_begin("##fixture_mod_updates", ImVec2(-1, 0));
        if (checking) {
            ImGui::PushFont(f_bold);
            ImGui::TextColored(k.brand, "Checking compatible updates");
            ImGui::PopFont();
            ImGui::ProgressBar(0.46f, ImVec2(-1, ui_px(8.0f)), "46%");
            ImGui::TextColored(k.muted,
                               "Comparing the representative Fabric 1.20.1 sample against its saved metadata.");
            primary_button("Checking…", ImVec2(ui_px(132.0f), ui_px(30.0f)), false, true);
        } else if (check_error) {
            ImGui::PushFont(f_bold);
            ImGui::TextColored(k.red, "Update check needs attention");
            ImGui::PopFont();
            ImGui::TextWrapped("The update service could not verify the representative sample. Your installed mods were not changed; retry the check when the service is available.");
            ImGui::TextColored(k.brand_hov,
                               "LOCAL VISUAL FIXTURE — no provider request was made and retry is disabled.");
            ImGui::Spacing();
            primary_button("Retry Update Check", ImVec2(ui_px(158.0f), ui_px(30.0f)), false, true);
        } else {
            ImGui::TextColored(k.text, "Sodium");
            ImGui::SameLine(0, ui_px(8.0f));
            ImGui::TextColored(k.muted, "0.5.10  →  0.5.11");
            ImGui::TextColored(k.green, "Compatibility checked for Fabric 1.20.1");
            ImGui::Spacing();
            primary_button("Install update", ImVec2(ui_px(118.0f), ui_px(30.0f)), false, true);
        }
        card_end();
    } else if (tab == 3) {
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("Dependencies");
        ImGui::PopFont();
        ImGui::TextColored(k.muted, "Representative dependency graph for the selected mod.");
        ImGui::Spacing();
        card_begin("##fixture_mod_dependencies", ImVec2(-1, 0));
        ImGui::TextColored(k.brand, "Sodium 0.5.11");
        ImGui::Indent(ui_px(22.0f));
        ImGui::TextColored(k.text, "└ Fabric Loader 0.15.11");
        ImGui::TextColored(k.text, "└ Minecraft 1.20.1");
        ImGui::Unindent(ui_px(22.0f));
        ImGui::TextColored(k.green, "All required dependencies are present in this sample.");
        card_end();
    } else if (tab == 4) {
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("Mod settings");
        ImGui::PopFont();
        ImGui::TextColored(k.muted, "Representative preferences; controls cannot change launcher configuration.");
        ImGui::Spacing();
        card_begin("##fixture_mod_settings", ImVec2(-1, 0));
        bool check_updates = true;
        bool automatic_update = false;
        bool beta_versions = false;
        ImGui::BeginDisabled();
        ImGui::Checkbox("Check for updates at launch", &check_updates);
        ImGui::Checkbox("Automatically install compatible updates", &automatic_update);
        ImGui::Checkbox("Include beta versions", &beta_versions);
        ImGui::EndDisabled();
        card_end();
    } else {
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("Publish project");
        ImGui::PopFont();
        ImGui::TextColored(k.muted,
                           "Representative approved-project workflow; no account service or profile export is contacted.");
        ImGui::Spacing();
        card_begin("##fixture_mod_publish", ImVec2(-1, 0));
        static std::string project_name = "Aurora Frontier";
        static std::string project_slug = "aurora-frontier";
        ImGui::TextColored(k.muted, "PROJECT NAME");
        ImGui::SetNextItemWidth(-1);
        ImGui::BeginDisabled();
        ImGui::InputText("##fixture_publish_name", &project_name);
        ImGui::TextColored(k.muted, "SLUG");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##fixture_publish_slug", &project_slug);
        ImGui::EndDisabled();
        ImGui::Spacing();
        primary_button("Create draft project", ImVec2(ui_px(160.0f), ui_px(30.0f)), false, true);
        card_end();
    }
    draw_fixture_mod_manager_overlay(st);
}

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

struct PublishWorkerContext {
    PublishAsyncScope scope;
    std::shared_ptr<std::atomic_bool> stop_requested;

    bool stop_requested_now() const {
        return stop_requested && stop_requested->load();
    }
};

struct PublishMediaInput {
    std::string kind;
    std::string local_path;
};

class PublishWorkReset {
public:
    explicit PublishWorkReset(std::shared_ptr<std::atomic_bool> work_in_progress)
        : work_in_progress_(std::move(work_in_progress)) {}

    ~PublishWorkReset() {
        if (work_in_progress_) work_in_progress_->store(false);
    }

private:
    std::shared_ptr<std::atomic_bool> work_in_progress_;
};

static AsyncUiRequestResult publish_result(bool success, bool warning,
                                           const char* title, std::string detail) {
    AsyncUiRequestResult result;
    result.success = success;
    result.warning = warning;
    result.title = title;
    result.detail = std::move(detail);
    return result;
}

static AsyncUiRequestResult publish_stopped_result(std::string detail) {
    return publish_result(true, true, "Publish stopped", std::move(detail));
}

static AsyncUiRequestResult publish_session_changed_result(
    const std::string& detail = {}) {
    return publish_result(
        false, true, "Account changed",
        detail.empty()
            ? "Your Amalgam account changed or signed out while this request was running. "
              "The result was not applied to the current Publish form."
            : detail);
}

static std::string publish_error_or(const std::string& error, const char* fallback) {
    return error.empty() ? std::string(fallback) : error;
}

// Account mutations and this publish worker share the same UiState-owned
// request lane. The repeated checks are still necessary: a worker can finish
// after a sign-out or other external session replacement, and must not issue a
// later remote write with that new session or expose the earlier result.
static bool publish_worker_session_matches(const PublishWorkerContext& context,
                                           aml::supabase::SupabaseClient*& client,
                                           std::string* error = nullptr) {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    if (supabase.session_generation() != context.scope.session_generation) {
        if (error) *error = "The Amalgam account session changed.";
        return false;
    }
    auto* current = supabase.client();
    if (!current || !current->is_authenticated()) {
        if (error) *error = "The account is no longer signed in.";
        return false;
    }
    const std::string current_identity = current->current_user().id;
    if (current_identity.empty() || current_identity != context.scope.account_identity) {
        if (error) *error = "The signed-in Amalgam account changed.";
        return false;
    }
    client = current;
    return true;
}

static bool read_publish_binary_file(const std::wstring& path, int64_t maximum_size,
                                     std::vector<uint8_t>& out, int64_t* size_out,
                                     std::string* error) {
    std::ifstream input(std::filesystem::path(path), std::ios::binary);
    if (!input) {
        if (error) *error = "could not open the selected file";
        return false;
    }
    input.seekg(0, std::ios::end);
    const std::streamsize size = input.tellg();
    input.seekg(0, std::ios::beg);
    if (size <= 0 || size > static_cast<std::streamsize>(maximum_size)) {
        if (error) *error = "the selected file is empty or exceeds the publish size limit";
        return false;
    }
    out.resize(static_cast<size_t>(size));
    input.read(reinterpret_cast<char*>(out.data()), size);
    if (!input) {
        if (error) *error = "could not read the selected file completely";
        return false;
    }
    if (size_out) *size_out = static_cast<int64_t>(size);
    return true;
}

static bool upload_publish_media(const PublishWorkerContext& context,
                                 const std::string& project_id, const std::string& kind,
                                 const std::string& local_path, bool* stopped,
                                 std::string* error) {
    if (stopped) *stopped = false;
    const auto mark_stopped = [&](const std::string& detail) {
        if (stopped) *stopped = true;
        if (error) *error = detail;
        return false;
    };
    if (context.stop_requested_now()) {
        return mark_stopped("Stopped before the " + kind + " file was uploaded.");
    }
    if (local_path.empty()) {
        if (error) *error = kind + " media file is not selected";
        return false;
    }
    const std::wstring path = net::to_wide(local_path);
    if (!net::file_exists(path)) {
        if (error) *error = "media file does not exist: " + local_path;
        return false;
    }
    aml::supabase::SupabaseClient* client = nullptr;
    if (!publish_worker_session_matches(context, client, error)) return false;

    std::vector<uint8_t> bytes;
    int64_t size = 0;
    if (!read_publish_binary_file(path, 512LL * 1024LL * 1024LL, bytes, &size, error)) {
        return false;
    }
    if (context.stop_requested_now()) {
        return mark_stopped("Stopped before the " + kind + " file was sent to project storage.");
    }
    if (!publish_worker_session_matches(context, client, error)) return false;

    const std::string filename = std::filesystem::path(path).filename().string();
    const std::string storage_path = "projects/" + project_id + "/media/" + filename;
    const std::string sha256 = net::sha256_file(path);
    if (sha256.empty()) {
        if (error) *error = "could not verify the selected media file";
        return false;
    }
    aml::supabase::SupabaseClient::StorageUploadOptions upload{};
    upload.bucket = "project-media";
    upload.path = storage_path;
    upload.data = std::move(bytes);
    upload.content_type = media_mime(path);
    upload.upsert = true;
    const auto uploaded = client->upload_file(upload);
    if (!uploaded.success) {
        if (error) *error = publish_error_or(uploaded.error, "the media file could not be uploaded");
        return false;
    }
    if (!publish_worker_session_matches(context, client, error)) {
        if (error) {
            *error = "The account changed after the file upload. The file may remain in project "
                     "storage, but no further change was made.";
        }
        return false;
    }
    if (context.stop_requested_now()) {
        return mark_stopped("Stopped after the " + kind +
                            " file reached project storage. It was not removed or attached to "
                            "the project record.");
    }

    publishing::Media media{};
    media.project_id = project_id;
    media.kind = kind;
    media.storage_path = storage_path;
    media.mime_type = upload.content_type;
    media.size_bytes = size;
    media.sha256 = sha256;
    media.title = filename;
    if (!publishing::add_media(*client, media, error)) return false;
    if (!publish_worker_session_matches(context, client, error)) {
        if (error) {
            *error = "The account changed after the media record was saved. The result was not "
                     "applied to the current Publish form.";
        }
        return false;
    }
    if (context.stop_requested_now() && stopped) *stopped = true;
    return true;
}

// export_mrpack and the zip helper create sibling staging files while they
// work. Put every one of those files inside a freshly-created, uniquely named
// workspace and delete only that workspace after the request finishes. This
// avoids "cleanup" ever targeting a pre-existing profile file.
class PublishTempArtifact {
public:
    PublishTempArtifact(std::wstring profile_directory, std::wstring workspace_name)
        : profile_directory_(std::filesystem::path(std::move(profile_directory)).lexically_normal()),
          workspace_name_(std::move(workspace_name)) {
        workspace_path_ = (profile_directory_ / workspace_name_).lexically_normal();
        artifact_path_ = (workspace_path_ / L"artifact.mrpack").lexically_normal();
    }

    ~PublishTempArtifact() {
        std::string ignored;
        cleanup(&ignored);
    }

    bool reserve(std::string* error) {
        if (!is_owned_workspace()) {
            if (error) *error = "the temporary publish workspace path was rejected";
            return false;
        }
        std::error_code ec;
        const bool created = std::filesystem::create_directory(workspace_path_, ec);
        if (ec) {
            if (error) *error = "could not create the temporary publish workspace: " + ec.message();
            return false;
        }
        if (!created) {
            if (error) *error = "a temporary publish workspace already exists; retry the request";
            return false;
        }
        owned_ = true;
        return true;
    }

    bool cleanup(std::string* error) {
        if (!owned_) return true;
        if (!is_owned_workspace()) {
            if (error) *error = "temporary publish workspace ownership could not be verified";
            return false;
        }
        std::error_code ec;
        std::filesystem::remove_all(workspace_path_, ec);
        if (ec) {
            if (error) *error = "could not remove Amalgam's temporary export workspace: " + ec.message();
            return false;
        }
        owned_ = false;
        return true;
    }

    const std::wstring path() const { return artifact_path_.wstring(); }

private:
    bool is_owned_workspace() const {
        const std::filesystem::path workspace_name(workspace_name_);
        return !profile_directory_.empty() && !workspace_name_.empty() &&
               workspace_name_.rfind(L".amalgam-publish-", 0) == 0 &&
               !workspace_name.has_parent_path() &&
               workspace_path_.parent_path().lexically_normal() == profile_directory_ &&
               artifact_path_.parent_path().lexically_normal() == workspace_path_ &&
               artifact_path_.filename() == std::filesystem::path(L"artifact.mrpack");
    }

    std::filesystem::path profile_directory_;
    std::filesystem::path workspace_path_;
    std::filesystem::path artifact_path_;
    std::wstring workspace_name_;
    bool owned_ = false;
};

static AsyncUiRequestResult finish_publish_temp_artifact(PublishTempArtifact& artifact,
                                                          AsyncUiRequestResult result) {
    std::string cleanup_error;
    if (!artifact.cleanup(&cleanup_error)) {
        result.warning = true;
        if (!result.detail.empty()) result.detail += " ";
        result.detail += "The temporary local export workspace needs attention: " + cleanup_error;
    }
    return result;
}

static std::atomic_uint64_t g_publish_artifact_sequence{0};

static AsyncUiRequestResult create_publish_draft_request(const PublishWorkerContext& context,
                                                         publishing::ProjectDraft draft) {
    if (context.stop_requested_now()) {
        return publish_stopped_result("Stopped before a draft project was created.");
    }
    aml::supabase::SupabaseClient* client = nullptr;
    std::string error;
    if (!publish_worker_session_matches(context, client, &error)) {
        return publish_session_changed_result();
    }
    publishing::Project project{};
    if (!publishing::create_project(*client, draft, project, &error)) {
        return publish_result(false, false, "Create draft failed",
                              publish_error_or(error, "The project draft could not be created."));
    }
    if (!publish_worker_session_matches(context, client, &error)) {
        return publish_session_changed_result();
    }
    AsyncUiRequestResult result = publish_result(
        true, context.stop_requested_now(), "Draft created",
        context.stop_requested_now()
            ? "The draft was created before the stop request could take effect."
            : "Your project draft is ready for artwork, version export, and staff review.");
    result.payload_a = project.id;
    result.payload_b = project.status;
    return result;
}

static AsyncUiRequestResult upload_publish_media_request(
    const PublishWorkerContext& context, const std::string& project_id,
    const std::vector<PublishMediaInput>& media_files) {
    int uploaded_count = 0;
    for (const auto& media : media_files) {
        bool stopped = false;
        std::string error;
        if (!upload_publish_media(context, project_id, media.kind, media.local_path, &stopped,
                                  &error)) {
            if (stopped) return publish_stopped_result(error);
            return publish_result(false, false, "Media upload failed",
                                  publish_error_or(error,
                                                   "A selected media file could not be uploaded."));
        }
        ++uploaded_count;
        if (stopped || context.stop_requested_now()) {
            return publish_stopped_result(
                "Stopped after uploading " + std::to_string(uploaded_count) +
                " selected file(s). Uploaded media was not removed; review the project before retrying.");
        }
    }
    return publish_result(true, false, "Media uploaded",
                          std::to_string(uploaded_count) + " selected media file(s) uploaded.");
}

static AsyncUiRequestResult create_and_publish_version_request(
    const PublishWorkerContext& context, std::wstring profile_directory,
    std::string project_id, std::string version, std::string changelog) {
    if (context.stop_requested_now()) {
        return publish_stopped_result("Stopped before the active profile was exported. No version was created.");
    }
    aml::supabase::SupabaseClient* client = nullptr;
    std::string error;
    if (!publish_worker_session_matches(context, client, &error)) {
        return publish_session_changed_result();
    }

    instances::Instance instance{};
    if (!instances::load(profile_directory, instance, &error)) {
        return publish_result(false, false, "Profile load failed",
                              publish_error_or(error, "The active profile could not be loaded."));
    }
    if (context.stop_requested_now()) {
        return publish_stopped_result("Stopped before the active profile was exported. No version was created.");
    }

    const uint64_t request_id = g_publish_artifact_sequence.fetch_add(1) + 1;
    const std::wstring workspace_name =
        L".amalgam-publish-" + net::to_wide(version) + L"-" + std::to_wstring(request_id);
    PublishTempArtifact artifact(std::move(profile_directory), workspace_name);
    if (!artifact.reserve(&error)) {
        return publish_result(false, false, "Export setup failed",
                              publish_error_or(error, "The temporary export could not be prepared."));
    }
    const auto finish = [&artifact](AsyncUiRequestResult result) {
        return finish_publish_temp_artifact(artifact, std::move(result));
    };

    // import_pack uses Instance::id in its staging directory name. The active
    // profile metadata is user-editable, so give this worker a safe local
    // staging id while preserving the original id as the human-facing name.
    const std::string source_instance_id = instance.id;
    if (instance.name.empty()) {
        instance.name = source_instance_id.empty() ? "Amalgam project" : source_instance_id;
    }
    instance.id = "publish-" + std::to_string(request_id);
    if (!import_pack::export_mrpack(instance, artifact.path(), &error)) {
        return finish(publish_result(false, false, "Export failed",
                                     publish_error_or(error, "The profile export failed.")));
    }
    if (context.stop_requested_now()) {
        return finish(publish_stopped_result(
            "Stopped after the local export. Amalgam removed only its temporary export; no version was created."));
    }
    if (!publish_worker_session_matches(context, client, &error)) {
        return finish(publish_session_changed_result());
    }

    std::vector<uint8_t> artifact_bytes;
    int64_t artifact_size = 0;
    if (!read_publish_binary_file(artifact.path(), 2LL * 1024LL * 1024LL * 1024LL,
                                  artifact_bytes, &artifact_size, &error)) {
        return finish(publish_result(false, false, "Artifact read failed",
                                     publish_error_or(error,
                                                      "The temporary export could not be read.")));
    }
    const std::string artifact_sha256 = net::sha256_file(artifact.path());
    if (artifact_sha256.empty()) {
        return finish(publish_result(false, false, "Artifact verification failed",
                                     "The temporary export could not be verified."));
    }
    if (context.stop_requested_now()) {
        return finish(publish_stopped_result(
            "Stopped before the exported artifact was sent. Amalgam removed only its temporary export."));
    }
    if (!publish_worker_session_matches(context, client, &error)) {
        return finish(publish_session_changed_result());
    }

    const std::string artifact_path =
        "projects/" + project_id + "/versions/" + version + ".mrpack";
    aml::supabase::SupabaseClient::StorageUploadOptions upload{};
    upload.bucket = "project-artifacts";
    upload.path = artifact_path;
    upload.data = std::move(artifact_bytes);
    upload.content_type = "application/zip";
    upload.upsert = false;
    const auto stored = client->upload_file(upload);
    if (!stored.success) {
        return finish(publish_result(false, false, "Artifact upload failed",
                                     publish_error_or(stored.error,
                                                      "The version artifact could not be uploaded.")));
    }
    if (!publish_worker_session_matches(context, client, &error)) {
        return finish(publish_session_changed_result(
            "The account changed after the artifact upload. The artifact may remain in project "
            "storage, but no version record was created by this request."));
    }
    if (context.stop_requested_now()) {
        return finish(publish_stopped_result(
            "Stopped after the artifact upload. The uploaded artifact was not removed, and no version "
            "record was created."));
    }

    publishing::Version draft{};
    draft.project_id = project_id;
    draft.version = version;
    draft.changelog = changelog;
    draft.artifact_path = artifact_path;
    draft.artifact_size = artifact_size;
    draft.artifact_sha256 = artifact_sha256;
    publishing::Version created{};
    if (!publishing::create_version(*client, draft, created, &error)) {
        return finish(publish_result(false, false, "Version creation failed",
                                     publish_error_or(error,
                                                      "The version record could not be created.")));
    }
    if (!publish_worker_session_matches(context, client, &error)) {
        return finish(publish_session_changed_result(
            "The account changed after the version record was created. The result was not applied to "
            "the current Publish form."));
    }
    if (context.stop_requested_now()) {
        return finish(publish_result(
            true, true, "Version created",
            "The version record was created, but the stop request prevented publication. Review the "
            "project before retrying so a second version is not created."));
    }

    if (!publishing::publish_version(*client, project_id, created.id, &error)) {
        if (!publish_worker_session_matches(context, client, &error)) {
            return finish(publish_session_changed_result(
                "The account changed while publication was being confirmed. Review the project before "
                "trying again."));
        }
        return finish(publish_result(
            true, true, "Version created",
            "The version record was created, but publication could not be confirmed: " +
                publish_error_or(error, "the service did not confirm publication") +
                ". Review the project before retrying; first publication may require staff approval."));
    }
    if (!publish_worker_session_matches(context, client, &error)) {
        return finish(publish_session_changed_result(
            "The account changed after publication was confirmed. The result was not applied to the "
            "current Publish form."));
    }
    AsyncUiRequestResult result = publish_result(
        true, context.stop_requested_now(), "Version published",
        context.stop_requested_now()
            ? "The version was published before the stop request could take effect."
            : "Approved projects can publish future versions without repeating staff review.");
    result.payload_a = "approved";
    return finish(std::move(result));
}

static AsyncUiRequestResult submit_publish_review_request(const PublishWorkerContext& context,
                                                          const std::string& project_id) {
    if (context.stop_requested_now()) {
        return publish_stopped_result("Stopped before the project was submitted for staff review.");
    }
    aml::supabase::SupabaseClient* client = nullptr;
    std::string error;
    if (!publish_worker_session_matches(context, client, &error)) {
        return publish_session_changed_result();
    }
    if (!publishing::submit_for_review(*client, project_id, &error)) {
        return publish_result(false, false, "Staff-review submission failed",
                              publish_error_or(error,
                                               "The project could not be submitted for staff review."));
    }
    if (!publish_worker_session_matches(context, client, &error)) {
        return publish_session_changed_result(
            "The account changed after staff-review submission. The result was not applied to the "
            "current Publish form.");
    }
    AsyncUiRequestResult result = publish_result(
        true, context.stop_requested_now(), "Submitted for staff review",
        context.stop_requested_now()
            ? "The project was submitted before the stop request could take effect."
            : "Staff will review this project for safety before its first publication.");
    result.payload_a = "pending_review";
    return result;
}

using PublishRequestWork = std::function<AsyncUiRequestResult(const PublishWorkerContext&)>;

static bool start_publish_request(UiState& st, ModManagerUIState& state,
                                  const char* action, PublishAsyncScope scope,
                                  PublishRequestWork work) {
    if (mod_manager_operation_busy(state)) {
        state.publish_error = "Another Mod Manager job is still finishing. Wait for it before publishing.";
        state.publish_feedback.clear();
        state.publish_feedback_warning = false;
        state.publish_retry_action = action;
        return false;
    }
    if (async_ui_request_is_reserved(st.auth_async_request)) {
        state.publish_error = "Another account request is still finishing. Wait for it before publishing.";
        state.publish_feedback.clear();
        state.publish_feedback_warning = false;
        state.publish_retry_action = action;
        return false;
    }

    auto stop_requested = std::make_shared<std::atomic_bool>(false);
    auto work_in_progress = std::make_shared<std::atomic_bool>(true);
    if (!start_auth_async_request(
            st, action,
            [scope, stop_requested, work_in_progress,
             work = std::move(work)]() mutable {
                PublishWorkReset reset(work_in_progress);
                return work(PublishWorkerContext{scope, stop_requested});
    })) {
        state.publish_error = "The account request lane became busy. Wait for it to finish, then retry.";
        state.publish_feedback.clear();
        state.publish_feedback_warning = false;
        state.publish_retry_action = action;
        return false;
    }

    state.publish_request_scope = scope;
    state.publish_request_action = action;
    state.publish_retry_action.clear();
    state.publish_stop_requested = std::move(stop_requested);
    state.publish_work_in_progress = std::move(work_in_progress);
    state.publish_feedback.clear();
    state.publish_feedback_warning = false;
    state.publish_error.clear();
    return true;
}

static bool publish_version_is_safe_path_segment(const std::string& version) {
    if (version.empty() || version == "." || version == ".." || version.size() > 80) return false;
    return std::all_of(version.begin(), version.end(), [](unsigned char ch) {
        return std::isalnum(ch) || ch == '.' || ch == '-' || ch == '_' || ch == '+';
    });
}

static void set_publish_local_error(UiState& st, ModManagerUIState& state,
                                    const char* title, const std::string& detail) {
    state.publish_error = detail;
    state.publish_feedback.clear();
    state.publish_feedback_warning = false;
    push_notice(st, ui_model::NoticeLevel::Warning, title, detail);
}

static void consume_mod_publish_result(UiState& st, ModManagerUIState& state) {
    const auto snapshot = snapshot_async_ui_request(st.auth_async_request);
    if (!is_mod_publish_action(snapshot.action)) return;

    AsyncUiRequestSnapshot completed;
    if (!take_auth_async_request_result(st, snapshot.action, &completed)) return;

    const std::string action = completed.action;
    const auto current_user = aml::supabase::SupabaseManager::instance().get_current_user();
    const bool scope_matches = publish_async_scope_matches(
        state.publish_request_scope, current_user.id,
        aml::supabase::SupabaseManager::instance().session_generation(),
        state.publish_project_id);
    state.publish_work_in_progress.reset();
    state.publish_stop_requested.reset();
    state.publish_request_action.clear();
    state.publish_request_scope = {};

    if (!scope_matches) {
        // Never apply the old result (especially a newly-created project ID)
        // to a different account or project. The user can safely inspect their
        // current project state and explicitly begin a new action instead.
        state.publish_retry_action.clear();
        state.publish_feedback.clear();
        state.publish_feedback_warning = false;
        state.publish_error =
            "The account or selected project changed while the publish request was running. "
            "Its result was not applied to this form.";
        push_notice(st, ui_model::NoticeLevel::Warning, "Publish result not applied",
                    state.publish_error);
        return;
    }

    const auto& result = completed.result;
    if (!result.success) {
        state.publish_feedback.clear();
        state.publish_feedback_warning = false;
        state.publish_error = publish_error_or(result.detail, "The publish request could not finish.");
        // A session-scope failure is intentionally not offered as a blind
        // retry: the user must first establish the current account/project.
        state.publish_retry_action = result.warning ? "" : action;
        push_notice(st, result.warning ? ui_model::NoticeLevel::Warning : ui_model::NoticeLevel::Error,
                    result.title.empty() ? "Publish request failed" : result.title,
                    state.publish_error);
        return;
    }

    if (action == kPublishCreateDraftAction) {
        if (result.payload_a.empty()) {
            state.publish_feedback.clear();
            state.publish_feedback_warning = false;
            state.publish_error = "The draft request completed without a project identifier. Retry the draft action.";
            state.publish_retry_action = action;
            push_notice(st, ui_model::NoticeLevel::Error, "Draft result incomplete", state.publish_error);
            return;
        }
        state.publish_project_id = result.payload_a;
        state.publish_status = result.payload_b.empty() ? "draft" : result.payload_b;
    } else if (action == kPublishCreateVersionAction && result.payload_a == "approved") {
        state.publish_status = "approved";
    } else if (action == kPublishSubmitReviewAction && !result.payload_a.empty()) {
        state.publish_status = result.payload_a;
    }

    state.publish_error.clear();
    state.publish_feedback = publish_error_or(result.detail, "The publish request completed.");
    state.publish_feedback_warning = result.warning;
    state.publish_retry_action.clear();
    push_notice(st, result.warning ? ui_model::NoticeLevel::Info : ui_model::NoticeLevel::Success,
                result.title.empty() ? "Publish request complete" : result.title,
                state.publish_feedback);
}

static void draw_publish_feedback(const ModManagerUIState& state) {
    if (state.publish_error.empty() && state.publish_feedback.empty()) return;
    const bool is_error = !state.publish_error.empty();
    const bool is_warning = !is_error && state.publish_feedback_warning;
    const ImVec4 tone = is_error ? k.red : (is_warning ? k.yellow : k.green);
    const char* title = is_error ? "Publish needs attention"
                                 : (is_warning ? "Publish update" : "Publish complete");
    const std::string& detail = is_error ? state.publish_error : state.publish_feedback;
    card_begin("##publish_feedback", ImVec2(-1, 0));
    ImGui::PushFont(f_bold);
    ImGui::TextColored(tone, "%s", title);
    ImGui::PopFont();
    ImGui::PushTextWrapPos();
    ImGui::TextColored(k.text, "%s", detail.c_str());
    ImGui::PopTextWrapPos();
    if (is_error) {
        ImGui::Spacing();
        ImGui::TextColored(k.muted, "%s",
                           state.publish_retry_action.empty()
                               ? "Check the active account and project, then begin a new publish action."
                               : "Correct the issue, then use the same Publish action to retry. Nothing retries automatically.");
    }
    card_end();
    ImGui::Spacing();
}

static void draw_publish_progress(ModManagerUIState& state, std::string_view action,
                                  bool working) {
    if (!working) return;
    card_begin("##publish_progress", ImVec2(-1, 0));
    ImGui::PushFont(f_bold);
    ImGui::TextColored(k.yellow, "%s", publish_action_label(action));
    ImGui::PopFont();
    const bool stop_requested = state.publish_stop_requested &&
                                state.publish_stop_requested->load();
    ImGui::TextColored(k.muted, "%s", stop_requested
        ? "Stopping at the next safe checkpoint."
        : "This runs in the background; you can read the rest of the launcher while it finishes.");
    ImGui::Spacing();
    if (stop_requested) {
        ghost_button("Stop requested", ImVec2(ui_px(132.0f), ui_px(28.0f)), true);
    } else if (ghost_button("Stop after current step", ImVec2(ui_px(186.0f), ui_px(28.0f)),
                            !state.publish_stop_requested)) {
        state.publish_stop_requested->store(true);
    }
    ImGui::PushTextWrapPos();
    ImGui::TextColored(k.muted,
                       "A request already sent to the service cannot be undone. Amalgam only removes "
                       "the temporary local export created by this operation.");
    ImGui::PopTextWrapPos();
    card_end();
    ImGui::Spacing();
}

void draw_mod_manager_publish(UiState& st) {
    auto& ui = get_mod_manager_ui_state();
    consume_mod_publish_result(st, ui);
    page_title("Publish Project", "Create a project, submit it for one-time staff safety review, and publish future versions yourself.");
    draw_mod_operation_status(ui);

    auto& supabase = aml::supabase::SupabaseManager::instance();
    const auto current_user = supabase.get_current_user();
    const uint64_t session_generation = supabase.session_generation();
    if (current_user.id.empty()) {
        bind_publish_form_to_account(ui, {}, session_generation);
        empty_state("Sign in required", "Sign in to create and publish Amalgam projects.", "@");
        return;
    }
    bind_publish_form_to_account(ui, current_user.id, session_generation);

    const auto auth_snapshot = snapshot_async_ui_request(st.auth_async_request);
    const bool publish_request_working = auth_snapshot.working &&
        is_mod_publish_action(auth_snapshot.action);
    const bool account_lane_reserved = async_ui_request_is_reserved(st.auth_async_request);
    const bool mod_operation_running = mod_manager_operation_busy(ui);
    const bool form_locked = account_lane_reserved || mod_operation_running;

    if (account_lane_reserved && !publish_request_working) {
        ImGui::TextColored(k.yellow,
                           "Another account request is still finishing. Publishing will unlock when it is complete.");
        ImGui::Spacing();
    } else if (mod_operation_running && !publish_request_working) {
        ImGui::TextColored(k.yellow,
                           "Another Mod Manager job is still finishing. Publishing will unlock when it is complete.");
        ImGui::Spacing();
    }
    draw_publish_feedback(ui);
    draw_publish_progress(ui, auth_snapshot.action, publish_request_working);

    card_begin("##publish_project");
    ImGui::TextUnformatted("Project Details");
    ImGui::Separator();
    ImGui::BeginDisabled(form_locked);
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
        const char* button_label = ui.publish_retry_action == kPublishCreateDraftAction
            ? "Retry Create Draft" : "Create Draft Project";
        if (primary_button(publish_request_working && auth_snapshot.action == kPublishCreateDraftAction
                               ? "Creating Draft..." : button_label,
                           ImVec2(ui_px(180.0f), ui_px(32.0f)),
                           publish_request_working && auth_snapshot.action == kPublishCreateDraftAction,
                           form_locked)) {
            publishing::ProjectDraft draft{};
            draft.name = ui.publish_project_name;
            draft.slug = ui.publish_project_slug;
            draft.description = ui.publish_description;
            draft.type = ui.publish_type;
            if (draft.name.empty() || draft.slug.empty()) {
                set_publish_local_error(st, ui, "Missing details", "Name and slug are required.");
            } else {
                const PublishAsyncScope scope{current_user.id, session_generation, {}};
                start_publish_request(
                    st, ui, kPublishCreateDraftAction, scope,
                    [draft = std::move(draft)](const PublishWorkerContext& context) mutable {
                        return create_publish_draft_request(context, std::move(draft));
                    });
            }
        }
    } else {
        ImGui::TextColored(k.green, "Project ID: %s", ui.publish_project_id.c_str());
        ImGui::TextColored(k.muted, "Status: %s", ui.publish_status.empty() ? "draft" : ui.publish_status.c_str());
    }
    ImGui::EndDisabled();
    card_end();

    if (ui.publish_project_id.empty()) return;
    ImGui::Spacing();

    card_begin("##publish_media");
    ImGui::TextUnformatted("Artwork and Video");
    ImGui::TextColored(k.muted, "Upload an icon, banner, gallery image, or demonstration video for the project.");
    ImGui::BeginDisabled(form_locked);
    auto media_picker = [&](const char* label, const wchar_t* filter, std::string& path) {
        ImGui::TextUnformatted(label);
        ImGui::SameLine(ui_px(120.0f));
        ImGui::SetNextItemWidth(ui_px(420.0f));
        ImGui::InputText((std::string("##") + label).c_str(), &path);
        ImGui::SameLine();
        if (ghost_button((std::string("Browse##") + label).c_str(), ImVec2(ui_px(80.0f), ui_px(26.0f)),
                         form_locked))
            choose_publish_file(st.hwnd, filter, path);
    };
    media_picker("Icon", L"Images\0*.png;*.jpg;*.jpeg;*.webp\0\0", ui.publish_icon_path);
    media_picker("Banner", L"Images\0*.png;*.jpg;*.jpeg;*.webp\0\0", ui.publish_banner_path);
    media_picker("Gallery", L"Images\0*.png;*.jpg;*.jpeg;*.webp\0\0", ui.publish_gallery_path);
    media_picker("Video", L"Videos\0*.mp4;*.webm\0\0", ui.publish_video_path);
    const char* media_button_label = ui.publish_retry_action == kPublishUploadMediaAction
        ? "Retry Upload Media" : "Upload Media";
    if (ghost_button(publish_request_working && auth_snapshot.action == kPublishUploadMediaAction
                         ? "Uploading Media..." : media_button_label,
                     ImVec2(ui_px(146.0f), ui_px(30.0f)), form_locked)) {
        const bool has_media = !ui.publish_icon_path.empty() || !ui.publish_banner_path.empty() ||
            !ui.publish_gallery_path.empty() || !ui.publish_video_path.empty();
        if (!has_media) {
            set_publish_local_error(st, ui, "No media selected",
                                    "Select at least one artwork or video file before uploading.");
        } else {
            std::vector<PublishMediaInput> media_files;
            if (!ui.publish_icon_path.empty()) media_files.push_back({"icon", ui.publish_icon_path});
            if (!ui.publish_banner_path.empty()) media_files.push_back({"banner", ui.publish_banner_path});
            if (!ui.publish_gallery_path.empty()) media_files.push_back({"gallery", ui.publish_gallery_path});
            if (!ui.publish_video_path.empty()) media_files.push_back({"video", ui.publish_video_path});
            const PublishAsyncScope scope{current_user.id, session_generation, ui.publish_project_id};
            start_publish_request(
                st, ui, kPublishUploadMediaAction, scope,
                [project_id = ui.publish_project_id, media_files = std::move(media_files)](
                    const PublishWorkerContext& context) {
                    return upload_publish_media_request(context, project_id, media_files);
                });
        }
    }
    ImGui::EndDisabled();
    card_end();
    ImGui::Spacing();

    card_begin("##publish_version");
    ImGui::TextUnformatted("Version Artifact");
    ImGui::TextColored(k.muted, "The active profile is exported as a verified .mrpack artifact.");
    ImGui::BeginDisabled(form_locked);
    const char* version_button_label = ui.publish_retry_action == kPublishCreateVersionAction
        ? "Retry Create and Publish" : "Create and Publish Version";
    if (primary_button(publish_request_working && auth_snapshot.action == kPublishCreateVersionAction
                           ? "Publishing Version..." : version_button_label,
                       ImVec2(ui_px(220.0f), ui_px(32.0f)),
                       publish_request_working && auth_snapshot.action == kPublishCreateVersionAction,
                       form_locked)) {
        if (st.active_instance_dir.empty()) {
            set_publish_local_error(st, ui, "No profile", "Select a profile to export.");
        } else if (!publish_version_is_safe_path_segment(ui.publish_version)) {
            set_publish_local_error(st, ui, "Invalid version",
                                    "Use 1–80 letters, numbers, dots, hyphens, underscores, or plus signs.");
        } else {
            const PublishAsyncScope scope{current_user.id, session_generation, ui.publish_project_id};
            start_publish_request(
                st, ui, kPublishCreateVersionAction, scope,
                [profile_directory = st.active_instance_dir, project_id = ui.publish_project_id,
                 version = ui.publish_version, changelog = ui.publish_changelog](
                    const PublishWorkerContext& context) {
                    return create_and_publish_version_request(context, profile_directory, project_id,
                                                              version, changelog);
                });
        }
    }
    const char* review_button_label = ui.publish_retry_action == kPublishSubmitReviewAction
        ? "Retry Staff Review Submission" : "Submit Project for Staff Review";
    if (primary_button(publish_request_working && auth_snapshot.action == kPublishSubmitReviewAction
                           ? "Submitting for Review..." : review_button_label,
                       ImVec2(ui_px(240.0f), ui_px(32.0f)),
                       publish_request_working && auth_snapshot.action == kPublishSubmitReviewAction,
                       form_locked)) {
        const PublishAsyncScope scope{current_user.id, session_generation, ui.publish_project_id};
        start_publish_request(
            st, ui, kPublishSubmitReviewAction, scope,
            [project_id = ui.publish_project_id](const PublishWorkerContext& context) {
                return submit_publish_review_request(context, project_id);
            });
    }
    ImGui::EndDisabled();
    card_end();
}

void draw_mod_manager_page(UiState& st) {
    if (st.fixture_mode) {
        draw_fixture_mod_manager_page(st);
        return;
    }

    auto& mod_ui = get_mod_manager_ui_state();
    // Poll once for every Mod Manager tab.  A request can finish after the
    // user navigates away from Browse/Updates, and its UI-thread completion
    // must not be stranded until they happen to return to that tab.
    consume_mod_publish_result(st, mod_ui);
    poll_mod_operation(st, mod_ui);
    draw_mod_uninstall_confirmation(st, mod_ui);
    
    page_title("Mod Manager", "Advanced mod management with versioning and dependency resolution");
    
    // Mod manager tabs
    if (ImGui::BeginTabBar("##mod_manager_tabs")) {
    
    if (ImGui::BeginTabItem("Installed", nullptr,
                            st.fixture_mode && mod_ui.current_tab == 0
                                ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None)) {
        mod_ui.current_tab = 0;
        ImGui::EndTabItem();
    }
    
    if (ImGui::BeginTabItem("Browse", nullptr,
                            st.fixture_mode && mod_ui.current_tab == 1
                                ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None)) {
        mod_ui.current_tab = 1;
        ImGui::EndTabItem();
    }
    
    if (ImGui::BeginTabItem("Updates", nullptr,
                            st.fixture_mode && mod_ui.current_tab == 2
                                ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None)) {
        mod_ui.current_tab = 2;
        ImGui::EndTabItem();
    }
    
    if (ImGui::BeginTabItem("Dependencies", nullptr,
                            st.fixture_mode && mod_ui.current_tab == 3
                                ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None)) {
        mod_ui.current_tab = 3;
        ImGui::EndTabItem();
    }
    
    if (ImGui::BeginTabItem("Settings", nullptr,
                            st.fixture_mode && mod_ui.current_tab == 4
                                ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None)) {
        mod_ui.current_tab = 4;
        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Publish", nullptr,
                            st.fixture_mode && mod_ui.current_tab == 5
                                ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None)) {
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
