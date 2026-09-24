#pragma once

#include "config.h"

#include <cctype>
#include <string>
#include <vector>

namespace aml::diagnostics {

enum class CheckStatus { PASS, WARN, FAIL, UNKNOWN };

struct CheckResult {
    std::string system;
    std::string check;
    CheckStatus status;
    std::string detail;
};

// Pure, local-only status rules. They intentionally contain no filesystem,
// credential, provider, or network access, which makes the diagnostics
// wording testable with an in-memory Config and keeps "Run Diagnostics" from
// becoming an implicit external health probe.
namespace local_state {

inline bool has_complete_backend_config(const config::Config& config) {
    return !config.supabase_url.empty() && !config.supabase_anon_key.empty() &&
           !config.has_unreadable_secrets;
}

inline bool is_https_endpoint(const std::string& endpoint) {
    static constexpr char kHttpsPrefix[] = "https://";
    if (endpoint.size() <= sizeof(kHttpsPrefix) - 1 ||
        endpoint.compare(0, sizeof(kHttpsPrefix) - 1, kHttpsPrefix) != 0) {
        return false;
    }
    for (const unsigned char c : endpoint) {
        if (std::isspace(c) != 0) return false;
    }
    return true;
}

inline CheckResult backend(const config::Config& config, bool initialized) {
    if (config.has_unreadable_secrets) {
        return {"Backend", "Local configuration", CheckStatus::WARN,
                "Protected backend settings cannot be read on this Windows account. "
                "Reconnect online services; no remote service was contacted."};
    }
    if (config.supabase_url.empty() || config.supabase_anon_key.empty()) {
        return {"Backend", "Local configuration", CheckStatus::WARN,
                "Amalgam backend is not configured locally. Add the public endpoint and "
                "client key before using online services; no remote service was contacted."};
    }
    if (!is_https_endpoint(config.supabase_url)) {
        return {"Backend", "Local configuration", CheckStatus::FAIL,
                "The local Amalgam backend endpoint must use HTTPS. Diagnostics refused to "
                "probe it."};
    }
    if (!initialized) {
        return {"Backend", "Local configuration", CheckStatus::WARN,
                "The Amalgam backend is configured locally, but its client has not initialized "
                "in this process. A remote service was not contacted."};
    }
    return {"Backend", "Local configuration", CheckStatus::PASS,
            "The local backend client is initialized. Remote reachability was not contacted by "
            "Diagnostics."};
}

inline CheckResult essentials(const config::Config& config, bool initialized,
                              bool authenticated) {
    const CheckResult backend_state = backend(config, initialized);
    if (backend_state.status == CheckStatus::FAIL) {
        return {"Essentials", "Local prerequisites", CheckStatus::FAIL,
                "Essentials is blocked by invalid local backend configuration. Fix the HTTPS "
                "endpoint before using online features; no remote service was probed."};
    }
    if (!has_complete_backend_config(config)) {
        return {"Essentials", "Local prerequisites", CheckStatus::WARN,
                "Essentials requires local Amalgam backend configuration before it can start. "
                "No remote service was probed."};
    }
    if (!initialized) {
        return {"Essentials", "Local prerequisites", CheckStatus::WARN,
                "Essentials is configured locally, but the backend client has not initialized "
                "in this process. No remote service was probed."};
    }
    if (!authenticated) {
        return {"Essentials", "Local prerequisites", CheckStatus::WARN,
                "Essentials is configured locally. Sign in to an Amalgam account before using "
                "it; remote reachability was not probed by Diagnostics."};
    }
    return {"Essentials", "Local prerequisites", CheckStatus::PASS,
            "A local backend client and Amalgam session are available. Remote reachability is "
            "not probed by Diagnostics."};
}

inline CheckResult updater(bool signing_key_configured) {
    if (!signing_key_configured) {
        return {"Updater", "Signed manifest policy", CheckStatus::WARN,
                "No update-signing public key is embedded. Remote update manifests will fail "
                "closed until a release key is configured."};
    }
    return {"Updater", "Signed manifest policy", CheckStatus::PASS,
            "In-process signed-manifest updater is enabled. Diagnostics did not fetch a "
            "manifest."};
}

inline CheckResult providers() {
    return {"Providers", "External availability", CheckStatus::WARN,
            "External providers are not contacted by Diagnostics. Use Discover or an explicit "
            "refresh to check availability."};
}

inline std::string version_detail(const std::string& version) {
    return version.empty() ? "Launcher release version is unavailable."
                           : "Version " + version;
}

}  // namespace local_state

// Run local diagnostics and return results. Callers provide the live
// in-process backend/session flags; this function never makes an external
// provider request.
std::vector<CheckResult> run_all(const config::Config& config, bool backend_initialized,
                                 bool backend_authenticated);

// Individual checks.
CheckResult check_launcher_version();
CheckResult check_java_runtime();
CheckResult check_backend_connectivity(const config::Config& config, bool backend_initialized);
CheckResult check_ai_models();
CheckResult check_ai_art();
CheckResult check_providers();
CheckResult check_essentials(const config::Config& config, bool backend_initialized,
                             bool backend_authenticated);
CheckResult check_updater();
CheckResult check_disk_space();
CheckResult check_bedrock();

// Format results for display.
std::string format_results(const std::vector<CheckResult>& results);

}  // namespace aml::diagnostics
