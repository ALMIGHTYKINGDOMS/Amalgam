#include "server_providers.h"

#include "json.h"

#include <cassert>
#include <string>
#include <vector>

namespace {

using aml::server_providers::BuildInfo;
using aml::server_providers::GameVersion;

bool test_urls() {
    assert(aml::server_providers::paper_versions_url() ==
           "https://fill.papermc.io/v3/projects/paper");
    assert(aml::server_providers::paper_builds_url("1.21.4") ==
           "https://fill.papermc.io/v3/projects/paper/versions/1.21.4/builds");
    assert(aml::server_providers::purpur_versions_url() ==
           "https://api.purpurmc.org/v2/purpur");
    assert(aml::server_providers::purpur_download_url("1.21.4", "2382") ==
           "https://api.purpurmc.org/v2/purpur/1.21.4/2382/download");
    assert(aml::server_providers::fabric_game_versions_url() ==
           "https://meta.fabricmc.net/v2/versions/game");
    assert(aml::server_providers::fabric_loader_versions_url() ==
           "https://meta.fabricmc.net/v2/versions/loader");
    assert(aml::server_providers::fabric_installer_versions_url() ==
           "https://meta.fabricmc.net/v2/versions/installer");
    assert(aml::server_providers::fabric_server_jar_url("1.21.4", "0.16.9", "1.0.1") ==
           "https://meta.fabricmc.net/v2/versions/loader/1.21.4/0.16.9/1.0.1/server/jar");
    assert(aml::server_providers::folia_versions_url() ==
           "https://fill.papermc.io/v3/projects/folia");
    assert(aml::server_providers::folia_builds_url("1.21.4") ==
           "https://fill.papermc.io/v3/projects/folia/versions/1.21.4/builds");
    assert(aml::server_providers::velocity_versions_url() ==
           "https://fill.papermc.io/v3/projects/velocity");
    assert(aml::server_providers::quilt_game_versions_url() ==
           "https://meta.quiltmc.org/v3/versions/game");
    assert(aml::server_providers::quilt_loaders_for_game_url("1.21.4") ==
           "https://meta.quiltmc.org/v3/versions/loader/1.21.4");
    assert(aml::server_providers::quilt_installer_versions_url() ==
           "https://meta.quiltmc.org/v3/versions/installer");
    assert(aml::server_providers::quilt_installer_url("0.15.1") ==
           "https://maven.quiltmc.org/repository/release/org/quiltmc/quilt-installer/0.15.1/"
           "quilt-installer-0.15.1.jar");
    assert(aml::server_providers::bungeecord_jar_url().find("BungeeCord.jar") !=
           std::string::npos);
    return true;
}

// fill.papermc.io answers a project with versions grouped into families, and a
// builds request with a bare array. Both were previously read with the wrong
// shape, which made Paper, Folia and Velocity impossible to install.
bool test_fill_versions() {
    std::string err;
    auto root = aml::Json::parse(
        R"({"project":{"id":"paper"},"versions":{
             "1.21":["1.21.11","1.21.11-rc3","1.21.10","1.21.1"],
             "1.20":["1.20.6","1.20.1"]}})", &err);
    assert(err.empty());
    std::vector<std::string> versions;
    assert(aml::server_providers::parse_paper_versions(root, versions));
    // Pre-releases are not server targets, and each id appears once.
    assert(versions.size() == 5);
    assert(versions[0] == "1.21.11" && versions[1] == "1.21.10" && versions[4] == "1.20.1");
    for (const std::string& version : versions)
        assert(version.find('-') == std::string::npos);

    // The older application-shaped response is not what the API sends, so it
    // must not be silently accepted as an empty success.
    auto stale = aml::Json::parse("{\"versions\":[\"1.21.4\"]}", &err);
    assert(err.empty());
    assert(!aml::server_providers::parse_paper_versions(stale, versions));
    return true;
}

