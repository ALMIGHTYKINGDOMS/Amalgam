#include "instances.h"

#include "json.h"
#include "net.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cwctype>
#include <filesystem>
#include <ctime>
#include <fstream>
#include <map>

namespace aml::instances {

namespace {

bool require_real_directory(const std::filesystem::path& directory, const char* label,
                            std::string* err);

namespace fs = std::filesystem;

// MSVC's non-throwing symlink_status may report ERROR_FILE_NOT_FOUND or
// ERROR_PATH_NOT_FOUND for a path that is simply not created yet.  Callers
// that intentionally create the path after inspection must distinguish that
// expected absence from an unsafe or inaccessible path.
bool allow_missing_path_error(std::error_code& ec) {
    if (!ec) return true;
    if (ec.value() == ERROR_FILE_NOT_FOUND || ec.value() == ERROR_PATH_NOT_FOUND) {
        ec.clear();
        return true;
    }
    return false;
}

int64_t now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string safe_id(const std::string& value) {
    std::string id;
    for (unsigned char c : value) {
        if (std::isalnum(c)) id += static_cast<char>(std::tolower(c));
        else if (c == '-' || c == '_' || c == ' ') id += c == ' ' ? '-' : static_cast<char>(c);
    }
    while (id.find("--") != std::string::npos) id.replace(id.find("--"), 2, "-");
    if (id.empty()) id = "instance";
    return id;
}

bool safe_filename(const std::wstring& value) {
    if (value.empty() || value == L"." || value == L".." || value.back() == L'.' ||
        value.back() == L' ') return false;
    for (wchar_t c : value) {
        if (c < 0x20 || std::wstring(L"<>:\"/\\|?*").find(c) != std::wstring::npos)
            return false;
    }
    return true;
}

std::wstring metadata_path(const Instance& instance) {
    return instance.directory + L"\\instance.json";
}

bool from_json(const std::wstring& directory, Instance& out, std::string* err) {
    Json j;
    Instance metadata;
    metadata.directory = directory;
    if (!json_parse_file(metadata_path(metadata), j, err)) return false;
    out.directory = directory;
    out.id = j.get("id").as_str();
    out.name = j.get("name").as_str(out.id);
    out.minecraft_version = j.get("minecraft_version").as_str();
    out.loader = j.get("loader").as_str("auto");
    out.loader_version = j.get("loader_version").as_str();
    out.performance_profile = j.get("performance_profile").as_str("auto");
    out.java_path = j.get("java_path").as_str();
    out.icon_url = j.get("icon_url").as_str();
    out.pack_source = j.get("pack_source").as_str();
    out.pack_project = j.get("pack_project").as_str();
    out.pack_version = j.get("pack_version").as_str();
    out.pack_modified = j.get("pack_modified").as_bool(false);
    out.group = j.get("group").as_str();
    out.memory_mb = static_cast<int>(j.get("memory_mb").as_int(0));
    out.last_played = j.get("last_played").as_int(0);
    out.favorite = j.get("favorite").as_bool(false);
    out.is_ai_profile = j.get("is_ai_profile").as_bool(false);
    out.ai_mode = static_cast<int>(j.get("ai_mode").as_int(0));
    out.ai_live_vision = j.get("ai_live_vision").as_bool(false);
    return true;
}

}  // namespace

bool save(const Instance& instance, std::string* err) {
    Json j = Json::obj();
    j.set("format", Json::num(1));
    j.set("id", Json::str(instance.id));
    j.set("name", Json::str(instance.name));
    j.set("minecraft_version", Json::str(instance.minecraft_version));
    j.set("loader", Json::str(instance.loader));
    j.set("loader_version", Json::str(instance.loader_version));
    j.set("performance_profile", Json::str(instance.performance_profile));
    j.set("java_path", Json::str(instance.java_path));
    j.set("icon_url", Json::str(instance.icon_url));
    j.set("pack_source", Json::str(instance.pack_source));
    j.set("pack_project", Json::str(instance.pack_project));
    j.set("pack_version", Json::str(instance.pack_version));
    j.set("pack_modified", Json::boolean(instance.pack_modified));
    j.set("group", Json::str(instance.group));
    j.set("memory_mb", Json::num(instance.memory_mb));
    j.set("last_played", Json::num(static_cast<double>(instance.last_played)));
    j.set("favorite", Json::boolean(instance.favorite));
    j.set("is_ai_profile", Json::boolean(instance.is_ai_profile));
    j.set("ai_mode", Json::num(instance.ai_mode));
    j.set("ai_live_vision", Json::boolean(instance.ai_live_vision));
    return json_write_file(metadata_path(instance), j, err);
}

bool load(const std::wstring& directory, Instance& out, std::string* err) {
    return from_json(directory, out, err);
}

bool create(const std::wstring& instances_dir, const Instance& source, Instance& out,
            std::string* err) {
    out = source;
    out.name = source.name.empty() ? "New Instance" : source.name;
    out.id = safe_id(source.id.empty() ? out.name : source.id);
    fs::path root(instances_dir);
    fs::path target = root / out.id;
    int suffix = 2;
    while (fs::exists(target)) target = root / (out.id + "-" + std::to_string(suffix++));
    out.id = target.filename().string();
    out.directory = target.wstring();
    out.last_played = 0;
    if (!net::mkdirs(out.directory + L"\\mods") || !net::mkdirs(out.directory + L"\\screenshots")) {
        if (err) *err = "could not create instance directories";
        return false;
    }
    return save(out, err);
}

bool duplicate(const Instance& source, const std::wstring& instances_dir, const std::string& new_name,
               Instance& out, std::string* err) {
    if (err) err->clear();
    ProfileIdentitySnapshot source_identity;
    if (!capture_profile_identity(source, source_identity, err)) return false;

    fs::path root(instances_dir);
    if (root.empty()) {
        if (err) *err = "profile library directory is missing";
        return false;
    }
    std::error_code ec;
    fs::create_directories(root, ec);
    if (ec) {
        if (err) *err = "could not create profile library directory: " + ec.message();
        return false;
    }
    const fs::file_status root_status = fs::symlink_status(root, ec);
    if (ec || fs::is_symlink(root_status) || !fs::is_directory(root_status)) {
        if (err) *err = ec ? "could not inspect profile library directory: " + ec.message()
                           : "profile library directory must not be redirected";
        return false;
    }

    std::string base = safe_id(new_name.empty() ? source.name : new_name);
    fs::path target = root / base;
    int suffix = 2;
    while (fs::exists(target, ec) && !ec)
        target = root / (base + "-" + std::to_string(suffix++));
    if (ec) {
        if (err) *err = "could not choose duplicate profile directory: " + ec.message();
        return false;
    }

    // Copy into a sibling staging directory first. A failed copy must never
    // leave a partially indexed profile in the active library.
    const std::wstring staging_base =
        L".amalgam-duplicate-stage-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(GetTickCount64());
    fs::path staging = root / staging_base;
    for (int stage_suffix = 2; fs::exists(staging, ec) && !ec; ++stage_suffix)
        staging = root / (staging_base + L"-" + std::to_wstring(stage_suffix));
    if (ec) {
        if (err) *err = "could not prepare duplicate staging directory: " + ec.message();
        return false;
    }

    fs::copy(fs::path(source.directory), staging,
             fs::copy_options::recursive | fs::copy_options::skip_symlinks, ec);
    if (ec) {
        std::error_code cleanup_error;
        fs::remove_all(staging, cleanup_error);
        if (err) *err = "duplicate failed: " + ec.message();
        return false;
    }

    Instance duplicate = source;
    duplicate.id = target.filename().string();
    duplicate.name = new_name.empty() ? source.name + " Copy" : new_name;
    duplicate.directory = staging.wstring();
    duplicate.last_played = 0;
    duplicate.favorite = false;
    if (!save(duplicate, err)) {
        std::error_code cleanup_error;
        fs::remove_all(staging, cleanup_error);
        return false;
    }
    if (!profile_identity_matches(source, source_identity, err)) {
        std::error_code cleanup_error;
        fs::remove_all(staging, cleanup_error);
        return false;
    }
    fs::rename(staging, target, ec);
    if (ec) {
        std::error_code cleanup_error;
        fs::remove_all(staging, cleanup_error);
        if (err) *err = "could not activate duplicated profile: " + ec.message();
        return false;
    }
    duplicate.directory = target.wstring();
    out = std::move(duplicate);
    return true;
}

std::vector<Instance> scan(const std::wstring& instances_dir, std::string* err) {
    std::vector<Instance> result;
    std::error_code ec;
    fs::path root(instances_dir);
    if (!fs::exists(root, ec)) {
        if (ec && err) *err = "Unable to inspect profiles: " + ec.message();
        return result;
    }
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (ec || !entry.is_directory()) continue;
        Instance instance;
        std::string local_err;
        if (load(entry.path().wstring(), instance, &local_err)) {
            result.push_back(std::move(instance));
        }
    }
    if (ec && err) *err = "Unable to finish scanning profiles: " + ec.message();
    std::sort(result.begin(), result.end(), [](const Instance& a, const Instance& b) {
        if (a.favorite != b.favorite) return a.favorite > b.favorite;
        return a.last_played > b.last_played;
    });
    return result;
}

