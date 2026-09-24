#include "ui.h"
#include "ui_internal.h"
#include "supabase.h"
#include "account_manager.h"
#include "server_manager.h"
#include "storage_manager.h"
#include "sync_manager.h"
#include "admin_auth.h"
#include "auth_wizard.h"
#include "net.h"
#include "project_publishing.h"

#include <windows.h>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <exception>
#include <functional>
#include <limits>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace aml::ui {

// ---------------------------------------------------------------------------
// Admin UI State
// ---------------------------------------------------------------------------

enum class AdminActionKind {
    None,
    StartServer,
    StopServer,
    RestartServer,
    DeleteServer,
    DeregisterNode,
    CheckAllNodes,
    ApproveProject,
    RejectProject,
    TakeDownProject,
};

// Every remote Admin request is tied to a single authenticated identity and
// Supabase session generation. A late response is discarded rather than
// crossing a sign-in/sign-out or same-user session-replacement boundary.
struct AdminRemoteScope {
    std::string account_identity;
    uint64_t session_generation = 0;
};

// Only copied, presentation-safe values live in the pending action.  A
// confirmation must never retain a pointer or reference to a server, node,
// storage bucket, or project that may have changed while the dialog was open.
struct AdminPendingAction {
    AdminActionKind kind = AdminActionKind::None;
    std::string target_id;
    std::string target_label;
    std::string impact_summary;
    std::string reason;
    std::string typed_target;
    std::string error;
    bool requires_reason = false;
    bool requires_typed_target = false;
    bool requires_acknowledgement = false;
    bool acknowledged = false;
    bool submitting = false;
    bool close_after_submit = false;
    AdminRemoteScope confirmation_scope;
    AdminRemoteScope submission_scope;
};

// Passive Admin data is identity-scoped because a delayed response must never
// be rendered for a newly signed-in staff account. Workers only publish flat
// AsyncUiRequestResult values; the render thread owns this cache.
struct AdminPassiveLoadState {
    bool attempted = false;
    bool loading = false;
    bool has_value = false;
    bool stale = false;
    bool empty = false;
    std::string error;
    std::string warning;
    uint64_t loaded_at_ms = 0;
    AdminRemoteScope request_scope;
};

struct AdminDashboardData {
    int user_count = 0;
    aml::storage::StorageStats storage;
    std::vector<Json> activity_rows;
};

struct AdminPassiveCache {
    AdminRemoteScope scope;
    AdminPassiveLoadState dashboard_state;
    AdminDashboardData dashboard;
    AdminPassiveLoadState users_state;
    std::vector<aml::supabase::SupabaseUser> users;
    AdminPassiveLoadState storage_state;
    std::vector<aml::storage::StorageBucket> storage_buckets;
    AdminPassiveLoadState moderation_state;
    std::vector<aml::publishing::Project> moderation_queue;
    AdminPassiveLoadState feedback_state;
    std::vector<Json> feedback_rows;

    bool bind(std::string account_identity, uint64_t session_generation) {
        if (!account_identity.empty() && scope.account_identity == account_identity &&
            scope.session_generation == session_generation) {
            return false;
        }
        scope = {std::move(account_identity), session_generation};
        dashboard_state = {};
        dashboard = {};
        users_state = {};
        users.clear();
        storage_state = {};
        storage_buckets.clear();
        moderation_state = {};
        moderation_queue.clear();
        feedback_state = {};
        feedback_rows.clear();
        return true;
    }

    void clear() {
        scope = {};
        dashboard_state = {};
        dashboard = {};
        users_state = {};
        users.clear();
        storage_state = {};
        storage_buckets.clear();
        moderation_state = {};
        moderation_queue.clear();
        feedback_state = {};
        feedback_rows.clear();
    }
};

// Staff-role discovery is intentionally separate from the data snapshots. It
// has a short retry cadence because role assignment can happen while the
// launcher is open, but it must never run its RPC on the render thread.
struct AdminStaffAccessState {
    AdminRemoteScope scope;
    AdminRemoteScope request_scope;
    bool attempted = false;
    bool loading = false;
    bool granted_access = false;
    std::string error;
    uint64_t next_attempt_ms = 0;
};

struct AdminUIState {
    int current_tab = 0; // 0=dashboard, 1=users, 2=servers, 3=nodes, 4=storage, 5=settings, 6=moderation, 7=feedback
    
    // Dashboard
    int64_t last_refresh = 0;
    
    // Users
    std::string user_filter;
    int user_sort = 0; // 0=username, 1=email, 2=created, 3=last login
    std::string selected_user_id;
    bool user_editing = false;
    
    // Servers
    std::string server_filter;
    int server_sort = 0; // 0=name, 1=status, 2=created, 3=players
    std::string selected_server_id;
    
    // Nodes
    std::string node_filter;
    int node_sort = 0; // 0=name, 1=status, 2=cpu, 3=memory
    std::string selected_node_id;
    
    // Storage
    std::string storage_bucket_filter;
    
    // Settings
    bool maintenance_mode = false;
    std::string maintenance_message;
    int max_servers_per_user = 10;
    int max_nodes_per_user = 5;
    uint64_t max_storage_per_user = 1024 * 1024 * 1024; // 1GB
    std::string moderation_reason;
    AdminPendingAction pending_action;
    AdminPassiveCache passive;
    AdminStaffAccessState staff_access;
};

static AdminUIState& get_admin_ui_state() {
    static AdminUIState state;
    return state;
}

constexpr char kAdminPassiveDashboardAction[] = "admin-passive-dashboard";
constexpr char kAdminPassiveUsersAction[] = "admin-passive-users";
constexpr char kAdminPassiveStorageAction[] = "admin-passive-storage";
constexpr char kAdminPassiveModerationAction[] = "admin-passive-moderation";
constexpr char kAdminPassiveFeedbackAction[] = "admin-passive-feedback";
constexpr char kAdminStaffAccessAction[] = "admin-staff-access";
constexpr char kAdminMutationAction[] = "admin-mutation";

static void consume_admin_mutation_result(UiState& st);
static void consume_admin_staff_access_result(UiState& st);

static uint64_t admin_passive_now_ms() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

static bool admin_remote_scopes_match(const AdminRemoteScope& lhs,
                                       const AdminRemoteScope& rhs) {
    return !lhs.account_identity.empty() &&
           lhs.account_identity == rhs.account_identity &&
           lhs.session_generation == rhs.session_generation;
}

static bool admin_remote_scopes_equal(const AdminRemoteScope& lhs,
                                      const AdminRemoteScope& rhs) {
    return lhs.account_identity == rhs.account_identity &&
           lhs.session_generation == rhs.session_generation;
}

static bool is_admin_passive_action(std::string_view action) {
    return action == kAdminPassiveDashboardAction ||
           action == kAdminPassiveUsersAction ||
           action == kAdminPassiveStorageAction ||
           action == kAdminPassiveModerationAction ||
           action == kAdminPassiveFeedbackAction;
}

static AdminPassiveLoadState* admin_passive_state_for_action(
    AdminPassiveCache& cache, std::string_view action) {
    if (action == kAdminPassiveDashboardAction) return &cache.dashboard_state;
    if (action == kAdminPassiveUsersAction) return &cache.users_state;
    if (action == kAdminPassiveStorageAction) return &cache.storage_state;
    if (action == kAdminPassiveModerationAction) return &cache.moderation_state;
    if (action == kAdminPassiveFeedbackAction) return &cache.feedback_state;
    return nullptr;
}

static int64_t admin_bounded_counter(uint64_t value) {
    const uint64_t maximum = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    return static_cast<int64_t>(std::min(value, maximum));
}

static int64_t admin_parse_i64(const std::string& value) {
    if (value.empty()) return 0;
    try {
        return std::stoll(value);
    } catch (...) {
        return 0;
    }
}

static AdminRemoteScope current_admin_remote_scope(const UiState& st) {
    (void)st;
    auto& supabase = aml::supabase::SupabaseManager::instance();
    const auto current_user = supabase.get_current_user();
    return {current_user.id, supabase.session_generation()};
}

static bool admin_remote_scope_is_current(const UiState& st,
                                          const AdminRemoteScope& expected_scope) {
    (void)st;
    auto& supabase = aml::supabase::SupabaseManager::instance();
    if (expected_scope.account_identity.empty() ||
        supabase.session_generation() != expected_scope.session_generation) {
        return false;
    }
    const auto current_user = supabase.get_current_user();
    return current_user.id == expected_scope.account_identity;
}

static void invalidate_admin_passive_request(UiState& st) {
    const auto snapshot = snapshot_async_ui_request(st.auth_async_request);
    if (is_admin_passive_action(snapshot.action) &&
        (snapshot.working || snapshot.has_result)) {
        invalidate_auth_async_request(st, snapshot.action);
    }
}

static void invalidate_admin_staff_access_request(UiState& st) {
    const auto snapshot = snapshot_async_ui_request(st.auth_async_request);
    if (snapshot.action == kAdminStaffAccessAction &&
        (snapshot.working || snapshot.has_result)) {
        invalidate_auth_async_request(st, snapshot.action);
    }
}

static void bind_admin_passive_cache(UiState& st) {
    // The shared account lane owns mutable provider session state. Do not
    // inspect that state from the render thread while an account worker is
    // signing in, signing out, or refreshing a token.
    if (auth_async_request_lane_busy(st)) return;
    auto& cache = get_admin_ui_state().passive;
    const AdminRemoteScope current = current_admin_remote_scope(st);
    if (current.account_identity.empty()) {
        if (!cache.scope.account_identity.empty()) {
            invalidate_admin_passive_request(st);
            cache.clear();
        }
        return;
    }
    if (cache.bind(current.account_identity, current.session_generation)) {
        // An old passive response is never valid for a new identity/generation.
        invalidate_admin_passive_request(st);
    }
}

static void bind_admin_staff_access(UiState& st) {
    if (auth_async_request_lane_busy(st)) return;
    auto& staff_access = get_admin_ui_state().staff_access;
    const AdminRemoteScope current = current_admin_remote_scope(st);
    if (admin_remote_scopes_equal(staff_access.scope, current)) return;

    const bool revoke_staff_grant = staff_access.granted_access && st.admin_unlocked;
    invalidate_admin_staff_access_request(st);
    staff_access = {};
    staff_access.scope = current;
    if (revoke_staff_grant) {
        st.admin_unlocked = false;
        st.admin_unlock_until_ms = 0;
        st.admin_password.clear();
        st.admin_password_confirm.clear();
        get_admin_ui_state().pending_action = {};
        st.admin_status = "Staff access ended because the signed-in account changed.";
        push_notice(st, ui_model::NoticeLevel::Warning, "Staff access ended",
                    "The signed-in account changed, so the previous staff session was locked.");
    }
}

static void invalidate_admin_confirmation_for_changed_scope(UiState& st) {
    auto& action = get_admin_ui_state().pending_action;
    if (action.kind == AdminActionKind::None || action.submitting || action.close_after_submit) {
        return;
    }
    const AdminRemoteScope current = current_admin_remote_scope(st);
    if (admin_remote_scopes_match(current, action.confirmation_scope)) return;

    action.close_after_submit = true;
    push_notice(st, ui_model::NoticeLevel::Warning, "Administrative action closed",
                "The signed-in account changed while this confirmation was open, so the launcher did not submit it.");
}

static bool admin_passive_cache_is_stale(const AdminPassiveLoadState& state) {
    constexpr uint64_t kStaleAfterMs = 60ULL * 1000ULL;
    if (!state.has_value) return false;
    if (state.stale) return true;
    const uint64_t now = admin_passive_now_ms();
    return state.loaded_at_ms > 0 && now > state.loaded_at_ms &&
           now - state.loaded_at_ms >= kStaleAfterMs;
}

static bool start_admin_passive_request(
    UiState& st, const char* action, AdminPassiveLoadState& state,
    std::function<AsyncUiRequestResult()> work, bool force = false) {
    auto& cache = get_admin_ui_state().passive;
    if (state.loading || (!force && state.attempted) || cache.scope.account_identity.empty()) {
        return false;
    }
    // The passive Admin lane deliberately reuses the account mutation lane.
    // That prevents a remote query from racing a sign-in/sign-out/token change.
    if (auth_async_request_lane_busy(st)) return false;

    if (!start_auth_async_request(st, action, std::move(work))) return false;
    state.attempted = true;
    state.loading = true;
    state.empty = false;
    state.error.clear();
    state.warning.clear();
    state.stale = state.has_value;
    state.request_scope = cache.scope;
    return true;
}

static void mark_admin_passive_failure(AdminPassiveLoadState& state,
                                       const AsyncUiRequestResult& result,
                                       const char* fallback) {
    state.loading = false;
    state.error = result.detail.empty() ? fallback : humanize_error(result.detail);
    state.warning.clear();
    state.stale = state.has_value;
}

static void consume_admin_passive_result(UiState& st) {
    const auto snapshot = snapshot_async_ui_request(st.auth_async_request);
    if (!is_admin_passive_action(snapshot.action) || !snapshot.has_result) return;

    AsyncUiRequestSnapshot completed;
    if (!take_auth_async_request_result(st, snapshot.action, &completed)) return;

    auto& cache = get_admin_ui_state().passive;
    AdminPassiveLoadState* state = admin_passive_state_for_action(cache, completed.action);
    if (!state || !admin_remote_scopes_match(cache.scope, state->request_scope)) {
        return;
    }

    const AsyncUiRequestResult& result = completed.result;
    if (!result.payload_a.empty() &&
        result.payload_a != state->request_scope.account_identity) {
        return;
    }
    if (!result.payload_c.empty() &&
        result.payload_c != std::to_string(state->request_scope.session_generation)) {
        return;
    }
    if (!result.success) {
        mark_admin_passive_failure(*state, result,
                                   "This administrative information could not be refreshed. Please try again.");
        return;
    }

    state->loading = false;
    state->error.clear();
    state->warning = result.warning ? result.detail : std::string();
    state->loaded_at_ms = admin_passive_now_ms();
    state->stale = false;

    if (completed.action == kAdminPassiveDashboardAction) {
        std::vector<Json> activity;
        activity.reserve(result.items.size());
        for (const auto& encoded : result.items) {
            std::string parse_error;
            Json row = Json::parse(encoded, &parse_error);
            if (!parse_error.empty() || !row.isObject()) {
                mark_admin_passive_failure(*state, AsyncUiRequestResult{},
                                           "The activity response could not be read. Please refresh it.");
                return;
            }
            activity.push_back(std::move(row));
        }
        cache.dashboard.user_count = result.number_a < 0 ? 0 : static_cast<int>(result.number_a);
        cache.dashboard.storage.total_size_bytes = result.numbers.size() > 0 && result.numbers[0] > 0
            ? static_cast<uint64_t>(result.numbers[0]) : 0;
        cache.dashboard.storage.total_files = result.numbers.size() > 1 && result.numbers[1] > 0
            ? result.numbers[1] : 0;
        cache.dashboard.storage.buckets = result.numbers.size() > 2 && result.numbers[2] > 0
            ? static_cast<int>(result.numbers[2]) : 0;
        cache.dashboard.activity_rows = std::move(activity);
        state->has_value = true;
        state->empty = cache.dashboard.activity_rows.empty();
        return;
    }

    if (completed.action == kAdminPassiveUsersAction) {
        if (result.items.size() % 8 != 0) {
            mark_admin_passive_failure(*state, AsyncUiRequestResult{},
                                       "The user response could not be read. Please refresh it.");
            return;
        }
        std::vector<aml::supabase::SupabaseUser> users;
        users.reserve(result.items.size() / 8);
        for (size_t i = 0; i < result.items.size(); i += 8) {
            aml::supabase::SupabaseUser user;
            user.id = result.items[i];
            user.email = result.items[i + 1];
            user.username = result.items[i + 2];
            user.display_name = result.items[i + 3];
            user.avatar_url = result.items[i + 4];
            user.created_at = admin_parse_i64(result.items[i + 5]);
            user.updated_at = admin_parse_i64(result.items[i + 6]);
            user.last_login_at = admin_parse_i64(result.items[i + 7]);
            users.push_back(std::move(user));
        }
        cache.users = std::move(users);
        state->has_value = true;
        state->empty = cache.users.empty();
        return;
    }

    if (completed.action == kAdminPassiveStorageAction) {
        if (result.items.size() % 4 != 0) {
            mark_admin_passive_failure(*state, AsyncUiRequestResult{},
                                       "The storage response could not be read. Please refresh it.");
            return;
        }
        std::vector<aml::storage::StorageBucket> buckets;
        buckets.reserve(result.items.size() / 4);
        for (size_t i = 0; i < result.items.size(); i += 4) {
            aml::storage::StorageBucket bucket;
            bucket.name = result.items[i];
            bucket.description = result.items[i + 1];
            bucket.public_access = result.items[i + 2] == "1";
            bucket.created_at = admin_parse_i64(result.items[i + 3]);
            buckets.push_back(std::move(bucket));
        }
        cache.storage_buckets = std::move(buckets);
        state->has_value = true;
        state->empty = cache.storage_buckets.empty();
        return;
    }

    if (completed.action == kAdminPassiveModerationAction) {
        if (result.items.size() % 6 != 0) {
            mark_admin_passive_failure(*state, AsyncUiRequestResult{},
                                       "The review queue could not be read. Please refresh it.");
            return;
        }
        std::vector<aml::publishing::Project> queue;
        queue.reserve(result.items.size() / 6);
        for (size_t i = 0; i < result.items.size(); i += 6) {
            aml::publishing::Project project;
            project.id = result.items[i];
            project.name = result.items[i + 1];
            project.type = result.items[i + 2];
            project.owner_id = result.items[i + 3];
            project.description = result.items[i + 4];
            project.status = result.items[i + 5];
            queue.push_back(std::move(project));
        }
        cache.moderation_queue = std::move(queue);
        state->has_value = true;
        state->empty = cache.moderation_queue.empty();
        return;
    }

    if (completed.action == kAdminPassiveFeedbackAction) {
        std::vector<Json> feedback;
        feedback.reserve(result.items.size());
        for (const auto& encoded : result.items) {
            std::string parse_error;
            Json row = Json::parse(encoded, &parse_error);
            if (!parse_error.empty() || !row.isObject()) {
                mark_admin_passive_failure(*state, AsyncUiRequestResult{},
                                           "The feedback response could not be read. Please refresh it.");
                return;
            }
            feedback.push_back(std::move(row));
        }
        cache.feedback_rows = std::move(feedback);
        state->has_value = true;
        state->empty = cache.feedback_rows.empty();
    }
}

