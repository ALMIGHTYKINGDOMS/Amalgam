#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

namespace aml::supabase { class SupabaseManager; }

namespace aml::entitlements {

// ---------------------------------------------------------------------------
// AmalgamEntitlements
//
// Capability-based entitlements fetched from the backend.  The backend is the
// authoritative source; the launcher never hardcodes "isPremium = true".
// All resource limits are backend-supplied so plans can change remotely.
// ---------------------------------------------------------------------------

struct AmalgamEntitlements {
    std::string plan;               // "free" | "amalgam_plus" | ... (backend)
    std::string plan_label;         // "FREE" | "AMALGAM+" (display)

    bool ads_enabled = false;       // free: true, Amalgam+: false
    bool premium_themes = false;
    bool premium_essentials = false;
    bool cloud_sync = false;
    bool cloud_backups = false;
    bool early_access = false;

    // Quotas / limits (backend authoritative)
    uint64_t turn_monthly_bytes = 0;
    uint64_t cloud_storage_bytes = 0;
    int max_cloud_servers = 0;
    int hosting_discount_percent = 0;

    // TURN billing cycle reset (Unix seconds) — for the usage meter.
    int64_t turn_reset_at = 0;

    // Current usage (display only; backend re-checks before authorizing).
    uint64_t turn_used_bytes = 0;
    uint64_t cloud_storage_used_bytes = 0;
    int cloud_servers_used = 0;

    bool valid = false;             // false until first successful fetch
};

// ---------------------------------------------------------------------------
// EntitlementManager
//
// Caches entitlements and refreshes on an interval (default 10 minutes) or on
// demand.  Never blocks the UI thread: callers use try_refresh() which fires
// an async fetch guarded by a mutex; the worker writes results back.
// ---------------------------------------------------------------------------

class EntitlementManager {
public:
    static EntitlementManager& instance();

    // Fetch entitlements from the backend using the caller's access token.
    // Returns true when the cached copy is fresh (or fetch succeeded).
    bool refresh(const std::string& access_token, std::string* err = nullptr);

    // Non-blocking refresh attempt. Returns immediately; the worker updates
    // the cache. Call from the UI thread.
    void request_refresh(const std::string& access_token);

    // Refresh entitlements from Supabase subscription tables synchronized from
    // Whop billing. The backend remains authoritative for relay credentials;
    // this cache is display-only and never enforces access.
    void request_refresh_from_supabase();

    // Snapshot of the current cached entitlements (thread-safe).
    AmalgamEntitlements snapshot() const;

    // True when a refresh is currently in flight.
    bool refreshing() const;

    // Force the cache to be treated as stale (e.g. after returning from the
    // website checkout page).
    void invalidate();

    // Convenience helpers
    bool is_plus() const {
        const auto e = snapshot();
        // Only the backend's explicit active plan identifier grants the
        // Amalgam+ presentation. Unknown/future plans remain non-premium until
        // their entitlement contract is understood by this client.
        return e.valid && e.plan == "amalgam_plus";
    }
    uint64_t turn_remaining_bytes() const {
        auto e = snapshot();
        return e.turn_monthly_bytes > e.turn_used_bytes
                   ? e.turn_monthly_bytes - e.turn_used_bytes : 0;
    }
    float turn_usage_fraction() const {
        auto e = snapshot();
        return e.turn_monthly_bytes > 0
                   ? static_cast<float>(static_cast<double>(e.turn_used_bytes) /
                                        e.turn_monthly_bytes)
                   : 0.0f;
    }

    // Default refresh interval (10 minutes).
    static constexpr std::chrono::minutes kRefreshInterval{10};

private:
    EntitlementManager() = default;

    mutable std::mutex mu_;
    AmalgamEntitlements cached_;
    std::atomic<bool> refreshing_{false};
    // Incremented whenever the signed-in identity changes. Detached network
    // workers compare this token before publishing results so a late response
    // from the previous account can never restore stale premium state.
    std::atomic<uint64_t> refresh_generation_{0};
    std::chrono::steady_clock::time_point last_refresh_{};
    std::chrono::steady_clock::time_point last_attempt_{};
    std::string last_error_;
};

}  // namespace aml::entitlements
