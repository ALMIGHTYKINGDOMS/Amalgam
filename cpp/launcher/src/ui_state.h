#pragma once

#include "auth.h"
#include "config.h"
#include "instances.h"
#include "java.h"
#include "model.h"
#include "mods.h"
#include "readiness.h"
#include "server_types.h"
#include "ui_model.h"
#include "ui_async_request.h"
#include "account_passive_cache.h"
#include "updater.h"

#include "ai_core.h"
#include "diagnostics.h"

#include <windows.h>
#include <GL/gl.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace aml::ui {

struct AuthWizardState {
    int current_step = 0;
    std::string email;
    std::string username;
    std::string password;
    std::string confirm_password;
    std::string display_name;
    std::string verification_code;
    bool code_sent = false;
    int code_resend_seconds = 0;
    std::string microsoft_code;
    std::string microsoft_uri;
    std::string microsoft_user_code;
    bool microsoft_waiting = false;
    int microsoft_expires_in = 0;
    std::string error_message;
    std::string success_message;
    bool working = false;
    bool accept_terms = false;
    bool accept_privacy = false;
    bool receive_newsletter = true;
};

struct UiState {
    config::Config* cfg = nullptr;
    HICON window_icon = nullptr;
    std::vector<model::ManifestEntry> versions;
    std::mutex version_mu;
    std::string search;
    std::string selected;
    std::string selected_type;
    std::atomic_bool fetching{true};
    // ImGui clock time of the last message the user acted with (mouse, keys,
    // resize). The render loop keeps drawing at the display rate for a moment
    // after one, so ImGui's own tweens (popup fades, tab scrolls, drags) are
    // not stepped at the idle tick rate.
    float last_activity_time = -1000.0f;

    std::string ui_base;
    std::string ui_assets;
    std::string ui_java_cache;
    std::string ui_username;
    std::string ui_server;
    std::string server_name;
    int selected_server = -1;
    int server_delete_pending = -1;
    int server_type_filter = -1;
    int server_edit_type = 0;
    std::string server_edit_profile;
    std::string pending_server;
    std::string ui_jvm;
    bool focus_global_search = false;
    auth::Account account;
    std::atomic_bool auth_checked{false};
    std::atomic_bool auth_working{false};
    // Each device-code login owns one generation. Destructive credential
    // actions advance it so an older worker can never persist a stale account
    // after the user has disconnected or started a newer flow.
    std::atomic_uint64_t auth_operation_generation{0};
    std::string auth_status;
    bool login_wizard_open = false;
    bool microsoft_login_popup_open = false;
    int login_wizard_state = 0;
    std::string login_verification_uri;
    std::string login_user_code;
    std::string login_error;
    int login_expires_in = 0;
    AuthWizardState auth_wizard_state;
    std::mutex auth_mu;
    // State-changing Amalgam-account requests share a single joined lane.
    // This prevents sign-in, verification, recovery, and password mutations
    // from racing each other or blocking the render thread.
    AsyncUiRequestState auth_async_request;
    // Security, Activity, and Overview statistics keep account-scoped,
    // render-thread-owned cached values here. Remote Security/Activity reads
    // reuse the serialized auth lane; the local-only instance statistics scan
    // gets its own joined lane because it receives only render-thread copies.
    // Worker results are accepted only when this scope still matches.
    AccountPassiveCache account_passive_cache;
    AsyncUiRequestState account_passive_stats_async_request;

    std::deque<std::string> logs;
    std::mutex log_mu;
    // Log filtering is a render-time operation. Keep a revision so the
    // viewer only rebuilds its filtered copy when the source, filter, or
    // selected log channel actually changes.
    std::atomic_uint64_t log_revision{0};
    uint64_t log_cache_revision = 0;
    int log_cache_source = -1;
    std::string log_cache_filter;
    std::vector<std::string> log_cache_filtered;
    std::wstring session_log_path;
    bool profile_log_only = false;

