#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace aml::ui {

// Passive account surfaces may outlive a request that began for a previous
// signed-in identity.  The UI thread owns this cache; workers only publish an
// AsyncUiRequestResult, which the UI validates against this scope before it
// copies any data here.
struct AccountPassiveScope {
    std::string account_identity;
    uint64_t session_generation = 0;
};

inline bool account_passive_scope_matches(const AccountPassiveScope& scope,
                                          const std::string& account_identity,
                                          uint64_t session_generation) {
    return !account_identity.empty() &&
           scope.account_identity == account_identity &&
           scope.session_generation == session_generation;
}

struct AccountPassiveLoadState {
    bool attempted = false;
    bool loading = false;
    bool has_value = false;
    bool stale = false;
    bool empty = false;
    std::string error;
    std::string warning;
    uint64_t loaded_at_ms = 0;
    AccountPassiveScope request_scope;
};

struct AccountPassiveStats {
    int total_sessions = 0;
    int64_t account_created_at = 0;
    int64_t last_login_at = 0;
    bool current_session_active = false;
    int total_instances = 0;
    int total_modpacks = 0;
};

struct AccountPassiveSecurity {
    bool two_factor_enabled = false;
    std::string two_factor_method;
};

struct AccountPassiveActivity {
    std::string id;
    std::string type;
    std::string description;
    int64_t timestamp = 0;
};

struct AccountPassiveCache {
    AccountPassiveScope scope;
    uint64_t cache_generation = 0;
    AccountPassiveLoadState stats_state;
    AccountPassiveStats stats;
    AccountPassiveLoadState security_state;
    AccountPassiveSecurity security;
    AccountPassiveLoadState activity_state;
    std::vector<AccountPassiveActivity> activity;

    // Returns true when all cached values were invalidated for a different
    // Amalgam identity or credential generation.
    bool bind(const std::string& account_identity, uint64_t session_generation) {
        if (account_passive_scope_matches(scope, account_identity, session_generation)) {
            return false;
        }
        scope = {account_identity, session_generation};
        ++cache_generation;
        stats_state = {};
        stats = {};
        security_state = {};
        security = {};
        activity_state = {};
        activity.clear();
        return true;
    }

    void clear() {
        scope = {};
        ++cache_generation;
        stats_state = {};
        stats = {};
        security_state = {};
        security = {};
        activity_state = {};
        activity.clear();
    }

    void mark_values_stale() {
        if (stats_state.has_value) stats_state.stale = true;
        if (security_state.has_value) security_state.stale = true;
        if (activity_state.has_value) activity_state.stale = true;
    }
};

}  // namespace aml::ui