namespace {

bool disabled_suffix(const std::wstring& name) {
    return name.size() > 9 && name.compare(name.size() - 9, 9, L".disabled") == 0;
}

const wchar_t* content_root_name(ContentType type) {
    switch (type) {
        case ContentType::Mod: return L"mods";
        case ContentType::ResourcePack: return L"resourcepacks";
        case ContentType::Shader: return L"shaderpacks";
        case ContentType::DataPack: return L"datapacks";
        case ContentType::Config: return L"config";
        default: return nullptr;
    }
}

bool is_direct_child(const fs::path& path, const fs::path& directory, std::string* err,
                     const char* label) {
    std::error_code ec;
    if (!fs::equivalent(path.parent_path(), directory, ec) || ec) {
        if (err) *err = ec ? ec.message() : std::string(label) + " is outside the selected profile";
        return false;
    }
    return true;
}

fs::path unique_recovery_destination(const fs::path& directory, const std::wstring& name,
                                     std::error_code& ec) {
    const std::wstring prefix = std::to_wstring(now_seconds()) + L"-";
    fs::path destination = directory / (prefix + name);
    int suffix = 2;
    while (fs::exists(destination, ec)) {
        if (!allow_missing_path_error(ec)) return {};
        destination = directory / (prefix + std::to_wstring(suffix++) + L"-" + name);
    }
    if (!allow_missing_path_error(ec)) return {};
    return destination;
}

struct MetadataMutation {
    fs::path path;
    Json original;
    Json updated;
};

bool prepare_metadata_prune(const fs::path& path, const std::string& filename,
                            std::vector<MetadataMutation>& out, std::string* err) {
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        if (!allow_missing_path_error(ec)) {
            if (err) *err = ec.message();
            return false;
        }
        return true;
    }
    Json original;
    std::string parse_error;
    if (!json_parse_file(path.wstring(), original, &parse_error)) {
        if (err) *err = "could not read " + path.filename().string() + ": " + parse_error;
        return false;
    }
    const Json entries = original.get("entries");
    if (!entries.is(Json::Type::Arr)) return true;

    Json updated_entries = Json::arr();
    bool changed = false;
    for (const auto& item : entries.items()) {
        if (item.get("file").as_str() == filename) {
            changed = true;
            continue;
        }
        updated_entries.push(item);
    }
    if (!changed) return true;
    Json updated = original;
    updated.set("entries", updated_entries);
    out.push_back({path, std::move(original), std::move(updated)});
    return true;
}

bool validate_world_source(const Instance& instance, const std::wstring& source, fs::path& world,
                           std::string* err) {
    world = fs::path(source);
    std::error_code ec;
    if (!require_real_directory(fs::path(instance.directory), "selected profile directory", err)) return false;
    const fs::path saves = fs::path(instance.directory) / L"saves";
    if (!require_real_directory(saves, "profile worlds directory", err)) return false;
    const fs::file_status world_status = fs::symlink_status(world, ec);
    if (ec || fs::is_symlink(world_status) || !fs::is_directory(world_status)) {
        if (err) *err = ec ? ec.message() : "world directory does not exist";
        return false;
    }
    if (!is_direct_child(world, saves, err, "world")) return false;
    const std::wstring name = world.filename().wstring();
    if (!safe_filename(name) || name.rfind(L".amalgam-", 0) == 0) {
        if (err) *err = "world directory name is unsafe";
        return false;
    }
    return true;
}

bool require_real_directory(const fs::path& directory, const char* label, std::string* err) {
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(directory, ec);
    if (ec) {
        if (err) *err = std::string("could not inspect ") + label + ": " + ec.message();
        return false;
    }
    if (!fs::exists(status)) {
        if (err) *err = std::string(label) + " does not exist";
        return false;
    }
    const DWORD attributes = GetFileAttributesW(directory.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        if (err) *err = std::string("could not inspect ") + label + " attributes";
        return false;
    }
    if (fs::is_symlink(status) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        !fs::is_directory(status)) {
        if (err) *err = std::string(label) + " must be a real profile directory";
        return false;
    }
    return true;
}

bool normalized_profile_path(const fs::path& path, std::wstring& out, std::string* err) {
    std::error_code ec;
    const fs::path absolute = fs::absolute(path, ec);
    if (ec) {
        if (err) *err = "could not resolve selected profile path: " + ec.message();
        return false;
    }
    out = absolute.lexically_normal().wstring();
    return !out.empty();
}

bool same_profile_path(const fs::path& left, const fs::path& right, std::string* err) {
    std::wstring normalized_left;
    std::wstring normalized_right;
    if (!normalized_profile_path(left, normalized_left, err) ||
        !normalized_profile_path(right, normalized_right, err)) {
        return false;
    }
    return _wcsicmp(normalized_left.c_str(), normalized_right.c_str()) == 0;
}

