#include "performance_cache.h"

#include <filesystem>
#include <limits>
#include <system_error>

namespace aml::performance_cache {
namespace {

namespace fs = std::filesystem;

void record_scan_issue(CacheUsage& usage) {
    usage.complete = false;
    if (usage.error.empty()) {
        usage.error = "Some cache files could not be read. Try refreshing cache usage.";
    }
}

void add_bytes(uint64_t& destination, uint64_t value) {
    if (std::numeric_limits<uint64_t>::max() - destination < value) {
        destination = std::numeric_limits<uint64_t>::max();
    } else {
        destination += value;
    }
}

void add_launcher_cache_bytes(CacheUsage& usage, const fs::path& root,
                              const fs::path& file, uint64_t bytes) {
    add_bytes(usage.total_bytes, bytes);
    const fs::path relative = file.lexically_relative(root);
    const auto first = relative.begin();
    if (first == relative.end()) {
        add_bytes(usage.other_launcher_bytes, bytes);
        return;
    }
    const std::wstring category = first->wstring();
    if (category == L"mods") {
        add_bytes(usage.mod_metadata_bytes, bytes);
    } else if (category == L"configs") {
        add_bytes(usage.instance_config_bytes, bytes);
    } else if (category == L"downloads") {
        add_bytes(usage.download_bytes, bytes);
    } else {
        add_bytes(usage.other_launcher_bytes, bytes);
    }
}

void scan_root(const fs::path& root, bool launcher_cache, CacheUsage& usage) {
    if (root.empty()) {
        record_scan_issue(usage);
        return;
    }

    std::error_code ec;
    if (!fs::exists(root, ec)) {
        if (ec) record_scan_issue(usage);
        return;
    }
    if (!fs::is_directory(root, ec)) {
        record_scan_issue(usage);
        return;
    }

    const fs::directory_options options = fs::directory_options::skip_permission_denied;
    fs::recursive_directory_iterator it(root, options, ec);
    const fs::recursive_directory_iterator end;
    if (ec) {
        record_scan_issue(usage);
        return;
    }

    while (it != end) {
        const fs::directory_entry& entry = *it;
        std::error_code entry_error;
        if (entry.is_regular_file(entry_error)) {
            if (usage.item_count != std::numeric_limits<uint64_t>::max()) {
                ++usage.item_count;
            }
            const uint64_t bytes = entry.file_size(entry_error);
            if (entry_error) {
                record_scan_issue(usage);
            } else if (launcher_cache) {
                add_launcher_cache_bytes(usage, root, entry.path(), bytes);
            } else {
                add_bytes(usage.total_bytes, bytes);
                add_bytes(usage.temporary_bytes, bytes);
            }
        } else if (entry_error) {
            record_scan_issue(usage);
        }

        it.increment(ec);
        if (ec) {
            record_scan_issue(usage);
            break;
        }
    }
}

void remove_root(const fs::path& root, ClearResult& result) {
    if (root.empty()) {
        result.success = false;
        if (result.error.empty()) {
            result.error = "The launcher cache location could not be determined.";
        }
        return;
    }
    std::error_code ec;
    fs::remove_all(root, ec);
    if (ec) {
        result.success = false;
        if (result.error.empty()) {
            result.error = "Some cached data could not be removed.";
        }
    }
}

}  // namespace

bool default_cache_roots(CacheRoots& roots, std::string* error) {
    roots = {};
    std::error_code ec;
    const fs::path temporary_directory = fs::temp_directory_path(ec);
    if (ec || temporary_directory.empty()) {
        if (error) *error = "The launcher cache location could not be determined.";
        return false;
    }
    roots.cache_root = temporary_directory / L"amalgam_cache";
    roots.temporary_root = temporary_directory / L"amalgam";
    if (error) error->clear();
    return true;
}

CacheUsage scan_cache_usage(const CacheRoots& roots) {
    CacheUsage usage;
    if (roots.cache_root.empty() || roots.temporary_root.empty()) {
        usage.complete = false;
        usage.error = "The launcher cache location could not be determined.";
        return usage;
    }
    scan_root(roots.cache_root, true, usage);
    scan_root(roots.temporary_root, false, usage);
    return usage;
}

ClearResult clear_cache(const CacheRoots& roots, ClearScope scope) {
    ClearResult result;
    result.success = true;
    if (roots.cache_root.empty() || roots.temporary_root.empty()) {
        result.success = false;
        result.error = "The launcher cache location could not be determined.";
        return result;
    }
    switch (scope) {
        case ClearScope::All:
            remove_root(roots.cache_root, result);
            remove_root(roots.temporary_root, result);
            break;
        case ClearScope::ModMetadata:
            remove_root(roots.cache_root / L"mods", result);
            break;
        case ClearScope::Downloads:
            remove_root(roots.cache_root / L"downloads", result);
            break;
        case ClearScope::TemporaryFiles:
            remove_root(roots.temporary_root, result);
            break;
    }
    return result;
}

}  // namespace aml::performance_cache
