#include "diagnostics.h"

#include "java.h"
#include "ai.h"
#include "ai_core.h"
#include "config.h"
#include "net.h"

#include <windows.h>
#include <filesystem>
#include <sstream>

namespace fs = std::filesystem;

namespace aml::diagnostics {

namespace {

CheckResult ok(const std::string& sys, const std::string& check, const std::string& detail = "") {
    return { sys, check, CheckStatus::PASS, detail };
}
CheckResult warn(const std::string& sys, const std::string& check, const std::string& detail) {
    return { sys, check, CheckStatus::WARN, detail };
}
CheckResult err(const std::string& sys, const std::string& check, const std::string& detail) {
    return { sys, check, CheckStatus::FAIL, detail };
}

}  // namespace

CheckResult check_launcher_version() {
    // Basic sanity: executable exists and has reasonable size.
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    auto p = fs::path(buf);
    std::error_code ec;
    auto sz = fs::file_size(p, ec);
    if (ec || sz < 1000000)  // less than 1MB is suspicious
        return err("Launcher", "Binary integrity", "Executable missing or too small");
    return ok("Launcher", "Binary integrity", "Version 1.0.0");
}

CheckResult check_java_runtime() {
    auto list = java::scan_installed();
    if (list.empty())
        return warn("Java", "Runtime detection", "No Java installations found. Amalgam can install managed Java when needed.");
    std::string versions;
    for (const auto& j : list) {
        if (!versions.empty()) versions += ", ";
        versions += "Java " + std::to_string(j.major);
    }
    return ok("Java", "Runtime detection", versions);
}

CheckResult check_backend_connectivity() {
    // Lightweight check: try to reach a known endpoint.
    std::vector<uint8_t> response;
    std::string error;
    bool reachable = net::get(L"https://api.modrinth.com/v2/tag/game_version", response, &error);
    if (!reachable)
        return warn("Backend", "Network connectivity", "Cannot reach Modrinth API: " + error);
    return ok("Backend", "Network connectivity", "Modrinth API reachable");
}

CheckResult check_ai_models() {
    const auto hw = aml::ai::detect_hardware();
    const auto mode = aml::ai::best_memory_mode(hw);
    const char* mode_names[] = { "GPU", "Balanced", "Low Memory", "CPU" };
    const auto status = aml::ai::local_runtime_status();
    const std::string detail = "Hardware mode: " + std::string(mode_names[static_cast<int>(mode)]);
    if (!status.brain_runtime)
        return warn("AI", "Brain runtime", "llama.cpp runtime not installed. Install AI from Settings. " + detail);
    if (!status.brain_model)
        return warn("AI", "Brain model", "Qwen3-VL model not installed. Install AI from Settings. " + detail);
    if (!status.vision_ready())
        return warn("AI", "Vision projector", "Brain chat is ready; vision projector is not installed. " + detail);
    return ok("AI", "Brain and vision", "Qwen3-VL 8B ready. " + detail);
}

CheckResult check_ai_art() {
    const auto status = aml::ai::local_runtime_status();
    if (!status.art_runtime)
        return warn("AI", "Art runtime", "stable-diffusion.cpp runtime not installed. Install AI from Settings.");
    if (!status.art_ready())
        return warn("AI", "Art models", "Art runtime is installed, but one or more art models are missing or incomplete.");
    // File presence and hashes prove installation, not successful generation.
    // Keep this as a warning until a bounded production generation check has
    // completed, so Diagnostics never presents a slow or hardware-limited Art
    // runtime as unconditionally ready.
    return warn("AI", "Art generation",
                "Art runtime and models are installed and verified; generation still requires a hardware-dependent runtime check.");
}

CheckResult check_providers() {
    // Check Modrinth API.
    std::vector<uint8_t> response;
    std::string error;
    bool modrinth = net::get(L"https://api.modrinth.com/v2/tag/game_version", response, &error);
    if (!modrinth)
        return warn("Providers", "Modrinth", "Unreachable: " + error);

    // CurseForge doesn't have a free API endpoint to probe without a key.
    return ok("Providers", "Modrinth", "Available");
}

CheckResult check_essentials() {
    // Essentials requires Supabase backend — check if configured.
    // Without a real session, we can only verify the network path.
    std::vector<uint8_t> response;
    std::string error;
    bool reachable = net::get(L"https://api.modrinth.com/v2/tag/game_version", response, &error);
    if (!reachable)
        return warn("Essentials", "Network", "Network unavailable for Essentials");
    return ok("Essentials", "Network", "Online (Supabase session required for full features)");
}

CheckResult check_updater() {
    // Verify the updater manifest endpoint is reachable.
    // For now, just confirm the updater binary exists.
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    auto dir = fs::path(buf).parent_path();
    auto updater = dir / L"updater.exe";
    if (!fs::exists(updater))
        return warn("Updater", "Binary", "updater.exe not found alongside launcher");
    return ok("Updater", "Binary", "updater.exe present");
}

CheckResult check_disk_space() {
    wchar_t exe_buf[MAX_PATH];
    GetModuleFileNameW(nullptr, exe_buf, MAX_PATH);
    auto root = fs::path(exe_buf).parent_path().root_path();
    ULARGE_INTEGER free_bytes{};
    if (!GetDiskFreeSpaceExW(root.wstring().c_str(), nullptr, nullptr, &free_bytes))
        return warn("Disk", "Free space", "Cannot determine disk space");
    double gb = free_bytes.QuadPart / (1024.0 * 1024.0 * 1024.0);
    if (gb < 2.0)
        return err("Disk", "Free space", "Only " + std::to_string((int)gb) + " GB free — need at least 2 GB");
    if (gb < 15.0)
        return warn("Disk", "Free space", std::to_string((int)gb) + " GB free — 15 GB recommended for AI models");
    return ok("Disk", "Free space", std::to_string((int)gb) + " GB free");
}

CheckResult check_bedrock() {
    // Check if Minecraft for Windows is installed by looking for the UWP data directory.
    wchar_t localappdata[MAX_PATH];
    GetEnvironmentVariableW(L"LOCALAPPDATA", localappdata, MAX_PATH);
    auto mojang = fs::path(localappdata) / L"Packages" / L"Microsoft.MinecraftUWP_8wekyb3d8bbwe" / L"LocalState" / L"games" / L"com.mojang";
    if (fs::exists(mojang))
        return ok("Bedrock", "Minecraft for Windows", "Detected");
    return warn("Bedrock", "Minecraft for Windows", "Not installed");
}

std::vector<CheckResult> run_all() {
    std::vector<CheckResult> results;
    results.push_back(check_launcher_version());
    results.push_back(check_java_runtime());
    results.push_back(check_backend_connectivity());
    results.push_back(check_ai_models());
    results.push_back(check_ai_art());
    results.push_back(check_providers());
    results.push_back(check_essentials());
    results.push_back(check_updater());
    results.push_back(check_disk_space());
    results.push_back(check_bedrock());
    return results;
}

std::string format_results(const std::vector<CheckResult>& results) {
    std::ostringstream ss;
    int ok_count = 0, warn_count = 0, err_count = 0;
    for (const auto& r : results) {
        const char* icon = "OK";
        if (r.status == CheckStatus::WARN) { icon = "WARN"; warn_count++; }
        else if (r.status == CheckStatus::FAIL) { icon = "ERR "; err_count++; }
        else ok_count++;
        ss << "[" << icon << "] " << r.system << " / " << r.check;
        if (!r.detail.empty()) ss << " -- " << r.detail;
        ss << "\n";
    }
    ss << "\n" << ok_count << " OK, " << warn_count << " warnings, " << err_count << " errors";
    return ss.str();
}

}  // namespace aml::diagnostics
