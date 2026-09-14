#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <ctime>

namespace aml::bedrock {

// ---------------------------------------------------------------------------
// Data model
// ---------------------------------------------------------------------------

enum class BedrockPackType {
    Behavior = 0,
    Resource = 1,
    Skin = 2,
    World = 3,
    Template = 4,
};

inline const char* bedrock_pack_type_name(BedrockPackType t) {
    switch (t) {
        case BedrockPackType::Behavior: return "Behavior Pack";
        case BedrockPackType::Resource:  return "Resource Pack";
        case BedrockPackType::Skin:      return "Skin Pack";
        case BedrockPackType::World:     return "World";
        case BedrockPackType::Template:  return "Template";
    }
    return "Unknown";
}

struct BedrockPackEntry {
    std::string profile_id;
    std::string profile_name;
    std::string uuid;            // canonical pack UUID from manifest
    std::string name;
    std::string version;
    BedrockPackType type = BedrockPackType::Behavior;
    std::string filename;        // installed file name in profile content dir
    bool enabled = true;
    int load_order = 0;
    std::string source;          // "imported" | "discovered" | "curseforge" | "modrinth"
    std::string project_id;      // optional provider project id
    std::string author;
    std::string description;
    uint64_t size_bytes = 0;
    std::string sha1;
    bool duplicate = false;      // flagged duplicate UUID
};

struct BedrockWorldEntry {
    std::string profile_id;
    std::string profile_name;
    std::string name;
    std::string folder;          // folder name under profile worlds/
    std::string dimension;       // "overworld" | "nether" | "the_end"
    bool enabled = true;
    uint64_t size_bytes = 0;
    std::string last_played;     // ISO-ish timestamp
    std::string icon_path;       // relative to profile dir
};

struct BedrockProfile {
    std::string id;
    std::string name;
    std::string minecraft_version;
    std::string banner_path;      // relative to profile dir
    std::string icon_path;
    std::vector<BedrockPackEntry> packs;
    std::vector<BedrockWorldEntry> worlds;
    std::string created;
    std::string last_played;
    int64_t last_played_ts = 0;
    bool favorite = false;
    std::string group;
    std::string updated_at;
};

struct BedrockBackupEntry {
    std::string id;
    std::string profile_id;
    std::string world_name;
    std::string timestamp;
    std::string label;
    uint64_t size_bytes = 0;
    std::string path;             // absolute path
};

struct BedrockHealthIssue {
    std::string severity;   // "error" | "warning" | "info"
    std::string message;
    bool auto_fixable = false;
};

// ---------------------------------------------------------------------------
// Low-level detection / launch (existing, unchanged)
// ---------------------------------------------------------------------------

const std::wstring& uwp_family();
std::wstring detect(std::string* err = nullptr);
bool installed();
bool package_registered(std::string* err = nullptr);
bool launch(std::string* err = nullptr);
std::wstring data_dir(std::string* err = nullptr);
bool validate_addon_file(const std::wstring& addon_file, std::string* err = nullptr);
bool install_addon(const std::wstring& addon_file, std::string* err = nullptr);

// ---------------------------------------------------------------------------
// Profile manager
// ---------------------------------------------------------------------------

class BedrockProfileManager {
public:
    BedrockProfileManager(const std::wstring& profiles_root);

    std::vector<BedrockProfile> scan(std::string* err = nullptr);
    bool create(const BedrockProfile& source, BedrockProfile& out, std::string* err = nullptr);
    bool duplicate(const BedrockProfile& source, const std::string& new_name,
                   BedrockProfile& out, std::string* err = nullptr);
    bool remove(const BedrockProfile& target, std::string* err = nullptr);
    bool save(const BedrockProfile& profile, std::string* err = nullptr);
    bool load(const std::string& id, BedrockProfile& out, std::string* err = nullptr);

