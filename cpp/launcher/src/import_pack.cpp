#include "import_pack.h"

#include "extract.h"
#include "json.h"
#include "net.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <system_error>
#include <vector>

namespace aml::import_pack {

namespace {

namespace fs = std::filesystem;

bool report(const Progress& progress, float value, const std::string& text) {
    return !progress || progress(value, text);
}

bool safe_relative(const std::string& path) {
    if (path.empty() || path[0] == '/' || path[0] == '\\' || path.find(':') != std::string::npos)
        return false;
    fs::path p(path);
    if (p.is_absolute() || !p.root_name().empty() || !p.root_directory().empty()) return false;
    for (const auto& part : p) if (part == "..") return false;
    return true;
}

bool safe_filename(const std::string& filename) {
    if (filename.empty() || filename == "." || filename == "..") return false;
    for (unsigned char c : filename) {
        if (c < 0x20 || std::string("<>:\"/\\|?*").find(static_cast<char>(c)) != std::string::npos)
            return false;
    }
    return filename.back() != '.' && filename.back() != ' ';
}

bool require_real_directory(const fs::path& directory, const char* label, std::string* err) {
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(directory, ec);
    if (ec) {
        if (err) *err = std::string("could not inspect ") + label + ": " + ec.message();
        return false;
    }
    const DWORD attributes = GetFileAttributesW(directory.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        if (err) *err = std::string("could not inspect ") + label + " attributes";
        return false;
    }
    if (fs::is_symlink(status) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        !fs::is_directory(status)) {
        if (err) *err = std::string(label) + " must be a real directory";
        return false;
    }
    return true;
}

std::wstring lower_name(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });
    return value;
}

bool is_runtime_owned_entry(const std::wstring& name) {
    const std::wstring lower = lower_name(name);
    if (lower.rfind(L".amalgam-", 0) == 0) return true;
    static const std::set<std::wstring> protected_entries = {
        L"saves", L"screenshots", L"logs", L"crash-reports", L"natives", L"assets",
         L"libraries", L"versions", L"runtime", L"javas", L"runtimes", L"launcher_profiles.json"};
    return protected_entries.find(lower) != protected_entries.end();
}

bool is_launcher_metadata_entry(const std::wstring& name) {
    const std::wstring lower = lower_name(name);
    return lower == L"instance.json" || lower == L"amalgam-dependencies.json" ||
           lower == L"amalgam-local-content.json";
}

std::wstring normalized_relative(const fs::path& path) {
    return lower_name(path.lexically_normal().generic_wstring());
}

bool preview_path_allowed(const fs::path& relative) {
    if (relative.empty()) return false;
    auto it = relative.begin();
    if (it == relative.end()) return false;
    const std::wstring top = it->wstring();
    return !is_runtime_owned_entry(top) && !is_launcher_metadata_entry(top);
}

void add_preview_change(ArchivePreview& out, const fs::path& relative, ChangeKind kind) {
    switch (kind) {
        case ChangeKind::Added: ++out.added; break;
        case ChangeKind::Removed: ++out.removed; break;
        case ChangeKind::Replaced: ++out.replaced; break;
    }
    // Keep a useful UI/log sample without letting a very large pack inflate
    // memory usage. The counters always retain the complete result.
    constexpr size_t kMaxPreviewEntries = 160;
    if (out.changes.size() < kMaxPreviewEntries)
        out.changes.push_back({net::to_utf8(relative.generic_wstring()), kind});
}

