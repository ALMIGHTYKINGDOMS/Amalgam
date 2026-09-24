#include "server_providers.h"

#include "json.h"
#include "net.h"

#include <algorithm>
#include <cctype>

namespace aml::server_providers {

namespace {

constexpr const char* kFillBase = "https://fill.papermc.io/v3/projects/";
constexpr const char* kPurpurBase = "https://api.purpurmc.org/v2/purpur";
constexpr const char* kFabricBase = "https://meta.fabricmc.net/v2/versions";
constexpr const char* kQuiltBase = "https://meta.quiltmc.org/v3/versions";
constexpr const char* kForgeBase = "https://maven.minecraftforge.net/net/minecraftforge/forge";
constexpr const char* kNeoForgeBase =
    "https://maven.neoforged.net/releases/net/neoforged/neoforge";
constexpr const char* kBungeeCordJar =
    "https://ci.md-5.net/job/BungeeCord/lastSuccessfulBuild/artifact/bootstrap/target/"
    "BungeeCord.jar";
// Mojang's own download index. It is what minecraft.net's download page reads,
// so the Bedrock zip it names is the current official server build.
constexpr const char* kBedrockLinks =
    "https://net-secondary.web.minecraft-services.net/api/v1.0/download/links";

// Identifier, not a version: these providers publish a single current build.
constexpr const char* kLatestOnly = "latest";

bool all_digits_or_dots(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c)) && c != '.') return false;
    }
    return true;
}

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

std::string paper_versions_url() { return fill_versions_url("paper"); }

std::string paper_builds_url(const std::string& version) {
    return fill_builds_url("paper", version);
}

std::string folia_versions_url() { return fill_versions_url("folia"); }

std::string folia_builds_url(const std::string& version) {
    return fill_builds_url("folia", version);
}

std::string velocity_versions_url() { return fill_versions_url("velocity"); }

