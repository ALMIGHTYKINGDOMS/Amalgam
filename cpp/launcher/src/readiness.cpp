#include "readiness.h"

#include "auth.h"
#include "bedrock.h"
#include "java.h"
#include "net.h"

#include <filesystem>
#include <array>
#include <system_error>

namespace aml::readiness {

namespace {

int count_files_with_extension(const std::wstring& root, const wchar_t* extension) {
    std::error_code ec;
    const std::filesystem::path path(root);
    if (!std::filesystem::exists(path, ec) || ec) return 0;
    int count = 0;
    for (const auto& item : std::filesystem::directory_iterator(path, ec)) {
        if (ec) break;
        if (item.is_regular_file(ec) && !ec && item.path().extension() == extension) ++count;
    }
    return count;
}

struct BridgeCoverage {
    int present = 0;
    int expected = 0;
};

BridgeCoverage count_supported_bridges(const std::wstring& root) {
    static constexpr std::array<const wchar_t*, 19> kExpected = {
        L"amalgam-fabric-1.18.2.jar", L"amalgam-fabric-1.19.2.jar",
        L"amalgam-fabric-1.20.1.jar", L"amalgam-fabric-1.21.1.jar",
        L"amalgam-fabric-1.21.4.jar", L"amalgam-fabric-1.21.5.jar",
        L"amalgam-fabric-1.21.6.jar", L"amalgam-fabric-1.21.8.jar",
        L"amalgam-fabric-1.21.11.jar", L"amalgam-forge-1.12.2.jar",
        L"amalgam-forge-1.18.2.jar", L"amalgam-forge-1.19.2.jar",
        L"amalgam-forge-1.20.1.jar", L"amalgam-neoforge-1.21.1.jar",
        L"amalgam-neoforge-1.21.4.jar", L"amalgam-neoforge-1.21.5.jar",
        L"amalgam-neoforge-1.21.6.jar", L"amalgam-neoforge-1.21.8.jar",
        L"amalgam-neoforge-1.21.11.jar"
    };
    BridgeCoverage result;
    result.expected = static_cast<int>(kExpected.size());
    for (const auto* name : kExpected) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(std::filesystem::path(root) / name, ec) && !ec)
            ++result.present;
    }
    return result;
}

int count_cached_java(const std::wstring& root) {
    std::error_code ec;
    const std::filesystem::path path(root);
    if (root.empty() || !std::filesystem::exists(path, ec) || ec) return 0;
    int count = 0;
    for (const auto& item : std::filesystem::recursive_directory_iterator(path, ec)) {
        if (ec) break;
        if (!item.is_regular_file(ec) || ec) continue;
        const std::filesystem::path file = item.path();
        if (_wcsicmp(file.filename().c_str(), L"java.exe") == 0 &&
            _wcsicmp(file.parent_path().filename().c_str(), L"bin") == 0)
            ++count;
    }
    return count;
}

std::wstring resolved_cache_dir(const Options& options) {
    std::filesystem::path cache(options.java_cache_dir.empty()
                                    ? options.launcher_dir + L"\\runtimes\\java"
                                    : options.java_cache_dir);
    if (cache.is_relative()) cache = std::filesystem::path(options.launcher_dir) / cache;
    return cache.wstring();
}

std::string count_detail(int count, const char* singular, const char* plural) {
    return std::to_string(count) + " " + (count == 1 ? singular : plural);
}

void add_provider_check(Report& report, const Options& options, const char* source,
                        const char* label, bool key_required) {
    const bool has_access = mods::curseforge_available(options.provider_api);
    if (key_required && !has_access) {
        const bool proxy_configured = mods::curseforge_proxy_configured(options.provider_api);
        report.checks.push_back({source, label,
                                 proxy_configured
                                     ? "Secure backend configured; sign in to an Amalgam account to use CurseForge"
                                     : "Provider key or backend proxy is not configured; Modrinth remains available",
                                 State::Optional, false});
        return;
    }
    if (!options.verify_providers) {
        report.checks.push_back({source, label,
                                 key_required ? "Provider access configured; run a live check to verify access"
                                              : "Public catalog; run a live check to verify access",
                                 State::Optional, false});
        return;
    }
    std::string error;
    if (mods::test_provider(options.provider_api, source, &error)) {
        report.checks.push_back({source, label, "Live catalog request succeeded", State::Ready, false});
    } else {
        report.checks.push_back({source, label,
                                 error.empty() ? "Live catalog request failed" : error,
                                 State::Attention, false});
    }
}

}  // namespace

