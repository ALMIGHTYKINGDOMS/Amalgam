// Atomic config persistence tests.
//
// json_write_file must never destroy the last good configuration: it stages
// a temp file, validates it, atomically replaces the destination, and keeps
// a last-known-good .bak. json_parse_file must recover from the .bak when
// the primary is corrupt or truncated. These tests exercise those paths on
// real files in a temporary directory.

#include "json.h"

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

std::wstring temp_dir() {
    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    wchar_t name[MAX_PATH]{};
    GetTempFileNameW(tmp, L"amalj", 0, name);
    DeleteFileW(name);
    std::filesystem::create_directories(name);
    return name;
}

void write_text(const std::wstring& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << text;
}

std::string read_text(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

aml::Json config_json(const char* key, const char* value) {
    aml::Json j = aml::Json::obj();
    j.set("version", aml::Json::str("3.0"));
    j.set(key, aml::Json::str(value));
    return j;
}

void test_atomic_save_and_backup() {
    const std::wstring root = temp_dir();
    const std::wstring path = root + L"\\launcher.json";
    std::string err;

    // First save: creates the file (no previous file to back up).
    check(aml::json_write_file(path, config_json("a", "one"), &err),
          "first atomic save succeeds");
    check(std::filesystem::exists(path), "primary written");
    check(!std::filesystem::exists(path + L".bak"),
          "no backup for a brand-new file");

    // Second save: primary updated, backup holds the previous content.
    check(aml::json_write_file(path, config_json("a", "two"), &err),
          "second atomic save succeeds");
    check(read_text(path).find("\"two\"") != std::string::npos,
          "primary holds newest content");
    check(std::filesystem::exists(path + L".bak"),
          "last-known-good backup created");
    check(read_text(path + L".bak").find("\"one\"") != std::string::npos,
          "backup holds previous content");

    std::filesystem::remove_all(root);
}

void test_corrupt_primary_recovers_from_backup() {
    const std::wstring root = temp_dir();
    const std::wstring path = root + L"\\launcher.json";

    // Two saves so a last-known-good backup exists (the first save of a
    // brand-new file has no previous content to back up).
    check(aml::json_write_file(path, config_json("a", "first"), nullptr),
          "save v1");
    check(aml::json_write_file(path, config_json("a", "good"), nullptr),
          "save v2");
    // Corrupt the primary (partial write / truncation simulation).
    write_text(path, "{\"version\":\"3.0\",\"a\":\"g");
    aml::Json out;
    std::string err;
    check(aml::json_parse_file(path, out, &err),
          "corrupt primary recovers from backup");
    // Recovery returns the last-known-good backup (the previous save).
    check(out.get("a").as_str() == "first", "recovery returns the last-known-good state");

    std::filesystem::remove_all(root);
}

void test_truncated_primary_recovers_from_backup() {
    const std::wstring root = temp_dir();
    const std::wstring path = root + L"\\launcher.json";

    check(aml::json_write_file(path, config_json("a", "first"), nullptr),
          "save v1");
    check(aml::json_write_file(path, config_json("a", "whole"), nullptr),
          "save v2");
    // Simulate a crash mid-write: the primary is cut off mid-document.
    const std::string full = read_text(path);
    write_text(path, full.substr(0, full.size() / 2));

    aml::Json out;
    std::string err;
    check(aml::json_parse_file(path, out, &err),
          "truncated primary recovers from backup");
    check(out.get("a").as_str() == "first", "truncation recovery returns last-known-good state");

    std::filesystem::remove_all(root);
}

void test_invalid_json_without_backup_fails() {
    const std::wstring root = temp_dir();
    const std::wstring path = root + L"\\launcher.json";
    write_text(path, "this is not json at all");

    aml::Json out;
    std::string err;
    check(!aml::json_parse_file(path, out, &err),
          "invalid JSON without backup is rejected");
    check(!err.empty(), "parse failure explains itself");

    std::filesystem::remove_all(root);
}

void test_failed_replace_preserves_previous() {
    const std::wstring root = temp_dir();
    const std::wstring path = root + L"\\launcher.json";

    check(aml::json_write_file(path, config_json("a", "keep"), nullptr),
          "save the good config");
    const std::string before = read_text(path);

    // Make the destination read-only so the replacement must fail.
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY);
    std::string err;
    check(!aml::json_write_file(path, config_json("a", "clobber"), &err),
          "write to read-only destination fails");
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);

    check(read_text(path) == before,
          "failed replacement preserves the previous config");
    check(read_text(path).find("\"keep\"") != std::string::npos,
          "previous content intact after failure");

    std::filesystem::remove_all(root);
}

void test_stress_cycles_and_backup_boundedness() {
    const std::wstring root = temp_dir();
    const std::wstring path = root + L"\\launcher.json";

    // 1000 alternating write/read cycles: the last committed state must
    // always survive, the primary must never be partially written, and the
    // backup must remain parseable throughout.
    for (int i = 0; i < 1000; ++i) {
        const std::string tag = "cycle-" + std::to_string(i);
        std::string err;
        check(aml::json_write_file(path, config_json("tag", tag.c_str()), &err),
              "stress write succeeds");
        if (i % 50 == 0 || i == 999) {
            aml::Json out;
            std::string perr;
            check(aml::json_parse_file(path, out, &perr),
                  "stress read parses");
            check(out.get("tag").as_str() == tag,
                  "stress read returns last committed state");
        }
        // The primary must never contain a partially-written document:
        // parse it raw and require it to be complete JSON.
        std::string raw = read_text(path);
        aml::Json raw_parse;
        std::string raw_err;
        raw_parse = aml::Json::parse(raw, &raw_err);
        check(raw_err.empty(), "primary never partially written");
    }

    // Backup boundedness: after many writes the backup directory holds only
    // the single .bak (plus the primary) — rotation must not accumulate.
    int bak_count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        const std::string name = entry.path().filename().string();
        if (name.find(".bak") != std::string::npos) ++bak_count;
    }
    check(bak_count == 1, "exactly one last-known-good backup retained");

    // The backup must remain usable as a recovery source at the end.
    write_text(path, "{\"broken");
    aml::Json recovered;
    std::string err2;
    check(aml::json_parse_file(path, recovered, &err2),
          "backup usable for recovery after stress");

    std::filesystem::remove_all(root);
}

void test_directory_destination_fails_safely() {
    const std::wstring root = temp_dir();
    const std::wstring path = root + L"\\not-a-file";
    std::filesystem::create_directories(path);

    std::string err;
    check(!aml::json_write_file(path, config_json("a", "x"), &err),
          "directory destination is rejected");
    check(std::filesystem::is_directory(path),
          "destination directory left untouched");

    std::filesystem::remove_all(root);
}

}  // namespace

int main() {
    test_atomic_save_and_backup();
    test_corrupt_primary_recovers_from_backup();
    test_truncated_primary_recovers_from_backup();
    test_invalid_json_without_backup_fails();
    test_failed_replace_preserves_previous();
    test_directory_destination_fails_safely();
    test_stress_cycles_and_backup_boundedness();

    if (g_failures == 0) {
        std::printf("json atomic tests passed\n");
        return 0;
    }
    std::printf("%d json atomic test(s) failed\n", g_failures);
    return 1;
}
