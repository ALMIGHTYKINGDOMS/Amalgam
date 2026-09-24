#pragma once

#include "server_types.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace aml {

class Json;

namespace server_providers {

using Progress = std::function<bool(uint64_t done, uint64_t total)>;

// A single downloadable server build from a provider.
struct BuildInfo {
    int build = 0;
    std::string version;
    std::string download_url;
    std::string sha256;   // hex; empty when the provider does not publish one
    int64_t size = -1;    // exact byte count; -1 when the provider does not publish one
    std::string channel;  // "default" / "experimental" / ...
    bool stable = false;
    std::string built_at;
};

struct GameVersion {
    std::string version;
    bool stable = false;
};


// A published Forge/NeoForge build. Both providers publish a Maven metadata
// version string, but Forge spells the game version into it ("1.20.1-47.4.23")
// while NeoForge encodes it in the leading numbers ("21.1.66"); both are
// normalized into `game` here so the UI never has to parse provider syntax.
struct LoaderBuild {
    std::string game;
    std::string loader;
    std::string raw;
};

// The artifact a software installs from. The kind lives in server_types.h so
// the catalog and this resolver agree on one enum.
struct RuntimeArtifact {
    server::RuntimeKind kind = server::RuntimeKind::Manual;
    std::string url;
    std::string sha1;            // Mojang publishes SHA-1 for the vanilla server jar
    std::string sha256;          // empty when the provider publishes none
    int64_t size = -1;           // exact byte size when the provider publishes it
    std::string label;           // human-readable, e.g. "Paper 1.20.1 build 196"
    std::string resolved_version;// provider build the version selector maps to
    std::string manual_hint;     // where a manual runtime comes from

    // A loader whose server jar is only a launcher needs the vanilla server
    // beside it, and this is where that second download goes.
    std::string companion_url;
    std::string companion_sha1;
    int64_t companion_size = -1;
    std::string companion_name;

