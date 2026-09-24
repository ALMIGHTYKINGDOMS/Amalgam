#include "ui.h"
#include "ui_internal.h"
#include "ui_model.h"
#include "auth_wizard.h"
#include "account_passive_cache.h"
#include "account_manager.h"
#include "supabase.h"
#include "auth.h"
#include "net.h"
#include "instances.h"
#include "entitlements.h"
#include "online_config.h"

#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <exception>
#include <functional>
#include <limits>
#include <string_view>
#include <thread>
#include <utility>

namespace aml::ui {

// ---------------------------------------------------------------------------
// Account UI State
// ---------------------------------------------------------------------------

enum class AccountActionKind {
    None,
    SignOutThisDevice,
    RemoveRememberedSession,
    RemoveAllRememberedAccounts,
    DisconnectMinecraft,
};

// A confirmation never retains an account object, token, pointer, or mutable
// session reference. The copied identifiers are revalidated immediately
// before the action runs so a delayed confirmation cannot affect a different
// account after a switch or sign-in change.
struct AccountPendingAction {
    AccountActionKind kind = AccountActionKind::None;
    std::string target_label;
    std::string expected_amalgam_user_id;
    std::string expected_current_session_id;
    std::string expected_session_id;
    std::string expected_session_email;
    std::vector<std::string> expected_local_session_ids;
    std::string expected_minecraft_uuid;
    std::string expected_minecraft_username;
    // This is deliberately separate from the Minecraft device-code generation
    // below: Amalgam provider work must also reject a same-user session reset.
    uint64_t expected_supabase_session_generation = 0;
    uint64_t expected_auth_generation = 0;
    std::string typed_confirmation;
    std::string error;
    bool submitting = false;
};

struct AccountUIState {
    int current_tab = 0; // 0=overview, 1=profile, 2=settings, 3=security, 4=sessions, 5=activity

    // Profile editing
    account::AccountProfile editing_profile;
    bool profile_editing = false;
    std::string profile_error;
    std::string profile_success;

    // Settings
    std::string theme;
    std::string language;
    bool receive_newsletter = true;
    bool enable_beta_features = false;
    bool social_notifications = true;
    bool social_show_offline = true;
    bool social_auto_accept = false;
    bool settings_initialized = false;
    std::string settings_profile_identity;

    // Security
    std::string current_password;
    std::string new_password;
    std::string confirm_password;
    std::string security_error;
    std::string security_success;
    bool changing_password = false;
    std::string password_request_user_id;
    uint64_t password_request_session_generation = 0;
    // A submitted sign-out keeps its confirmation alive until the serialized
    // account worker reports the remote outcome, then closes the modal on the
    // next render frame without leaving stale dialog state behind.
    bool action_popup_close_requested = false;
    bool session_switch_submitting = false;
    std::string session_switch_target_id;
    std::string session_switch_target_email;
    std::string session_switch_previous_id;

    AccountPendingAction pending_action;
    // The UI owns transient password, profile, and confirmation fields. Track
    // the authenticated Amalgam identity so none of those fields survive a
    // switch to a different account in the same process.
    std::string bound_amalgam_user_id;

