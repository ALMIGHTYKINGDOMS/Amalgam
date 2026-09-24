#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <cctype>
#include <filesystem>
#include <initializer_list>

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
    Velocity,
    // Appended so the values already written to servers.json keep their meaning.
    BungeeCord
};

// How a software's runtime reaches the disk. The installer step differs per
// kind, so the Servers page asks for the kind instead of re-deriving it from
// the software enum in more than one place.
enum class RuntimeKind {
    Jar,       // a runnable server jar, written to server.jar
    LoaderJar, // a loader launcher jar plus the vanilla server jar it wraps
    Installer, // an installer jar that builds the server in the directory
    Archive,   // a zip of prebuilt server files (Bedrock Dedicated Server)
    Manual,    // no automatable distribution (Spigot is built with BuildTools)
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

// The local-server name is also a Windows directory component and a supervisor
// key. Reject aliases instead of silently rewriting them, because a renamed
// folder would make the persisted display name and on-disk server disagree.
inline bool validate_server_name(const std::string& name, std::string* reason = nullptr) {
    if (reason) reason->clear();
    const auto fail = [reason](const char* message) {
        if (reason) *reason = message;
        return false;
    };
    if (name.empty()) return fail("Enter a server name.");

    const auto ascii_space = [](unsigned char value) { return std::isspace(value) != 0; };
    if (ascii_space(static_cast<unsigned char>(name.front())) ||
        ascii_space(static_cast<unsigned char>(name.back()))) {
        return fail("Server names cannot begin or end with whitespace.");
    }
    if (name == "." || name == "..") {
        return fail("A server name cannot be a folder alias.");
    }
    for (const unsigned char value : name) {
        if (value < 32 || value == 127) {
            return fail("Server names cannot contain control characters.");
        }
        switch (value) {
            case '\\': case '/': case ':': case '*': case '?': case '"':
            case '<': case '>': case '|':
                return fail("Server names cannot contain Windows path characters.");
            default:
                break;
        }
    }
    if (name.back() == '.' || name.back() == ' ') {
        return fail("Server names cannot end with a dot or space.");
    }

    // Windows reserves device names even with an extension (for example,
    // NUL.txt). Trim whitespace/dots immediately before an extension as well
    // so a visually similar device alias cannot become a managed directory.
    std::string base = name.substr(0, name.find('.'));
    while (!base.empty() && (base.back() == '.' || base.back() == ' ' ||
                             ascii_space(static_cast<unsigned char>(base.back())))) {
        base.pop_back();
    }
    for (char& value : base) {
        if (value >= 'A' && value <= 'Z') value = static_cast<char>(value - 'A' + 'a');
    }
    if (base == "con" || base == "prn" || base == "aux" || base == "nul" ||
        (base.size() == 4 && base.rfind("com", 0) == 0 &&
         base[3] >= '1' && base[3] <= '9') ||
        (base.size() == 4 && base.rfind("lpt", 0) == 0 &&
         base[3] >= '1' && base[3] <= '9')) {
        return fail("This is a Windows-reserved device name. Choose another server name.");
    }
    return true;
}

// One entry per offerable software. This table is the only place the launcher
// decides what the create dialog lists, so a software cannot appear in the UI
// without a runtime source behind it.
struct ServerSoftwareEntry {
    ServerSoftware software;
    const char* label;
    RuntimeKind kind;
    const char* note;  // shown under the selector when it is chosen
};

inline const std::vector<ServerSoftwareEntry>& software_catalog() {
    static const std::vector<ServerSoftwareEntry> kCatalog = {
        {ServerSoftware::Vanilla, "Vanilla", RuntimeKind::Jar,
         "Mojang's own server, downloaded and verified from the version manifest."},
        {ServerSoftware::Paper, "Paper", RuntimeKind::Jar,
         "High-performance server; the newest stable build for the version is used."},
        {ServerSoftware::Purpur, "Purpur", RuntimeKind::Jar,
         "Paper fork with extra gameplay settings; the newest build is used."},
        {ServerSoftware::Folia, "Folia", RuntimeKind::Jar,
         "Regionised Paper for very large player counts; not all plugins support it."},
        {ServerSoftware::Fabric, "Fabric", RuntimeKind::LoaderJar,
         "Lightweight mod loader; the launcher installs its launcher jar and the server it wraps."},
        {ServerSoftware::Quilt, "Quilt", RuntimeKind::Installer,
         "Fabric-compatible mod loader; its installer builds the server and its launch jar."},
        {ServerSoftware::Forge, "Forge", RuntimeKind::Installer,
         "The launcher runs Forge's installer, then starts the server it produces."},
        {ServerSoftware::NeoForge, "NeoForge", RuntimeKind::Installer,
         "The launcher runs NeoForge's installer, then starts the server it produces."},
        {ServerSoftware::Velocity, "Velocity", RuntimeKind::Jar,
         "Modern proxy for linked servers; join it instead of the backends."},
        {ServerSoftware::BungeeCord, "BungeeCord", RuntimeKind::Jar,
         "Classic proxy, distributed as its newest successful build."},
        {ServerSoftware::BedrockDedicatedServer, "Bedrock Dedicated Server",
         RuntimeKind::Archive,
         "Mojang's Windows Bedrock server, published only as a zip of its current build."},
        {ServerSoftware::Spigot, "Spigot", RuntimeKind::Manual,
         "Not distributed as a download: build it with BuildTools, then add the jar."},
    };
    return kCatalog;
}

inline const char* server_software_name(ServerSoftware s) {
    for (const ServerSoftwareEntry& entry : software_catalog()) {
        if (entry.software == s) return entry.label;
    }
    return "Unknown";
}

inline RuntimeKind server_software_kind(ServerSoftware s) {
    for (const ServerSoftwareEntry& entry : software_catalog()) {
        if (entry.software == s) return entry.kind;
    }
    return RuntimeKind::Manual;
}

inline bool is_modded_software(ServerSoftware s) {
    return s == ServerSoftware::Forge || s == ServerSoftware::NeoForge ||
           s == ServerSoftware::Fabric || s == ServerSoftware::Quilt;
}

// Proxies start without the `nogui` argument: they have no console window to
// suppress and reject arguments they do not define.
inline bool is_proxy_software(ServerSoftware s) {
    return s == ServerSoftware::Velocity || s == ServerSoftware::BungeeCord;
}

// Bedrock's server is a native executable unpacked from Mojang's zip rather
// than a Java program.
inline bool is_native_software(ServerSoftware s) {
    return s == ServerSoftware::BedrockDedicatedServer;
}

// A stage restored from disk is only a hint. The process the service supervises
// owns local run state, so an unsupervised Running stage has to read as Stopped
// instead of as a server that is actually up.
inline ServerStage reconciled_stage(const ServerConfig& server, bool supervised) {
    return (server.stage == ServerStage::Running && !supervised) ? ServerStage::Stopped
                                                                 : server.stage;
}

// The argument file a Forge/NeoForge install writes beside its libraries. Its
// presence is what "the loader is installed" means, for the start command and
// for the page's readiness display, so both ask this one function.
inline std::filesystem::path loader_args_path(const std::filesystem::path& server_dir,
                                              ServerSoftware software) {
    const std::filesystem::path root =
        server_dir / (software == ServerSoftware::NeoForge
                          ? L"libraries\\net\\neoforged\\neoforge"
                          : L"libraries\\net\\minecraftforge\\forge");
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) return {};
    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
        if (ec) break;
        const std::filesystem::path args = entry.path() / "win_args.txt";
        if (std::filesystem::exists(args, ec)) return args;
    }
    return {};
}