bool test_paper_builds() {
    std::string err;
    auto root = aml::Json::parse(
        R"([
            {"id":125,"time":"2024-12-03T00:00:00Z","channel":"STABLE","commits":[],
             "downloads":{
               "server:mojang":{"name":"paper-1.21.4-125-mojang.jar",
                 "checksums":{"sha256":"aaa"},"size":1,"url":"https://fill/aaa"},
               "server:default":{"name":"paper-1.21.4-125.jar",
                 "checksums":{"sha256":"deadbeef"},"size":2,"url":"https://fill/deadbeef"}}},
            {"id":124,"time":"2024-12-02T00:00:00Z","channel":"BETA","commits":[],
             "downloads":{"server:default":{"name":"paper-1.21.4-124.jar",
                 "checksums":{"sha256":"cafebabe"},"size":3,"url":"https://fill/cafebabe"}}}
        ])", &err);
    assert(err.empty());
    std::vector<BuildInfo> builds;
    assert(aml::server_providers::parse_paper_builds(root, "1.21.4", builds));
    assert(builds.size() == 2);
    assert(builds[0].build == 125 && builds[0].stable);
    assert(builds[0].sha256 == "deadbeef");
    assert(builds[0].size == 2);
    assert(builds[0].download_url == "https://fill/deadbeef");
    assert(builds[1].build == 124 && !builds[1].stable && builds[1].size == 3);
    return true;
}

bool test_purpur() {
    std::string err;
    auto versions_root = aml::Json::parse(
        "{\"project\":\"purpur\",\"versions\":[\"1.21.4\",\"1.21.3\"]}", &err);
    assert(err.empty());
    std::vector<std::string> versions;
    assert(aml::server_providers::parse_purpur_versions(versions_root, versions));
    assert(versions.size() == 2 && versions[0] == "1.21.4");

    auto builds_root = aml::Json::parse(
        "{\"project\":\"purpur\",\"version\":\"1.21.4\","
        "\"builds\":{\"latest\":\"2382\",\"all\":[\"2382\",\"2381\",\"2380\"]}}", &err);
    assert(err.empty());
    std::vector<BuildInfo> builds;
    assert(aml::server_providers::parse_purpur_builds(builds_root, "1.21.4", builds));
    assert(builds.size() == 3);
    assert(builds[0].download_url ==
           "https://api.purpurmc.org/v2/purpur/1.21.4/2382/download");
    return true;
}

bool test_fabric() {
    std::string err;
    auto game_root = aml::Json::parse(
        "[{\"version\":\"1.21.4\",\"stable\":true},{\"version\":\"25w03a\",\"stable\":false}]",
        &err);
    assert(err.empty());
    std::vector<GameVersion> games;
    assert(aml::server_providers::parse_fabric_game_versions(game_root, games));
    assert(games.size() == 2);
    assert(games[0].version == "1.21.4" && games[0].stable);
    assert(games[1].version == "25w03a" && !games[1].stable);

    auto loader_root = aml::Json::parse(
        "[{\"loader\":{\"version\":\"0.16.9\",\"stable\":true}},"
        "{\"loader\":{\"version\":\"0.16.8\",\"stable\":false}}]", &err);
    assert(err.empty());
    std::vector<std::string> loaders;
    assert(aml::server_providers::parse_fabric_loader_versions(loader_root, loaders));
    assert(loaders.size() == 2 && loaders[0] == "0.16.9" && loaders[1] == "0.16.8");

    auto installer_root = aml::Json::parse(
        "[{\"version\":\"1.0.1\",\"stable\":true},{\"version\":\"1.0.0\",\"stable\":true}]",
        &err);
    assert(err.empty());
    std::vector<GameVersion> installers;
    assert(aml::server_providers::parse_fabric_installer_versions(installer_root, installers));
    assert(installers.size() == 2 && installers[0].version == "1.0.1" && installers[0].stable);
    return true;
}

bool test_quilt() {
    std::string err;
    auto game_root = aml::Json::parse(
        "[{\"version\":\"1.21.4\",\"stable\":true},{\"version\":\"1.21.3\",\"stable\":true}]",
        &err);
    assert(err.empty());
    std::vector<GameVersion> games;
    assert(aml::server_providers::parse_quilt_game_versions(game_root, games));
    assert(games.size() == 2 && games[0].version == "1.21.4" && games[0].stable);

    // Quilt's per-game loader list nests each loader the way Fabric's does.
    auto loader_root = aml::Json::parse(
        "[{\"loader\":{\"version\":\"0.24.0\"},\"intermediary\":{\"version\":\"1.0\"}},"
        "{\"loader\":{\"version\":\"0.20.0-beta.9\"}}]", &err);
    assert(err.empty());
    std::vector<std::string> loaders;
    assert(aml::server_providers::parse_fabric_loader_versions(loader_root, loaders));
    assert(loaders.size() == 2 && loaders[0] == "0.24.0" && loaders[1] == "0.20.0-beta.9");

    // Quilt's installer list is flat, and it is what builds a Quilt server.
    auto installer_root = aml::Json::parse(
        "[{\"version\":\"0.15.1\",\"url\":\"https://maven/0.15.1.jar\"},"
        "{\"version\":\"0.15.0\"}]", &err);
    assert(err.empty());
    std::vector<std::string> installers;
    assert(aml::server_providers::parse_quilt_installer_versions(installer_root, installers));
    assert(installers.size() == 2 && installers[0] == "0.15.1" && installers[1] == "0.15.0");
    assert(!aml::server_providers::parse_quilt_installer_versions(
        aml::Json::parse("{}", &err), installers));
    return true;
}