bool same_persisted_profile_metadata(const Instance& left, const Instance& right) {
    return left.id == right.id &&
           left.name == right.name &&
           left.minecraft_version == right.minecraft_version &&
           left.loader == right.loader &&
           left.loader_version == right.loader_version &&
           left.performance_profile == right.performance_profile &&
           left.java_path == right.java_path &&
           left.icon_url == right.icon_url &&
           left.pack_source == right.pack_source &&
           left.pack_project == right.pack_project &&
           left.pack_version == right.pack_version &&
           left.pack_modified == right.pack_modified &&
           left.group == right.group &&
           left.memory_mb == right.memory_mb &&
           left.last_played == right.last_played &&
           left.favorite == right.favorite &&
           left.is_ai_profile == right.is_ai_profile &&
           left.ai_mode == right.ai_mode &&
           left.ai_live_vision == right.ai_live_vision;
}

bool validate_profile_identity(const Instance& instance, std::string* err) {
    if (instance.directory.empty() || instance.id.empty()) {
        if (err) *err = "the selected profile is missing its identity; refresh the Library and try again";
        return false;
    }

    const fs::path directory(instance.directory);
    if (!require_real_directory(directory, "selected profile directory", err)) return false;

    const std::wstring expected_name = net::to_wide(instance.id);
    if (!safe_filename(expected_name) ||
        _wcsicmp(directory.filename().wstring().c_str(), expected_name.c_str()) != 0) {
        if (err) *err = "the selected profile no longer matches its managed directory; refresh the Library and try again";
        return false;
    }

    const fs::path metadata = directory / L"instance.json";
    std::error_code ec;
    const fs::file_status metadata_status = fs::symlink_status(metadata, ec);
    if (ec) {
        if (err) *err = "could not inspect selected profile metadata: " + ec.message();
        return false;
    }
    if (fs::is_symlink(metadata_status) || !fs::is_regular_file(metadata_status)) {
        if (err) *err = "selected profile metadata must be a regular file inside the profile";
        return false;
    }

    Instance persisted;
    std::string load_error;
    if (!from_json(directory.wstring(), persisted, &load_error)) {
        if (err) *err = "could not verify selected profile metadata: " + load_error;
        return false;
    }
    if (persisted.id != instance.id) {
        if (err) *err = "the selected profile changed or was moved; refresh the Library and try again";
        return false;
    }
    if (!same_persisted_profile_metadata(instance, persisted)) {
        if (err) *err = "the selected profile changed since it was loaded; refresh the Library and try again";
        return false;
    }
    return true;
}

bool validate_managed_content_source(const Instance& instance, const ContentEntry& entry,
                                     fs::path& source, fs::path& content_root,
                                     std::string* err) {
    const wchar_t* root_name = content_root_name(entry.type);
    if (!root_name) {
        if (err) *err = "this content type is not managed by the selected profile";
        return false;
    }
    if (instance.directory.empty() || entry.path.empty()) {
        if (err) *err = "the selected content file is no longer available";
        return false;
    }

    source = fs::path(entry.path);
    std::error_code ec;
    const fs::file_status source_status = fs::symlink_status(source, ec);
    if (ec) {
        if (err) *err = "could not inspect content file: " + ec.message();
        return false;
    }
    const DWORD source_attributes = GetFileAttributesW(source.c_str());
    if (source_attributes == INVALID_FILE_ATTRIBUTES) {
        if (err) *err = "could not inspect content file attributes";
        return false;
    }
    if (fs::is_symlink(source_status) || (source_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        !fs::is_regular_file(source_status)) {
        if (err) *err = "content file must be a regular file inside this profile";
        return false;
    }

    content_root = fs::path(instance.directory) / root_name;
    if (!require_real_directory(content_root, "profile content directory", err)) return false;
    if (!is_direct_child(source, content_root, err, "content file")) return false;

    const std::wstring source_name = source.filename().wstring();
    if (!safe_filename(source_name)) {
        if (err) *err = "content filename is unsafe";
        return false;
    }
    return true;
}

bool prepare_recovery_directory(const Instance& instance, const wchar_t* root_name,
                                fs::path& trash, std::string* err) {
    const fs::path profile(instance.directory);
    const fs::path trash_root = profile / L".amalgam-trash";
    const fs::path content_root = trash_root / L"content";
    trash = content_root / root_name;

    // Do not follow a redirected recovery directory.  A profile itself may be
    // stored on a supported redirected drive, but recovery must remain a
    // normal child of that selected profile rather than an arbitrary target.
    for (const fs::path& component : {trash_root, content_root, trash}) {
        std::error_code ec;
        const fs::file_status status = fs::symlink_status(component, ec);
        if (!allow_missing_path_error(ec)) {
            if (err) *err = "could not inspect content recovery folder: " + ec.message();
            return false;
        }
        if (fs::exists(status) && (fs::is_symlink(status) || !fs::is_directory(status))) {
            if (err) *err = "content recovery folder must remain inside the selected profile";
            return false;
        }
    }

    std::error_code ec;
    fs::create_directories(trash, ec);
    if (ec) {
        if (err) *err = "cannot create content recovery folder: " + ec.message();
        return false;
    }
    for (const fs::path& component : {trash_root, content_root, trash}) {
        if (!require_real_directory(component, "content recovery folder", err)) return false;
    }
    return true;
}

bool prepare_profile_local_directory(const Instance& instance, const fs::path& relative,
                                     const char* label, fs::path& out, std::string* err) {
    const fs::path profile(instance.directory);
    if (!require_real_directory(profile, "selected profile directory", err)) return false;
    if (relative.empty() || relative.is_absolute() || !relative.root_name().empty() ||
        !relative.root_directory().empty()) {
        if (err) *err = std::string(label) + " path is invalid";
        return false;
    }

    std::vector<fs::path> directories;
    fs::path current = profile;
    for (const auto& part : relative) {
        if (part.empty() || part == L"." || part == L"..") {
            if (err) *err = std::string(label) + " path is invalid";
            return false;
        }
        current /= part;
        directories.push_back(current);
        std::error_code ec;
        const fs::file_status status = fs::symlink_status(current, ec);
        if (!allow_missing_path_error(ec)) {
            if (err) *err = "could not inspect " + std::string(label) + ": " + ec.message();
            return false;
        }
        if (fs::exists(status) && (fs::is_symlink(status) || !fs::is_directory(status))) {
            if (err) *err = std::string(label) + " must remain inside the selected profile";
            return false;
        }
    }

    std::error_code ec;
    fs::create_directories(current, ec);
    if (ec) {
        if (err) *err = "cannot create " + std::string(label) + ": " + ec.message();
        return false;
    }
    for (const fs::path& directory : directories) {
        if (!require_real_directory(directory, label, err)) return false;
    }
    out = current;
    return true;
}

}  // namespace