    // Activity
    int activity_page = 0;
    int activity_per_page = 20;
};

static AccountUIState& get_account_ui_state(UiState& /*st*/) {
    static AccountUIState state;
    return state;
}

static bool is_fixture_account_action_case(const std::string& fixture_case) {
    return fixture_case == "dialog-amalgam-signout-this-device" ||
           fixture_case == "dialog-remove-local-remembered-session" ||
           fixture_case == "dialog-remove-local-accounts" ||
           fixture_case == "dialog-remove-local-accounts-ready" ||
           fixture_case == "dialog-minecraft-disconnect" ||
           fixture_case == "dialog-account-action-state-changed" ||
           fixture_case == "dialog-account-action-local-error";
}

static void draw_fixture_account_action_dialog(UiState& st);

static void wipe_string(std::string& value) {
    std::fill(value.begin(), value.end(), '\0');
    value.clear();
}

static void reset_sensitive_account_ui_state(AccountUIState& state) {
    state.editing_profile = {};
    state.profile_editing = false;
    wipe_string(state.profile_error);
    wipe_string(state.profile_success);
    wipe_string(state.theme);
    wipe_string(state.language);
    state.settings_initialized = false;
    wipe_string(state.settings_profile_identity);
    wipe_string(state.current_password);
    wipe_string(state.new_password);
    wipe_string(state.confirm_password);
    wipe_string(state.security_error);
    wipe_string(state.security_success);
    state.changing_password = false;
    wipe_string(state.password_request_user_id);
    state.password_request_session_generation = 0;
    state.pending_action = {};
    state.session_switch_submitting = false;
    wipe_string(state.session_switch_target_id);
    wipe_string(state.session_switch_target_email);
    wipe_string(state.session_switch_previous_id);
    state.activity_page = 0;
}

constexpr const char* kAccountPasswordChangeAction = "account-password-change";
constexpr const char* kAccountSignOutAction = "account-sign-out-this-device";
constexpr const char* kAccountSwitchSessionAction = "account-switch-session";
constexpr const char* kAccountPassiveStatsAction = "account-passive-stats";
constexpr const char* kAccountPassiveSecurityAction = "account-passive-security";
constexpr const char* kAccountPassiveActivityAction = "account-passive-activity";
constexpr const char* kAccountActionPopup = "Confirm account action##account_confirm_action";
constexpr uint64_t kAccountPassiveStaleAfterMs = 5ULL * 60ULL * 1000ULL;

static uint64_t account_passive_now_ms() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

static bool is_account_passive_action(std::string_view action) {
    return action == kAccountPassiveStatsAction ||
           action == kAccountPassiveSecurityAction ||
           action == kAccountPassiveActivityAction;
}

// `working` alone is not enough here: a completed result remains in its lane
// until its owner consumes it on the render thread. Starting another request
// in that interval would overwrite it before the appropriate owner sees it.
static bool async_ui_request_lane_reserved(const AsyncUiRequestState& lane) {
    const auto snapshot = snapshot_async_ui_request(lane);
    return snapshot.working || (snapshot.has_result && !snapshot.result_delivered);
}

// Provider-backed passive reads intentionally share the mutation lane. This
// keeps the current session stable while an account mutation is in progress.
static bool account_auth_lane_reserved(const UiState& st) {
    return async_ui_request_lane_reserved(st.auth_async_request);
}

// Overview statistics only inspect render-thread copies and local instance
// files, so they may use an independent joined lane without racing provider
// state or making account actions wait for slow local disk IO.
static bool account_passive_stats_lane_reserved(const UiState& st) {
    return async_ui_request_lane_reserved(st.account_passive_stats_async_request);
}

static bool account_passive_scopes_match(const AccountPassiveScope& lhs,
                                         const AccountPassiveScope& rhs) {
    return account_passive_scope_matches(lhs, rhs.account_identity,
                                         rhs.session_generation);
}

static bool account_passive_cache_is_stale(const AccountPassiveLoadState& state) {
    if (!state.has_value) return false;
    if (state.stale) return true;
    const uint64_t now = account_passive_now_ms();
    return state.loaded_at_ms > 0 && now > state.loaded_at_ms &&
           now - state.loaded_at_ms >= kAccountPassiveStaleAfterMs;
}

static void invalidate_account_passive_request(UiState& st) {
    const auto auth_snapshot = snapshot_async_ui_request(st.auth_async_request);
    if ((auth_snapshot.action == kAccountPassiveSecurityAction ||
         auth_snapshot.action == kAccountPassiveActivityAction) &&
        (auth_snapshot.working || auth_snapshot.has_result)) {
        invalidate_async_ui_request(st.auth_async_request);
    }

    const auto stats_snapshot =
        snapshot_async_ui_request(st.account_passive_stats_async_request);
    if (stats_snapshot.action == kAccountPassiveStatsAction &&
        (stats_snapshot.working || stats_snapshot.has_result)) {
        invalidate_async_ui_request(st.account_passive_stats_async_request);
    }
}

static void clear_account_passive_cache(UiState& st) {
    auto& cache = st.account_passive_cache;
    if (cache.scope.account_identity.empty() && !cache.stats_state.attempted &&
        !cache.security_state.attempted && !cache.activity_state.attempted) {
        return;
    }
    invalidate_account_passive_request(st);
    cache.clear();
}

static void bind_account_passive_cache(UiState& st,
                                       const std::string& account_identity) {
    auto& cache = st.account_passive_cache;
    const uint64_t session_generation =
        aml::supabase::SupabaseManager::instance().session_generation();
    if (cache.bind(account_identity, session_generation)) {
        // A prior response may have already arrived while the selected account
        // changed. Drop it before it can be consumed by this account surface.
        invalidate_account_passive_request(st);
    }
}

static std::string account_passive_identity(const std::string& amalgam_user_id,
                                            const std::string& local_session_id) {
    if (!amalgam_user_id.empty()) return "amalgam:" + amalgam_user_id;
    if (!local_session_id.empty()) return "local-session:" + local_session_id;
    return {};
}

static bool start_account_remote_passive_request(
    UiState& st, const char* action, AccountPassiveLoadState& state,
    std::function<AsyncUiRequestResult()> work) {
    auto& cache = st.account_passive_cache;
    if (state.loading || cache.scope.account_identity.empty()) return false;

    // The existing auth lane is intentionally reused for provider-backed
    // passive reads. It serializes them with sign-in, verification, password,
    // and profile-sensitive account work, so no worker can use a session while
    // an account mutation replaces it.
    if (account_auth_lane_reserved(st) ||
        !start_auth_async_request(st, action, std::move(work))) return false;

    state.attempted = true;
    state.loading = true;
    state.empty = false;
    state.error.clear();
    state.warning.clear();
    state.stale = state.has_value;
    state.request_scope = cache.scope;
    return true;
}

static bool start_account_stats_passive_request(
    UiState& st, AccountPassiveLoadState& state,
    std::function<AsyncUiRequestResult()> work) {
    auto& cache = st.account_passive_cache;
    auto& lane = st.account_passive_stats_async_request;
    if (state.loading || cache.scope.account_identity.empty() ||
        account_passive_stats_lane_reserved(st)) {
        return false;
    }

    uint64_t generation = 0;
    if (!begin_async_ui_request(lane, kAccountPassiveStatsAction, &generation)) {
        return false;
    }

    state.attempted = true;
    state.loading = true;
    state.empty = false;
    state.error.clear();
    state.warning.clear();
    state.stale = state.has_value;
    state.request_scope = cache.scope;

    // UiState owns this worker and joins it at shutdown. The task captures
    // only values copied on the render thread; it cannot read or mutate the
    // selected account, session, form, or ImGui state.
    spawn_worker(st, std::thread([&st, generation, work = std::move(work)]() mutable {
        AsyncUiRequestResult result;
        try {
            result = work();
        } catch (const std::exception&) {
            result.title = "Account statistics unavailable";
            result.detail = "Local account statistics could not be refreshed. Please try again.";
        } catch (...) {
            result.title = "Account statistics unavailable";
            result.detail = "Local account statistics could not be refreshed. Please try again.";
        }
        if (!st.shutting_down.load()) {
            complete_async_ui_request(st.account_passive_stats_async_request,
                                      kAccountPassiveStatsAction, generation,
                                      std::move(result));
        }
    }));
    return true;
}

static AccountPassiveLoadState* account_passive_state_for_action(
    AccountPassiveCache& cache, std::string_view action) {
    if (action == kAccountPassiveStatsAction) return &cache.stats_state;
    if (action == kAccountPassiveSecurityAction) return &cache.security_state;
    if (action == kAccountPassiveActivityAction) return &cache.activity_state;
    return nullptr;
}

static int64_t parse_account_passive_i64(const std::string& value) {
    if (value.empty()) return 0;
    try {
        return std::stoll(value);
    } catch (...) {
        return 0;
    }
}

static int account_passive_non_negative_int(const std::string& value) {
    const int64_t parsed = parse_account_passive_i64(value);
    if (parsed <= 0) return 0;
    constexpr int64_t kMaxInt = static_cast<int64_t>(std::numeric_limits<int>::max());
    return static_cast<int>(std::min(parsed, kMaxInt));
}

static void reconcile_account_passive_request(UiState& st) {
    auto& cache = st.account_passive_cache;
    const auto auth_snapshot = snapshot_async_ui_request(st.auth_async_request);
    const auto stats_snapshot =
        snapshot_async_ui_request(st.account_passive_stats_async_request);
    const auto reconcile = [](AccountPassiveLoadState& state, const char* action,
                              const AsyncUiRequestSnapshot& snapshot) {
        if (!state.loading || snapshot.action == action) return;
        // A different account action can legitimately replace an undelivered
        // passive result while this page is not visible. Do not leave a
        // permanent spinner behind; preserve any prior value as stale and let
        // the next page visit perform one fresh, deliberate refresh.
        state.loading = false;
        state.attempted = false;
        state.stale = state.has_value;
        if (!state.has_value) {
            state.error = "This account refresh was superseded by another account action. Please retry.";
        }
    };
    reconcile(cache.stats_state, kAccountPassiveStatsAction, stats_snapshot);
    reconcile(cache.security_state, kAccountPassiveSecurityAction, auth_snapshot);
    reconcile(cache.activity_state, kAccountPassiveActivityAction, auth_snapshot);
}

static void consume_account_passive_result(UiState& st, AsyncUiRequestState& lane) {
    const auto snapshot = snapshot_async_ui_request(lane);
    if (!is_account_passive_action(snapshot.action) || !snapshot.has_result) return;

    AsyncUiRequestSnapshot completed;
    if (!take_async_ui_request_result(lane, &completed)) return;

    auto& cache = st.account_passive_cache;
    AccountPassiveLoadState* state = account_passive_state_for_action(cache, completed.action);
    if (!state || !account_passive_scopes_match(cache.scope, state->request_scope)) {
        // The identity guard intentionally discards all data after a sign-out
        // or account switch, including a response that finished just before
        // the renderer observed the new identity.
        return;
    }

    state->loading = false;
    if (!completed.result.success) {
        state->error = completed.result.detail.empty()
            ? "This account information could not be refreshed. Please try again."
            : humanize_error(completed.result.detail);
        state->warning.clear();
        state->stale = state->has_value;
        return;
    }

    state->error.clear();
    state->warning = completed.result.warning ? completed.result.detail : std::string();
    state->loaded_at_ms = account_passive_now_ms();
    state->stale = false;

    if (completed.action == kAccountPassiveStatsAction) {
        if (completed.result.items.size() != 2) {
            state->error = "The local statistics response could not be read. Please retry.";
            state->warning.clear();
            state->stale = state->has_value;
            return;
        }
        cache.stats.total_sessions = static_cast<int>(completed.result.number_a);
        cache.stats.account_created_at = parse_account_passive_i64(completed.result.payload_a);
        cache.stats.last_login_at = parse_account_passive_i64(completed.result.payload_b);
        cache.stats.current_session_active = completed.result.payload_c == "active";
        cache.stats.total_instances =
            account_passive_non_negative_int(completed.result.items[0]);
        cache.stats.total_modpacks =
            account_passive_non_negative_int(completed.result.items[1]);
        state->has_value = true;
        state->empty = false;
        return;
    }

    if (completed.action == kAccountPassiveSecurityAction) {
        cache.security.two_factor_enabled = completed.result.number_a != 0;
        cache.security.two_factor_method = completed.result.payload_a;
        state->has_value = true;
        state->empty = false;
        return;
    }

    if (completed.result.items.size() % 4 != 0) {
        state->error = "The account activity response could not be read. Please retry.";
        state->warning.clear();
        state->stale = state->has_value;
        return;
    }

    std::vector<AccountPassiveActivity> activity;
    activity.reserve(completed.result.items.size() / 4);
    for (size_t i = 0; i < completed.result.items.size(); i += 4) {
        AccountPassiveActivity item;
        item.id = completed.result.items[i];
        item.type = completed.result.items[i + 1];
        item.description = completed.result.items[i + 2];
        item.timestamp = parse_account_passive_i64(completed.result.items[i + 3]);
        if (item.id.empty()) item.id = "activity-" + std::to_string(i / 4);
        activity.push_back(std::move(item));
    }
    cache.activity = std::move(activity);
    state->has_value = true;
    state->empty = cache.activity.empty();
}

static void consume_account_passive_request(UiState& st) {
    // Statistics use their own local-only joined lane. Security and Activity
    // share the serialized provider lane with stateful account work.
    consume_account_passive_result(st, st.account_passive_stats_async_request);
    consume_account_passive_result(st, st.auth_async_request);
}

static void request_account_stats(UiState& st, bool force = false) {
    auto& cache = st.account_passive_cache;
    auto& state = cache.stats_state;
    if (state.loading || (!force && state.attempted)) return;

    const auto current_user = aml::supabase::SupabaseManager::instance().get_current_user();
    const auto sessions = aml::account::AccountManager::instance().get_all_sessions();
    const auto current_session = aml::account::AccountManager::instance().get_current_session();
    const int total_sessions = static_cast<int>(sessions.size());
    const int64_t account_created_at = current_user.created_at;
    const int64_t last_login_at = current_user.last_login_at;
    const bool current_session_active = !current_session.id.empty();
    const std::wstring instances_dir = net::get_local_app_data_path() + L"\\instances";

    start_account_stats_passive_request(
        st, state,
        [total_sessions, account_created_at, last_login_at, current_session_active,
         instances_dir]() {
            AsyncUiRequestResult result;
            std::string scan_error;
            const auto instances = aml::instances::scan(instances_dir, &scan_error);
            int modpacks = 0;
            for (const auto& instance : instances) {
                if (!instance.pack_source.empty() || !instance.pack_project.empty() ||
                    !instance.pack_version.empty()) {
                    ++modpacks;
                }
            }

            result.success = true;
            result.number_a = total_sessions;
            result.payload_a = std::to_string(account_created_at);
            result.payload_b = std::to_string(last_login_at);
            result.payload_c = current_session_active ? "active" : "none";
            result.items.push_back(std::to_string(instances.size()));
            result.items.push_back(std::to_string(modpacks));
            if (!scan_error.empty()) {
                result.warning = true;
                result.detail = "Some local profile statistics could not be refreshed: " + scan_error;
            }
            return result;
        });
}

static void request_account_security(UiState& st, const std::string& expected_user_id,
                                     bool force = false) {
    auto& cache = st.account_passive_cache;
    auto& state = cache.security_state;
    if (state.loading || (!force && state.attempted)) return;
    if (expected_user_id.empty()) {
        state.attempted = true;
        state.loading = false;
        state.error = "Security details require an active Amalgam account session.";
        state.warning.clear();
        return;
    }

    start_account_remote_passive_request(
        st, kAccountPassiveSecurityAction, state,
        [expected_user_id]() {
            AsyncUiRequestResult result;
            auto& supabase = aml::supabase::SupabaseManager::instance();
            if (supabase.get_current_user().id != expected_user_id) {
                result.title = "Security details not applied";
                result.detail = "Your active account changed before security details could be refreshed.";
                return result;
            }

            std::string error;
            const auto settings = aml::account::AccountManager::instance()
                                      .get_security_settings(&error);
            if (supabase.get_current_user().id != expected_user_id) {
                result.title = "Security details not applied";
                result.detail = "Your active account changed before security details could be refreshed.";
                return result;
            }
            if (!error.empty()) {
                result.title = "Security details unavailable";
                result.detail = error;
                return result;
            }

            result.success = true;
            result.number_a = settings.two_factor_enabled ? 1 : 0;
            result.payload_a = settings.two_factor_method;
            return result;
        });
}

static void request_account_activity(UiState& st, const std::string& expected_user_id,
                                     bool force = false) {
    auto& cache = st.account_passive_cache;
    auto& state = cache.activity_state;
    if (state.loading || (!force && state.attempted)) return;

    const auto sessions = aml::account::AccountManager::instance().get_all_sessions();
    const std::wstring instances_dir = net::get_local_app_data_path() + L"\\instances";
    start_account_remote_passive_request(
        st, kAccountPassiveActivityAction, state,
        [expected_user_id, sessions, instances_dir]() {
            AsyncUiRequestResult result;
            std::string refresh_error;
            bool used_local_fallback = false;
            const auto activity = aml::account::AccountManager::instance().get_recent_activity(
                expected_user_id, sessions, instances_dir, &refresh_error, &used_local_fallback);

            if (!refresh_error.empty() && activity.empty()) {
                result.title = "Activity unavailable";
                result.detail = refresh_error;
                return result;
            }

            result.success = true;
            result.warning = !refresh_error.empty() || used_local_fallback;
            if (!refresh_error.empty()) {
                result.detail =
                    "Showing activity stored on this Windows device because recent "
                    "activity could not be fully refreshed: " +
                    humanize_error(refresh_error);
            } else if (used_local_fallback) {
                result.detail = "Showing activity stored on this Windows device.";
            }
            result.items.reserve(activity.size() * 4);
            for (const auto& entry : activity) {
                result.items.push_back(entry.id);
                result.items.push_back(entry.type);
                result.items.push_back(entry.description);
                result.items.push_back(std::to_string(entry.timestamp));
            }
            return result;
        });
}

static void cancel_account_password_change(UiState& st) {
    invalidate_auth_async_request(st, kAccountPasswordChangeAction);
}

static void consume_account_password_change(UiState& st,
                                            AccountUIState& account_state) {
    AsyncUiRequestSnapshot completed;
    if (!take_auth_async_request_result(st, kAccountPasswordChangeAction, &completed)) {
        return;
    }

    if (!completed.result.success) {
        account_state.security_error = completed.result.detail.empty()
            ? "We could not change your password. Check the current password and try again."
            : humanize_error(completed.result.detail);
        account_state.security_success.clear();
        return;
    }

    // The provider call may finish after a local sign-out or account switch.
    // Never apply a success message or clear fields into a different account's
    // security form, even though the underlying request could not be forcibly
    // cancelled once it left this device.
    const auto current_user = aml::supabase::SupabaseManager::instance().get_current_user();
    if (account_state.password_request_user_id.empty() ||
        current_user.id != account_state.password_request_user_id ||
        aml::supabase::SupabaseManager::instance().session_generation() !=
            account_state.password_request_session_generation) {
        account_state.security_error =
            "Your account changed before the password update completed. The result was not applied to this form.";
        account_state.security_success.clear();
        wipe_string(account_state.current_password);
        wipe_string(account_state.new_password);
        wipe_string(account_state.confirm_password);
        account_state.changing_password = false;
        return;
    }

    account_state.security_success = "Password changed successfully.";
    account_state.security_error.clear();
    wipe_string(account_state.current_password);
    wipe_string(account_state.new_password);
    wipe_string(account_state.confirm_password);
    wipe_string(account_state.password_request_user_id);
    account_state.password_request_session_generation = 0;
    account_state.changing_password = false;
    // The provider may report a new password-change timestamp or revised
    // security posture. Keep the visible cache, but require an explicit
    // refresh before presenting it as current.
    st.account_passive_cache.mark_values_stale();
    push_notice(st, ui_model::NoticeLevel::Success, "Password changed",
                "Your Amalgam password was updated successfully.");
}

static std::vector<std::string> local_session_ids(
    const aml::account::AccountManager& account_manager) {
    std::vector<std::string> ids;
    for (const auto& session : account_manager.get_all_sessions()) {
        ids.push_back(session.id);
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

static constexpr char kRemoteSignOutUnconfirmed[] =
    "This launcher removed its local session, but remote sign-out could not be confirmed. "
    "Already-issued access tokens may remain valid until they expire.";

static bool account_supabase_scope_is_current(const std::string& expected_user_id,
                                              uint64_t expected_session_generation) {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    return supabase.session_generation() == expected_session_generation &&
           supabase.get_current_user().id == expected_user_id;
}

// The protected-local removal is deliberately completed before starting the
// worker. AccountManager owns mutable local session storage and is currently
// render-thread owned; the potentially slow remote sign-out itself always runs
// on the serialized account lane below.
static AsyncUiRequestResult run_remote_sign_out(const std::string& expected_user_id,
                                                uint64_t expected_session_generation) {
    AsyncUiRequestResult result;
    result.payload_a = expected_user_id;
    result.payload_c = std::to_string(expected_session_generation);
    result.success = true; // Local sign-out already succeeded before this task.

    try {
        if (!account_supabase_scope_is_current(expected_user_id,
                                               expected_session_generation)) {
            result.warning = true;
            result.title = "Local sign-out completed";
            result.detail =
                "The active Amalgam session changed before its remote sign-out request began. "
                "The launcher did not sign out the newer session.";
            return result;
        }

        const bool remote_signed_out = aml::supabase::SupabaseManager::instance().sign_out(
            aml::supabase::SignOutScope::Local);
        result.number_a = remote_signed_out ? 1 : 0;
        result.warning = !remote_signed_out;
        result.title = remote_signed_out ? "Signed out of Amalgam"
                                         : "Local sign-out completed";
        result.detail = remote_signed_out
            ? "This Windows device was signed out. Other devices were not affected."
            : kRemoteSignOutUnconfirmed;
        return result;
    } catch (const std::exception&) {
        result.warning = true;
        result.title = "Local sign-out completed";
        result.detail = kRemoteSignOutUnconfirmed;
        return result;
    } catch (...) {
        result.warning = true;
        result.title = "Local sign-out completed";
        result.detail = kRemoteSignOutUnconfirmed;
        return result;
    }
}

static void consume_account_sign_out(UiState& st) {
    const auto snapshot = snapshot_async_ui_request(st.auth_async_request);
    if (snapshot.action != kAccountSignOutAction || !snapshot.has_result) return;

    AsyncUiRequestSnapshot completed;
    if (!take_auth_async_request_result(st, kAccountSignOutAction, &completed)) return;

    auto& account_state = get_account_ui_state(st);
    auto& action = account_state.pending_action;
    const bool owns_result = action.kind == AccountActionKind::SignOutThisDevice &&
        action.submitting &&
        (completed.result.payload_a.empty() ||
         completed.result.payload_a == action.expected_amalgam_user_id) &&
        (completed.result.payload_c.empty() ||
         completed.result.payload_c ==
             std::to_string(action.expected_supabase_session_generation));
    if (!owns_result) {
        // This can only occur after the confirmation was superseded locally.
        // The worker's remote call is terminal, so never retry it against an
        // unknown current account.
        return;
    }

    // A successful remote sign-out intentionally advances the manager session
    // generation, so this completion must be validated by ownership of this
    // serialized action rather than by comparing its now-obsolete scope.
    reset_sensitive_account_ui_state(account_state);
    account_state.bound_amalgam_user_id.clear();
    account_state.action_popup_close_requested = true;
    clear_account_passive_cache(st);
    aml::entitlements::EntitlementManager::instance().invalidate();

    if (completed.result.warning || !completed.result.success) {
        push_notice(st, ui_model::NoticeLevel::Warning, "Local sign-out completed",
                    completed.result.detail.empty() ? kRemoteSignOutUnconfirmed
                                                    : completed.result.detail);
        return;
    }
    push_notice(st, ui_model::NoticeLevel::Success, "Signed out of Amalgam",
                completed.result.detail.empty()
                    ? "This Windows device was signed out. Other devices were not affected."
                    : completed.result.detail);
}

static AsyncUiRequestResult run_account_session_switch(
    aml::account::AccountSession target, aml::account::AccountSession previous) {
    AsyncUiRequestResult result;
    result.payload_a = target.id;
    result.payload_b = previous.id;
    try {
        auto& supabase = aml::supabase::SupabaseManager::instance();
        result.success = !target.access_token.empty() &&
            supabase.auto_login(target.access_token, target.refresh_token);
        if (result.success) {
            result.title = "Account switched";
            result.detail = "The selected Amalgam account is ready on this Windows device.";
            return result;
        }

        // `auto_login` clears an invalid provisional session. Restore the
        // previously active account inside the same serialized lane before the
        // UI reselects its durable local record.
        const bool previous_restored = !previous.access_token.empty() &&
            supabase.auto_login(previous.access_token, previous.refresh_token);
        result.number_a = previous_restored ? 1 : 0;
        result.title = "Account switch failed";
        result.detail = previous_restored
            ? "The selected remembered sign-in could not be restored. Your previous account remains active."
            : "The selected remembered sign-in could not be restored. Please sign in again.";
        return result;
    } catch (const std::exception&) {
        result.title = "Account switch failed";
        result.detail = "The selected remembered sign-in could not be restored. Please sign in again.";
        return result;
    } catch (...) {
        result.title = "Account switch failed";
        result.detail = "The selected remembered sign-in could not be restored. Please sign in again.";
        return result;
    }
}

static void consume_account_session_switch(UiState& st) {
    const auto snapshot = snapshot_async_ui_request(st.auth_async_request);
    if (snapshot.action != kAccountSwitchSessionAction || !snapshot.has_result) return;

    AsyncUiRequestSnapshot completed;
    if (!take_auth_async_request_result(st, kAccountSwitchSessionAction, &completed)) return;

    auto& account_state = get_account_ui_state(st);
    if (!account_state.session_switch_submitting ||
        completed.result.payload_a != account_state.session_switch_target_id ||
        completed.result.payload_b != account_state.session_switch_previous_id) {
        return;
    }

    const std::string target_email = account_state.session_switch_target_email;
    const std::string previous_id = account_state.session_switch_previous_id;
    account_state.session_switch_submitting = false;
    wipe_string(account_state.session_switch_target_id);
    wipe_string(account_state.session_switch_target_email);
    wipe_string(account_state.session_switch_previous_id);

    auto& account_manager = aml::account::AccountManager::instance();
    if (!completed.result.success) {
        const bool selected_previous = previous_id.empty() ||
            account_manager.set_current_session(previous_id);
        clear_account_passive_cache(st);
        account_state.bound_amalgam_user_id.clear();
        sync_account_services(completed.result.number_a != 0);
        push_notice(st, ui_model::NoticeLevel::Error, "Account switch failed",
                    completed.result.detail.empty()
                        ? (selected_previous
                            ? "Your previous account remains selected. Please sign in again to retry."
                            : "The previous local account could not be reselected. Please sign in again.")
                        : completed.result.detail);
        return;
    }

    reset_sensitive_account_ui_state(account_state);
    account_state.bound_amalgam_user_id.clear();
    clear_account_passive_cache(st);
    sync_account_services(true);
    aml::entitlements::EntitlementManager::instance().request_refresh_from_supabase();
    push_notice(st, ui_model::NoticeLevel::Success, "Account switched",
                target_email.empty()
                    ? "The selected Amalgam account is ready on this Windows device."
                    : "Switched to account: " + target_email);
}

static void request_account_session_switch(UiState& st,
                                           const aml::account::AccountSession& target) {
    if (st.fixture_mode) return;
    if (account_auth_lane_reserved(st)) {
        push_notice(st, ui_model::NoticeLevel::Info, "Account operation in progress",
                    "Wait for the current account request to finish before switching accounts.");
        return;
    }
    if (target.id.empty()) return;
    if (target.is_expired()) {
        push_notice(st, ui_model::NoticeLevel::Warning, "Remembered sign-in expired",
                    "This remembered Amalgam sign-in has expired. Sign in again to refresh it safely.");
        return;
    }

    auto& account_manager = aml::account::AccountManager::instance();
    const auto previous = account_manager.get_current_session();
    if (target.id == previous.id) return;
    if (!account_manager.set_current_session(target.id)) {
        push_notice(st, ui_model::NoticeLevel::Error, "Account switch failed",
                    "The selected remembered sign-in could not be selected on this Windows device.");
        return;
    }

    auto& account_state = get_account_ui_state(st);
    account_state.session_switch_target_id = target.id;
    account_state.session_switch_target_email = target.email;
    account_state.session_switch_previous_id = previous.id;
    if (!start_auth_async_request(
            st, kAccountSwitchSessionAction,
            [target, previous]() { return run_account_session_switch(target, previous); })) {
        if (!previous.id.empty()) account_manager.set_current_session(previous.id);
        account_state.session_switch_target_id.clear();
        account_state.session_switch_target_email.clear();
        account_state.session_switch_previous_id.clear();
        push_notice(st, ui_model::NoticeLevel::Warning, "Account switch delayed",
                    "The selected local account was restored. Wait for the current account request, then try again.");
        return;
    }
    account_state.session_switch_submitting = true;
    clear_account_passive_cache(st);
}

static const char* account_action_title(AccountActionKind kind) {
    switch (kind) {
        case AccountActionKind::SignOutThisDevice: return "Sign out of Amalgam on this device";
        case AccountActionKind::RemoveRememberedSession: return "Remove remembered local sign-in";
        case AccountActionKind::RemoveAllRememberedAccounts: return "Remove all remembered local accounts";
        case AccountActionKind::DisconnectMinecraft: return "Disconnect Minecraft from this device";
        case AccountActionKind::None: break;
    }
    return "Confirm account action";
}

static const char* account_action_button(AccountActionKind kind) {
    switch (kind) {
        case AccountActionKind::SignOutThisDevice: return "Sign out on this device";
        case AccountActionKind::RemoveRememberedSession: return "Remove local sign-in";
        case AccountActionKind::RemoveAllRememberedAccounts: return "Remove local accounts";
        case AccountActionKind::DisconnectMinecraft: return "Disconnect Minecraft";
        case AccountActionKind::None: break;
    }
    return "Continue";
}

static bool account_action_is_destructive(AccountActionKind kind) {
    return kind == AccountActionKind::RemoveAllRememberedAccounts ||
           kind == AccountActionKind::DisconnectMinecraft;
}

static void begin_account_action(UiState& st, AccountPendingAction action) {
    if (account_auth_lane_reserved(st)) {
        push_notice(st, ui_model::NoticeLevel::Info, "Account operation in progress",
                    "Wait for the current account request to finish before reviewing another action.");
        return;
    }
    auto& account_manager = aml::account::AccountManager::instance();
    const auto current_session = account_manager.get_current_session();
    const auto current_user = aml::supabase::SupabaseManager::instance().get_current_user();
    action.expected_amalgam_user_id = current_user.id;
    action.expected_current_session_id = current_session.id;
    action.expected_local_session_ids = local_session_ids(account_manager);
    action.expected_supabase_session_generation =
        aml::supabase::SupabaseManager::instance().session_generation();
    action.expected_auth_generation = st.auth_operation_generation.load();
    get_account_ui_state(st).pending_action = std::move(action);
}

void request_amalgam_sign_out(UiState& st) {
    if (st.fixture_mode) return;
    if (account_auth_lane_reserved(st)) {
        push_notice(st, ui_model::NoticeLevel::Info, "Account operation in progress",
                    "Wait for the current account request to finish before signing out.");
        return;
    }
    auto& account_manager = aml::account::AccountManager::instance();
    const auto current_session = account_manager.get_current_session();
    const auto current_user = aml::supabase::SupabaseManager::instance().get_current_user();
    if (current_session.id.empty() && current_user.id.empty()) {
        push_notice(st, ui_model::NoticeLevel::Info, "Already signed out",
                    "There is no active Amalgam account on this device.");
        return;
    }
    AccountPendingAction action;
    action.kind = AccountActionKind::SignOutThisDevice;
    action.target_label = !current_user.email.empty() ? current_user.email :
        (!current_session.email.empty() ? current_session.email : "the active Amalgam account");
    begin_account_action(st, std::move(action));
}

void request_remove_local_session(UiState& st, const std::string& session_id) {
    if (st.fixture_mode) return;
    auto& account_manager = aml::account::AccountManager::instance();
    const auto sessions = account_manager.get_all_sessions();
    const auto it = std::find_if(sessions.begin(), sessions.end(), [&](const auto& session) {
        return session.id == session_id;
    });
    if (it == sessions.end()) {
        push_notice(st, ui_model::NoticeLevel::Warning, "Account state changed",
                    "That remembered local sign-in is no longer available. Review the list again.");
        return;
    }
    AccountPendingAction action;
    action.kind = AccountActionKind::RemoveRememberedSession;
    action.target_label = it->email.empty() ? "the selected local sign-in" : it->email;
    action.expected_session_id = it->id;
    action.expected_session_email = it->email;
    begin_account_action(st, std::move(action));
}

void request_remove_all_local_accounts(UiState& st) {
    if (st.fixture_mode) return;
    auto& account_manager = aml::account::AccountManager::instance();
    const auto ids = local_session_ids(account_manager);
    if (ids.empty()) {
        push_notice(st, ui_model::NoticeLevel::Info, "Nothing to remove",
                    "This device has no remembered Amalgam sign-ins.");
        return;
    }
    AccountPendingAction action;
    action.kind = AccountActionKind::RemoveAllRememberedAccounts;
    action.target_label = std::to_string(ids.size()) +
        (ids.size() == 1 ? " remembered local account" : " remembered local accounts");
    begin_account_action(st, std::move(action));
}

void request_minecraft_disconnect(UiState& st) {
    if (st.fixture_mode) return;
    if (st.auth_working.load()) {
        push_notice(st, ui_model::NoticeLevel::Warning, "Sign-in is still running",
                    "Wait for the Microsoft sign-in flow to finish before disconnecting Minecraft.");
        return;
    }
    auth::Account account;
    {
        std::lock_guard<std::mutex> lock(st.auth_mu);
        account = st.account;
    }
    if (account.uuid.empty() || account.username.empty()) {
        push_notice(st, ui_model::NoticeLevel::Info, "Minecraft is already disconnected",
                    "This launcher has no saved Minecraft sign-in to remove.");
        return;
    }
    AccountPendingAction action;
    action.kind = AccountActionKind::DisconnectMinecraft;
    action.target_label = account.username;
    action.expected_minecraft_uuid = account.uuid;
    action.expected_minecraft_username = account.username;
    begin_account_action(st, std::move(action));
}

void open_account_tab(UiState& st, int tab) {
    auto& account_state = get_account_ui_state(st);
    account_state.current_tab = std::clamp(tab, 0, 5);
    navigate_to(st, 15, 15);
}

static bool revalidate_account_action(UiState& st, AccountPendingAction& action) {
    auto& account_manager = aml::account::AccountManager::instance();
    const auto fail_changed = [&action]() {
        action.error = "Your account state changed. Review the action again before continuing.";
        return false;
    };

    if (account_auth_lane_reserved(st)) {
        action.error = "An account request is still finishing. Wait for it to complete, then review this action again.";
        return false;
    }

    switch (action.kind) {
        case AccountActionKind::SignOutThisDevice: {
            const auto current_user = aml::supabase::SupabaseManager::instance().get_current_user();
            const auto current_session = account_manager.get_current_session();
            if ((!action.expected_amalgam_user_id.empty() &&
                 current_user.id != action.expected_amalgam_user_id) ||
                aml::supabase::SupabaseManager::instance().session_generation() !=
                    action.expected_supabase_session_generation ||
                current_session.id != action.expected_current_session_id) {
                return fail_changed();
            }
            return true;
        }
        case AccountActionKind::RemoveRememberedSession: {
            const auto sessions = account_manager.get_all_sessions();
            const auto it = std::find_if(sessions.begin(), sessions.end(), [&](const auto& session) {
                return session.id == action.expected_session_id &&
                       session.email == action.expected_session_email;
            });
            return it == sessions.end() ? fail_changed() : true;
        }
        case AccountActionKind::RemoveAllRememberedAccounts:
            return local_session_ids(account_manager) == action.expected_local_session_ids
                ? true : fail_changed();
        case AccountActionKind::DisconnectMinecraft: {
            if (st.auth_working.load() ||
                st.auth_operation_generation.load() != action.expected_auth_generation) {
                return fail_changed();
            }
            std::lock_guard<std::mutex> lock(st.auth_mu);
            return st.account.uuid == action.expected_minecraft_uuid &&
                   st.account.username == action.expected_minecraft_username
                ? true : fail_changed();
        }
        case AccountActionKind::None:
            break;
    }
    action.error = "No account action is pending.";
    return false;
}

static bool execute_account_action(UiState& st, AccountPendingAction& action) {
    auto& account_manager = aml::account::AccountManager::instance();
    auto& account_state = get_account_ui_state(st);
    switch (action.kind) {
        case AccountActionKind::SignOutThisDevice: {
            if (action.submitting) return false;
            // Keep local AccountManager state on the render thread until that
            // subsystem gains a fully synchronized ownership boundary. The
            // network operation below is the potentially unbounded work and
            // always leaves the render path.
            if (!account_manager.end_current_session()) {
                action.error =
                    "The protected local session could not be removed. You remain signed in here; "
                    "no remote sign-out was attempted.";
                return false;
            }
            const std::string expected_user_id = action.expected_amalgam_user_id;
            const uint64_t expected_session_generation =
                action.expected_supabase_session_generation;
            if (!start_auth_async_request(
                    st, kAccountSignOutAction,
                    [expected_user_id, expected_session_generation]() {
                        return run_remote_sign_out(expected_user_id,
                                                   expected_session_generation);
                    })) {
                // A lane collision is unlikely after revalidation, but the
                // durable local sign-out has already happened. Finish it
                // honestly rather than retaining a confirmation that can no
                // longer target the removed session.
                reset_sensitive_account_ui_state(account_state);
                account_state.bound_amalgam_user_id.clear();
                clear_account_passive_cache(st);
                aml::entitlements::EntitlementManager::instance().invalidate();
                push_notice(st, ui_model::NoticeLevel::Warning, "Local sign-out completed",
                            kRemoteSignOutUnconfirmed);
                return true;
            }
            action.submitting = true;
            action.error.clear();
            return false;
        }
        case AccountActionKind::RemoveRememberedSession:
            if (!account_manager.end_session(action.expected_session_id)) {
                action.error = "The remembered sign-in could not be removed from protected local storage.";
                return false;
            }
            reset_sensitive_account_ui_state(account_state);
            // A local session removal changes the values captured by the
            // independent statistics worker. Drop an in-flight pre-removal
            // snapshot rather than letting it make the cache look fresh.
            invalidate_account_passive_request(st);
            st.account_passive_cache.mark_values_stale();
            push_notice(st, ui_model::NoticeLevel::Success, "Local sign-in removed",
                        "Only this Windows device was changed. Other devices remain signed in.");
            return true;
        case AccountActionKind::RemoveAllRememberedAccounts:
            if (!account_manager.end_all_sessions()) {
                action.error = "The remembered sign-ins could not be removed from protected local storage.";
                return false;
            }
            aml::entitlements::EntitlementManager::instance().invalidate();
            reset_sensitive_account_ui_state(account_state);
            clear_account_passive_cache(st);
            push_notice(st, ui_model::NoticeLevel::Success, "Local accounts removed",
                        "Remembered Amalgam sign-ins were removed from this Windows device. The active session was not remotely revoked.");
            return true;
        case AccountActionKind::DisconnectMinecraft: {
            bool expected = false;
            if (!st.auth_working.compare_exchange_strong(expected, true)) {
                action.error = "A Minecraft sign-in is running. Wait for it to finish, then review this action again.";
                return false;
            }
            const uint64_t disconnect_generation = st.auth_operation_generation.fetch_add(1) + 1;
            std::string local_error;
            const bool local_removed = auth::logout(&local_error);
            {
                std::lock_guard<std::mutex> lock(st.auth_mu);
                st.auth_working = false;
                if (local_removed) {
                    st.account = {};
                    st.auth_checked = true;
                    st.auth_status = "No Microsoft account connected";
                    st.login_wizard_state = 0;
                    st.login_verification_uri.clear();
                    st.login_user_code.clear();
                    st.login_error.clear();
                    st.login_expires_in = 0;
                }
            }
            if (!local_removed) {
                action.expected_auth_generation = disconnect_generation;
                action.error = "Minecraft could not be disconnected from protected local storage" +
                    (local_error.empty() ? std::string(".") : std::string(": ") + local_error);
                return false;
            }

            const auto current_user = aml::supabase::SupabaseManager::instance().get_current_user();
            const bool remotely_linked = current_user.metadata.count("microsoft_linked") &&
                current_user.metadata.at("microsoft_linked") == "true";
            const bool remote_unlinked = !remotely_linked || account_manager.unlink_microsoft_account();
            reset_sensitive_account_ui_state(account_state);
            st.account_passive_cache.mark_values_stale();
            if (remote_unlinked) {
                push_notice(st, ui_model::NoticeLevel::Success, "Minecraft disconnected",
                            "The saved Minecraft sign-in was removed from this Windows device. Official Minecraft Launcher and browser sessions were not changed.");
            } else {
                push_notice(st, ui_model::NoticeLevel::Warning, "Minecraft disconnected locally",
                            "The saved Minecraft sign-in was removed from this Windows device, but Amalgam could not confirm its linked-account metadata update.");
            }
            return true;
        }
        case AccountActionKind::None:
            break;
    }
    action.error = "No account action is pending.";
    return false;
}

void draw_account_action_dialogs(UiState& st) {
    auto& account_state = get_account_ui_state(st);
    if (!st.fixture_mode) {
        // This owner is drawn with the shell overlays, not only on the Account
        // page, so a sign-out result is always drained after navigation.
        consume_account_sign_out(st);
        consume_account_session_switch(st);
        if (account_state.action_popup_close_requested) {
            if (ImGui::BeginPopupModal(kAccountActionPopup, nullptr,
                                       ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
            account_state.action_popup_close_requested = false;
        }
    }
    if (st.fixture_mode) {
        if (is_fixture_account_action_case(st.fixture_case)) {
            draw_fixture_account_action_dialog(st);
        }
        return;
    }
    auto& action = account_state.pending_action;
    if (action.kind == AccountActionKind::None) return;

    ImGui::OpenPopup(kAccountActionPopup);
    ImGui::SetNextWindowSize(ImVec2(ui_px(540.0f), 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(kAccountActionPopup, nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) return;

    const bool destructive = account_action_is_destructive(action.kind);
    ImGui::PushFont(f_h2);
    ImGui::TextColored(destructive ? k.red : k.text, "%s", account_action_title(action.kind));
    ImGui::PopFont();
    ImGui::TextColored(k.muted, "Account: %s", action.target_label.c_str());
    ImGui::Spacing();
    if (action.submitting) {
        ImGui::TextColored(k.brand_hov,
                           "Signing out safely in the background. You can keep using the launcher.");
        ImGui::Spacing();
    }
    switch (action.kind) {
        case AccountActionKind::SignOutThisDevice:
            ImGui::TextWrapped("This signs the active Amalgam account out on this Windows device. It removes this launcher's protected local session and requests a local-scope remote sign-out.");
            ImGui::TextColored(k.muted, "Other devices are not affected. This does not disconnect Minecraft.");
            break;
        case AccountActionKind::RemoveRememberedSession:
            ImGui::TextWrapped("This removes a locally remembered Amalgam sign-in from this Windows device only. It does not sign that account out on another device.");
            break;
        case AccountActionKind::RemoveAllRememberedAccounts:
            ImGui::TextWrapped("This removes every locally remembered Amalgam sign-in from this Windows device. It does not remotely revoke sessions on other devices.");
            ImGui::Spacing();
            ImGui::TextColored(k.red, "Type REMOVE LOCAL ACCOUNTS to enable this action.");
            ImGui::SetNextItemWidth(-1);
            input_text_hint("##remove_local_accounts_phrase", "REMOVE LOCAL ACCOUNTS", &action.typed_confirmation);
            break;
        case AccountActionKind::DisconnectMinecraft:
            ImGui::TextWrapped("This removes the saved Minecraft sign-in from this Windows device. It does not sign you out of the official Minecraft Launcher, Microsoft browser sessions, or other devices.");
            break;
        case AccountActionKind::None:
            break;
    }
    ImGui::Spacing();
    if (!action.error.empty()) {
        ImGui::TextColored(k.red, "%s", action.error.c_str());
        ImGui::Spacing();
    }

    const bool phrase_matches = action.kind != AccountActionKind::RemoveAllRememberedAccounts ||
        action.typed_confirmation == "REMOVE LOCAL ACCOUNTS";
    const bool account_lane_busy = account_auth_lane_reserved(st);
    if (account_lane_busy && !action.submitting) {
        ImGui::TextColored(k.muted,
                           "An account request is still finishing. This action is unavailable until it completes.");
        ImGui::Spacing();
    }
    const bool ready = phrase_matches && !account_lane_busy && !action.submitting;
    if (!action.submitting &&
        (ImGui::IsKeyPressed(ImGuiKey_Escape) ||
         ghost_button("Cancel", ImVec2(ui_px(136.0f), ui_px(34.0f))))) {
        get_account_ui_state(st).pending_action = {};
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    if (action.submitting) {
        ghost_button("Working...", ImVec2(ui_px(136.0f), ui_px(34.0f)), true);
    }
    ImGui::SameLine();
    const bool confirmed = destructive
        ? danger_button(action.submitting ? "Signing out..." : account_action_button(action.kind),
                        ImVec2(ui_px(220.0f), ui_px(34.0f)), !ready)
        : primary_button(action.submitting ? "Signing out..." : account_action_button(action.kind),
                         ImVec2(ui_px(220.0f), ui_px(34.0f)), false, !ready);
    if (confirmed && ready && revalidate_account_action(st, action) &&
        execute_account_action(st, action)) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// ---------------------------------------------------------------------------
// Account Overview Page
// ---------------------------------------------------------------------------

void draw_account_overview(UiState& st) {
    auto& account_state = get_account_ui_state(st);
    auto& account_manager = aml::account::AccountManager::instance();

    page_title("Account Overview", "Manage your Amalgam account and preferences");

    // Account card
    card_begin("##account_card");
    const bool compact_account_header =
        ImGui::GetContentRegionAvail().x < ui_px(760.0f);

    auto profile = account_manager.get_profile();
    request_account_stats(st);
    const auto& stats_state = st.account_passive_cache.stats_state;
    const auto& stats = st.account_passive_cache.stats;

    // Avatar and info
    ImGui::BeginGroup();

    // Avatar
    if (!profile.avatar_url.empty()) {
        ImVec2 avatar_pos = ImGui::GetCursorScreenPos();
        draw_project_image(st, profile.avatar_url,
                           avatar_pos, ImVec2(ui_px(80.0f), ui_px(80.0f)), c32(k.brand));
        ImGui::Dummy(ImVec2(ui_px(80.0f), ui_px(80.0f)));
    } else {
        // Default avatar
        ImVec2 avatar_pos = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddCircleFilled(avatar_pos + ImVec2(ui_px(40.0f), ui_px(40.0f)), ui_px(40.0f), c32(k.brand));
        draw_brand_mark(dl, avatar_pos + ImVec2(ui_px(40.0f), ui_px(40.0f)), ui_px(0.5f));
        ImGui::Dummy(ImVec2(ui_px(80.0f), ui_px(80.0f)));
    }

    ImGui::EndGroup();
    if (!compact_account_header) ImGui::SameLine();
    else ImGui::Spacing();

    // Account info
    ImGui::BeginGroup();
    ImGui::PushFont(f_title);
    ImGui::Text("%s", profile.display_name.empty() ? profile.username.c_str() : profile.display_name.c_str());
    ImGui::PopFont();
    ImGui::TextColored(k.muted, "%s", profile.email.c_str());

    // Keep Amalgam account state separate from Minecraft authentication.
    ImGui::Spacing();
    ImGui::TextColored(k.green, "✓ Amalgam Account Connected");
    ImGui::TextColored(k.muted,
                       "Minecraft sign-in is handled by the official Minecraft Launcher when you play.");

    ImGui::EndGroup();

    if (!compact_account_header) ImGui::SameLine();
    else ImGui::Spacing();

    // Quick actions
    ImGui::BeginGroup();
    if (ghost_button("Edit Profile", ImVec2(ui_px(120.0f), ui_px(32.0f)))) {
        account_state.current_tab = 1;
        account_state.profile_editing = true;
        account_state.editing_profile = profile;
    }
    if (ghost_button("Account Settings", ImVec2(ui_px(120.0f), ui_px(32.0f)))) {
        account_state.current_tab = 2;
    }
    if (ghost_button("Security", ImVec2(ui_px(120.0f), ui_px(32.0f)))) {
        account_state.current_tab = 3;
    }
    ImGui::EndGroup();

    card_end();

    ImGui::Spacing();

    // Statistics
    card_begin("##account_stats");
    ImGui::TextUnformatted("Account Statistics");
    ImGui::SameLine();
    const bool stats_refresh_blocked = account_passive_stats_lane_reserved(st);
    if (ghost_button(stats_state.loading ? "Refreshing..."
                                         : (!stats_state.error.empty() ? "Retry" : "Refresh"),
                     ImVec2(ui_px(92.0f), ui_px(26.0f)), stats_refresh_blocked)) {
        request_account_stats(st, true);
    }
    ImGui::Separator();
    ImGui::Spacing();

    if (stats_state.loading) {
        ImGui::TextColored(k.muted, stats_state.has_value
            ? "Refreshing local account statistics..."
            : "Loading local account statistics...");
        ImGui::Spacing();
    } else if (!stats_state.error.empty()) {
        ImGui::TextColored(k.red, "%s", stats_state.error.c_str());
        ImGui::Spacing();
    } else if (!stats_state.warning.empty()) {
        ImGui::TextColored(k.orange, "%s", stats_state.warning.c_str());
        ImGui::Spacing();
    } else if (account_passive_cache_is_stale(stats_state)) {
        ImGui::TextColored(k.muted,
                           "Showing cached account statistics. Refresh when you want the latest local scan.");
        ImGui::Spacing();
    } else if (!stats_state.has_value && stats_refresh_blocked) {
        ImGui::TextColored(k.muted,
                           "Waiting for the local statistics scan to finish.");
        ImGui::Spacing();
    }

    const float available_stats = ImGui::GetContentRegionAvail().x;
    const float stat_gap = ui_px(10.0f);
    const int stat_columns = ui_model::stat_card_columns(available_stats, g_ui_scale);
    const float stat_w = std::max(ui_px(120.0f),
        (available_stats - stat_gap * static_cast<float>(stat_columns - 1)) /
            static_cast<float>(stat_columns));
    const std::string sessions_value = stats_state.has_value
        ? std::to_string(stats.total_sessions) : (stats_state.loading ? "Loading..." : "Unavailable");
    const std::string created_value = stats_state.has_value
        ? format_date(stats.account_created_at) : (stats_state.loading ? "Loading..." : "Unavailable");
    const std::string login_value = stats_state.has_value
        ? format_date(stats.last_login_at) : (stats_state.loading ? "Loading..." : "Unavailable");
    const char* current_value = !stats_state.has_value
        ? (stats_state.loading ? "Loading..." : "Unavailable")
        : (stats.current_session_active ? "Active" : "None");
    struct AccountStat { const char* label; const char* value; ImVec4 accent; };
    const AccountStat account_stats[] = {
        {"Sessions", sessions_value.c_str(), k.brand},
        {"Created", created_value.c_str(), k.blue},
        {"Last Login", login_value.c_str(), k.green},
        {"Current", current_value,
         (!stats_state.has_value || !stats.current_session_active) ? k.muted : k.green},
    };
    for (int i = 0; i < 4; ++i) {
        if (i > 0 && i % stat_columns != 0) ImGui::SameLine(0, stat_gap);
        draw_stat_card(account_stats[i].label, account_stats[i].value,
                       -1.0f, account_stats[i].accent, stat_w);
    }
    if (stats_state.has_value) {
        ImGui::Spacing();
        ImGui::TextColored(k.muted, "%d local profiles · %d installed modpacks",
                           stats.total_instances, stats.total_modpacks);
    }
    card_end();

    ImGui::Spacing();

    // ── Membership & entitlements ─────────────────────────────────
    {
        auto& ents = aml::entitlements::EntitlementManager::instance();
        const auto e = ents.snapshot();
        const bool plus = e.valid && ents.is_plus();
        const bool entitlement_known = e.valid;

        // Request a refresh from Supabase (Whop-synced subscription data)
        // if we have no cached data yet. The manager throttles by interval.
        if (!e.valid && !ents.refreshing()) {
            if (aml::supabase::SupabaseManager::instance().is_authenticated()) {
                ents.request_refresh_from_supabase();
            } else {
                const std::string token = account_manager.get_current_session().access_token;
                if (!token.empty()) ents.request_refresh(token);
            }
        }

        card_begin("##account_membership");
        ImVec2 mem_card_min = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // Top accent strip
        ImVec4 accent = plus ? k.brand : k.surface2;
        dl->AddRectFilled(mem_card_min,
            ImVec2(mem_card_min.x + ImGui::GetContentRegionAvail().x, mem_card_min.y + ui_px(4.0f)),
            c32(accent), ui_px(2.0f));
        ImGui::Dummy(ImVec2(0, ui_px(8.0f)));

        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("MEMBERSHIP");
        ImGui::PopFont();
        ImGui::Spacing();

        // ── Plan header ─────────────────────────────────────────────
        const char* plan_label = !entitlement_known
            ? "CHECKING MEMBERSHIP"
            : (e.plan_label.empty() ? (plus ? "AMALGAM+" : "FREE") : e.plan_label.c_str());
        ImGui::PushFont(f_title);
        ImGui::TextColored(plus ? k.brand : k.text, "%s", plan_label);
        ImGui::PopFont();
        ImGui::SameLine(0, ui_px(12.0f));

        if (plus) {
            dl->AddRectFilled(ImGui::GetCursorScreenPos(),
                ImVec2(ImGui::GetCursorScreenPos().x + ui_px(60.0f), ImGui::GetCursorScreenPos().y + ui_px(20.0f)),
                c32(k.green), ui_px(4.0f));
            ImGui::PushFont(f_small);
            dl->AddText(ImVec2(ImGui::GetCursorScreenPos().x + ui_px(8.0f), ImGui::GetCursorScreenPos().y + ui_px(3.0f)),
                IM_COL32(255,255,255,255), "ACTIVE");
            ImGui::PopFont();
            ImGui::Dummy(ImVec2(ui_px(60.0f), ui_px(20.0f)));
        }

        if (entitlement_known) {
            ImGui::TextColored(k.muted, "$%.2f/month", plus ? 19.99 : 0.0);
        } else {
            ImGui::TextColored(k.muted, "Membership status is loading");
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // ── ESSENTIALS RELAY section ─────────────────────────────────
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("ESSENTIALS RELAY");
        ImGui::PopFont();
        ImGui::Spacing();

        if (e.turn_monthly_bytes > 0) {
            const uint64_t used = e.turn_used_bytes;
            const uint64_t total = e.turn_monthly_bytes;
            const uint64_t remaining = total > used ? total - used : 0;
            const float frac = total > 0
                ? static_cast<float>(static_cast<double>(used) / total) : 0.0f;
            const float gb = 1024.0f * 1024.0f * 1024.0f;

            // Used / Total with progress bar
            ImGui::TextColored(k.text, "%.1f / %.1f GB", used / gb, total / gb);
            ImGui::Spacing();

            ImVec4 meter_color = frac >= 1.0f ? k.red :
                                 frac >= 0.9f ? k.red :
                                 frac >= 0.75f ? k.yellow : k.green;
            progress_bar(frac, ImVec2(ui_px(340.0f), ui_px(14.0f)),
                         nullptr, &meter_color);
            ImGui::Spacing();

            ImGui::TextColored(k.muted, "%.1f GB remaining", remaining / gb);

            // Reset date
            if (e.turn_reset_at > 0) {
                std::time_t reset_time = static_cast<std::time_t>(e.turn_reset_at);
                std::tm reset_tm{};
                localtime_s(&reset_tm, &reset_time);
                char reset_buf[64]{};
                std::strftime(reset_buf, sizeof(reset_buf), "%B %d", &reset_tm);
                ImGui::SameLine(ui_px(200.0f));
                ImGui::TextColored(k.muted, "Resets %s", reset_buf);
            }

            // Warnings
            ImGui::Spacing();
            if (frac >= 1.0f) {
                ImGui::TextColored(k.red, "Relay limit reached — direct P2P still works.");
            } else if (frac >= 0.9f) {
                ImGui::TextColored(k.red, "Almost at monthly relay limit.");
            } else if (frac >= 0.75f) {
                ImGui::TextColored(k.yellow, "Relay usage high.");
            }
        } else {
            ImGui::TextColored(k.muted, "No relay data available.");
            if (plus)
                ImGui::TextColored(k.muted, "Relay entitlement will appear after your first Essentials connection.");
        }

        // ── CLOUD SERVERS section ────────────────────────────────────
        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("CLOUD SERVERS");
        ImGui::PopFont();
        ImGui::Spacing();

        if (!entitlement_known) {
            ImGui::TextColored(k.muted, "Hosted-server details are loading from your account.");
        } else if (e.cloud_servers_used > 0) {
            ImGui::TextColored(k.text, "%d hosted server%s linked to this account",
                               e.cloud_servers_used, e.cloud_servers_used == 1 ? "" : "s");
            ImGui::TextColored(k.muted,
                               "Plan names, billing, and server controls are managed on the website.");
        } else {
            ImGui::TextColored(k.muted, "No hosted servers linked to this account.");
            ImGui::TextColored(k.muted,
                               "Create and manage Amalgam Cloud servers on the Amalgam website.");
        }

        ImGui::Spacing();

        // ── Action buttons ───────────────────────────────────────────
        if (plus) {
            if (ghost_button("Manage Membership", ImVec2(ui_px(170.0f), ui_px(32.0f)))) {
                ShellExecuteW(st.hwnd, L"open",
                    aml::net::to_wide(aml::online::config().manage_membership_url()).c_str(),
                    nullptr, nullptr, SW_SHOWNORMAL);
            }
        } else {
            if (primary_button(entitlement_known ? "View Amalgam+ Plans" : "View Membership Plans", ImVec2(ui_px(190.0f), ui_px(32.0f)))) {
                ShellExecuteW(st.hwnd, L"open",
                    aml::net::to_wide(aml::online::config().plans_url()).c_str(),
                    nullptr, nullptr, SW_SHOWNORMAL);
            }
        }
        if (entitlement_known) {
            ImGui::SameLine(0, ui_px(6.0f));
            if (ghost_button("Manage Cloud on Website", ImVec2(ui_px(190.0f), ui_px(32.0f)))) {
                ShellExecuteW(st.hwnd, L"open",
                    aml::net::to_wide(aml::online::config().cloud_account_url()).c_str(),
                    nullptr, nullptr, SW_SHOWNORMAL);
            }
        }
        if (plus && e.turn_monthly_bytes > 0 &&
            static_cast<float>(static_cast<double>(e.turn_used_bytes) / e.turn_monthly_bytes) >= 0.75f) {
            ImGui::SameLine(0, ui_px(6.0f));
            if (ghost_button("Review Relay Options", ImVec2(ui_px(170.0f), ui_px(32.0f)))) {
                ShellExecuteW(st.hwnd, L"open",
                    aml::net::to_wide(aml::online::config().plans_url()).c_str(),
                    nullptr, nullptr, SW_SHOWNORMAL);
            }
        }
        card_end();
    }

    ImGui::Spacing();

    // ── Plan comparison ──────────────────────────────────────────
    {
        auto& ents2 = aml::entitlements::EntitlementManager::instance();
        const auto e2 = ents2.snapshot();
        const bool plus2 = ents2.is_plus();

        card_begin("##plan_comparison", ImVec2(-1, 0));
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("PLAN COMPARISON");
        ImGui::PopFont();
        ImGui::Spacing();

        // Feature comparison table
        struct Feature { const char* name; bool free_val; bool plus_val; };
        const Feature features[] = {
            {"Launcher",                          true,  true},
            {"Accounts",                          true,  true},
            {"Java Management",                   true,  true},
            {"Mods & Modpacks",                   true,  true},
            {"Local Servers",                     true,  true},
            {"Basic Essentials",                  true,  true},
            {"Direct P2P",                        true,  true},
            {"Basic TURN Allowance",              true,  true},
            {"No Ads",                            false, true},
            {"80 GB Monthly TURN",                false, true},
            {"Premium Essentials",                false, true},
            {"Cloud Profile Sync",                false, true},
            {"Cloud Settings Sync",               false, true},
            {"Premium Themes",                    false, true},
            {"Advanced Profile Tools",            false, true},
            {"Early Access",                      false, true},
            {"Amalgam+ Badge",                    false, true},
            {"Priority Support",                  false, true},
        };

        const float avail = ImGui::GetContentRegionAvail().x;
        const float col1 = ui_px(240.0f);  // Feature name
        const float col2 = avail * 0.35f;  // Free
        // col3 = rest                        // Amalgam+

        // Header
        ImGui::PushFont(f_bold);
        ImGui::TextColored(k.muted, "Feature");
        ImGui::SameLine(col1);
        ImGui::TextColored(k.muted, "FREE ($0)");
        ImGui::SameLine(col1 + col2);
        ImGui::TextColored(k.brand, "AMALGAM+ ($19.99/mo)");
        ImGui::PopFont();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::PushFont(f_small);
        for (const auto& feat : features) {
            // Highlight current plan's column
            ImVec4 free_color = feat.free_val ? k.green : k.muted;
            ImVec4 plus_color = feat.plus_val ? k.green : k.muted;
            const char* free_text = feat.free_val ? "\xe2\x9c\x93" : "\xe2\x80\x94";
            const char* plus_text = feat.plus_val ? "\xe2\x9c\x93" : "\xe2\x80\x94";

            ImGui::TextColored(k.text, "%s", feat.name);
            ImGui::SameLine(col1);
            ImGui::TextColored(free_color, "%s", free_text);
            ImGui::SameLine(col1 + col2);
            ImGui::TextColored(plus_color, "%s", plus_text);
        }
        ImGui::PopFont();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (!e2.valid || !plus2) {
            if (primary_button(e2.valid ? "Upgrade to Amalgam+" : "View Membership Plans", ImVec2(ui_px(190.0f), ui_px(32.0f)))) {
                ShellExecuteW(st.hwnd, L"open",
                    aml::net::to_wide(aml::online::config().plans_url()).c_str(),
                    nullptr, nullptr, SW_SHOWNORMAL);
            }
        } else {
            ImGui::TextColored(k.green, "You're on Amalgam+ — thank you!");
        }
        card_end();
    }

    ImGui::Spacing();

    // ── TURN Add-ons ──────────────────────────────────────────────
    {
        auto& ents3 = aml::entitlements::EntitlementManager::instance();
        const auto e3 = ents3.snapshot();
        if (ents3.is_plus() && e3.turn_monthly_bytes > 0) {
            card_begin("##turn_addons", ImVec2(-1, 0));
            ImGui::PushFont(f_h2);
            ImGui::TextUnformatted("RELAY DATA ADD-ONS");
            ImGui::PopFont();
            ImGui::TextColored(k.muted,
                               "Additional relay capacity is not activated in the launcher yet.");
            ImGui::TextColored(k.muted,
                               "When metering and billing are available, options will be managed on the website.");
            ImGui::Spacing();
            if (ghost_button("Review Website Plans", ImVec2(ui_px(160.0f), ui_px(30.0f)))) {
                ShellExecuteW(st.hwnd, L"open",
                    aml::net::to_wide(aml::online::config().plans_url()).c_str(),
                    nullptr, nullptr, SW_SHOWNORMAL);
            }
            card_end();
            ImGui::Spacing();
        }
    }

    // Quick actions
    card_begin("##account_actions");
    ImGui::TextUnformatted("Quick Actions");
    ImGui::Separator();

    if (st.account.username.empty() && !account_manager.is_microsoft_linked()) {
        if (ghost_button("Connect Minecraft Account", ImVec2(ui_px(220.0f), ui_px(36.0f)))) {
            start_microsoft_login(st);
        }
        ImGui::SameLine();
        ImGui::TextColored(k.muted, "Optional here; the official launcher handles sign-in at Play.");
    }

    ImGui::Spacing();

    if (ghost_button("Sign Out", ImVec2(ui_px(120.0f), ui_px(32.0f)))) {
        request_amalgam_sign_out(st);
    }

    ImGui::SameLine();
    // Supabase cannot invalidate sessions on other devices from this client.
    // Keep that limitation visible without presenting a dead button; the
    // account website is the authoritative place for remote session control.
    ImGui::TextColored(k.muted, "Other-device sessions are managed on the website.");
    ImGui::SameLine(0, ui_px(8.0f));
    if (ghost_button("Manage Sessions", ImVec2(ui_px(135.0f), ui_px(32.0f)))) {
        ShellExecuteW(st.hwnd, L"open",
            aml::net::to_wide(aml::online::config().page_url("account/sessions")).c_str(),
            nullptr, nullptr, SW_SHOWNORMAL);
    }

    card_end();
}

// ---------------------------------------------------------------------------
// Profile Management
// ---------------------------------------------------------------------------

void draw_account_profile(UiState& st) {
    auto& account_state = get_account_ui_state(st);
    auto& account_manager = aml::account::AccountManager::instance();

    page_title("Profile Settings", "Update your personal information");

    if (!account_state.profile_editing) {
        // Display profile
        auto profile = account_manager.get_profile();

        card_begin("##profile_view");

        ImGui::TextUnformatted("Your Profile");
        ImGui::Separator();
        ImGui::Spacing();

        // Avatar
        ImGui::TextUnformatted("Avatar");
        ImGui::TextColored(k.muted, "Upload or change your profile picture");
        ImGui::Spacing();

        // Basic info
        ImGui::TextUnformatted("Email");
        ImGui::Text("%s", profile.email.c_str());
        ImGui::TextColored(k.muted, "Primary email address - cannot be changed");
        ImGui::Spacing();

        ImGui::TextUnformatted("Username");
        ImGui::Text("%s", profile.username.c_str());
        ImGui::TextColored(k.muted, "Your unique Amalgam username");
        ImGui::Spacing();

        ImGui::TextUnformatted("Display Name");
        ImGui::Text("%s", profile.display_name.empty() ? "Not set" : profile.display_name.c_str());
        ImGui::TextColored(k.muted, "How you appear to other users");
        ImGui::Spacing();

        ImGui::TextUnformatted("Bio");
        ImGui::Text("%s", profile.bio.empty() ? "Not set" : profile.bio.c_str());
        ImGui::Spacing();

        if (ghost_button("Edit Profile", ImVec2(ui_px(120.0f), ui_px(32.0f)))) {
            account_state.editing_profile = profile;
            account_state.profile_editing = true;
        }

        card_end();

        ImGui::Spacing();

        // Account creation info
        card_begin("##account_info");
        ImGui::TextUnformatted("Account Information");
        ImGui::Separator();

        ImGui::TextColored(k.muted, "Account Created");
        ImGui::Text("%s", format_date(profile.created_at).c_str());
        ImGui::Spacing();

        ImGui::TextColored(k.muted, "Last Login");
        ImGui::Text("%s", format_date(profile.last_login_at).c_str());

        card_end();
    } else {
        // Edit profile
        card_begin("##profile_edit");

        ImGui::TextUnformatted("Edit Profile");
        ImGui::Separator();
        ImGui::Spacing();

        // Display Name
        ImGui::TextUnformatted("Display Name");
        ImGui::SetNextItemWidth(ui_px(300.0f));
        input_text("##edit_display_name", &account_state.editing_profile.display_name);
        ImGui::TextColored(k.muted, "How you'll appear to other Amalgam users");
        ImGui::Spacing();

        // Bio
        ImGui::TextUnformatted("Bio");
        ImGui::SetNextItemWidth(ui_px(300.0f));
        ImGui::InputTextMultiline("##edit_bio", &account_state.editing_profile.bio,
                                  ImVec2(ui_px(400.0f), ui_px(100.0f)));
        ImGui::TextColored(k.muted, "Tell other users about yourself");
        ImGui::Spacing();

        // Avatar URL
        ImGui::TextUnformatted("Avatar URL");
        ImGui::SetNextItemWidth(ui_px(300.0f));
        input_text("##edit_avatar_url", &account_state.editing_profile.avatar_url);
        ImGui::TextColored(k.muted, "URL to your profile picture");
        ImGui::Spacing();

        // Error and success messages
        if (!account_state.profile_error.empty()) {
            ImGui::TextColored(k.red, "%s", account_state.profile_error.c_str());
            ImGui::Spacing();
        }

        if (!account_state.profile_success.empty()) {
            ImGui::TextColored(k.green, "%s", account_state.profile_success.c_str());
            ImGui::Spacing();
        }

        const bool account_lane_busy = account_auth_lane_reserved(st);
        if (account_lane_busy) {
            ImGui::TextColored(k.muted,
                               "Another account request is finishing. Saving stays unavailable until it is complete.");
            ImGui::Spacing();
        }

        ImGui::Spacing();
        ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - ui_px(200.0f));

        if (ghost_button("Cancel", ImVec2(ui_px(80.0f), ui_px(32.0f)))) {
            account_state.profile_editing = false;
            account_state.profile_error.clear();
            account_state.profile_success.clear();
        }

        ImGui::SameLine();
        if (primary_button("Save Changes", ImVec2(ui_px(120.0f), ui_px(32.0f)),
                           false, account_lane_busy)) {
            if (account_manager.update_profile(account_state.editing_profile)) {
                account_state.profile_success = "Profile updated successfully!";
                account_state.profile_error.clear();
                account_state.profile_editing = false;
                st.account_passive_cache.mark_values_stale();
            } else {
                account_state.profile_error = "Failed to update profile";
                account_state.profile_success.clear();
            }
        }

        card_end();
    }
}

// ---------------------------------------------------------------------------
// Account Settings
// ---------------------------------------------------------------------------

static std::string normalize_theme_code(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (value == "light" || value == "dark" || value == "system") return value;
    return "system";
}

static std::string normalize_language_code(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (value == "en" || value == "es" || value == "fr" || value == "de" ||
        value == "zh" || value == "ja") return value;
    return "en";
}

void draw_account_settings(UiState& st) {
    auto& account_state = get_account_ui_state(st);
    auto& account_manager = aml::account::AccountManager::instance();

    page_title("Account Settings", "Configure your preferences");

    const auto profile = account_manager.get_profile();
    const std::string profile_identity = profile.email.empty() ? profile.username : profile.email;
    if (!account_state.settings_initialized ||
        account_state.settings_profile_identity != profile_identity) {
        account_state.theme = normalize_theme_code(profile.theme);
        account_state.language = normalize_language_code(profile.language);
        account_state.receive_newsletter = profile.receive_newsletter;
        account_state.enable_beta_features = profile.enable_beta_features;
        if (st.cfg) {
            account_state.social_notifications = st.cfg->social_notifications;
            account_state.social_show_offline = st.cfg->social_show_offline;
            account_state.social_auto_accept = st.cfg->social_auto_accept;
        }
        account_state.settings_profile_identity = profile_identity;
        account_state.settings_initialized = true;
    }

    card_begin("##settings_general");
    ImGui::TextUnformatted("General Settings");
    ImGui::Separator();
    ImGui::Spacing();

    // Theme
    ImGui::TextUnformatted("Theme");
    const char* themes[] = { "System", "Light", "Dark" };
    const char* theme_codes[] = { "system", "light", "dark" };
    int current_theme = 0;
    for (int i = 0; i < 3; ++i) {
        if (account_state.theme == theme_codes[i]) {
            current_theme = i;
            break;
        }
    }

    if (ImGui::BeginCombo("##theme_combo", themes[current_theme])) {
        for (int i = 0; i < 3; ++i) {
            if (ImGui::Selectable(themes[i], current_theme == i)) {
                current_theme = i;
                account_state.theme = theme_codes[i];
            }
        }
        ImGui::EndCombo();
    }
    ImGui::Spacing();

    // Language
    ImGui::TextUnformatted("Language");
    const char* languages[] = { "English", "Spanish", "French", "German", "Chinese", "Japanese" };
    const char* language_codes[] = { "en", "es", "fr", "de", "zh", "ja" };
    int current_language = 0;
    for (int i = 0; i < 6; ++i) {
        if (account_state.language == language_codes[i]) {
            current_language = i;
            break;
        }
    }

    if (ImGui::BeginCombo("##language_combo", languages[current_language])) {
        for (int i = 0; i < 6; ++i) {
            if (ImGui::Selectable(languages[i], current_language == i)) {
                current_language = i;
                account_state.language = language_codes[i];
            }
        }
        ImGui::EndCombo();
    }
    ImGui::Spacing();

    // Newsletter
    ImGui::Checkbox("Receive Newsletter", &account_state.receive_newsletter);
    ImGui::TextColored(k.muted, "Get updates about new features and improvements");
    ImGui::Spacing();

    // Beta features
    ImGui::Checkbox("Enable Beta Features", &account_state.enable_beta_features);
    ImGui::TextColored(k.muted, "Get early access to experimental features");

    card_end();

    ImGui::Spacing();

    card_begin("##settings_notifications");
    ImGui::TextUnformatted("Social Notifications");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Checkbox("Enable friend and party notifications", &account_state.social_notifications);
    ImGui::TextColored(k.muted, "Show friend requests, messages, invites, and party activity.");
    ImGui::Spacing();
    ImGui::Checkbox("Show offline friends", &account_state.social_show_offline);
    ImGui::Spacing();
    ImGui::Checkbox("Automatically accept trusted invites", &account_state.social_auto_accept);
    ImGui::TextColored(k.muted, "Only use this when you understand the invite source.");

    card_end();

    ImGui::Spacing();
    const bool account_lane_busy = account_auth_lane_reserved(st);
    if (account_lane_busy) {
        ImGui::TextColored(k.muted,
                           "Another account request is finishing. Saving stays unavailable until it is complete.");
        ImGui::Spacing();
    }
    ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - ui_px(120.0f));

    if (primary_button("Save Settings", ImVec2(ui_px(120.0f), ui_px(36.0f)),
                       false, account_lane_busy)) {
        account::AccountProfile updated_profile = profile;
        updated_profile.theme = account_state.theme.empty()
            ? normalize_theme_code(profile.theme) : normalize_theme_code(account_state.theme);
        updated_profile.language = account_state.language.empty()
            ? normalize_language_code(profile.language) : normalize_language_code(account_state.language);
        updated_profile.receive_newsletter = account_state.receive_newsletter;
        updated_profile.enable_beta_features = account_state.enable_beta_features;
        if (st.cfg) {
            st.cfg->social_notifications = account_state.social_notifications;
            st.cfg->social_show_offline = account_state.social_show_offline;
            st.cfg->social_auto_accept = account_state.social_auto_accept;
        }

        if (account_manager.update_profile(updated_profile) &&
            (!st.cfg || config::save(st.exe_dir + L"\\launcher.json", *st.cfg))) {
            st.account_passive_cache.mark_values_stale();
            push_notice(st, ui_model::NoticeLevel::Success, "Settings Saved", "Your preferences have been updated");
        } else {
            push_notice(st, ui_model::NoticeLevel::Error, "Save Failed", "Could not save your preferences");
        }
    }
}

// ---------------------------------------------------------------------------
// Security Settings
// ---------------------------------------------------------------------------

void draw_account_security(UiState& st) {
    auto& account_state = get_account_ui_state(st);
    auto& account_manager = aml::account::AccountManager::instance();
    const auto current_user = aml::supabase::SupabaseManager::instance().get_current_user();
    request_account_security(st, current_user.id);
    const auto& security_state = st.account_passive_cache.security_state;
    const auto& security_settings = st.account_passive_cache.security;
    consume_account_password_change(st, account_state);
    const bool password_change_working =
        auth_async_request_is_working(st, kAccountPasswordChangeAction);
    const bool auth_lane_busy = account_auth_lane_reserved(st);

    page_title("Security Settings", "Manage your account security");

    card_begin("##security_password");
    ImGui::TextUnformatted("Password");
    ImGui::Separator();
    ImGui::Spacing();

    if (!account_state.changing_password) {
        ImGui::TextWrapped("Change your account password to keep your account secure.");
        if (!account_state.security_success.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(k.green, "%s", account_state.security_success.c_str());
        }
        ImGui::Spacing();

        if (ghost_button("Change Password", ImVec2(ui_px(150.0f), ui_px(32.0f)))) {
            account_state.changing_password = true;
        }
    } else {
        if (auth_lane_busy) ImGui::BeginDisabled();
        // Current password
        ImGui::TextUnformatted("Current Password");
        ImGui::SetNextItemWidth(ui_px(300.0f));
        ImGui::InputText("##current_password", &account_state.current_password, ImGuiInputTextFlags_Password);
        ImGui::Spacing();

        // New password
        ImGui::TextUnformatted("New Password");
        ImGui::SetNextItemWidth(ui_px(300.0f));
        ImGui::InputText("##new_password", &account_state.new_password, ImGuiInputTextFlags_Password);
        ImGui::Spacing();

        // Confirm password
        ImGui::TextUnformatted("Confirm New Password");
        ImGui::SetNextItemWidth(ui_px(300.0f));
        ImGui::InputText("##confirm_password", &account_state.confirm_password, ImGuiInputTextFlags_Password);
        if (auth_lane_busy) ImGui::EndDisabled();
        ImGui::Spacing();

        // Error message
        if (!account_state.security_error.empty()) {
            ImGui::TextColored(k.red, "%s", account_state.security_error.c_str());
            ImGui::Spacing();
        }

        // Success message
        if (!account_state.security_success.empty()) {
            ImGui::TextColored(k.green, "%s", account_state.security_success.c_str());
            ImGui::Spacing();
        }

        ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - ui_px(200.0f));

        if (ghost_button(password_change_working ? "Cancel request" : "Cancel",
                         ImVec2(ui_px(110.0f), ui_px(32.0f)))) {
            cancel_account_password_change(st);
            account_state.changing_password = false;
            wipe_string(account_state.current_password);
            wipe_string(account_state.new_password);
            wipe_string(account_state.confirm_password);
            wipe_string(account_state.password_request_user_id);
            account_state.password_request_session_generation = 0;
            account_state.security_error.clear();
            account_state.security_success.clear();
        }

        ImGui::SameLine();
        if (password_change_working) {
            primary_button("Changing password...", ImVec2(ui_px(150.0f), ui_px(32.0f)), true, true);
        } else if (auth_lane_busy) {
            primary_button("Change Password", ImVec2(ui_px(150.0f), ui_px(32.0f)), false, true);
        } else if (primary_button("Change Password", ImVec2(ui_px(150.0f), ui_px(32.0f)))) {
            if (account_state.new_password.empty() || account_state.confirm_password.empty()) {
                account_state.security_error = "Please enter and confirm your new password";
            } else if (account_state.new_password != account_state.confirm_password) {
                account_state.security_error = "Passwords do not match";
            } else if (account_state.current_password.empty()) {
                account_state.security_error = "Please enter your current password";
            } else {
                const auto active_user = aml::supabase::SupabaseManager::instance().get_current_user();
                if (active_user.id.empty()) {
                    account_state.security_error =
                        "Your Amalgam session is no longer available. Sign in again, then retry the password change.";
                } else {
                    std::string current_password = account_state.current_password;
                    std::string new_password = account_state.new_password;
                    account_state.password_request_user_id = active_user.id;
                    account_state.password_request_session_generation =
                        aml::supabase::SupabaseManager::instance().session_generation();
                    if (start_auth_async_request(
                            st, kAccountPasswordChangeAction,
                            [current_password, new_password]() mutable {
                                try {
                                    const auto response = aml::supabase::SupabaseManager::instance()
                                                              .change_password(current_password, new_password);
                                    AsyncUiRequestResult result;
                                    result.success = response.success;
                                    result.title = response.success ? "Password changed"
                                                                    : "Password change failed";
                                    result.detail = response.success ? std::string()
                                        : (response.error.empty()
                                            ? "We could not change your password. Please try again."
                                            : response.error);
                                    wipe_string(current_password);
                                    wipe_string(new_password);
                                    return result;
                                } catch (...) {
                                    wipe_string(current_password);
                                    wipe_string(new_password);
                                    throw;
                                }
                            })) {
                        account_state.security_error.clear();
                        account_state.security_success.clear();
                    } else {
                        account_state.security_error =
                            "Another account request is still finishing. Please try again in a moment.";
                    }
                }
            }
        }
        if (auth_lane_busy && !password_change_working) {
            ImGui::Spacing();
            ImGui::TextColored(k.muted,
                               "Another account request is finishing. You can retry when it is complete.");
        }
    }

    card_end();

    ImGui::Spacing();

    card_begin("##security_2fa");
    ImGui::TextUnformatted("Two-Factor Authentication");
    ImGui::SameLine();
    const bool security_refresh_blocked = account_auth_lane_reserved(st);
    if (ghost_button(security_state.loading ? "Refreshing..."
                                             : (!security_state.error.empty() ? "Retry" : "Refresh"),
                     ImVec2(ui_px(92.0f), ui_px(26.0f)), security_refresh_blocked)) {
        request_account_security(st, current_user.id, true);
    }
    ImGui::Separator();
    ImGui::Spacing();

    if (security_state.loading) {
        ImGui::TextColored(k.muted, security_state.has_value
            ? "Refreshing account security details..."
            : "Loading account security details...");
    } else if (!security_state.error.empty()) {
        ImGui::TextColored(k.red, "%s", security_state.error.c_str());
        ImGui::TextColored(k.muted,
                           "No security status is being inferred while the account service is unavailable.");
    } else if (!security_state.warning.empty()) {
        ImGui::TextColored(k.orange, "%s", security_state.warning.c_str());
    } else if (account_passive_cache_is_stale(security_state)) {
        ImGui::TextColored(k.muted,
                           "Showing cached security details. Refresh when you want the latest account status.");
    } else if (!security_state.has_value && security_refresh_blocked) {
        ImGui::TextColored(k.muted,
                           "Waiting for the current account request to finish before loading security details.");
    }

    if (security_state.has_value && security_settings.two_factor_enabled) {
        ImGui::TextColored(k.green, "✓ Two-Factor Authentication is enabled");
        ImGui::TextColored(k.muted, "Method: %s", security_settings.two_factor_method.c_str());
    } else if (security_state.has_value) {
        ImGui::TextWrapped("Two-factor authentication is not enabled for this account.");
    }
    ImGui::Spacing();
    ImGui::TextColored(k.muted,
                       "Enrollment and removal are managed by the account portal so this launcher never presents an unfinished security action.");
    if (ghost_button("Open Account Security", ImVec2(ui_px(180.0f), ui_px(32.0f)))) {
        ShellExecuteW(st.hwnd, L"open",
            aml::net::to_wide(aml::online::config().page_url("account/security")).c_str(),
            nullptr, nullptr, SW_SHOWNORMAL);
    }

    card_end();

    ImGui::Spacing();

    card_begin("##security_sessions");
    ImGui::TextUnformatted("Remembered Local Sign-ins");
    ImGui::Separator();
    ImGui::Spacing();

    auto sessions = account_manager.get_all_sessions();

    if (sessions.empty()) {
        ImGui::TextColored(k.muted, "No remembered local sign-ins");
    } else {
        for (const auto& session : sessions) {
            ImGui::Text("%s", session.email.c_str());
            ImGui::SameLine();
            ImGui::TextColored(k.muted, " - %s", format_date(session.created_at).c_str());
            ImGui::SameLine();

            if (session.id != account_manager.get_current_session().id) {
                if (ghost_button("Remove", ImVec2(ui_px(80.0f), ui_px(24.0f)))) {
                    request_remove_local_session(st, session.id);
                }
            } else {
                ImGui::TextColored(k.muted, "(Current)");
            }
        }
    }

    card_end();
}

// ---------------------------------------------------------------------------
// Session Management
// ---------------------------------------------------------------------------

void draw_account_sessions(UiState& st) {
    auto& account_manager = aml::account::AccountManager::instance();

    page_title("Remembered Local Sign-ins", "Manage Amalgam sign-ins stored on this Windows device");

    auto sessions = account_manager.get_all_sessions();
    auto current_session = account_manager.get_current_session();

    card_begin("##sessions_list");
    ImGui::TextUnformatted("Stored on this Windows device");
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextColored(k.muted,
                       "This list is local to this launcher. It is not a list of other devices signed in to Amalgam.");
    ImGui::Spacing();

    if (sessions.empty()) {
        empty_state("No Remembered Sign-ins", "This Windows device has no saved Amalgam sign-ins.", "S");
    } else {
        for (const auto& session : sessions) {
            ImGui::PushID(session.id.c_str());

            ImGui::BeginGroup();

            // Session info
            ImGui::Text("%s", session.email.c_str());
            ImGui::TextColored(k.muted, "Session ID: %s", session.id.substr(0, 8).c_str());
            ImGui::TextColored(k.muted, "Created: %s", format_date(session.created_at).c_str());
            ImGui::TextColored(k.muted, "Last Used: %s", format_date(session.last_used).c_str());

            // Session status
            if (session.id == current_session.id) {
                ImGui::TextColored(k.green, "✓ Current Session");
            } else if (session.is_expired()) {
                ImGui::TextColored(k.red, "✗ Expired");
            } else {
                ImGui::TextColored(k.muted, "Active");
            }

            ImGui::EndGroup();
            ImGui::SameLine();

            // Actions
            ImGui::BeginGroup();
            if (session.id != current_session.id) {
                if (ghost_button("Remove", ImVec2(ui_px(100.0f), ui_px(28.0f)))) {
                    request_remove_local_session(st, session.id);
                }
            }
            ImGui::EndGroup();

            ImGui::Separator();
            ImGui::PopID();
        }
    }

    card_end();
}

// ---------------------------------------------------------------------------
// Account Activity
// ---------------------------------------------------------------------------

void draw_account_activity(UiState& st) {
    const auto current_user = aml::supabase::SupabaseManager::instance().get_current_user();
    request_account_activity(st, current_user.id);
    const auto& activity_state = st.account_passive_cache.activity_state;
    const auto& activities = st.account_passive_cache.activity;

    page_title("Account Activity", "View your recent account activity");

    card_begin("##activity_list");
    ImGui::TextUnformatted("Recent Activity");
    ImGui::SameLine();
    const bool activity_refresh_blocked = account_auth_lane_reserved(st);
    if (ghost_button(activity_state.loading ? "Refreshing..."
                                             : (!activity_state.error.empty() ? "Retry" : "Refresh"),
                     ImVec2(ui_px(92.0f), ui_px(26.0f)), activity_refresh_blocked)) {
        request_account_activity(st, current_user.id, true);
    }
    ImGui::Separator();
    ImGui::Spacing();

    if (activity_state.loading) {
        ImGui::TextColored(k.muted, activity_state.has_value
            ? "Refreshing account activity..."
            : "Loading account activity...");
        ImGui::Spacing();
    } else if (!activity_state.error.empty()) {
        ImGui::TextColored(k.red, "%s", activity_state.error.c_str());
        ImGui::Spacing();
        if (!activity_state.has_value) {
            empty_state("Activity Unavailable",
                        "We could not load account activity. Use Retry when the account service is available.",
                        "A");
        }
    } else if (!activity_state.warning.empty()) {
        ImGui::TextColored(k.orange, "%s", activity_state.warning.c_str());
        ImGui::Spacing();
    } else if (account_passive_cache_is_stale(activity_state)) {
        ImGui::TextColored(k.muted,
                           "Showing cached activity. Refresh when you want the latest account events.");
        ImGui::Spacing();
    } else if (!activity_state.has_value && activity_refresh_blocked) {
        ImGui::TextColored(k.muted,
                           "Waiting for the current account request to finish before loading activity.");
        ImGui::Spacing();
    }

    if (activity_state.has_value && activities.empty()) {
        empty_state("No Recent Activity", "Your account activity will appear here.", "A");
    } else if (activity_state.has_value) {
        for (const auto& activity : activities) {
            ImGui::PushID(activity.id.c_str());

            ImGui::BeginGroup();
            ImGui::Text("%s", activity.type.c_str());
            ImGui::TextColored(k.muted, "%s", activity.description.c_str());
            ImGui::TextColored(k.muted, "%s", format_date(activity.timestamp).c_str());
            ImGui::EndGroup();

            ImGui::Separator();
            ImGui::PopID();
        }
    }

    card_end();
}

// ---------------------------------------------------------------------------
// Visual-fixture account facade
// ---------------------------------------------------------------------------

// Snapshot fixtures deliberately do not hydrate AccountManager or Supabase.
// That keeps visual QA deterministic and prevents a capture run from exposing
// the signed-in user's profile, sessions, entitlements, or activity history.
static int fixture_account_tab_for_case(std::string_view fixture_case) {
    if (fixture_case == "account-overview") return 0;
    if (fixture_case == "account-profile" || fixture_case == "account-profile-edit" ||
        fixture_case == "account-profile-edit-error") return 1;
    if (fixture_case == "account-settings") return 2;
    if (fixture_case == "account-security" ||
        fixture_case == "account-security-password-validation" ||
        fixture_case == "account-security-password-working" ||
        fixture_case == "account-security-password-error") return 3;
    if (fixture_case == "account-sessions") return 4;
    if (fixture_case == "account-activity") return 5;
    return -1;
}

static void draw_fixture_account_caption() {
    ImGui::TextColored(k.muted,
                       "Visual QA fixture \xe2\x80\x94 local sample data only; no account data is loaded.");
    ImGui::Spacing();
}

static void draw_fixture_account_avatar(float diameter = ui_px(72.0f)) {
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float radius = diameter * 0.5f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddCircleFilled(origin + ImVec2(radius, radius), radius, c32(k.brand));
    dl->AddCircle(origin + ImVec2(radius, radius), radius, c32(k.brand_hov), 0, ui_px(1.0f));
    draw_brand_mark(dl, origin + ImVec2(radius, radius), ui_px(0.45f));
    ImGui::Dummy(ImVec2(diameter, diameter));
}

static void draw_fixture_account_tabs(int active_tab) {
    const char* tabs[] = {"Overview", "Profile", "Settings", "Security", "Sessions", "Activity"};
    const float tab_gap = ui_px(6.0f);
    for (int i = 0; i < 6; ++i) {
        if (i) ImGui::SameLine(0, tab_gap);
        const bool active = active_tab == i;
        const ImVec2 label_size = ImGui::CalcTextSize(tabs[i]);
        const float pad_x = ui_px(14.0f);
        const float tab_h = ui_px(34.0f);
        const ImVec2 tab_size(label_size.x + pad_x * 2.0f, tab_h);
        const ImVec2 tab_min = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(tab_min, tab_min + tab_size,
                          c32(active ? k.surface2 : ImVec4(0, 0, 0, 0)), ui_px(8.0f));
        if (active) {
            dl->AddRect(tab_min, tab_min + tab_size, c32(k.brand), ui_px(8.0f), 0, ui_px(1.5f));
        }
        ImGui::InvisibleButton((std::string("##fixture_account_tab_") + std::to_string(i)).c_str(), tab_size);
        if (ImGui::IsItemHovered() && !active) {
            dl->AddRect(tab_min, tab_min + tab_size, c32(k.border), ui_px(8.0f), 0, ui_px(1.0f));
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
        ImGui::PushFont(active ? f_bold : f_body);
        dl->AddText(tab_min + ImVec2(pad_x, (tab_h - ImGui::GetTextLineHeight()) * 0.5f),
                    c32(active ? k.text : k.muted), tabs[i]);
        ImGui::PopFont();
    }
}

static void draw_fixture_account_row(const char* label, const char* value,
                                     const char* detail = nullptr,
                                     const ImVec4* value_color = nullptr) {
    ImGui::TextColored(k.muted, "%s", label);
    ImGui::TextColored(value_color ? *value_color : k.text, "%s", value);
    if (detail && detail[0]) {
        ImGui::TextColored(k.muted, "%s", detail);
    }
    ImGui::Spacing();
}

static void draw_fixture_account_overview(UiState& /*st*/) {
    page_title("Account Overview", "Manage your Amalgam account and preferences");
    draw_fixture_account_caption();

    card_begin("##fixture_account_identity");
    const bool compact = ImGui::GetContentRegionAvail().x < ui_px(760.0f);
    ImGui::BeginGroup();
    draw_fixture_account_avatar();
    ImGui::EndGroup();
    if (!compact) ImGui::SameLine(0, ui_px(18.0f));
    else ImGui::Spacing();

    ImGui::BeginGroup();
    ImGui::PushFont(f_title);
    ImGui::TextUnformatted("Alex Morgan");
    ImGui::PopFont();
    ImGui::TextColored(k.muted, "preview@amalgam.test");
    ImGui::Spacing();
    ImGui::TextColored(k.green, "\xe2\x9c\x93  Amalgam account connected");
    ImGui::TextColored(k.muted, "Minecraft sign-in stays with the official launcher.");
    ImGui::EndGroup();

    if (!compact) ImGui::SameLine(0, ui_px(28.0f));
    else ImGui::Spacing();
    ImGui::BeginGroup();
    if (ghost_button("Edit Profile", ImVec2(ui_px(128.0f), ui_px(32.0f)))) {}
    if (ghost_button("Account Settings", ImVec2(ui_px(128.0f), ui_px(32.0f)))) {}
    ImGui::EndGroup();
    card_end();

    ImGui::Spacing();
    card_begin("##fixture_account_stats");
    ImGui::TextUnformatted("Account at a glance");
    ImGui::Separator();
    ImGui::Spacing();
    const float available = ImGui::GetContentRegionAvail().x;
    const float gap = ui_px(10.0f);
    const int columns = ui_model::stat_card_columns(available, g_ui_scale);
    const float width = std::max(ui_px(120.0f),
        (available - gap * static_cast<float>(columns - 1)) / static_cast<float>(columns));
    struct FixtureStat { const char* label; const char* value; ImVec4 accent; };
    const FixtureStat stats[] = {
        {"ACTIVE SESSIONS", "3", k.brand},
        {"MEMBER SINCE", "JAN 2026", k.blue},
        {"LAST SIGN-IN", "TODAY", k.green},
        {"CURRENT PLAN", "AMALGAM+", k.orange},
    };
    for (int i = 0; i < 4; ++i) {
        if (i > 0 && i % columns != 0) ImGui::SameLine(0, gap);
        draw_stat_card(stats[i].label, stats[i].value, -1.0f, stats[i].accent, width);
    }
    card_end();

    ImGui::Spacing();
    card_begin("##fixture_account_membership");
    const ImVec2 membership_min = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(membership_min,
                      ImVec2(membership_min.x + ImGui::GetContentRegionAvail().x,
                             membership_min.y + ui_px(4.0f)),
                      c32(k.brand), ui_px(2.0f));
    ImGui::Dummy(ImVec2(0, ui_px(8.0f)));
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted("MEMBERSHIP & RELAY");
    ImGui::PopFont();
    ImGui::Spacing();
    ImGui::PushFont(f_title);
    ImGui::TextUnformatted("Amalgam+");
    ImGui::PopFont();
    ImGui::SameLine(0, ui_px(10.0f));
    ImGui::TextColored(k.green, "ACTIVE");
    ImGui::TextColored(k.muted, "Your next renewal is October 12.");
    ImGui::Spacing();
    ImGui::TextUnformatted("Essentials relay");
    ImGui::TextColored(k.muted, "54 GB of 80 GB used this month");
    progress_bar(0.675f, ImVec2(std::min(ui_px(420.0f), ImGui::GetContentRegionAvail().x), ui_px(12.0f)),
                 nullptr, &k.green);
    ImGui::Spacing();
    ImGui::TextColored(k.muted, "1 cloud server linked  \xc2\xb7  Resets October 12");
    ImGui::Spacing();
    if (ghost_button("Manage membership", ImVec2(ui_px(162.0f), ui_px(32.0f)))) {}
    ImGui::SameLine(0, ui_px(8.0f));
    if (ghost_button("View cloud servers", ImVec2(ui_px(162.0f), ui_px(32.0f)))) {}
    card_end();

    ImGui::Spacing();
    card_begin("##fixture_account_safety");
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted("ACCOUNT SAFETY");
    ImGui::PopFont();
    ImGui::Spacing();
    ImGui::TextColored(k.green, "\xe2\x9c\x93  Two-factor authentication is enabled");
    ImGui::TextColored(k.muted, "Authenticator app · Recovery methods verified");
    ImGui::Spacing();
    if (ghost_button("Review security", ImVec2(ui_px(150.0f), ui_px(32.0f)))) {}
    card_end();
}

static void draw_fixture_account_profile(UiState& st) {
    const bool editing = st.fixture_case == "account-profile-edit" ||
        st.fixture_case == "account-profile-edit-error";
    if (editing) {
        const bool has_error = st.fixture_case == "account-profile-edit-error";
        page_title("Edit Profile", "Update how other Amalgam players recognize you");
        draw_fixture_account_caption();

        card_begin("##fixture_profile_edit", ImVec2(-1, 0));
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("PROFILE DETAILS");
        ImGui::PopFont();
        ImGui::Spacing();
        ImGui::TextColored(k.brand_hov,
                           "Visual QA fixture — sample fields are local and no profile update is sent.");
        ImGui::Spacing();

        std::string display_name = "Alex Morgan";
        std::string bio = "Building worlds, sharing discoveries, and keeping every profile tidy.";
        std::string avatar_url = "https://preview.invalid/avatar.png";
        ImGui::BeginDisabled();
        ImGui::TextUnformatted("Display Name");
        ImGui::SetNextItemWidth(std::min(ui_px(360.0f), ImGui::GetContentRegionAvail().x));
        input_text("##fixture_edit_display_name", &display_name);
        ImGui::TextColored(k.muted, "How you appear to other Amalgam users");
        ImGui::Spacing();
        ImGui::TextUnformatted("About");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextMultiline("##fixture_edit_bio", &bio, ImVec2(-1, ui_px(82.0f)));
        ImGui::TextColored(k.muted, "A short introduction shown on your profile");
        ImGui::Spacing();
        ImGui::TextUnformatted("Avatar URL");
        ImGui::SetNextItemWidth(-1);
        input_text("##fixture_edit_avatar_url", &avatar_url);
        ImGui::EndDisabled();

        if (has_error) {
            ImGui::Spacing();
            ImGui::PushFont(f_bold);
            ImGui::TextColored(k.red, "Profile update needs attention");
            ImGui::PopFont();
            ImGui::TextColored(k.red,
                               "The profile could not be saved. Your current public profile was not changed.");
            ImGui::TextColored(k.muted,
                               "Review the details and retry when your account connection is available.");
        }
        ImGui::Spacing();
        ghost_button("Cancel", ImVec2(ui_px(96.0f), ui_px(32.0f)), true);
        ImGui::SameLine(0, ui_px(8.0f));
        primary_button(has_error ? "Retry Save Changes" : "Save Changes",
                       ImVec2(ui_px(158.0f), ui_px(32.0f)), false, true);
        card_end();
        return;
    }

    page_title("Profile Settings", "Update the information other Amalgam players see");
    draw_fixture_account_caption();

    card_begin("##fixture_profile_identity");
    draw_fixture_account_avatar(ui_px(64.0f));
    ImGui::SameLine(0, ui_px(16.0f));
    ImGui::BeginGroup();
    ImGui::PushFont(f_title);
    ImGui::TextUnformatted("Alex Morgan");
    ImGui::PopFont();
    ImGui::TextColored(k.muted, "@visual-guide");
    ImGui::TextColored(k.green, "Profile complete");
    ImGui::EndGroup();
    ImGui::SameLine(0, ui_px(28.0f));
    if (ghost_button("Edit Profile", ImVec2(ui_px(128.0f), ui_px(32.0f)))) {}
    card_end();

    ImGui::Spacing();
    const bool split = ImGui::GetContentRegionAvail().x >= ui_px(760.0f);
    const float gap = ui_px(12.0f);
    const float column_width = split
        ? (ImGui::GetContentRegionAvail().x - gap) * 0.5f
        : ImGui::GetContentRegionAvail().x;

    card_begin("##fixture_profile_details", ImVec2(column_width, 0));
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted("YOUR PROFILE");
    ImGui::PopFont();
    ImGui::Spacing();
    draw_fixture_account_row("EMAIL", "preview@amalgam.test", "Primary email address");
    draw_fixture_account_row("USERNAME", "visual-guide", "Your unique Amalgam handle");
    draw_fixture_account_row("DISPLAY NAME", "Alex Morgan", "Shown to friends and parties");
    draw_fixture_account_row("ABOUT", "Building worlds, sharing discoveries, and keeping every profile tidy.");
    card_end();

    if (split) ImGui::SameLine(0, gap);
    else ImGui::Spacing();
    card_begin("##fixture_profile_account", ImVec2(column_width, 0));
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted("ACCOUNT INFORMATION");
    ImGui::PopFont();
    ImGui::Spacing();
    draw_fixture_account_row("ACCOUNT CREATED", "January 12, 2026");
    draw_fixture_account_row("LAST SIGN-IN", "Today at 10:42 AM");
    draw_fixture_account_row("PROFILE VISIBILITY", "Friends", "Only approved friends can see your profile details.");
    ImGui::Spacing();
    if (ghost_button("Privacy controls", ImVec2(ui_px(150.0f), ui_px(32.0f)))) {}
    card_end();
}

static void draw_fixture_account_settings(UiState& /*st*/) {
    page_title("Account Settings", "Choose how the launcher feels and communicates");
    draw_fixture_account_caption();

    static int theme = 0;
    static int language = 0;
    static bool newsletter = true;
    static bool beta_features = false;
    static bool social_notifications = true;
    static bool show_offline = true;
    static bool auto_accept = false;

    card_begin("##fixture_account_preferences");
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted("PERSONALIZATION");
    ImGui::PopFont();
    ImGui::Spacing();
    ImGui::TextUnformatted("Theme");
    ImGui::TextColored(k.muted, "Use your system setting, or choose a launcher theme.");
    const char* themes[] = {"System", "Light", "Dark"};
    ImGui::SetNextItemWidth(std::min(ui_px(260.0f), ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo("##fixture_account_theme", themes[theme])) {
        for (int i = 0; i < 3; ++i) {
            if (ImGui::Selectable(themes[i], theme == i)) theme = i;
        }
        ImGui::EndCombo();
    }
    ImGui::Spacing();
    ImGui::TextUnformatted("Language");
    ImGui::TextColored(k.muted, "Select the language used by account and social features.");
    const char* languages[] = {"English", "Spanish", "French", "German", "Japanese"};
    ImGui::SetNextItemWidth(std::min(ui_px(260.0f), ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo("##fixture_account_language", languages[language])) {
        for (int i = 0; i < 5; ++i) {
            if (ImGui::Selectable(languages[i], language == i)) language = i;
        }
        ImGui::EndCombo();
    }
    ImGui::Spacing();
    ImGui::Checkbox("Receive launcher and product updates", &newsletter);
    ImGui::TextColored(k.muted, "Occasional news about releases, improvements, and account changes.");
    ImGui::Spacing();
    ImGui::Checkbox("Join optional preview features", &beta_features);
    ImGui::TextColored(k.muted, "Preview features may change before their public release.");
    card_end();

    ImGui::Spacing();
    card_begin("##fixture_account_social_preferences");
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted("SOCIAL & INVITES");
    ImGui::PopFont();
    ImGui::Spacing();
    ImGui::Checkbox("Enable friend and party notifications", &social_notifications);
    ImGui::TextColored(k.muted, "Show requests, messages, invites, and party activity.");
    ImGui::Spacing();
    ImGui::Checkbox("Show offline friends", &show_offline);
    ImGui::TextColored(k.muted, "Keep the full friends list visible when people go offline.");
    ImGui::Spacing();
    ImGui::Checkbox("Automatically accept trusted invites", &auto_accept);
    ImGui::TextColored(k.muted, "Use this only for people and communities you trust.");
    card_end();

    ImGui::Spacing();
    ImGui::TextColored(k.muted, "Preview controls stay local to this visual fixture and are never saved.");
    ImGui::SetCursorPosX(std::max(0.0f, ImGui::GetContentRegionAvail().x - ui_px(148.0f)));
    primary_button("Save preferences", ImVec2(ui_px(148.0f), ui_px(36.0f)), false, true);
}

static void draw_fixture_account_security(UiState& st) {
    const bool password_validation =
        st.fixture_case == "account-security-password-validation";
    const bool password_working =
        st.fixture_case == "account-security-password-working";
    const bool password_error =
        st.fixture_case == "account-security-password-error";
    if (password_validation || password_working || password_error) {
        page_title("Change Password", "Use a unique password you do not reuse elsewhere");
        draw_fixture_account_caption();

        card_begin("##fixture_account_password_change", ImVec2(-1, 0));
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("CHANGE PASSWORD");
        ImGui::PopFont();
        ImGui::Spacing();
        ImGui::TextColored(k.brand_hov,
                           "Visual QA fixture — redacted local sample values only; no password request is sent.");
        ImGui::Spacing();

        std::string current_password = "fixture-current-password";
        std::string new_password = password_validation ? "new-password" : "fixture-new-password";
        std::string confirm_password = password_validation ? "different-password" : "fixture-new-password";
        ImGui::BeginDisabled();
        ImGui::TextUnformatted("Current Password");
        ImGui::SetNextItemWidth(std::min(ui_px(360.0f), ImGui::GetContentRegionAvail().x));
        ImGui::InputText("##fixture_current_password", &current_password,
                         ImGuiInputTextFlags_Password);
        ImGui::Spacing();
        ImGui::TextUnformatted("New Password");
        ImGui::SetNextItemWidth(std::min(ui_px(360.0f), ImGui::GetContentRegionAvail().x));
        ImGui::InputText("##fixture_new_password", &new_password,
                         ImGuiInputTextFlags_Password);
        ImGui::Spacing();
        ImGui::TextUnformatted("Confirm New Password");
        ImGui::SetNextItemWidth(std::min(ui_px(360.0f), ImGui::GetContentRegionAvail().x));
        ImGui::InputText("##fixture_confirm_password", &confirm_password,
                         ImGuiInputTextFlags_Password);
        ImGui::EndDisabled();

        ImGui::Spacing();
        if (password_validation) {
            ImGui::PushFont(f_bold);
            ImGui::TextColored(k.red, "Passwords do not match");
            ImGui::PopFont();
            ImGui::TextColored(k.muted,
                               "Enter the same new password in both fields before continuing.");
        } else if (password_working) {
            ImGui::PushFont(f_bold);
            ImGui::TextColored(k.yellow, "Changing password…");
            ImGui::PopFont();
            ImGui::TextColored(k.muted,
                               "The secure request is in progress. Keep this window open until it finishes.");
        } else {
            ImGui::PushFont(f_bold);
            ImGui::TextColored(k.red, "Password change failed");
            ImGui::PopFont();
            ImGui::TextColored(k.red,
                               "The password could not be changed. Your current password remains active.");
            ImGui::TextColored(k.muted,
                               "Check your connection and try again; Amalgam never stores this password in the fixture.");
        }

        ImGui::Spacing();
        ghost_button(password_working ? "Cancel request" : "Cancel",
                     ImVec2(ui_px(126.0f), ui_px(32.0f)), true);
        ImGui::SameLine(0, ui_px(8.0f));
        primary_button(password_working ? "Changing password…" :
                       (password_error ? "Retry Change Password" : "Change Password"),
                       ImVec2(ui_px(188.0f), ui_px(32.0f)), password_working, true);
        card_end();
        return;
    }

    page_title("Security Settings", "Keep your Amalgam account protected");
    draw_fixture_account_caption();

    card_begin("##fixture_account_password");
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted("PASSWORD");
    ImGui::PopFont();
    ImGui::Spacing();
    ImGui::TextUnformatted("Password protected");
    ImGui::TextColored(k.muted, "Last updated 2 months ago · Use a unique password you do not reuse elsewhere.");
    ImGui::Spacing();
    if (ghost_button("Change password", ImVec2(ui_px(150.0f), ui_px(32.0f)))) {}
    card_end();

    ImGui::Spacing();
    card_begin("##fixture_account_2fa");
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted("TWO-FACTOR AUTHENTICATION");
    ImGui::PopFont();
    ImGui::Spacing();
    ImGui::TextColored(k.muted, "Status is managed by the Amalgam account portal.");
    ImGui::TextColored(k.muted, "This launcher does not present incomplete enrollment or recovery controls.");
    ImGui::Spacing();
    if (ghost_button("Open Account Security", ImVec2(ui_px(180.0f), ui_px(32.0f)))) {}
    card_end();

    ImGui::Spacing();
    card_begin("##fixture_account_security_summary");
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted("SECURITY SUMMARY");
    ImGui::PopFont();
    ImGui::Spacing();
    draw_fixture_account_row("TRUSTED DEVICES", "2 devices", "This Windows desktop and one other desktop");
    draw_fixture_account_row("REMEMBERED LOCAL SIGN-INS", "3 saved accounts", "Stored on this Windows device only");
    draw_fixture_account_row("LAST SECURITY EVENT", "Account security status reviewed", "Today at 10:40 AM", &k.green);
    card_end();
}

static void draw_fixture_account_session(const char* title, const char* subtitle,
                                         const char* detail, bool current, int id) {
    ImGui::PushID(id);
    card_begin("##fixture_account_session", ImVec2(-1, 0));
    ImGui::TextUnformatted(title);
    ImGui::TextColored(k.muted, "%s", subtitle);
    ImGui::TextColored(k.muted, "%s", detail);
    ImGui::Spacing();
    if (current) {
        ImGui::TextColored(k.green, "\xe2\x9c\x93  This device");
    } else {
        if (ghost_button("Remove", ImVec2(ui_px(112.0f), ui_px(30.0f)))) {}
    }
    card_end();
    ImGui::PopID();
}

static void draw_fixture_account_sessions(UiState& /*st*/) {
    page_title("Remembered Local Sign-ins", "Review Amalgam sign-ins stored on this Windows device");
    draw_fixture_account_caption();

    card_begin("##fixture_account_sessions_summary");
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted("REMEMBERED LOCAL SIGN-INS");
    ImGui::PopFont();
    ImGui::Spacing();
    ImGui::TextColored(k.muted, "Three saved launcher sign-ins · removing one affects this Windows device only.");
    card_end();

    ImGui::Spacing();
    draw_fixture_account_session("Primary account", "Amalgam Launcher · protected local session", "Last used just now", true, 0);
    ImGui::Spacing();
    draw_fixture_account_session("Creator account", "Amalgam Launcher · protected local session", "Last used 2 days ago", false, 1);
    ImGui::Spacing();
    draw_fixture_account_session("Test account", "Amalgam Launcher · protected local session", "Last used September 18", false, 2);
}

// Deterministic, local-only confirmation states used by the visual matrix.
// They intentionally do not construct AccountManager, Supabase, or auth
// objects: screenshot evidence must never inspect a reviewer's credentials.
static void draw_fixture_account_action_dialog(UiState& st) {
    const std::string& fixture = st.fixture_case;
    const bool remove_all = fixture == "dialog-remove-local-accounts" ||
        fixture == "dialog-remove-local-accounts-ready";
    const bool ready = fixture == "dialog-remove-local-accounts-ready";
    const bool disconnect = fixture == "dialog-minecraft-disconnect";
    const bool remove_one = fixture == "dialog-remove-local-remembered-session";
    const bool stale = fixture == "dialog-account-action-state-changed";
    const bool local_error = fixture == "dialog-account-action-local-error";
    const bool dangerous = remove_all || disconnect;
    const char* title = disconnect ? "Disconnect Minecraft from this device" :
        (remove_all ? "Remove all remembered local accounts" :
         (remove_one ? "Remove remembered local sign-in" :
          "Sign out of Amalgam on this device"));
    const char* action_label = disconnect ? "Disconnect Minecraft" :
        (remove_all ? "Remove local accounts" :
         (remove_one ? "Remove local sign-in" : "Sign out on this device"));
    const char* account_label = disconnect ? "Alex Morgan" :
        (remove_all ? "3 remembered local accounts" : "alex@amalgam-mc.com");

    constexpr const char* kFixturePopup = "Fixture account action##fixture_account_confirm_action";
    ImGui::OpenPopup(kFixturePopup);
    ImGui::SetNextWindowSize(ImVec2(ui_px(540.0f), 0), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(kFixturePopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

    ImGui::PushFont(f_h2);
    ImGui::TextColored(dangerous ? k.red : k.text, "%s", title);
    ImGui::PopFont();
    ImGui::TextColored(k.muted, "Account: %s", account_label);
    ImGui::Spacing();
    if (disconnect) {
        ImGui::TextWrapped("This removes the saved Minecraft sign-in from this Windows device. It does not sign you out of the official Minecraft Launcher, Microsoft browser sessions, or other devices.");
    } else if (remove_all) {
        ImGui::TextWrapped("This removes every locally remembered Amalgam sign-in from this Windows device. It does not remotely revoke sessions on other devices.");
        ImGui::Spacing();
        ImGui::TextColored(k.red, "Type REMOVE LOCAL ACCOUNTS to enable this action.");
        std::string phrase = ready ? "REMOVE LOCAL ACCOUNTS" : "";
        ImGui::SetNextItemWidth(-1);
        input_text_hint("##fixture_remove_local_accounts_phrase", "REMOVE LOCAL ACCOUNTS", &phrase);
    } else if (remove_one) {
        ImGui::TextWrapped("This removes a locally remembered Amalgam sign-in from this Windows device only. It does not sign that account out on another device.");
    } else {
        ImGui::TextWrapped("This signs the active Amalgam account out on this Windows device. Other devices are not affected, and Minecraft remains connected.");
    }
    if (stale) {
        ImGui::Spacing();
        ImGui::TextColored(k.red, "Your account state changed. Review the action again before continuing.");
    } else if (local_error) {
        ImGui::Spacing();
        ImGui::TextColored(k.red, "The protected local session could not be removed. You remain signed in here; no remote sign-out was attempted.");
    }
    ImGui::Spacing();
    if (ghost_button("Cancel", ImVec2(ui_px(136.0f), ui_px(34.0f)))) {}
    ImGui::SameLine();
    const bool disabled = (remove_all && !ready) || stale || local_error;
    if (dangerous) {
        if (danger_button(action_label, ImVec2(ui_px(220.0f), ui_px(34.0f)), disabled)) {}
    } else {
        if (primary_button(action_label, ImVec2(ui_px(220.0f), ui_px(34.0f)), false, disabled)) {}
    }
    ImGui::EndPopup();
}

static void draw_fixture_account_activity_row(const char* title, const char* detail,
                                              const char* timestamp, const ImVec4& accent, int id) {
    ImGui::PushID(id);
    card_begin("##fixture_account_activity", ImVec2(-1, 0));
    const ImVec2 dot = ImGui::GetCursorScreenPos() + ImVec2(ui_px(6.0f), ui_px(8.0f));
    ImGui::GetWindowDrawList()->AddCircleFilled(dot, ui_px(5.0f), c32(accent));
    ImGui::Dummy(ImVec2(ui_px(16.0f), ui_px(16.0f)));
    ImGui::SameLine(0, ui_px(8.0f));
    ImGui::BeginGroup();
    ImGui::TextUnformatted(title);
    ImGui::TextColored(k.muted, "%s", detail);
    ImGui::TextColored(k.muted, "%s", timestamp);
    ImGui::EndGroup();
    card_end();
    ImGui::PopID();
}

static void draw_fixture_account_activity(UiState& /*st*/) {
    page_title("Account Activity", "Review meaningful changes and security events");
    draw_fixture_account_caption();

    card_begin("##fixture_account_activity_summary");
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted("RECENT ACTIVITY");
    ImGui::PopFont();
    ImGui::Spacing();
    ImGui::TextColored(k.muted, "Activity is private to this account. Events appear here after they are verified.");
    card_end();

    ImGui::Spacing();
    draw_fixture_account_activity_row("Two-factor authentication verified",
                                      "Authenticator app confirmed for this Windows desktop.",
                                      "Today at 10:40 AM", k.green, 0);
    ImGui::Spacing();
    draw_fixture_account_activity_row("Session started",
                                      "Amalgam Launcher signed in on this Windows desktop.",
                                      "Today at 10:37 AM", k.brand, 1);
    ImGui::Spacing();
    draw_fixture_account_activity_row("Profile preferences updated",
                                      "Language and social notification settings were reviewed.",
                                      "September 20", k.blue, 2);
    ImGui::Spacing();
    draw_fixture_account_activity_row("Recovery methods verified",
                                      "Backup recovery methods are available for your account.",
                                      "September 12", k.orange, 3);
}

static void draw_fixture_account_portal(UiState& st) {
    // This mirrors the signed-out portal without opening auth flows. A visual
    // capture must stay read-only even when a tester clicks inside the window.
    draw_breadcrumbs({"Home", "Account"});
    page_title("Your Amalgam account",
               "One identity for launcher preferences, friends, profiles, and supported sync.");

    const float hero_height = std::clamp(ImGui::GetContentRegionAvail().x * 0.27f,
                                         ui_px(250.0f), ui_px(340.0f));
    card_begin("##fixture_account_signed_out", ImVec2(-1, hero_height));
    const ImVec2 hero_pos = ImGui::GetCursorScreenPos();
    const ImVec2 hero_size(ImGui::GetContentRegionAvail().x,
                           std::max(ui_px(210.0f), ImGui::GetContentRegionAvail().y));
    draw_local_image(st, st.exe_dir + L"\\branding\\ai\\account-portal-ai-v2.png",
                     hero_pos, hero_size, c32(k.brand_dk));
    ImDrawList* hero_draw = ImGui::GetWindowDrawList();
    hero_draw->AddRectFilledMultiColor(
        hero_pos, hero_pos + hero_size,
        c32(ImVec4(k.bg.x, k.bg.y, k.bg.z, 0.98f)),
        c32(ImVec4(k.bg.x, k.bg.y, k.bg.z, 0.46f)),
        c32(ImVec4(k.bg.x, k.bg.y, k.bg.z, 0.46f)),
        c32(ImVec4(k.bg.x, k.bg.y, k.bg.z, 0.98f)));
    hero_draw->AddRect(hero_pos, hero_pos + hero_size, c32(k.border), ui_px(10.0f));

    const float copy_width = std::min(hero_size.x * 0.56f, ui_px(640.0f));
    ImGui::SetCursorScreenPos(hero_pos + ImVec2(ui_px(28.0f), ui_px(28.0f)));
    ImGui::PushFont(f_title);
    ImGui::TextUnformatted("Play together. Keep your setup yours.");
    ImGui::PopFont();
    ImGui::PushTextWrapPos(hero_pos.x + ui_px(28.0f) + copy_width);
    ImGui::TextColored(k.muted,
        "Create a free Amalgam account for social features and launcher sync, then connect Microsoft separately when you are ready to play Minecraft.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    ImGui::TextColored(k.green, "Secure browser sign-in");
    ImGui::SameLine(0, ui_px(18.0f));
    ImGui::TextColored(k.blue, "Windows-protected local credentials");
    ImGui::Spacing();
    if (primary_button("Sign In", ImVec2(ui_px(140.0f), ui_px(38.0f)))) {}
    ImGui::SameLine(0, ui_px(8.0f));
    if (ghost_button("Create Free Account", ImVec2(ui_px(190.0f), ui_px(38.0f)))) {}
    ImGui::SetCursorScreenPos(hero_pos);
    ImGui::Dummy(hero_size);
    card_end();

    ImGui::Spacing();
    const bool three_columns = ImGui::GetContentRegionAvail().x >= ui_px(860.0f);
    const float gap = ui_px(12.0f);
    const float feature_width = three_columns
        ? (ImGui::GetContentRegionAvail().x - gap * 2.0f) / 3.0f
        : ImGui::GetContentRegionAvail().x;
    struct AccountFeature {
        const char* title;
        const char* detail;
        const ImVec4* color;
    };
    const AccountFeature features[] = {
        {"Private by design", "Passwords stay on the provider page; protected tokens stay with this Windows user.", &k.green},
        {"Your profiles, together", "Keep launcher preferences, friends, and supported profile settings connected.", &k.brand_hov},
        {"Microsoft stays separate", "Link the Minecraft account that owns the game without sharing its password with Amalgam.", &k.blue},
    };
    for (int i = 0; i < 3; ++i) {
        if (three_columns && i) ImGui::SameLine(0, gap);
        card_begin((std::string("##fixture_account_feature_") + std::to_string(i)).c_str(),
                   ImVec2(feature_width, ui_px(118.0f)));
        ImGui::TextColored(*features[i].color, "%s", features[i].title);
        ImGui::Spacing();
        ImGui::PushTextWrapPos();
        ImGui::TextColored(k.muted, "%s", features[i].detail);
        ImGui::PopTextWrapPos();
        card_end();
        if (!three_columns) ImGui::Spacing();
    }
}

static void draw_fixture_account_page(UiState& st, int active_tab) {
    draw_fixture_account_tabs(active_tab);
    ImGui::Spacing();
    switch (active_tab) {
        case 0: draw_fixture_account_overview(st); break;
        case 1: draw_fixture_account_profile(st); break;
        case 2: draw_fixture_account_settings(st); break;
        case 3: draw_fixture_account_security(st); break;
        case 4: draw_fixture_account_sessions(st); break;
        case 5: draw_fixture_account_activity(st); break;
        default: break;
    }
}

// ---------------------------------------------------------------------------
// Main Account Page
// ---------------------------------------------------------------------------

void draw_account_page(UiState& st) {
    // The screenshot harness must never hydrate or inspect a real account.
    // Keep this branch before AccountManager::instance(), whose constructor
    // restores protected local session data.
    if (st.fixture_mode) {
        // `account` is the legacy/base route registered by the fixture
        // manifest.  It must use the same local signed-out facade as the
        // explicit portal route; otherwise it would fall through and hydrate
        // AccountManager/Supabase before a visual capture is rendered.
        if (st.fixture_case == "account" || st.fixture_case == "account-portal") {
            draw_fixture_account_portal(st);
            return;
        }
        if (is_fixture_account_action_case(st.fixture_case)) {
            draw_fixture_account_page(st, 0);
            return;
        }
        const int fixture_tab = fixture_account_tab_for_case(st.fixture_case);
        if (fixture_tab >= 0) {
            draw_fixture_account_page(st, fixture_tab);
            return;
        }
    }

    auto& account_state = get_account_ui_state(st);
    const auto account_request = snapshot_async_ui_request(st.auth_async_request);
    // A worker on the shared account lane can replace the provider session.
    // Do not inspect Supabase or AccountManager state from this render path
    // until it has finished publishing its result.
    if (account_request.working) {
        draw_breadcrumbs({"Home", "Account"});
        page_title("Your Amalgam account", "Keeping your account state in sync.");
        card_begin("##account_operation_in_progress", ImVec2(-1, ui_px(128.0f)));
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("Updating your account");
        ImGui::PopFont();
        ImGui::Spacing();
        ImGui::TextColored(k.muted,
                            "This account request is running safely in the background. "
                            "You can continue using the launcher while it finishes.");
        card_end();
        return;
    }
    auto& account_manager = aml::account::AccountManager::instance();
    auto& supabase = aml::supabase::SupabaseManager::instance();

    // Check if authenticated
    if (!account_manager.is_authenticated() && !supabase.is_authenticated()) {
        clear_account_passive_cache(st);
        if (!account_state.bound_amalgam_user_id.empty()) {
            cancel_account_password_change(st);
            reset_sensitive_account_ui_state(account_state);
            account_state.bound_amalgam_user_id.clear();
        }
        draw_breadcrumbs({"Home", "Account"});
        page_title("Your Amalgam account",
                   "One identity for launcher preferences, friends, profiles, and supported sync.");

        const float hero_height = std::clamp(ImGui::GetContentRegionAvail().x * 0.27f,
                                             ui_px(250.0f), ui_px(340.0f));
        card_begin("##account_signed_out", ImVec2(-1, hero_height));
        const ImVec2 hero_pos = ImGui::GetCursorScreenPos();
        const ImVec2 hero_size(ImGui::GetContentRegionAvail().x,
                               std::max(ui_px(210.0f), ImGui::GetContentRegionAvail().y));
        draw_local_image(st, st.exe_dir + L"\\branding\\ai\\account-portal-ai-v2.png",
                         hero_pos, hero_size, c32(k.brand_dk));
        ImDrawList* hero_draw = ImGui::GetWindowDrawList();
        hero_draw->AddRectFilledMultiColor(
            hero_pos, hero_pos + hero_size,
            c32(ImVec4(k.bg.x, k.bg.y, k.bg.z, 0.98f)),
            c32(ImVec4(k.bg.x, k.bg.y, k.bg.z, 0.46f)),
            c32(ImVec4(k.bg.x, k.bg.y, k.bg.z, 0.46f)),
            c32(ImVec4(k.bg.x, k.bg.y, k.bg.z, 0.98f)));
        hero_draw->AddRect(hero_pos, hero_pos + hero_size, c32(k.border), ui_px(10.0f));

        const float copy_width = std::min(hero_size.x * 0.56f, ui_px(640.0f));
        ImGui::SetCursorScreenPos(hero_pos + ImVec2(ui_px(28.0f), ui_px(28.0f)));
        ImGui::PushFont(f_title);
        ImGui::TextUnformatted("Play together. Keep your setup yours.");
        ImGui::PopFont();
        ImGui::PushTextWrapPos(hero_pos.x + ui_px(28.0f) + copy_width);
        ImGui::TextColored(k.muted,
            "Create a free Amalgam account for social features and launcher sync, then connect Microsoft separately when you are ready to play Minecraft.");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        ImGui::TextColored(k.green, "Secure browser sign-in");
        ImGui::SameLine(0, ui_px(18.0f));
        ImGui::TextColored(k.blue, "Windows-protected local credentials");
        ImGui::Spacing();
        if (primary_button("Sign In", ImVec2(ui_px(140.0f), ui_px(38.0f)))) {
            st.auth_prompt_dismissed = false;
            st.login_popup_open = true;
        }
        ImGui::SameLine(0, ui_px(8.0f));
        if (ghost_button("Create Free Account", ImVec2(ui_px(190.0f), ui_px(38.0f)))) {
            st.auth_prompt_dismissed = false;
            st.auth_wizard_state = AuthWizardState();
            st.register_popup_open = true;
        }
        ImGui::SetCursorScreenPos(hero_pos);
        ImGui::Dummy(hero_size);
        card_end();

        ImGui::Spacing();
        const bool three_columns = ImGui::GetContentRegionAvail().x >= ui_px(860.0f);
        const float gap = ui_px(12.0f);
        const float feature_width = three_columns
            ? (ImGui::GetContentRegionAvail().x - gap * 2.0f) / 3.0f
            : ImGui::GetContentRegionAvail().x;
        struct AccountFeature {
            const char* title;
            const char* detail;
            const ImVec4* color;
        };
        const AccountFeature features[] = {
            {"Private by design", "Passwords stay on the provider page; protected tokens stay with this Windows user.", &k.green},
            {"Your profiles, together", "Keep launcher preferences, friends, and supported profile settings connected.", &k.brand_hov},
            {"Microsoft stays separate", "Link the Minecraft account that owns the game without sharing its password with Amalgam.", &k.blue},
        };
        for (int i = 0; i < 3; ++i) {
            if (three_columns && i) ImGui::SameLine(0, gap);
            card_begin((std::string("##account_feature_") + std::to_string(i)).c_str(),
                       ImVec2(feature_width, ui_px(118.0f)));
            ImGui::TextColored(*features[i].color, "%s", features[i].title);
            ImGui::Spacing();
            ImGui::PushTextWrapPos();
            ImGui::TextColored(k.muted, "%s", features[i].detail);
            ImGui::PopTextWrapPos();
            card_end();
            if (!three_columns) ImGui::Spacing();
        }
        return;
    }

    const auto current_user = supabase.get_current_user();
    const auto current_session = account_manager.get_current_session();
    bind_account_passive_cache(
        st, account_passive_identity(current_user.id, current_session.id));
    reconcile_account_passive_request(st);
    consume_account_passive_request(st);
    if (account_state.bound_amalgam_user_id != current_user.id) {
        cancel_account_password_change(st);
        reset_sensitive_account_ui_state(account_state);
        account_state.bound_amalgam_user_id = current_user.id;
    }

    // Account tabs — use the shared premium pill language instead of the
    // default ImGui tab strip so Account feels like the rest of the launcher.
    const char* account_tabs[] = {"Overview", "Profile", "Settings", "Security", "Sessions", "Activity"};
    const float tab_gap = ui_px(6.0f);
    for (int i = 0; i < 6; ++i) {
        if (i) ImGui::SameLine(0, tab_gap);
        const bool active = account_state.current_tab == i;
        const ImVec2 label_size = ImGui::CalcTextSize(account_tabs[i]);
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
        ImGui::InvisibleButton((std::string("##account_tab_") + std::to_string(i)).c_str(), tab_size);
        if (ImGui::IsItemHovered() && !active)
            dl->AddRect(tab_min, tab_min + tab_size, c32(k.border), ui_px(8.0f),
                        0, ui_px(1.0f));
        if (ImGui::IsItemClicked()) account_state.current_tab = i;
        ImGui::PushFont(active ? f_bold : f_body);
        dl->AddText(tab_min + ImVec2(pad_x, (tab_h - ImGui::GetTextLineHeight()) * 0.5f),
                    c32(active ? k.text : k.muted), account_tabs[i]);
        ImGui::PopFont();
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }

    if (account_state.current_tab != 2) {
        account_state.settings_initialized = false;
        account_state.settings_profile_identity.clear();
    }

    ImGui::Spacing();

    // Draw current tab
    switch (account_state.current_tab) {
        case 0:
        default:
            draw_account_overview(st);
            break;
        case 1:
            draw_account_profile(st);
            break;
        case 2:
            draw_account_settings(st);
            break;
        case 3:
            draw_account_security(st);
            break;
        case 4:
            draw_account_sessions(st);
            break;
        case 5:
            draw_account_activity(st);
            break;
    }
}

// ---------------------------------------------------------------------------
// Account Switcher
// ---------------------------------------------------------------------------

void draw_account_switcher(UiState& st) {
    // The shell invokes this every frame, even while its popup is closed.
    // AccountManager loads and persists protected local sessions in its
    // lifetime, so a deterministic fixture must return before touching it.
    // There is no named visual-fixture route for switching real accounts.
    if (st.fixture_mode) return;

    const auto account_request = snapshot_async_ui_request(st.auth_async_request);
    if (account_request.working) {
        if (ImGui::BeginPopupModal("Switch Account", nullptr,
                                   ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse)) {
            page_title("Switch Account", "Preparing the selected account safely");
            ImGui::TextColored(k.muted,
                                "This account request is running in the background. "
                                "The saved sessions will be available again when it finishes.");
            ImGui::Spacing();
            ghost_button("Working...", ImVec2(ui_px(120.0f), ui_px(32.0f)), true);
            ImGui::EndPopup();
        }
        return;
    }

    auto& account_manager = aml::account::AccountManager::instance();
    auto sessions = account_manager.get_all_sessions();
    auto current_session = account_manager.get_current_session();

    ImGui::SetNextWindowSize(ImVec2(ui_px(400.0f), ui_px(500.0f)));
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (!ImGui::BeginPopupModal("Switch Account", nullptr,
                                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse)) {
        return;
    }

    page_title("Switch Account", "Choose which Amalgam account to use");

    if (sessions.empty()) {
        empty_state("No Accounts", "You need to sign in with an Amalgam account first.", "A");
    } else {
        for (const auto& session : sessions) {
            ImGui::PushID(session.id.c_str());

            // Session card
            card_begin("##session_card", ImVec2(-1, ui_px(80.0f)));

            ImGui::BeginGroup();

            // Session info
            auto profile = account_manager.get_profile();
            ImGui::Text("%s", session.email.c_str());

            if (session.id == current_session.id) {
                ImGui::TextColored(k.green, "✓ Current Session");
            } else if (session.is_expired()) {
                ImGui::TextColored(k.red, "✗ Expired");
            } else {
                ImGui::TextColored(k.muted, "Active");
            }

            ImGui::TextColored(k.muted, "Last used: %s", format_date(session.last_used).c_str());

            ImGui::EndGroup();

            ImGui::SameLine();
            ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - ui_px(200.0f));

            ImGui::BeginGroup();
            if (session.id != current_session.id) {
                const bool account_lane_busy = account_auth_lane_reserved(st);
                if (ghost_button("Switch", ImVec2(ui_px(80.0f), ui_px(28.0f)), account_lane_busy)) {
                    request_account_session_switch(st, session);
                    ImGui::CloseCurrentPopup();
                }
                if (account_lane_busy) {
                    ImGui::TextColored(k.muted, "Account request in progress");
                }

                ImGui::SameLine();
                if (ghost_button("Remove", ImVec2(ui_px(80.0f), ui_px(28.0f)))) {
                    request_remove_local_session(st, session.id);
                    ImGui::CloseCurrentPopup();
                }
            } else {
                if (ghost_button("Sign Out", ImVec2(ui_px(160.0f), ui_px(28.0f)))) {
                    request_amalgam_sign_out(st);
                    ImGui::CloseCurrentPopup();
                }
            }

            ImGui::EndGroup();

            card_end();
            ImGui::PopID();
        }
    }

    ImGui::Spacing();
    ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - ui_px(120.0f));

    if (ghost_button("Close", ImVec2(ui_px(120.0f), ui_px(32.0f)))) {
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

// ---------------------------------------------------------------------------
// Account Button for Top Bar
// ---------------------------------------------------------------------------

void draw_account_button_enhanced(UiState& st) {
    // Keep the shared top-bar control private during visual QA too. The
    // account page facade above is not enough if a caller renders this button
    // before the page content.
    if (st.fixture_mode) {
        if (fixture_account_tab_for_case(st.fixture_case) >= 0) {
            ImGui::PushFont(f_bold);
            ImGui::TextUnformatted("Alex Morgan");
            ImGui::PopFont();
            ImGui::SameLine();
            if (ghost_button("v", ImVec2(ui_px(24.0f), ui_px(24.0f)))) {}
        } else {
            if (ghost_button("Sign In / Create Account", ImVec2(ui_px(180.0f), ui_px(32.0f)))) {}
        }
        return;
    }

    const auto account_request = snapshot_async_ui_request(st.auth_async_request);
    if (account_request.working) {
        ImGui::PushFont(f_bold);
        ImGui::TextUnformatted("Updating account...");
        ImGui::PopFont();
        ImGui::SameLine();
        ghost_button("v", ImVec2(ui_px(24.0f), ui_px(24.0f)), true);
        return;
    }

    auto& account_manager = aml::account::AccountManager::instance();
    auto& supabase_manager = aml::supabase::SupabaseManager::instance();

    // A user may deliberately remove remembered local accounts while keeping
    // the current in-memory Amalgam session alive. Keep the top-bar state
    // consistent with that explicit choice until the launcher closes or the
    // user signs out of this device.
    if (account_manager.is_authenticated() || supabase_manager.is_authenticated()) {
        auto profile = account_manager.get_profile();

        ImGui::PushFont(f_bold);
    ImGui::Text("%s", profile.display_name.empty() ? profile.username.c_str() : profile.display_name.c_str());
        ImGui::PopFont();
        ImGui::SameLine();

        if (ghost_button("v", ImVec2(ui_px(24.0f), ui_px(24.0f)))) {
            ImGui::OpenPopup("Account Menu");
        }

        // Account menu popup
        if (ImGui::BeginPopup("Account Menu")) {
            ImGui::TextUnformatted("Account");
            ImGui::Separator();

            if (ImGui::MenuItem("Account Settings")) {
                open_account_tab(st, 2);
            }

            if (ImGui::MenuItem("Profile")) {
                open_account_tab(st, 1);
            }

            if (ImGui::MenuItem("Security")) {
                open_account_tab(st, 3);
            }

            if (ImGui::MenuItem("Sessions")) {
                open_account_tab(st, 4);
            }

            ImGui::Separator();

            // Show all sessions for switching
            auto sessions = account_manager.get_all_sessions();
            if (sessions.size() > 1) {
                ImGui::TextUnformatted("Switch Account");
                ImGui::Separator();

                for (const auto& session : sessions) {
                    if (session.id != account_manager.get_current_session().id) {
                        if (ImGui::MenuItem(session.email.c_str(), nullptr, false,
                                            !account_auth_lane_reserved(st))) {
                            request_account_session_switch(st, session);
                        }
                    }
                }

                ImGui::Separator();
            }

            if (ImGui::MenuItem("Sign Out")) {
                request_amalgam_sign_out(st);
                ImGui::CloseCurrentPopup();
            }

            if (ImGui::MenuItem("Remove local accounts…")) {
                request_remove_all_local_accounts(st);
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
    } else {
        // Show sign in button
        if (ghost_button("Sign In / Create Account", ImVec2(ui_px(180.0f), ui_px(32.0f)))) {
            st.auth_prompt_dismissed = false;
            ImGui::OpenPopup("Amalgam Account Setup");
        }
    }
}

// ---------------------------------------------------------------------------
// Helper Functions
}  // namespace aml::ui