bool collect_override_preview(const fs::path& overrides,
                              std::map<std::wstring, std::string>& desired,
                              std::string* err) {
    std::error_code ec;
    if (!fs::exists(overrides, ec)) {
        if (ec && err) *err = "cannot inspect creator overrides: " + ec.message();
        return !ec;
    }
    for (fs::recursive_directory_iterator it(overrides, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::file_status status = it->symlink_status(ec);
        if (ec) break;
        if (fs::is_symlink(status) || !fs::is_regular_file(status)) continue;
        const fs::path relative = it->path().lexically_relative(overrides);
        if (!safe_relative(net::to_utf8(relative.generic_wstring())) || !preview_path_allowed(relative)) continue;
        desired[normalized_relative(relative)] = net::sha1_file(it->path().wstring());
    }
    if (ec && err) *err = "cannot inspect creator overrides: " + ec.message();
    return !ec;
}

bool collect_existing_managed(const fs::path& target,
                              std::map<std::wstring, fs::path>& existing,
                              std::string* err) {
    static const std::wstring managed_roots[] = {
        L"mods", L"resourcepacks", L"shaderpacks", L"datapacks", L"config"};
    std::error_code ec;
    for (const std::wstring& root_name : managed_roots) {
        const fs::path root = target / root_name;
        const fs::file_status root_status = fs::symlink_status(root, ec);
        // Managed roots are optional.  In particular, a newly created profile
        // commonly has no resourcepacks, shaderpacks, datapacks, or config
        // folder yet.  Windows reports that normal absence through
        // symlink_status as ERROR_FILE_NOT_FOUND, which is an empty root, not
        // an unsafe one.  Keep every other inspection failure fatal.
        if (ec == std::errc::no_such_file_or_directory) {
            ec.clear();
            continue;
        }
        if (ec) break;
        if (!fs::exists(root_status)) continue;
        if (fs::is_symlink(root_status) || !fs::is_directory(root_status)) {
            if (err) *err = "managed profile content folder is redirected or invalid";
            return false;
        }
        for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
            const fs::file_status status = it->symlink_status(ec);
            if (ec) break;
            if (fs::is_symlink(status) || !fs::is_regular_file(status)) continue;
            const fs::path relative = it->path().lexically_relative(target);
            existing[normalized_relative(relative)] = it->path();
        }
        if (ec) break;
    }
    if (ec && err) *err = "cannot inspect managed profile content: " + ec.message();
    return !ec;
}

bool copy_overrides(const fs::path& root, const fs::path& target, std::string* err) {
    const fs::path overrides = root / "overrides";
    std::error_code ec;
    if (!fs::exists(overrides, ec)) return !ec;
    fs::create_directories(target, ec);
    if (ec) {
        if (err) *err = "create override destination failed: " + ec.message();
        return false;
    }
    for (fs::directory_iterator it(overrides, ec), end; !ec && it != end; it.increment(ec)) {
        const std::wstring name = it->path().filename().wstring();
        // An archive can provide game configuration, scripts, and pack assets,
        // but it must never overwrite launcher metadata or user-owned worlds,
        // captures, diagnostics, and runtime files.
        if (is_runtime_owned_entry(name) || is_launcher_metadata_entry(name)) continue;
        const fs::file_status status = it->symlink_status(ec);
        if (ec) break;
        if (fs::is_symlink(status)) {
            if (err) *err = "creator overrides may not contain redirected files or folders";
            return false;
        }
        fs::copy(it->path(), target / name,
                 fs::copy_options::recursive | fs::copy_options::overwrite_existing |
                     fs::copy_options::skip_symlinks,
                 ec);
    }
    if (ec && err) *err = "copy overrides failed: " + ec.message();
    return !ec;
}

struct CommitSlot {
    fs::path source;
    fs::path destination;
    fs::path backup;
    bool moved_previous = false;
    bool moved_replacement = false;
};

bool rollback_commit(std::vector<CommitSlot>& slots, std::string* err) {
    std::string problems;
    for (auto it = slots.rbegin(); it != slots.rend(); ++it) {
        std::error_code ec;
        if (it->moved_replacement) {
            fs::remove_all(it->destination, ec);
            if (ec) {
                if (!problems.empty()) problems += "; ";
                problems += "could not remove staged replacement " +
                            it->destination.filename().string() + ": " + ec.message();
                continue;
            }
        }
        if (it->moved_previous) {
            fs::rename(it->backup, it->destination, ec);
            if (ec) {
                if (!problems.empty()) problems += "; ";
                problems += "could not restore " + it->destination.filename().string() +
                            ": " + ec.message();
            }
        }
    }
    if (!problems.empty()) {
        if (err) *err = problems;
        return false;
    }
    return true;
}

