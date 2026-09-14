#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace aml::performance {

struct Tuning {
    std::string profile;
    int heap_mb = 0;
    int min_heap_mb = 0;
    std::vector<std::string> jvm_args;
    std::vector<std::pair<std::string, std::string>> game_options;
};

std::string normalize_profile(const std::string& profile);
const char* profile_label(const std::string& profile);
uint64_t physical_memory_mb();
Tuning make_tuning(const std::string& profile, uint64_t total_memory_mb, int java_major);

std::vector<std::string> recommended_mods(const std::string& loader,
                                          const std::string& minecraft_version,
                                          bool shaders);

bool apply_game_options(const std::wstring& instance_dir, const std::string& profile,
                        std::string* err = nullptr);
bool restore_game_options(const std::wstring& instance_dir, std::string* err = nullptr);

}  // namespace aml::performance
