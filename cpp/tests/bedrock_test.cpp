#include "bedrock.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

class ScopedEnvironment {
public:
    ScopedEnvironment(const wchar_t* name, const std::wstring& value) : name_(name) {
        const DWORD needed = GetEnvironmentVariableW(name_, nullptr, 0);
        if (needed > 0) {
            std::vector<wchar_t> buffer(needed);
            if (GetEnvironmentVariableW(name_, buffer.data(), needed) > 0) {
                had_original_ = true;
                original_ = buffer.data();
            }
        }
        SetEnvironmentVariableW(name_, value.c_str());
    }

    ~ScopedEnvironment() {
        SetEnvironmentVariableW(name_, had_original_ ? original_.c_str() : nullptr);
    }

private:
    const wchar_t* name_;
    bool had_original_ = false;
    std::wstring original_;
};

bool write_text(const std::filesystem::path& path, const std::string& contents) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return false;
    std::ofstream out(path, std::ios::binary);
    out << contents;
    return static_cast<bool>(out);
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool test_restore_is_staged_and_recoverable(const std::filesystem::path& root) {
    namespace fs = std::filesystem;
    const fs::path data_root = root / "data";
    const fs::path profile_root = data_root / "profiles" / "fixture-profile";
    const fs::path live_saves = profile_root / "saves";
    const fs::path backup_root = profile_root / "backups";
    const fs::path selected_backup = backup_root / "before-restore";

    if (!write_text(live_saves / "old-world" / "level.dat", "old-level") ||
        !write_text(live_saves / "old-world" / "db" / "chunk.bin", "old-chunk") ||
        !write_text(selected_backup / "new-world" / "level.dat", "new-level") ||
        !write_text(selected_backup / "new-world" / "db" / "chunk.bin", "new-chunk")) {
        std::cerr << "FAILED: cannot create Bedrock restore fixture\n";
        return false;
    }

    ScopedEnvironment test_data_root(L"AMALGAM_BEDROCK_TEST_DATA_DIR", data_root.wstring());
    aml::bedrock::BedrockProfile profile;
    profile.id = "fixture-profile";
    aml::bedrock::BedrockBackupEntry backup;
    backup.id = "before-restore";
    backup.profile_id = profile.id;
    std::string error;

    // A rejected restore must leave the current worlds untouched before any
    // staging or automatic-backup operation begins.
    aml::bedrock::BedrockBackupEntry missing = backup;
    missing.id = "missing-backup";
    if (aml::bedrock::restore_backup(profile, missing, &error) ||
        read_text(live_saves / "old-world" / "level.dat") != "old-level" ||
        !fs::exists(selected_backup / "new-world" / "level.dat")) {
        std::cerr << "FAILED: rejected restore changed current Bedrock worlds\n";
        return false;
    }

    // The failure point after the original folder has moved is deliberately
    // injected by the test-only Bedrock target.  A failed activation must roll
    // the original worlds back into place rather than leaving the profile
    // empty or only in a hidden staging directory.
    {
        ScopedEnvironment fail_activation(L"AMALGAM_BEDROCK_TEST_FAIL_ACTIVATION", L"1");
        error.clear();
        if (aml::bedrock::restore_backup(profile, backup, &error) ||
            read_text(live_saves / "old-world" / "level.dat") != "old-level" ||
            read_text(live_saves / "old-world" / "db" / "chunk.bin") != "old-chunk" ||
            !fs::exists(selected_backup / "new-world" / "level.dat")) {
            std::cerr << "FAILED: activation failure did not restore original Bedrock worlds\n";
            return false;
        }
    }

    error.clear();
    if (!aml::bedrock::restore_backup(profile, backup, &error)) {
        std::cerr << "FAILED: staged Bedrock restore failed: " << error << "\n";
        return false;
    }
    if (read_text(live_saves / "new-world" / "level.dat") != "new-level" ||
        read_text(live_saves / "new-world" / "db" / "chunk.bin") != "new-chunk" ||
        fs::exists(live_saves / "old-world")) {
        std::cerr << "FAILED: restore did not atomically activate the selected backup\n";
        return false;
    }

    bool preserved_original = false;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(backup_root, ec)) {
        if (ec) break;
        const std::wstring name = entry.path().filename().wstring();
        if (name.rfind(L"pre_restore_", 0) == 0 &&
            read_text(entry.path() / "old-world" / "level.dat") == "old-level" &&
            read_text(entry.path() / "old-world" / "db" / "chunk.bin") == "old-chunk") {
            preserved_original = true;
            break;
        }
    }
    if (ec || !preserved_original) {
        std::cerr << "FAILED: restore did not retain an automatic pre-restore backup\n";
        return false;
    }

    for (const auto& entry : fs::directory_iterator(profile_root, ec)) {
        if (ec) break;
        if (entry.path().filename().wstring().rfind(L"restore-stage-", 0) == 0) {
            std::cerr << "FAILED: completed restore left a staging directory\n";
            return false;
        }
    }
    if (ec) {
        std::cerr << "FAILED: cannot inspect restore fixture after success\n";
        return false;
    }
    return true;
}