// This is called by the shell every frame, not only while the Admin route is
// visible. A completed passive response must be consumed (or discarded for a
// changed identity) so it never leaves the shared account lane reserved after
// the operator navigates elsewhere.
void reconcile_admin_background_requests(UiState& st) {
    // Visual fixtures never start an Admin request and must not inspect the
    // reviewer's authenticated state merely because the shell renders them.
    if (st.fixture_mode) return;

    // A completed Admin result is safe to validate now that its worker has
    // stopped. While any account worker is still active, defer provider reads
    // completely; the account lane may be replacing session state.
    const auto snapshot = snapshot_async_ui_request(st.auth_async_request);
    if (snapshot.has_result) {
        if (is_admin_passive_action(snapshot.action)) {
            consume_admin_passive_result(st);
        } else if (snapshot.action == kAdminStaffAccessAction) {
            consume_admin_staff_access_result(st);
        } else if (snapshot.action == kAdminMutationAction) {
            consume_admin_mutation_result(st);
        }
    }
    if (auth_async_request_lane_busy(st)) return;

    bind_admin_passive_cache(st);
    bind_admin_staff_access(st);
    invalidate_admin_confirmation_for_changed_scope(st);
}

static AsyncUiRequestResult admin_remote_scope_changed_result(
    const AdminRemoteScope& expected_scope) {
    AsyncUiRequestResult result;
    result.payload_a = expected_scope.account_identity;
    result.payload_c = std::to_string(expected_scope.session_generation);
    result.title = "Administrative data not applied";
    result.detail = "Your signed-in account changed before this administrative data could be refreshed.";
    return result;
}

void request_admin_staff_access_check(UiState& st, bool force) {
    if (st.fixture_mode || auth_async_request_lane_busy(st)) return;
    bind_admin_staff_access(st);

    auto& staff_access = get_admin_ui_state().staff_access;
    if (staff_access.scope.account_identity.empty()) return;
    const uint64_t now = admin_passive_now_ms();
    if (staff_access.loading ||
        (!force && staff_access.attempted && now < staff_access.next_attempt_ms)) {
        return;
    }

    const AdminRemoteScope request_scope = staff_access.scope;
    if (!start_auth_async_request(
            st, kAdminStaffAccessAction,
            [&st, request_scope]() {
                if (!admin_remote_scope_is_current(st, request_scope)) {
                    return admin_remote_scope_changed_result(request_scope);
                }

                AsyncUiRequestResult result;
                result.payload_a = request_scope.account_identity;
                result.payload_c = std::to_string(request_scope.session_generation);
                auto* client = aml::supabase::SupabaseManager::instance().client();
                if (!client || !client->is_authenticated()) {
                    result.title = "Staff access unavailable";
                    result.detail = "Sign in to your Amalgam account before checking staff access.";
                    return result;
                }

                const auto response = client->rpc("is_project_staff");
                if (!admin_remote_scope_is_current(st, request_scope)) {
                    return admin_remote_scope_changed_result(request_scope);
                }
                if (!response.success) {
                    result.title = "Staff access unavailable";
                    result.detail = response.error.empty()
                        ? "The staff role could not be verified. Please try again."
                        : response.error;
                    return result;
                }

                result.success = true;
                result.number_a = !response.data.empty() && response.data.front().as_bool() ? 1 : 0;
                result.title = result.number_a != 0 ? "Staff access enabled" : "Staff role required";
                result.detail = result.number_a != 0
                    ? "Staff access was verified for this signed-in account."
                    : "This signed-in account does not currently have a staff role.";
                return result;
            })) {
        return;
    }

    staff_access.attempted = true;
    staff_access.loading = true;
    staff_access.error.clear();
    staff_access.request_scope = request_scope;
}

bool admin_staff_access_check_in_progress(const UiState&) {
    return get_admin_ui_state().staff_access.loading;
}

void note_admin_local_password_unlock() {
    // A locally configured fallback password is not a server-issued staff
    // grant. A later negative staff-role probe must not revoke that separate
    // local session.
    get_admin_ui_state().staff_access.granted_access = false;
}

static void consume_admin_staff_access_result(UiState& st) {
    const auto snapshot = snapshot_async_ui_request(st.auth_async_request);
    if (snapshot.action != kAdminStaffAccessAction || !snapshot.has_result) return;

    AsyncUiRequestSnapshot completed;
    if (!take_auth_async_request_result(st, kAdminStaffAccessAction, &completed)) return;

    auto& staff_access = get_admin_ui_state().staff_access;
    staff_access.loading = false;
    const AsyncUiRequestResult& result = completed.result;
    const AdminRemoteScope current = current_admin_remote_scope(st);
    if (!admin_remote_scopes_match(staff_access.scope, current) ||
        !admin_remote_scopes_match(staff_access.request_scope, current) ||
        (!result.payload_a.empty() && result.payload_a != current.account_identity) ||
         (!result.payload_c.empty() &&
          result.payload_c != std::to_string(current.session_generation))) {
        return;
    }

    staff_access.next_attempt_ms = admin_passive_now_ms() + 5000ULL;
    if (!result.success) {
        staff_access.error = result.detail.empty()
            ? "Staff access could not be verified. Please try again."
            : humanize_error(result.detail);
        st.admin_status = staff_access.error;
        return;
    }

    staff_access.error.clear();
    if (result.number_a != 0) {
        staff_access.granted_access = true;
        st.admin_unlocked = true;
        st.admin_unlock_until_ms = GetTickCount64() + 15ull * 60ull * 1000ull;
        st.admin_status = "Staff access enabled for this account.";
    } else if (staff_access.granted_access) {
        staff_access.granted_access = false;
        st.admin_unlocked = false;
        st.admin_unlock_until_ms = 0;
        st.admin_password.clear();
        st.admin_password_confirm.clear();
        get_admin_ui_state().pending_action = {};
        st.admin_status = "Staff access is no longer assigned to this account.";
        push_notice(st, ui_model::NoticeLevel::Warning, "Staff access removed",
                    "The server no longer grants this account staff access, so administrative controls were locked.");
    } else if (!st.admin_unlocked) {
        st.admin_status = "Staff role required for this signed-in account.";
    }
}

static void request_admin_dashboard(UiState& st, bool force = false) {
    bind_admin_passive_cache(st);
    auto& cache = get_admin_ui_state().passive;
    auto& state = cache.dashboard_state;
    const AdminRemoteScope request_scope = cache.scope;
    start_admin_passive_request(
        st, kAdminPassiveDashboardAction, state,
        [&st, request_scope]() {
            if (!admin_remote_scope_is_current(st, request_scope)) {
                return admin_remote_scope_changed_result(request_scope);
            }
            AsyncUiRequestResult result;
            result.payload_a = request_scope.account_identity;
            result.payload_c = std::to_string(request_scope.session_generation);
            auto* client = aml::supabase::SupabaseManager::instance().client();
            if (!client || !client->is_authenticated()) {
                result.title = "Dashboard unavailable";
                result.detail = "Staff authentication is required to refresh the dashboard.";
                return result;
            }

            const auto users = client->rpc("admin_list_users");
            if (!users.success) {
                result.title = "Dashboard unavailable";
                result.detail = users.error.empty()
                    ? "The user summary could not be refreshed."
                    : users.error;
                return result;
            }

            const auto storage = aml::storage::StorageManager::instance().get_stats();
            aml::supabase::SupabaseClient::DBQueryOptions query;
            query.table = "account_activity";
            query.order_by = "created_at";
            query.order_asc = false;
            query.limit = 10;
            const auto activity = client->select(query);

            if (!admin_remote_scope_is_current(st, request_scope)) {
                return admin_remote_scope_changed_result(request_scope);
            }

            result.success = true;
            result.title = "Dashboard refreshed";
            result.number_a = users.count > 0 ? users.count
                                                : static_cast<int64_t>(users.data.size());
            result.numbers = {
                admin_bounded_counter(storage.total_size_bytes),
                std::max<int64_t>(0, storage.total_files),
                std::max(0, storage.buckets),
            };
            if (activity.success) {
                result.items.reserve(activity.data.size());
                for (const auto& row : activity.data) result.items.push_back(row.dump());
            } else {
                result.warning = true;
                result.detail = activity.error.empty()
                    ? "Recent activity could not be refreshed."
                    : activity.error;
            }
            return result;
        }, force);
}

static void request_admin_users(UiState& st, bool force = false) {
    bind_admin_passive_cache(st);
    auto& cache = get_admin_ui_state().passive;
    auto& state = cache.users_state;
    const AdminRemoteScope request_scope = cache.scope;
    start_admin_passive_request(
        st, kAdminPassiveUsersAction, state,
        [&st, request_scope]() {
            if (!admin_remote_scope_is_current(st, request_scope)) {
                return admin_remote_scope_changed_result(request_scope);
            }
            AsyncUiRequestResult result;
            result.payload_a = request_scope.account_identity;
            result.payload_c = std::to_string(request_scope.session_generation);
            auto* client = aml::supabase::SupabaseManager::instance().client();
            if (!client || !client->is_authenticated()) {
                result.title = "Users unavailable";
                result.detail = "Staff authentication is required to refresh users.";
                return result;
            }
            const auto response = client->rpc("admin_list_users");
            if (!response.success) {
                result.title = "Users unavailable";
                result.detail = response.error.empty() ? "Users could not be refreshed."
                                                       : response.error;
                return result;
            }
            if (!admin_remote_scope_is_current(st, request_scope)) {
                return admin_remote_scope_changed_result(request_scope);
            }
            result.success = true;
            result.title = "Users refreshed";
            result.number_a = response.count > 0 ? response.count
                                                   : static_cast<int64_t>(response.data.size());
            result.items.reserve(response.data.size() * 8);
            for (const auto& row : response.data) {
                result.items.push_back(row.get("id").as_str());
                result.items.push_back(row.get("email").as_str());
                result.items.push_back(row.get("username").as_str());
                result.items.push_back(row.get("display_name").as_str());
                result.items.push_back(row.get("avatar_url").as_str());
                result.items.push_back(std::to_string(row.get("created_at").as_int()));
                result.items.push_back(std::to_string(row.get("updated_at").as_int()));
                result.items.push_back(std::to_string(row.get("last_login_at").as_int()));
            }
            return result;
        }, force);
}

static void request_admin_storage(UiState& st, bool force = false) {
    bind_admin_passive_cache(st);
    auto& cache = get_admin_ui_state().passive;
    auto& state = cache.storage_state;
    const AdminRemoteScope request_scope = cache.scope;
    start_admin_passive_request(
        st, kAdminPassiveStorageAction, state,
        [&st, request_scope]() {
            if (!admin_remote_scope_is_current(st, request_scope)) {
                return admin_remote_scope_changed_result(request_scope);
            }
            AsyncUiRequestResult result;
            result.payload_a = request_scope.account_identity;
            result.payload_c = std::to_string(request_scope.session_generation);
            auto* client = aml::supabase::SupabaseManager::instance().client();
            if (!client || !client->is_authenticated()) {
                result.title = "Storage unavailable";
                result.detail = "Staff authentication is required to refresh storage buckets.";
                return result;
            }
            const auto buckets = aml::storage::StorageManager::instance().get_buckets();
            if (!admin_remote_scope_is_current(st, request_scope)) {
                return admin_remote_scope_changed_result(request_scope);
            }
            result.success = true;
            result.title = "Storage buckets refreshed";
            result.items.reserve(buckets.size() * 4);
            for (const auto& bucket : buckets) {
                result.items.push_back(bucket.name);
                result.items.push_back(bucket.description);
                result.items.push_back(bucket.public_access ? "1" : "0");
                result.items.push_back(std::to_string(bucket.created_at));
            }
            return result;
        }, force);
}

static void request_admin_moderation(UiState& st, bool force = false) {
    bind_admin_passive_cache(st);
    auto& cache = get_admin_ui_state().passive;
    auto& state = cache.moderation_state;
    const AdminRemoteScope request_scope = cache.scope;
    start_admin_passive_request(
        st, kAdminPassiveModerationAction, state,
        [&st, request_scope]() {
            if (!admin_remote_scope_is_current(st, request_scope)) {
                return admin_remote_scope_changed_result(request_scope);
            }
            AsyncUiRequestResult result;
            result.payload_a = request_scope.account_identity;
            result.payload_c = std::to_string(request_scope.session_generation);
            auto* client = aml::supabase::SupabaseManager::instance().client();
            if (!client || !client->is_authenticated()) {
                result.title = "Review queue unavailable";
                result.detail = "Staff authentication is required to refresh the review queue.";
                return result;
            }
            std::vector<aml::publishing::Project> queue;
            std::string error;
            if (!aml::publishing::list_moderation_queue(*client, queue, &error)) {
                result.title = "Review queue unavailable";
                result.detail = error.empty() ? "The review queue could not be refreshed." : error;
                return result;
            }
            if (!admin_remote_scope_is_current(st, request_scope)) {
                return admin_remote_scope_changed_result(request_scope);
            }
            result.success = true;
            result.title = "Review queue refreshed";
            result.items.reserve(queue.size() * 6);
            for (const auto& project : queue) {
                result.items.push_back(project.id);
                result.items.push_back(project.name);
                result.items.push_back(project.type);
                result.items.push_back(project.owner_id);
                result.items.push_back(project.description);
                result.items.push_back(project.status);
            }
            return result;
        }, force);
}