bool test_folia_velocity() {
    std::string err;
    auto folia_root = aml::Json::parse(
        R"([{"id":55,"time":"2025-01-01T00:00:00Z","channel":"STABLE","commits":[],
             "downloads":{"server:default":{"name":"folia-1.21.4-55.jar",
               "checksums":{"sha256":"abc123"},"size":5,"url":"https://fill/folia-55"}}}])",
        &err);
    assert(err.empty());
    std::vector<BuildInfo> folia_builds;
    assert(aml::server_providers::parse_fill_builds(folia_root, "folia", "1.21.4", folia_builds));
    assert(folia_builds.size() == 1 && folia_builds[0].stable);
    assert(folia_builds[0].size == 5);
    assert(folia_builds[0].download_url == "https://fill/folia-55");

    auto velocity_root = aml::Json::parse(
        R"([{"id":472,"time":"2025-01-02T00:00:00Z","channel":"STABLE","commits":[],
             "downloads":{"server:default":{"name":"velocity-3.4.0-472.jar",
               "checksums":{"sha256":"def456"},"size":6,"url":"https://fill/velocity-472"}}}])",
        &err);
    assert(err.empty());
    std::vector<BuildInfo> velocity_builds;
    assert(aml::server_providers::parse_fill_builds(velocity_root, "velocity", "3.4.0",
                                                    velocity_builds));
    assert(velocity_builds.size() == 1 && velocity_builds[0].stable);
    assert(velocity_builds[0].size == 6);
    assert(velocity_builds[0].download_url == "https://fill/velocity-472");
    return true;
}

// Forge and NeoForge publish Maven metadata rather than JSON, and each spells
// the Minecraft version differently. The mapping is the only thing standing
// between the version selector and the right installer jar.
bool test_loader_metadata() {
    std::string err;
    auto maven = aml::Json::parse("{}", &err);
    (void)maven;

    assert(aml::server_providers::forge_metadata_url() ==
           "https://maven.minecraftforge.net/net/minecraftforge/forge/maven-metadata.xml");
    assert(aml::server_providers::neoforge_metadata_url() ==
           "https://maven.neoforged.net/releases/net/neoforged/neoforge/maven-metadata.xml");
    assert(aml::server_providers::forge_installer_url("1.20.1-47.4.23") ==
           "https://maven.minecraftforge.net/net/minecraftforge/forge/1.20.1-47.4.23/"
           "forge-1.20.1-47.4.23-installer.jar");
    assert(aml::server_providers::neoforge_installer_url("21.1.66") ==
           "https://maven.neoforged.net/releases/net/neoforged/neoforge/21.1.66/"
           "neoforge-21.1.66-installer.jar");
    assert(aml::server_providers::fabric_loaders_for_game_url("1.20.1") ==
           "https://meta.fabricmc.net/v2/versions/loader/1.20.1");
    assert(aml::server_providers::bedrock_download_links_url().find(
               "minecraft-services.net") != std::string::npos);

    std::vector<std::string> versions;
    assert(aml::server_providers::parse_maven_versions(
        "<?xml version=\"1.0\"?><metadata><versioning><versions>"
        "<version>1.20.1-47.4.23</version><version>1.21-51.0.33</version>"
        "</versions></versioning></metadata>",
        versions));
    assert(versions.size() == 2 && versions[0] == "1.20.1-47.4.23" &&
           versions[1] == "1.21-51.0.33");
    // No <version> tags at all is a provider failure, not an empty success.
    assert(!aml::server_providers::parse_maven_versions("<metadata/>", versions));

    std::string game, loader;
    assert(aml::server_providers::split_forge_version("1.20.1-47.4.23", game, loader));
    assert(game == "1.20.1" && loader == "47.4.23");
    assert(!aml::server_providers::split_forge_version("1.20.1", game, loader));
    assert(!aml::server_providers::split_forge_version("1.20.1-47.4.23-beta", game, loader));

    assert(aml::server_providers::derive_neoforge_game("21.1.66", game) && game == "1.21.1");
    assert(aml::server_providers::derive_neoforge_game("21.0.167", game) && game == "1.21");
    assert(aml::server_providers::derive_neoforge_game("26.2.0.87", game) && game == "1.26.2");
    assert(!aml::server_providers::derive_neoforge_game("26.3.0.1-beta", game));
    assert(!aml::server_providers::derive_neoforge_game("47.1.106", game) || game == "1.47.1");
    return true;
}