bool capture_profile_identity(const Instance& instance, ProfileIdentitySnapshot& out,
                              std::string* err) {
    out = {};
    if (err) err->clear();
    if (!validate_profile_identity(instance, err)) return false;

    const fs::path metadata = fs::path(instance.directory) / L"instance.json";
    std::error_code ec;
    const uintmax_t size = fs::file_size(metadata, ec);
    if (ec) {
        if (err) *err = "could not inspect selected profile metadata size: " + ec.message();
        return false;
    }
    const fs::file_time_type modified = fs::last_write_time(metadata, ec);
    if (ec) {
        if (err) *err = "could not inspect selected profile metadata timestamp: " + ec.message();
        return false;
    }
    if (!normalized_profile_path(fs::path(instance.directory), out.directory, err)) return false;
    out.id = instance.id;
    out.metadata_size = static_cast<uint64_t>(size);
    out.metadata_last_write_time = modified;
    return true;
}

bool profile_identity_matches(const Instance& instance, const ProfileIdentitySnapshot& snapshot,
                              std::string* err) {
    if (err) err->clear();
    if (snapshot.directory.empty() || snapshot.id.empty() || snapshot.id != instance.id) {
        if (err) *err = "the selected profile changed or was moved; refresh the Library and try again";
        return false;
    }
    std::wstring current_directory;
    if (!normalized_profile_path(fs::path(instance.directory), current_directory, err)) return false;
    if (_wcsicmp(snapshot.directory.c_str(), current_directory.c_str()) != 0) {
        if (err) *err = "the selected profile changed or was moved; refresh the Library and try again";
        return false;
    }

    Instance persisted;
    std::string current_error;
    if (!from_json(instance.directory, persisted, &current_error)) {
        if (err) *err = current_error.empty()
            ? "the selected profile is no longer available; refresh the Library and try again"
            : current_error;
        return false;
    }
    if (persisted.id != snapshot.id) {
        if (err) *err = "the selected profile changed or was moved; refresh the Library and try again";
        return false;
    }
    ProfileIdentitySnapshot current;
    if (!capture_profile_identity(persisted, current, &current_error)) {
        if (err) *err = current_error.empty()
            ? "the selected profile is no longer available; refresh the Library and try again"
            : current_error;
        return false;
    }
    if (current.metadata_size != snapshot.metadata_size ||
        current.metadata_last_write_time != snapshot.metadata_last_write_time) {
        if (err) *err = "the selected profile changed while this action was open; refresh the Library and try again";
        return false;
    }
    return true;
}

std::vector<ContentEntry> list_content(const Instance& instance, std::string* err) {
    std::vector<ContentEntry> result;
    std::error_code ec;
    const std::pair<const wchar_t*, ContentType> roots[] = {
        {L"mods", ContentType::Mod}, {L"resourcepacks", ContentType::ResourcePack},
        {L"shaderpacks", ContentType::Shader}, {L"datapacks", ContentType::DataPack},
        {L"config", ContentType::Config}};
    for (const auto& root : roots) {
        fs::path directory = fs::path(instance.directory) / root.first;
        if (!fs::exists(directory, ec)) continue;
        for (const auto& file : fs::directory_iterator(directory, ec)) {
            if (ec || !file.is_regular_file()) continue;
            ContentEntry entry;
            entry.path = file.path().wstring();
            entry.filename = file.path().filename().string();
            entry.type = root.second;
            entry.enabled = !disabled_suffix(file.path().filename().wstring());
            entry.size = static_cast<uint64_t>(file.file_size(ec));
            result.push_back(std::move(entry));
        }
    }
    if (ec && err) *err = ec.message();

    // Enrich the filesystem view with provider ownership when the profile was
    // created or updated by the managed installer. Older/manual files remain
    // visible and explicitly report as unmanaged.
    Json ownership;
    if (json_parse_file(instance.directory + L"\\amalgam-dependencies.json", ownership, nullptr)) {
        std::map<std::string, const Json*> records;
        for (const auto& item : ownership.get("entries").items()) {
            const std::string file = item.get("file").as_str();
            if (!file.empty()) records[file] = &item;
        }
        for (auto& entry : result) {
            std::string key = entry.filename;
            if (key.size() > 9 && key.compare(key.size() - 9, 9, ".disabled") == 0)
                key.resize(key.size() - 9);
            auto found = records.find(key);
            if (found == records.end()) continue;
            entry.managed = true;
            entry.owner_project = found->second->get("project").as_str();
            entry.owner_source = found->second->get("source").as_str();
            entry.owner_version = found->second->get("version_id").as_str();
            entry.sha1 = found->second->get("sha1").as_str();
        }
    }
    std::sort(result.begin(), result.end(), [](const ContentEntry& a, const ContentEntry& b) {
        return a.filename < b.filename;
    });
    return result;
}

