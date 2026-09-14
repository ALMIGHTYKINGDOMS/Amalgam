#include "performance.h"

#include "net.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace aml::performance {

namespace {

std::string lower(std::string value) {
    for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

int rank(const std::string& version) {
    int major = 0;
    int minor = 0;
    int patch = 0;
    std::stringstream stream(version);
    char dot = 0;
    stream >> major >> dot >> minor;
    if (stream >> dot >> patch) {}
    return major * 1000 + minor * 10 + patch;
}

void add_common_jvm(Tuning& tuning, int pause_ms, int java_major) {
    tuning.jvm_args.push_back("-Xms" + std::to_string(tuning.min_heap_mb) + "m");
    tuning.jvm_args.push_back("-XX:+UseG1GC");
    tuning.jvm_args.push_back("-XX:+ParallelRefProcEnabled");
    tuning.jvm_args.push_back("-XX:+DisableExplicitGC");
    tuning.jvm_args.push_back("-XX:MaxGCPauseMillis=" + std::to_string(pause_ms));
    if (java_major >= 17) tuning.jvm_args.push_back("-XX:+UseStringDeduplication");
}

void add_low_end_options(Tuning& tuning) {
    tuning.game_options = {
        {"renderDistance", "6"}, {"simulationDistance", "5"},
        {"entityDistanceScaling", "0.5"}, {"particles", "minimal"},
        {"graphics", "fast"}, {"smoothLighting", "false"},
        {"biomeBlendRadius", "0"}, {"clouds", "off"},
        {"renderClouds", "false"}, {"entityShadows", "false"},
        {"mipmapLevels", "0"}, {"enableVsync", "false"}, {"maxFps", "120"}
    };
}

void add_balanced_options(Tuning& tuning) {
    tuning.game_options = {
        {"renderDistance", "10"}, {"simulationDistance", "8"},
        {"entityDistanceScaling", "0.75"}, {"particles", "decreased"},
        {"clouds", "fast"}, {"renderClouds", "true"},
        {"entityShadows", "true"}, {"mipmapLevels", "2"}, {"maxFps", "144"}
    };
}

void add_shader_options(Tuning& tuning) {
    tuning.game_options = {
        {"renderDistance", "8"}, {"simulationDistance", "6"},
        {"entityDistanceScaling", "0.5"}, {"particles", "decreased"},
        {"clouds", "fast"}, {"renderClouds", "true"},
        {"entityShadows", "false"}, {"mipmapLevels", "2"},
        {"enableVsync", "false"}, {"maxFps", "90"}
    };
}

void add_heavy_options(Tuning& tuning) {
    tuning.game_options = {
        {"renderDistance", "8"}, {"simulationDistance", "6"},
        {"entityDistanceScaling", "0.5"}, {"particles", "minimal"},
        {"graphics", "fast"}, {"clouds", "off"},
        {"renderClouds", "false"}, {"entityShadows", "false"},
        {"mipmapLevels", "1"}, {"enableVsync", "false"}, {"maxFps", "120"}
    };
}

}  // namespace

std::string normalize_profile(const std::string& profile) {
    std::string value = lower(profile);
    if (value == "low-end" || value == "low_end" || value == "lowend") return "low_end";
    if (value == "balanced") return value;
    if (value == "shaders" || value == "shader") return "shaders";
    if (value == "heavy" || value == "heavy_modpack" || value == "heavy-modpack")
        return "heavy_modpack";
    if (value == "custom") return value;
    return "auto";
}

const char* profile_label(const std::string& profile) {
    const std::string value = normalize_profile(profile);
    if (value == "low_end") return "Low-end device";
    if (value == "balanced") return "Balanced";
    if (value == "shaders") return "Shaders";
    if (value == "heavy_modpack") return "Heavy modpack";
    if (value == "custom") return "Custom";
    return "Auto";
}

uint64_t physical_memory_mb() {
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (!GlobalMemoryStatusEx(&status)) return 0;
    return status.ullTotalPhys / (1024ull * 1024ull);
}

Tuning make_tuning(const std::string& requested, uint64_t total_memory_mb, int java_major) {
    Tuning tuning;
    tuning.profile = normalize_profile(requested);
    const uint64_t memory = total_memory_mb == 0 ? 16384 : total_memory_mb;
    if (tuning.profile == "auto") {
        tuning.heap_mb = memory <= 8192 ? 3072 : memory <= 12288 ? 4096 : 5120;
        tuning.min_heap_mb = tuning.heap_mb >= 4096 ? 1536 : 1024;
        add_common_jvm(tuning, 100, java_major);
    } else if (tuning.profile == "low_end") {
        tuning.heap_mb = 3072;
        tuning.min_heap_mb = 1024;
        add_common_jvm(tuning, 80, java_major);
        add_low_end_options(tuning);
    } else if (tuning.profile == "balanced") {
        tuning.heap_mb = 4096;
        tuning.min_heap_mb = 1536;
        add_common_jvm(tuning, 100, java_major);
        add_balanced_options(tuning);
    } else if (tuning.profile == "shaders") {
        tuning.heap_mb = 5120;
        tuning.min_heap_mb = 2048;
        add_common_jvm(tuning, 120, java_major);
        add_shader_options(tuning);
    } else if (tuning.profile == "heavy_modpack") {
        tuning.heap_mb = 6144;
        tuning.min_heap_mb = 2048;
        add_common_jvm(tuning, 120, java_major);
        add_heavy_options(tuning);
    }
    if (tuning.heap_mb > 0) {
        const uint64_t reserved = memory > 2048 ? memory - 2048 : 1024;
        const int safe_max = static_cast<int>(std::min<uint64_t>(reserved, 8192));
        tuning.heap_mb = std::max(1024, std::min(tuning.heap_mb, safe_max));
        tuning.min_heap_mb = std::min(tuning.min_heap_mb, std::max(512, tuning.heap_mb / 2));
        if (!tuning.jvm_args.empty())
            tuning.jvm_args[0] = "-Xms" + std::to_string(tuning.min_heap_mb) + "m";
    }
    return tuning;
}

std::vector<std::string> recommended_mods(const std::string& loader,
                                          const std::string& minecraft_version,
                                          bool shaders) {
    const std::string normalized_loader = lower(loader);
    const int version = rank(minecraft_version);
    if (version > 0 && version < 1130) {
        return {"foamfix", "betterfps", "vanillafix", "ai-improvements"};
    }

    std::vector<std::string> result = {"ferrite-core", "modernfix", "immediatelyfast",
                                       "entityculling"};
    if (normalized_loader == "fabric" || normalized_loader == "quilt" || normalized_loader.empty()) {
        result.insert(result.begin(), {"sodium", "lithium"});
        if (shaders) result.push_back("iris");
    } else if (normalized_loader == "forge" || normalized_loader == "neoforge") {
        result.insert(result.begin(), "embeddium");
        if (shaders) result.push_back("oculus");
    }
    return result;
}

bool apply_game_options(const std::wstring& instance_dir, const std::string& profile,
                        std::string* err) {
    Tuning tuning = make_tuning(profile, 16384, 21);
    if (tuning.game_options.empty()) return true;
    const std::wstring path = instance_dir + L"\\options.txt";
    const std::wstring backup = path + L".amalgam-original";
    if (net::file_exists(path) && !net::file_exists(backup) &&
        !CopyFileW(path.c_str(), backup.c_str(), TRUE)) {
        if (err) *err = "could not create options backup";
        return false;
    }

    std::vector<std::string> lines;
    std::ifstream input(std::filesystem::path(path), std::ios::binary);
    if (input.is_open()) {
        std::string line;
        while (std::getline(input, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(std::move(line));
        }
        input.close();
    }
    std::map<std::string, std::string> values;
    for (const auto& option : tuning.game_options) values[option.first] = option.second;
    std::vector<std::string> output;
    for (const auto& line : lines) {
        size_t colon = line.find(':');
        if (colon == std::string::npos) {
            output.push_back(line);
            continue;
        }
        std::string key = line.substr(0, colon);
        auto found = values.find(key);
        if (found == values.end()) {
            output.push_back(line);
        } else {
            output.push_back(key + ":" + found->second);
            values.erase(found);
        }
    }
    for (const auto& option : values) output.push_back(option.first + ":" + option.second);
    std::ofstream file(std::filesystem::path(path + L".amalgam-tmp"), std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        if (err) *err = "could not write optimized options";
        return false;
    }
    for (const auto& line : output) file << line << '\n';
    file.close();
    if (!MoveFileExW((path + L".amalgam-tmp").c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW((path + L".amalgam-tmp").c_str());
        if (err) *err = "could not activate optimized options";
        return false;
    }
    return true;
}

bool restore_game_options(const std::wstring& instance_dir, std::string* err) {
    const std::wstring backup = instance_dir + L"\\options.txt.amalgam-original";
    const std::wstring path = instance_dir + L"\\options.txt";
    if (!net::file_exists(backup)) {
        if (err) *err = "no Amalgam options backup exists";
        return false;
    }
    if (!CopyFileW(backup.c_str(), path.c_str(), FALSE)) {
        if (GetLastError() == ERROR_FILE_EXISTS || GetLastError() == ERROR_ALREADY_EXISTS) {
            DeleteFileW(path.c_str());
            if (CopyFileW(backup.c_str(), path.c_str(), FALSE)) return true;
        }
        if (err) *err = "could not restore original options";
        return false;
    }
    return true;
}

}  // namespace aml::performance