static void request_admin_feedback(UiState& st, bool force = false) {
    bind_admin_passive_cache(st);
    auto& cache = get_admin_ui_state().passive;
    auto& state = cache.feedback_state;
    const AdminRemoteScope request_scope = cache.scope;
    start_admin_passive_request(
        st, kAdminPassiveFeedbackAction, state,
        [&st, request_scope]() {
            if (!admin_remote_scope_is_current(st, request_scope)) {
                return admin_remote_scope_changed_result(request_scope);
            }
            AsyncUiRequestResult result;
            result.payload_a = request_scope.account_identity;
            result.payload_c = std::to_string(request_scope.session_generation);
            auto* client = aml::supabase::SupabaseManager::instance().client();
            if (!client || !client->is_authenticated()) {
                result.title = "Feedback unavailable";
                result.detail = "Staff authentication is required to refresh beta feedback.";
                return result;
            }
            aml::supabase::SupabaseClient::DBQueryOptions query;
            query.table = "beta_feedback";
            query.select = "id,user_id,category,rating,message,page,app_version,status,created_at";
            query.order_by = "created_at";
            query.order_asc = false;
            query.limit = 100;
            const auto response = client->select(query);
            if (!response.success) {
                result.title = "Feedback unavailable";
                result.detail = response.error.empty() ? "Feedback could not be refreshed."
                                                       : response.error;
                return result;
            }
            if (!admin_remote_scope_is_current(st, request_scope)) {
                return admin_remote_scope_changed_result(request_scope);
            }
            result.success = true;
            result.title = "Feedback refreshed";
            result.items.reserve(response.data.size());
            for (const auto& row : response.data) result.items.push_back(row.dump());
            return result;
        }, force);
}

static constexpr const char* kAdminConfirmationPopup =
    "Confirm administrative action##admin_confirm_action";

static bool has_visible_text(const std::string& value) {
    return std::any_of(value.begin(), value.end(), [](unsigned char c) {
        return !std::isspace(c);
    });
}

static const char* admin_action_title(AdminActionKind kind) {
    switch (kind) {
        case AdminActionKind::StartServer: return "Start server";
        case AdminActionKind::StopServer: return "Stop server";
        case AdminActionKind::RestartServer: return "Restart server";
        case AdminActionKind::DeleteServer: return "Delete server";
        case AdminActionKind::DeregisterNode: return "Deregister node";
        case AdminActionKind::CheckAllNodes: return "Check all nodes";
        case AdminActionKind::ApproveProject: return "Approve project";
        case AdminActionKind::RejectProject: return "Reject project";
        case AdminActionKind::TakeDownProject: return "Take down project";
        case AdminActionKind::None: break;
    }
    return "Confirm action";
}

static const char* admin_action_confirm_label(AdminActionKind kind) {
    switch (kind) {
        case AdminActionKind::StartServer: return "Start server";
        case AdminActionKind::StopServer: return "Stop server";
        case AdminActionKind::RestartServer: return "Restart server";
        case AdminActionKind::DeleteServer: return "Delete permanently";
        case AdminActionKind::DeregisterNode: return "Deregister node";
        case AdminActionKind::CheckAllNodes: return "Request checks";
        case AdminActionKind::ApproveProject: return "Approve project";
        case AdminActionKind::RejectProject: return "Reject project";
        case AdminActionKind::TakeDownProject: return "Take down project";
        case AdminActionKind::None: break;
    }
    return "Confirm";
}

static bool admin_action_is_dangerous(AdminActionKind kind) {
    return kind == AdminActionKind::DeleteServer ||
           kind == AdminActionKind::DeregisterNode ||
           kind == AdminActionKind::TakeDownProject ||
           kind == AdminActionKind::RejectProject;
}

static const char* admin_action_acknowledgement(AdminActionKind kind) {
    switch (kind) {
        case AdminActionKind::StartServer:
            return "I understand this starts the selected server.";
        case AdminActionKind::StopServer:
        case AdminActionKind::RestartServer:
            return "I understand this interrupts connected players.";
        case AdminActionKind::DeleteServer:
            return "I understand this removes the server control-plane record.";
        case AdminActionKind::DeregisterNode:
            return "I understand this can affect capacity and routing.";
        case AdminActionKind::CheckAllNodes:
            return "I understand this sends health checks to every registered node.";
        case AdminActionKind::ApproveProject:
            return "I understand this makes the project eligible for public release.";
        case AdminActionKind::RejectProject:
        case AdminActionKind::TakeDownProject:
            return "I understand this changes the project's publication status.";
        case AdminActionKind::None:
            break;
    }
    return "I understand the effect of this action.";
}

static void clear_admin_pending_action() {
    get_admin_ui_state().pending_action = AdminPendingAction{};
}

static void request_admin_confirmation(UiState& st, AdminActionKind kind, std::string target_id,
                                        std::string target_label, std::string impact_summary,
                                        bool requires_reason = false,
                                        bool requires_typed_target = false,
                                        bool requires_acknowledgement = false) {
    auto& admin = get_admin_ui_state();
    // One immutable action owns the shared background lane at a time. Do not
    // overwrite it from another tab while its confirmation or submission is
    // still visible; otherwise a completion could no longer be matched to the
    // server-side action that produced it.
    if (admin.pending_action.kind != AdminActionKind::None) return;
    if (auth_async_request_lane_busy(st)) {
        push_notice(st, ui_model::NoticeLevel::Info, "Account operation in progress",
                    "Wait for the current account operation to finish before confirming another administrative action.");
        return;
    }
    const AdminRemoteScope confirmation_scope = current_admin_remote_scope(st);
    if (confirmation_scope.account_identity.empty()) {
        push_notice(st, ui_model::NoticeLevel::Warning, "Staff authentication required",
                    "Sign in to an approved staff account before confirming this administrative action.");
        return;
    }
    AdminPendingAction action;
    action.kind = kind;
    action.target_id = std::move(target_id);
    action.target_label = std::move(target_label);
    action.impact_summary = std::move(impact_summary);
    action.requires_reason = requires_reason;
    action.requires_typed_target = requires_typed_target;
    action.requires_acknowledgement = requires_acknowledgement;
    action.confirmation_scope = confirmation_scope;
    if (requires_reason) action.reason = admin.moderation_reason;
    admin.pending_action = std::move(action);
    ImGui::OpenPopup(kAdminConfirmationPopup);
}

static AsyncUiRequestResult perform_admin_pending_action(
    const UiState& st, const AdminPendingAction& action,
    const AdminRemoteScope& expected_scope) {
    if (!admin_remote_scope_is_current(st, expected_scope)) {
        return admin_remote_scope_changed_result(expected_scope);
    }

    AsyncUiRequestResult result;
    result.payload_a = expected_scope.account_identity;
    result.payload_b = action.target_id;
    result.payload_c = std::to_string(expected_scope.session_generation);
    result.number_a = static_cast<int64_t>(action.kind);

    bool success = false;
    switch (action.kind) {
        case AdminActionKind::StartServer:
            success = aml::servers::ServerManager::instance().start_server(action.target_id);
            result.title = success ? "Server Starting" : "Server Start Failed";
            result.detail = success ? "The server start request was submitted."
                                    : "The server could not be started. Confirm its current state and try again.";
            break;
        case AdminActionKind::StopServer:
            success = aml::servers::ServerManager::instance().stop_server(action.target_id);
            result.title = success ? "Server Stopping" : "Server Stop Failed";
            result.detail = success ? "The server stop request was submitted."
                                    : "The server could not be stopped. Confirm its current state and try again.";
            break;
        case AdminActionKind::RestartServer:
            success = aml::servers::ServerManager::instance().restart_server(action.target_id);
            result.title = success ? "Server Restarting" : "Server Restart Failed";
            result.detail = success ? "The server restart request was submitted."
                                    : "The server could not be restarted. Confirm its current state and try again.";
            break;
        case AdminActionKind::DeleteServer:
            success = aml::servers::ServerManager::instance().delete_server(action.target_id);
            result.title = success ? "Server Deleted" : "Server Delete Failed";
            result.detail = success ? "The server was deleted successfully."
                                    : "The server could not be deleted. It may have changed or you may no longer have access.";
            break;
        case AdminActionKind::DeregisterNode:
            success = aml::servers::ServerManager::instance().deregister_node(action.target_id);
            result.title = success ? "Node Deregistered" : "Node Deregistration Failed";
            result.detail = success ? "The node was removed from the control plane."
                                    : "The node could not be deregistered. Refresh its state and try again.";
            break;
        case AdminActionKind::CheckAllNodes:
            aml::servers::ServerManager::instance().check_node_health();
            success = true;
            result.title = "Health Checks Requested";
            result.detail = "Health checks were requested for all registered nodes.";
            break;
        case AdminActionKind::ApproveProject:
        case AdminActionKind::RejectProject:
        case AdminActionKind::TakeDownProject: {
            auto* client = aml::supabase::SupabaseManager::instance().client();
            if (!client || !client->is_authenticated() || !client->is_current_user_staff()) {
                result.title = "Moderation unavailable";
                result.detail = "Staff authentication is required to complete this moderation action.";
                break;
            }
            const char* decision = action.kind == AdminActionKind::ApproveProject ? "approve" :
                (action.kind == AdminActionKind::RejectProject ? "reject" : "takedown");
            std::string error;
            success = aml::publishing::review_project(*client, action.target_id, decision,
                                                       action.reason, &error);
            if (success) {
                if (action.kind == AdminActionKind::ApproveProject) {
                    result.title = "Project Approved";
                    result.detail = "Future versions may publish without repeat review.";
                } else if (action.kind == AdminActionKind::RejectProject) {
                    result.title = "Project Rejected";
                    result.detail = "The creator can revise and resubmit the project.";
                } else {
                    result.title = "Project Unpublished";
                    result.detail = "The project is no longer publicly listed.";
                }
            } else {
                result.title = "Moderation Failed";
                result.detail = error.empty() ? "The moderation decision could not be recorded." : error;
            }
            break;
        }
        case AdminActionKind::None:
            result.title = "Administrative action unavailable";
            result.detail = "Choose an administrative action before confirming it.";
            break;
    }
    if (!admin_remote_scope_is_current(st, expected_scope)) {
        return admin_remote_scope_changed_result(expected_scope);
    }
    result.success = success;
    return result;
}

static void mark_admin_passive_snapshots_stale() {
    auto& cache = get_admin_ui_state().passive;
    const auto mark = [](AdminPassiveLoadState& state) {
        if (state.has_value) state.stale = true;
        state.attempted = false;
    };
    mark(cache.dashboard_state);
    mark(cache.users_state);
    mark(cache.storage_state);
    mark(cache.moderation_state);
    mark(cache.feedback_state);
}

static ui_model::NoticeLevel admin_mutation_notice_level(AdminActionKind kind) {
    switch (kind) {
        case AdminActionKind::DeleteServer:
        case AdminActionKind::DeregisterNode:
        case AdminActionKind::ApproveProject:
            return ui_model::NoticeLevel::Success;
        case AdminActionKind::TakeDownProject:
            return ui_model::NoticeLevel::Warning;
        case AdminActionKind::StartServer:
        case AdminActionKind::StopServer:
        case AdminActionKind::RestartServer:
        case AdminActionKind::CheckAllNodes:
        case AdminActionKind::RejectProject:
        case AdminActionKind::None:
            return ui_model::NoticeLevel::Info;
    }
    return ui_model::NoticeLevel::Info;
}

static void consume_admin_mutation_result(UiState& st) {
    const auto snapshot = snapshot_async_ui_request(st.auth_async_request);
    if (snapshot.action != kAdminMutationAction || !snapshot.has_result) return;

    AsyncUiRequestSnapshot completed;
    if (!take_auth_async_request_result(st, kAdminMutationAction, &completed)) return;

    auto& action = get_admin_ui_state().pending_action;
    const AsyncUiRequestResult& result = completed.result;
    const AdminRemoteScope current = current_admin_remote_scope(st);
    const bool action_still_matches = action.submitting &&
        admin_remote_scopes_match(current, action.submission_scope) &&
        admin_remote_scopes_match(current, action.confirmation_scope) &&
        result.payload_a == action.submission_scope.account_identity &&
        result.payload_b == action.target_id &&
        result.payload_c == std::to_string(action.submission_scope.session_generation) &&
        result.number_a == static_cast<int64_t>(action.kind);
    if (!action_still_matches) {
        if (action.submitting) {
            action.submitting = false;
            action.close_after_submit = true;
            push_notice(st, ui_model::NoticeLevel::Warning, "Administrative action needs review",
                        "The account, session, or selected target changed before the action completed. The launcher did not retry it; verify the current state before taking another action.");
        }
        return;
    }

    action.submitting = false;
    if (!result.success) {
        action.error = result.detail.empty() ? "The administrative action could not be completed. Try again."
                                             : humanize_error(result.detail);
        return;
    }

    push_notice(st, admin_mutation_notice_level(action.kind), result.title, result.detail);
    action.close_after_submit = true;
    mark_admin_passive_snapshots_stale();
}

static bool execute_admin_pending_action(UiState& st) {
    auto& action = get_admin_ui_state().pending_action;
    if (action.kind == AdminActionKind::None) return false;
    const uint64_t now = GetTickCount64();
    if (!st.admin_unlocked ||
        (st.admin_unlock_until_ms != 0 && now >= st.admin_unlock_until_ms)) {
        st.admin_unlocked = false;
        st.admin_unlock_until_ms = 0;
        st.admin_password.clear();
        st.admin_password_confirm.clear();
        action.error = "Your administrative session is no longer active. Re-authenticate before confirming this action.";
        return false;
    }
    if (action.submitting || action.close_after_submit) return false;
    if (auth_async_request_lane_busy(st)) {
        action.error = "Another account operation is still finishing. Please try again in a moment.";
        return false;
    }

    const AdminRemoteScope scope = current_admin_remote_scope(st);
    if (scope.account_identity.empty()) {
        action.error = "Staff authentication is required to complete this administrative action.";
        return false;
    }
    if (!admin_remote_scopes_match(scope, action.confirmation_scope)) {
        action.error = "The signed-in account changed after this action was opened. Review the current state before starting a new action.";
        action.close_after_submit = true;
        push_notice(st, ui_model::NoticeLevel::Warning, "Administrative action closed",
                    "The signed-in account changed before confirmation, so the launcher did not submit the previous action.");
        return false;
    }
    const AdminPendingAction action_snapshot = action;
    if (!start_auth_async_request(st, kAdminMutationAction,
                                  [&st, action_snapshot, request_scope = scope]() {
                                      return perform_admin_pending_action(st, action_snapshot,
                                                                          request_scope);
                                  })) {
        action.error = "Another account operation is still finishing. Please try again in a moment.";
        return false;
    }

    action.submission_scope = scope;
    action.submitting = true;
    action.error.clear();
    return true;
}

