#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <filesystem>

namespace aml::server {

enum class ServerStage {
    NotInstalled,
    Installing,
    Ready,
    Running,
    Stopped,
    Error,
    Crashed
};

enum class ServerSoftware {
    Vanilla = 0,
    Paper,
    Purpur,
    Spigot,
    Forge,
    NeoForge,
    Fabric,
    Quilt,
    BedrockDedicatedServer,
    Folia,
    Velocity
};

enum class DeploymentType {
    Local,
    AmalgamCloud
};

enum class ServerFeature {
    Vanilla,
    Modded,
    Plugins
};

struct ServerConfig {
    std::string name;
    ServerSoftware software = ServerSoftware::Vanilla;
    std::string minecraft_version = "1.20.1";
    std::string software_version;
    std::string java_version;
    int allocated_ram_mb = 4096;
    int max_players = 20;
    int port = 25565;
    std::string server_directory;
    std::string linked_profile_id;
    ServerStage stage = ServerStage::NotInstalled;
    std::string status_message;
};

inline const char* server_software_name(ServerSoftware s) {
    switch (s) {
        case ServerSoftware::Vanilla: return "Vanilla";
        case ServerSoftware::Paper: return "Paper";
        case ServerSoftware::Purpur: return "Purpur";
        case ServerSoftware::Spigot: return "Spigot";
        case ServerSoftware::Forge: return "Forge";
        case ServerSoftware::NeoForge: return "NeoForge";
        case ServerSoftware::Fabric: return "Fabric";
        case ServerSoftware::Quilt: return "Quilt";
        case ServerSoftware::BedrockDedicatedServer: return "Bedrock Dedicated Server";
        case ServerSoftware::Folia: return "Folia";
        case ServerSoftware::Velocity: return "Velocity";
    }
    return "Unknown";
}

inline bool is_modded_software(ServerSoftware s) {
    return s == ServerSoftware::Forge || s == ServerSoftware::NeoForge ||
           s == ServerSoftware::Fabric || s == ServerSoftware::Quilt;
}

// A stage restored from disk is only a hint. The process the service supervises
// owns local run state, so an unsupervised Running stage has to read as Stopped
// instead of as a server that is actually up.
inline ServerStage reconciled_stage(const ServerConfig& server, bool supervised) {
    return (server.stage == ServerStage::Running && !supervised) ? ServerStage::Stopped
                                                                 : server.stage;
}

struct ServerMod {
    std::string name;
    std::string version;
    std::string filename;
    std::string uuid;
    bool server_compatible = true;
    bool client_only = false;
};

struct ServerPlayer {
    std::string name;
    std::string uuid;
    std::string ping_str;
    int ping_ms = 0;
};

struct ServerMetrics {
    // Metrics are unknown until a local server probe or authoritative backend
    // response supplies them. The UI must not present the defaults as live data.
    bool valid = false;
    float cpu_percent = 0.0f;
    float ram_percent = 0.0f;
    int ram_mb = 0;
    int players_online = 0;
    float tps = 0.0f;
};

struct ServerConsoleEntry {
    std::string timestamp;
    std::string message;
};

struct ServerCreateOptions {
    std::string name;
    ServerSoftware software = ServerSoftware::Vanilla;
    std::string minecraft_version;
    std::string forge_version;
    std::string fabric_loader_version;
    int allocated_ram_mb = 4096;
    int max_players = 20;
    std::string linked_profile_id;
    std::vector<std::string> mod_list;
    bool create_from_profile = false;
};

}  // namespace aml::server