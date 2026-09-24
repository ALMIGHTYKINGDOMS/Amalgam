#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace aml::instances {

struct Instance {
    std::string id;
    std::string name;
    std::string minecraft_version;
    std::string loader = "auto";
    std::string loader_version;
    std::string performance_profile = "auto";
    std::string java_path;
    std::string icon_url;
    std::string pack_source;
    std::string pack_project;
    std::string pack_version;
    bool pack_modified = false;
    std::string group;
    int memory_mb = 0;  // 0 = performance profile recommendation
    int64_t last_played = 0;
    bool favorite = false;
    bool legacy = false;
    bool is_ai_profile = false;  // AI Profile: built-in Amalgam AI assistant
    int ai_mode = 0;             // 0 = Ask, 1 = Build, 2 = Agent, 3 = Auto
    bool ai_live_vision = false; // live Minecraft window observation
    std::wstring directory;
};

enum class ContentType { Mod, ResourcePack, Shader, DataPack, Config, Other };

struct ContentEntry {
    std::wstring path;
    std::string filename;
    ContentType type = ContentType::Other;
    bool enabled = true;
    uint64_t size = 0;
    bool managed = false;
    std::string owner_project;
    std::string owner_source;
    std::string owner_version;
    std::string sha1;
};

// Captures the identity attributes used by UI confirmation flows.  A caller
// can take this snapshot when the user chooses a file, then require it to
// still match immediately before a recoverable move or state change.  It is
// intentionally profile-qualified: arbitrary filesystem paths are never a
// valid substitute for managed profile content.
struct ContentFileSnapshot {
    std::wstring path;
    ContentType type = ContentType::Other;
    uint64_t size = 0;
    std::filesystem::file_time_type last_write_time{};
};

// A confirmation or background request captures this before it starts.  The
// profile actions below re-check it immediately before they mutate managed
// files, so a profile that was replaced, moved, or edited while a dialog was
// open cannot silently receive the result intended for an older selection.
struct ProfileIdentitySnapshot {
    std::wstring directory;
    std::string id;
    uint64_t metadata_size = 0;
    std::filesystem::file_time_type metadata_last_write_time{};
};

struct BackupEntry {
    std::wstring path;
    std::string name;
    uint64_t size_bytes = 0;
    int64_t modified_at = 0;
};

std::vector<Instance> scan(const std::wstring& instances_dir, std::string* err = nullptr);
bool create(const std::wstring& instances_dir, const Instance& source, Instance& out,
            std::string* err = nullptr);
bool duplicate(const Instance& source, const std::wstring& instances_dir, const std::string& new_name,
               Instance& out, std::string* err = nullptr);
bool save(const Instance& instance, std::string* err = nullptr);
bool load(const std::wstring& directory, Instance& out, std::string* err = nullptr);
bool capture_profile_identity(const Instance& instance, ProfileIdentitySnapshot& out,
                              std::string* err = nullptr);
bool profile_identity_matches(const Instance& instance, const ProfileIdentitySnapshot& snapshot,
                              std::string* err = nullptr);
// Removes a profile from the active library by moving its complete directory
// into a sibling recovery folder. It is intentionally recoverable rather than
// a permanent recursive delete.
bool remove(const Instance& instance, std::string* err = nullptr);
std::vector<ContentEntry> list_content(const Instance& instance, std::string* err = nullptr);
bool set_content_enabled(const ContentEntry& entry, bool enabled, std::string* err = nullptr);
// Profile-qualified form for new UI flows.  It rejects content outside the
// selected profile (including redirected content roots) before renaming it.
bool set_content_enabled(const Instance& instance, const ContentEntry& entry, bool enabled,
                         std::string* err = nullptr);
bool capture_content_file_snapshot(const Instance& instance, const ContentEntry& entry,
                                   ContentFileSnapshot& out, std::string* err = nullptr);
bool content_file_matches_snapshot(const Instance& instance, const ContentEntry& entry,
                                   const ContentFileSnapshot& snapshot,
                                   std::string* err = nullptr);
// Backward-compatible remove action: it now routes through profile recovery
// instead of permanently deleting content.
bool remove_content(const Instance& instance, const ContentEntry& entry, std::string* err = nullptr);
bool import_content(const Instance& instance, const std::wstring& source, ContentType type,
                    ContentEntry* out = nullptr, std::string* err = nullptr);
// Moves a managed-profile content file into profile-local recovery and removes
// stale ownership records. The entry must be a direct child of its content
// root in the selected profile.
bool move_content_to_trash(const Instance& instance, const ContentEntry& entry,
                           std::wstring* out_path = nullptr, std::string* err = nullptr);
// Moves a screenshot into a profile-local recovery folder instead of deleting
// it. The source must belong to this instance's screenshots directory.
bool move_screenshot_to_trash(const Instance& instance, const std::wstring& source,
                              std::wstring* out_path = nullptr, std::string* err = nullptr);
// World actions are intentionally profile-scoped. Backups and recovered worlds
// live outside Minecraft's saves directory so they cannot appear as playable
// worlds or be overwritten by pack content.
bool backup_world(const Instance& instance, const std::wstring& source,
                  std::wstring* out_path = nullptr, std::string* err = nullptr);
bool move_world_to_trash(const Instance& instance, const std::wstring& source,
                         std::wstring* out_path = nullptr, std::string* err = nullptr);
const char* content_type_name(ContentType type);
bool create_restore_point(const Instance& instance, std::wstring* out_path = nullptr,
                          std::string* err = nullptr);
bool restore_latest(const Instance& instance, std::string* err = nullptr);
std::vector<BackupEntry> list_restore_points(const Instance& instance, std::string* err = nullptr);
bool remove_restore_point(const Instance& instance, const std::wstring& path,
                          std::string* err = nullptr);

}  // namespace aml::instances
