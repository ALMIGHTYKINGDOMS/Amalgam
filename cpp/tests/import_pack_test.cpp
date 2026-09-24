#include "import_pack.h"
#include "extract.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>

namespace {

std::wstring temporary_path(const wchar_t* suffix) {
    wchar_t directory[MAX_PATH]{};
    wchar_t file[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, directory) == 0 ||
        GetTempFileNameW(directory, L"aml", 0, file) == 0)
        return {};
    std::wstring path = file;
    DeleteFileW(path.c_str());
    return path + suffix;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

bool has_staging_folder(const std::filesystem::path& root) {
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
        if (error) return true;
        const std::string name = entry.path().filename().string();
        if (name.rfind(".amalgam-pack-stage-", 0) == 0) return true;
    }
    return false;
}

bool has_import_staging_folder(const std::filesystem::path& root) {
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
        if (error) return true;
        const std::string name = entry.path().filename().string();
        if (name.rfind(".amalgam-import-", 0) == 0) return true;
    }
    return false;
}

}  // namespace

int main() {
    namespace fs = std::filesystem;
    fs::path root = temporary_path(L"_import_pack_test");
    fs::remove_all(root);
    fs::create_directories(root / "mods");
    std::ofstream(root / "mods" / "example.jar") << "jar";
    aml::instances::Instance instance;
    instance.id = "forge-test";
    instance.name = "Forge Test";
    instance.minecraft_version = "1.20.1";
    instance.loader = "forge";
    instance.loader_version = "47.4.15";
    instance.directory = root.wstring();
    fs::path mrpack = temporary_path(L"_import_pack_test.mrpack");
    fs::path curseforge = temporary_path(L"_import_pack_test.zip");
    fs::path instances_dir = temporary_path(L"_import_pack_instances");
    std::string error;
    if (!aml::import_pack::export_mrpack(instance, mrpack.wstring(), &error) ||
        !aml::import_pack::export_curseforge(instance, curseforge.wstring(), &error)) {
        std::cerr << "FAILED: export " << error << "\n";
        return 1;
    }
    if (!fs::exists(mrpack) || !fs::exists(curseforge)) {
        std::cerr << "FAILED: archive missing\n";
        return 1;
    }
    aml::mods::ApiCfg api;
    aml::instances::Instance imported;
    if (!aml::import_pack::archive(mrpack.wstring(), instances_dir.wstring(), api, imported,
                                   nullptr, &error)) {
        std::cerr << "FAILED: Modrinth round trip " << error << "\n";
        return 1;
    }
    if (imported.minecraft_version != instance.minecraft_version || imported.loader != instance.loader ||
        !fs::exists(fs::path(imported.directory) / "mods" / "example.jar") ||
        has_import_staging_folder(instances_dir)) {
        std::cerr << "FAILED: imported Modrinth instance mismatch\n";
        return 1;
    }
    aml::instances::Instance target_source;
    target_source.id = "target";
    target_source.name = "Existing Target";
    target_source.minecraft_version = "1.20.1";
    target_source.loader = "forge";
    aml::instances::Instance target;
    if (!aml::instances::create(instances_dir.wstring(), target_source, target, &error)) {
        std::cerr << "FAILED: target profile " << error << "\n";
        return 1;
    }

    // A rejected pack must never clear live profile files. This exercises the
    // staging path used by a published-pack update before the commit starts.
    fs::create_directories(fs::path(target.directory) / "mods");
    fs::create_directories(fs::path(target.directory) / "config");
    fs::create_directories(fs::path(target.directory) / "saves" / "world");
    std::ofstream(fs::path(target.directory) / "mods" / "keep.jar", std::ios::binary) << "keep";
    std::ofstream(fs::path(target.directory) / "config" / "keep.cfg", std::ios::binary) << "config";
    std::ofstream(fs::path(target.directory) / "saves" / "world" / "level.dat", std::ios::binary) << "world";
    std::ofstream(fs::path(target.directory) / "options.txt", std::ios::binary) << "options";
    std::ofstream(fs::path(target.directory) / "amalgam-dependencies.json", std::ios::binary) << "stale";
    std::ofstream(fs::path(target.directory) / "amalgam-local-content.json", std::ios::binary) << "stale";

    aml::instances::Instance imported_into;
    aml::instances::Instance incompatible = instance;
    incompatible.minecraft_version = "1.21.1";
    fs::path incompatible_pack = temporary_path(L"_import_pack_incompatible.mrpack");
    if (!aml::import_pack::export_mrpack(incompatible, incompatible_pack.wstring(), &error) ||
        aml::import_pack::archive_into(incompatible_pack.wstring(), target, api, imported_into, nullptr, &error) ||
        read_file(fs::path(target.directory) / "mods" / "keep.jar") != "keep" ||
        read_file(fs::path(target.directory) / "config" / "keep.cfg") != "config" ||
        read_file(fs::path(target.directory) / "saves" / "world" / "level.dat") != "world" ||
        has_staging_folder(target.directory)) {
        std::cerr << "FAILED: rejected profile import changed live content " << error << "\n";
        return 1;
    }

    // A long-running import must revalidate the selected profile immediately
    // before activation. Simulate an edit while the candidate is staged; the
    // new pack must not overwrite either the changed metadata or live files.
    aml::instances::Instance stale_result;
    bool changed_during_stage = false;
    bool mutation_ok = true;
    std::string stale_error;
    const bool stale_import = aml::import_pack::archive_into(
        mrpack.wstring(), target, api, stale_result,
        [&](float, const std::string& detail) {
            if (!changed_during_stage && detail.find("Files staged") != std::string::npos) {
                aml::instances::Instance edited = target;
                edited.name = "Edited while import was staging";
                std::string mutation_error;
                mutation_ok = aml::instances::save(edited, &mutation_error);
                changed_during_stage = mutation_ok;
            }
            return true;
        },
        &stale_error);
    if (stale_import || !changed_during_stage || !mutation_ok ||
        read_file(fs::path(target.directory) / "mods" / "keep.jar") != "keep" ||
        read_file(fs::path(target.directory) / "config" / "keep.cfg") != "config" ||
        has_staging_folder(target.directory)) {
        std::cerr << "FAILED: stale profile import was activated " << stale_error << "\n";
        return 1;
    }
    if (!aml::instances::load(target.directory, target, &error) ||
        target.name != "Edited while import was staging") {
        std::cerr << "FAILED: stale profile import overwrote profile metadata " << error << "\n";
        return 1;
    }

    // A creator update preview must describe the managed-content change set
    // without touching the live profile. The exported fixture adds one mod
    // and removes the two current managed files from the selected profile.
    aml::import_pack::ArchivePreview preview;
    error.clear();
    if (!aml::import_pack::preview_archive(mrpack.wstring(), target, preview, &error) ||
        preview.added != 1 || preview.removed != 2 || preview.replaced != 0 ||
        preview.unchanged != 0 ||
        read_file(fs::path(target.directory) / "mods" / "keep.jar") != "keep" ||
        !preview.provider_file_names_resolved) {
        std::cerr << "FAILED: archive preview " << error << " [added=" << preview.added
                  << " removed=" << preview.removed << " replaced=" << preview.replaced << "]\n";
        return 1;
    }

    // CurseForge manifests carry project/file ids rather than final names, so
    // the preview must flag that provider resolution is still required. The
    // exported fixture carries its mod under overrides, so the change set is
    // still counted against the live profile.
    aml::import_pack::ArchivePreview cf_preview;
    error.clear();
    if (!aml::import_pack::preview_archive(curseforge.wstring(), target, cf_preview, &error) ||
        cf_preview.provider_file_names_resolved || cf_preview.added != 1 ||
        cf_preview.removed != 2 || cf_preview.replaced != 0 || cf_preview.unchanged != 0) {
        std::cerr << "FAILED: curseforge archive preview " << error << " [added=" << cf_preview.added
                  << " removed=" << cf_preview.removed << " replaced=" << cf_preview.replaced
                  << " unchanged=" << cf_preview.unchanged << "]\n";
        return 1;
    }

    error.clear();
    if (!aml::import_pack::archive_into(mrpack.wstring(), target, api, imported_into, nullptr, &error) ||
        imported_into.directory != target.directory || imported_into.pack_source != "modrinth" ||
        !fs::exists(fs::path(target.directory) / "mods" / "example.jar") ||
        fs::exists(fs::path(target.directory) / "mods" / "keep.jar") ||
        fs::exists(fs::path(target.directory) / "config" / "keep.cfg") ||
        !fs::exists(fs::path(target.directory) / "saves" / "world" / "level.dat") ||
        read_file(fs::path(target.directory) / "options.txt") != "options" ||
        fs::exists(fs::path(target.directory) / "amalgam-dependencies.json") ||
        fs::exists(fs::path(target.directory) / "amalgam-local-content.json") ||
        has_staging_folder(target.directory)) {
        std::cerr << "FAILED: existing profile import " << error << "\n";
        return 1;
    }
    // archive_into intentionally replaces managed profile metadata along with
    // managed content. Refresh the fixture's selected profile before opening
    // the next preview so the production stale-selection guard sees the same
    // persisted identity a real Library refresh would provide.
    if (!aml::instances::load(target.directory, target, &error)) {
        std::cerr << "FAILED: refresh committed profile " << error << "\n";
        return 1;
    }

    // The diff must classify every managed outcome: a content mismatch is
    // replaced, new paths are added, and nothing else is removed — while the
    // live profile stays untouched until the user commits.
    fs::path replace_stage = temporary_path(L"_import_pack_replace");
    fs::path replace_pack = temporary_path(L"_import_pack_replace.mrpack");
    fs::create_directories(replace_stage);
    std::ofstream(replace_stage / "modrinth.index.json", std::ios::binary)
        << "{\"formatVersion\":1,\"game\":\"minecraft\",\"versionId\":\"diff\","
           "\"name\":\"Diff\",\"files\":["
           "{\"path\":\"mods/example.jar\",\"hashes\":{\"sha1\":\""
           "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}},"
           "{\"path\":\"mods/newmod.jar\",\"hashes\":{\"sha1\":\""
           "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"}},"
           "{\"path\":\"config/newmod.cfg\",\"hashes\":{\"sha1\":\""
           "cccccccccccccccccccccccccccccccccccccccc\"}}],"
           "\"dependencies\":{\"minecraft\":\"1.20.1\",\"forge\":\"47.4.15\"}}";
    aml::import_pack::ArchivePreview diff_preview;
    error.clear();
    if (!aml::extract::create_zip(replace_stage.wstring(), replace_pack.wstring(), &error) ||
        !aml::import_pack::preview_archive(replace_pack.wstring(), target, diff_preview, &error) ||
        diff_preview.added != 2 || diff_preview.removed != 0 || diff_preview.replaced != 1 ||
        diff_preview.unchanged != 0 || !diff_preview.provider_file_names_resolved ||
        diff_preview.changes.empty() ||
        read_file(fs::path(target.directory) / "mods" / "example.jar") != "jar") {
        std::cerr << "FAILED: archive diff classification " << error << " [added=" << diff_preview.added
                  << " removed=" << diff_preview.removed << " replaced=" << diff_preview.replaced
                  << " unchanged=" << diff_preview.unchanged << "]\n";
        return 1;
    }

    // Re-exporting the committed profile produces an identical override: the
    // diff must recognize the file as unchanged rather than a replacement.
    fs::path unchanged_pack = temporary_path(L"_import_pack_unchanged.mrpack");
    error.clear();
    if (!aml::import_pack::export_mrpack(imported_into, unchanged_pack.wstring(), &error) ||
        !aml::import_pack::preview_archive(unchanged_pack.wstring(), target, diff_preview, &error) ||
        diff_preview.unchanged != 1 || diff_preview.added != 0 || diff_preview.removed != 0 ||
        diff_preview.replaced != 0) {
        std::cerr << "FAILED: unchanged archive diff " << error << " [added=" << diff_preview.added
                  << " removed=" << diff_preview.removed << " replaced=" << diff_preview.replaced
                  << " unchanged=" << diff_preview.unchanged << "]\n";
        return 1;
    }

    // Creator overrides are allowed to provide pack content, but they must not
    // overwrite launcher metadata or user-owned worlds/screenshots at commit.
    fs::path protected_stage = fs::current_path() / "amalgam_import_pack_protected";
    fs::path protected_pack = fs::current_path() / "amalgam_import_pack_protected.mrpack";
    fs::create_directories(protected_stage / "overrides" / "saves" / "world");
    std::ofstream(protected_stage / "modrinth.index.json", std::ios::binary)
        << "{\"formatVersion\":1,\"game\":\"minecraft\",\"versionId\":\"creator-update\","
           "\"name\":\"Creator Update\",\"files\":[],\"dependencies\":{\"minecraft\":\"1.20.1\",\"forge\":\"47.4.15\"}}";
    std::ofstream(protected_stage / "overrides" / "saves" / "world" / "creator.txt", std::ios::binary)
        << "must not copy";
    std::ofstream(protected_stage / "overrides" / "instance.json", std::ios::binary)
        << "{\"id\":\"unsafe\"}";
    std::ofstream(protected_stage / "overrides" / "amalgam-local-content.json", std::ios::binary)
        << "unsafe";
    if (!aml::extract::create_zip(protected_stage.wstring(), protected_pack.wstring(), &error) ||
        !aml::import_pack::archive_into(protected_pack.wstring(), target, api, imported_into, nullptr, &error)) {
        std::cerr << "FAILED: protected override import " << error << "\n";
        return 1;
    }
    aml::instances::Instance committed;
    if (fs::exists(fs::path(target.directory) / "saves" / "world" / "creator.txt") ||
        !fs::exists(fs::path(target.directory) / "saves" / "world" / "level.dat") ||
        fs::exists(fs::path(target.directory) / "amalgam-local-content.json") ||
        !aml::instances::load(target.directory, committed, &error) || committed.id != target.id) {
        std::cerr << "FAILED: protected override committed into user data " << error << "\n";
        return 1;
    }
    // The successful creator update also replaces managed metadata. Keep the
    // next negative archive test focused on traversal rejection rather than
    // intentionally stale in-memory profile state.
    target = committed;
    fs::path traversal_stage = fs::current_path() / "amalgam_import_pack_traversal";
    fs::path traversal_pack = fs::current_path() / "amalgam_import_pack_traversal.mrpack";
    fs::create_directories(traversal_stage);
    std::ofstream(traversal_stage / "modrinth.index.json", std::ios::binary)
        << "{\"formatVersion\":1,\"game\":\"minecraft\",\"versionId\":\"bad\","
           "\"name\":\"Bad\",\"files\":[{\"path\":\"C:\\\\outside.txt\","
           "\"downloads\":[\"https://example.invalid/outside\"],\"hashes\":{\"sha1\":\""
           "0000000000000000000000000000000000000000\"},\"fileSize\":1}],"
           "\"dependencies\":{\"minecraft\":\"1.20.1\",\"forge\":\"47.4.15\"}}";
    aml::instances::Instance rejected;
    if (!aml::extract::create_zip(traversal_stage.wstring(), traversal_pack.wstring(), &error) ||
        aml::import_pack::archive_into(traversal_pack.wstring(), target, api, rejected, nullptr, &error) ||
        !fs::exists(fs::path(target.directory) / "saves" / "world" / "level.dat")) {
        std::cerr << "FAILED: traversal archive was accepted or altered profile " << error << "\n";
        return 1;
    }
    fs::remove_all(root);
    fs::remove_all(instances_dir);
    fs::remove(mrpack);
    fs::remove(curseforge);
    fs::remove(incompatible_pack);
    fs::remove_all(protected_stage);
    fs::remove(protected_pack);
    fs::remove_all(traversal_stage);
    fs::remove(traversal_pack);
    fs::remove_all(replace_stage);
    fs::remove(replace_pack);
    fs::remove(unchanged_pack);
    return 0;
}