bool test_destructive_targets_are_checked(const std::filesystem::path& root) {
    namespace fs = std::filesystem;
    const fs::path data_root = root / "data";
    const fs::path profile_root = data_root / "profiles" / "fixture-profile";
    const fs::path world = profile_root / "saves" / "keep-or-delete";
    const fs::path backup = profile_root / "backups" / "checked-backup";
    if (!write_text(world / "level.dat", "world-data") ||
        !write_text(backup / "level.dat", "backup-data")) {
        std::cerr << "FAILED: cannot create destructive-target fixture\n";
        return false;
    }

    ScopedEnvironment test_data_root(L"AMALGAM_BEDROCK_TEST_DATA_DIR", data_root.wstring());
    aml::bedrock::BedrockProfile profile;
    profile.id = "fixture-profile";
    std::string error;

    // Unsafe folders must be rejected before a path is resolved, leaving the
    // selected world untouched.
    if (aml::bedrock::delete_world(profile, "..", &error) ||
        read_text(world / "level.dat") != "world-data") {
        std::cerr << "FAILED: unsafe world deletion was not rejected\n";
        return false;
    }
    error.clear();
    if (aml::bedrock::delete_world(profile, "missing-world", &error) ||
        read_text(world / "level.dat") != "world-data") {
        std::cerr << "FAILED: missing world deletion changed an existing world\n";
        return false;
    }
    error.clear();
    if (!aml::bedrock::delete_world(profile, "keep-or-delete", &error) ||
        fs::exists(world)) {
        std::cerr << "FAILED: checked world deletion did not remove only the target\n";
        return false;
    }

    aml::bedrock::BedrockBackupEntry wrong_profile;
    wrong_profile.id = "checked-backup";
    wrong_profile.profile_id = "another-profile";
    error.clear();
    if (aml::bedrock::delete_backup(profile, wrong_profile, &error) ||
        read_text(backup / "level.dat") != "backup-data") {
        std::cerr << "FAILED: mismatched backup deletion was not rejected\n";
        return false;
    }

    aml::bedrock::BedrockBackupEntry selected_backup;
    selected_backup.id = "checked-backup";
    selected_backup.profile_id = profile.id;
    error.clear();
    if (!aml::bedrock::delete_backup(profile, selected_backup, &error) ||
        fs::exists(backup)) {
        std::cerr << "FAILED: checked backup deletion did not remove the target\n";
        return false;
    }
    return true;
}

}  // namespace

int main() {
    namespace fs = std::filesystem;
    auto temporary_path = [](const wchar_t* suffix) -> std::wstring {
        wchar_t directory[MAX_PATH]{};
        wchar_t file[MAX_PATH]{};
        if (GetTempPathW(MAX_PATH, directory) == 0 ||
            GetTempFileNameW(directory, L"aml", 0, file) == 0)
            return {};
        std::wstring path = file;
        DeleteFileW(path.c_str());
        return path + suffix;
    };
    if (aml::bedrock::uwp_family() != L"Microsoft.MinecraftUWP_8wekyb3d8bbwe") {
        std::cerr << "FAILED: Bedrock package family changed unexpectedly\n";
        return 1;
    }
    fs::path root = temporary_path(L"_bedrock_test");
    fs::remove_all(root);
    fs::create_directories(root);
    fs::path valid = root / "example.mcpack";
    const char valid_bytes[] = {'P', 'K', 3, 4, 'a', 'd', 'd', 'o', 'n'};
    std::ofstream valid_file(valid, std::ios::binary);
    valid_file.write(valid_bytes, sizeof(valid_bytes));
    valid_file.close();
    std::string error;
    if (!aml::bedrock::validate_addon_file(valid.wstring(), &error)) {
        std::cerr << "FAILED: valid addon rejected: " << error << "\n";
        return 1;
    }
    fs::path invalid = root / "example.txt";
    std::ofstream invalid_file(invalid, std::ios::binary);
    invalid_file << "not an addon";
    invalid_file.close();
    if (aml::bedrock::validate_addon_file(invalid.wstring(), &error)) {
        std::cerr << "FAILED: invalid addon accepted\n";
        return 1;
    }
    fs::remove_all(root);

    fs::path restore_root = temporary_path(L"_bedrock_restore_test");
    fs::remove_all(restore_root);
    fs::create_directories(restore_root);
    const bool restore_ok = test_restore_is_staged_and_recoverable(restore_root);
    fs::remove_all(restore_root);

    fs::path destructive_root = temporary_path(L"_bedrock_destructive_target_test");
    fs::remove_all(destructive_root);
    fs::create_directories(destructive_root);
    const bool destructive_targets_ok = test_destructive_targets_are_checked(destructive_root);
    fs::remove_all(destructive_root);
    return restore_ok && destructive_targets_ok ? 0 : 1;
}