// Commit a fully imported pack only after it has been built in a sibling
// staging directory.  Saved worlds, screenshots, logs, and runtime files are
// explicitly protected from creator-pack overrides.
bool commit_staged_profile(const fs::path& staged, const fs::path& target, std::string* err) {
    std::error_code ec;
    const fs::file_status metadata_status = fs::symlink_status(staged / L"instance.json", ec);
    if (ec || fs::is_symlink(metadata_status) || !fs::is_regular_file(metadata_status)) {
        if (err) *err = ec ? "cannot inspect staged profile: " + ec.message()
                           : "staged profile metadata is missing";
        return false;
    }

    std::vector<std::wstring> names = {
        L"mods", L"resourcepacks", L"shaderpacks", L"datapacks", L"config",
        L"amalgam-dependencies.json", L"amalgam-local-content.json", L"instance.json"};
    for (fs::directory_iterator it(staged, ec), end; !ec && it != end; it.increment(ec)) {
        const std::wstring name = it->path().filename().wstring();
        const fs::file_status status = it->symlink_status(ec);
        if (ec) break;
        if (fs::is_symlink(status)) {
            if (err) *err = "staged profile contains a redirected file or folder";
            return false;
        }
        if (is_runtime_owned_entry(name)) continue;
        const bool already_known = std::any_of(names.begin(), names.end(), [&](const std::wstring& known) {
            return _wcsicmp(known.c_str(), name.c_str()) == 0;
        });
        if (!already_known) names.push_back(name);
    }
    if (ec) {
        if (err) *err = "cannot inspect staged profile: " + ec.message();
        return false;
    }

    const fs::path rollback = target /
        (L".amalgam-pack-rollback-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()));
    fs::create_directories(rollback, ec);
    if (ec) {
        if (err) *err = "cannot create update rollback: " + ec.message();
        return false;
    }

    std::vector<CommitSlot> slots;
    for (const std::wstring& name : names) {
        CommitSlot slot;
        slot.source = staged / name;
        slot.destination = target / name;
        slot.backup = rollback / name;
        const bool has_replacement = fs::exists(slot.source, ec);
        if (ec) break;
        const bool has_previous = fs::exists(slot.destination, ec);
        if (ec || (!has_replacement && !has_previous)) continue;
        slots.push_back(std::move(slot));
        CommitSlot& current = slots.back();

        if (has_previous) {
            fs::rename(current.destination, current.backup, ec);
            if (ec) break;
            current.moved_previous = true;
        }
        if (has_replacement) {
            fs::rename(current.source, current.destination, ec);
            if (ec) break;
            current.moved_replacement = true;
        }
    }

    if (ec) {
        const std::string apply_error = ec.message();
        std::string rollback_error;
        const bool restored = rollback_commit(slots, &rollback_error);
        if (restored) {
            fs::remove_all(rollback, ec);
            if (err) *err = "could not apply staged pack: " + apply_error +
                            "; the original profile was restored";
        } else if (err) {
            *err = "could not apply staged pack: " + apply_error +
                   "; rollback also failed: " + rollback_error;
        }
        return false;
    }
    fs::remove_all(rollback, ec);
    if (ec) {
        // The profile has committed successfully. A stale rollback folder is
        // safe and recoverable, so do not turn a successful update into a
        // failure merely because cleanup was interrupted.
        ec.clear();
    }
    return true;
}

bool read_json(const fs::path& path, Json& out, std::string* err) {
    return json_parse_file(path.wstring(), out, err);
}

std::string loader_from_modrinth(const Json& deps) {
    const char* names[] = {"fabric-loader", "quilt-loader", "neoforge", "forge"};
    for (const char* name : names) if (!deps.get(name).as_str().empty()) {
        std::string loader = name;
        if (loader == "fabric-loader" || loader == "quilt-loader") loader.erase(loader.find("-loader"));
        return loader;
    }
    return "vanilla";
}

std::string loader_version_from_modrinth(const Json& deps, const std::string& loader) {
    if (loader == "fabric") return deps.get("fabric-loader").as_str();
    if (loader == "quilt") return deps.get("quilt-loader").as_str();
    if (loader == "neoforge") return deps.get("neoforge").as_str();
    if (loader == "forge") return deps.get("forge").as_str();
    return {};
}

bool download_to(const std::string& url, const fs::path& destination, const Progress& progress,
                 float start, float span, const std::string& sha1, int64_t size, std::string* err) {
    if (url.empty()) {
        if (err) *err = "file has no download URL";
        return false;
    }
    std::error_code ec;
    fs::create_directories(destination.parent_path(), ec);
    if (ec) {
        if (err) *err = "create destination failed: " + ec.message();
        return false;
    }
    if (net::verify_file(destination.wstring(), sha1, size)) {
        report(progress, start + span, destination.filename().string() + " (cached)");
        return true;
    }
    bool ok = net::download(net::to_wide(url), destination.wstring(),
                            [&](uint64_t done, uint64_t total) {
                                float part = total ? static_cast<float>(done) / static_cast<float>(total) : 0.0f;
                                 return report(progress, start + span * part, destination.filename().string());
                            }, err, sha1, size);
    return ok;
}

bool resolve_curseforge_download_url(const mods::ApiCfg& api, const std::string& project,
                                     const std::string& file_id, std::string& out,
                                     std::string* err) {
    const std::string url = "https://api.curseforge.com/v1/mods/" + project +
                            "/files/" + file_id + "/download-url";
    std::string response_text;
    if (!mods::curseforge_json(api, url, response_text, err)) return false;
    Json response = Json::parse(response_text, err);
    out = response.get("data").as_str();
    if (out.empty() && err && err->empty()) *err = "CurseForge returned no download URL";
    return !out.empty() && (!err || err->empty());
}

bool import_modrinth(const fs::path& root, const fs::path& manifest_path,
                      const std::wstring& instances_dir, instances::Instance& out,
                      const Progress& progress, std::string* err,
                      const instances::Instance* destination = nullptr) {
    Json manifest;
    if (!read_json(manifest_path, manifest, err)) return false;
    instances::Instance source;
    source.name = manifest.get("name").as_str(manifest_path.stem().string());
    source.pack_source = "modrinth";
    source.pack_project = manifest.get("projectId").as_str(manifest.get("project_id").as_str());
    source.pack_version = manifest.get("versionId").as_str();
    source.minecraft_version = manifest.get("dependencies").get("minecraft").as_str();
    source.loader = loader_from_modrinth(manifest.get("dependencies"));
    source.loader_version = loader_version_from_modrinth(manifest.get("dependencies"), source.loader);
    if (source.minecraft_version.empty()) {
        if (err) *err = "Modrinth manifest has no Minecraft version";
        return false;
    }
    if (destination) {
        if (!destination->minecraft_version.empty() && destination->minecraft_version != source.minecraft_version) {
            if (err) *err = "modpack Minecraft version does not match the selected profile";
            return false;
        }
        if (!destination->loader.empty() && destination->loader != "auto" && source.loader != "vanilla" &&
            destination->loader != source.loader) {
            if (err) *err = "modpack loader does not match the selected profile";
            return false;
        }
        if (!destination->loader_version.empty() && !source.loader_version.empty() &&
            destination->loader_version != source.loader_version) {
            if (err) *err = "modpack loader version does not match the selected profile";
            return false;
        }
        out = *destination;
        out.pack_source = source.pack_source;
        out.pack_project = source.pack_project;
        out.pack_version = source.pack_version;
        if (!source.loader_version.empty()) out.loader_version = source.loader_version;
        out.pack_modified = false;
        if (!instances::save(out, err)) return false;
    } else if (!instances::create(instances_dir, source, out, err)) {
        return false;
    }
    if (!copy_overrides(root, fs::path(out.directory), err)) return false;
    const auto& files = manifest.get("files").items();
    size_t index = 0;
    for (const auto& item : files) {
        std::string relative = item.get("path").as_str();
        if (!safe_relative(relative)) {
            if (err) *err = "manifest contains unsafe path";
            return false;
        }
        std::string url;
        const auto& downloads = item.get("downloads").items();
        if (!downloads.empty()) url = downloads.front().as_str();
        std::string environment = item.get("env").get("client").as_str("required");
        if (environment == "unsupported") {
            ++index;
            continue;
        }
        std::string sha1 = item.get("hashes").get("sha1").as_str();
        int64_t size = item.get("fileSize").as_int(-1);
        if (sha1.empty() || size < 0) {
            if (err) *err = "manifest file has no usable integrity metadata";
            return false;
        }
        float start = files.empty() ? 0.0f : static_cast<float>(index) / files.size();
        float span = files.empty() ? 1.0f : 1.0f / files.size();
        if (!report(progress, start, relative)) {
            if (err) *err = "import cancelled";
            return false;
        }
        if (!download_to(url, fs::path(out.directory) / fs::path(relative), progress, start, span,
                         sha1, size, err)) return false;
        ++index;
    }
    report(progress, 1.0f, "Files staged; preparing profile");
    return true;
}

bool import_curseforge(const fs::path& root, const fs::path& manifest_path,
                       const std::wstring& instances_dir, const mods::ApiCfg& api,
                       instances::Instance& out, const Progress& progress, std::string* err,
                       const instances::Instance* destination = nullptr) {
    Json manifest;
    if (!read_json(manifest_path, manifest, err)) return false;
    instances::Instance source;
    source.name = manifest.get("name").as_str(manifest_path.stem().string());
    source.pack_source = "curseforge";
    source.pack_project = std::to_string(manifest.get("projectID").as_int(0));
    source.pack_version = manifest.get("version").as_str();
    source.minecraft_version = manifest.get("minecraft").get("version").as_str();
    source.loader = "vanilla";
    const auto& loaders = manifest.get("minecraft").get("modLoaders").items();
    if (!loaders.empty()) {
        std::string id = loaders.front().get("id").as_str();
        size_t dash = id.find('-');
        source.loader = dash == std::string::npos ? id : id.substr(0, dash);
        source.loader_version = dash == std::string::npos ? "" : id.substr(dash + 1);
    }
    if (source.minecraft_version.empty()) {
        if (err) *err = "CurseForge manifest has no Minecraft version";
        return false;
    }
    if (destination) {
        if (!destination->minecraft_version.empty() && destination->minecraft_version != source.minecraft_version) {
            if (err) *err = "modpack Minecraft version does not match the selected profile";
            return false;
        }
        if (!destination->loader.empty() && destination->loader != "auto" && destination->loader != source.loader) {
            if (err) *err = "modpack loader does not match the selected profile";
            return false;
        }
        out = *destination;
        out.pack_source = source.pack_source;
        out.pack_project = source.pack_project;
        out.pack_version = source.pack_version;
        if (!source.loader_version.empty()) out.loader_version = source.loader_version;
        out.pack_modified = false;
        if (!instances::save(out, err)) return false;
    } else if (!instances::create(instances_dir, source, out, err)) {
        return false;
    }
    if (!copy_overrides(root, fs::path(out.directory), err)) return false;
    const auto& files = manifest.get("files").items();
    size_t index = 0;
    for (const auto& item : files) {
        std::string project = std::to_string(item.get("projectID").as_int());
        std::string file_id = std::to_string(item.get("fileID").as_int());
        std::string url = "https://api.curseforge.com/v1/mods/" + project + "/files/" + file_id;
        std::string request_error;
        std::string response_text;
        if (!mods::curseforge_json(api, url, response_text, &request_error)) {
            if (err) *err = "CurseForge file lookup failed: " + request_error;
            return false;
        }
        std::string parse_error;
        Json file = Json::parse(response_text, &parse_error);
        std::string download_url = file.get("data").get("downloadUrl").as_str();
        std::string filename = file.get("data").get("fileName").as_str("mod-" + file_id + ".jar");
        if (!safe_filename(filename)) {
            if (err) *err = "provider returned an unsafe content filename";
            return false;
        }
        std::string sha1;
        for (const auto& hash : file.get("data").get("hashes").items()) {
            if (hash.get("algo").as_int() == 1) sha1 = hash.get("value").as_str();
        }
        int64_t size = file.get("data").get("fileLength").as_int(-1);
        if (sha1.empty() || size < 0) {
            if (err) *err = "provider file has no usable integrity metadata";
            return false;
        }
        if (download_url.empty()) {
            std::string resolve_error;
            if (!resolve_curseforge_download_url(api, project, file_id, download_url, &resolve_error)) {
                if (err) *err = "CurseForge download URL lookup failed: " + resolve_error;
                return false;
            }
        }
        float start = files.empty() ? 0.0f : static_cast<float>(index) / files.size();
        float span = files.empty() ? 1.0f : 1.0f / files.size();
        if (!report(progress, start, filename)) {
            if (err) *err = "import cancelled";
            return false;
        }
        if (!download_to(download_url, fs::path(out.directory) / "mods" / filename, progress, start,
                         span, sha1, size, err)) return false;
        ++index;
    }
    report(progress, 1.0f, "Files staged; preparing profile");
    return true;
}

}  // namespace

