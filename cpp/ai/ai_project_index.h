#pragma once

#include "ai_core.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace aml::ai {

// ──────────────────────────────────────────────
// Project Index — indexed knowledge of a profile
// ──────────────────────────────────────────────

struct IndexedFile {
    std::wstring path;          // absolute path
    std::string  rel;           // path relative to profile root
    std::string  category;      // "kubejs", "config", "datapack", "resourcepack", "java", "mod", "quest", "log", "crash", "fancymenu"
    int64_t      size = 0;
    int64_t      mtime = 0;
};

struct ProfileSnapshot {
    std::string  minecraft_version;
    std::string  loader;
    std::string  loader_version;
    std::string  java_version;
    int          mod_count = 0;
    std::vector<std::string> mod_ids;     // mod file names or ids
    std::vector<std::string> mod_versions;
    std::vector<IndexedFile> files;
};

// Scans a profile directory and builds the file index.
// Scans every frame-expensive filesystem; call on open and on demand.
ProfileSnapshot build_profile_index(const std::wstring& profile_root, std::string* err);

// Returns up to `limit` index entries whose path/rel contains `needle`.
std::vector<IndexedFile> search_index(const ProfileSnapshot& snap, const std::string& needle, int limit = 20);

// Compact textual summary of a profile for the model context window.
std::string profile_summary(const ProfileSnapshot& snap, int max_files = 60);

}  // namespace aml::ai