    // ── Amalgam AI state (V10) ────────────────────────────────────────────
    std::string ai_input;            // chat input buffer
    std::string ai_reply;
    std::atomic_bool ai_chat_working{false};   // brain chat request in flight
    std::atomic_bool ai_vision_working{false}; // screen capture/inference in flight
    std::atomic_bool ai_art_working{false};    // art generation in flight
    std::atomic_uint64_t ai_request_id{0};     // invalidates stale workers on profile changes
    std::shared_ptr<std::atomic_bool> ai_chat_cancel_token;
    std::shared_ptr<std::atomic_bool> ai_vision_cancel_token;
    std::shared_ptr<std::atomic_bool> ai_art_cancel_token;
    std::mutex ai_mu;
    std::vector<aml::ai::Turn> ai_turns;      // loaded conversation (mirrored)
    std::string ai_status;           // "Generating...", "Ready", error text
    std::string ai_active_profile;   // profile id the chat is bound to
    int ai_selected_mode = 0;        // 0 Ask, 1 Build, 2 Agent, 3 Auto
    // Art generation state
    std::string ai_art_prompt;
    std::vector<uint8_t> ai_art_image;
    std::string ai_art_status;
    // Vision state
    std::string ai_vision_text;
    std::string ai_vision_status;
    // Diagnostics state
    std::vector<aml::diagnostics::CheckResult> ui_diagnostics_results;
    bool ui_diagnostics_ran = false;
    // auto-scroll to bottom of chat when new content arrives
    bool ai_scroll_bottom = true;

    std::vector<java::Install> javas;
    std::atomic_bool java_scanned{false};
    std::mutex java_mu;
    std::atomic_bool java_installing{false};
    std::atomic_int java_install_progress{-1};
    int java_install_major = 0;
    bool java_install_success = false;
    std::string java_install_message;
    int java_selected_major = 0;
    std::wstring java_selected_home;

    std::atomic_bool running{false};
    std::atomic_bool shutting_down{false};
    std::atomic_bool pending_launch{false};
    std::string pending_id;
    std::wstring pending_instance_dir;
    std::wstring active_instance_dir;
    std::mutex workers_mu;
    std::vector<std::thread> workers;

    // Essentials and Social mutate account-backed state through joined
    // launcher workers.  These hand-off states keep the render thread free of
    // HTTP/disk work and reject stale worker completions by generation.
    AsyncUiRequestState essentials_async_request;
    AsyncUiRequestState social_async_request;

    int active_tab = 0;
    int sidebar_item = 0;
    bool sidebar_collapsed = false;
    bool sidebar_compact = false;
    int provider_tab = 0;

