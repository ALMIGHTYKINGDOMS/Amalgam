#pragma once

#include <string>
#include <vector>
#include <map>
#include <functional>
#include <cstdint>
#include <ctime>

namespace aml::essentials {

// ---------------------------------------------------------------------------
// Enums
// ---------------------------------------------------------------------------

enum class SessionPrivacy {
    InviteOnly,
    FriendsOnly,
    FriendsOfFriends,
    Private
};

enum class SessionState {
    Starting,
    Online,
    Stopping,
    Ended,
    Crashed
};

enum class FriendStatus {
    Offline,
    Online,
    InLauncher,
    Playing,
    Hosting,
    Joining,
    Away
};

enum class InviteStatus {
    Pending,
    Accepted,
    Declined,
    Expired,
    Cancelled
};

enum class SyncMode {
    None,
    ExistingProfile,
    TemporaryProfile
};

enum class ConnectionType {
    None,
    Direct,
    Relay,
    Failed
};

enum class JobState {
    Idle,
    Running,
    Completed,
    Failed,
    Cancelled
};

enum class CompatibilityLevel {
    Match,
    MinorMismatch,
    MajorMismatch,
    Incompatible
};

enum class ConfigClassification {
    Required,
    Recommended,
    LocalOnly,
    Unknown
};

// ---------------------------------------------------------------------------
// Core Models
// ---------------------------------------------------------------------------

struct EssentialsFriend {
    std::string user_id;
    std::string username;
    std::string display_name;
    std::string avatar_url;
    FriendStatus status = FriendStatus::Offline;
    std::string status_message;
    std::string current_profile_name;
    std::string current_game_version;
    int64_t last_seen = 0;
    bool blocked = false;
};

struct EssentialsFriendRequest {
    std::string id;
    std::string sender_id;
    std::string sender_username;
    std::string sender_avatar_url;
    std::string receiver_id;
    InviteStatus status = InviteStatus::Pending;
    std::string message;
    int64_t created_at = 0;
};

struct EssentialsPresence {
    std::string user_id;
    FriendStatus status = FriendStatus::Offline;
    std::string status_message;
    std::string current_profile_id;
    std::string current_profile_name;
    std::string current_game_version;
    std::string current_loader;
    std::string session_id;
    int64_t last_seen = 0;
};

struct EssentialsInvite {
    std::string id;
    std::string host_user_id;
    std::string host_username;
    std::string target_user_id;
    std::string session_id;
    std::string session_address;
    std::string join_token;
    std::string world_name;
    std::string minecraft_version;
    std::string loader;
    std::string loader_version;
    int mod_count = 0;
    InviteStatus status = InviteStatus::Pending;
    int64_t created_at = 0;
    int64_t expires_at = 0;
};

struct ModEntry {
    std::string id;
    std::string name;
    std::string version;
    std::string hash;
    std::string source;
    bool enabled = true;
};

struct EssentialsManifest {
    std::string session_id;
    std::string host_user_id;
    std::string profile_id;
    std::string profile_name;
    std::string minecraft_version;
    std::string loader;
    std::string loader_version;
    std::vector<ModEntry> mods;
    std::vector<std::string> required_resource_packs;
    std::map<std::string, std::string> required_configs;
    int64_t created_at = 0;
};

struct CompatCheck {
    CompatibilityLevel version_level = CompatibilityLevel::Match;
    CompatibilityLevel loader_level = CompatibilityLevel::Match;
    CompatibilityLevel mods_level = CompatibilityLevel::Match;
    CompatibilityLevel overall = CompatibilityLevel::Match;

    std::string host_minecraft_version;
    std::string local_minecraft_version;
    std::string host_loader;
    std::string local_loader;
    std::string host_loader_version;
    std::string local_loader_version;

    std::vector<ModEntry> missing_mods;
    std::vector<ModEntry> outdated_mods;
    std::vector<ModEntry> extra_mods;
    std::vector<std::string> missing_resource_packs;
    std::vector<std::string> missing_configs;