    const std::wstring& root() const { return root_; }

private:
    std::wstring root_;
};

// ---------------------------------------------------------------------------
// Manifest parsing (.mcpack / .mcaddon / .zip)
// ---------------------------------------------------------------------------

struct BedrockManifest {
    std::string header_name;
    std::string header_description;
    std::string header_version;
    std::string manifest_version;
    std::string uuid;
    std::string module_name;
    std::string module_description;
    std::string module_version;
    std::string type;            // "resources" | "data" | "world"
    BedrockPackType pack_type = BedrockPackType::Behavior;
    std::string format_version;
    std::vector<std::string> game_versions;
    std::vector<std::string> dependencies;  // UUIDs
    bool valid = false;
};

BedrockManifest parse_manifest(const std::wstring& archive_path, std::string* err = nullptr);

// ---------------------------------------------------------------------------
// Import manager
// ---------------------------------------------------------------------------

struct BedrockImportResult {
    bool success = false;
    BedrockPackEntry pack;
    std::string error;
};

BedrockImportResult import_addon(const std::wstring& archive_path,
                                 const BedrockProfile& target,
                                 std::string* err = nullptr);
std::vector<BedrockImportResult> import_addons(
    const std::vector<std::wstring>& paths, const BedrockProfile& target,
    std::string* err = nullptr);
BedrockImportResult import_world(const std::wstring& world_path,
                                 const BedrockProfile& target,
                                 std::string* err = nullptr);

// ---------------------------------------------------------------------------
// Dependency resolver
// ---------------------------------------------------------------------------

struct BedrockDependencyInfo {
    std::string uuid;
    std::string name;
    bool installed = false;
    bool required = true;
};

std::vector<BedrockDependencyInfo> resolve_dependencies(
    const std::vector<BedrockPackEntry>& packs,
    const std::string& target_uuid);

// ---------------------------------------------------------------------------
// World manager
// ---------------------------------------------------------------------------

std::vector<BedrockWorldEntry> list_worlds(const BedrockProfile& profile,
                                           std::string* err = nullptr);
bool backup_world(const BedrockProfile& profile, const std::string& world_folder,
                  std::wstring& out_path, std::string* err = nullptr);
bool import_world_file(const std::wstring& source, const BedrockProfile& profile,
                       const std::string& target_name, std::string* err = nullptr);
bool export_world(const BedrockProfile& profile, const std::string& world_folder,
                  const std::wstring& dest_path, std::string* err = nullptr);

// ---------------------------------------------------------------------------
// Backup manager
// ---------------------------------------------------------------------------

std::vector<BedrockBackupEntry> list_backups(const BedrockProfile& profile,
                                             std::string* err = nullptr);
bool create_backup(const BedrockProfile& profile, const std::string& label,
                   std::wstring& out_path, std::string* err = nullptr);
bool restore_backup(const BedrockProfile& profile, const BedrockBackupEntry& backup,
                    std::string* err = nullptr);
bool delete_backup(const BedrockProfile& profile, const BedrockBackupEntry& backup,
                   std::string* err = nullptr);

// ---------------------------------------------------------------------------
// Health scanner
// ---------------------------------------------------------------------------

std::vector<BedrockHealthIssue> scan_health(const BedrockProfile& profile,
                                            std::string* err = nullptr);
bool auto_repair(const BedrockProfile& profile, std::string* err = nullptr);

// ---------------------------------------------------------------------------
// Sync manager — profile ↔ Bedrock com.mojang
// ---------------------------------------------------------------------------

bool sync_to_bedrock(const BedrockProfile& profile, std::string* err = nullptr);
bool sync_from_bedrock(BedrockProfile& profile, std::string* err = nullptr);

// ---------------------------------------------------------------------------
// Storage / activity
// ---------------------------------------------------------------------------

struct BedrockStorageInfo {
    uint64_t total_bytes = 0;
    uint64_t behavior_bytes = 0;
    uint64_t resource_bytes = 0;
    uint64_t world_bytes = 0;
    uint64_t other_bytes = 0;
};

BedrockStorageInfo compute_storage(const BedrockProfile& profile);

}  // namespace aml::bedrock
