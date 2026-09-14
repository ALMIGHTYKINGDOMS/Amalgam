#include "server_providers.h"

#include "json.h"
#include "net.h"

#include <algorithm>

namespace aml::server_providers {

namespace {

constexpr const char* kFillBase = "https://fill.papermc.io/v3/projects/";
constexpr const char* kPurpurBase = "https://api.purpurmc.org/v2/purpur";
constexpr const char* kFabricBase = "https://meta.fabricmc.net/v2/versions";
constexpr const char* kQuiltBase = "https://meta.quiltmc.org/v3/versions";

bool get_json(const std::string& url, Json& out, std::string* err) {
    std::vector<uint8_t> body;
    std::string fetch_err;
    if (!net::get(net::to_wide(url), body, &fetch_err)) {
        if (err) *err = fetch_err.empty() ? "Request failed" : fetch_err;
        return false;
    }
    std::string text(reinterpret_cast<const char*>(body.data()), body.size());
    std::string parse_err;
    out = Json::parse(text, &parse_err);
    if (!parse_err.empty()) {
        if (err) *err = "Invalid provider response: " + parse_err;
        return false;
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// URL builders
// ---------------------------------------------------------------------------

std::string fill_versions_url(const std::string& project) {
    return std::string(kFillBase) + project;
}

std::string fill_builds_url(const std::string& project, const std::string& version) {
    return fill_versions_url(project) + "/versions/" + version + "/builds";
}

std::string fill_download_url(const std::string& project, const std::string& version,
                              int build, const std::string& download_name) {
    return fill_versions_url(project) + "/versions/" + version + "/builds/" +
           std::to_string(build) + "/downloads/" + download_name;
}

std::string paper_versions_url() { return fill_versions_url("paper"); }

std::string paper_builds_url(const std::string& version) {
    return fill_builds_url("paper", version);
}

std::string paper_download_url(const std::string& version, int build,
                               const std::string& download_name) {
    return fill_download_url("paper", version, build, download_name);
}

std::string folia_versions_url() { return fill_versions_url("folia"); }

std::string folia_builds_url(const std::string& version) {
    return fill_builds_url("folia", version);
}

std::string folia_download_url(const std::string& version, int build,
                               const std::string& download_name) {
    return fill_download_url("folia", version, build, download_name);
}

std::string velocity_versions_url() { return fill_versions_url("velocity"); }

std::string velocity_builds_url(const std::string& version) {
    return fill_builds_url("velocity", version);
}

std::string velocity_download_url(const std::string& version, int build,
                                  const std::string& download_name) {
    return fill_download_url("velocity", version, build, download_name);
}

std::string purpur_versions_url() { return kPurpurBase; }

std::string purpur_builds_url(const std::string& version) {
    return std::string(kPurpurBase) + "/" + version;
}

std::string purpur_download_url(const std::string& version, const std::string& build) {
    return std::string(kPurpurBase) + "/" + version + "/" + build + "/download";
}

std::string fabric_game_versions_url() { return std::string(kFabricBase) + "/game"; }

std::string fabric_loader_versions_url() { return std::string(kFabricBase) + "/loader"; }

std::string fabric_installer_versions_url() { return std::string(kFabricBase) + "/installer"; }

std::string fabric_server_jar_url(const std::string& game, const std::string& loader,
                                  const std::string& installer) {
    return std::string(kFabricBase) + "/loader/" + game + "/" + loader + "/" + installer +
           "/server/jar";
}

std::string quilt_game_versions_url() { return std::string(kQuiltBase) + "/game"; }

std::string quilt_loader_versions_url() { return std::string(kQuiltBase) + "/loader"; }

std::string quilt_server_jar_url(const std::string& game, const std::string& loader,
                                 const std::string& installer) {
    return std::string(kQuiltBase) + "/loader/" + game + "/" + loader + "/" + installer +
           "/server/jar";
}

bool vanilla_server_runtime(const std::string& game, std::string& url,
                            std::string& sha1, int64_t& size, std::string* err) {
    constexpr const char* manifest_url =
        "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json";
    url.clear();
    sha1.clear();
    size = -1;

    Json manifest;
    if (!get_json(manifest_url, manifest, err)) return false;
    const Json& versions = manifest.get("versions");
    std::string version_url;
    for (const auto& version : versions.items()) {
        if (version.get("id").as_str() == game) {
            version_url = version.get("url").as_str();
            break;
        }
    }
    if (version_url.empty()) {
        if (err) *err = "Minecraft version " + game + " is not in Mojang's manifest";
        return false;
    }

    Json details;
    if (!get_json(version_url, details, err)) return false;
    const Json& server = details.get("downloads").get("server");
    url = server.get("url").as_str();
    sha1 = server.get("sha1").as_str();
    size = server.get("size").as_int(-1);
    if (url.empty()) {
        if (err) *err = "Mojang does not publish a dedicated server jar for " + game;
        return false;
    }
    if (err) err->clear();
    return true;
}

// ---------------------------------------------------------------------------
// Parsers
// ---------------------------------------------------------------------------

bool parse_fill_versions(const Json& root, std::vector<std::string>& versions) {
    const Json* list = root.find("versions");
    if (!list || !list->isArray()) return false;
    versions.clear();
    for (const Json& v : list->items()) {
        if (v.is(Json::Type::Str)) versions.push_back(v.as_str());
    }
    return !versions.empty();
}

bool parse_paper_versions(const Json& root, std::vector<std::string>& versions) {
    return parse_fill_versions(root, versions);
}

bool parse_fill_builds(const Json& root, const std::string& project,
                       const std::string& version, std::vector<BuildInfo>& builds) {
    const Json* list = root.find("builds");
    if (!list || !list->isArray()) return false;
    builds.clear();
    for (const Json& b : list->items()) {
        BuildInfo info;
        info.build = static_cast<int>(b.get("build").as_int());
        info.version = version;
        info.channel = b.get("channel").as_str();
        info.built_at = b.get("time").as_str();
        info.stable = info.channel == "default";
        const Json& downloads = b.get("downloads");
        if (downloads.isObject()) {
            const Json& application = downloads.get("application");
            if (application.isObject()) {
                const std::string name = application.get("name").as_str();
                info.sha256 = application.get("sha256").as_str();
                info.download_url = fill_download_url(project, version, info.build, name);
            }
        }
        if (!info.download_url.empty()) builds.push_back(std::move(info));
    }
    return true;
}

bool parse_paper_builds(const Json& root, const std::string& version,
                        std::vector<BuildInfo>& builds) {
    return parse_fill_builds(root, "paper", version, builds);
}

bool parse_purpur_versions(const Json& root, std::vector<std::string>& versions) {
    const Json* list = root.find("versions");
    if (!list || !list->isArray()) return false;
    versions.clear();
    for (const Json& v : list->items()) {
        if (v.is(Json::Type::Str)) versions.push_back(v.as_str());
    }
    return !versions.empty();
}

bool parse_purpur_builds(const Json& root, const std::string& version,
                         std::vector<BuildInfo>& builds) {
    const Json* builds_obj = root.find("builds");
    if (!builds_obj || !builds_obj->isObject()) return false;
    const Json* all = builds_obj->find("all");
    if (!all || !all->isArray()) return false;
    builds.clear();
    for (const Json& b : all->items()) {
        if (!b.is(Json::Type::Str)) continue;
        BuildInfo info;
        info.build = 0;
        info.version = version;
        info.channel = "default";
        info.stable = true;
        const std::string build_id = b.as_str();
        info.download_url = purpur_download_url(version, build_id);
        builds.push_back(std::move(info));
    }
    return !builds.empty();
}

bool parse_fabric_game_versions(const Json& root, std::vector<GameVersion>& versions) {
    if (!root.isArray()) return false;
    versions.clear();
    for (const Json& v : root.items()) {
        GameVersion gv;
        gv.version = v.get("version").as_str();
        gv.stable = v.get("stable").as_bool();
        if (!gv.version.empty()) versions.push_back(std::move(gv));
    }
    return !versions.empty();
}

bool parse_fabric_loader_versions(const Json& root, std::vector<std::string>& versions) {
    if (!root.isArray()) return false;
    versions.clear();
    for (const Json& v : root.items()) {
        const Json& loader = v.get("loader");
        const std::string version = loader.get("version").as_str();
        if (!version.empty()) versions.push_back(version);
    }
    return !versions.empty();
}

bool parse_fabric_installer_versions(const Json& root, std::vector<GameVersion>& versions) {
    if (!root.isArray()) return false;
    versions.clear();
    for (const Json& v : root.items()) {
        GameVersion gv;
        gv.version = v.get("version").as_str();
        gv.stable = v.get("stable").as_bool();
        if (!gv.version.empty()) versions.push_back(std::move(gv));
    }
    return !versions.empty();
}

bool parse_quilt_game_versions(const Json& root, std::vector<GameVersion>& versions) {
    // Quilt meta mirrors the Fabric meta shape.
    return parse_fabric_game_versions(root, versions);
}

bool parse_quilt_loader_versions(const Json& root, std::vector<std::string>& versions) {
    return parse_fabric_loader_versions(root, versions);
}

bool parse_quilt_loader_installer(const Json& root, std::vector<QuiltLoaderPair>& pairs) {
    if (!root.isArray()) return false;
    pairs.clear();
    for (const Json& v : root.items()) {
        const Json& loader = v.get("loader");
        const Json& installer = v.get("installer");
        const std::string lv = loader.get("version").as_str();
        const std::string iv = installer.get("version").as_str();
        if (!lv.empty() && !iv.empty()) pairs.push_back({lv, iv});
    }
    return !pairs.empty();
}

// ---------------------------------------------------------------------------
// Network fetchers
// ---------------------------------------------------------------------------

bool paper_versions(std::vector<std::string>& out, std::string* err) {
    Json root;
    if (!get_json(paper_versions_url(), root, err)) return false;
    return parse_paper_versions(root, out);
}

bool paper_builds(const std::string& version, std::vector<BuildInfo>& out, std::string* err) {
    Json root;
    if (!get_json(paper_builds_url(version), root, err)) return false;
    return parse_paper_builds(root, version, out);
}

bool paper_latest_stable(const std::string& version, BuildInfo& out, std::string* err) {
    std::vector<BuildInfo> builds;
    if (!paper_builds(version, builds, err)) return false;
    // Builds are returned newest-first by the API; pick the first stable build.
    for (const auto& build : builds) {
        if (build.stable) {
            out = build;
            return true;
        }
    }
    return false;
}

bool folia_versions(std::vector<std::string>& out, std::string* err) {
    Json root;
    if (!get_json(folia_versions_url(), root, err)) return false;
    return parse_fill_versions(root, out);
}

bool folia_builds(const std::string& version, std::vector<BuildInfo>& out, std::string* err) {
    Json root;
    if (!get_json(folia_builds_url(version), root, err)) return false;
    return parse_fill_builds(root, "folia", version, out);
}

bool folia_latest_stable(const std::string& version, BuildInfo& out, std::string* err) {
    std::vector<BuildInfo> builds;
    if (!folia_builds(version, builds, err)) return false;
    for (const auto& build : builds) {
        if (build.stable) {
            out = build;
            return true;
        }
    }
    return false;
}

bool velocity_versions(std::vector<std::string>& out, std::string* err) {
    Json root;
    if (!get_json(velocity_versions_url(), root, err)) return false;
    return parse_fill_versions(root, out);
}

bool velocity_builds(const std::string& version, std::vector<BuildInfo>& out, std::string* err) {
    Json root;
    if (!get_json(velocity_builds_url(version), root, err)) return false;
    return parse_fill_builds(root, "velocity", version, out);
}

bool purpur_versions(std::vector<std::string>& out, std::string* err) {
    Json root;
    if (!get_json(purpur_versions_url(), root, err)) return false;
    return parse_purpur_versions(root, out);
}

bool purpur_builds(const std::string& version, std::vector<BuildInfo>& out, std::string* err) {
    Json root;
    if (!get_json(purpur_builds_url(version), root, err)) return false;
    return parse_purpur_builds(root, version, out);
}

bool fabric_game_versions(std::vector<GameVersion>& out, std::string* err) {
    Json root;
    if (!get_json(fabric_game_versions_url(), root, err)) return false;
    return parse_fabric_game_versions(root, out);
}

bool fabric_loader_versions(std::vector<std::string>& out, std::string* err) {
    Json root;
    if (!get_json(fabric_loader_versions_url(), root, err)) return false;
    return parse_fabric_loader_versions(root, out);
}

bool fabric_installer_versions(std::vector<GameVersion>& out, std::string* err) {
    Json root;
    if (!get_json(fabric_installer_versions_url(), root, err)) return false;
    return parse_fabric_installer_versions(root, out);
}

bool quilt_game_versions(std::vector<GameVersion>& out, std::string* err) {
    Json root;
    if (!get_json(quilt_game_versions_url(), root, err)) return false;
    return parse_quilt_game_versions(root, out);
}

bool quilt_loader_versions(std::vector<std::string>& out, std::string* err) {
    Json root;
    if (!get_json(quilt_loader_versions_url(), root, err)) return false;
    return parse_quilt_loader_versions(root, out);
}

bool quilt_loader_installer_pairs(std::vector<QuiltLoaderPair>& out, std::string* err) {
    Json root;
    if (!get_json(quilt_loader_versions_url(), root, err)) return false;
    return parse_quilt_loader_installer(root, out);
}

bool download_runtime(const std::string& url, const std::string& expected_sha256,
                      const std::wstring& out_path, std::string* err) {
    return net::download(net::to_wide(url), out_path, nullptr, err,
                         std::string(), -1, expected_sha256);
}

}  // namespace aml::server_providers