bool archive(const std::wstring& path, const std::wstring& instances_dir, const mods::ApiCfg& api,
             instances::Instance& out, Progress progress, std::string* err) {
    out = {};
    const fs::path library_root(instances_dir);
    std::error_code ec;
    if (library_root.empty()) {
        if (err) *err = "profile library directory is missing";
        return false;
    }
    fs::create_directories(library_root, ec);
    if (ec || !require_real_directory(library_root, "profile library directory", err)) {
        if (ec && err) *err = "could not create profile library directory: " + ec.message();
        return false;
    }

    const std::wstring operation = std::to_wstring(GetCurrentProcessId()) + L"-" +
                                   std::to_wstring(GetTickCount64());
    const fs::path temp = library_root / (L".amalgam-import-archive-" + operation);
    const fs::path staging_root = library_root / (L".amalgam-import-stage-" + operation);
    if (fs::exists(temp, ec) || ec || fs::exists(staging_root, ec) || ec) {
        if (err) *err = ec ? "could not inspect import staging directories: " + ec.message()
                           : "an import staging directory is already present; retry the import";
        return false;
    }
    fs::create_directories(temp, ec);
    if (ec) {
        if (err) *err = "could not create import archive staging directory: " + ec.message();
        std::error_code cleanup_error;
        fs::remove_all(temp, cleanup_error);
        return false;
    }
    if (!require_real_directory(temp, "import archive staging directory", err) ||
        !extract::zip(path, temp.wstring(), err)) {
        std::error_code cleanup_error;
        fs::remove_all(temp, cleanup_error);
        return false;
    }
    fs::create_directories(staging_root, ec);
    if (ec) {
        std::error_code cleanup_error;
        fs::remove_all(temp, cleanup_error);
        fs::remove_all(staging_root, cleanup_error);
        if (err) *err = "could not create import profile staging directory: " + ec.message();
        return false;
    }
    if (!require_real_directory(staging_root, "import profile staging directory", err)) {
        std::error_code cleanup_error;
        fs::remove_all(temp, cleanup_error);
        fs::remove_all(staging_root, cleanup_error);
        return false;
    }

    bool ok = false;
    fs::path modrinth = temp / "modrinth.index.json";
    fs::path curseforge = temp / "manifest.json";
    if (fs::exists(modrinth)) {
        ok = import_modrinth(temp, modrinth, staging_root.wstring(), out, progress, err);
    } else if (fs::exists(curseforge)) {
        ok = import_curseforge(temp, curseforge, staging_root.wstring(), api, out, progress, err);
    }
    else if (err) *err = "archive is not a Modrinth or CurseForge modpack";

    if (ok && !report(progress, 0.99f, "Activating imported profile")) {
        ok = false;
        if (err) *err = "import cancelled before the new profile was activated";
    }
    if (ok) {
        const instances::Instance staged_profile = out;
        instances::ProfileIdentitySnapshot staged_identity;
        if (!instances::capture_profile_identity(staged_profile, staged_identity, err)) {
            ok = false;
        } else {
            fs::path target = library_root / net::to_wide(staged_profile.id);
            int suffix = 2;
            while (fs::exists(target, ec) && !ec) {
                target = library_root /
                    (net::to_wide(staged_profile.id) + L"-" + std::to_wstring(suffix++));
            }
            if (ec) {
                if (err) *err = "could not choose imported profile directory: " + ec.message();
                ok = false;
            } else if (!instances::profile_identity_matches(staged_profile, staged_identity, err)) {
                ok = false;
            } else {
                out.id = net::to_utf8(target.filename().wstring());
                out.directory = staged_profile.directory;
                if (!instances::save(out, err)) {
                    ok = false;
                } else {
                    fs::rename(staged_profile.directory, target, ec);
                    if (ec) {
                        if (err) *err = "could not activate imported profile: " + ec.message();
                        ok = false;
                    } else {
                        out.directory = target.wstring();
                    }
                }
            }
        }
    }
    if (ok) report(progress, 1.0f, "Import complete");
    std::error_code cleanup_error;
    fs::remove_all(temp, cleanup_error);
    fs::remove_all(staging_root, cleanup_error);
    if (!ok) {
        out = {};
    }
    return ok;
}