    std::string mod_query;
    std::string mod_loader;
    std::string mod_version;
    std::string instance_name;
    std::string instance_search;
    std::string instance_version;
    std::string instance_loader = "fabric";
    std::string java_filter;
    std::string bedrock_profile_filter;
    std::string bedrock_world_filter;
    std::string bedrock_backup_filter;
    std::string backup_filter;
    std::string log_filter;
    bool backup_wizard_open = false;
    bool bedrock_profile_wizard_open = false;
    int instance_sort = 0;
    int instance_filter_idx = 0;
    int library_section = 0;
    int settings_section = 0;
    bool admin_unlocked = false;
    uint64_t admin_unlock_until_ms = 0;
    uint64_t admin_retry_after_ms = 0;
    int admin_failed_attempts = 0;
    int admin_section = 0;
    std::string admin_password;
    std::string admin_password_confirm;
    std::string admin_status;
    std::string new_group_name;
    std::string group_target;
    std::string rename_group_target;
    std::string delete_group_target;
    std::string delete_target;
    // High-impact profile changes share one modal so every active entry point
    // states its consequence before files are overwritten or deleted.
    // 1=restore latest, 2=delete restore point, 3=restore game options.
    bool profile_data_confirm_open = false;
    int profile_data_confirm_action = 0;
    instances::Instance profile_data_confirm_instance;
    std::wstring profile_data_confirm_path;
    std::string profile_data_confirm_label;
    std::string profile_data_confirm_error;
    int version_change_kind = 0;   // 1 = minecraft version, 2 = loader
    bool version_change_backup = true;
    // Kept with the confirmation state so a failed pre-change restore point
    // remains visible and retryable instead of silently advancing the wizard.
    std::string version_change_backup_error;
    bool wizard_open = false;
    int wizard_step = 0;
    int wizard_source = 0;
    std::string wizard_name;
    std::string wizard_project;
    std::string wizard_version;
    std::string wizard_loader = "fabric";
    std::string wizard_java;
    std::string wizard_prompt;
    std::string wizard_archive;
    int wizard_memory = 0;
    std::string wizard_performance_profile = "auto";
    int wizard_preset = -1;
    std::atomic_bool wizard_resolving{false};
    std::string wizard_resolve_error;
    std::mutex wizard_resolve_mu;
    std::vector<std::string> wizard_resolved_loaders;
    std::vector<std::string> wizard_resolved_versions;
    std::string wizard_resolved_title;
    std::string wizard_resolved_icon;
    std::string wizard_last_project;
    std::vector<instances::Instance> instance_list;
    bool instances_loaded = false;
    instances::Instance selected_instance;
    bool instance_detail_open = false;
    int instance_detail_tab = 0;
    int browse_category = 0;
    int browse_source = 0;
    int browse_sort = 0;
    int browse_page = 0;
    bool browse_initial_request_sent = false;
    std::chrono::steady_clock::time_point browse_search_debounce;
    std::string browse_last_query;
    std::vector<std::string> search_history;
    bool browse_search_focused = false;
    bool browse_grid_view = false;
    int discover_sub_tab = 0;
    int project_file_selected = -1;
    int mod_facet = 0;
    int content_filter = 0;
    std::string content_search;
    std::string content_status;
    std::string screenshot_search;
    std::vector<mods::SearchResult> mod_results;
    std::mutex mod_mu;
    mods::SearchResult project_detail;
    mods::ModInfo project_info;
    std::mutex project_mu;
    bool project_detail_open = false;
    int project_detail_tab = 0;
    int project_return_tab = 1;
    int project_return_sidebar_item = 2;
    int project_return_provider_tab = 1;
    int project_return_browse_category = 0;
    std::atomic_bool project_loading{false};
    std::string project_error;
    std::atomic_bool project_translation_working{false};
    std::atomic_uint64_t project_translation_request{0};
    std::mutex project_translation_mu;
    std::string project_translation_text;
    std::string project_translation_error;
    bool project_show_original_text = false;
    bool install_confirm_open = false;
    bool install_confirm_modpack = false;
    mods::SearchResult install_confirm_project;
    instances::Instance install_confirm_target;
    std::vector<mods::DepInfo> install_confirm_dependencies;
    std::vector<std::string> install_confirm_conflict_files;
    int install_confirm_conflicts = 0;
    std::mutex async_text_mu;
    std::atomic_bool pack_update_checking{false};
    mods::UpdateInfo pack_update;
    std::string pack_update_error;
    std::wstring pack_update_directory;
    std::mutex pack_update_mu;
    bool pack_update_confirm_open = false;
    bool pack_update_confirm_copy = false;
    std::string pack_update_confirm_version;
    instances::Instance pack_update_confirm_target;
    std::atomic_bool owned_update_checking{false};
    std::vector<mods::UpdateEntry> owned_updates;
    std::set<std::string> owned_update_selected;
    std::string owned_update_error;
    std::wstring owned_update_directory;
    std::mutex owned_update_mu;
    std::string selected_source = "modrinth";
    std::vector<mods::SearchResult> home_packs;
    std::vector<mods::SearchResult> home_mods;
    std::string home_error;
    std::atomic_bool home_fetching{true};
    int home_pack_selected = 0;

    struct Image {
        std::vector<uint8_t> rgba;
        int width = 0;
        int height = 0;
        GLuint texture = 0;
        bool loading = false;
        bool failed = false;
        // LRU tracking: bumped whenever the image is requested/uploaded so the
        // cache can evict stale entries instead of growing without bound while
        // browsing large catalogs.
        uint64_t last_used = 0;
    };
    std::mutex image_mu;
    std::unordered_map<std::string, std::shared_ptr<Image>> images;
    std::atomic_bool mod_fetching{false};
    std::atomic_bool mod_installing{false};
    std::atomic_bool curseforge_testing{false};
    std::string mod_status;
    int modrinth_result_count = 0;
    int curseforge_result_count = 0;
    std::mutex mod_status_mu;
    int mod_selected = -1;
    std::vector<std::string> mod_install_log;

    // Readiness detail behind the Home summary; nothing gates Play on these.
    struct LaunchCheck {
        std::string label;
        std::string detail;
        bool passed = false;
    };