bool test_bedrock_link() {
    std::string err;
    auto root = aml::Json::parse(
        R"({"result":{"links":[
            {"downloadType":"serverBedrockLinux","downloadUrl":"https://x/bedrock-server-1.26.51.1-linux.zip"},
            {"downloadType":"serverBedrockWindows","downloadUrl":"https://www.minecraft.net/bedrockdedicatedserver/bin-win/bedrock-server-1.26.51.1.zip"}
        ]}})",
        &err);
    assert(err.empty());
    std::string url;
    assert(aml::server_providers::parse_bedrock_link(root, "serverBedrockWindows", url));
    assert(url == "https://www.minecraft.net/bedrockdedicatedserver/bin-win/bedrock-server-1.26.51.1.zip");
    assert(!aml::server_providers::parse_bedrock_link(root, "serverBedrockPreviewWindows", url));
    return true;
}

// The catalog and the resolver have to agree: every software the create dialog
// offers must name an artifact, and a manual one must say why it is manual
// instead of silently failing at install time.
bool test_resolve_runtime_shape() {
    using aml::server::ServerSoftware;
    using aml::server::RuntimeKind;
    assert(aml::server::server_software_kind(ServerSoftware::Forge) == RuntimeKind::Installer);
    assert(aml::server::server_software_kind(ServerSoftware::BedrockDedicatedServer) ==
           RuntimeKind::Archive);
    assert(aml::server::server_software_kind(ServerSoftware::Paper) == RuntimeKind::Jar);
    assert(aml::server::server_software_kind(ServerSoftware::Spigot) == RuntimeKind::Manual);

    aml::server_providers::RuntimeArtifact artifact;
    std::string err;
    assert(aml::server_providers::resolve_runtime(ServerSoftware::Spigot, "1.20.1", artifact, &err));
    assert(artifact.kind == RuntimeKind::Manual && !artifact.manual_hint.empty());
    assert(aml::server_providers::resolve_runtime(ServerSoftware::BungeeCord, "latest", artifact, &err));
    assert(artifact.kind == RuntimeKind::Jar && artifact.url.find("BungeeCord.jar") != std::string::npos);
    return true;
}

// The provider resolver may supply SHA-1, SHA-256, and an exact byte count.
// The transfer wrapper must carry all published values to net::download.
bool test_runtime_download_request() {
    aml::server_providers::RuntimeArtifact verified;
    verified.url = "https://example.invalid/runtime.jar";
    verified.sha1 = "0123456789abcdef";
    verified.sha256 = "abcdef0123456789";
    verified.size = 123456;
    const auto request = aml::server_providers::runtime_download_request(
        verified, L"C:\\runtime\\server.jar");
    assert(request.url == L"https://example.invalid/runtime.jar");
    assert(request.out_path == L"C:\\runtime\\server.jar");
    assert(request.expected_sha1 == verified.sha1);
    assert(request.expected_sha256 == verified.sha256);
    assert(request.expected_size == verified.size);

    aml::server_providers::RuntimeArtifact transport_only;
    transport_only.url = "https://example.invalid/transport-only.jar";
    const auto unverified = aml::server_providers::runtime_download_request(
        transport_only, L"C:\\runtime\\transport-only.jar");
    assert(unverified.expected_sha1.empty());
    assert(unverified.expected_sha256.empty());
    assert(unverified.expected_size == -1);
    return true;
}

}  // namespace

int main() {
    if (!test_urls() || !test_fill_versions() || !test_paper_builds() || !test_purpur() ||
        !test_fabric() || !test_quilt() || !test_folia_velocity() ||
        !test_loader_metadata() || !test_bedrock_link() || !test_resolve_runtime_shape() ||
        !test_runtime_download_request()) {
        return 1;
    }
    return 0;
}