    std::string summary() const;
};

struct SyncPlan {
    SyncMode mode = SyncMode::None;
    std::string source_profile_id;
    std::string target_profile_name;
    std::vector<ModEntry> mods_to_download;
    std::vector<ModEntry> mods_to_update;
    std::vector<std::string> configs_to_apply;
    std::vector<std::string> resource_packs_to_download;
    uint64_t total_download_size = 0;
    bool backup_existing = true;
};

struct EssentialsSession {
    std::string id;
    std::string alias;
    std::string address;
    std::string status;
    std::string host_user_id;
    std::string host_username;
    std::string profile_id;
    std::string profile_name;
    std::string world_name;
    std::string minecraft_version;
    std::string loader;
    std::string loader_version;
    std::string join_token; // kept locally; never persisted or logged remotely
    int player_limit = 8;
    int player_count = 1;
    SessionPrivacy privacy = SessionPrivacy::FriendsOnly;
    SessionState state = SessionState::Starting;
    std::vector<std::string> connected_players;
    std::vector<std::string> banned_players;
    EssentialsManifest manifest;
    int64_t created_at = 0;
    int64_t expires_at = 0;
};

struct HostOptions {
    std::string profile_id;
    std::string world_name;
    std::string alias;
    SessionPrivacy privacy = SessionPrivacy::FriendsOnly;
    int player_limit = 8;
    uint16_t server_port = 25565;
    std::string java_path;
    int memory_mb = 2048;
};

struct JoinResult {
    bool success = false;
    std::string error;
    std::string session_id;
    ConnectionType connection_type = ConnectionType::None;
};

struct ConnectionStatus {
    ConnectionType type = ConnectionType::None;
    int ping_ms = 0;
    std::string remote_address;
    bool encrypted = true;
};

struct EssentialsNotification {
    std::string id;
    std::string title;
    std::string body;
    std::string session_id;
    std::string from_user_id;
    std::string from_username;
    int64_t created_at = 0;
    bool read = false;
    std::function<void()> on_accept;
    std::function<void()> on_decline;
};

struct EssentialsJob {
    std::string id;
    std::string description;
    JobState state = JobState::Idle;
    float progress = 0.0f;
    std::string status_text;
    int64_t started_at = 0;
};

struct WorldEntry {
    std::string name;
    std::string path;
    uint64_t size = 0;
    int64_t last_modified = 0;
};

struct SessionBanEntry {
    std::string user_id;
    std::string username;
    int64_t banned_at = 0;
    std::string reason;
};

// ---------------------------------------------------------------------------
// Friend Code
// ---------------------------------------------------------------------------

inline std::string generate_friend_code(const std::string& user_id) {
    // AMG-XXXX-XXXX format from user UUID
    std::string clean;
    for (char c : user_id) {
        if (std::isalnum(c)) clean += static_cast<char>(std::toupper(c));
    }
    if (clean.size() < 8) clean.resize(8, '0');
    return "AMG-" + clean.substr(0, 4) + "-" + clean.substr(4, 4);
}

// ---------------------------------------------------------------------------
// Utility
// ---------------------------------------------------------------------------

inline const char* session_privacy_name(SessionPrivacy p) {
    switch (p) {
        case SessionPrivacy::InviteOnly: return "Invite Only";
        case SessionPrivacy::FriendsOnly: return "Friends Only";
        case SessionPrivacy::FriendsOfFriends: return "Friends of Friends";
        case SessionPrivacy::Private: return "Private";
        default: return "Unknown";
    }
}

inline const char* session_state_name(SessionState s) {
    switch (s) {
        case SessionState::Starting: return "Starting";
        case SessionState::Online: return "Online";
        case SessionState::Stopping: return "Stopping";
        case SessionState::Ended: return "Ended";
        case SessionState::Crashed: return "Crashed";
        default: return "Unknown";
    }
}

inline const char* friend_status_name(FriendStatus s) {
    switch (s) {
        case FriendStatus::Offline: return "Offline";
        case FriendStatus::Online: return "Online";
        case FriendStatus::InLauncher: return "In Launcher";
        case FriendStatus::Playing: return "Playing";
        case FriendStatus::Hosting: return "Hosting";
        case FriendStatus::Joining: return "Joining";
        case FriendStatus::Away: return "Away";
        default: return "Unknown";
    }
}

inline const char* compat_level_name(CompatibilityLevel l) {
    switch (l) {
        case CompatibilityLevel::Match: return "Match";
        case CompatibilityLevel::MinorMismatch: return "Minor Mismatch";
        case CompatibilityLevel::MajorMismatch: return "Major Mismatch";
        case CompatibilityLevel::Incompatible: return "Incompatible";
        default: return "Unknown";
    }
}

}  // namespace aml::essentials