// Forge before 1.17 installs as one universal jar in the server root instead
// of an argument file.
inline std::filesystem::path loader_universal_jar(const std::filesystem::path& server_dir) {
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(server_dir, ec)) {
        if (ec) break;
        const std::string name = entry.path().filename().string();
        if (name.rfind("forge-", 0) != 0 || name.size() < 5) continue;
        if (name.find("installer") != std::string::npos) continue;
        if (name.find("-sources") != std::string::npos) continue;
        if (entry.path().extension() != ".jar") continue;
        return entry.path();
    }
    return {};
}

// The launcher jar a loader's own installer leaves behind, for loaders whose
// server jar is a wrapper rather than the game itself.
inline const char* loader_launcher_jar(ServerSoftware software) {
    switch (software) {
        case ServerSoftware::Fabric: return "fabric-server-launch.jar";
        case ServerSoftware::Quilt: return "quilt-server-launch.jar";
        default: return nullptr;
    }
}

// What launches this server, resolved from what is on disk. This is the one
// expression of "is this server installed", so the Start path and the page's
// readiness display can never disagree about it.
struct LaunchTarget {
    enum class Kind { Missing, NativeExecutable, ServerJar, LoaderArgs };
    Kind kind = Kind::Missing;
    std::filesystem::path path;  // executable, jar to run, or loader args file
};

inline LaunchTarget resolve_launch_target(const ServerConfig& server) {
    LaunchTarget target;
    if (server.server_directory.empty()) return target;
    const std::filesystem::path dir(server.server_directory);
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return target;

    if (is_native_software(server.software)) {
        const std::filesystem::path executable = dir / "bedrock_server.exe";
        if (std::filesystem::is_regular_file(executable, ec) && !ec) {
            target.kind = LaunchTarget::Kind::NativeExecutable;
            target.path = executable;
        }
        return target;
    }
    if (server.software == ServerSoftware::Forge ||
        server.software == ServerSoftware::NeoForge) {
        const std::filesystem::path args = loader_args_path(dir, server.software);
        if (!args.empty() && std::filesystem::is_regular_file(args, ec) && !ec) {
            target.kind = LaunchTarget::Kind::LoaderArgs;
            target.path = args;
            return target;
        }
        const std::filesystem::path universal = loader_universal_jar(dir);
        if (!universal.empty() && std::filesystem::is_regular_file(universal, ec) && !ec) {
            target.kind = LaunchTarget::Kind::ServerJar;
            target.path = universal;
        }
        return target;
    }
    if (const char* launcher = loader_launcher_jar(server.software)) {
        const std::filesystem::path launcher_jar = dir / launcher;
        if (std::filesystem::is_regular_file(launcher_jar, ec) && !ec) {
            target.kind = LaunchTarget::Kind::ServerJar;
            target.path = launcher_jar;
        }
        return target;
    }
    const std::filesystem::path jar = dir / "server.jar";
    if (std::filesystem::is_regular_file(jar, ec) && !ec) {
        target.kind = LaunchTarget::Kind::ServerJar;
        target.path = jar;
    }
    return target;
}

// Whether the files that let this server start are actually on disk. Asking
// this keeps a Ready badge honest after a manual delete, and lets the page
// offer the download instead of a Start that can only fail.
inline bool runtime_files_present(const ServerConfig& server) {
    return resolve_launch_target(server).kind != LaunchTarget::Kind::Missing;
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
    // A local process probe can provide CPU and memory without pretending it
    // knows Minecraft's internal TPS or player list.  Keep each signal
    // independently truth-labelled in the UI.
    bool cpu_valid = false;
    bool ram_valid = false;
    bool tps_valid = false;
    bool players_valid = false;
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