Report run(const Options& options) {
    Report report;

    const std::wstring dll = options.launcher_dir + L"\\amalgam.dll";
    const bool dll_present = net::file_exists(dll);
    report.checks.push_back({"native_bridge", "Native bridge",
                             dll_present ? "amalgam.dll found" : "amalgam.dll is missing",
                             dll_present ? State::Ready : State::Attention, true});

    const std::wstring bridges_dir = options.launcher_dir + L"\\bridges";
    const int bridge_count = count_files_with_extension(bridges_dir, L".jar");
    const BridgeCoverage bridge_coverage = count_supported_bridges(bridges_dir);
    const bool any_bridge = bridge_count > 0;
    const bool complete_bridge_set = bridge_coverage.present == bridge_coverage.expected;
    std::string bridge_detail;
    if (complete_bridge_set) {
        bridge_detail = std::to_string(bridge_coverage.present) + "/" +
            std::to_string(bridge_coverage.expected) + " supported bridge jars staged";
    } else if (any_bridge) {
        bridge_detail = std::to_string(bridge_coverage.present) + "/" +
            std::to_string(bridge_coverage.expected) + " supported bridge jars staged; " +
            std::to_string(bridge_count) + " total JARs found";
    } else {
        bridge_detail = "No bridge jars were found";
    }
    report.checks.push_back({"java_bridges", "Java bridges", std::move(bridge_detail),
                             complete_bridge_set ? State::Ready : State::Attention,
                             !any_bridge});

    const int installed_java = static_cast<int>(java::scan_installed().size());
    const int cached_java = count_cached_java(resolved_cache_dir(options));
    if (installed_java > 0 || cached_java > 0) {
        std::string detail = count_detail(installed_java, "system Java detected", "system Javas detected");
        if (cached_java > 0) detail += "; " + count_detail(cached_java, "cached runtime", "cached runtimes");
        report.checks.push_back({"java_runtime", "Java runtime", std::move(detail), State::Ready, false});
    } else {
        report.checks.push_back({"java_runtime", "Java runtime",
                                 "No runtime found; Amalgam will provision one on first launch",
                                 State::Attention, false});
    }

    auth::Account account;
    std::string account_error;
    if (auth::load(account, &account_error)) {
        report.checks.push_back({"microsoft_account", "Minecraft authentication",
                                 "Official Minecraft Launcher account detected",
                                 State::Ready, false});
    } else {
        report.checks.push_back({"microsoft_account", "Minecraft authentication",
                                 "Handled by the Official Minecraft Launcher when Play opens it",
                                 State::Optional, false});
    }

    add_provider_check(report, options, "modrinth", "Modrinth", false);
    add_provider_check(report, options, "curseforge", "CurseForge", true);

    std::string bedrock_error;
    if (aml::bedrock::installed()) {
        std::wstring data = aml::bedrock::detect(&bedrock_error);
        report.checks.push_back({"bedrock", "Minecraft Bedrock",
                                 data.empty() ? "Windows app detected; launch is available"
                                              : "Windows app and addon data folder detected",
                                 State::Ready, false});
    } else {
        report.checks.push_back({"bedrock", "Minecraft Bedrock",
                                 "Install Minecraft for Windows to enable Bedrock launch and addon import",
                                 State::Optional, false});
    }

    const std::wstring bedrock_client = options.launcher_dir +
        L"\\bedrock\\AmalgamBedrockClient.mcaddon";
    const bool bedrock_client_present = net::file_exists(bedrock_client);
    report.checks.push_back({"amalgam_bedrock_client", "Amalgam Bedrock Client",
                             bedrock_client_present
                                 ? "Bundled add-on package is ready to import"
                                 : "Bundled add-on package is not present in this build",
                             bedrock_client_present ? State::Ready : State::Optional, false});

    const bool release_manifest =
        net::file_exists(options.launcher_dir + L"\\component-manifest.json") &&
        net::file_exists(options.launcher_dir + L"\\release.sha256");
    report.checks.push_back({"release_integrity", "Release integrity",
                             release_manifest
                                 ? "Component manifest and release hashes are present"
                                 : "Development build: release integrity manifests are not present",
                             release_manifest ? State::Ready : State::Optional, false});
    return report;
}

}  // namespace aml::readiness