bool archive_into(const std::wstring& path, const instances::Instance& target,
                  const mods::ApiCfg& api, instances::Instance& out, Progress progress,
                  std::string* err) {
    out = {};
    instances::ProfileIdentitySnapshot target_identity;
    if (!instances::capture_profile_identity(target, target_identity, err)) return false;
    const fs::path target_root(target.directory);
    const fs::path staging = target_root /
        (L".amalgam-pack-stage-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()));
    const fs::path archive_root = staging / L"archive";
    const fs::path payload_root = staging / L"payload";
    std::error_code ec;
    if (fs::exists(staging, ec) || ec) {
        if (err) *err = ec ? "could not inspect pack staging directory: " + ec.message()
                           : "a pack staging directory is already present; retry the update";
        return false;
    }
    fs::create_directories(archive_root, ec);
    if (ec) {
        if (err) *err = "could not create pack staging directory: " + ec.message();
        return false;
    }
    if (!require_real_directory(staging, "pack staging directory", err) ||
        !require_real_directory(archive_root, "pack archive staging directory", err)) {
        std::error_code cleanup_error;
        fs::remove_all(staging, cleanup_error);
        return false;
    }
    fs::create_directories(payload_root, ec);
    if (ec) {
        if (err) *err = "could not create pack payload staging directory: " + ec.message();
        fs::remove_all(staging, ec);
        return false;
    }
    if (!extract::zip(path, archive_root.wstring(), err)) {
        fs::remove_all(staging, ec);
        return false;
    }
    if (!require_real_directory(payload_root, "pack payload staging directory", err)) {
        std::error_code cleanup_error;
        fs::remove_all(staging, cleanup_error);
        return false;
    }

    // Build the candidate profile away from the live instance. Provider
    // downloads, manifest validation, and override extraction must all finish
    // before a single live profile file is replaced.
    instances::Instance staged_target = target;
    staged_target.directory = payload_root.wstring();
    bool ok = false;
    fs::path modrinth = archive_root / "modrinth.index.json";
    fs::path curseforge = archive_root / "manifest.json";
    if (fs::exists(modrinth)) {
        ok = import_modrinth(archive_root, modrinth, L"", out, progress, err, &staged_target);
    } else if (fs::exists(curseforge)) {
        ok = import_curseforge(archive_root, curseforge, L"", api, out, progress, err, &staged_target);
    }
    else if (err) *err = "archive is not a Modrinth or CurseForge modpack";
    if (ok && !report(progress, 0.98f, "Reviewing staged profile before applying changes")) {
        ok = false;
        if (err) *err = "import cancelled before profile changes";
    }
    if (ok && !instances::profile_identity_matches(target, target_identity, err)) ok = false;
    if (ok) ok = commit_staged_profile(payload_root, target_root, err);
    if (ok) report(progress, 1.0f, "Import complete");
    fs::remove_all(staging, ec);
    if (!ok) {
        out = {};
        return false;
    }
    out.directory = target.directory;
    return true;
}

bool preview_archive(const std::wstring& path, const instances::Instance& target,
                     ArchivePreview& out, std::string* err) {
    out = {};
    instances::ProfileIdentitySnapshot target_identity;
    if (!instances::capture_profile_identity(target, target_identity, err)) return false;
    if (target.directory.empty()) {
        if (err) *err = "profile has no content directory";
        return false;
    }
    const fs::path target_root(target.directory);
    std::error_code ec;
    if (!fs::is_directory(target_root, ec) || ec) {
        if (err) *err = ec ? "cannot inspect profile: " + ec.message() : "profile directory is missing";
        return false;
    }
    const fs::path stage = target_root /
        (L".amalgam-pack-preview-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()));
    if (fs::exists(stage, ec) || ec) {
        if (err) *err = ec ? "cannot inspect archive preview staging directory: " + ec.message()
                           : "an archive preview is already in progress; retry shortly";
        return false;
    }
    fs::create_directories(stage, ec);
    if (ec || !extract::zip(path, stage.wstring(), err)) {
        fs::remove_all(stage, ec);
        if (ec && err && err->empty()) *err = "cannot prepare archive preview: " + ec.message();
        return false;
    }
    if (!require_real_directory(stage, "archive preview staging directory", err)) {
        std::error_code cleanup_error;
        fs::remove_all(stage, cleanup_error);
        return false;
    }

    std::map<std::wstring, std::string> desired;
    bool ok = collect_override_preview(stage / L"overrides", desired, err);
    const fs::path modrinth = stage / L"modrinth.index.json";
    const fs::path curseforge = stage / L"manifest.json";
    if (ok && fs::exists(modrinth, ec)) {
        Json manifest;
        ok = read_json(modrinth, manifest, err);
        if (ok) {
            for (const auto& item : manifest.get("files").items()) {
                const std::string raw_path = item.get("path").as_str();
                const std::string environment = item.get("env").get("client").as_str("required");
                const fs::path relative(net::to_wide(raw_path));
                if (environment == "unsupported") continue;
                if (!safe_relative(raw_path) || !preview_path_allowed(relative)) {
                    if (err) *err = "manifest contains unsafe preview path";
                    ok = false;
                    break;
                }
                desired[normalized_relative(relative)] = item.get("hashes").get("sha1").as_str();
            }
        }
    } else if (ok && fs::exists(curseforge, ec)) {
        // CurseForge manifests carry project/file ids rather than final names.
        // The installer resolves those names through the provider before staging.
        out.provider_file_names_resolved = false;
    } else if (ok) {
        if (err) *err = "archive is not a Modrinth or CurseForge modpack";
        ok = false;
    }

    std::map<std::wstring, fs::path> existing;
    if (ok) ok = collect_existing_managed(target_root, existing, err);
    if (ok) {
        for (const auto& wanted : desired) {
            const fs::path relative(wanted.first);
            const auto current = existing.find(wanted.first);
            if (current == existing.end()) {
                add_preview_change(out, relative, ChangeKind::Added);
                continue;
            }
            const std::string& expected_sha1 = wanted.second;
            if (!expected_sha1.empty() && net::sha1_file(current->second.wstring()) == expected_sha1) {
                ++out.unchanged;
            } else {
                add_preview_change(out, relative, ChangeKind::Replaced);
            }
            existing.erase(current);
        }
        for (const auto& leftover : existing)
            add_preview_change(out, fs::path(leftover.first), ChangeKind::Removed);
    }
    if (ok && !instances::profile_identity_matches(target, target_identity, err)) ok = false;
    fs::remove_all(stage, ec);
    if (ec && ok && err) *err = "archive preview succeeded but cleanup failed: " + ec.message();
    return ok;
}

bool export_mrpack(const instances::Instance& instance, const std::wstring& path, std::string* err) {
    fs::path target(path);
    fs::path staging = target.parent_path() / (".amalgam-mrpack-" + instance.id);
    std::error_code ec;
    fs::remove_all(staging, ec);
    fs::create_directories(staging / "overrides", ec);
    if (ec) {
        if (err) *err = "cannot create export staging directory: " + ec.message();
        return false;
    }
    Json manifest = Json::obj();
    manifest.set("formatVersion", Json::num(1));
    manifest.set("game", Json::str("minecraft"));
    manifest.set("versionId", Json::str("amalgam-" + instance.id));
    manifest.set("name", Json::str(instance.name.empty() ? instance.id : instance.name));
    manifest.set("summary", Json::str("Exported by Amalgam Launcher"));
    manifest.set("files", Json::arr());
    Json dependencies = Json::obj();
    dependencies.set("minecraft", Json::str(instance.minecraft_version));
    if (instance.loader == "fabric") dependencies.set("fabric-loader", Json::str(instance.loader_version));
    else if (instance.loader == "quilt") dependencies.set("quilt-loader", Json::str(instance.loader_version));
    else if (instance.loader == "neoforge") dependencies.set("neoforge", Json::str(instance.loader_version));
    else if (instance.loader == "forge") dependencies.set("forge", Json::str(instance.loader_version));
    manifest.set("dependencies", dependencies);
    std::string json_error;
    if (!json_write_file((staging / "modrinth.index.json").wstring(), manifest, &json_error)) {
        fs::remove_all(staging, ec);
        if (err) *err = json_error;
        return false;
    }

    const std::wstring roots[] = {L"mods", L"resourcepacks", L"shaderpacks", L"datapacks", L"config"};
    for (const auto& root : roots) {
        fs::path source = fs::path(instance.directory) / root;
        if (!fs::exists(source, ec)) continue;
        fs::copy(source, staging / "overrides" / root,
                 fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        if (ec) {
            fs::remove_all(staging, ec);
            if (err) *err = "copy export content failed: " + ec.message();
            return false;
        }
    }
    bool ok = extract::create_zip(staging.wstring(), path, err);
    fs::remove_all(staging, ec);
    return ok;
}

bool export_curseforge(const instances::Instance& instance, const std::wstring& path, std::string* err) {
    fs::path target(path);
    fs::path staging = target.parent_path() / (".amalgam-curseforge-" + instance.id);
    std::error_code ec;
    fs::remove_all(staging, ec);
    fs::create_directories(staging / "overrides", ec);
    if (ec) {
        if (err) *err = "cannot create CurseForge export staging directory: " + ec.message();
        return false;
    }
    Json manifest = Json::obj();
    manifest.set("manifestType", Json::str("minecraftModpack"));
    manifest.set("manifestVersion", Json::num(1));
    manifest.set("name", Json::str(instance.name.empty() ? instance.id : instance.name));
    manifest.set("version", Json::str("1.0"));
    manifest.set("author", Json::str("Amalgam"));
    manifest.set("files", Json::arr());
    Json minecraft = Json::obj();
    minecraft.set("version", Json::str(instance.minecraft_version));
    Json loaders = Json::arr();
    if (!instance.loader.empty() && instance.loader != "vanilla") {
        Json loader = Json::obj();
        loader.set("id", Json::str(instance.loader +
                                    (instance.loader_version.empty() ? "" : "-" + instance.loader_version)));
        loader.set("primary", Json::boolean(true));
        loaders.push(loader);
    }
    minecraft.set("modLoaders", loaders);
    manifest.set("minecraft", minecraft);
    manifest.set("overrides", Json::str("overrides"));
    std::string json_error;
    if (!json_write_file((staging / "manifest.json").wstring(), manifest, &json_error)) {
        fs::remove_all(staging, ec);
        if (err) *err = json_error;
        return false;
    }
    const std::wstring roots[] = {L"mods", L"resourcepacks", L"shaderpacks", L"datapacks", L"config"};
    for (const auto& root : roots) {
        fs::path source = fs::path(instance.directory) / root;
        if (!fs::exists(source, ec)) continue;
        fs::copy(source, staging / "overrides" / root,
                 fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        if (ec) {
            fs::remove_all(staging, ec);
            if (err) *err = "copy CurseForge export content failed: " + ec.message();
            return false;
        }
    }
    bool ok = extract::create_zip(staging.wstring(), path, err);
    fs::remove_all(staging, ec);
    return ok;
}

}  // namespace aml::import_pack
