#include "entitlements.h"
#include "json.h"
#include "net.h"
#include "online_config.h"
#include "supabase.h"

#include <thread>

namespace aml::entitlements {

// ---------------------------------------------------------------------------
// Response parsing
// ---------------------------------------------------------------------------

// The backend returns a JSON object. Keys follow the entitlements contract:
//   plan, planLabel, adsEnabled, premiumThemes, premiumEssentials,
//   cloudSync, cloudBackups, earlyAccess,
//   turnMonthlyBytes, cloudStorageBytes, maxCloudServers,
//   hostingDiscountPercent, turnResetAt,
//   usage: { turnUsedBytes, cloudStorageUsedBytes, cloudServersUsed }
// Unknown keys are ignored so the launcher stays forward-compatible.
static AmalgamEntitlements parse_entitlements(const Json& root) {
    AmalgamEntitlements e;
    e.valid = root.isObject();

    e.plan = root.get("plan").as_str("free");
    e.plan_label = root.get("planLabel").as_str(e.plan == "free" ? "FREE" : e.plan);
    e.ads_enabled = root.get("adsEnabled").as_bool(e.plan == "free");
    e.premium_themes = root.get("premiumThemes").as_bool(false);
    e.premium_essentials = root.get("premiumEssentials").as_bool(false);
    e.cloud_sync = root.get("cloudSync").as_bool(false);
    e.cloud_backups = root.get("cloudBackups").as_bool(false);
    e.early_access = root.get("earlyAccess").as_bool(false);

    e.turn_monthly_bytes = static_cast<uint64_t>(root.get("turnMonthlyBytes").as_int(0));
    e.cloud_storage_bytes = static_cast<uint64_t>(root.get("cloudStorageBytes").as_int(0));
    e.max_cloud_servers = static_cast<int>(root.get("maxCloudServers").as_int(0));
    e.hosting_discount_percent = static_cast<int>(root.get("hostingDiscountPercent").as_int(0));
    e.turn_reset_at = root.get("turnResetAt").as_int(0);

    const Json& usage = root.get("usage");
    if (usage.isObject()) {
        e.turn_used_bytes = static_cast<uint64_t>(usage.get("turnUsedBytes").as_int(0));
        e.cloud_storage_used_bytes =
            static_cast<uint64_t>(usage.get("cloudStorageUsedBytes").as_int(0));
        e.cloud_servers_used = static_cast<int>(usage.get("cloudServersUsed").as_int(0));
    }
    return e;
}

// ---------------------------------------------------------------------------
// EntitlementManager
// ---------------------------------------------------------------------------

EntitlementManager& EntitlementManager::instance() {
    static EntitlementManager manager;
    return manager;
}

bool EntitlementManager::refresh(const std::string& access_token, std::string* err) {
    const uint64_t generation = refresh_generation_.load();
    if (access_token.empty()) {
        if (err) *err = "Not signed in";
        return false;
    }

    const std::string api_url = online::config().api_url;
    if (api_url.empty()) {
        // No backend configured yet — keep the last known copy if valid, but
        // read it under the same lock used by worker publication.
        if (err) *err = "Online backend not configured";
        std::lock_guard<std::mutex> lock(mu_);
        return cached_.valid;
    }

    std::string request_url = api_url;
    if (request_url.back() == '/') request_url.pop_back();
    request_url += "/api/account/entitlements";

    std::vector<std::wstring> headers = {
        L"Authorization: Bearer " + net::to_wide(access_token),
        L"Content-Type: application/json",
        L"Accept: application/json",
    };

    std::vector<uint8_t> out;
    std::string fetch_err;
    if (!net::get_with_headers(net::to_wide(request_url), headers, out, &fetch_err)) {
        if (err) *err = fetch_err;
        return false;
    }

    const std::string body(out.begin(), out.end());
    std::string parse_err;
    Json root = Json::parse(body, &parse_err);
    if (!root.isObject()) {
        if (err) *err = parse_err.empty() ? "Invalid entitlements response" : parse_err;
        return false;
    }

    AmalgamEntitlements parsed = parse_entitlements(root);
    if (!parsed.valid) {
        if (err) *err = "Invalid entitlements payload";
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mu_);
        if (generation != refresh_generation_.load()) {
            if (err) *err = "Entitlement refresh superseded by an account change";
            return false;
        }
        cached_ = parsed;
        last_refresh_ = std::chrono::steady_clock::now();
        last_error_.clear();
    }
    return true;
}