bool set_content_enabled(const ContentEntry& entry, bool enabled, std::string* err) {
    fs::path source(entry.path);
    std::error_code ec;
    const fs::file_status source_status = fs::symlink_status(source, ec);
    const DWORD source_attributes = GetFileAttributesW(source.c_str());
    if (ec || source_attributes == INVALID_FILE_ATTRIBUTES ||
        fs::is_symlink(source_status) || (source_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        !fs::is_regular_file(source_status)) {
        if (err) *err = "content file does not exist";
        return false;
    }
    std::wstring name = source.filename().wstring();
    bool currently_enabled = !disabled_suffix(name);
    if (currently_enabled == enabled) return true;
    fs::path target = enabled ? source.parent_path() / name.substr(0, name.size() - 9)
                              : source.parent_path() / (name + L".disabled");
    if (fs::exists(target, ec) || ec) {
        if (err) *err = ec ? ec.message() : "a content file already exists in the requested state";
        return false;
    }
    fs::rename(source, target, ec);
    if (ec && err) *err = ec.message();
    return !ec;
}

bool set_content_enabled(const Instance& instance, const ContentEntry& entry, bool enabled,
                         std::string* err) {
    fs::path source;
    fs::path content_root;
    if (!validate_managed_content_source(instance, entry, source, content_root, err)) return false;

    const std::wstring name = source.filename().wstring();
    const bool currently_enabled = !disabled_suffix(name);
    if (currently_enabled == enabled) return true;

    const fs::path target = enabled ? source.parent_path() / name.substr(0, name.size() - 9)
                                    : source.parent_path() / (name + L".disabled");
    if (!is_direct_child(target, content_root, err, "content target")) return false;

    std::error_code ec;
    const fs::file_status target_status = fs::symlink_status(target, ec);
    if (!allow_missing_path_error(ec)) {
        if (err) *err = "could not inspect content target: " + ec.message();
        return false;
    }
    if (fs::exists(target_status)) {
        if (err) *err = "a content file already exists in the requested state";
        return false;
    }
    fs::rename(source, target, ec);
    if (ec && err) *err = "could not change content state: " + ec.message();
    return !ec;
}

bool capture_content_file_snapshot(const Instance& instance, const ContentEntry& entry,
                                   ContentFileSnapshot& out, std::string* err) {
    out = {};
    fs::path source;
    fs::path content_root;
    if (!validate_managed_content_source(instance, entry, source, content_root, err)) return false;

    std::error_code ec;
    const uintmax_t size = fs::file_size(source, ec);
    if (ec) {
        if (err) *err = "could not inspect content file size: " + ec.message();
        return false;
    }
    const fs::file_time_type modified = fs::last_write_time(source, ec);
    if (ec) {
        if (err) *err = "could not inspect content file timestamp: " + ec.message();
        return false;
    }
    out.path = source.wstring();
    out.type = entry.type;
    out.size = static_cast<uint64_t>(size);
    out.last_write_time = modified;
    return true;
}

bool content_file_matches_snapshot(const Instance& instance, const ContentEntry& entry,
                                   const ContentFileSnapshot& snapshot,
                                   std::string* err) {
    if (snapshot.path.empty() || snapshot.type != entry.type || snapshot.path != entry.path) {
        if (err) *err = "the selected content target changed; refresh the list and choose it again";
        return false;
    }
    ContentFileSnapshot current;
    if (!capture_content_file_snapshot(instance, entry, current, err)) return false;
    if (current.size != snapshot.size || current.last_write_time != snapshot.last_write_time) {
        if (err) *err = "the selected content file changed; refresh the list and choose it again";
        return false;
    }
    return true;
}

bool remove_content(const Instance& instance, const ContentEntry& entry, std::string* err) {
    return move_content_to_trash(instance, entry, nullptr, err);
}

bool import_content(const Instance& instance, const std::wstring& source, ContentType type,
                    ContentEntry* out, std::string* err) {
    if (!net::file_exists(source)) {
        if (err) *err = "content file does not exist";
        return false;
    }
    fs::path source_path(source);
    std::wstring filename = source_path.filename().wstring();
    if (!safe_filename(filename)) {
        if (err) *err = "content filename is unsafe";
        return false;
    }
    std::wstring extension = source_path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });
    const bool jar = extension == L".jar";
    const bool zip = extension == L".zip";
    if (extension == L".mcpack" || extension == L".mcaddon") {
        if (err) *err = "Bedrock addons must be imported from the Bedrock tab";
        return false;
    }
    if ((type == ContentType::Mod && !jar) ||
        (type != ContentType::Mod && type != ContentType::Other && !zip)) {
        if (err) *err = type == ContentType::Mod ? "Java mods must be .jar files"
                                                 : "this content type requires a .zip archive";
        return false;
    }
    if (net::file_size(source) == 0 || net::file_size(source) > 512ull * 1024 * 1024) {
        if (err) *err = "content file is empty or exceeds the 512 MiB limit";
        return false;
    }
    std::ifstream archive(source, std::ios::binary);
    unsigned char signature[2]{};
    archive.read(reinterpret_cast<char*>(signature), sizeof(signature));
    if (!archive || signature[0] != 'P' || signature[1] != 'K') {
        if (err) *err = "content file is not a ZIP/JAR archive";
        return false;
    }

    const wchar_t* root_name = L"mods";
    if (type == ContentType::ResourcePack) root_name = L"resourcepacks";
    else if (type == ContentType::Shader) root_name = L"shaderpacks";
    else if (type == ContentType::DataPack) root_name = L"datapacks";
    else if (type == ContentType::Config) root_name = L"config";
    fs::path root = fs::path(instance.directory) / root_name;
    std::error_code ec;
    fs::create_directories(root, ec);
    if (ec) {
        if (err) *err = "could not create content directory: " + ec.message();
        return false;
    }
    fs::path destination = root / filename;
    int suffix = 2;
    while (fs::exists(destination, ec)) {
        std::wstring stem = destination.stem().wstring();
        std::wstring ext = destination.extension().wstring();
        destination = root / (stem + L"-" + std::to_wstring(suffix++) + ext);
    }
    if (!CopyFileW(source.c_str(), destination.wstring().c_str(), TRUE)) {
        if (err) *err = "could not copy content (winerr " +
                        std::to_string(static_cast<long>(GetLastError())) + ")";
        return false;
    }

    Json metadata = Json::obj();
    fs::path metadata_path = fs::path(instance.directory) / L"amalgam-local-content.json";
    json_parse_file(metadata_path.wstring(), metadata, nullptr);
    if (!metadata.is(Json::Type::Obj)) metadata = Json::obj();
    Json entries = metadata.get("entries");
    if (!entries.is(Json::Type::Arr)) entries = Json::arr();
    Json record = Json::obj();
    record.set("file", Json::str(net::to_utf8(destination.filename().wstring())));
    record.set("original_file", Json::str(net::to_utf8(filename)));
    record.set("type", Json::str(content_type_name(type)));
    record.set("sha1", Json::str(net::sha1_file(destination.wstring())));
    record.set("size", Json::num(static_cast<double>(net::file_size(destination.wstring()))));
    record.set("imported_at", Json::num(static_cast<double>(std::time(nullptr))));
    entries.push(record);
    metadata.set("format", Json::num(1));
    metadata.set("entries", entries);
    std::string metadata_error;
    if (!json_write_file(metadata_path.wstring(), metadata, &metadata_error)) {
        fs::remove(destination, ec);
        if (err) *err = metadata_error.empty() ? "could not write local content metadata" : metadata_error;
        return false;
    }
    if (out) {
        out->path = destination.wstring();
        out->filename = net::to_utf8(destination.filename().wstring());
        out->type = type;
        out->enabled = true;
        out->size = net::file_size(destination.wstring());
    }
    return true;
}

bool move_content_to_trash(const Instance& instance, const ContentEntry& entry,
                           std::wstring* out_path, std::string* err) {
    const wchar_t* root_name = content_root_name(entry.type);
    if (!root_name) {
        if (err) *err = "this content type cannot be moved to profile recovery";
        return false;
    }
    fs::path source;
    fs::path content_root;
    if (!validate_managed_content_source(instance, entry, source, content_root, err)) return false;
    const std::wstring source_name = source.filename().wstring();
    std::wstring metadata_name = source_name;
    if (disabled_suffix(metadata_name)) metadata_name.resize(metadata_name.size() - 9);
    const std::string metadata_filename = net::to_utf8(metadata_name);

    std::vector<MetadataMutation> metadata;
    if (!prepare_metadata_prune(fs::path(instance.directory) / L"amalgam-dependencies.json",
                                metadata_filename, metadata, err) ||
        !prepare_metadata_prune(fs::path(instance.directory) / L"amalgam-local-content.json",
                                metadata_filename, metadata, err)) {
        return false;
    }

    fs::path trash;
    if (!prepare_recovery_directory(instance, root_name, trash, err)) return false;
    std::error_code ec;
    const fs::path destination = unique_recovery_destination(trash, source_name, ec);
    if (ec) {
        if (err) *err = "cannot prepare content recovery path: " + ec.message();
        return false;
    }
    fs::rename(source, destination, ec);
    if (ec) {
        if (err) *err = "could not move content to recovery: " + ec.message();
        return false;
    }

    size_t written = 0;
    for (const auto& mutation : metadata) {
        std::string write_error;
        if (!json_write_file(mutation.path.wstring(), mutation.updated, &write_error)) {
            for (size_t index = 0; index < written; ++index)
                json_write_file(metadata[index].path.wstring(), metadata[index].original, nullptr);
            std::error_code move_back_error;
            fs::rename(destination, source, move_back_error);
            if (err) {
                *err = "could not update content ownership metadata: " + write_error;
                if (move_back_error) *err += "; file recovery rollback failed: " + move_back_error.message();
            }
            return false;
        }
        ++written;
    }
    if (out_path) *out_path = destination.wstring();
    return true;
}

