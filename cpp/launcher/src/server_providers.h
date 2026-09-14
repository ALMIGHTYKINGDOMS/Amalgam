#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace aml {

class Json;

namespace server_providers {

// A single downloadable server build from a provider.
struct BuildInfo {
    int build = 0;
    std::string version;
    std::string download_url;
    std::string sha256;   // hex; empty when the provider does not publish one
    std::string channel;  // "default" / "experimental" / ...
    bool stable = false;
    std::string built_at;
};

struct GameVersion {
    std::string version;
    bool stable = false;
};

// Quilt's /versions/loader response pairs each loader with its installer
// version in a single entry; both are required for the server jar URL.
struct QuiltLoaderPair {
    std::string loader;
    std::string installer;
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
std::string quilt_loader_versions_url();                       // https://meta.quiltmc.org/v3/versions/loader
std::string quilt_server_jar_url(const std::string& game, const std::string& loader,
                                 const std::string& installer);

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
bool parse_quilt_loader_versions(const Json& root, std::vector<std::string>& versions);
bool parse_quilt_loader_installer(const Json& root, std::vector<QuiltLoaderPair>& pairs);

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
bool quilt_loader_versions(std::vector<std::string>& out, std::string* err);
bool quilt_loader_installer_pairs(std::vector<QuiltLoaderPair>& out, std::string* err);

// Download and verify a server jar using the provider-published SHA-256 (when
// available) and a safe `.part`-style transfer handled by net::download.
bool download_runtime(const std::string& url, const std::string& expected_sha256,
                      const std::wstring& out_path, std::string* err);

}  // namespace server_providers
}  // namespace aml
