#include "instances.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

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

bool write_file(const std::filesystem::path& path, const std::string& body) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << body;
    return static_cast<bool>(output);
}

}  // namespace

int main() {
    namespace fs = std::filesystem;
    fs::path root = temporary_path(L"_content_import_test");
    fs::create_directories(root / "profile");
    fs::path source = root / "external.jar";
    std::ofstream(source, std::ios::binary) << "PK\x03\x04local mod";

    aml::instances::Instance instance;
    instance.id = "profile";
    instance.name = "Profile";
    instance.directory = (root / "profile").wstring();
    aml::instances::ContentEntry entry;
    std::string error;
    if (!aml::instances::import_content(instance, source.wstring(), aml::instances::ContentType::Mod,
                                        &entry, &error)) {
        std::cerr << "FAILED: import " << error << "\n";
        return 1;
    }
    if (entry.filename != "external.jar" || !fs::exists(entry.path) ||
        !fs::exists(root / "profile" / "amalgam-local-content.json")) {
        std::cerr << "FAILED: imported content metadata\n";
        return 1;
    }
    if (!aml::instances::import_content(instance, source.wstring(), aml::instances::ContentType::Mod,
                                        &entry, &error) || entry.filename == "external.jar") {
        std::cerr << "FAILED: duplicate content was not renamed\n";
        return 1;
    }
    if (!write_file(root / "profile" / "amalgam-dependencies.json",
                    R"({"entries":[{"file":"external.jar","project":"fixture","source":"modrinth","version_id":"old"}]})")) {
        std::cerr << "FAILED: managed content metadata fixture\n";
        return 1;
    }
    std::wstring trashed_content;
    aml::instances::ContentEntry outside_content;
    outside_content.path = source.wstring();
    outside_content.filename = "external.jar";
    outside_content.type = aml::instances::ContentType::Mod;
    aml::instances::ContentEntry first_content;
    first_content.path = (root / "profile" / "mods" / "external.jar").wstring();
    first_content.filename = "external.jar";
    first_content.type = aml::instances::ContentType::Mod;
    if (!aml::instances::move_content_to_trash(instance, entry, &trashed_content, &error) ||
        fs::exists(entry.path) || !fs::exists(trashed_content) ||
        read_file(root / "profile" / "amalgam-local-content.json").find(entry.filename) != std::string::npos ||
        !aml::instances::remove_content(instance, first_content, &error) || fs::exists(first_content.path) ||
        read_file(root / "profile" / "amalgam-dependencies.json").find(first_content.filename) != std::string::npos ||
        aml::instances::remove_content(instance, outside_content, &error)) {
        std::cerr << "FAILED: content recovery handling " << error << "\n";
        return 1;
    }

    // The Mod Manager can remove a disabled mod too. Its physical filename
    // carries the state suffix, while profile metadata uses the original mod
    // name. Verify the recovery path and metadata cleanup preserve both.
    const fs::path disabled_mod = root / "profile" / "mods" / "disabled-fixture.jar.disabled";
    const fs::path mod_recovery = root / "profile" / ".amalgam-trash" / "content" / "mods";
    if (!write_file(disabled_mod, "disabled mod") ||
        !write_file(root / "profile" / "amalgam-local-content.json",
                    R"({"entries":[{"file":"disabled-fixture.jar","type":"mod"}]})")) {
        std::cerr << "FAILED: disabled mod recovery fixture\n";
        return 1;
    }
    aml::instances::ContentEntry disabled_entry;
    disabled_entry.path = disabled_mod.wstring();
    disabled_entry.filename = "disabled-fixture.jar";
    disabled_entry.type = aml::instances::ContentType::Mod;
    std::wstring disabled_recovery;
    error.clear();
    if (!aml::instances::move_content_to_trash(instance, disabled_entry, &disabled_recovery, &error) ||
        !error.empty() || fs::exists(disabled_mod) || !fs::exists(disabled_recovery) ||
        fs::path(disabled_recovery).parent_path() != mod_recovery ||
        read_file(root / "profile" / "amalgam-local-content.json").find(disabled_entry.filename) != std::string::npos) {
        std::cerr << "FAILED: disabled mod recovery handling " << error << "\n";
        return 1;
    }

    // The confirmation UI snapshots the chosen file before the recovery move.
    // A replacement at the same path must be detected rather than moving an
    // unexpectedly changed file after the user has already confirmed.
    const fs::path snapshot_mod = root / "profile" / "mods" / "snapshot-fixture.jar";
    if (!write_file(snapshot_mod, "first payload")) {
        std::cerr << "FAILED: content snapshot fixture\n";
        return 1;
    }
    aml::instances::ContentEntry snapshot_entry;
    snapshot_entry.path = snapshot_mod.wstring();
    snapshot_entry.filename = "snapshot-fixture.jar";
    snapshot_entry.type = aml::instances::ContentType::Mod;
    aml::instances::ContentFileSnapshot snapshot;
    error.clear();
    if (!aml::instances::capture_content_file_snapshot(instance, snapshot_entry, snapshot, &error) ||
        !aml::instances::content_file_matches_snapshot(instance, snapshot_entry, snapshot, &error) ||
        !aml::instances::set_content_enabled(instance, snapshot_entry, false, &error) ||
        fs::exists(snapshot_mod) || !fs::exists(snapshot_mod.wstring() + L".disabled")) {
        std::cerr << "FAILED: profile-qualified content toggle " << error << "\n";
        return 1;
    }
    snapshot_entry.path += L".disabled";
    snapshot_entry.filename += ".disabled";
    error.clear();
    if (!aml::instances::set_content_enabled(instance, snapshot_entry, true, &error) ||
        !fs::exists(snapshot_mod)) {
        std::cerr << "FAILED: profile-qualified content re-enable " << error << "\n";
        return 1;
    }
    snapshot_entry.path = snapshot_mod.wstring();
    snapshot_entry.filename = "snapshot-fixture.jar";
    if (!write_file(snapshot_mod, "replacement payload with a different length") ||
        aml::instances::content_file_matches_snapshot(instance, snapshot_entry, snapshot, &error) ||
        error.empty()) {
        std::cerr << "FAILED: stale content snapshot was accepted\n";
        return 1;
    }

    // A redirected mods directory must never allow a recovery action to move
    // an external file.  Some locked-down Windows environments do not permit
    // creating test symlinks; exercise the assertion whenever the OS allows
    // it without turning that platform restriction into a false test failure.
    const fs::path external_mods = root / "external-mods";
    const fs::path redirected_profile = root / "redirected-profile";
    const fs::path external_mod = external_mods / "outside.jar";
    fs::create_directories(external_mods);
    fs::create_directories(redirected_profile);
    if (!write_file(external_mod, "outside")) {
        std::cerr << "FAILED: redirected profile fixture\n";
        return 1;
    }
    std::error_code link_error;
    fs::create_directory_symlink(external_mods, redirected_profile / "mods", link_error);
    if (!link_error) {
        aml::instances::Instance redirected_instance;
        redirected_instance.directory = redirected_profile.wstring();
        aml::instances::ContentEntry redirected_entry;
        redirected_entry.path = (redirected_profile / "mods" / "outside.jar").wstring();
        redirected_entry.filename = "outside.jar";
        redirected_entry.type = aml::instances::ContentType::Mod;
        aml::instances::ContentFileSnapshot redirected_snapshot;
        error.clear();
        if (aml::instances::capture_content_file_snapshot(
                redirected_instance, redirected_entry, redirected_snapshot, &error) ||
            aml::instances::move_content_to_trash(redirected_instance, redirected_entry, nullptr, &error) ||
            !fs::exists(external_mod)) {
            std::cerr << "FAILED: redirected content root was accepted " << error << "\n";
            return 1;
        }
    }

    fs::path restore_profile = root / "restore-profile";
    fs::create_directories(restore_profile / "mods");
    fs::create_directories(restore_profile / "config");
    aml::instances::Instance restore_instance;
    restore_instance.id = "restore-profile";
    restore_instance.name = "Before restore";
    restore_instance.directory = restore_profile.wstring();
    if (!aml::instances::save(restore_instance, &error) ||
        !write_file(restore_profile / "mods" / "before.jar", "before") ||
        !write_file(restore_profile / "config" / "before.cfg", "before") ||
        !write_file(restore_profile / "amalgam-dependencies.json", "before-metadata") ||
        !write_file(restore_profile / "options.txt", "guiScale:2") ||
        !aml::instances::create_restore_point(restore_instance, nullptr, &error)) {
        std::cerr << "FAILED: could not prepare restore fixture " << error << "\n";
        return 1;
    }

    restore_instance.name = "After restore";
    if (!aml::instances::save(restore_instance, &error) ||
        !write_file(restore_profile / "mods" / "before.jar", "after") ||
        !write_file(restore_profile / "mods" / "added.jar", "added") ||
        !write_file(restore_profile / "config" / "before.cfg", "after") ||
        !write_file(restore_profile / "amalgam-dependencies.json", "after-metadata") ||
        !write_file(restore_profile / "options.txt", "guiScale:3") ||
        !aml::instances::restore_latest(restore_instance, &error)) {
        std::cerr << "FAILED: restore latest " << error << "\n";
        return 1;
    }

    aml::instances::Instance restored;
    const bool restored_before_jar = read_file(restore_profile / "mods" / "before.jar") == "before";
    const bool restored_removed_added = !fs::exists(restore_profile / "mods" / "added.jar");
    const bool restored_config = read_file(restore_profile / "config" / "before.cfg") == "before";
    const bool restored_metadata = read_file(restore_profile / "amalgam-dependencies.json") == "before-metadata";
    const bool restored_options = read_file(restore_profile / "options.txt") == "guiScale:2";
    error.clear();
    const bool loaded_instance = aml::instances::load(restore_profile.wstring(), restored, &error);
    const bool restored_instance = loaded_instance && restored.name == "Before restore";
    if (!restored_before_jar || !restored_removed_added || !restored_config || !restored_metadata ||
        !restored_options || !restored_instance) {
        std::cerr << "FAILED: restore did not replace the complete managed payload"
                  << " [jar=" << restored_before_jar
                  << " removed=" << restored_removed_added
                  << " config=" << restored_config
                  << " metadata=" << restored_metadata
                  << " options=" << restored_options
                  << " instance=" << restored_instance
                  << " loaded=" << loaded_instance
                  << " error=" << error
                  << " name=" << restored.name << "]\n";
        return 1;
    }

    // The safety backup created during restore is a normal restore point, so a
    // second restore is a user-visible undo of the first one.
    if (!aml::instances::restore_latest(restored, &error) ||
        read_file(restore_profile / "mods" / "before.jar") != "after" ||
        !fs::exists(restore_profile / "mods" / "added.jar") ||
        read_file(restore_profile / "amalgam-dependencies.json") != "after-metadata" ||
        read_file(restore_profile / "options.txt") != "guiScale:3" ||
        !aml::instances::load(restore_profile.wstring(), restored, &error) ||
        restored.name != "After restore") {
        std::cerr << "FAILED: restore safety backup was not recoverable " << error << "\n";
        return 1;
    }

    const fs::path world = restore_profile / "saves" / "My World";
    const fs::path outside_world = root / "outside-world";
    fs::create_directories(world);
    fs::create_directories(outside_world);
    if (!write_file(world / "level.dat", "world") || !write_file(outside_world / "level.dat", "outside")) {
        std::cerr << "FAILED: world recovery fixture\n";
        return 1;
    }
    std::wstring backed_up_world;
    std::wstring trashed_world;
    if (!aml::instances::backup_world(restored, world.wstring(), &backed_up_world, &error) ||
        !fs::exists(world) || read_file(fs::path(backed_up_world) / "level.dat") != "world" ||
        !aml::instances::move_world_to_trash(restored, world.wstring(), &trashed_world, &error) ||
        fs::exists(world) || read_file(fs::path(trashed_world) / "level.dat") != "world" ||
        aml::instances::move_world_to_trash(restored, outside_world.wstring(), nullptr, &error)) {
        std::cerr << "FAILED: world backup/recovery handling " << error << "\n";
        return 1;
    }

    fs::create_directories(restore_profile / "screenshots");
    const fs::path screenshot = restore_profile / "screenshots" / "proof.png";
    const fs::path outside_screenshot = root / "outside.png";
    if (!write_file(screenshot, "image") || !write_file(outside_screenshot, "outside")) {
        std::cerr << "FAILED: screenshot recovery fixture\n";
        return 1;
    }
    std::wstring trashed_screenshot;
    if (!aml::instances::move_screenshot_to_trash(restored, screenshot.wstring(),
                                                   &trashed_screenshot, &error) ||
        fs::exists(screenshot) || read_file(trashed_screenshot) != "image" ||
        aml::instances::move_screenshot_to_trash(restored, outside_screenshot.wstring(),
                                                 nullptr, &error)) {
        std::cerr << "FAILED: screenshot recovery handling " << error << "\n";
        return 1;
    }

    const fs::path removable_profile = root / "instances" / "remove-me";
    fs::create_directories(removable_profile / "mods");
    aml::instances::Instance removable;
    removable.id = "remove-me";
    removable.name = "Remove Me";
    removable.directory = removable_profile.wstring();
    if (!aml::instances::save(removable, &error) ||
        !write_file(removable_profile / "mods" / "keep.jar", "keep")) {
        std::cerr << "FAILED: profile recovery fixture\n";
        return 1;
    }
    const fs::path profile_recovery = removable_profile.parent_path() / L".amalgam-profile-recovery";
    if (!aml::instances::remove(removable, &error) || fs::exists(removable_profile) ||
        !fs::exists(profile_recovery) || std::filesystem::is_empty(profile_recovery) ||
        aml::instances::remove(removable, &error)) {
        std::cerr << "FAILED: profile recovery handling " << error << "\n";
        return 1;
    }
    fs::remove_all(root);
    return 0;
}