    // How this provider's installer has to be invoked. `{installer}` and
    // `{dir}` are filled in with the downloaded installer path and the server
    // directory, so each provider states its own command once.
    std::string installer_args;
};

// ---------------------------------------------------------------------------
// Fill API family (PaperMC hosts paper, folia and velocity on the same API)
// ---------------------------------------------------------------------------
std::string fill_versions_url(const std::string& project);   // .../v3/projects/<project>
std::string fill_builds_url(const std::string& project, const std::string& version);
std::string fill_download_url(const std::string& project, const std::string& version,
                              int build, const std::string& download_name);

// Pure URL builders (unit-testable, no network)
std::string paper_versions_url();                              // .../v3/projects/paper
std::string paper_builds_url(const std::string& version);      // .../v3/projects/paper/versions/<v>/builds
std::string paper_download_url(const std::string& version, int build,
                               const std::string& download_name);
std::string folia_versions_url();                              // .../v3/projects/folia
std::string folia_builds_url(const std::string& version);
std::string folia_download_url(const std::string& version, int build,
                               const std::string& download_name);
std::string velocity_versions_url();                           // .../v3/projects/velocity
std::string velocity_builds_url(const std::string& version);
std::string velocity_download_url(const std::string& version, int build,
                                  const std::string& download_name);
std::string purpur_versions_url();                             // .../v2/purpur
std::string purpur_builds_url(const std::string& version);     // .../v2/purpur/<v>
std::string purpur_download_url(const std::string& version, const std::string& build);
std::string fabric_game_versions_url();                        // .../v2/versions/game
std::string fabric_loader_versions_url();                      // .../v2/versions/loader
std::string fabric_installer_versions_url();                   // .../v2/versions/installer
std::string fabric_server_jar_url(const std::string& game, const std::string& loader,
                                  const std::string& installer);
std::string quilt_game_versions_url();                         // https://meta.quiltmc.org/v3/versions/game
std::string quilt_loaders_for_game_url(const std::string& game);   // .../loader/<game>
std::string quilt_installer_versions_url();                    // .../versions/installer
std::string quilt_installer_url(const std::string& installer); // maven.quiltmc.org quilt-installer
std::string fabric_loaders_for_game_url(const std::string& game);  // .../loader/<game>
std::string forge_metadata_url();                              // net.minecraftforge:forge
std::string neoforge_metadata_url();                           // net.neoforged:neoforge
std::string forge_installer_url(const std::string& raw);       // <base>/<raw>/forge-<raw>-installer.jar
std::string neoforge_installer_url(const std::string& loader); // <base>/<loader>/neoforge-<loader>-installer.jar
std::string bungeecord_jar_url();                              // Jenkins last successful build
std::string bedrock_download_links_url();                      // official Mojang download index

// Resolve the official Mojang vanilla dedicated-server artifact for a game
// version. The manifest is fetched at use time, so newly released versions do
// not require a launcher update.
bool vanilla_server_runtime(const std::string& game, std::string& url,
                            std::string& sha1, int64_t& size, std::string* err);

// ---------------------------------------------------------------------------
// Pure response parsers (unit-testable against fixture JSON)
// ---------------------------------------------------------------------------
bool parse_fill_versions(const Json& root, std::vector<std::string>& versions);
bool parse_fill_builds(const Json& root, const std::string& project,
                       const std::string& version, std::vector<BuildInfo>& builds);
bool parse_paper_versions(const Json& root, std::vector<std::string>& versions);
bool parse_paper_builds(const Json& root, const std::string& version,
                        std::vector<BuildInfo>& builds);
bool parse_purpur_versions(const Json& root, std::vector<std::string>& versions);
bool parse_purpur_builds(const Json& root, const std::string& version,
                         std::vector<BuildInfo>& builds);
bool parse_fabric_game_versions(const Json& root, std::vector<GameVersion>& versions);
bool parse_fabric_loader_versions(const Json& root, std::vector<std::string>& versions);
bool parse_fabric_installer_versions(const Json& root, std::vector<GameVersion>& versions);
bool parse_quilt_game_versions(const Json& root, std::vector<GameVersion>& versions);
// Quilt's installer list is a flat array of {version}; the per-game loader list
// nests each loader the same way Fabric's does, so parse_fabric_loader_versions
// reads both.
bool parse_quilt_installer_versions(const Json& root, std::vector<std::string>& versions);
bool parse_maven_versions(const std::string& xml, std::vector<std::string>& versions);
bool parse_bedrock_link(const Json& root, const std::string& download_type, std::string& url);
// "1.20.1-47.4.23" -> game 1.20.1 / loader 47.4.23; false when unstructured.
bool split_forge_version(const std::string& raw, std::string& game, std::string& loader);
// NeoForge loader versions carry the game version: 21.1.66 -> 1.21.1,
// 21.0.167 -> 1.21, 26.2.0.87 -> 1.26.2. Pre-release suffixes are rejected.
bool derive_neoforge_game(const std::string& loader, std::string& game);

// ---------------------------------------------------------------------------
// Network fetchers (thin net::get + parser)
// ---------------------------------------------------------------------------
bool paper_versions(std::vector<std::string>& out, std::string* err);
bool paper_builds(const std::string& version, std::vector<BuildInfo>& out, std::string* err);
bool paper_latest_stable(const std::string& version, BuildInfo& out, std::string* err);
bool folia_versions(std::vector<std::string>& out, std::string* err);
bool folia_builds(const std::string& version, std::vector<BuildInfo>& out, std::string* err);
bool folia_latest_stable(const std::string& version, BuildInfo& out, std::string* err);
bool velocity_versions(std::vector<std::string>& out, std::string* err);
bool velocity_builds(const std::string& version, std::vector<BuildInfo>& out, std::string* err);
bool purpur_versions(std::vector<std::string>& out, std::string* err);
bool purpur_builds(const std::string& version, std::vector<BuildInfo>& out, std::string* err);
bool fabric_game_versions(std::vector<GameVersion>& out, std::string* err);
bool fabric_loader_versions(std::vector<std::string>& out, std::string* err);
bool fabric_installer_versions(std::vector<GameVersion>& out, std::string* err);
bool quilt_game_versions(std::vector<GameVersion>& out, std::string* err);
bool quilt_loaders_for_game(const std::string& game, std::vector<std::string>& out,
                            std::string* err);
bool quilt_installer_versions(std::vector<std::string>& out, std::string* err);
bool vanilla_release_versions(std::vector<std::string>& out, std::string* err);
bool fabric_loaders_for_game(const std::string& game, std::vector<std::string>& out,
                             std::string* err);
bool forge_versions(std::vector<LoaderBuild>& out, std::string* err);
bool neoforge_versions(std::vector<LoaderBuild>& out, std::string* err);
bool velocity_latest_stable(const std::string& version, BuildInfo& out, std::string* err);
bool bedrock_server_zip(std::string& url, std::string* err);

// The game versions a piece of software actually publishes, newest first. An
// empty list with no error means the software has no version axis (Bedrock and
// BungeeCord ship a single current build).
bool software_versions(server::ServerSoftware software, std::vector<std::string>& out,
                       std::string* err);

// The artifact a software+version installs from. This is the single place that
// knows which provider publishes which software.
bool resolve_runtime(server::ServerSoftware software, const std::string& version,
                     RuntimeArtifact& out, std::string* err);

// The complete transfer contract passed to net::download. A provider may
// publish SHA-1, SHA-256, a byte count, any combination, or none.
struct RuntimeDownloadRequest {
    std::wstring url;
    std::wstring out_path;
    std::string expected_sha1;
    std::string expected_sha256;
    int64_t expected_size = -1;
};

// Build the full integrity-aware request in one place so a caller cannot
// accidentally retain only one of a provider's published digest/size fields.
RuntimeDownloadRequest runtime_download_request(const RuntimeArtifact& artifact,
                                                const std::wstring& out_path);

// Download and verify a server artifact with every value its provider
// published. Empty digest fields and a -1 size preserve the honest
// transport-only path for providers that do not publish integrity metadata.
bool download_runtime(const RuntimeArtifact& artifact, const std::wstring& out_path,
                      Progress progress, std::string* err);

}  // namespace server_providers
}  // namespace aml
