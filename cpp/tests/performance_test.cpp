#include "performance.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

int main() {
    namespace fs = std::filesystem;
    bool ok = true;
    ok = ok && aml::performance::normalize_profile("low-end") == "low_end";
    ok = ok && aml::performance::normalize_profile("shader") == "shaders";

    const auto low = aml::performance::make_tuning("low_end", 8192, 21);
    ok = ok && low.heap_mb == 3072 && low.min_heap_mb == 1024;
    ok = ok && !low.jvm_args.empty() && !low.game_options.empty();
    const auto balanced_small = aml::performance::make_tuning("balanced", 4096, 21);
    ok = ok && balanced_small.heap_mb == 2048 && balanced_small.jvm_args.front() == "-Xms1024m";

    const auto fabric_mods = aml::performance::recommended_mods("fabric", "1.21.8", true);
    const auto forge_mods = aml::performance::recommended_mods("forge", "1.20.1", true);
    ok = ok && std::find(fabric_mods.begin(), fabric_mods.end(), "sodium") != fabric_mods.end();
    ok = ok && std::find(fabric_mods.begin(), fabric_mods.end(), "iris") != fabric_mods.end();
    ok = ok && std::find(forge_mods.begin(), forge_mods.end(), "embeddium") != forge_mods.end();
    ok = ok && std::find(forge_mods.begin(), forge_mods.end(), "oculus") != forge_mods.end();

    fs::path root = fs::current_path() / "amalgam_performance_test";
    fs::remove_all(root);
    fs::create_directories(root);
    fs::path options = root / "options.txt";
    std::ofstream(options) << "renderDistance:20\ncustomValue:keep\n";
    std::string error;
    if (!aml::performance::apply_game_options(root.wstring(), "low_end", &error)) {
        std::cerr << "FAILED: apply " << error << "\n";
        return 1;
    }
    std::ifstream input(options);
    std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    input.close();
    ok = ok && text.find("renderDistance:6") != std::string::npos;
    ok = ok && text.find("customValue:keep") != std::string::npos;
    ok = ok && fs::exists(root / "options.txt.amalgam-original");
    ok = ok && aml::performance::restore_game_options(root.wstring(), &error);
    fs::remove_all(root);
    if (!ok) {
        std::cerr << "FAILED: performance behavior\n";
        return 1;
    }
    return 0;
}
