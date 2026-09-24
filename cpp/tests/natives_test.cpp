#include "extract.h"
#include "natives.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct TempRoot {
    std::wstring path;

    ~TempRoot() {
        if (!path.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(std::filesystem::path(path), ignored);
        }
    }
};

bool write_file(const std::wstring& path, const std::string& contents) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    return static_cast<bool>(out);
}

bool exists(const std::wstring& path) {
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::path(path), ec) && !ec;
}

}  // namespace

int main() {
    wchar_t temp_dir[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, temp_dir) == 0) {
        std::cerr << "cannot locate temporary directory\n";
        return 1;
    }
    wchar_t unique[MAX_PATH]{};
    if (GetTempFileNameW(temp_dir, L"aml", 0, unique) == 0) {
        std::cerr << "cannot allocate temporary native test root\n";
        return 1;
    }
    DeleteFileW(unique);
    TempRoot root{unique};
    if (!CreateDirectoryW(root.path.c_str(), nullptr)) {
        std::cerr << "cannot create temporary native test root\n";
        return 1;
    }

    const std::wstring source = root.path + L"\\source";
    const std::wstring archive_path = root.path + L"\\native-fixture.zip";
    if (!write_file(source + L"\\keep.dll", "native payload") ||
        !write_file(source + L"\\META-INF\\MANIFEST.MF", "must be excluded")) {
        std::cerr << "cannot write native fixture\n";
        return 1;
    }
    std::string error;
    if (!aml::extract::create_zip(source, archive_path, &error)) {
        std::cerr << "cannot create native fixture archive: " << error << "\n";
        return 1;
    }
    // Version metadata is untrusted input.  An exclusion may omit files from
    // the layout, but it must never be accepted as a traversal-shaped path.
    const std::wstring rejected_output = root.path + L"\\rejected-native-output";
    if (aml::extract::zip_excluding(archive_path, rejected_output, {"../escape"}, &error) ||
        exists(rejected_output)) {
        std::cerr << "unsafe native extract.exclude rule was accepted\n";
        return 1;
    }

    const std::vector<aml::natives::Archive> archives = {
        {archive_path, {"META-INF/"}},
    };
    std::string fingerprint;
    if (!aml::natives::layout_fingerprint(archives, &fingerprint, &error) ||
        fingerprint.size() != 64) {
        std::cerr << "cannot fingerprint native layout: " << error << "\n";
        return 1;
    }

    // The pre-existing root represents a legacy overlay from an earlier
    // launcher.  It must survive while the new launch points at a clean,
    // fingerprinted child directory instead.
    const std::wstring native_root = root.path + L"\\instance\\natives";
    if (!write_file(native_root + L"\\legacy.dll", "keep legacy data")) {
        std::cerr << "cannot create legacy native fixture\n";
        return 1;
    }
    std::wstring active;
    if (!aml::natives::prepare_layout(native_root, archives, &active, &error)) {
        std::cerr << "cannot prepare native layout: " << error << "\n";
        return 1;
    }
    const std::wstring canonical = native_root + L"\\" + std::wstring(fingerprint.begin(), fingerprint.end());
    if (active != canonical || !exists(native_root + L"\\legacy.dll") ||
        !exists(active + L"\\keep.dll") || exists(active + L"\\META-INF\\MANIFEST.MF")) {
        std::cerr << "native layout was not isolated or did not honor extract.exclude\n";
        return 1;
    }
    std::wstring reused;
    if (!aml::natives::prepare_layout(native_root, archives, &reused, &error) || reused != active) {
        std::cerr << "complete native layout was not reused: " << error << "\n";
        return 1;
    }

    // An unmarked canonical directory is treated as an interrupted or
    // user-owned attempt.  It is left untouched and the launcher promotes a
    // separate recovery directory rather than deleting anything in the game.
    const std::wstring recovery_root = root.path + L"\\recovery-instance\\natives";
    const std::wstring incomplete = recovery_root + L"\\" +
                                    std::wstring(fingerprint.begin(), fingerprint.end());
    if (!write_file(incomplete + L"\\do-not-delete.dll", "preserve me")) {
        std::cerr << "cannot create incomplete native fixture\n";
        return 1;
    }
    std::wstring recovered;
    if (!aml::natives::prepare_layout(recovery_root, archives, &recovered, &error) ||
        recovered == incomplete || !exists(incomplete + L"\\do-not-delete.dll") ||
        !exists(recovered + L"\\keep.dll")) {
        std::cerr << "incomplete native layout was not safely recovered: " << error << "\n";
        return 1;
    }
    std::wstring recovered_reuse;
    if (!aml::natives::prepare_layout(recovery_root, archives, &recovered_reuse, &error) ||
        recovered_reuse != recovered) {
        std::cerr << "recovery native layout was not reused: " << error << "\n";
        return 1;
    }

    std::cout << "natives_test passed\n";
    return 0;
}