void EntitlementManager::request_refresh(const std::string& access_token) {
    if (access_token.empty() || refreshing_.exchange(true)) return;
    {
        std::lock_guard<std::mutex> lock(mu_);
        const auto now = std::chrono::steady_clock::now();
        if (last_attempt_ != std::chrono::steady_clock::time_point{} &&
            now - last_attempt_ < kRefreshInterval) {
            refreshing_.store(false);
            return;
        }
        last_attempt_ = now;
    }

    const uint64_t generation = refresh_generation_.load();
    std::thread([this, access_token, generation]() {
        std::string err;
        if (generation == refresh_generation_.load()) {
            refresh(access_token, &err);
            std::lock_guard<std::mutex> lock(mu_);
            if (generation == refresh_generation_.load()) last_error_ = err;
        }
        refreshing_.store(false);
    }).detach();
}

AmalgamEntitlements EntitlementManager::snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    return cached_;
}

bool EntitlementManager::refreshing() const {
    return refreshing_.load();
}

void EntitlementManager::invalidate() {
    refresh_generation_.fetch_add(1);
    std::lock_guard<std::mutex> lock(mu_);
    cached_ = AmalgamEntitlements{};
    last_refresh_ = {};
    last_attempt_ = {};
    last_error_.clear();
}

void EntitlementManager::request_refresh_from_supabase() {
    if (!aml::supabase::SupabaseManager::instance().is_authenticated() ||
        refreshing_.exchange(true)) return;
    {
        std::lock_guard<std::mutex> lock(mu_);
        const auto now = std::chrono::steady_clock::now();
        if (last_attempt_ != std::chrono::steady_clock::time_point{} &&
            now - last_attempt_ < kRefreshInterval) {
            refreshing_.store(false);
            return;
        }
        last_attempt_ = now;
    }

    const uint64_t generation = refresh_generation_.load();
    std::thread([this, generation]() {
        auto& supabase = aml::supabase::SupabaseManager::instance();
        if (!supabase.is_authenticated()) {
            // No Supabase session — try the API backend instead
            refreshing_.store(false);
            return;
        }

        // Query subscription from Supabase (synced from Whop). An empty
        // successful result means a real FREE account; a failed query must
        // remain unknown so an outage is never presented as a plan change.
        auto sub = supabase.get_subscription();
        auto turn = supabase.get_turn_usage();
        if (!sub.query_succeeded) {
            std::lock_guard<std::mutex> lock(mu_);
            if (generation == refresh_generation_.load())
                last_error_ = "Membership data is temporarily unavailable";
            refreshing_.store(false);
            return;
        }

        AmalgamEntitlements e;
        e.valid = true;

        // Derive entitlements from Supabase subscription
        if (sub.valid && sub.status == "active") {
            const bool is_plus = sub.plan_id == "amalgam_plus";
            e.plan = sub.plan_id;
            e.plan_label = sub.plan_label.empty()
                ? (is_plus ? "AMALGAM+" : sub.plan_id)
                : sub.plan_label;
            e.ads_enabled = !is_plus;
            e.premium_themes = is_plus;
            e.premium_essentials = is_plus;
            e.cloud_sync = is_plus;
            e.early_access = is_plus;

            // TURN allowance: 80 GB for Amalgam+, 0 for free
            e.turn_monthly_bytes = is_plus
                ? 80ULL * 1024ULL * 1024ULL * 1024ULL : 0;
            e.turn_reset_at = sub.current_period_end;
        } else {
            e.plan = "free";
            e.plan_label = "FREE";
            e.ads_enabled = true;
            e.turn_monthly_bytes = 0;
        }

        // Apply actual usage from Supabase
        if (turn.valid) {
            e.turn_used_bytes = turn.used_bytes;
            if (turn.monthly_bytes > 0) e.turn_monthly_bytes = turn.monthly_bytes;
            if (turn.period_end > 0) e.turn_reset_at = turn.period_end;
        }

        {
            std::lock_guard<std::mutex> lock(mu_);
            if (generation == refresh_generation_.load()) {
                cached_ = e;
                last_refresh_ = std::chrono::steady_clock::now();
                last_error_.clear();
            }
        }
        refreshing_.store(false);
    }).detach();
}

}  // namespace aml::entitlements
