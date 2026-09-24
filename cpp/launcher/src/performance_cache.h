#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace aml::performance_cache {

// The launcher owns exactly these two local cache roots.  Keeping the roots
// explicit makes scans and cleanup testable without touching a user's real
// temporary directory.
struct CacheRoots {
    std::filesystem::path cache_root;
    std::filesystem::path temporary_root;
};

struct CacheUsage {
    uint64_t total_bytes = 0;
    uint64_t item_count = 0;
    uint64_t mod_metadata_bytes = 0;
    uint64_t instance_config_bytes = 0;
    uint64_t download_bytes = 0;
    uint64_t temporary_bytes = 0;
    uint64_t other_launcher_bytes = 0;
    // A partial scan still returns the trustworthy portion of the data.  The
    // UI can show it with a refresh hint instead of presenting an invented 0.
    bool complete = true;
    std::string error;
};

enum class ClearScope {
    All,
    ModMetadata,
    Downloads,
    TemporaryFiles,
};

struct ClearResult {
    bool success = false;
    std::string error;
};

// Resolves the launcher-managed paths below the process temporary directory.
// No files are enumerated or created by this helper.
bool default_cache_roots(CacheRoots& roots, std::string* error = nullptr);

// Performs at most one recursive walk per root.  Missing roots are a valid,
// empty cache.  Call this from a worker rather than from the render path.
CacheUsage scan_cache_usage(const CacheRoots& roots);

// Removes only the root(s) represented by the requested scope.  Call this
// from a serialized worker; it never expands its deletion target beyond the
// provided CacheRoots.
ClearResult clear_cache(const CacheRoots& roots, ClearScope scope);

}  // namespace aml::performance_cache
