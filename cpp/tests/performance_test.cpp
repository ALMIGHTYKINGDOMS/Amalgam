#include "performance.h"
#include "performance_cache.h"

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
    std::ifstream restored_input(options);
    std::string restored((std::istreambuf_iterator<char>(restored_input)),
                         std::istreambuf_iterator<char>());
    restored_input.close();
    ok = ok && restored.find("renderDistance:20") != std::string::npos;
    bool recovered_current_options = false;
    const fs::path recovery_dir = root / ".amalgam-backups";
    if (fs::exists(recovery_dir)) {
        for (const auto& entry : fs::directory_iterator(recovery_dir)) {
            if (!entry.is_regular_file()) continue;
            std::ifstream recovery_input(entry.path());
            std::string recovery((std::istreambuf_iterator<char>(recovery_input)),
                                 std::istreambuf_iterator<char>());
            if (recovery.find("renderDistance:6") != std::string::npos &&
                recovery.find("customValue:keep") != std::string::npos) {
                recovered_current_options = true;
                break;
            }
        }
    }
    ok = ok && recovered_current_options;

    // Restoring the saved original must also work when Minecraft has already
    // removed options.txt.  The restore creates its recovery folder/staging
    // file on demand, so none of those paths exist for a first-run profile.
    fs::remove(options);
    error.clear();
    const bool restored_without_current =
        aml::performance::restore_game_options(root.wstring(), &error);
    ok = ok && restored_without_current && error.empty() && fs::exists(options);
    if (restored_without_current) {
        std::ifstream restored_missing_input(options);
        std::string restored_missing((std::istreambuf_iterator<char>(restored_missing_input)),
                                     std::istreambuf_iterator<char>());
        ok = ok && restored_missing.find("renderDistance:20") != std::string::npos;
    }

    // Cache accounting is intentionally rooted in this disposable test tree.
    // This proves one scan classifies every launcher-owned category without
    // looking at the user's actual TEMP directory.
    const fs::path cache_fixture = root / "cache-fixture";
    const aml::performance_cache::CacheRoots cache_roots{
        cache_fixture / "amalgam_cache", cache_fixture / "amalgam"};
    const auto write_cache_file = [](const fs::path& path, size_t bytes) {
        fs::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary);
        output << std::string(bytes, 'x');
    };
    write_cache_file(cache_roots.cache_root / "mods" / "metadata.json", 3);
    write_cache_file(cache_roots.cache_root / "configs" / "profile.json", 5);
    write_cache_file(cache_roots.cache_root / "downloads" / "archive.zip", 7);
    write_cache_file(cache_roots.cache_root / "other" / "index.bin", 11);
    write_cache_file(cache_roots.temporary_root / "extract.tmp", 13);

    const auto cache_usage = aml::performance_cache::scan_cache_usage(cache_roots);
    ok = ok && cache_usage.complete && cache_usage.item_count == 5 &&
         cache_usage.total_bytes == 39 && cache_usage.mod_metadata_bytes == 3 &&
         cache_usage.instance_config_bytes == 5 && cache_usage.download_bytes == 7 &&
         cache_usage.other_launcher_bytes == 11 && cache_usage.temporary_bytes == 13;

    const auto clear_metadata = aml::performance_cache::clear_cache(
        cache_roots, aml::performance_cache::ClearScope::ModMetadata);
    const auto after_metadata_clear = aml::performance_cache::scan_cache_usage(cache_roots);
    ok = ok && clear_metadata.success && !fs::exists(cache_roots.cache_root / "mods") &&
         after_metadata_clear.complete && after_metadata_clear.item_count == 4 &&
         after_metadata_clear.total_bytes == 36 && after_metadata_clear.mod_metadata_bytes == 0;

    const auto clear_all = aml::performance_cache::clear_cache(
        cache_roots, aml::performance_cache::ClearScope::All);
    const auto after_clear_all = aml::performance_cache::scan_cache_usage(cache_roots);
    ok = ok && clear_all.success && !fs::exists(cache_roots.cache_root) &&
         !fs::exists(cache_roots.temporary_root) && after_clear_all.complete &&
         after_clear_all.item_count == 0 && after_clear_all.total_bytes == 0;

    // A caller with no verified roots must fail closed rather than deriving a
    // relative deletion target from an empty path.
    const auto invalid_clear = aml::performance_cache::clear_cache(
        {}, aml::performance_cache::ClearScope::All);
    ok = ok && !invalid_clear.success;

    fs::remove_all(root);
    if (!ok) {
        std::cerr << "FAILED: performance behavior";
        if (!error.empty()) std::cerr << " " << error;
        std::cerr << "\n";
        return 1;
    }
    return 0;
}
