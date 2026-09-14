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
    assert(aml::server_providers::paper_download_url("1.21.4", 125, "paper-1.21.4-125.jar") ==
           "https://fill.papermc.io/v3/projects/paper/versions/1.21.4/builds/125/downloads/"
           "paper-1.21.4-125.jar");
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
    assert(aml::server_providers::velocity_download_url("3.4.0", 472, "velocity-3.4.0-472.jar") ==
           "https://fill.papermc.io/v3/projects/velocity/versions/3.4.0/builds/472/downloads/"
           "velocity-3.4.0-472.jar");
    assert(aml::server_providers::quilt_game_versions_url() ==
           "https://meta.quiltmc.org/v3/versions/game");
    assert(aml::server_providers::quilt_loader_versions_url() ==
           "https://meta.quiltmc.org/v3/versions/loader");
    assert(aml::server_providers::quilt_server_jar_url("1.21.4", "0.27.0", "0.8.0") ==
           "https://meta.quiltmc.org/v3/versions/loader/1.21.4/0.27.0/0.8.0/server/jar");
    return true;
}

bool test_paper_versions() {
    std::string err;
    auto root = aml::Json::parse(
        "{\"project_id\":\"paper\",\"versions\":[\"1.21.4\",\"1.21.3\",\"1.21.1\"]}", &err);
    assert(err.empty());
    std::vector<std::string> versions;
    assert(aml::server_providers::parse_paper_versions(root, versions));
    assert(versions.size() == 3 && versions[0] == "1.21.4" && versions[2] == "1.21.1");
    return true;
}

bool test_paper_builds() {
    std::string err;
    auto root = aml::Json::parse(
        R"({"version":"1.21.4","builds":[
            {"build":125,"time":"2024-12-03T00:00:00Z","channel":"default",
             "downloads":{"application":{"name":"paper-1.21.4-125.jar","sha256":"deadbeef"}}},
            {"build":124,"time":"2024-12-02T00:00:00Z","channel":"experimental",
             "downloads":{"application":{"name":"paper-1.21.4-124.jar","sha256":"cafebabe"}}}
        ]})", &err);
    assert(err.empty());
    std::vector<BuildInfo> builds;
    assert(aml::server_providers::parse_paper_builds(root, "1.21.4", builds));
    assert(builds.size() == 2);
    assert(builds[0].build == 125 && builds[0].stable);
    assert(builds[0].sha256 == "deadbeef");
    assert(builds[0].download_url.find("/builds/125/downloads/paper-1.21.4-125.jar") !=
           std::string::npos);
    assert(builds[1].build == 124 && !builds[1].stable);
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

    auto loader_root = aml::Json::parse(
        "[{\"loader\":{\"version\":\"0.27.0\",\"stable\":true}}]", &err);
    assert(err.empty());
    std::vector<std::string> loaders;
    assert(aml::server_providers::parse_quilt_loader_versions(loader_root, loaders));
    assert(loaders.size() == 1 && loaders[0] == "0.27.0");

    // Quilt's loader endpoint pairs each loader with its installer; both are
    // required to build the server jar URL.
    auto pair_root = aml::Json::parse(
        "[{\"loader\":{\"version\":\"0.27.0\"},\"installer\":{\"version\":\"0.8.0\"}},"
        "{\"loader\":{\"version\":\"0.26.0\"},\"installer\":{\"version\":\"0.7.1\"}}]", &err);
    assert(err.empty());
    std::vector<aml::server_providers::QuiltLoaderPair> pairs;
    assert(aml::server_providers::parse_quilt_loader_installer(pair_root, pairs));
    assert(pairs.size() == 2);
    assert(pairs[0].loader == "0.27.0" && pairs[0].installer == "0.8.0");
    assert(pairs[1].loader == "0.26.0" && pairs[1].installer == "0.7.1");

    // A malformed entry (missing installer) is skipped, not fatal.
    auto bad_root = aml::Json::parse(
        "[{\"loader\":{\"version\":\"0.27.0\"}}]", &err);
    assert(err.empty());
    std::vector<aml::server_providers::QuiltLoaderPair> bad;
    assert(!aml::server_providers::parse_quilt_loader_installer(bad_root, bad));
    assert(bad.empty());
    return true;
}

bool test_folia_velocity() {
    std::string err;
    auto folia_root = aml::Json::parse(
        R"({"project_id":"folia","builds":[
            {"build":55,"time":"2025-01-01T00:00:00Z","channel":"default",
             "downloads":{"application":{"name":"folia-1.21.4-55.jar","sha256":"abc123"}}}
        ]})", &err);
    assert(err.empty());
    std::vector<BuildInfo> folia_builds;
    assert(aml::server_providers::parse_fill_builds(folia_root, "folia", "1.21.4", folia_builds));
    assert(folia_builds.size() == 1 && folia_builds[0].stable);
    assert(folia_builds[0].download_url.find("projects/folia/") != std::string::npos);

    auto velocity_root = aml::Json::parse(
        R"({"project_id":"velocity","builds":[
            {"build":472,"time":"2025-01-02T00:00:00Z","channel":"default",
             "downloads":{"application":{"name":"velocity-3.4.0-472.jar","sha256":"def456"}}}
        ]})", &err);
    assert(err.empty());
    std::vector<BuildInfo> velocity_builds;
    assert(aml::server_providers::parse_fill_builds(velocity_root, "velocity", "3.4.0",
                                                    velocity_builds));
    assert(velocity_builds.size() == 1 && velocity_builds[0].stable);
    assert(velocity_builds[0].download_url.find("projects/velocity/") != std::string::npos);
    return true;
}

}  // namespace

int main() {
    if (!test_urls() || !test_paper_versions() || !test_paper_builds() || !test_purpur() ||
        !test_fabric() || !test_quilt() || !test_folia_velocity()) {
        return 1;
    }
    return 0;
}
