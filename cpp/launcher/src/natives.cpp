#include "natives.h"

#include "extract.h"
#include "net.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace aml::natives {

namespace {

constexpr char kMarkerHeader[] = "amalgam-native-layout-v1\n";
constexpr wchar_t kMarkerName[] = L".amalgam-native-layout";

struct FingerprintedArchive {
    const Archive* archive = nullptr;
    std::string sha256;
};

bool path_exists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

void append_field(std::string* out, const std::string& field) {
    *out += std::to_string(field.size());
    *out += ':';
    *out += field;
    out->push_back('\n');
}

bool fingerprint_archives(const std::vector<Archive>& archives, std::string* fingerprint,
                         std::vector<FingerprintedArchive>* prepared, std::string* error) {
    if (fingerprint) fingerprint->clear();
    if (prepared) prepared->clear();

    std::string layout = "amalgam-native-layout-v1\n";
    append_field(&layout, std::to_string(archives.size()));
    if (prepared) prepared->reserve(archives.size());
    for (const Archive& archive : archives) {
        if (archive.path.empty() || !net::file_exists(archive.path)) {
            if (error) *error = "required native archive is unavailable";
            return false;
        }
        const std::string hash = net::sha256_file(archive.path);
        if (hash.size() != 64) {
            if (error) *error = "cannot fingerprint native archive";
            return false;
        }

        append_field(&layout, hash);
        std::vector<std::string> exclusions = archive.exclusions;
        std::sort(exclusions.begin(), exclusions.end());
        exclusions.erase(std::unique(exclusions.begin(), exclusions.end()), exclusions.end());
        append_field(&layout, std::to_string(exclusions.size()));
        for (const std::string& exclusion : exclusions) append_field(&layout, exclusion);

        if (prepared) prepared->push_back(FingerprintedArchive{&archive, hash});
    }

    const std::string result = net::sha256_hex(layout.data(), layout.size());
    if (result.size() != 64) {
        if (error) *error = "cannot fingerprint native layout";
        return false;
    }
    if (fingerprint) *fingerprint = result;
    return true;
}

std::wstring marker_path(const std::wstring& directory) {
    return directory + L"\\" + kMarkerName;
}

std::string marker_contents(const std::string& fingerprint) {
    return std::string(kMarkerHeader) + fingerprint + "\n";
}

bool is_complete_layout(const std::wstring& directory, const std::string& fingerprint) {
    if (!net::directory_exists(directory)) return false;
    std::ifstream marker(marker_path(directory), std::ios::binary);
    if (!marker.is_open()) return false;
    std::string contents((std::istreambuf_iterator<char>(marker)),
                         std::istreambuf_iterator<char>());
    return contents == marker_contents(fingerprint);
}

bool write_completion_marker(const std::wstring& staging, const std::string& fingerprint,
                             std::string* error) {
    std::ofstream marker(marker_path(staging), std::ios::binary | std::ios::trunc);
    if (!marker.is_open()) {
        if (error) *error = "cannot mark native layout complete";
        return false;
    }
    const std::string contents = marker_contents(fingerprint);
    marker.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    marker.close();
    if (!marker) {
        if (error) *error = "cannot finalize native layout marker";
        return false;
    }
    return true;
}

std::wstring recovery_directory(const std::wstring& native_root, const std::string& fingerprint,
                                unsigned int attempt) {
    std::wstring name = net::to_wide(fingerprint);
    if (attempt == 1) name += L"-recovery";
    if (attempt > 1) name += L"-recovery-" + std::to_wstring(attempt - 1);
    return native_root + L"\\" + name;
}

}  // namespace

bool layout_fingerprint(const std::vector<Archive>& archives, std::string* fingerprint,
                        std::string* error) {
    return fingerprint_archives(archives, fingerprint, nullptr, error);
}

bool prepare_layout(const std::wstring& native_root, const std::vector<Archive>& archives,
                    std::wstring* active_directory, std::string* error) {
    if (active_directory) active_directory->clear();
    if (native_root.empty() || !net::mkdirs(native_root)) {
        if (error) *error = "cannot create native library directory";
        return false;
    }

    std::string fingerprint;
    std::vector<FingerprintedArchive> prepared;
    if (!fingerprint_archives(archives, &fingerprint, &prepared, error)) return false;

    const std::wstring canonical = recovery_directory(native_root, fingerprint, 0);
    if (is_complete_layout(canonical, fingerprint)) {
        if (active_directory) *active_directory = canonical;
        return true;
    }

    // A partial directory may be from an interrupted old launch or created by
    // the user.  Never erase it: build a separately named recovery layout.
    for (unsigned int attempt = 0; attempt < 16; ++attempt) {
        const std::wstring target = recovery_directory(native_root, fingerprint, attempt);
        if (is_complete_layout(target, fingerprint)) {
            if (active_directory) *active_directory = target;
            return true;
        }
        if (path_exists(target)) continue;

        const std::wstring staging = target + L".staging-" +
                                     std::to_wstring(GetCurrentProcessId()) + L"-" +
                                     std::to_wstring(GetTickCount64());
        if (!CreateDirectoryW(staging.c_str(), nullptr)) {
            if (GetLastError() == ERROR_ALREADY_EXISTS) continue;
            if (error) *error = "cannot create native extraction staging directory";
            return false;
        }

        for (const FingerprintedArchive& archive : prepared) {
            // Protect the content-addressed promotion invariant if another
            // process replaces an archive between fingerprinting and extract.
            if (net::sha256_file(archive.archive->path) != archive.sha256) {
                if (error) *error = "native archive changed while preparing launch";
                return false;
            }
            std::string extract_error;
            if (!extract::zip_excluding(archive.archive->path, staging,
                                        archive.archive->exclusions, &extract_error)) {
                if (error) {
                    *error = extract_error.empty() ? "native extraction failed" : extract_error;
                }
                return false;
            }
        }
        if (!write_completion_marker(staging, fingerprint, error)) return false;

        if (!MoveFileExW(staging.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) {
            const DWORD move_error = GetLastError();
            if (error) {
                char message[160];
                std::snprintf(message, sizeof(message),
                              "cannot promote native layout atomically (err=%lu)", move_error);
                *error = message;
            }
            return false;
        }
        if (active_directory) *active_directory = target;
        return true;
    }

    if (error) *error = "cannot reserve a safe native extraction directory";
    return false;
}

}  // namespace aml::natives