    struct DownloadJob {
        int id = 0;
        std::string kind;
        std::string label;
        std::string detail;
        std::string phase;
        std::string current_item;
        std::string target_profile;
        std::string provider;
        float progress = 0.0f;
        uint64_t bytes_done = 0;
        uint64_t bytes_total = 0;
        double bytes_per_second = 0.0;
        double eta_seconds = -1.0;
        uint64_t created_at = 0;
        uint64_t started_at = 0;
        uint64_t finished_at = 0;
        uint64_t last_sample_bytes = 0;
        uint64_t last_sample_ms = 0;
        bool active = true;
        bool paused = false;
        bool completed = false;
        bool failed = false;
        bool cancelled = false;
        bool resumable = false;
        bool cancel_requested = false;
        bool queued = false;
        bool exclusive = false;
        std::vector<std::string> history;
        std::string retry_action;
        std::string retry_payload;
        std::string error;
    };
    std::mutex jobs_mu;
    std::vector<DownloadJob> jobs;
    int next_job_id = 1;
    uint64_t next_notice_id = 1;
    std::deque<ui_model::Notice> notices;
    std::mutex notice_mu;
    bool notice_center_open = false;
    int operation_filter = 0;
    int expanded_operation_id = 0;
    std::atomic_uint64_t next_request_id{1};
    ui_model::ScreenState<std::vector<mods::SearchResult>> mod_screen;
    ui_model::ScreenState<mods::ModInfo> project_screen;
    ui_model::ScreenState<std::vector<mods::SearchResult>> home_screen;
    bool settings_dirty = false;
    std::string settings_status;
    bool feedback_open = false;
    int feedback_category = 0;
    int feedback_rating = 0;
    std::string feedback_message;
    // The authenticated beta-feedback RPC runs on the shared account worker
    // lane. Only this render-thread state controls the modal presentation.
    bool feedback_submitting = false;
    std::string feedback_submission_account_id;
    uint64_t feedback_submission_generation = 0;
    bool local_feedback_open = false;
    int local_feedback_category = 0;
    std::string local_feedback_message;
    readiness::Report readiness_report;
    std::mutex readiness_mu;
    std::atomic_bool readiness_testing{false};
    bool readiness_checked = false;
    // Cached Home "Updates" panel summary. Recomputing it every frame re-runs
    // per-profile health checks and evaluate_launch, which probes every
    // installed JDK with a java.exe -version subprocess. Computing that
    // inline on the render thread froze page navigation for seconds whenever
    // the summary went stale, so a background worker recomputes it and the
    // Home page renders the last cached values in the meantime.
    struct HomeReadinessSummary {
        std::mutex mu;
        std::atomic_bool dirty{true};
        std::atomic_bool computing{false};
        std::atomic_uint64_t generation{0};
        int ready_profiles = 0;      // guarded by mu
        int attention_profiles = 0;  // guarded by mu
        std::string first_issue;     // guarded by mu
    };
    HomeReadinessSummary home_readiness;
    // Instance detail page caches. list_content scans five content directories
    // and the storage bars recursively size the whole instance (a modded world
    // can be tens of thousands of files); both used to run every rendered
    // frame. Cache per instance and refresh only after a short TTL.
    struct InstanceDetailCache {
        std::mutex mu;
        std::wstring directory;
        uint64_t generation = 0;
        uint64_t content_at_ms = 0;
        std::vector<instances::ContentEntry> content;
        bool content_scan_pending = false;
        // A fresh cache is intentionally empty while its worker is reading the
        // profile. Keep that distinct from a completed scan that found no
        // content, and retain a filesystem error instead of rendering it as an
        // empty profile.
        bool content_initial_scan_complete = false;
        std::string content_error;
        uint64_t counts_at_ms = 0;
        int world_count = 0;
        int screenshot_count = 0;
        std::vector<std::wstring> world_directories;
        std::vector<std::wstring> screenshot_paths;
        bool counts_scan_pending = false;
        uint64_t storage_at_ms = 0;
        bool storage_scan_pending = false;
        uint64_t storage_mods = 0;
        uint64_t storage_worlds = 0;
        uint64_t storage_shots = 0;
        uint64_t storage_total = 0;
        std::unordered_map<std::wstring, uint64_t> world_sizes;
    };
    InstanceDetailCache detail_cache;

