#pragma once

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

// Run all diagnostics and return results.
std::vector<CheckResult> run_all();

// Individual checks.
CheckResult check_launcher_version();
CheckResult check_java_runtime();
CheckResult check_backend_connectivity();
CheckResult check_ai_models();
CheckResult check_ai_art();
CheckResult check_providers();
CheckResult check_essentials();
CheckResult check_updater();
CheckResult check_disk_space();
CheckResult check_bedrock();

// Format results for display.
std::string format_results(const std::vector<CheckResult>& results);

}  // namespace aml::diagnostics