bool move_screenshot_to_trash(const Instance& instance, const std::wstring& source,
                              std::wstring* out_path, std::string* err) {
    ProfileIdentitySnapshot identity;
    if (!capture_profile_identity(instance, identity, err)) return false;
    const fs::path screenshots = fs::path(instance.directory) / L"screenshots";
    const fs::path file(source);
    std::error_code ec;
    if (!require_real_directory(screenshots, "profile screenshots directory", err)) return false;
    const fs::file_status file_status = fs::symlink_status(file, ec);
    if (ec || fs::is_symlink(file_status) || !fs::is_regular_file(file_status)) {
        if (err) *err = ec ? ec.message() : "screenshot file does not exist";
        return false;
    }
    if (!is_direct_child(file, screenshots, err, "screenshot")) return false;
    const std::wstring filename = file.filename().wstring();
    if (!safe_filename(filename)) {
        if (err) *err = "screenshot filename is unsafe";
        return false;
    }
    fs::path trash;
    if (!prepare_profile_local_directory(instance, fs::path(L".amalgam-trash") / L"screenshots",
                                         "screenshot recovery folder", trash, err)) return false;
    const std::wstring prefix = std::to_wstring(now_seconds()) + L"-";
    fs::path destination = trash / (prefix + filename);
    int suffix = 2;
    while (fs::exists(destination, ec) && !ec)
        destination = trash / (prefix + std::to_wstring(suffix++) + L"-" + filename);
    if (ec) {
        if (err) *err = "cannot prepare screenshot recovery path: " + ec.message();
        return false;
    }
    if (!profile_identity_matches(instance, identity, err) ||
        !require_real_directory(screenshots, "profile screenshots directory", err) ||
        !is_direct_child(file, screenshots, err, "screenshot")) {
        return false;
    }
    const fs::file_status current_status = fs::symlink_status(file, ec);
    if (ec || fs::is_symlink(current_status) || !fs::is_regular_file(current_status)) {
        if (err) *err = ec ? ec.message() : "screenshot changed; refresh the list and try again";
        return false;
    }
    fs::rename(file, destination, ec);
    if (ec) {
        if (err) *err = "could not move screenshot to recovery: " + ec.message();
        return false;
    }
    if (out_path) *out_path = destination.wstring();
    return true;
}

bool backup_world(const Instance& instance, const std::wstring& source,
                  std::wstring* out_path, std::string* err) {
    ProfileIdentitySnapshot identity;
    if (!capture_profile_identity(instance, identity, err)) return false;
    fs::path world;
    if (!validate_world_source(instance, source, world, err)) return false;
    std::error_code ec;
    fs::path backups;
    if (!prepare_profile_local_directory(instance, fs::path(L".amalgam-backups") / L"worlds",
                                         "world backup folder", backups, err)) return false;
    const fs::path destination = unique_recovery_destination(backups, world.filename().wstring(), ec);
    if (ec) {
        if (err) *err = "cannot prepare world backup path: " + ec.message();
        return false;
    }
    if (!profile_identity_matches(instance, identity, err) ||
        !validate_world_source(instance, source, world, err)) return false;
    fs::copy(world, destination, fs::copy_options::recursive | fs::copy_options::skip_symlinks, ec);
    if (ec) {
        std::error_code cleanup_error;
        fs::remove_all(destination, cleanup_error);
        if (err) *err = "could not create world backup: " + ec.message();
        return false;
    }
    if (out_path) *out_path = destination.wstring();
    return true;
}

bool move_world_to_trash(const Instance& instance, const std::wstring& source,
                         std::wstring* out_path, std::string* err) {
    ProfileIdentitySnapshot identity;
    if (!capture_profile_identity(instance, identity, err)) return false;
    fs::path world;
    if (!validate_world_source(instance, source, world, err)) return false;
    std::error_code ec;
    fs::path trash;
    if (!prepare_profile_local_directory(instance, fs::path(L".amalgam-trash") / L"worlds",
                                         "world recovery folder", trash, err)) return false;
    const fs::path destination = unique_recovery_destination(trash, world.filename().wstring(), ec);
    if (ec) {
        if (err) *err = "cannot prepare world recovery path: " + ec.message();
        return false;
    }
    if (!profile_identity_matches(instance, identity, err) ||
        !validate_world_source(instance, source, world, err)) return false;
    fs::rename(world, destination, ec);
    if (ec) {
        if (err) *err = "could not move world to recovery: " + ec.message();
        return false;
    }
    if (out_path) *out_path = destination.wstring();
    return true;
}