    // Library and screenshot galleries are backed by user folders. Their
    // directory enumeration is cached so switching tabs does not turn every
    // frame into a filesystem scan.
    struct WorldIndexEntry {
        std::wstring instance_directory;
        std::wstring directory;
        std::string name;
    };
    std::vector<WorldIndexEntry> library_worlds_cache;
    uint64_t library_worlds_cache_at_ms = 0;
    bool library_worlds_scan_pending = false;
    uint64_t library_worlds_scan_generation = 0;

    struct ScreenshotIndexEntry {
        std::wstring path;
    };
    std::vector<ScreenshotIndexEntry> screenshots_cache;
    std::wstring screenshots_cache_instance_directory;
    uint64_t screenshots_cache_at_ms = 0;
    bool screenshots_scan_pending = false;
    uint64_t screenshots_scan_generation = 0;
    std::mutex file_index_mu;

    // Quick search uses a small in-memory index. The world portion is rebuilt
    // on a throttle instead of enumerating every profile's saves directory
    // for every typed character.
    struct QuickSearchEntry {
        std::string haystack;
        std::string label;
    };
    std::vector<QuickSearchEntry> quick_search_index;
    uint64_t quick_search_index_at_ms = 0;

    std::string pack_prompt;
    std::string pack_summary;
    std::vector<mods::Match> pack_plan;
    std::mutex pack_mu;
    std::atomic_bool pack_building{false};
    std::string pack_install_log;

    std::wstring exe_dir;
    HWND hwnd = nullptr;

    // Launcher self-update status (checked on a worker thread; the UI only
    // reads these atomics/strings under the state's normal threading rules).
    updater::UpdateStatus update_status;
    std::atomic_bool update_check_started{false};

    // Release UX.
    bool known_issues_open = false;      // packaged Known Issues dialog
    bool recovery_dialog_open = false;   // previous session ended abnormally
    bool safe_mode = false;              // started in safe mode (no auto network)

    std::string bedrock_status;
    std::string bedrock_addon;
    bool bedrock_addon_import_open = false;
    bool bedrock_scanned = false;

    int server_filter_stage = 0;
    int server_filter_software = 0;
    int server_filter_edition = 0;
    // One-click cloud deployment from the Library: profile id to pre-select
    // in the cloud deployment wizard, and the flag that opens the wizard.
    std::string server_cloud_deploy_profile;
    bool server_cloud_wizard_open = false;
    bool server_create_open = false;
    server::ServerCreateOptions server_create_opts;
    std::vector<server::ServerConfig> servers;
    std::string server_console_filter;
    std::vector<server::ServerConsoleEntry> server_console_log;
    std::vector<server::ServerPlayer> server_players;
    server::ServerMetrics server_metrics;

    bool diagnostics_open = false;
    bool downloads_open = false;
    float log_h = 0.0f;
    bool fixture_mode = false;
    // Release visual-QA state.  Populated only by `--ui-snapshot`; normal
    // player sessions neither read nor persist these fields.
    std::wstring fixture_root;
    std::string fixture_case;
    int fixture_scroll_position = 0;
    // Fixture-only transient state. These flags are populated exclusively by
    // named --ui-snapshot routes so screenshot evidence can prove popup and
    // combo geometry without synthesizing clicks or touching live settings.
    bool fixture_theme_selector_open = false;
    // The page scroll range is known only after a fully laid-out frame. Keep
    // that fixture-only value so the next frame can set the requested capture
    // position before the page body is drawn.
    float fixture_scroll_host_max_y = 0.0f;
    std::wstring capture_path;
    int capture_after_frames = 0;
    int rendered_frames = 0;
    bool capture_failed = false;
    bool startup_metrics_logged = false;
    bool shell_initialized = false;

    // Sidebar account dropdown
    bool account_dropdown_open = false;
    bool login_popup_open = false;
    bool register_popup_open = false;
    bool password_reset_popup_open = false;
    bool ms_connect_popup_open = false;
    // A user may dismiss the account prompt and continue browsing.  Explicit
    // sign-in/create-account actions clear this flag before reopening it.
    bool auth_prompt_dismissed = false;
};

}  // namespace aml::ui
