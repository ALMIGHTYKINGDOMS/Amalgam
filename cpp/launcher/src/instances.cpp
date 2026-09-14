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

namespace fs = std::filesystem;

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
    fs::path root(instances_dir);
    std::string base = safe_id(new_name.empty() ? source.name : new_name);
    fs::path target = root / base;
    int suffix = 2;
    while (fs::exists(target)) target = root / (base + "-" + std::to_string(suffix++));
    std::error_code ec;
    fs::create_directories(target, ec);
    fs::copy(fs::path(source.directory), target,
             fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) {
        if (err) *err = "duplicate failed: " + ec.message();
        return false;
    }
    out = source;
    out.id = target.filename().string();
    out.name = new_name.empty() ? source.name + " Copy" : new_name;
    out.directory = target.wstring();
    out.last_played = 0;
    out.favorite = false;
    if (!save(out, err)) return false;
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
    while (fs::exists(destination, ec) && !ec)
        destination = directory / (prefix + std::to_wstring(suffix++) + L"-" + name);
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
        if (ec && err) *err = ec.message();
        return !ec;
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
    if (!fs::exists(world, ec) || ec || !fs::is_directory(world, ec)) {
        if (err) *err = ec ? ec.message() : "world directory does not exist";
        return false;
    }
    const fs::path saves = fs::path(instance.directory) / L"saves";
    if (!is_direct_child(world, saves, err, "world")) return false;
    const std::wstring name = world.filename().wstring();
    if (!safe_filename(name) || name.rfind(L".amalgam-", 0) == 0) {
        if (err) *err = "world directory name is unsafe";
        return false;
    }
    return true;
}

}  // namespace

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
    if (!fs::exists(source)) {
        if (err) *err = "content file does not exist";
        return false;
    }
    std::wstring name = source.filename().wstring();
    bool currently_enabled = !disabled_suffix(name);
    if (currently_enabled == enabled) return true;
    fs::path target = enabled ? source.parent_path() / name.substr(0, name.size() - 9)
                              : source.parent_path() / (name + L".disabled");
    std::error_code ec;
    fs::rename(source, target, ec);
    if (ec && err) *err = ec.message();
    return !ec;
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
    const fs::path source(entry.path);
    std::error_code ec;
    if (!fs::exists(source, ec) || ec || !fs::is_regular_file(source, ec)) {
        if (err) *err = ec ? ec.message() : "content file does not exist";
        return false;
    }
    const fs::path content_root = fs::path(instance.directory) / root_name;
    if (!is_direct_child(source, content_root, err, "content file")) return false;
    const std::wstring source_name = source.filename().wstring();
    if (!safe_filename(source_name)) {
        if (err) *err = "content filename is unsafe";
        return false;
    }
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

    const fs::path trash = fs::path(instance.directory) / L".amalgam-trash" / L"content" / root_name;
    fs::create_directories(trash, ec);
    if (ec) {
        if (err) *err = "cannot create content recovery folder: " + ec.message();
        return false;
    }
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
    const fs::path screenshots = fs::path(instance.directory) / L"screenshots";
    const fs::path file(source);
    std::error_code ec;
    if (!fs::exists(file, ec) || ec || !fs::is_regular_file(file, ec)) {
        if (err) *err = ec ? ec.message() : "screenshot file does not exist";
        return false;
    }
    if (!fs::equivalent(file.parent_path(), screenshots, ec) || ec) {
        if (err) *err = ec ? ec.message() : "screenshot is outside the selected profile";
        return false;
    }
    const std::wstring filename = file.filename().wstring();
    if (!safe_filename(filename)) {
        if (err) *err = "screenshot filename is unsafe";
        return false;
    }
    const fs::path trash = fs::path(instance.directory) / L".amalgam-trash" / L"screenshots";
    fs::create_directories(trash, ec);
    if (ec) {
        if (err) *err = "cannot create screenshot recovery folder: " + ec.message();
        return false;
    }
    const std::wstring prefix = std::to_wstring(now_seconds()) + L"-";
    fs::path destination = trash / (prefix + filename);
    int suffix = 2;
    while (fs::exists(destination, ec) && !ec)
        destination = trash / (prefix + std::to_wstring(suffix++) + L"-" + filename);
    if (ec) {
        if (err) *err = "cannot prepare screenshot recovery path: " + ec.message();
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
    fs::path world;
    if (!validate_world_source(instance, source, world, err)) return false;
    std::error_code ec;
    const fs::path backups = fs::path(instance.directory) / L".amalgam-backups" / L"worlds";
    fs::create_directories(backups, ec);
    if (ec) {
        if (err) *err = "cannot create world backup folder: " + ec.message();
        return false;
    }
    const fs::path destination = unique_recovery_destination(backups, world.filename().wstring(), ec);
    if (ec) {
        if (err) *err = "cannot prepare world backup path: " + ec.message();
        return false;
    }
    fs::copy(world, destination, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
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
    fs::path world;
    if (!validate_world_source(instance, source, world, err)) return false;
    std::error_code ec;
    const fs::path trash = fs::path(instance.directory) / L".amalgam-trash" / L"worlds";
    fs::create_directories(trash, ec);
    if (ec) {
        if (err) *err = "cannot create world recovery folder: " + ec.message();
        return false;
    }
    const fs::path destination = unique_recovery_destination(trash, world.filename().wstring(), ec);
    if (ec) {
        if (err) *err = "cannot prepare world recovery path: " + ec.message();
        return false;
    }
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
    std::error_code ec;
    fs::create_directories(destination_root, ec);
    if (ec) {
        if (err) *err = "cannot create restore payload: " + ec.message();
        return false;
    }
    for (const wchar_t* root_name : kRestoreRoots) {
        const fs::path source = source_root / root_name;
        const bool exists = fs::exists(source, ec);
        if (ec) {
            if (err) *err = "cannot inspect restore payload: " + ec.message();
            return false;
        }
        if (!exists) continue;
        fs::copy(source, destination_root / root_name,
                 fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        if (ec) {
            if (err) *err = "restore point copy failed: " + ec.message();
            return false;
        }
    }
    for (const wchar_t* file_name : kRestoreFiles) {
        const fs::path source = source_root / file_name;
        const bool exists = fs::exists(source, ec);
        if (ec) {
            if (err) *err = "cannot inspect restore metadata: " + ec.message();
            return false;
        }
        if (!exists) continue;
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
    std::error_code ec;
    for (const wchar_t* root_name : kRestoreRoots) {
        const fs::path source = source_root / root_name;
        const bool exists = fs::exists(source, ec);
        if (ec) {
            if (err) *err = "cannot inspect restore source: " + ec.message();
            return false;
        }
        const fs::path destination = destination_root / root_name;
        fs::remove_all(destination, ec);
        if (ec) {
            if (err) *err = "cannot replace restored content: " + ec.message();
            return false;
        }
        if (!exists) continue;
        fs::copy(source, destination,
                 fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        if (ec) {
            if (err) *err = "restore copy failed: " + ec.message();
            return false;
        }
    }
    for (const wchar_t* file_name : kRestoreFiles) {
        const fs::path source = source_root / file_name;
        const bool exists = fs::exists(source, ec);
        if (ec) {
            if (err) *err = "cannot inspect restore metadata: " + ec.message();
            return false;
        }
        const fs::path destination = destination_root / file_name;
        // Never remove the profile definition merely because an old manually
        // created restore point predates it.
        if (!exists) {
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
    const fs::path backup_root = fs::path(instance.directory) / L".amalgam-restore";
    const fs::path target = unique_restore_directory(
        backup_root, std::to_wstring(static_cast<long long>(std::time(nullptr))));
    std::string copy_error;
    if (!copy_restore_payload(fs::path(instance.directory), target, &copy_error)) {
        std::error_code cleanup_error;
        fs::remove_all(target, cleanup_error);
        if (err) *err = copy_error;
        return false;
    }
    if (out_path) *out_path = target.wstring();
    return true;
}

bool restore_latest(const Instance& instance, std::string* err) {
    const fs::path profile_root(instance.directory);
    const fs::path backup_root = profile_root / L".amalgam-restore";
    std::error_code ec;
    if (!fs::exists(backup_root, ec)) {
        if (err) *err = "no restore points found";
        return false;
    }
    fs::path latest;
    for (const auto& entry : fs::directory_iterator(backup_root, ec)) {
        if (ec || !entry.is_directory()) continue;
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
    const fs::path staging_root = profile_root / L".amalgam-restore-staging";
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
        if (!entry.is_regular_file(ec)) continue;
        total += static_cast<uint64_t>(entry.file_size(ec));
    }
    return total;
}

bool backup_path_is_owned(const Instance& instance, const fs::path& path) {
    const fs::path root = fs::path(instance.directory) / L".amalgam-restore";
    const fs::path normalized_root = fs::absolute(root).lexically_normal();
    const fs::path normalized_path = fs::absolute(path).lexically_normal();
    const std::wstring root_text = normalized_root.wstring() + L"\\";
    return normalized_path.wstring().rfind(root_text, 0) == 0;
}

}  // namespace

std::vector<BackupEntry> list_restore_points(const Instance& instance, std::string* err) {
    std::vector<BackupEntry> result;
    const fs::path root = fs::path(instance.directory) / L".amalgam-restore";
    std::error_code ec;
    if (!fs::exists(root, ec)) return result;
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (ec) break;
        if (!entry.is_directory(ec)) continue;
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
    if (!backup_path_is_owned(instance, fs::path(path))) {
        if (err) *err = "restore point path is outside the selected profile";
        return false;
    }
    std::error_code ec;
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
    const fs::path source(instance.directory);
    std::error_code ec;
    if (!fs::exists(source, ec) || ec || !fs::is_directory(source, ec)) {
        if (err) *err = ec ? ec.message() : "profile directory does not exist";
        return false;
    }
    const std::wstring name = source.filename().wstring();
    if (!safe_filename(name) || name.rfind(L".amalgam-", 0) == 0 || source.parent_path().empty()) {
        if (err) *err = "profile directory is not safe to move";
        return false;
    }
    const fs::path recovery = source.parent_path() / L".amalgam-profile-recovery";
    fs::create_directories(recovery, ec);
    if (ec) {
        if (err) *err = "cannot create profile recovery folder: " + ec.message();
        return false;
    }
    const fs::path destination = unique_recovery_destination(recovery, name, ec);
    if (ec) {
        if (err) *err = "cannot prepare profile recovery path: " + ec.message();
        return false;
    }
    fs::rename(source, destination, ec);
    if (ec && err) *err = "could not move profile to recovery: " + ec.message();
    return !ec;
}

}  // namespace aml::instances