namespace {

const wchar_t* const kRestoreRoots[] = {
    L"mods", L"config", L"resourcepacks", L"shaderpacks", L"datapacks"};
const wchar_t* const kRestoreFiles[] = {
    L"instance.json", L"amalgam-dependencies.json", L"amalgam-local-content.json", L"options.txt"};

fs::path unique_restore_directory(const fs::path& root, const std::wstring& base_name) {
    fs::path candidate = root / base_name;
    std::error_code ec;
    int suffix = 2;
    while (fs::exists(candidate, ec))
        candidate = root / (base_name + L"-" + std::to_wstring(suffix++));
    return candidate;
}

bool copy_restore_payload(const fs::path& source_root, const fs::path& destination_root,
                          std::string* err) {
    if (!require_real_directory(source_root, "restore source", err)) return false;
    const fs::path destination_parent = destination_root.parent_path();
    if (destination_parent.empty() ||
        !require_real_directory(destination_parent, "restore destination folder", err)) {
        return false;
    }
    std::error_code ec;
    const fs::file_status existing_destination = fs::symlink_status(destination_root, ec);
    if (!allow_missing_path_error(ec) || (fs::exists(existing_destination) &&
               (fs::is_symlink(existing_destination) || !fs::is_directory(existing_destination)))) {
        if (err) *err = ec ? "cannot inspect restore payload: " + ec.message()
                           : "restore payload must remain inside its managed profile folder";
        return false;
    }
    fs::create_directories(destination_root, ec);
    if (ec) {
        if (err) *err = "cannot create restore payload: " + ec.message();
        return false;
    }
    if (!require_real_directory(destination_root, "restore payload", err)) return false;
    for (const wchar_t* root_name : kRestoreRoots) {
        const fs::path source = source_root / root_name;
        const fs::file_status source_status = fs::symlink_status(source, ec);
        if (!allow_missing_path_error(ec)) {
            if (err) *err = "cannot inspect restore payload: " + ec.message();
            return false;
        }
        if (!fs::exists(source_status)) continue;
        if (fs::is_symlink(source_status) || !fs::is_directory(source_status)) {
            if (err) *err = "restore payload contains a redirected managed folder";
            return false;
        }
        fs::copy(source, destination_root / root_name,
                 fs::copy_options::recursive | fs::copy_options::overwrite_existing |
                     fs::copy_options::skip_symlinks,
                 ec);
        if (ec) {
            if (err) *err = "restore point copy failed: " + ec.message();
            return false;
        }
    }
    for (const wchar_t* file_name : kRestoreFiles) {
        const fs::path source = source_root / file_name;
        const fs::file_status source_status = fs::symlink_status(source, ec);
        if (!allow_missing_path_error(ec)) {
            if (err) *err = "cannot inspect restore metadata: " + ec.message();
            return false;
        }
        if (!fs::exists(source_status)) continue;
        if (fs::is_symlink(source_status) || !fs::is_regular_file(source_status)) {
            if (err) *err = "restore metadata must be a regular file";
            return false;
        }
        fs::copy_file(source, destination_root / file_name,
                      fs::copy_options::overwrite_existing, ec);
        if (ec) {
            if (err) *err = "restore metadata copy failed: " + ec.message();
            return false;
        }
    }
    return true;
}

// Replace the managed payload as a set.  It is intentionally limited to game
// content, launcher ownership metadata, and game options; worlds remain out of
// scope for profile-content restores.
bool apply_restore_payload(const fs::path& source_root, const fs::path& destination_root,
                           std::string* err) {
    if (!require_real_directory(source_root, "restore source", err) ||
        !require_real_directory(destination_root, "selected profile directory", err)) {
        return false;
    }
    std::error_code ec;
    for (const wchar_t* root_name : kRestoreRoots) {
        const fs::path source = source_root / root_name;
        const fs::file_status source_status = fs::symlink_status(source, ec);
        if (!allow_missing_path_error(ec)) {
            if (err) *err = "cannot inspect restore source: " + ec.message();
            return false;
        }
        const fs::path destination = destination_root / root_name;
        const fs::file_status destination_status = fs::symlink_status(destination, ec);
        if (!allow_missing_path_error(ec)) {
            if (err) *err = "cannot inspect restored content destination: " + ec.message();
            return false;
        }
        if (fs::exists(source_status) &&
            (fs::is_symlink(source_status) || !fs::is_directory(source_status))) {
            if (err) *err = "restore source contains a redirected managed folder";
            return false;
        }
        if (fs::exists(destination_status) &&
            (fs::is_symlink(destination_status) || !fs::is_directory(destination_status))) {
            if (err) *err = "restored content destination is redirected or invalid";
            return false;
        }
        fs::remove_all(destination, ec);
        if (ec) {
            if (err) *err = "cannot replace restored content: " + ec.message();
            return false;
        }
        if (!fs::exists(source_status)) continue;
        fs::copy(source, destination,
                 fs::copy_options::recursive | fs::copy_options::overwrite_existing |
                     fs::copy_options::skip_symlinks,
                 ec);
        if (ec) {
            if (err) *err = "restore copy failed: " + ec.message();
            return false;
        }
    }
    for (const wchar_t* file_name : kRestoreFiles) {
        const fs::path source = source_root / file_name;
        const fs::file_status source_status = fs::symlink_status(source, ec);
        if (!allow_missing_path_error(ec)) {
            if (err) *err = "cannot inspect restore metadata: " + ec.message();
            return false;
        }
        const fs::path destination = destination_root / file_name;
        const fs::file_status destination_status = fs::symlink_status(destination, ec);
        if (!allow_missing_path_error(ec)) {
            if (err) *err = "cannot inspect restored metadata destination: " + ec.message();
            return false;
        }
        if (fs::exists(source_status) &&
            (fs::is_symlink(source_status) || !fs::is_regular_file(source_status))) {
            if (err) *err = "restore metadata must be a regular file";
            return false;
        }
        if (fs::exists(destination_status) &&
            (fs::is_symlink(destination_status) || !fs::is_regular_file(destination_status))) {
            if (err) *err = "restored metadata destination is redirected or invalid";
            return false;
        }
        // Never remove the profile definition merely because an old manually
        // created restore point predates it.
        if (!fs::exists(source_status)) {
            if (std::wstring(file_name) != L"instance.json") fs::remove(destination, ec);
        } else {
            fs::copy_file(source, destination, fs::copy_options::overwrite_existing, ec);
        }
        if (ec) {
            if (err) *err = "restore metadata failed: " + ec.message();
            return false;
        }
    }
    return true;
}

}  // namespace

bool create_restore_point(const Instance& instance, std::wstring* out_path, std::string* err) {
    ProfileIdentitySnapshot identity;
    if (!capture_profile_identity(instance, identity, err)) return false;
    fs::path backup_root;
    if (!prepare_profile_local_directory(instance, L".amalgam-restore", "profile restore folder",
                                         backup_root, err)) return false;
    const fs::path target = unique_restore_directory(
        backup_root, std::to_wstring(static_cast<long long>(std::time(nullptr))));
    std::string copy_error;
    if (!copy_restore_payload(fs::path(instance.directory), target, &copy_error)) {
        std::error_code cleanup_error;
        fs::remove_all(target, cleanup_error);
        if (err) *err = copy_error;
        return false;
    }
    if (!profile_identity_matches(instance, identity, err)) {
        std::error_code cleanup_error;
        fs::remove_all(target, cleanup_error);
        return false;
    }
    if (out_path) *out_path = target.wstring();
    return true;
}