std::string velocity_builds_url(const std::string& version) {
    return fill_builds_url("velocity", version);
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

std::string quilt_loaders_for_game_url(const std::string& game) {
    return std::string(kQuiltBase) + "/loader/" + game;
}

std::string quilt_installer_versions_url() { return std::string(kQuiltBase) + "/installer"; }

// Quilt's own installer is what can build a Quilt server: the meta API never
// published a ready-made server launcher jar.
std::string quilt_installer_url(const std::string& installer) {
    return std::string("https://maven.quiltmc.org/repository/release/org/quiltmc/"
                       "quilt-installer/") +
           installer + "/quilt-installer-" + installer + ".jar";
}

std::string fabric_loaders_for_game_url(const std::string& game) {
    return std::string(kFabricBase) + "/loader/" + game;
}

std::string forge_metadata_url() { return std::string(kForgeBase) + "/maven-metadata.xml"; }

std::string neoforge_metadata_url() { return std::string(kNeoForgeBase) + "/maven-metadata.xml"; }

std::string forge_installer_url(const std::string& raw) {
    return std::string(kForgeBase) + "/" + raw + "/forge-" + raw + "-installer.jar";
}

std::string neoforge_installer_url(const std::string& loader) {
    return std::string(kNeoForgeBase) + "/" + loader + "/neoforge-" + loader +
           "-installer.jar";
}

std::string bungeecord_jar_url() { return kBungeeCordJar; }

std::string bedrock_download_links_url() { return kBedrockLinks; }

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

// fill.papermc.io groups a project's versions into families
// ({"1.21":["1.21.11","1.21.10",...]}), each listed newest-first and
// including pre-releases. A server can only be built for a release, so the
// -rc/-pre entries are dropped and the order they arrived in is kept.
bool parse_fill_versions(const Json& root, std::vector<std::string>& versions) {
    const Json* families = root.find("versions");
    if (!families || !families->isObject()) return false;
    versions.clear();
    for (const std::string& family : families->getMemberNames()) {
        for (const Json& entry : families->get(family).items()) {
            if (!entry.is(Json::Type::Str)) continue;
            const std::string id = entry.as_str();
            if (id.empty() || id.find('-') != std::string::npos) continue;
            if (std::find(versions.begin(), versions.end(), id) == versions.end())
                versions.push_back(id);
        }
    }
    return !versions.empty();
}

bool parse_paper_versions(const Json& root, std::vector<std::string>& versions) {
    return parse_fill_versions(root, versions);
}

// A builds response is a bare array of {id, time, channel, downloads}, where
// downloads maps an artifact role to its own file. "server:default" is the
// server jar PaperMC serves; the older "application" shape is no longer used.
bool parse_fill_builds(const Json& root, const std::string& project,
                       const std::string& version, std::vector<BuildInfo>& builds) {
    (void)project;
    if (!root.isArray()) return false;
    builds.clear();
    for (const Json& b : root.items()) {
        BuildInfo info;
        info.build = static_cast<int>(b.get("id").as_int());
        info.version = version;
        info.channel = b.get("channel").as_str();
        info.built_at = b.get("time").as_str();
        info.stable = info.channel == "STABLE";
        const Json& downloads = b.get("downloads");
        if (!downloads.isObject()) continue;
        const Json* server = downloads.find("server:default");
        if (!server) continue;
        info.download_url = server->get("url").as_str();
        info.sha256 = server->get("checksums").get("sha256").as_str();
        info.size = server->get("size").as_int(-1);
        if (!info.download_url.empty()) builds.push_back(std::move(info));
    }
    return !builds.empty();
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

bool parse_quilt_installer_versions(const Json& root, std::vector<std::string>& versions) {
    if (!root.isArray()) return false;
    versions.clear();
    for (const Json& v : root.items()) {
        const std::string version = v.get("version").as_str();
        if (!version.empty()) versions.push_back(version);
    }
    return !versions.empty();
}

bool parse_maven_versions(const std::string& xml, std::vector<std::string>& versions) {
    versions.clear();
    const std::string open = "<version>";
    const std::string close = "</version>";
    size_t pos = 0;
    while ((pos = xml.find(open, pos)) != std::string::npos) {
        const size_t start = pos + open.size();
        const size_t end = xml.find(close, start);
        if (end == std::string::npos) break;
        const std::string value = xml.substr(start, end - start);
        if (!value.empty()) versions.push_back(value);
        pos = end + close.size();
    }
    return !versions.empty();
}

bool split_forge_version(const std::string& raw, std::string& game, std::string& loader) {
    const size_t dash = raw.find('-');
    if (dash == std::string::npos || dash == 0 || dash + 1 >= raw.size()) return false;
    game = raw.substr(0, dash);
    loader = raw.substr(dash + 1);
    if (!all_digits_or_dots(game) || !all_digits_or_dots(loader)) return false;
    return true;
}

bool derive_neoforge_game(const std::string& loader, std::string& game) {
    // NeoForge's loader version starts with the Minecraft version it targets,
    // with the patch dropped when it is zero: 21.1.66 -> 1.21.1, 21.0.167 ->
    // 1.21, 26.2.0.87 -> 1.26.2. Suffixed entries are pre-releases.
    if (loader.find('-') != std::string::npos) return false;
    std::vector<std::string> parts;
    std::string current;
    for (char c : loader) {
        if (c == '.') {
            parts.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    parts.push_back(current);
    if (parts.size() < 2) return false;
    for (const std::string& part : parts) {
        if (part.empty() || !all_digits_or_dots(part)) return false;
    }
    game = "1." + parts[0];
    if (parts[1] != "0") game += "." + parts[1];
    return true;
}

bool parse_bedrock_link(const Json& root, const std::string& download_type, std::string& url) {
    const Json& links = root.get("result").get("links");
    if (!links.isArray()) return false;
    for (const Json& link : links.items()) {
        if (link.get("downloadType").as_str() != download_type) continue;
        url = link.get("downloadUrl").as_str();
        return !url.empty();
    }
    return false;
}

// ---------------------------------------------------------------------------
// Network fetchers
// ---------------------------------------------------------------------------

bool vanilla_release_versions(std::vector<std::string>& out, std::string* err) {
    Json manifest;
    if (!get_json("https://piston-meta.mojang.com/mc/game/version_manifest_v2.json",
                  manifest, err))
        return false;
    const Json& versions = manifest.get("versions");
    if (!versions.isArray()) {
        if (err) *err = "Mojang's version manifest had no versions";
        return false;
    }
    out.clear();
    for (const Json& version : versions.items()) {
        if (version.get("type").as_str() != "release") continue;
        const std::string id = version.get("id").as_str();
        if (!id.empty()) out.push_back(id);
    }
    return !out.empty();
}

bool paper_versions(std::vector<std::string>& out, std::string* err) {
    Json root;
    if (!get_json(paper_versions_url(), root, err)) return false;
    return parse_paper_versions(root, out);
}

bool paper_builds(const std::string& version, std::vector<BuildInfo>& out, std::string* err) {
    Json root;
    if (!get_json(paper_builds_url(version), root, err)) return false;
    if (!parse_paper_builds(root, version, out)) {
        if (err) *err = "Paper published no server build for " + version;
        return false;
    }
    return true;
}

// Builds arrive newest-first, so the first stable one is the current release.
// A project with only beta builds for a version reports that instead of
// returning a silent failure the page would have to guess at.
bool newest_stable_build(const std::vector<BuildInfo>& builds, const std::string& project,
                         const std::string& version, BuildInfo& out, std::string* err) {
    for (const BuildInfo& build : builds) {
        if (build.stable) {
            out = build;
            return true;
        }
    }
    if (err) *err = project + " published no stable build for " + version;
    return false;
}

bool paper_latest_stable(const std::string& version, BuildInfo& out, std::string* err) {
    std::vector<BuildInfo> builds;
    if (!paper_builds(version, builds, err)) return false;
    return newest_stable_build(builds, "Paper", version, out, err);
}

bool folia_versions(std::vector<std::string>& out, std::string* err) {
    Json root;
    if (!get_json(folia_versions_url(), root, err)) return false;
    return parse_fill_versions(root, out);
}

bool folia_builds(const std::string& version, std::vector<BuildInfo>& out, std::string* err) {
    Json root;
    if (!get_json(folia_builds_url(version), root, err)) return false;
    if (!parse_fill_builds(root, "folia", version, out)) {
        if (err) *err = "Folia published no server build for " + version;
        return false;
    }
    return true;
}

bool folia_latest_stable(const std::string& version, BuildInfo& out, std::string* err) {
    std::vector<BuildInfo> builds;
    if (!folia_builds(version, builds, err)) return false;
    return newest_stable_build(builds, "Folia", version, out, err);
}

bool velocity_versions(std::vector<std::string>& out, std::string* err) {
    Json root;
    if (!get_json(velocity_versions_url(), root, err)) return false;
    return parse_fill_versions(root, out);
}

bool velocity_builds(const std::string& version, std::vector<BuildInfo>& out, std::string* err) {
    Json root;
    if (!get_json(velocity_builds_url(version), root, err)) return false;
    if (!parse_fill_builds(root, "velocity", version, out)) {
        if (err) *err = "Velocity published no server build for " + version;
        return false;
    }
    return true;
}

bool purpur_versions(std::vector<std::string>& out, std::string* err) {
    Json root;
    if (!get_json(purpur_versions_url(), root, err)) return false;
    return parse_purpur_versions(root, out);
}

bool purpur_builds(const std::string& version, std::vector<BuildInfo>& out, std::string* err) {
    Json root;
    if (!get_json(purpur_builds_url(version), root, err)) return false;
    if (!parse_purpur_builds(root, version, out)) {
        if (err) *err = "Purpur published no build for " + version;
        return false;
    }
    return true;
}

namespace {

// A loader list mixes beta and release builds; a server target wants the newest
// release, falling back to the first entry when a version has only betas.
std::string newest_release_loader(const std::vector<std::string>& loaders) {
    for (const std::string& loader : loaders) {
        if (loader.find('-') == std::string::npos) return loader;
    }
    return loaders.empty() ? std::string() : loaders.front();
}

}  // namespace

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

bool quilt_loaders_for_game(const std::string& game, std::vector<std::string>& out,
                           std::string* err) {
    Json root;
    if (!get_json(quilt_loaders_for_game_url(game), root, err)) return false;
    if (!parse_fabric_loader_versions(root, out)) {
        if (err) *err = "Quilt published no loader for " + game;
        return false;
    }
    return true;
}

bool quilt_installer_versions(std::vector<std::string>& out, std::string* err) {
    Json root;
    if (!get_json(quilt_installer_versions_url(), root, err)) return false;
    if (!parse_quilt_installer_versions(root, out)) {
        if (err) *err = "Quilt published no installer build";
        return false;
    }
    return true;
}

bool fabric_loaders_for_game(const std::string& game, std::vector<std::string>& out,
                             std::string* err) {
    Json root;
    if (!get_json(fabric_loaders_for_game_url(game), root, err)) return false;
    if (!parse_fabric_loader_versions(root, out)) {
        if (err) *err = "Fabric published no loader for " + game;
        return false;
    }
    return true;
}

bool forge_versions(std::vector<LoaderBuild>& out, std::string* err) {
    std::vector<uint8_t> body;
    std::string fetch_err;
    if (!net::get(net::to_wide(forge_metadata_url()), body, &fetch_err)) {
        if (err) *err = fetch_err.empty() ? "Forge metadata request failed" : fetch_err;
        return false;
    }
    const std::string xml(reinterpret_cast<const char*>(body.data()), body.size());
    std::vector<std::string> raw;
    if (!parse_maven_versions(xml, raw)) {
        if (err) *err = "Forge published no versions";
        return false;
    }
    out.clear();
    for (const std::string& entry : raw) {
        LoaderBuild build;
        if (!split_forge_version(entry, build.game, build.loader)) continue;
        build.raw = entry;
        out.push_back(std::move(build));
    }
    return !out.empty();
}

bool neoforge_versions(std::vector<LoaderBuild>& out, std::string* err) {
    std::vector<uint8_t> body;
    std::string fetch_err;
    if (!net::get(net::to_wide(neoforge_metadata_url()), body, &fetch_err)) {
        if (err) *err = fetch_err.empty() ? "NeoForge metadata request failed" : fetch_err;
        return false;
    }
    const std::string xml(reinterpret_cast<const char*>(body.data()), body.size());
    std::vector<std::string> raw;
    if (!parse_maven_versions(xml, raw)) {
        if (err) *err = "NeoForge published no versions";
        return false;
    }
    out.clear();
    for (const std::string& entry : raw) {
        LoaderBuild build;
        if (!derive_neoforge_game(entry, build.game)) continue;
        build.loader = entry;
        build.raw = entry;
        out.push_back(std::move(build));
    }
    return !out.empty();
}

bool velocity_latest_stable(const std::string& version, BuildInfo& out, std::string* err) {
    std::vector<BuildInfo> builds;
    if (!velocity_builds(version, builds, err)) return false;
    return newest_stable_build(builds, "Velocity", version, out, err);
}

bool bedrock_server_zip(std::string& url, std::string* err) {
    Json root;
    if (!get_json(bedrock_download_links_url(), root, err)) return false;
    // This launcher runs on Windows, so the Windows build is the one that can
    // actually be started here.
    if (!parse_bedrock_link(root, "serverBedrockWindows", url)) {
        if (err) *err = "Mojang published no Windows Bedrock server build";
        return false;
    }
    return true;
}

namespace {

// Newest-first, de-duplicated game versions from a loader build list.
std::vector<std::string> distinct_games(const std::vector<LoaderBuild>& builds) {
    std::vector<std::string> games;
    for (const LoaderBuild& build : builds) {
        if (std::find(games.begin(), games.end(), build.game) == games.end())
            games.push_back(build.game);
    }
    return games;
}

// Newest build for a game version. Both Maven indexes publish ascending, so the
// last match is the newest loader build for that game.
bool newest_loader_for_game(const std::vector<LoaderBuild>& builds, const std::string& game,
                            LoaderBuild& out) {
    bool found = false;
    for (const LoaderBuild& build : builds) {
        if (build.game != game) continue;
        out = build;
        found = true;
    }
    return found;
}

std::vector<std::string> stable_only(const std::vector<GameVersion>& versions) {
    std::vector<std::string> out;
    for (const GameVersion& v : versions) {
        if (v.stable) out.push_back(v.version);
    }
    return out;
}

}  // namespace

bool software_versions(server::ServerSoftware software, std::vector<std::string>& out,
                       std::string* err) {
    namespace sv = server;
    out.clear();
    switch (software) {
        case sv::ServerSoftware::Vanilla:
        case sv::ServerSoftware::Spigot:
            // Spigot publishes no version index; it builds against the same
            // Minecraft releases, so Mojang's manifest is the honest list.
            return vanilla_release_versions(out, err);
        case sv::ServerSoftware::Paper:
            return paper_versions(out, err);
        case sv::ServerSoftware::Folia:
            return folia_versions(out, err);
        case sv::ServerSoftware::Velocity:
            return velocity_versions(out, err);
        case sv::ServerSoftware::Purpur:
            return purpur_versions(out, err);
        case sv::ServerSoftware::Fabric: {
            std::vector<GameVersion> games;
            if (!fabric_game_versions(games, err)) return false;
            out = stable_only(games);
            if (out.empty()) {
                if (err) *err = "Fabric published no stable game versions";
                return false;
            }
            return true;
        }
        case sv::ServerSoftware::Quilt: {
            std::vector<GameVersion> games;
            if (!quilt_game_versions(games, err)) return false;
            out = stable_only(games);
            if (out.empty()) {
                if (err) *err = "Quilt published no stable game versions";
                return false;
            }
            return true;
        }
        case sv::ServerSoftware::Forge: {
            std::vector<LoaderBuild> builds;
            if (!forge_versions(builds, err)) return false;
            out = distinct_games(builds);
            return true;
        }
        case sv::ServerSoftware::NeoForge: {
            std::vector<LoaderBuild> builds;
            if (!neoforge_versions(builds, err)) return false;
            out = distinct_games(builds);
            return true;
        }
        case sv::ServerSoftware::BedrockDedicatedServer:
        case sv::ServerSoftware::BungeeCord:
            // These publish one current build rather than a version axis.
            out.push_back(kLatestOnly);
            return true;
    }
    if (err) *err = "No version source for this software";
    return false;
}

bool resolve_runtime(server::ServerSoftware software, const std::string& version,
                     RuntimeArtifact& out, std::string* err) {
    namespace sv = server;
    out = RuntimeArtifact();
    switch (software) {
        case sv::ServerSoftware::Vanilla: {
            std::string sha1;
            int64_t size = -1;
            if (!vanilla_server_runtime(version, out.url, sha1, size, err)) return false;
            out.kind = server::RuntimeKind::Jar;
            out.sha1 = sha1;
            out.size = size;
            out.label = "Vanilla server " + version;
            out.resolved_version = version;
            return true;
        }
        case sv::ServerSoftware::Paper: {
            BuildInfo build;
            if (!paper_latest_stable(version, build, err)) return false;
            out.kind = server::RuntimeKind::Jar;
            out.url = build.download_url;
            out.sha256 = build.sha256;
            out.size = build.size;
            out.resolved_version = std::to_string(build.build);
            out.label = "Paper " + version + " build " + out.resolved_version;
            return true;
        }
        case sv::ServerSoftware::Folia: {
            BuildInfo build;
            if (!folia_latest_stable(version, build, err)) return false;
            out.kind = server::RuntimeKind::Jar;
            out.url = build.download_url;
            out.sha256 = build.sha256;
            out.size = build.size;
            out.resolved_version = std::to_string(build.build);
            out.label = "Folia " + version + " build " + out.resolved_version;
            return true;
        }
        case sv::ServerSoftware::Velocity: {
            BuildInfo build;
            if (!velocity_latest_stable(version, build, err)) return false;
            out.kind = server::RuntimeKind::Jar;
            out.url = build.download_url;
            out.sha256 = build.sha256;
            out.size = build.size;
            out.resolved_version = std::to_string(build.build);
            out.label = "Velocity " + version + " build " + out.resolved_version;
            return true;
        }
        case sv::ServerSoftware::Purpur: {
            std::vector<BuildInfo> builds;
            if (!purpur_builds(version, builds, err)) return false;
            if (builds.empty()) {
                if (err) *err = "Purpur published no build for " + version;
                return false;
            }
            out.kind = server::RuntimeKind::Jar;
            // Purpur lists builds oldest-first; the last one is current.
            out.url = builds.back().download_url;
            out.label = "Purpur " + version;
            return true;
        }
        case sv::ServerSoftware::Fabric: {
            std::vector<std::string> loaders;
            std::vector<GameVersion> installers;
            if (!fabric_loaders_for_game(version, loaders, err)) return false;
            if (!fabric_installer_versions(installers, err)) return false;
            const std::string loader = newest_release_loader(loaders);
            if (loader.empty() || installers.empty()) {
                if (err) *err = "Fabric published no loader for " + version;
                return false;
            }
            // Fabric's server jar is a launcher that expects the vanilla server
            // beside it, so the install is two files, not one.
            std::string companion_sha1;
            int64_t companion_size = -1;
            if (!vanilla_server_runtime(version, out.companion_url, companion_sha1,
                                        companion_size, err))
                return false;
            out.companion_sha1 = companion_sha1;
            out.companion_size = companion_size;
            out.companion_name = "server.jar";
            out.kind = server::RuntimeKind::LoaderJar;
            out.url = fabric_server_jar_url(version, loader, installers.front().version);
            out.resolved_version = loader;
            out.label = "Fabric " + version + " loader " + loader;
            return true;
        }
        case sv::ServerSoftware::Quilt: {
            std::vector<std::string> loaders;
            std::vector<std::string> installers;
            if (!quilt_loaders_for_game(version, loaders, err)) return false;
            if (!quilt_installer_versions(installers, err)) return false;
            const std::string loader = newest_release_loader(loaders);
            if (loader.empty() || installers.empty()) {
                if (err) *err = "Quilt published no loader for " + version;
                return false;
            }
            // Quilt ships no server launcher, so its own installer builds the
            // server (and downloads the vanilla jar it wraps).
            out.kind = server::RuntimeKind::Installer;
            out.url = quilt_installer_url(installers.front());
            out.resolved_version = loader;
            out.label = "Quilt " + version + " loader " + loader;
            out.installer_args = "-jar {installer} install server " + version + " " + loader +
                                 " --install-dir={dir} --download-server";
            return true;
        }
        case sv::ServerSoftware::Forge: {
            std::vector<LoaderBuild> builds;
            if (!forge_versions(builds, err)) return false;
            LoaderBuild build;
            if (!newest_loader_for_game(builds, version, build)) {
                if (err) *err = "Forge publishes no build for Minecraft " + version;
                return false;
            }
            out.kind = server::RuntimeKind::Installer;
            out.url = forge_installer_url(build.raw);
            out.resolved_version = build.loader;
            out.label = "Forge " + build.raw;
            out.installer_args = "-jar {installer} --installServer {dir}";
            return true;
        }
        case sv::ServerSoftware::NeoForge: {
            std::vector<LoaderBuild> builds;
            if (!neoforge_versions(builds, err)) return false;
            LoaderBuild build;
            if (!newest_loader_for_game(builds, version, build)) {
                if (err) *err = "NeoForge publishes no build for Minecraft " + version;
                return false;
            }
            out.kind = server::RuntimeKind::Installer;
            out.url = neoforge_installer_url(build.loader);
            out.resolved_version = build.loader;
            out.label = "NeoForge " + build.loader;
            out.installer_args = "-jar {installer} --installServer {dir}";
            return true;
        }
        case sv::ServerSoftware::BedrockDedicatedServer: {
            if (!bedrock_server_zip(out.url, err)) return false;
            out.kind = server::RuntimeKind::Archive;
            out.label = "Bedrock Dedicated Server";
            return true;
        }
        case sv::ServerSoftware::BungeeCord: {
            out.kind = server::RuntimeKind::Jar;
            out.url = bungeecord_jar_url();
            out.label = "BungeeCord latest successful build";
            out.resolved_version = kLatestOnly;
            return true;
        }
        case sv::ServerSoftware::Spigot:
            out.kind = server::RuntimeKind::Manual;
            out.manual_hint =
                "Spigot is built from source with BuildTools and is not published as a "
                "download. Build or download SpigotServer.jar yourself and place it in "
                "this server's folder as server.jar.";
            return true;
    }
    if (err) *err = "No runtime source for this software";
    return false;
}

RuntimeDownloadRequest runtime_download_request(const RuntimeArtifact& artifact,
                                                const std::wstring& out_path) {
    RuntimeDownloadRequest request;
    request.url = net::to_wide(artifact.url);
    request.out_path = out_path;
    request.expected_sha1 = artifact.sha1;
    request.expected_sha256 = artifact.sha256;
    request.expected_size = artifact.size;
    return request;
}

bool download_runtime(const RuntimeArtifact& artifact, const std::wstring& out_path,
                      Progress progress, std::string* err) {
    const RuntimeDownloadRequest request = runtime_download_request(artifact, out_path);
    return net::download(request.url, request.out_path, progress, err, request.expected_sha1,
                         request.expected_size, request.expected_sha256);
}

}  // namespace aml::server_providers