static void draw_admin_confirmation_dialog(UiState& st) {
    auto& action = get_admin_ui_state().pending_action;
    if (action.kind == AdminActionKind::None) return;

    // Keep the request live until the operator cancels or a confirmed backend
    // operation succeeds. This also prevents a stale menu selection from
    // bypassing the modal on the next frame.
    ImGui::OpenPopup(kAdminConfirmationPopup);
    ImGui::SetNextWindowSize(ImVec2(ui_px(520.0f), 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(kAdminConfirmationPopup, nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    // A successful worker result is consumed by the shell even if the operator
    // navigated away. Close this modal only when it is next rendered, so ImGui
    // never leaves an empty confirmation window behind.
    if (action.close_after_submit) {
        clear_admin_pending_action();
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    const bool dangerous = admin_action_is_dangerous(action.kind);
    ImGui::PushFont(f_h2);
    ImGui::TextColored(dangerous ? k.red : k.text, "%s", admin_action_title(action.kind));
    ImGui::PopFont();
    ImGui::TextColored(k.muted, "Target: %s", action.target_label.c_str());
    ImGui::Spacing();
    ImGui::TextWrapped("%s", action.impact_summary.c_str());
    ImGui::Spacing();

    if (action.submitting) {
        ImGui::TextColored(k.brand_hov,
                           "Submitting the confirmed action in the background. Keep this window open until it finishes.");
        ImGui::Spacing();
    }
    const bool account_operation_pending = auth_async_request_lane_busy(st);
    if (account_operation_pending && !action.submitting) {
        ImGui::TextColored(k.muted,
                           "Finish the current account operation before confirming this action.");
        ImGui::Spacing();
    }
    const bool form_locked = action.submitting || account_operation_pending;

    ImGui::BeginDisabled(form_locked);

    if (action.requires_reason) {
        ImGui::TextUnformatted("Reason");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextMultiline("##admin_action_reason", &action.reason,
                                  ImVec2(-1, ui_px(76.0f)));
        ImGui::TextColored(k.muted, "A reason is required and will be included with the moderation decision.");
        ImGui::Spacing();
    }
    if (action.requires_typed_target) {
        ImGui::TextColored(dangerous ? k.red : k.muted,
                           "Type \"%s\" to enable the final action.", action.target_label.c_str());
        ImGui::SetNextItemWidth(-1);
        input_text_hint("##admin_action_typed_target", "Type the exact target name", &action.typed_target);
        ImGui::Spacing();
    }
    if (action.requires_acknowledgement) {
        ImGui::Checkbox(admin_action_acknowledgement(action.kind), &action.acknowledged);
        ImGui::Spacing();
    }
    if (!action.error.empty()) {
        ImGui::TextColored(k.red, "%s", action.error.c_str());
        ImGui::Spacing();
    }

    const bool typed_target_matches = !action.requires_typed_target ||
        action.typed_target == action.target_label;
    const bool reason_present = !action.requires_reason || has_visible_text(action.reason);
    const bool acknowledged = !action.requires_acknowledgement || action.acknowledged;
    const bool ready = typed_target_matches && reason_present && acknowledged && !form_locked;
    ImGui::EndDisabled();

    if (ghost_button("Cancel", ImVec2(ui_px(100.0f), ui_px(34.0f)))) {
        clear_admin_pending_action();
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(form_locked);
    const bool confirmed = dangerous
        ? danger_button(admin_action_confirm_label(action.kind), ImVec2(ui_px(180.0f), ui_px(34.0f)), !ready)
        : primary_button(admin_action_confirm_label(action.kind), ImVec2(ui_px(180.0f), ui_px(34.0f)), action.submitting, !ready);
    if (confirmed && ready) execute_admin_pending_action(st);
    ImGui::EndDisabled();
    ImGui::EndPopup();
}

static int64_t activity_timestamp(const Json& row) {
    for (const char* key : {"timestamp", "occurred_at", "created_at"}) {
        const int64_t value = row.get(key).as_int();
        if (value > 0) return value;
    }
    return 0;
}

static std::string activity_text(const Json& row) {
    std::string text = row.get("description").as_str();
    if (text.empty()) text = row.get("message").as_str();
    if (text.empty()) text = row.get("action").as_str();
    if (text.empty()) text = row.get("type").as_str();
    return text;
}

static bool has_authoritative_metrics(const aml::servers::Node& node) {
    const auto it = node.metadata.find("metrics_status");
    return it != node.metadata.end() && it->second == "available";
}

static void draw_admin_moderation(UiState& st) {
    auto& admin = get_admin_ui_state();
    page_title("Project Moderation", "Review creator projects before their first public release.");
    if (auth_async_request_lane_busy(st)) {
        empty_state("Account activity in progress",
                    "Finish the current account operation before loading the review queue.", "…");
        return;
    }
    auto* client = aml::supabase::SupabaseManager::instance().client();
    if (!client || !client->is_authenticated()) {
        empty_state("Sign in required", "Staff authentication is required to load the review queue.", "@");
        return;
    }
    card_begin("##moderation_header");
    ImGui::TextUnformatted("Safety Review Queue");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - ui_px(100.0f));
    auto& cache = admin.passive;
    auto& load_state = cache.moderation_state;
    if (ghost_button(load_state.loading ? "Refreshing..." : "Refresh",
                     ImVec2(ui_px(90.0f), ui_px(30.0f)), load_state.loading)) {
        request_admin_moderation(st, true);
    }
    ImGui::InputTextMultiline("Review reason", &admin.moderation_reason, ImVec2(-1, ui_px(55.0f)));
    card_end();
    ImGui::Spacing();

    request_admin_moderation(st);
    if (load_state.loading && !load_state.has_value) {
        empty_state("Loading review queue", "Retrieving the review queue in the background. You can keep browsing.", "…");
        return;
    }
    if (!load_state.error.empty() && !load_state.has_value) {
        card_begin("##moderation_error");
        ImGui::TextColored(k.red, "REVIEW QUEUE UNAVAILABLE");
        ImGui::TextWrapped("%s", load_state.error.c_str());
        if (ghost_button("Try again", ImVec2(ui_px(110.0f), ui_px(30.0f))))
            request_admin_moderation(st, true);
        card_end();
        return;
    }
    if (load_state.loading || admin_passive_cache_is_stale(load_state))
        ImGui::TextColored(k.brand_hov, "Showing the last verified review queue while it refreshes.");
    if (!load_state.warning.empty())
        ImGui::TextColored(k.yellow, "%s", load_state.warning.c_str());

    const auto& queue = cache.moderation_queue;
    if (queue.empty()) {
        empty_state("Queue is clear", "No projects are waiting for first publication review.", "✓");
        return;
    }
    for (const auto& project : queue) {
        ImGui::PushID(project.id.c_str());
        card_begin("##moderation_project", ImVec2(-1, ui_px(145.0f)));
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted(project.name.c_str());
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::TextColored(k.muted, "(%s)", project.type.c_str());
        ImGui::TextColored(k.muted, "Owner: %s", project.owner_id.c_str());
        ImGui::TextWrapped("%s", project.description.c_str());
        const std::string target_label = project.name.empty() ? project.id : project.name;
        if (primary_button("Approve", ImVec2(ui_px(100.0f), ui_px(30.0f)))) {
            request_admin_confirmation(
                st, AdminActionKind::ApproveProject, project.id, target_label,
                "Approval allows this project to publish future versions without repeat review.",
                false, false, true);
        }
        ImGui::SameLine();
        if (ghost_button("Reject", ImVec2(ui_px(100.0f), ui_px(30.0f)))) {
            request_admin_confirmation(
                st, AdminActionKind::RejectProject, project.id, target_label,
                "This rejects the current submission. The creator can revise and resubmit it.",
                true, true, true);
        }
        ImGui::SameLine();
        if (ghost_button("Take Down", ImVec2(ui_px(100.0f), ui_px(30.0f)))) {
            request_admin_confirmation(
                st, AdminActionKind::TakeDownProject, project.id, target_label,
                "This removes the project from public listing until it is reviewed again.",
                true, true, true);
        }
        card_end();
        ImGui::PopID();
        ImGui::Spacing();
    }
}

static void draw_admin_feedback(UiState& st) {
    page_title("Beta Feedback", "Review feedback submitted by beta testers.");
    if (auth_async_request_lane_busy(st)) {
        empty_state("Account activity in progress",
                    "Finish the current account operation before loading beta feedback.", "…");
        return;
    }
    auto* client = aml::supabase::SupabaseManager::instance().client();
    if (!client || !client->is_authenticated()) {
        empty_state("Sign in required", "Staff authentication is required to review feedback.", "@");
        return;
    }
    card_begin("##admin_feedback_header");
    ImGui::TextUnformatted("Tester feedback");
    ImGui::TextColored(k.muted, "Feedback is collected without passwords, tokens, or raw logs.");
    card_end();
    ImGui::Spacing();

    auto& cache = get_admin_ui_state().passive;
    auto& load_state = cache.feedback_state;
    request_admin_feedback(st);
    if (load_state.loading && !load_state.has_value) {
        empty_state("Loading beta feedback", "Retrieving feedback in the background. You can keep browsing.", "…");
        return;
    }
    if (!load_state.error.empty() && !load_state.has_value) {
        card_begin("##admin_feedback_error");
        ImGui::TextColored(k.red, "FEEDBACK UNAVAILABLE");
        ImGui::TextWrapped("%s", load_state.error.c_str());
        if (ghost_button("Try again", ImVec2(ui_px(110.0f), ui_px(30.0f))))
            request_admin_feedback(st, true);
        card_end();
        return;
    }
    if (load_state.loading || admin_passive_cache_is_stale(load_state))
        ImGui::TextColored(k.brand_hov, "Showing the last verified feedback while it refreshes.");
    if (!load_state.warning.empty())
        ImGui::TextColored(k.yellow, "%s", load_state.warning.c_str());
    const auto& feedback = cache.feedback_rows;
    if (feedback.empty()) {
        empty_state("No feedback yet", "Submitted beta feedback will appear here.", "✓");
        return;
    }
    if (ImGui::BeginTable("##admin_feedback_table", 6,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY,
                          ImVec2(0, ui_px(520.0f)))) {
        ImGui::TableSetupColumn("Category", 0, 0.12f);
        ImGui::TableSetupColumn("Rating", 0, 0.08f);
        ImGui::TableSetupColumn("Message", 0, 0.42f);
        ImGui::TableSetupColumn("Page", 0, 0.14f);
        ImGui::TableSetupColumn("Version", 0, 0.12f);
        ImGui::TableSetupColumn("Status", 0, 0.12f);
        ImGui::TableHeadersRow();
        for (const auto& row : feedback) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(row.get("category").as_str().c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%d/5", static_cast<int>(row.get("rating").as_int()));
            ImGui::TableSetColumnIndex(2);
            ImGui::TextWrapped("%s", row.get("message").as_str().c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::TextColored(k.muted, "%s", row.get("page").as_str().c_str());
            ImGui::TableSetColumnIndex(4);
            ImGui::TextColored(k.muted, "%s", row.get("app_version").as_str().c_str());
            ImGui::TableSetColumnIndex(5);
            ImGui::TextColored(k.yellow, "%s", row.get("status").as_str("open").c_str());
        }
        ImGui::EndTable();
    }
    (void)st;
}

// ---------------------------------------------------------------------------
// Admin Dashboard
// ---------------------------------------------------------------------------

void draw_admin_dashboard(UiState& st) {
    auto& admin_ui = get_admin_ui_state();
    auto& server_manager = aml::servers::ServerManager::instance();
    auto& cache = admin_ui.passive;
    auto& load_state = cache.dashboard_state;
    request_admin_dashboard(st);
    const bool account_operation_pending = auth_async_request_lane_busy(st);
    
    page_title("Admin Dashboard", "Review accounts and manage servers or nodes available to this staff session.");
    
    // Stats cards
    ImGui::BeginGroup();
    
    // Users card
    card_begin("##admin_stat_users");
    ImGui::TextUnformatted("Users");
    ImGui::PushFont(f_title);
    
    if (load_state.has_value) ImGui::Text("%d", cache.dashboard.user_count);
    else ImGui::TextUnformatted(load_state.loading ? "…" : "—");
    ImGui::PopFont();
    ImGui::TextColored(k.muted, "Total registered users");
    card_end();
    
    ImGui::EndGroup();
    ImGui::SameLine();
    
    ImGui::BeginGroup();
    
    // Servers card
    card_begin("##admin_stat_servers");
    ImGui::TextUnformatted("Servers");
    ImGui::PushFont(f_title);
    
    auto server_stats = server_manager.get_stats();
    ImGui::Text("%d", server_stats.total_servers);
    ImGui::PopFont();
    ImGui::TextColored(k.muted, "%d running", server_stats.running);
    card_end();
    
    ImGui::EndGroup();
    ImGui::SameLine();
    
    ImGui::BeginGroup();
    
    // Nodes card
    card_begin("##admin_stat_nodes");
    ImGui::TextUnformatted("Nodes");
    ImGui::PushFont(f_title);
    
    auto node_stats = server_manager.get_node_stats();
    ImGui::Text("%d", node_stats.total_nodes);
    ImGui::PopFont();
    ImGui::TextColored(k.muted, "%d online", node_stats.online);
    card_end();
    
    ImGui::EndGroup();
    ImGui::SameLine();
    
    ImGui::BeginGroup();
    
    // Storage card
    card_begin("##admin_stat_storage");
    ImGui::TextUnformatted("Storage");
    ImGui::PushFont(f_title);
    
    const auto storage_stats = cache.dashboard.storage;
    if (load_state.has_value) ImGui::Text("%s", format_bytes(storage_stats.total_size_bytes).c_str());
    else ImGui::TextUnformatted(load_state.loading ? "…" : "—");
    ImGui::PopFont();
    if (load_state.has_value) {
        ImGui::TextColored(k.muted, "%d files", storage_stats.total_files);
    } else {
        ImGui::TextColored(k.muted, load_state.loading
            ? "Loading storage summary"
            : "Storage summary unavailable");
    }
    card_end();
    
    ImGui::EndGroup();
    
    ImGui::Spacing();
    
    // System status
    card_begin("##admin_system_status");
    ImGui::TextUnformatted("System Status");
    ImGui::Separator();
    ImGui::Spacing();
    
    // Check system health
    auto sync_stats = aml::sync::SyncManager::instance().get_stats();
    
    ImGui::TextUnformatted("Sync Status");
    if (sync_stats.is_online) {
        ImGui::TextColored(k.green, "✓ Online");
    } else {
        ImGui::TextColored(k.red, "✗ Offline");
    }
    ImGui::SameLine();
    ImGui::TextColored(k.muted, " - Last sync: %s", 
                     sync_stats.last_sync_time > 0 ? 
                     format_date(sync_stats.last_sync_time).c_str() : "Never");
    
    ImGui::Spacing();
    
    ImGui::TextUnformatted("Database Connection");
    if (account_operation_pending) {
        ImGui::TextColored(k.yellow, "• Account activity in progress");
    } else if (aml::supabase::SupabaseManager::instance().is_authenticated()) {
        ImGui::TextColored(k.green, "✓ Connected");
    } else {
        ImGui::TextColored(k.red, "✗ Disconnected");
    }
    
    ImGui::Spacing();
    
    if (ghost_button(load_state.loading ? "Refreshing..." : "Refresh Stats",
                     ImVec2(ui_px(120.0f), ui_px(32.0f)), load_state.loading)) {
        admin_ui.last_refresh = std::time(nullptr);
        request_admin_dashboard(st, true);
    }
    
    card_end();
    
    ImGui::Spacing();
    
    // Recent activity
    card_begin("##admin_recent_activity");
    ImGui::TextUnformatted("Recent Activity");
    ImGui::Separator();
    ImGui::Spacing();
    
    int shown_activity = 0;
    if (load_state.has_value && !cache.dashboard.activity_rows.empty()) {
        for (const auto& row : cache.dashboard.activity_rows) {
            const int64_t timestamp = activity_timestamp(row);
            const std::string text = activity_text(row);
            if (timestamp <= 0 || text.empty()) continue;
            std::string actor = row.get("username").as_str();
            if (actor.empty()) actor = row.get("email").as_str();
            if (actor.empty()) actor = row.get("user_id").as_str();
            ImGui::TextColored(k.muted, "%s", format_date(timestamp).c_str());
            ImGui::SameLine();
            ImGui::TextWrapped("%s%s%s", actor.empty() ? "" : actor.c_str(),
                               actor.empty() ? "" : ": ", text.c_str());
            ++shown_activity;
        }
    }
    if (shown_activity == 0) {
        if (load_state.loading && !load_state.has_value) {
            ImGui::TextColored(k.muted, "Loading recent activity in the background…");
        } else if (!load_state.error.empty()) {
            ImGui::TextColored(k.red, "%s", load_state.error.c_str());
        } else if (!load_state.warning.empty()) {
            ImGui::TextColored(k.yellow, "%s", load_state.warning.c_str());
        } else {
            ImGui::TextColored(k.muted, "No activity has been recorded.");
        }
    }

    if (load_state.has_value && (load_state.loading || admin_passive_cache_is_stale(load_state)))
        ImGui::TextColored(k.brand_hov, "Showing the last verified dashboard snapshot while it refreshes.");
    
    card_end();
}

// ---------------------------------------------------------------------------
// Admin Users Tab
// ---------------------------------------------------------------------------

void draw_admin_users(UiState& st) {
    auto& admin_ui = get_admin_ui_state();
    auto& cache = admin_ui.passive;
    auto& load_state = cache.users_state;
    request_admin_users(st);
    
    page_title("User Management", "Manage Amalgam launcher users");
    
    card_begin("##admin_users_header");
    
    ImGui::TextUnformatted("Users");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - ui_px(200.0f));
    if (ghost_button(load_state.loading ? "Refreshing..." : "Refresh",
                     ImVec2(ui_px(90.0f), ui_px(30.0f)), load_state.loading))
        request_admin_users(st, true);
    ImGui::SameLine();
    ImGui::TextColored(k.muted, "User invitations are managed by Supabase Auth.");
    
    ImGui::Spacing();
    
    // Filter and sort
    ImGui::SetNextItemWidth(ui_px(240.0f));
    input_text_hint("##admin_user_filter", "Filter users...", &admin_ui.user_filter);
    ImGui::SameLine();
    
    ImGui::SetNextItemWidth(ui_px(160.0f));
    const char* sort_options[] = {"Username (A-Z)", "Email (A-Z)", "Recently Created", "Recently Active"};
    if (ImGui::BeginCombo("##admin_user_sort", sort_options[admin_ui.user_sort])) {
        for (int i = 0; i < 4; ++i) {
            if (ImGui::Selectable(sort_options[i], admin_ui.user_sort == i)) {
                admin_ui.user_sort = i;
            }
        }
        ImGui::EndCombo();
    }
    
    card_end();
    
    ImGui::Spacing();
    
    // User list is a cached render-thread copy. The RPC is initiated only on
    // first entry or explicit refresh, never while the table is being drawn.
    if (load_state.loading && !load_state.has_value) {
        empty_state("Loading users", "Retrieving users in the background. You can keep browsing.", "…");
        return;
    }
    if (!load_state.error.empty() && !load_state.has_value) {
        card_begin("##admin_users_error");
        ImGui::TextColored(k.red, "USERS UNAVAILABLE");
        ImGui::TextWrapped("%s", load_state.error.c_str());
        if (ghost_button("Try again", ImVec2(ui_px(110.0f), ui_px(30.0f))))
            request_admin_users(st, true);
        card_end();
        return;
    }
    if (load_state.loading || admin_passive_cache_is_stale(load_state))
        ImGui::TextColored(k.brand_hov, "Showing the last verified user list while it refreshes.");
    if (!load_state.warning.empty())
        ImGui::TextColored(k.yellow, "%s", load_state.warning.c_str());
    const auto& users = cache.users;
    if (users.empty()) {
        empty_state("No Users", "No users have registered yet.", "U");
        return;
    }
    
    // Filter users
    auto filtered_users = users;
    if (!admin_ui.user_filter.empty()) {
        std::string filter = admin_ui.user_filter;
        std::transform(filter.begin(), filter.end(), filter.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        
        filtered_users.erase(std::remove_if(filtered_users.begin(), filtered_users.end(),
            [&filter](const auto& user) {
                std::string username = user.username;
                std::string email = user.email;
                std::transform(username.begin(), username.end(), username.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                std::transform(email.begin(), email.end(), email.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                return username.find(filter) == std::string::npos && 
                       email.find(filter) == std::string::npos;
            }), filtered_users.end());
    }
    
    // Sort users
    std::sort(filtered_users.begin(), filtered_users.end(),
        [&admin_ui](const auto& a, const auto& b) {
            switch (admin_ui.user_sort) {
                case 1: return a.email < b.email; // Email (A-Z)
                case 2: return a.created_at > b.created_at; // Recently Created
                case 3: return a.last_login_at > b.last_login_at; // Recently Active
                default: return a.username < b.username; // Username (A-Z)
            }
        });
    
    // Display users
    for (auto& user : filtered_users) {
        ImGui::PushID(user.id.c_str());
        
        card_begin(("##admin_user_" + user.id).c_str(), ImVec2(-1, ui_px(80.0f)));
        
        ImGui::BeginGroup();
        
        // User info
        ImGui::Text("%s", user.username.c_str());
        ImGui::TextColored(k.muted, "%s", user.email.c_str());
        
        // User stats
        ImGui::TextColored(k.muted, "Created: %s", format_date(user.created_at).c_str());
        ImGui::TextColored(k.muted, "Last Login: %s", format_date(user.last_login_at).c_str());
        
        ImGui::EndGroup();
        
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - ui_px(200.0f));
        
        ImGui::BeginGroup();
        
        ImGui::TextColored(k.muted, "Profile edits are self-service");
        
        ImGui::SameLine();
        
        ImGui::TextColored(k.muted, "Account deletion requires an audited Auth operation");
        
        ImGui::EndGroup();
        
        card_end();
        ImGui::PopID();
    }
}

// ---------------------------------------------------------------------------
// Admin Servers Tab
// ---------------------------------------------------------------------------

void draw_admin_servers(UiState& st) {
    auto& admin_ui = get_admin_ui_state();
    auto& server_manager = aml::servers::ServerManager::instance();
    
    page_title("Server Management", "Manage all user servers");
    
    card_begin("##admin_servers_header");
    
    ImGui::TextUnformatted("Servers");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - ui_px(200.0f));
    
    ImGui::TextColored(k.muted, "Server creation is temporarily unavailable.");
    
    ImGui::Spacing();
    
    // Filter and sort
    ImGui::SetNextItemWidth(ui_px(240.0f));
    input_text_hint("##admin_server_filter", "Filter servers...", &admin_ui.server_filter);
    ImGui::SameLine();
    
    ImGui::SetNextItemWidth(ui_px(160.0f));
    const char* sort_options[] = {"Name (A-Z)", "Status", "Recently Created", "Most Players"};
    if (ImGui::BeginCombo("##admin_server_sort", sort_options[admin_ui.server_sort])) {
        for (int i = 0; i < 4; ++i) {
            if (ImGui::Selectable(sort_options[i], admin_ui.server_sort == i)) {
                admin_ui.server_sort = i;
            }
        }
        ImGui::EndCombo();
    }
    
    card_end();
    
    ImGui::Spacing();
    
    // Get all servers
    auto servers = server_manager.get_servers();
    
    if (servers.empty()) {
        empty_state("No Servers", "No servers have been created yet.", "S");
        return;
    }
    
    // Filter servers
    auto filtered_servers = servers;
    if (!admin_ui.server_filter.empty()) {
        std::string filter = admin_ui.server_filter;
        std::transform(filter.begin(), filter.end(), filter.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        
        filtered_servers.erase(std::remove_if(filtered_servers.begin(), filtered_servers.end(),
            [&filter](const auto& server) {
                std::string name = server.name;
                std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                return name.find(filter) == std::string::npos;
            }), filtered_servers.end());
    }
    
    // Sort servers
    std::sort(filtered_servers.begin(), filtered_servers.end(),
        [&admin_ui](const auto& a, const auto& b) {
            switch (admin_ui.server_sort) {
                case 1: return a.status < b.status; // Status
                case 2: return a.created_at > b.created_at; // Recently Created
                case 3: return a.current_players > b.current_players; // Most Players
                default: return a.name < b.name; // Name (A-Z)
            }
        });
    
    // Display servers
    for (auto& server : filtered_servers) {
        ImGui::PushID(server.id.c_str());
        
        card_begin(("##admin_server_" + server.id).c_str(), ImVec2(-1, ui_px(90.0f)));
        
        ImGui::BeginGroup();
        
        // Server info
        ImGui::Text("%s", server.name.c_str());
        ImGui::TextColored(k.muted, "Type: %s", server.type.c_str());
        ImGui::TextColored(k.muted, "Version: %s", server.version.c_str());
        
        // Server status
        ImGui::TextColored(k.muted, "Status:");
        ImGui::SameLine();
        switch (server.status) {
            case aml::servers::ServerStatus::Running:
                ImGui::TextColored(k.green, "Running");
                break;
            case aml::servers::ServerStatus::Stopped:
                ImGui::TextColored(k.muted, "Stopped");
                break;
            case aml::servers::ServerStatus::Starting:
                ImGui::TextColored(k.yellow, "Starting");
                break;
            case aml::servers::ServerStatus::Stopping:
                ImGui::TextColored(k.yellow, "Stopping");
                break;
            case aml::servers::ServerStatus::Error:
                ImGui::TextColored(k.red, "Error");
                break;
            default:
                ImGui::TextColored(k.muted, "Unknown");
                break;
        }
        
        ImGui::TextColored(k.muted, "Players: %d/%d", server.current_players, server.max_players);
        
        ImGui::EndGroup();
        
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - ui_px(200.0f));
        
        ImGui::BeginGroup();
        const std::string target_label = server.name.empty() ? server.id : server.name;
        
        // Start/Stop button
        if (server.status == aml::servers::ServerStatus::Running) {
            if (ghost_button("Stop", ImVec2(ui_px(80.0f), ui_px(28.0f)))) {
                request_admin_confirmation(
                    st, AdminActionKind::StopServer, server.id, target_label,
                    "Stopping this server interrupts connected players and may take a moment.",
                    false, false, true);
            }
        } else {
            if (ghost_button("Start", ImVec2(ui_px(80.0f), ui_px(28.0f)))) {
                request_admin_confirmation(
                    st, AdminActionKind::StartServer, server.id, target_label,
                    "Starting this server submits a control-plane request and may take a moment to become available.",
                    false, false, true);
            }
        }
        
        ImGui::SameLine();
        
        // More button
        if (icon_button(IconId::More, ImVec2(ui_px(30.0f), ui_px(28.0f)),
                        "More server actions")) {
            ImGui::OpenPopup(("##admin_server_menu_" + server.id).c_str());
        }
        
        // Server menu
        if (ImGui::BeginPopup(("##admin_server_menu_" + server.id).c_str())) {
            if (ImGui::MenuItem("Restart")) {
                request_admin_confirmation(
                    st, AdminActionKind::RestartServer, server.id, target_label,
                    "Restarting this server interrupts connected players before it returns to service.",
                    false, false, true);
            }
            
            if (ImGui::MenuItem("Delete", nullptr, false, true)) {
                request_admin_confirmation(
                    st, AdminActionKind::DeleteServer, server.id, target_label,
                    "This removes the server record from the Amalgam control plane. It does not claim to delete provider or runtime files.",
                    false, true, true);
            }
            
            ImGui::EndPopup();
        }
        
        ImGui::EndGroup();
        
        card_end();
        ImGui::PopID();
    }
}

// ---------------------------------------------------------------------------
// Admin Nodes Tab
// ---------------------------------------------------------------------------

void draw_admin_nodes(UiState& st) {
    auto& admin_ui = get_admin_ui_state();
    auto& server_manager = aml::servers::ServerManager::instance();
    
    page_title("Node Management", "Manage server nodes and clusters");
    
    card_begin("##admin_nodes_header");
    
    ImGui::TextUnformatted("Nodes");
    ImGui::SameLine();
    if (ghost_button("Check all nodes", ImVec2(ui_px(136.0f), ui_px(28.0f)))) {
        request_admin_confirmation(
            st, AdminActionKind::CheckAllNodes, "all-nodes", "all registered nodes",
            "This requests a health refresh for every registered node, not just the filtered list.",
            false, false, true);
    }
    ImGui::TextColored(k.muted, "Node registration is temporarily unavailable.");
    
    ImGui::Spacing();
    
    // Filter and sort
    ImGui::SetNextItemWidth(ui_px(240.0f));
    input_text_hint("##admin_node_filter", "Filter nodes...", &admin_ui.node_filter);
    ImGui::SameLine();
    
    ImGui::SetNextItemWidth(ui_px(160.0f));
    const char* sort_options[] = {"Name (A-Z)", "Status", "CPU Usage", "Memory Usage"};
    if (ImGui::BeginCombo("##admin_node_sort", sort_options[admin_ui.node_sort])) {
        for (int i = 0; i < 4; ++i) {
            if (ImGui::Selectable(sort_options[i], admin_ui.node_sort == i)) {
                admin_ui.node_sort = i;
            }
        }
        ImGui::EndCombo();
    }
    
    card_end();
    
    ImGui::Spacing();
    
    // Get all nodes
    auto nodes = server_manager.get_nodes();
    
    if (nodes.empty()) {
        empty_state("No Nodes", "No server nodes have been registered yet.", "N");
        return;
    }
    
    // Filter nodes
    auto filtered_nodes = nodes;
    if (!admin_ui.node_filter.empty()) {
        std::string filter = admin_ui.node_filter;
        std::transform(filter.begin(), filter.end(), filter.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        
        filtered_nodes.erase(std::remove_if(filtered_nodes.begin(), filtered_nodes.end(),
            [&filter](const auto& node) {
                std::string name = node.name;
                std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                return name.find(filter) == std::string::npos;
            }), filtered_nodes.end());
    }
    
    // Sort nodes
    std::sort(filtered_nodes.begin(), filtered_nodes.end(),
        [&admin_ui](const auto& a, const auto& b) {
            switch (admin_ui.node_sort) {
                case 1: return a.status < b.status; // Status
                case 2: {
                    const bool a_available = has_authoritative_metrics(a);
                    const bool b_available = has_authoritative_metrics(b);
                    if (a_available != b_available) return a_available > b_available;
                    return a.cpu_usage > b.cpu_usage; // CPU Usage (descending)
                }
                case 3: {
                    const bool a_available = has_authoritative_metrics(a);
                    const bool b_available = has_authoritative_metrics(b);
                    if (a_available != b_available) return a_available > b_available;
                    return a.memory_usage > b.memory_usage; // Memory Usage (descending)
                }
                default: return a.name < b.name; // Name (A-Z)
            }
        });
    
    // Display nodes
    for (auto& node : filtered_nodes) {
        ImGui::PushID(node.id.c_str());
        
        card_begin(("##admin_node_" + node.id).c_str(), ImVec2(-1, ui_px(100.0f)));
        
        ImGui::BeginGroup();
        
        // Node info
        ImGui::Text("%s", node.name.c_str());
        ImGui::TextColored(k.muted, "Host: %s:%d", node.host.c_str(), node.port);
        
        // Node status
        ImGui::TextColored(k.muted, "Status:");
        ImGui::SameLine();
        switch (node.status) {
            case aml::servers::NodeStatus::Online:
                ImGui::TextColored(k.green, "Online");
                break;
            case aml::servers::NodeStatus::Offline:
                ImGui::TextColored(k.red, "Offline");
                break;
            case aml::servers::NodeStatus::Maintenance:
                ImGui::TextColored(k.yellow, "Maintenance");
                break;
            case aml::servers::NodeStatus::Overloaded:
                ImGui::TextColored(k.red, "Overloaded");
                break;
            default:
                ImGui::TextColored(k.muted, "Unknown");
                break;
        }
        
        // Resource usage
        if (has_authoritative_metrics(node)) {
            ImGui::TextColored(k.muted, "CPU: %.1f%%", node.cpu_usage * 100);
            ImGui::TextColored(k.muted, "Memory: %.1f%%", node.memory_usage * 100);
            ImGui::TextColored(k.muted, "Storage: %.1f%%", node.storage_usage * 100);
        } else {
            ImGui::TextColored(k.muted, "CPU: Unavailable");
            ImGui::TextColored(k.muted, "Memory: Unavailable");
            ImGui::TextColored(k.muted, "Storage: Unavailable");
        }
        
        // Hardware specs
        ImGui::TextColored(k.muted, "%d CPU cores | %d MB RAM | %d GB Storage",
                         node.cpu_cores, node.memory_mb, node.storage_gb);
        
        ImGui::EndGroup();
        
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - ui_px(150.0f));
        
        ImGui::BeginGroup();
        
        ImGui::TextColored(k.muted, "Health checks apply to all nodes.");
        ImGui::SameLine();
        
        // More button
        if (icon_button(IconId::More, ImVec2(ui_px(30.0f), ui_px(28.0f)),
                        "More node actions")) {
            ImGui::OpenPopup(("##admin_node_menu_" + node.id).c_str());
        }
        
        // Node menu
        if (ImGui::BeginPopup(("##admin_node_menu_" + node.id).c_str())) {
            if (ImGui::MenuItem("Deregister", nullptr, false, true)) {
                const std::string target_label = node.name.empty() ? node.id : node.name;
                request_admin_confirmation(
                    st, AdminActionKind::DeregisterNode, node.id, target_label,
                    "This removes the node from the Amalgam control plane. Existing capacity or routing may be affected.",
                    false, true, true);
            }
            
            ImGui::EndPopup();
        }
        
        ImGui::EndGroup();
        
        card_end();
        ImGui::PopID();
    }
}

// ---------------------------------------------------------------------------
// Admin Storage Tab
// ---------------------------------------------------------------------------

void draw_admin_storage(UiState& st) {
    auto& admin_ui = get_admin_ui_state();
    auto& cache = admin_ui.passive;
    auto& load_state = cache.storage_state;
    request_admin_storage(st);
    
    page_title("Storage Management", "Manage storage buckets and files");
    
    card_begin("##admin_storage_header");
    
    ImGui::TextUnformatted("Storage");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - ui_px(200.0f));
    if (ghost_button(load_state.loading ? "Refreshing..." : "Refresh",
                     ImVec2(ui_px(90.0f), ui_px(30.0f)), load_state.loading))
        request_admin_storage(st, true);
    ImGui::SameLine();
    ImGui::TextColored(k.muted, "Storage buckets are managed by the deployment environment.");
    
    ImGui::Spacing();
    
    // Filter
    ImGui::SetNextItemWidth(ui_px(240.0f));
    input_text_hint("##admin_storage_filter", "Filter buckets...", &admin_ui.storage_bucket_filter);
    
    card_end();
    
    ImGui::Spacing();
    
    // Bucket availability is fetched through the serialized background lane;
    // this render path only filters the last verified snapshot.
    if (load_state.loading && !load_state.has_value) {
        empty_state("Loading storage", "Retrieving bucket availability in the background. You can keep browsing.", "…");
        return;
    }
    if (!load_state.error.empty() && !load_state.has_value) {
        card_begin("##admin_storage_error");
        ImGui::TextColored(k.red, "STORAGE UNAVAILABLE");
        ImGui::TextWrapped("%s", load_state.error.c_str());
        if (ghost_button("Try again", ImVec2(ui_px(110.0f), ui_px(30.0f))))
            request_admin_storage(st, true);
        card_end();
        return;
    }
    if (load_state.loading || admin_passive_cache_is_stale(load_state))
        ImGui::TextColored(k.brand_hov, "Showing the last verified storage snapshot while it refreshes.");
    if (!load_state.warning.empty())
        ImGui::TextColored(k.yellow, "%s", load_state.warning.c_str());
    const auto& buckets = cache.storage_buckets;
    if (buckets.empty()) {
        empty_state("No Buckets", "No storage buckets have been created yet.", "B");
        return;
    }
    
    // Filter buckets
    auto filtered_buckets = buckets;
    if (!admin_ui.storage_bucket_filter.empty()) {
        std::string filter = admin_ui.storage_bucket_filter;
        std::transform(filter.begin(), filter.end(), filter.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        
        filtered_buckets.erase(std::remove_if(filtered_buckets.begin(), filtered_buckets.end(),
            [&filter](const auto& bucket) {
                std::string name = bucket.name;
                std::string description = bucket.description;
                std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                std::transform(description.begin(), description.end(), description.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                return name.find(filter) == std::string::npos && 
                       description.find(filter) == std::string::npos;
            }), filtered_buckets.end());
    }
    
    // Display buckets
    for (auto& bucket : filtered_buckets) {
        ImGui::PushID(bucket.name.c_str());
        
        card_begin(("##admin_bucket_" + bucket.name).c_str(), ImVec2(-1, ui_px(80.0f)));
        
        ImGui::BeginGroup();
        
        // Bucket info
        ImGui::Text("%s", bucket.name.c_str());
        ImGui::TextColored(k.muted, "%s", bucket.description.c_str());
        
        // Bucket stats
        ImGui::TextColored(k.muted, "Created: %s", format_date(bucket.created_at).c_str());
        if (bucket.public_access) {
            ImGui::TextColored(k.green, "Public Access");
        } else {
            ImGui::TextColored(k.muted, "Private Access");
        }
        
        ImGui::EndGroup();
        
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - ui_px(150.0f));
        
        ImGui::BeginGroup();
        
        // File browsing is intentionally withheld until storage permissions
        // and pagination are exposed through the deployment control plane.
        ImGui::TextColored(k.muted, "File browser unavailable");
        
        ImGui::SameLine();
        
        // More button
        if (icon_button(IconId::More, ImVec2(ui_px(30.0f), ui_px(28.0f)),
                        "More storage actions")) {
            ImGui::OpenPopup(("##admin_bucket_menu_" + bucket.name).c_str());
        }
        
        // Bucket menu
        if (ImGui::BeginPopup(("##admin_bucket_menu_" + bucket.name).c_str())) {
            ImGui::MenuItem("Delete unavailable", nullptr, false, false);
            ImGui::TextColored(k.muted,
                               "Bucket deletion is managed by the deployment environment.");
            
            ImGui::EndPopup();
        }
        
        ImGui::EndGroup();
        
        card_end();
        ImGui::PopID();
    }
}

// ---------------------------------------------------------------------------
// Admin Settings Tab
// ---------------------------------------------------------------------------

void draw_admin_settings_tab(UiState& st) {
    auto& admin_ui = get_admin_ui_state();
    (void)st;
    
    page_title("Admin Settings", "Configure Amalgam launcher administration");
    
    card_begin("##admin_settings_general");
    ImGui::TextUnformatted("General Settings");
    ImGui::Separator();
    ImGui::Spacing();
    
    // Maintenance mode
    ImGui::Checkbox("Maintenance Mode", &admin_ui.maintenance_mode);
    ImGui::TextColored(k.muted, "Enable maintenance mode to prevent new logins and show a maintenance message");
    
    if (admin_ui.maintenance_mode) {
        ImGui::Spacing();
        ImGui::TextUnformatted("Maintenance Message");
        ImGui::SetNextItemWidth(ui_px(400.0f));
        ImGui::InputTextMultiline("##admin_maintenance_message", &admin_ui.maintenance_message,
                                 ImVec2(ui_px(400.0f), ui_px(100.0f)));
        ImGui::TextColored(k.muted, "Message to display to users during maintenance");
    }
    
    card_end();
    
    ImGui::Spacing();
    
    card_begin("##admin_settings_limits");
    ImGui::TextUnformatted("Resource Limits");
    ImGui::Separator();
    ImGui::Spacing();
    
    // Max servers per user
    ImGui::TextUnformatted("Max Servers per User");
    ImGui::SetNextItemWidth(ui_px(100.0f));
    ImGui::InputInt("##admin_max_servers", &admin_ui.max_servers_per_user);
    ImGui::TextColored(k.muted, "Maximum number of servers each user can create");
    
    ImGui::Spacing();
    
    // Max nodes per user
    ImGui::TextUnformatted("Max Nodes per User");
    ImGui::SetNextItemWidth(ui_px(100.0f));
    ImGui::InputInt("##admin_max_nodes", &admin_ui.max_nodes_per_user);
    ImGui::TextColored(k.muted, "Maximum number of nodes each user can register");
    
    ImGui::Spacing();
    
    // Max storage per user
    ImGui::TextUnformatted("Max Storage per User (MB)");
    ImGui::SetNextItemWidth(ui_px(100.0f));
    int max_storage_mb = static_cast<int>(admin_ui.max_storage_per_user / (1024 * 1024));
    if (ImGui::InputInt("##admin_max_storage", &max_storage_mb)) {
        admin_ui.max_storage_per_user = static_cast<uint64_t>(max_storage_mb) * 1024 * 1024;
    }
    ImGui::TextColored(k.muted, "Maximum storage space each user can use (%s)",
                     format_bytes(admin_ui.max_storage_per_user).c_str());
    
    card_end();
    
    ImGui::Spacing();
    
    card_begin("##admin_settings_actions");
    ImGui::TextUnformatted("Admin Actions");
    ImGui::Separator();
    ImGui::Spacing();
    
    ImGui::TextColored(k.muted,
                       "Admin settings persistence is unavailable; changes apply only to this launcher session.");
    
    if (ghost_button("Reset to Defaults", ImVec2(ui_px(150.0f), ui_px(36.0f)))) {
        // Reset to defaults
        admin_ui.maintenance_mode = false;
        admin_ui.maintenance_message.clear();
        admin_ui.max_servers_per_user = 10;
        admin_ui.max_nodes_per_user = 5;
        admin_ui.max_storage_per_user = 1024 * 1024 * 1024;
    }
    
    card_end();
}

// ---------------------------------------------------------------------------
// Admin Main Tab
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Visual-fixture admin facade
// ---------------------------------------------------------------------------
//
// The regular Admin pages intentionally own live Supabase, server, storage,
// sync, and publishing integrations.  Snapshot fixtures must not even
// construct those service singletons: release evidence has to be deterministic
// and must never expose a reviewer's staff role, server data, or storage
// metadata.  Keep the local facade here, ahead of draw_admin_page(), so the
// fixture branch can return before the live page path is considered.

static int fixture_admin_tab_for_case(const std::string& fixture_case) {
    if (fixture_case == "admin-login") return -1;
    if (fixture_case == "admin-users") return 1;
    if (fixture_case == "admin-servers" ||
        fixture_case == "admin-delete-server-confirm" ||
        fixture_case == "admin-delete-server-error" ||
        fixture_case == "admin-stop-server-confirm" ||
        fixture_case == "admin-stop-server-error" ||
        fixture_case == "admin-restart-server-confirm" ||
        fixture_case == "admin-restart-server-error" ||
        fixture_case == "admin-server-actions-menu") return 2;
    if (fixture_case == "admin-nodes" ||
        fixture_case == "admin-deregister-node-confirm" ||
        fixture_case == "admin-deregister-node-error" ||
        fixture_case == "admin-check-all-nodes-confirm" ||
        fixture_case == "admin-node-actions-menu") return 3;
    if (fixture_case == "admin-storage" ||
        fixture_case == "admin-storage-bucket-menu") return 4;
    if (fixture_case == "admin-settings") return 5;
    if (fixture_case == "admin-project-review" ||
        fixture_case == "admin-project-approve-confirm" ||
        fixture_case == "admin-project-approve-error" ||
        fixture_case == "admin-project-reject-confirm" ||
        fixture_case == "admin-project-reject-error" ||
        fixture_case == "admin-project-takedown-confirm" ||
        fixture_case == "admin-project-takedown-error") return 6;
    if (fixture_case == "admin-feedback") return 7;
    // Both "admin" and "admin-dashboard" intentionally land on Dashboard.
    return 0;
}

// This is deliberately independent from AdminPendingAction.  The fixture page
// returns before the live Admin path, so these copied literals cannot construct
// a service singleton, retain a live target, or submit an administrative action.
struct FixtureAdminConfirmation {
    const char* title;
    const char* target_label;
    const char* impact_summary;
    const char* confirmation_label;
    const char* sample_reason;
    const char* sample_typed_target;
    const char* error;
    const char* acknowledgement_label;
    bool requires_reason;
    bool requires_typed_target;
    bool requires_acknowledgement;
    bool is_dangerous;
};

static const FixtureAdminConfirmation* fixture_admin_confirmation_for_case(
    const std::string& fixture_case) {
    static const FixtureAdminConfirmation kDeleteServerConfirm = {
        "Delete server",
        "Northstar SMP",
        "This removes the server record from the Amalgam control plane. It does not claim to delete "
        "provider or runtime files.",
        "Delete permanently",
        "",
        "",
        "",
        "I understand this removes the server control-plane record.",
        false, true, true, true,
    };
    static const FixtureAdminConfirmation kProjectRejectConfirm = {
        "Reject project",
        "Aurora Atlas",
        "This rejects the current submission. The creator can revise and resubmit it.",
        "Reject project",
        "The sample submission needs a compatible dependency declaration before review can continue.",
        "",
        "",
        "I understand this changes the project's publication status.",
        true, true, true, true,
    };
    static const FixtureAdminConfirmation kDeleteServerError = {
        "Delete server",
        "Northstar SMP",
        "This removes the server record from the Amalgam control plane. It does not claim to delete "
        "provider or runtime files.",
        "Delete permanently",
        "",
        "Northstar SMP",
        "The sample server could not be deleted. It remains unchanged in this local fixture.",
        "I understand this removes the server control-plane record.",
        false, true, true, true,
    };
    static const FixtureAdminConfirmation kStopServerConfirm = {
        "Stop server",
        "Northstar SMP",
        "Stopping this server interrupts connected players and may take a moment.",
        "Stop server",
        "",
        "",
        "",
        "I understand this interrupts connected players.",
        false, false, true, false,
    };
    static const FixtureAdminConfirmation kStopServerError = {
        "Stop server",
        "Northstar SMP",
        "Stopping this server interrupts connected players and may take a moment.",
        "Stop server",
        "",
        "",
        "The sample server could not be stopped. It remains unchanged in this local fixture.",
        "I understand this interrupts connected players.",
        false, false, true, false,
    };
    static const FixtureAdminConfirmation kRestartServerConfirm = {
        "Restart server",
        "Northstar SMP",
        "Restarting this server interrupts connected players before it returns to service.",
        "Restart server",
        "",
        "",
        "",
        "I understand this interrupts connected players.",
        false, false, true, false,
    };
    static const FixtureAdminConfirmation kRestartServerError = {
        "Restart server",
        "Northstar SMP",
        "Restarting this server interrupts connected players before it returns to service.",
        "Restart server",
        "",
        "",
        "The sample server could not be restarted. It remains unchanged in this local fixture.",
        "I understand this interrupts connected players.",
        false, false, true, false,
    };
    static const FixtureAdminConfirmation kDeregisterNodeConfirm = {
        "Deregister node",
        "fixture-west-01",
        "This removes the node from the Amalgam control plane. Existing capacity or routing may be affected.",
        "Deregister node",
        "",
        "",
        "",
        "I understand this can affect capacity and routing.",
        false, true, true, true,
    };
    static const FixtureAdminConfirmation kDeregisterNodeError = {
        "Deregister node",
        "fixture-west-01",
        "This removes the node from the Amalgam control plane. Existing capacity or routing may be affected.",
        "Deregister node",
        "",
        "fixture-west-01",
        "The sample node could not be deregistered. It remains unchanged in this local fixture.",
        "I understand this can affect capacity and routing.",
        false, true, true, true,
    };
    static const FixtureAdminConfirmation kCheckAllNodesConfirm = {
        "Check all nodes",
        "all registered nodes",
        "This requests a health refresh for every registered node, not just the filtered list.",
        "Request checks",
        "",
        "",
        "",
        "I understand this sends health checks to every registered node.",
        false, false, true, false,
    };
    static const FixtureAdminConfirmation kProjectApproveConfirm = {
        "Approve project",
        "Aurora Atlas",
        "Approval allows this project to publish future versions without repeat review.",
        "Approve project",
        "",
        "",
        "",
        "I understand this makes the project eligible for public release.",
        false, false, true, false,
    };
    static const FixtureAdminConfirmation kProjectApproveError = {
        "Approve project",
        "Aurora Atlas",
        "Approval allows this project to publish future versions without repeat review.",
        "Approve project",
        "",
        "",
        "The sample approval could not be recorded. The project remains unchanged in this local fixture.",
        "I understand this makes the project eligible for public release.",
        false, false, true, false,
    };
    static const FixtureAdminConfirmation kProjectRejectError = {
        "Reject project",
        "Aurora Atlas",
        "This rejects the current submission. The creator can revise and resubmit it.",
        "Reject project",
        "The sample submission needs a compatible dependency declaration before review can continue.",
        "Aurora Atlas",
        "The sample rejection could not be recorded. The project remains unchanged in this local fixture.",
        "I understand this changes the project's publication status.",
        true, true, true, true,
    };
    static const FixtureAdminConfirmation kProjectTakeDownConfirm = {
        "Take down project",
        "Aurora Atlas",
        "This removes the project from public listing until it is reviewed again.",
        "Take down project",
        "The sample listing needs a policy review before it can return to public discovery.",
        "",
        "",
        "I understand this changes the project's publication status.",
        true, true, true, true,
    };
    static const FixtureAdminConfirmation kProjectTakeDownError = {
        "Take down project",
        "Aurora Atlas",
        "This removes the project from public listing until it is reviewed again.",
        "Take down project",
        "The sample listing needs a policy review before it can return to public discovery.",
        "Aurora Atlas",
        "The sample takedown could not be recorded. The project remains unchanged in this local fixture.",
        "I understand this changes the project's publication status.",
        true, true, true, true,
    };

    if (fixture_case == "admin-delete-server-confirm") return &kDeleteServerConfirm;
    if (fixture_case == "admin-stop-server-confirm") return &kStopServerConfirm;
    if (fixture_case == "admin-stop-server-error") return &kStopServerError;
    if (fixture_case == "admin-restart-server-confirm") return &kRestartServerConfirm;
    if (fixture_case == "admin-restart-server-error") return &kRestartServerError;
    if (fixture_case == "admin-deregister-node-confirm") return &kDeregisterNodeConfirm;
    if (fixture_case == "admin-deregister-node-error") return &kDeregisterNodeError;
    if (fixture_case == "admin-check-all-nodes-confirm") return &kCheckAllNodesConfirm;
    if (fixture_case == "admin-project-approve-confirm") return &kProjectApproveConfirm;
    if (fixture_case == "admin-project-approve-error") return &kProjectApproveError;
    if (fixture_case == "admin-project-reject-confirm") return &kProjectRejectConfirm;
    if (fixture_case == "admin-project-reject-error") return &kProjectRejectError;
    if (fixture_case == "admin-project-takedown-confirm") return &kProjectTakeDownConfirm;
    if (fixture_case == "admin-project-takedown-error") return &kProjectTakeDownError;
    if (fixture_case == "admin-delete-server-error") return &kDeleteServerError;
    return nullptr;
}

static void draw_fixture_admin_confirmation(const UiState& st) {
    const auto* presentation = fixture_admin_confirmation_for_case(st.fixture_case);
    // A locally closed preview stays closed until the fixture route changes;
    // that makes the Close control useful without ever touching live UI state.
    static std::string dismissed_fixture_case;
    if (!presentation) {
        dismissed_fixture_case.clear();
        return;
    }

    static constexpr const char* kFixtureAdminConfirmationPopup =
        "Administrative action preview##fixture_admin_confirmation";
    if (dismissed_fixture_case != st.fixture_case) {
        ImGui::OpenPopup(kFixtureAdminConfirmationPopup);
    }

    ImGui::SetNextWindowSize(ImVec2(ui_px(520.0f), 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(kFixtureAdminConfirmationPopup, nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    ImGui::PushFont(f_h2);
    ImGui::TextColored(presentation->is_dangerous ? k.red : k.text, "%s", presentation->title);
    ImGui::PopFont();
    ImGui::TextColored(k.muted, "Target: %s", presentation->target_label);
    ImGui::Spacing();
    ImGui::TextWrapped("%s", presentation->impact_summary);
    ImGui::Spacing();
    ImGui::TextColored(k.blue, "LOCAL VISUAL-QA FIXTURE — no request can be submitted.");
    ImGui::TextColored(k.muted,
                       "All example fields and the final action are intentionally disabled.");
    ImGui::Spacing();

    // These frame-local values intentionally model the form's visual states
    // only.  They are never copied into AdminPendingAction or any backend call.
    std::string reason = presentation->sample_reason;
    std::string typed_target = presentation->sample_typed_target;
    bool acknowledged = presentation->error[0] != '\0';

    if (presentation->requires_reason) {
        ImGui::TextUnformatted("Reason");
        ImGui::BeginDisabled(true);
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextMultiline("##fixture_admin_action_reason", &reason,
                                  ImVec2(-1, ui_px(76.0f)));
        ImGui::EndDisabled();
        ImGui::TextColored(k.muted,
                           "A reason is required and would be included with the moderation decision.");
        ImGui::Spacing();
    }
    if (presentation->requires_typed_target) {
        ImGui::TextColored(k.red, "Type \"%s\" to enable the final action.",
                           presentation->target_label);
        ImGui::BeginDisabled(true);
        ImGui::SetNextItemWidth(-1);
        input_text_hint("##fixture_admin_action_typed_target", "Type the exact target name",
                        &typed_target);
        ImGui::EndDisabled();
        ImGui::Spacing();
    }
    if (presentation->requires_acknowledgement) {
        ImGui::BeginDisabled(true);
        ImGui::Checkbox(presentation->acknowledgement_label, &acknowledged);
        ImGui::EndDisabled();
        ImGui::Spacing();
    }
    if (presentation->error[0] != '\0') {
        ImGui::TextColored(k.red, "%s", presentation->error);
        ImGui::Spacing();
    }

    ImGui::Separator();
    ImGui::Spacing();
    if (ghost_button("Close preview", ImVec2(ui_px(132.0f), ui_px(32.0f)))) {
        dismissed_fixture_case = st.fixture_case;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (presentation->is_dangerous) {
        danger_button(presentation->confirmation_label,
                      ImVec2(ui_px(180.0f), ui_px(32.0f)), true);
    } else {
        primary_button(presentation->confirmation_label,
                       ImVec2(ui_px(180.0f), ui_px(32.0f)), false, true);
    }
    ImGui::EndPopup();
}

// These three menus mirror the local action affordances visible in the live
// Admin tabs without constructing a server, node, or storage service.  Their
// routes exist solely to make the final capture ledger prove that the menu
// hierarchy and unavailable-action language remain legible at each viewport.
struct FixtureAdminActionMenu {
    const char* popup_id;
    const char* title;
    const char* target_label;
    const char* unavailable_note;
    const char* primary_action;
    const char* secondary_action;
};

static const FixtureAdminActionMenu* fixture_admin_action_menu_for_case(
    const std::string& fixture_case) {
    static const FixtureAdminActionMenu kServerActions = {
        "Server actions###fixture_admin_server_actions",
        "Server actions",
        "Northstar SMP",
        "No server action can run during local visual review.",
        "Restart",
        "Delete",
    };
    static const FixtureAdminActionMenu kNodeActions = {
        "Node actions###fixture_admin_node_actions",
        "Node actions",
        "fixture-west-01",
        "No node action can run during local visual review.",
        "Deregister",
        "Request health check",
    };
    static const FixtureAdminActionMenu kStorageActions = {
        "Storage actions###fixture_admin_storage_actions",
        "Storage actions",
        "fixture-backups",
        "Storage actions remain managed by the deployment environment.",
        "Browse files unavailable",
        "Delete unavailable",
    };

    if (fixture_case == "admin-server-actions-menu") return &kServerActions;
    if (fixture_case == "admin-node-actions-menu") return &kNodeActions;
    if (fixture_case == "admin-storage-bucket-menu") return &kStorageActions;
    return nullptr;
}

static void draw_fixture_admin_action_menu(const UiState& st) {
    const auto* menu = fixture_admin_action_menu_for_case(st.fixture_case);
    if (!menu) return;

    // Keep the fixture menu inside the visible work area instead of depending
    // on a live row's context-menu anchor.  This makes it stable at the two
    // host-valid capture sizes while retaining its relationship to the Admin
    // page behind it.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 owner_pos = ImGui::GetWindowPos();
    const ImVec2 owner_size = ImGui::GetWindowSize();
    const float popup_width = ui_px(274.0f);
    const float popup_height = ui_px(184.0f);
    ImVec2 popup_pos(owner_pos.x + owner_size.x - popup_width - ui_px(22.0f),
                      owner_pos.y + ui_px(212.0f));
    popup_pos.x = std::clamp(popup_pos.x, viewport->WorkPos.x + ui_px(12.0f),
                             viewport->WorkPos.x + viewport->WorkSize.x - popup_width - ui_px(12.0f));
    popup_pos.y = std::clamp(popup_pos.y, viewport->WorkPos.y + ui_px(12.0f),
                             viewport->WorkPos.y + viewport->WorkSize.y - popup_height - ui_px(12.0f));

    ImGui::SetNextWindowPos(popup_pos, ImGuiCond_Appearing);
    ImGui::OpenPopup(menu->popup_id);
    if (!ImGui::BeginPopup(menu->popup_id,
                           ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }

    ImGui::TextUnformatted(menu->title);
    ImGui::TextColored(k.muted, "%s — local fixture preview", menu->target_label);
    ImGui::Separator();
    ImGui::BeginDisabled(true);
    ImGui::MenuItem(menu->primary_action);
    ImGui::MenuItem(menu->secondary_action);
    ImGui::EndDisabled();
    ImGui::Separator();
    ImGui::TextColored(k.blue, "LOCAL VISUAL-QA FIXTURE — actions disabled");
    ImGui::TextWrapped("%s", menu->unavailable_note);
    ImGui::EndPopup();
}

static void draw_fixture_admin_notice() {
    card_begin("##fixture_admin_notice");
    ImGui::PushFont(f_bold);
    ImGui::TextColored(k.blue, "LOCAL VISUAL-QA FIXTURE");
    ImGui::PopFont();
    ImGui::TextWrapped(
        "Representative sample content only. No staff role, Supabase data, server "
        "manager, sync service, storage service, or publishing queue is read.");
    ImGui::Spacing();
    draw_status_indicator(k.blue, "Controls are intentionally inert in this capture.");
    card_end();
}

static void draw_fixture_admin_tabs(int active_tab) {
    static const char* kTabs[] = {
        "Dashboard", "Users", "Servers", "Nodes", "Storage", "Settings",
        "Project Review", "Beta Feedback"
    };
    constexpr int kTabCount = static_cast<int>(sizeof(kTabs) / sizeof(kTabs[0]));

    if (!ImGui::BeginTabBar("##fixture_admin_tabs")) return;
    for (int i = 0; i < kTabCount; ++i) {
        const ImGuiTabItemFlags flags =
            active_tab == i ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
        if (ImGui::BeginTabItem(kTabs[i], nullptr, flags)) {
            ImGui::EndTabItem();
        }
    }
    ImGui::EndTabBar();
}

static void draw_fixture_admin_metrics() {
    struct FixtureMetric {
        const char* label;
        const char* value;
        ImVec4 accent;
    };
    static const FixtureMetric kMetrics[] = {
        {"Sample accounts", "2,480", k.brand},
        {"Sample servers", "18", k.green},
        {"Sample nodes", "6", k.blue},
        {"Fixture storage", "1.6 GB", k.orange},
    };
    constexpr int kMetricCount = static_cast<int>(sizeof(kMetrics) / sizeof(kMetrics[0]));

    const float available = ImGui::GetContentRegionAvail().x;
    const int columns = available >= ui_px(980.0f) ? 4 :
                        (available >= ui_px(560.0f) ? 2 : 1);
    const float gap = ui_px(10.0f);
    const float width = std::max(ui_px(140.0f),
        (available - gap * static_cast<float>(columns - 1)) /
        static_cast<float>(columns));

    for (int i = 0; i < kMetricCount; ++i) {
        draw_stat_card(kMetrics[i].label, kMetrics[i].value, -1.0f,
                       kMetrics[i].accent, width);
        if ((i + 1) % columns != 0 && i + 1 < kMetricCount) {
            ImGui::SameLine(0, gap);
        }
    }
}

static void draw_fixture_admin_dashboard() {
    draw_fixture_admin_metrics();
    ImGui::Spacing();

    card_begin("##fixture_admin_dashboard_health");
    ImGui::TextUnformatted("Review status");
    ImGui::Separator();
    ImGui::Spacing();
    draw_status_indicator(k.blue, "Fixture isolated — no live health check was requested.");
    ImGui::TextColored(k.muted,
                       "The sample metrics above exist only to exercise density, hierarchy, and responsive layout.");
    ImGui::Spacing();
    ghost_button("Refresh sample", ImVec2(ui_px(136.0f), ui_px(32.0f)), true);
    card_end();
    ImGui::Spacing();

    card_begin("##fixture_admin_dashboard_activity");
    ImGui::TextUnformatted("Sample review activity");
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextUnformatted("Fixture run prepared the Administration review surface.");
    ImGui::TextColored(k.muted, "Local sample event • no audit log was queried");
    ImGui::Spacing();
    ImGui::TextUnformatted("Responsive verification is pending the capture ledger.");
    ImGui::TextColored(k.muted, "Local sample event • controls disabled");
    card_end();
}

static void draw_fixture_admin_users() {
    card_begin("##fixture_admin_users");
    ImGui::TextUnformatted("Sample accounts");
    ImGui::SameLine();
    ImGui::TextColored(k.muted, "Not loaded from Supabase");
    ImGui::Spacing();
    if (ImGui::BeginTable("##fixture_admin_users_table", 4,
                          ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Display name", 0, 0.30f);
        ImGui::TableSetupColumn("Email", 0, 0.34f);
        ImGui::TableSetupColumn("Role", 0, 0.18f);
        ImGui::TableSetupColumn("Review state", 0, 0.18f);
        ImGui::TableHeadersRow();
        const char* rows[][4] = {
            {"Alex Morgan", "preview@amalgam.test", "Member", "Sample"},
            {"Jordan Lee", "reviewer@amalgam.test", "Moderator", "Sample"},
            {"Mina Patel", "creator@amalgam.test", "Creator", "Sample"},
        };
        for (const auto& row : rows) {
            ImGui::TableNextRow();
            for (int col = 0; col < 4; ++col) {
                ImGui::TableSetColumnIndex(col);
                ImGui::TextColored(col == 3 ? k.blue : k.text, "%s", row[col]);
            }
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    ghost_button("Manage sample account", ImVec2(ui_px(184.0f), ui_px(32.0f)), true);
    card_end();
}

static void draw_fixture_admin_servers() {
    card_begin("##fixture_admin_servers");
    ImGui::TextUnformatted("Sample server inventory");
    ImGui::TextColored(k.muted, "This does not query ServerManager or a cloud control plane.");
    ImGui::Spacing();
    if (ImGui::BeginTable("##fixture_admin_servers_table", 4,
                          ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Name", 0, 0.36f);
        ImGui::TableSetupColumn("Software", 0, 0.20f);
        ImGui::TableSetupColumn("State", 0, 0.20f);
        ImGui::TableSetupColumn("Scope", 0, 0.24f);
        ImGui::TableHeadersRow();
        const char* rows[][4] = {
            {"Northstar SMP", "Fabric 1.21.1", "Sample running", "Local fixture"},
            {"Creative Lab", "Paper 1.21.1", "Sample stopped", "Local fixture"},
            {"Archive Realm", "Vanilla 1.20.4", "Sample standby", "Local fixture"},
        };
        for (const auto& row : rows) {
            ImGui::TableNextRow();
            for (int col = 0; col < 4; ++col) {
                ImGui::TableSetColumnIndex(col);
                const ImVec4 color = col == 2 ? k.blue : (col == 3 ? k.muted : k.text);
                ImGui::TextColored(color, "%s", row[col]);
            }
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    primary_button("Create sample server", ImVec2(ui_px(180.0f), ui_px(32.0f)),
                   false, true);
    card_end();
}

static void draw_fixture_admin_nodes() {
    card_begin("##fixture_admin_nodes");
    ImGui::TextUnformatted("Sample node inventory");
    ImGui::TextColored(k.muted, "No node agent, host telemetry, or capacity value was queried.");
    ImGui::Spacing();
    if (ImGui::BeginTable("##fixture_admin_nodes_table", 4,
                          ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Node", 0, 0.30f);
        ImGui::TableSetupColumn("Region", 0, 0.24f);
        ImGui::TableSetupColumn("State", 0, 0.24f);
        ImGui::TableSetupColumn("Capacity", 0, 0.22f);
        ImGui::TableHeadersRow();
        const char* rows[][4] = {
            {"fixture-west-01", "US West", "Sample ready", "Not queried"},
            {"fixture-east-02", "US East", "Sample standby", "Not queried"},
            {"fixture-eu-01", "Europe", "Sample offline", "Not queried"},
        };
        for (const auto& row : rows) {
            ImGui::TableNextRow();
            for (int col = 0; col < 4; ++col) {
                ImGui::TableSetColumnIndex(col);
                ImGui::TextColored(col >= 2 ? k.muted : k.text, "%s", row[col]);
            }
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    ghost_button("Refresh node inventory", ImVec2(ui_px(186.0f), ui_px(32.0f)), true);
    card_end();
}

static void draw_fixture_admin_storage() {
    draw_fixture_admin_metrics();
    ImGui::Spacing();
    card_begin("##fixture_admin_storage");
    ImGui::TextUnformatted("Sample storage areas");
    ImGui::TextColored(k.muted, "StorageManager is not initialized in visual-fixture mode.");
    ImGui::Spacing();
    draw_meta_line("Fixture profiles", "0.9 GB sample allocation");
    draw_meta_line("Fixture media", "0.5 GB sample allocation");
    draw_meta_line("Fixture backups", "0.2 GB sample allocation");
    ImGui::Spacing();
    danger_button("Delete sample bucket", ImVec2(ui_px(176.0f), ui_px(32.0f)), true);
    card_end();
}

static void draw_fixture_admin_settings() {
    card_begin("##fixture_admin_settings");
    ImGui::TextUnformatted("Local review settings");
    ImGui::TextColored(k.muted,
                       "These fields demonstrate the normal hierarchy only; nothing is persisted or applied.");
    ImGui::Spacing();
    bool maintenance_mode = false;
    int server_limit = 10;
    int node_limit = 5;
    ImGui::BeginDisabled(true);
    ImGui::Checkbox("Maintenance mode", &maintenance_mode);
    ImGui::SetNextItemWidth(ui_px(120.0f));
    ImGui::InputInt("Max servers per user", &server_limit);
    ImGui::SetNextItemWidth(ui_px(120.0f));
    ImGui::InputInt("Max nodes per user", &node_limit);
    ImGui::EndDisabled();
    ImGui::Spacing();
    ghost_button("Reset sample values", ImVec2(ui_px(170.0f), ui_px(32.0f)), true);
    card_end();
}

static void draw_fixture_admin_project_review() {
    card_begin("##fixture_admin_project_review");
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted("Aurora Atlas");
    ImGui::PopFont();
    ImGui::TextColored(k.muted, "Sample project • Creator preview@amalgam.test");
    ImGui::TextWrapped(
        "A representative review card used to verify content hierarchy and destructive-action separation. "
        "No publication queue is loaded and no decision can be submitted.");
    ImGui::Spacing();
    primary_button("Approve sample", ImVec2(ui_px(136.0f), ui_px(32.0f)), false, true);
    ImGui::SameLine();
    ghost_button("Request changes", ImVec2(ui_px(154.0f), ui_px(32.0f)), true);
    ImGui::SameLine();
    danger_button("Take down", ImVec2(ui_px(122.0f), ui_px(32.0f)), true);
    card_end();
}

static void draw_fixture_admin_feedback() {
    card_begin("##fixture_admin_feedback");
    ImGui::TextUnformatted("Sample feedback");
    ImGui::TextColored(k.muted, "No beta-feedback table or player-submitted content was read.");
    ImGui::Spacing();
    if (ImGui::BeginTable("##fixture_admin_feedback_table", 4,
                          ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Category", 0, 0.20f);
        ImGui::TableSetupColumn("Feedback", 0, 0.46f);
        ImGui::TableSetupColumn("Page", 0, 0.18f);
        ImGui::TableSetupColumn("State", 0, 0.16f);
        ImGui::TableHeadersRow();
        const char* rows[][4] = {
            {"Visual polish", "The content filters were easy to understand.", "Profile Content", "Sample"},
            {"Navigation", "The compact tab layout stayed readable.", "Servers", "Sample"},
            {"Accessibility", "Review focus order in the live pass.", "Settings", "Sample"},
        };
        for (const auto& row : rows) {
            ImGui::TableNextRow();
            for (int col = 0; col < 4; ++col) {
                ImGui::TableSetColumnIndex(col);
                if (col == 1) ImGui::TextWrapped("%s", row[col]);
                else ImGui::TextColored(col == 3 ? k.blue : k.text, "%s", row[col]);
            }
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    ghost_button("Export sample review", ImVec2(ui_px(174.0f), ui_px(32.0f)), true);
    card_end();
}

static void draw_fixture_admin_login() {
    page_title("Administration", "Permission-gate fixture for visual review");
    draw_fixture_admin_notice();
    ImGui::Spacing();
    card_begin("##fixture_admin_login");
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted("Staff access is not evaluated here");
    ImGui::PopFont();
    ImGui::TextWrapped(
        "This fixture renders the permission-gate experience without reading a local password, "
        "an Amalgam account, or a staff role. Use a separate live, authorized test for access control.");
    ImGui::Spacing();
    primary_button("Sign in as staff", ImVec2(ui_px(148.0f), ui_px(34.0f)), false, true);
    ImGui::SameLine();
    ghost_button("Request access", ImVec2(ui_px(142.0f), ui_px(34.0f)), true);
    card_end();
}

static void draw_fixture_admin_page(UiState& st) {
    const int active_tab = fixture_admin_tab_for_case(st.fixture_case);
    if (active_tab < 0) {
        draw_fixture_admin_login();
        return;
    }

    static const char* kTitles[] = {
        "Admin Dashboard", "Users", "Servers", "Nodes", "Storage", "Admin Settings",
        "Project Review", "Beta Feedback"
    };
    static const char* kSubtitles[] = {
        "Local sample layout for visual review",
        "Local sample account records for layout review",
        "Local sample server records for layout review",
        "Local sample node inventory for layout review",
        "Local sample capacity layout for visual review",
        "Representative controls are disabled for visual review",
        "Local sample queue item for visual review",
        "Local sample feedback for table and density review"
    };
    constexpr int kFixtureTabCount = static_cast<int>(sizeof(kTitles) / sizeof(kTitles[0]));
    const int bounded_tab = std::clamp(active_tab, 0, kFixtureTabCount - 1);
    page_title(kTitles[bounded_tab], kSubtitles[bounded_tab]);
    draw_fixture_admin_notice();
    ImGui::Spacing();
    draw_fixture_admin_tabs(bounded_tab);
    ImGui::Spacing();

    switch (bounded_tab) {
        case 0: draw_fixture_admin_dashboard(); break;
        case 1: draw_fixture_admin_users(); break;
        case 2: draw_fixture_admin_servers(); break;
        case 3: draw_fixture_admin_nodes(); break;
        case 4: draw_fixture_admin_storage(); break;
        case 5: draw_fixture_admin_settings(); break;
        case 6: draw_fixture_admin_project_review(); break;
        case 7: draw_fixture_admin_feedback(); break;
        default: draw_fixture_admin_dashboard(); break;
    }

    // This remains inside the fixture-only branch above draw_admin_page's
    // service-free early return; both overlays are visual presenters, not
    // action paths.
    draw_fixture_admin_confirmation(st);
    draw_fixture_admin_action_menu(st);
}

void draw_admin_page(UiState& st) {
    // The fixture path is deliberately first: no Admin backend/service
    // singleton may be initialized while producing a deterministic snapshot.
    if (st.fixture_mode) {
        draw_fixture_admin_page(st);
        return;
    }

    // Revalidate a server-issued staff role on its throttled background cadence
    // even while the dashboard is already unlocked. Server-side RLS remains
    // authoritative, and the local session gate follows role revocation too.
    request_admin_staff_access_check(st);

    auto& admin_ui = get_admin_ui_state();
    const uint64_t now = GetTickCount64();
    if (st.admin_unlocked && st.admin_unlock_until_ms != 0 && now >= st.admin_unlock_until_ms) {
        st.admin_unlocked = false;
        st.admin_unlock_until_ms = 0;
        st.admin_password.clear();
        st.admin_password_confirm.clear();
        st.admin_status = "Admin session expired after 15 minutes.";
        clear_admin_pending_action();
    }

    // Check if admin is unlocked after enforcing expiry on every live frame.
    if (!st.admin_unlocked) {
        // Show admin login
        draw_admin_login(st);
        return;
    }
    
    // Admin tabs
    ImGui::BeginTabBar("##admin_tabs");
    
    if (ImGui::BeginTabItem("Dashboard", nullptr,
                            st.fixture_mode && admin_ui.current_tab == 0
                                ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None)) {
        admin_ui.current_tab = 0;
        ImGui::EndTabItem();
    }
    
    if (ImGui::BeginTabItem("Users", nullptr,
                            st.fixture_mode && admin_ui.current_tab == 1
                                ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None)) {
        admin_ui.current_tab = 1;
        ImGui::EndTabItem();
    }
    
    if (ImGui::BeginTabItem("Servers", nullptr,
                            st.fixture_mode && admin_ui.current_tab == 2
                                ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None)) {
        admin_ui.current_tab = 2;
        ImGui::EndTabItem();
    }
    
    if (ImGui::BeginTabItem("Nodes", nullptr,
                            st.fixture_mode && admin_ui.current_tab == 3
                                ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None)) {
        admin_ui.current_tab = 3;
        ImGui::EndTabItem();
    }
    
    if (ImGui::BeginTabItem("Storage", nullptr,
                            st.fixture_mode && admin_ui.current_tab == 4
                                ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None)) {
        admin_ui.current_tab = 4;
        ImGui::EndTabItem();
    }
    
    if (ImGui::BeginTabItem("Settings", nullptr,
                            st.fixture_mode && admin_ui.current_tab == 5
                                ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None)) {
        admin_ui.current_tab = 5;
        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Project Review", nullptr,
                            st.fixture_mode && admin_ui.current_tab == 6
                                ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None)) {
        admin_ui.current_tab = 6;
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Beta Feedback", nullptr,
                            st.fixture_mode && admin_ui.current_tab == 7
                                ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None)) {
        admin_ui.current_tab = 7;
        ImGui::EndTabItem();
    }
    
    ImGui::EndTabBar();
    
    ImGui::Spacing();
    
    // Draw current tab
    switch (admin_ui.current_tab) {
        case 0:
        default:
            draw_admin_dashboard(st);
            break;
        case 1:
            draw_admin_users(st);
            break;
        case 2:
            draw_admin_servers(st);
            break;
        case 3:
            draw_admin_nodes(st);
            break;
        case 4:
            draw_admin_storage(st);
            break;
        case 5:
            draw_admin_settings_tab(st);
            break;
        case 6:
            draw_admin_moderation(st);
            break;
        case 7:
            draw_admin_feedback(st);
            break;
    }
    draw_admin_confirmation_dialog(st);
}

// ---------------------------------------------------------------------------
// Admin Login
// ---------------------------------------------------------------------------

void draw_admin_login(UiState& st) {
    config::Config& c = *st.cfg;
    const uint64_t now = GetTickCount64();

    // Use the authenticated Supabase staff role when available. The local
    // password remains an offline fallback; email strings are never trusted by
    // the client as an authorization mechanism.
    if (st.admin_unlocked && now >= st.admin_unlock_until_ms) {
        st.admin_unlocked = false;
        st.admin_password.clear();
        st.admin_password_confirm.clear();
        st.admin_status = "Admin session expired after 15 minutes.";
    }

    request_admin_staff_access_check(st);
    // A staff-role RPC, sign-in, or token refresh may now be running. Avoid
    // reading the mutable provider client until that one account operation is
    // complete; the login card instead presents an explicit safe loading state.
    const bool account_operation_pending = auth_async_request_lane_busy(st);
    const bool staff_check_pending = admin_staff_access_check_in_progress(st);
    
    card_begin("##adminaccess", ImVec2(-1, 0));
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted("Admin control center");
    ImGui::PopFont();
    ImGui::TextColored(k.muted,
                       "Publisher and credential controls are local to this Windows account. They are never included in exported profiles or release packages.");
    ImGui::Spacing();
    
    if (!admin_auth::configured(c) && !st.admin_unlocked) {
        if (account_operation_pending) {
            ImGui::TextColored(k.brand_hov, staff_check_pending
                ? "Checking staff access securely…"
                : "Account activity in progress…");
            ImGui::TextColored(k.muted,
                               staff_check_pending
                                   ? "Your launcher stays responsive while the account service verifies this role."
                                   : "Your launcher stays responsive while the account service finishes its current operation.");
        } else {
            auto* client = aml::supabase::SupabaseManager::instance().client();
            if (client && client->is_authenticated()) {
                ImGui::TextColored(k.yellow, "Staff role required.");
                ImGui::TextWrapped(
                    "This beta does not create local Admin passwords. Admin access is granted server-side to approved staff accounts.");
                ImGui::TextColored(k.muted, "Signed in account: %s", client->current_user().email.c_str());
                ImGui::Spacing();
                ImGui::TextColored(k.muted,
                                   "Ask the project owner to assign this account in public.staff_roles, then reload the page.");
                if (ghost_button("Refresh staff access", ImVec2(ui_px(170.0f), ui_px(32.0f))))
                    request_admin_staff_access_check(st, true);
            } else {
                ImGui::TextColored(k.yellow, "Amalgam account sign-in required.");
                ImGui::TextWrapped("Sign into your Amalgam account before requesting staff access.");
            }
        }
    } else {
        ImGui::SetNextItemWidth(auto_item_width(320.0f, 180.0f));
        input_secret("##admin_password", &st.admin_password);
        ImGui::SameLine();
        if (ghost_button("Unlock", ImVec2(ui_px(100.0f), ui_px(34.0f)))) {
            if (admin_auth::verify_password(c, st.admin_password)) {
                st.admin_unlocked = true;
                st.admin_unlock_until_ms = now + 15ull * 60ull * 1000ull;
                note_admin_local_password_unlock();
                st.admin_password.clear();
                st.admin_status = "Admin access enabled for this session.";
            } else {
                st.admin_password.clear();
                st.admin_status = "Admin password is incorrect.";
            }
        }
    }
    
    if (!st.admin_status.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(k.muted, "%s", st.admin_status.c_str());
    }
    
    card_end();
}

// ---------------------------------------------------------------------------
// Helper Functions
}  // namespace aml::ui