bool restore_latest(const Instance& instance, std::string* err) {
    ProfileIdentitySnapshot identity;
    if (!capture_profile_identity(instance, identity, err)) return false;
    const fs::path profile_root(instance.directory);
    const fs::path backup_root = profile_root / L".amalgam-restore";
    std::error_code ec;
    if (!fs::exists(backup_root, ec)) {
        if (err) *err = "no restore points found";
        return false;
    }
    if (!require_real_directory(backup_root, "profile restore folder", err)) return false;
    fs::path latest;
    for (const auto& entry : fs::directory_iterator(backup_root, ec)) {
        if (ec) break;
        const fs::file_status status = entry.symlink_status(ec);
        if (ec || fs::is_symlink(status) || !fs::is_directory(status)) continue;
        if (latest.empty() || entry.path().filename().wstring() > latest.filename().wstring())
            latest = entry.path();
    }
    if (latest.empty()) {
        if (err) *err = "no restore points found";
        return false;
    }

    // Stage the requested snapshot before changing the live profile. Then make
    // a normal restore point of the current state: a second Restore action is
    // therefore a real undo, and a failed apply can safely roll back.
    fs::path staging_root;
    if (!prepare_profile_local_directory(instance, L".amalgam-restore-staging",
                                         "restore staging folder", staging_root, err)) return false;
    const fs::path staged = unique_restore_directory(
        staging_root, L"restore-" + std::to_wstring(static_cast<long long>(std::time(nullptr))));
    std::string stage_error;
    if (!copy_restore_payload(latest, staged, &stage_error)) {
        std::error_code cleanup_error;
        fs::remove_all(staged, cleanup_error);
        if (err) *err = "restore could not be staged: " + stage_error;
        return false;
    }

    std::wstring rollback_path;
    std::string rollback_error;
    if (!create_restore_point(instance, &rollback_path, &rollback_error)) {
        std::error_code cleanup_error;
        fs::remove_all(staged, cleanup_error);
        if (err) *err = "restore cancelled: current profile backup failed: " + rollback_error;
        return false;
    }
    if (!profile_identity_matches(instance, identity, err)) {
        std::error_code cleanup_error;
        fs::remove_all(staged, cleanup_error);
        return false;
    }

    std::string apply_error;
    const bool restored = apply_restore_payload(staged, profile_root, &apply_error);
    std::error_code cleanup_error;
    fs::remove_all(staged, cleanup_error);
    if (restored) return true;

    std::string recovery_error;
    const bool recovered = apply_restore_payload(fs::path(rollback_path), profile_root, &recovery_error);
    if (err) {
        *err = "restore failed: " + apply_error;
        if (recovered)
            *err += "; the original profile was restored";
        else
            *err += "; rollback also failed: " + recovery_error;
    }
    return false;
}

namespace {

uint64_t backup_directory_size(const fs::path& root, std::error_code& ec) {
    uint64_t total = 0;
    if (!fs::exists(root, ec)) return 0;
    for (const auto& entry : fs::recursive_directory_iterator(root, ec)) {
        if (ec) break;
        const fs::file_status status = entry.symlink_status(ec);
        if (ec || fs::is_symlink(status) || !fs::is_regular_file(status)) continue;
        total += static_cast<uint64_t>(entry.file_size(ec));
    }
    return total;
}

bool backup_path_is_owned(const Instance& instance, const fs::path& path) {
    if (path.empty() || !safe_filename(path.filename().wstring())) return false;
    const fs::path root = fs::path(instance.directory) / L".amalgam-restore";
    return same_profile_path(path.parent_path(), root, nullptr);
}

}  // namespace

std::vector<BackupEntry> list_restore_points(const Instance& instance, std::string* err) {
    std::vector<BackupEntry> result;
    const fs::path root = fs::path(instance.directory) / L".amalgam-restore";
    std::error_code ec;
    const fs::file_status root_status = fs::symlink_status(root, ec);
    if (!allow_missing_path_error(ec)) {
        if (err) *err = "could not inspect restore points: " + ec.message();
        return result;
    }
    if (!fs::exists(root_status)) return result;
    if (fs::is_symlink(root_status) || !fs::is_directory(root_status)) {
        if (err) *err = "restore points folder must remain inside the selected profile";
        return result;
    }
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (ec) break;
        const fs::file_status status = entry.symlink_status(ec);
        if (ec || fs::is_symlink(status) || !fs::is_directory(status)) continue;
        BackupEntry backup;
        backup.path = entry.path().wstring();
        backup.name = entry.path().filename().string();
        backup.size_bytes = backup_directory_size(entry.path(), ec);
        const auto timestamp = entry.last_write_time(ec).time_since_epoch();
        backup.modified_at = std::chrono::duration_cast<std::chrono::seconds>(timestamp).count();
        result.push_back(std::move(backup));
    }
    if (ec && err) *err = "could not enumerate restore points: " + ec.message();
    std::sort(result.begin(), result.end(),
              [](const BackupEntry& a, const BackupEntry& b) { return a.name > b.name; });
    return result;
}

bool remove_restore_point(const Instance& instance, const std::wstring& path, std::string* err) {
    ProfileIdentitySnapshot identity;
    if (!capture_profile_identity(instance, identity, err)) return false;
    if (!backup_path_is_owned(instance, fs::path(path))) {
        if (err) *err = "restore point path is outside the selected profile";
        return false;
    }
    const fs::path restore_root = fs::path(instance.directory) / L".amalgam-restore";
    if (!require_real_directory(restore_root, "profile restore folder", err)) return false;
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(path, ec);
    if (ec || fs::is_symlink(status) || !fs::is_directory(status)) {
        if (err) *err = ec ? "could not inspect restore point: " + ec.message()
                           : "restore point must be a managed folder inside this profile";
        return false;
    }
    if (!profile_identity_matches(instance, identity, err)) return false;
    fs::remove_all(path, ec);
    if (ec) {
        if (err) *err = "could not remove restore point: " + ec.message();
        return false;
    }
    return true;
}

const char* content_type_name(ContentType type) {
    switch (type) {
        case ContentType::Mod: return "Mods";
        case ContentType::ResourcePack: return "Resource Packs";
        case ContentType::Shader: return "Shaders";
        case ContentType::DataPack: return "Data Packs";
        case ContentType::Config: return "Config";
        default: return "Other";
    }
}

bool remove(const Instance& instance, std::string* err) {
    if (err) err->clear();
    ProfileIdentitySnapshot identity;
    if (!capture_profile_identity(instance, identity, err)) return false;
    const fs::path source(instance.directory);
    std::error_code ec;
    const std::wstring name = source.filename().wstring();
    if (!safe_filename(name) || name.rfind(L".amalgam-", 0) == 0 || source.parent_path().empty()) {
        if (err) *err = "profile directory is not safe to move";
        return false;
    }
    const fs::path recovery = source.parent_path() / L".amalgam-profile-recovery";
    const fs::file_status recovery_status = fs::symlink_status(recovery, ec);
    if (!allow_missing_path_error(ec) || (fs::exists(recovery_status) &&
               (fs::is_symlink(recovery_status) || !fs::is_directory(recovery_status)))) {
        if (err) *err = ec ? "could not inspect profile recovery folder: " + ec.message()
                           : "profile recovery folder must remain beside the active library";
        return false;
    }
    fs::create_directories(recovery, ec);
    if (ec) {
        if (err) *err = "cannot create profile recovery folder: " + ec.message();
        return false;
    }
    if (!require_real_directory(recovery, "profile recovery folder", err)) return false;
    const fs::path destination = unique_recovery_destination(recovery, name, ec);
    if (ec) {
        if (err) *err = "cannot prepare profile recovery path: " + ec.message();
        return false;
    }
    if (!profile_identity_matches(instance, identity, err)) return false;
    fs::rename(source, destination, ec);
    if (ec && err) *err = "could not move profile to recovery: " + ec.message();
    return !ec;
}

}  // namespace aml::instances
