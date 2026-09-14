#include "mods.h"

#include "json.h"
#include "net.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <string>

namespace aml::mods {

namespace {

struct Ownership {
    struct Record {
        std::string project;
        std::string source;
        std::string version_id;
        std::string sha1;
        std::set<std::string> required_by;
    };
    std::map<std::string, Record> files;
    std::map<std::string, std::set<std::string>> optional;
};

std::string to_str(const Json& j) { return j.as_str(); }

std::vector<std::string> str_arr(const Json& j) {
    std::vector<std::string> out;
    for (const auto& i : j.items()) out.push_back(i.as_str());
    return out;
}

std::string urlencode(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

std::string lower_copy(std::string value) {
    for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

bool safe_filename(const std::string& value) {
    if (value.empty() || value == "." || value == ".." || value.back() == '.' ||
        value.back() == ' ') return false;
    for (unsigned char c : value) {
        if (c < 0x20 || std::string("<>:\"/\\|?*").find(static_cast<char>(c)) != std::string::npos)
            return false;
    }
    return true;
}

const char* facet_str(Facet f) {
    switch (f) {
        case Facet::Fabric: return "fabric";
        case Facet::Forge: return "forge";
        case Facet::NeoForge: return "neoforge";
        case Facet::Quilt: return "quilt";
        case Facet::Bukkit: return "bukkit";
        case Facet::Spigot: return "spigot";
        case Facet::Paper: return "paper";
        case Facet::Modpack: return "modpack";
        case Facet::ResourcePack: return "resourcepack";
        case Facet::Shader: return "shader";
        case Facet::Datapack: return "datapack";
        case Facet::Optimization: return "optimization";
        case Facet::Weapons: return "weapons";
        case Facet::Armor: return "armor";
        case Facet::Cosmetic: return "cosmetic";
        case Facet::Magic: return "magic";
        case Facet::Technology: return "technology";
        case Facet::Adventure: return "adventure";
        case Facet::Storage: return "storage";
        case Facet::Food: return "food";
        case Facet::Utility: return "utility";
        case Facet::BedrockAddon: return "addons";
    }
    return "";
}

// CurseForge class ids (mc game id 432)
int cf_class_id(Facet f) {
    switch (f) {
        case Facet::BedrockAddon: return 4559; // Minecraft Bedrock Addons
        case Facet::Modpack: return 4471;
        case Facet::ResourcePack: return 12;
        case Facet::Shader: return 6552;
        case Facet::Datapack: return 6945;
        default: return 6;  // mc-mods
    }
}

// CurseForge modLoaderType
int cf_loader_id(const std::string& loader) {
    if (loader == "fabric") return 4;
    if (loader == "quilt") return 5;
    if (loader == "neoforge") return 6;
    if (loader == "forge") return 1;
    return 0;
}

ProjectType parse_type(const std::string& s) {
    if (s == "mod") return ProjectType::Mod;
    if (s == "modpack") return ProjectType::Modpack;
    if (s == "resourcepack") return ProjectType::ResourcePack;
    if (s == "shader") return ProjectType::Shader;
    if (s == "datapack") return ProjectType::Datapack;
    if (s == "plugin") return ProjectType::Plugin;
    return ProjectType::Unknown;
}

std::vector<std::wstring> json_headers(const std::string& token) {
    std::vector<std::wstring> h;
    h.push_back(L"Accept: application/json");
    h.push_back(L"User-Agent: AmalgamLauncher/1.0 (personal; contact: amalgam@local)");
    if (!token.empty()) h.push_back(L"Authorization: Bearer " + net::to_wide(token));
    return h;
}

bool parse_json_bytes(const std::vector<uint8_t>& bytes, Json& out, std::string* err) {
    std::string text(bytes.begin(), bytes.end());
    std::string local_error;
    std::string* output_error = err ? err : &local_error;
    output_error->clear();
    out = Json::parse(text, output_error);
    return output_error->empty();
}

bool direct_json_get(const std::string& url, const std::vector<std::wstring>& headers,
                     Json& out, std::string* err) {
    std::vector<uint8_t> bytes;
    if (!net::get_with_headers(net::to_wide(url), headers, bytes, err)) return false;
    return parse_json_bytes(bytes, out, err);
}

bool curseforge_proxy_get(const ApiCfg& cfg, const std::string& url, Json& out,
                          std::string* err) {
    static const std::string provider_root = "https://api.curseforge.com";
    if (url.rfind(provider_root, 0) != 0 || cfg.curseforge_proxy_url.empty() ||
        cfg.curseforge_proxy_key.empty()) {
        if (err) *err = "CurseForge backend proxy is not configured";
        return false;
    }

    const std::string path = url.substr(provider_root.size());
    if (path.empty() || path.front() != '/') {
        if (err) *err = "invalid CurseForge provider path";
        return false;
    }

    Json body = Json::obj();
    body.set("path", Json::str(path));
    const std::string body_text = body.dump();
    const std::vector<uint8_t> body_bytes(body_text.begin(), body_text.end());
    std::vector<std::wstring> headers = {
        L"Accept: application/json",
        L"Content-Type: application/json",
        L"User-Agent: AmalgamLauncher/1.0",
        L"apikey: " + net::to_wide(cfg.curseforge_proxy_key),
    };
    const std::string bearer = cfg.curseforge_proxy_access_token.empty()
        ? cfg.curseforge_proxy_key : cfg.curseforge_proxy_access_token;
    headers.push_back(L"Authorization: Bearer " + net::to_wide(bearer));

    std::vector<uint8_t> bytes;
    std::string proxy_error;
    if (!net::post(net::to_wide(cfg.curseforge_proxy_url), headers, body_bytes, bytes,
                   &proxy_error)) {
        if (err) {
            *err = cfg.curseforge_proxy_access_token.empty()
                ? "Sign in to an Amalgam account to use the shared CurseForge catalog"
                : (proxy_error.empty() ? "CurseForge backend request failed" : proxy_error);
        }
        return false;
    }
    return parse_json_bytes(bytes, out, err);
}

bool fetch_json(const ApiCfg& cfg, const std::string& url, const std::string& source,
                 Json& out, std::string* err) {
    std::string local_error;
    std::string* output_error = err ? err : &local_error;
    output_error->clear();
    if (source == "modrinth") {
        return direct_json_get(url, json_headers(cfg.modrinth_token), out, output_error);
    }
    if (source == "curseforge") {
        const std::string key = normalize_curseforge_key(cfg.curseforge_key);
        std::string direct_error;
        if (!key.empty()) {
            std::vector<std::wstring> headers = {
                L"Accept: application/json",
                L"User-Agent: AmalgamLauncher/1.0",
                L"x-api-key: " + net::to_wide(key),
            };
            if (direct_json_get(url, headers, out, &direct_error)) return true;
        }
        std::string proxy_error;
        if (!cfg.curseforge_proxy_url.empty() && !cfg.curseforge_proxy_key.empty() &&
            curseforge_proxy_get(cfg, url, out, &proxy_error)) {
            return true;
        }
        *output_error = !proxy_error.empty() ? proxy_error :
                        !direct_error.empty() ? direct_error :
                        "CurseForge is not configured";
        return false;
    }
    *output_error = "unsupported content provider";
    return false;
}

// Modrinth facets json: [["project_type:mod"],["categories:forge"],["versions:1.20.1"]]
std::string modrinth_facets(const std::string& loader, const std::string& game_version,
                            Facet facet) {
    Json arr = Json::arr();
    Json ft = Json::arr();
    std::string type;
    switch (facet) {
        case Facet::Modpack: type = "modpack"; break;
        case Facet::ResourcePack: type = "resourcepack"; break;
        case Facet::Shader: type = "shader"; break;
        case Facet::Datapack: type = "datapack"; break;
        default: type = "mod"; break;
    }
    ft.push(Json::str("project_type:" + type));
    arr.push(ft);
    if (!loader.empty()) {
        Json l = Json::arr();
        l.push(Json::str("categories:" + loader));
        arr.push(l);
    }
    if (!game_version.empty()) {
        Json v = Json::arr();
        v.push(Json::str("versions:" + game_version));
        arr.push(v);
    }
    if (facet >= Facet::Optimization) {
        Json c = Json::arr();
        c.push(Json::str("categories:" + std::string(facet_str(facet))));
        arr.push(c);
    }
    return arr.dump();
}

// pick highest compatible file; modrinth files carry loaders+game_versions directly
const FileInfo* pick(const std::vector<FileInfo>& files, const std::string& loader,
                     const std::string& game_version) {
    const FileInfo* best = nullptr;
    for (const auto& f : files) {
        bool l = true, g = true;
        if (!loader.empty() && !f.loaders.empty()) {
            l = std::find(f.loaders.begin(), f.loaders.end(), loader) != f.loaders.end();
        }
        if (!game_version.empty() && !f.game_versions.empty()) {
            g = std::find(f.game_versions.begin(), f.game_versions.end(), game_version) !=
                f.game_versions.end();
        }
        if (l && g && (!best || (f.primary && !best->primary))) best = &f;
    }
    return best;
}

bool install_one(const ApiCfg& cfg, const std::string& slug, const std::string& source,
                 const std::string& loader, const std::string& game_version,
                 const std::wstring& mods_dir, std::set<std::string>& visited,
                 std::vector<std::string>& log_lines, std::string* err, Ownership& ownership,
                 const std::string& required_by, const Progress& progress);

}  // namespace

std::string canonical_source(const std::string& source) {
    const std::string normalized = lower_copy(source);
    if (normalized == "modrinth" || normalized == "curseforge") return normalized;
    return {};
}

bool curseforge_proxy_configured(const ApiCfg& cfg) {
    return !cfg.curseforge_proxy_url.empty() && !cfg.curseforge_proxy_key.empty();
}

bool curseforge_available(const ApiCfg& cfg) {
    return !normalize_curseforge_key(cfg.curseforge_key).empty() ||
           (curseforge_proxy_configured(cfg) &&
            !cfg.curseforge_proxy_access_token.empty());
}

bool curseforge_json(const ApiCfg& cfg, const std::string& url, std::string& json,
                     std::string* err) {
    Json response;
    if (!fetch_json(cfg, url, "curseforge", response, err)) return false;
    json = response.dump();
    return true;
}

bool test_provider(const ApiCfg& cfg, const std::string& source, std::string* err) {
    const std::string provider = canonical_source(source);
    if (provider.empty()) {
        if (err) *err = "unsupported content provider";
        return false;
    }
    if (provider == "curseforge" && !curseforge_available(cfg)) {
        if (err) {
            *err = curseforge_proxy_configured(cfg)
                ? "Sign in to an Amalgam account to use the shared CurseForge catalog"
                : "CurseForge API key or backend proxy is not configured";
        }
        return false;
    }
    const std::string url = provider == "modrinth"
        ? "https://api.modrinth.com/v2/search?query=sodium&limit=1"
        : "https://api.curseforge.com/v1/mods/search?gameId=432&classId=6&searchFilter=sodium&pageSize=1";
    Json response;
    if (!fetch_json(cfg, url, provider, response, err)) return false;
    const Json& results = provider == "modrinth" ? response.get("hits") : response.get("data");
    if (!results.is(Json::Type::Arr)) {
        if (err) *err = provider + " returned an unexpected catalog response";
        return false;
    }
    if (err) err->clear();
    return true;
}

bool search(const ApiCfg& cfg, const std::string& query, const std::string& loader,
            const std::string& game_version, Facet facet, std::vector<SearchResult>& out,
            std::string* err, int page, const std::string& source) {
    out.clear();
    const std::string requested_source = source.empty() ? std::string() : canonical_source(source);
    if (!source.empty() && requested_source.empty()) {
        if (err) *err = "unsupported content provider";
        return false;
    }
    std::string effective_loader = lower_copy(loader);
    if (effective_loader == "auto" || effective_loader == "any")
        effective_loader.clear();
    std::string q = urlencode(query.empty() ? "" : query);
    // Native Bedrock add-ons are not a Modrinth project type. Query only
    // CurseForge's Bedrock Addons class for this facet.
    std::string modrinth_error;
    bool modrinth_ok = false;
    if ((requested_source.empty() || requested_source == "modrinth") && facet != Facet::BedrockAddon) {
        std::string facets = modrinth_facets(effective_loader, game_version, facet);
        std::string facets_e = urlencode(facets);
        std::string url =
            "https://api.modrinth.com/v2/search?query=" + q + "&facets=" + facets_e +
            "&limit=50&offset=" + std::to_string((page > 0 ? page : 0) * 50);

        Json j;
        modrinth_ok = fetch_json(cfg, url, "modrinth", j, &modrinth_error);
        if (modrinth_ok) {
            const Json& hits = j.get("hits");
            for (const auto& h : hits.items()) {
                SearchResult r;
                r.slug = h.get("slug").as_str();
                r.title = h.get("title").as_str();
                r.description = h.get("description").as_str();
                r.icon_url = h.get("icon_url").as_str();
                r.type = parse_type(h.get("project_type").as_str());
                r.downloads = h.get("downloads").as_int();
                r.follows = h.get("follows").as_int();
                r.loaders = str_arr(h.get("loaders"));
                r.categories = str_arr(h.get("categories"));
                r.date_modified = h.get("date_modified").as_str();
                r.source = "modrinth";
                out.push_back(std::move(r));
            }
        }
    }

    bool curseforge_ok = false;
    std::string curseforge_error;
    if (requested_source.empty() || requested_source == "curseforge") {
        if (!curseforge_available(cfg)) {
            curseforge_error = curseforge_proxy_configured(cfg)
                ? "Sign in to an Amalgam account to use the shared CurseForge catalog"
                : "CurseForge API key or backend proxy is not configured";
        } else {
        std::string cfurl = "https://api.curseforge.com/v1/mods/search?gameId=432&classId=" +
                            std::to_string(cf_class_id(facet)) + "&searchFilter=" + q +
                            "&sortField=2&sortOrder=desc&pageSize=30&index=" +
                            std::to_string((page > 0 ? page : 0) * 30);
        if (!effective_loader.empty())
            cfurl += "&modLoaderType=" + std::to_string(cf_loader_id(effective_loader));
        if (!game_version.empty()) cfurl += "&gameVersion=" + urlencode(game_version);
        Json cj;
        curseforge_ok = fetch_json(cfg, cfurl, "curseforge", cj, &curseforge_error);
        if (curseforge_ok) {
            const Json& data = cj.get("data");
            for (const auto& d : data.items()) {
                SearchResult r;
                r.slug = std::to_string(d.get("id").as_int());
                r.title = d.get("name").as_str();
                r.description = d.get("summary").as_str();
                r.icon_url = d.get("logo").get("thumbnailUrl").as_str(
                    d.get("logo").get("url").as_str());
                r.downloads = d.get("downloadCount").as_int();
                r.follows = d.get("gamePopularityRank").as_int();
                r.date_modified = d.get("dateModified").as_str();
                if (facet == Facet::Modpack) r.type = ProjectType::Modpack;
                else if (facet == Facet::ResourcePack) r.type = ProjectType::ResourcePack;
                else if (facet == Facet::Shader) r.type = ProjectType::Shader;
                else if (facet == Facet::Datapack) r.type = ProjectType::Datapack;
                else r.type = ProjectType::Mod;
                r.source = "curseforge";
                for (const auto& gv : d.get("gameVersions").items()) {
                    std::string s = gv.as_str();
                    s = lower_copy(s);
                    if (s.find("fabric") != std::string::npos) r.loaders.push_back("fabric");
                    else if (s.find("neoforge") != std::string::npos) r.loaders.push_back("neoforge");
                    else if (s.find("forge") != std::string::npos) r.loaders.push_back("forge");
                    else if (s.find("quilt") != std::string::npos) r.loaders.push_back("quilt");
                }
                out.push_back(std::move(r));
            }
        }
        }
    }

    if (out.empty()) {
        if (err) {
            if (requested_source == "modrinth")
                *err = modrinth_ok ? "no results" : modrinth_error;
            else if (requested_source == "curseforge")
                *err = curseforge_ok ? "no results" : curseforge_error;
            else if (!modrinth_ok && !curseforge_ok)
                *err = curseforge_error.empty() ? modrinth_error : curseforge_error;
            else *err = "no results";
        }
        return requested_source == "modrinth" ? modrinth_ok
             : requested_source == "curseforge" ? curseforge_ok
             : modrinth_ok || curseforge_ok;
    }
    if (err) err->clear();
    return true;
}

bool project_files(const ApiCfg& cfg, const std::string& slug, const std::string& source,
                   ModInfo& out, std::string* err) {
    out = ModInfo{};
    const std::string provider = canonical_source(source);
    if (provider.empty()) {
        if (err) *err = "unsupported content provider";
        return false;
    }
    if (provider == "curseforge") {
        std::string base = "https://api.curseforge.com/v1/mods/" + urlencode(slug);
        Json proj;
        if (!fetch_json(cfg, base, "curseforge", proj, err)) return false;
        const Json& d = proj.get("data");
        out.slug = d.get("slug").as_str();
        out.title = d.get("name").as_str();
        out.description = d.get("summary").as_str();
        out.icon_url = d.get("logo").get("thumbnailUrl").as_str(
            d.get("logo").get("url").as_str());
        out.date_published = d.get("dateCreated").as_str();
        out.date_updated = d.get("dateModified").as_str();
        for (const auto& author : d.get("authors").items()) {
            const std::string name = author.get("name").as_str();
            if (name.empty()) continue;
            if (!out.author.empty()) out.author += ", ";
            out.author += name;
        }
        for (const auto& screenshot : d.get("screenshots").items()) {
            const std::string url = screenshot.get("url").as_str(
                screenshot.get("thumbnailUrl").as_str());
            if (!url.empty()) out.gallery_urls.push_back(url);
        }
        out.source_url = d.get("links").get("sourceUrl").as_str();
        out.website_url = d.get("links").get("websiteUrl").as_str(out.source_url);
        out.issues_url = d.get("links").get("issuesUrl").as_str();
        const int class_id = static_cast<int>(d.get("classId").as_int(6));
        out.type = class_id == 4471 ? ProjectType::Modpack :
                   class_id == 12 ? ProjectType::ResourcePack :
                   class_id == 6552 ? ProjectType::Shader :
                   class_id == 6945 ? ProjectType::Datapack : ProjectType::Mod;
        Json description;
        std::string description_error;
        if (fetch_json(cfg, base + "/description", "curseforge", description, &description_error))
            out.body = description.get("data").as_str();
        Json files;
        if (!fetch_json(cfg, base + "/files", "curseforge", files, err)) return false;
        const Json& fdata = files.get("data");
        for (const auto& f : fdata.items()) {
            FileInfo fi;
            fi.id = std::to_string(f.get("id").as_int());
            fi.filename = f.get("fileName").as_str();
            fi.url = f.get("downloadUrl").as_str();
            fi.primary = !f.get("isServerPack").as_bool(false);
            // CurseForge exposes the loader as a separate enum on each file.
            // Keep the game-version tags below for version matching, but do
            // not infer the loader only from free-form labels.
            switch (static_cast<int>(f.get("modLoader").as_int(0))) {
                case 1: fi.loaders.push_back("forge"); break;
                case 4: fi.loaders.push_back("fabric"); break;
                case 5: fi.loaders.push_back("quilt"); break;
                case 6: fi.loaders.push_back("neoforge"); break;
                default: break;  // Any / untagged file
            }
            for (const auto& gv : f.get("gameVersions").items()) {
                std::string s = gv.as_str();
                if (s.find("1.") == 0) fi.game_versions.push_back(s);
                std::string normalized = lower_copy(s);
                if (normalized.find("neoforge") != std::string::npos &&
                    std::find(fi.loaders.begin(), fi.loaders.end(), "neoforge") == fi.loaders.end()) {
                    fi.loaders.push_back("neoforge");
                } else if (normalized.find("forge") != std::string::npos &&
                           std::find(fi.loaders.begin(), fi.loaders.end(), "forge") == fi.loaders.end()) {
                    fi.loaders.push_back("forge");
                } else if (normalized.find("fabric") != std::string::npos &&
                           std::find(fi.loaders.begin(), fi.loaders.end(), "fabric") == fi.loaders.end()) {
                    fi.loaders.push_back("fabric");
                } else if (normalized.find("quilt") != std::string::npos &&
                           std::find(fi.loaders.begin(), fi.loaders.end(), "quilt") == fi.loaders.end()) {
                    fi.loaders.push_back("quilt");
                }
            }
            fi.size = f.get("fileLength").as_int();
            fi.version_number = f.get("displayName").as_str();
            fi.version_name = fi.version_number;
            fi.date_published = f.get("fileDate").as_str();
            for (const auto& hash : f.get("hashes").items()) {
                if (hash.get("algo").as_int() == 1) fi.sha1 = hash.get("value").as_str();
            }
            for (const auto& dependency : f.get("dependencies").items()) {
                const int relation = static_cast<int>(dependency.get("relationType").as_int(0));
                if (relation != 2 && relation != 3 && relation != 6) continue;
                DepInfo dep;
                dep.project_id = std::to_string(dependency.get("modId").as_int(0));
                dep.version_id = std::to_string(dependency.get("modVersionId").as_int(0));
                dep.required = relation != 2;
                if (dep.project_id != "0") fi.dependencies.push_back(std::move(dep));
            }
            out.files.push_back(std::move(fi));
        }
        if (out.files.empty() && err && err->empty()) *err = "no files";
        return true;
    }
    std::string base = "https://api.modrinth.com/v2/project/" + urlencode(slug);
    Json proj;
    if (!fetch_json(cfg, base, "modrinth", proj, err)) return false;
    out.slug = proj.get("slug").as_str();
    out.title = proj.get("title").as_str();
    out.description = proj.get("description").as_str();
    out.icon_url = proj.get("icon_url").as_str();
    out.body = proj.get("body").as_str();
    out.license = proj.get("license").get("id").as_str();
    out.date_published = proj.get("published").as_str();
    out.date_updated = proj.get("updated").as_str();
    for (const auto& image : proj.get("gallery").items()) {
        const std::string url = image.get("url").as_str();
        if (!url.empty()) out.gallery_urls.push_back(url);
    }
    out.source_url = proj.get("source_url").as_str();
    out.website_url = proj.get("website_url").as_str(out.source_url);
    out.issues_url = proj.get("issues_url").as_str();
    out.type = parse_type(proj.get("project_type").as_str());
    out.loaders = str_arr(proj.get("loaders"));
    out.categories = str_arr(proj.get("categories"));

    Json vers;
    if (!fetch_json(cfg, base + "/version", "modrinth", vers, err)) return false;
    for (const auto& v : vers.items()) {
        FileInfo f;
        f.id = v.get("id").as_str();
        f.filename = v.get("files").size() > 0 ? v.get("files").at(0).get("filename").as_str()
                                               : std::string();
        f.url = v.get("files").size() > 0 ? v.get("files").at(0).get("url").as_str()
                                          : std::string();
        f.loaders = str_arr(v.get("loaders"));
        f.game_versions = str_arr(v.get("game_versions"));
        f.primary = v.get("version_type").as_str() == "release";
        f.version_number = v.get("version_number").as_str();
        f.version_name = v.get("name").as_str();
        f.changelog = v.get("changelog").as_str();
        f.date_published = v.get("date_published").as_str();
        f.size = v.get("files").size() > 0 ? v.get("files").at(0).get("size").as_int() : 0;
        for (const auto& dependency : v.get("dependencies").items()) {
            DepInfo dep;
            dep.project_id = dependency.get("project_id").as_str();
            dep.version_id = dependency.get("version_id").as_str();
            dep.required = dependency.get("dependency_type").as_str() == "required";
            if (!dep.project_id.empty()) f.dependencies.push_back(std::move(dep));
        }
        if (v.get("files").size() > 0) {
            for (const auto& hash : v.get("files").at(0).get("hashes").pairs()) {
                if (lower_copy(hash.first) == "sha1") f.sha1 = hash.second.as_str();
            }
        }
        out.files.push_back(std::move(f));
    }
    if (out.files.empty() && err && err->empty()) *err = "no files";
    return true;
}

std::string pick_file(const ModInfo& mod, const std::string& loader,
                      const std::string& game_version) {
    const FileInfo* f = pick(mod.files, loader, game_version);
    return f ? f->id : std::string();
}

bool select_compatible_release(const ModInfo& mod, const std::string& preferred_loader,
                               const std::string& preferred_game_version,
                               CompatibleRelease& out, std::string* err) {
    out = {};
    const std::string wanted_loader = lower_copy(preferred_loader);
    const bool loader_is_hint = !wanted_loader.empty() && wanted_loader != "auto" &&
                                wanted_loader != "any";
    const bool version_is_hint = !preferred_game_version.empty();

    const FileInfo* best = nullptr;
    int best_score = -1;
    std::string best_loader;
    std::string best_version;
    for (const auto& file : mod.files) {
        if (file.id.empty() || file.filename.empty()) continue;

        std::vector<std::string> file_loaders;
        file_loaders.reserve(file.loaders.size());
        for (const std::string& value : file.loaders) {
            const std::string normalized = lower_copy(value);
            if (!normalized.empty() &&
                std::find(file_loaders.begin(), file_loaders.end(), normalized) == file_loaders.end())
                file_loaders.push_back(normalized);
        }
        if (file_loaders.empty()) file_loaders.push_back("vanilla");

        const bool exact_loader = !loader_is_hint ||
            std::find(file_loaders.begin(), file_loaders.end(), wanted_loader) != file_loaders.end();
        const bool exact_version = !version_is_hint ||
            std::find(file.game_versions.begin(), file.game_versions.end(), preferred_game_version) !=
                file.game_versions.end();

        // Hints are deliberately soft. Providers sometimes omit a loader on
        // a pack file, and a user may have a stale filter selected for a pack
        // that only ships a Forge release. Prefer the hint when possible, but
        // never turn that into the old false "no compatible release" error.
        int score = 0;
        if (file.primary) score += 100;
        if (exact_loader) score += 1000;
        if (exact_version) score += 2000;
        if (!file.url.empty()) score += 10;
        if (!file.sha1.empty() || file.size > 0) score += 5;
        if (score <= best_score) continue;

        best = &file;
        best_score = score;
        best_loader = exact_loader && loader_is_hint ? wanted_loader : file_loaders.front();
        best_version = exact_version && version_is_hint ? preferred_game_version :
                       (file.game_versions.empty() ? preferred_game_version : file.game_versions.front());
    }

    if (!best) {
        if (err) {
            *err = mod.files.empty() ? "the provider returned no releases"
                                     : "the provider returned no downloadable releases";
        }
        return false;
    }
    out.file_id = best->id;
    out.loader = best_loader.empty() ? "vanilla" : best_loader;
    out.game_version = best_version;
    if (err) err->clear();
    return true;
}

bool resolve_download_url(const ApiCfg& cfg, const std::string& project_id,
                          const std::string& source, FileInfo& file, std::string* err) {
    if (!file.url.empty()) return true;
    if (source != "curseforge") {
        if (err) *err = "provider returned no download URL";
        return false;
    }
    Json response;
    const std::string url = "https://api.curseforge.com/v1/mods/" + urlencode(project_id) +
                            "/files/" + urlencode(file.id) + "/download-url";
    if (!fetch_json(cfg, url, source, response, err)) return false;
    file.url = response.get("data").as_str();
    if (file.url.empty()) {
        if (err) *err = "CurseForge returned no download URL for file " + file.id;
        return false;
    }
    return true;
}

bool check_project_update(const ApiCfg& cfg, const std::string& slug, const std::string& source,
                          const std::string& loader, const std::string& game_version,
                          const std::string& current_version, UpdateInfo& out, std::string* err) {
    out = {};
    out.current_version = current_version;
    ModInfo mod;
    if (!project_files(cfg, slug, source, mod, err)) return false;
    const FileInfo* latest = pick(mod.files, loader, game_version);
    if (!latest) {
        if (err) *err = "no compatible upstream version";
        return false;
    }
    out.latest_version = latest->version_number.empty() ? latest->id : latest->version_number;
    out.latest_file_id = latest->id;
    out.latest_filename = latest->filename;
    out.latest_url = latest->url;
    out.latest_sha1 = latest->sha1;
    out.latest_size = latest->size;
    out.latest_published = latest->date_published;
    out.changelog = latest->changelog;
    out.project_url = mod.source_url;
    // Older imported profiles may retain the project but not the provider's
    // exact file/version id. Treat that as an update candidate so the user can
    // safely refresh it through the staged pack-update workflow.
    out.available = current_version.empty() || (latest->id != current_version &&
                                                latest->version_number != current_version);
    return true;
}

namespace {

bool load_existing_ownership(const std::wstring& metadata_path, Ownership& ownership,
                             std::string* err) {
    ownership = Ownership{};
    if (!net::file_exists(metadata_path)) return true;

    Json metadata;
    std::string read_error;
    if (!json_parse_file(metadata_path, metadata, &read_error)) {
        if (err) *err = read_error.empty()
                            ? "existing dependency metadata could not be read safely"
                            : "existing dependency metadata could not be read: " + read_error;
        return false;
    }
    for (const auto& item : metadata.get("entries").items()) {
        const std::string filename = item.get("file").as_str();
        if (filename.empty()) continue;
        if (!safe_filename(filename)) {
            if (err) *err = "existing dependency metadata contains an unsafe filename";
            return false;
        }
        Ownership::Record record;
        record.project = item.get("project").as_str();
        record.source = item.get("source").as_str("modrinth");
        record.version_id = item.get("version_id").as_str();
        record.sha1 = item.get("sha1").as_str();
        for (const auto& owner : item.get("required_by").items()) {
            const std::string label = owner.as_str();
            if (!label.empty()) record.required_by.insert(label);
        }
        if (record.required_by.empty()) record.required_by.insert("direct");
        ownership.files[filename] = std::move(record);
    }
    for (const auto& item : metadata.get("optional").items()) {
        const std::string project = item.get("project").as_str();
        if (project.empty()) continue;
        auto& offered_by = ownership.optional[project];
        for (const auto& owner : item.get("offered_by").items()) {
            const std::string label = owner.as_str();
            if (!label.empty()) offered_by.insert(label);
        }
    }
    return true;
}

Json ownership_metadata(const Ownership& ownership) {
    Json metadata = Json::obj();
    metadata.set("format", Json::num(1));
    Json entries = Json::arr();
    for (const auto& item : ownership.files) {
        Json entry = Json::obj();
        entry.set("file", Json::str(item.first));
        entry.set("project", Json::str(item.second.project));
        entry.set("source", Json::str(item.second.source));
        entry.set("version_id", Json::str(item.second.version_id));
        entry.set("sha1", Json::str(item.second.sha1));
        Json owners = Json::arr();
        for (const auto& owner : item.second.required_by) owners.push(Json::str(owner));
        entry.set("required_by", owners);
        entries.push(entry);
    }
    metadata.set("entries", entries);
    Json optional = Json::arr();
    for (const auto& item : ownership.optional) {
        Json entry = Json::obj();
        entry.set("project", Json::str(item.first));
        Json owners = Json::arr();
        for (const auto& owner : item.second) owners.push(Json::str(owner));
        entry.set("offered_by", owners);
        optional.push(entry);
    }
    metadata.set("optional", optional);
    return metadata;
}

bool staged_filenames(const std::wstring& stage_dir, std::set<std::string>& files,
                      std::string* err) {
    files.clear();
    std::error_code iterator_error;
    const std::filesystem::path root(stage_dir);
    if (!std::filesystem::exists(root, iterator_error) || iterator_error) {
        if (err) *err = "installer staging area is unavailable";
        return false;
    }
    for (std::filesystem::directory_iterator it(root, iterator_error), end;
         !iterator_error && it != end; it.increment(iterator_error)) {
        std::error_code type_error;
        if (!it->is_regular_file(type_error) || type_error) continue;
        const std::string filename = net::to_utf8(it->path().filename().wstring());
        if (!safe_filename(filename)) {
            if (err) *err = "installer staging area contains an unsafe filename";
            return false;
        }
        files.insert(filename);
    }
    if (iterator_error) {
        if (err) *err = "could not inspect installer staging area: " + iterator_error.message();
        return false;
    }
    if (files.empty()) {
        if (err) *err = "provider did not stage any installable files";
        return false;
    }
    return true;
}

struct Activation {
    std::wstring destination;
    std::wstring backup;
    bool original_moved = false;
    bool activated = false;
};

void rollback_activations(const std::vector<Activation>& activations) {
    for (auto it = activations.rbegin(); it != activations.rend(); ++it) {
        if (it->activated) DeleteFileW(it->destination.c_str());
        if (it->original_moved)
            MoveFileExW(it->backup.c_str(), it->destination.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
}

bool activate_staged_files(const std::wstring& stage_dir, const std::wstring& mods_dir,
                           const std::wstring& backup_dir, const std::set<std::string>& files,
                           std::vector<Activation>& activations, std::string* err) {
    activations.clear();
    for (const auto& filename : files) {
        const std::wstring staged = stage_dir + L"\\" + net::to_wide(filename);
        const std::wstring destination = mods_dir + L"\\" + net::to_wide(filename);
        Activation activation;
        activation.destination = destination;
        if (net::file_exists(destination)) {
            if (!net::mkdirs(backup_dir)) {
                if (err) *err = "could not create a recovery backup for existing content";
                rollback_activations(activations);
                return false;
            }
            activation.backup = backup_dir + L"\\" + net::to_wide(filename);
            if (!MoveFileExW(destination.c_str(), activation.backup.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                if (err) *err = "could not move existing content into recovery: " + filename;
                rollback_activations(activations);
                return false;
            }
            activation.original_moved = true;
        }
        if (!MoveFileExW(staged.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            if (activation.original_moved)
                MoveFileExW(activation.backup.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING);
            if (err) *err = "could not activate staged content: " + filename;
            rollback_activations(activations);
            return false;
        }
        activation.activated = true;
        activations.push_back(std::move(activation));
    }
    return true;
}

void remove_staging_area(const std::wstring& stage_dir) {
    std::error_code ignored;
    std::filesystem::remove_all(std::filesystem::path(stage_dir), ignored);
}

std::wstring install_staging_id(const std::string& source, const std::string& slug,
                                const std::string& loader, const std::string& game_version) {
    // Deterministic per target profile (the parent directory supplies that
    // scope), so a retry can reuse a verified .part prefix after a pause,
    // network interruption, or launcher restart without accepting arbitrary
    // provider text as a filesystem path.
    uint64_t hash = 1469598103934665603ull;
    const std::string key = source + "\n" + slug + "\n" + loader + "\n" + game_version;
    for (unsigned char byte : key) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return L"install-" + std::to_wstring(hash);
}

bool install_one(const ApiCfg& cfg, const std::string& slug, const std::string& source,
                 const std::string& loader, const std::string& game_version,
                 const std::wstring& mods_dir, std::set<std::string>& visited,
                 std::vector<std::string>& log_lines, std::string* err, Ownership& ownership,
                 const std::string& required_by, const Progress& progress) {
    if (!visited.insert(source + ":" + slug).second) return true;
    ModInfo mod;
    if (!project_files(cfg, slug, source, mod, err)) return false;
    const FileInfo* f = pick(mod.files, loader, game_version);
    if (!f) {
        const std::string target = (loader.empty() ? "any loader" : loader) +
                                   (game_version.empty() ? std::string() : " " + game_version);
        const std::string context = required_by.empty() ? "content" :
                                     "required dependency of " + required_by;
        log_lines.push_back("FAIL  " + mod.title + " (no compatible file for " + target + ")");
        if (err) *err = mod.title + " has no compatible release for " + context +
                        " (" + target + ")";
        return false;
    }
    FileInfo selected = *f;
    if (!resolve_download_url(cfg, slug, source, selected, err)) return false;
    if (!safe_filename(selected.filename)) {
        if (err) *err = "provider returned an unsafe filename";
        return false;
    }
    if (selected.sha1.empty() && selected.size <= 0) {
        if (err) *err = "provider file has no usable integrity metadata";
        return false;
    }
    log_lines.push_back("ADD   " + mod.title + " -> " + selected.filename);
    auto& ownership_record = ownership.files[selected.filename];
    ownership_record.project = slug;
    ownership_record.source = source;
    ownership_record.version_id = selected.id;
    ownership_record.sha1 = selected.sha1;
    ownership_record.required_by.insert(required_by.empty() ? "direct" : required_by);
    if (!net::mkdirs(mods_dir)) {
        if (err) *err = "cannot create mods dir";
        return false;
    }
    const std::wstring staged_path = mods_dir + L"\\" + net::to_wide(selected.filename);
    const int64_t expected_size = selected.size > 0 ? selected.size : -1;
    if (net::verify_file(staged_path, selected.sha1, expected_size)) {
        log_lines.push_back("REUSE verified staged file " + selected.filename);
    } else if (!net::download(net::to_wide(selected.url), staged_path, progress, err,
                              selected.sha1, expected_size)) {
        log_lines.push_back("FAIL  download " + selected.filename + ": " + *err);
        return false;
    }
    // project_files already provides the selected release's relation graph.
    // Reusing it avoids a second provider request and makes a failed required
    // dependency resolution fail while the transaction is still staged.
    for (const auto& dependency : selected.dependencies) {
        std::string dependency_project = dependency.project_id;
        if (dependency_project.empty() && source == "modrinth" && !dependency.version_id.empty()) {
            Json version;
            const std::string version_url = "https://api.modrinth.com/v2/version/" +
                                            urlencode(dependency.version_id);
            if (!fetch_json(cfg, version_url, "modrinth", version, err)) return false;
            dependency_project = version.get("project_id").as_str();
        }
        if (dependency_project.empty() || dependency_project == "0") {
            if (dependency.required) {
                if (err) *err = "provider returned a required dependency without a project reference";
                return false;
            }
            continue;
        }
        if (!dependency.required) {
            ownership.optional[dependency_project].insert(slug);
            continue;
        }
        std::string dependency_slug = dependency_project;
        if (source == "modrinth") {
            Json project;
            const std::string project_url = "https://api.modrinth.com/v2/project/" +
                                            urlencode(dependency_project);
            if (!fetch_json(cfg, project_url, "modrinth", project, err)) return false;
            dependency_slug = project.get("slug").as_str();
            if (dependency_slug.empty()) {
                if (err) *err = "Modrinth returned a dependency without a project slug";
                return false;
            }
        }
        if (!install_one(cfg, dependency_slug, source, loader, game_version, mods_dir, visited,
                         log_lines, err, ownership, slug, progress))
            return false;
    }
    return true;
}

bool create_update_backup(const std::wstring& instance_dir, std::wstring& backup_dir,
                          std::string* err) {
    namespace fs = std::filesystem;
    std::error_code ec;
    backup_dir = instance_dir + L"\\.amalgam-update-rollback-" +
                 std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    fs::create_directories(backup_dir, ec);
    if (ec) {
        if (err) *err = "could not create update rollback: " + ec.message();
        return false;
    }
    const fs::path mods = fs::path(instance_dir) / L"mods";
    if (fs::exists(mods, ec))
        fs::copy(mods, fs::path(backup_dir) / L"mods",
                 fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (!ec && fs::exists(fs::path(instance_dir) / L"amalgam-dependencies.json", ec))
        fs::copy_file(fs::path(instance_dir) / L"amalgam-dependencies.json",
                      fs::path(backup_dir) / L"amalgam-dependencies.json",
                      fs::copy_options::overwrite_existing, ec);
    if (ec) {
        fs::remove_all(backup_dir, ec);
        if (err) *err = "could not copy update rollback: " + ec.message();
        return false;
    }
    return true;
}

bool restore_update_backup(const std::wstring& instance_dir, const std::wstring& backup_dir,
                           std::string* err) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path target_mods = fs::path(instance_dir) / L"mods";
    fs::remove_all(target_mods, ec);
    if (!ec && fs::exists(fs::path(backup_dir) / L"mods", ec))
        fs::copy(fs::path(backup_dir) / L"mods", target_mods,
                 fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (!ec && fs::exists(fs::path(backup_dir) / L"amalgam-dependencies.json", ec))
        fs::copy_file(fs::path(backup_dir) / L"amalgam-dependencies.json",
                      fs::path(instance_dir) / L"amalgam-dependencies.json",
                      fs::copy_options::overwrite_existing, ec);
    fs::remove_all(backup_dir, ec);
    if (ec && err) *err = "rollback failed: " + ec.message();
    return !ec;
}

}  // namespace

bool install_mod(const ApiCfg& cfg, const std::string& slug, const std::string& source,
                 const std::string& loader, const std::string& game_version,
                 const std::wstring& mods_dir, std::vector<std::string>& log_lines,
                 std::string* err, const Progress& progress) {
    std::string local_error;
    if (!err) err = &local_error;
    err->clear();
    const std::string provider = canonical_source(source);
    if (provider.empty()) {
        if (err) *err = "unsupported content provider";
        return false;
    }
    std::wstring instance_dir = mods_dir;
    size_t slash = instance_dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        if (err) *err = "invalid profile mods directory";
        return false;
    }
    instance_dir.resize(slash);
    const std::wstring metadata_path = instance_dir + L"\\amalgam-dependencies.json";
    // Fresh profiles do not necessarily have a mods directory yet. Staging
    // the download is not enough because activation moves the verified file
    // directly into this destination.
    if (!net::mkdirs(mods_dir)) {
        if (err) *err = "cannot create mods dir";
        return false;
    }
    Ownership ownership;
    if (!load_existing_ownership(metadata_path, ownership, err)) return false;

    const std::wstring stage_id = install_staging_id(provider, slug, loader, game_version);
    const std::wstring stage_dir = instance_dir + L"\\.amalgam-staging\\" + stage_id;
    const std::wstring backup_dir = instance_dir + L"\\.amalgam-backups\\mods\\" + stage_id + L"-" +
                                    std::to_wstring(GetTickCount64());
    if (!net::mkdirs(stage_dir)) {
        if (err) *err = "could not create a safe installer staging area";
        return false;
    }
    log_lines.push_back("STAGE install before changing profile content");

    std::set<std::string> visited;
    bool ok = install_one(cfg, slug, provider, loader, game_version, stage_dir, visited, log_lines,
                          err, ownership, "", progress);
    if (!ok) {
        log_lines.push_back("RETRY keeps verified partial download data in the profile staging area");
        return false;
    }

    std::set<std::string> files_to_activate;
    if (!staged_filenames(stage_dir, files_to_activate, err)) {
        remove_staging_area(stage_dir);
        return false;
    }
    std::vector<Activation> activations;
    if (!activate_staged_files(stage_dir, mods_dir, backup_dir, files_to_activate, activations, err)) {
        remove_staging_area(stage_dir);
        return false;
    }

    std::string metadata_error;
    if (!json_write_file(metadata_path, ownership_metadata(ownership), &metadata_error)) {
        rollback_activations(activations);
        remove_staging_area(stage_dir);
        if (err) *err = metadata_error.empty() ? "could not write dependency metadata" : metadata_error;
        return false;
    }
    remove_staging_area(stage_dir);
    if (!activations.empty())
        log_lines.push_back("APPLY " + std::to_string(activations.size()) +
                            " staged file(s); replaced files were moved to profile recovery");
    return true;
}

bool update_owned(const ApiCfg& cfg, const std::wstring& instance_dir, const std::string& loader,
                  const std::string& game_version, std::vector<std::string>& log_lines,
                  std::string* err, const std::set<std::string>& selected_files,
                  const Progress& progress) {
    std::wstring metadata_path = instance_dir + L"\\amalgam-dependencies.json";
    Json metadata;
    if (!json_parse_file(metadata_path, metadata, err)) return false;
    std::wstring rollback_dir;
    if (!create_update_backup(instance_dir, rollback_dir, err)) return false;
    auto fail_update = [&](const std::string& message) {
        std::string rollback_error;
        restore_update_backup(instance_dir, rollback_dir, &rollback_error);
        if (err) {
            *err = message;
            if (!rollback_error.empty()) *err += "; " + rollback_error;
        }
        return false;
    };
    Json updated = metadata;
    Json entries = Json::arr();
    for (const auto& item : metadata.get("entries").items()) {
        std::string file_name = item.get("file").as_str();
        std::string project = item.get("project").as_str();
        std::string source = item.get("source").as_str("modrinth");
        std::string current_id = item.get("version_id").as_str();
        if (file_name.empty() || project.empty() || current_id.empty()) {
            entries.push(item);
            continue;
        }
        if (!selected_files.empty() && selected_files.find(file_name) == selected_files.end()) {
            entries.push(item);
            continue;
        }
        if (!safe_filename(file_name)) {
            if (err) *err = "dependency metadata contains an unsafe filename";
            return false;
        }
        ModInfo info;
        std::string lookup_error;
        if (!project_files(cfg, project, source, info, &lookup_error)) {
            log_lines.push_back("SKIP  " + file_name + ": " + lookup_error);
            entries.push(item);
            continue;
        }
        const FileInfo* candidate = pick(info.files, loader, game_version);
        if (!candidate || candidate->id == current_id) {
            entries.push(item);
            continue;
        }
        FileInfo selected = *candidate;
        if (!resolve_download_url(cfg, project, source, selected, err))
            return fail_update(err ? *err : "could not resolve update URL");
        if (!safe_filename(selected.filename)) {
            return fail_update("provider returned an unsafe update filename");
        }
        if (selected.sha1.empty() && selected.size <= 0) {
            return fail_update("provider update has no usable integrity metadata");
        }
        std::wstring old_path = instance_dir + L"\\mods\\" + net::to_wide(file_name);
        std::wstring new_path = instance_dir + L"\\mods\\" + net::to_wide(selected.filename) + L".update";
        if (!net::download(net::to_wide(selected.url), new_path, progress, err,
                           selected.sha1, selected.size > 0 ? selected.size : -1))
            return fail_update(err ? *err : "update download failed");
        std::wstring final_path = instance_dir + L"\\mods\\" + net::to_wide(selected.filename);
        if (!MoveFileExW(new_path.c_str(), final_path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            DeleteFileW(new_path.c_str());
            return fail_update("cannot activate updated mod: " + selected.filename);
        }
        if (old_path != final_path) DeleteFileW(old_path.c_str());
        Json replacement = item;
        replacement.set("file", Json::str(selected.filename));
        replacement.set("version_id", Json::str(selected.id));
        replacement.set("sha1", Json::str(selected.sha1));
        entries.push(replacement);
        log_lines.push_back("UPDATE " + file_name + " -> " + selected.filename);
    }
    updated.set("entries", entries);
    if (!json_write_file(metadata_path, updated, err))
        return fail_update(err ? *err : "could not write updated dependency metadata");
    std::error_code cleanup_error;
    std::filesystem::remove_all(rollback_dir, cleanup_error);
    if (cleanup_error && err) *err = "update succeeded but rollback cleanup failed: " + cleanup_error.message();
    return !cleanup_error;
}

bool preview_owned(const ApiCfg& cfg, const std::wstring& instance_dir, const std::string& loader,
                   const std::string& game_version, std::vector<UpdateEntry>& out,
                   std::string* err) {
    out.clear();
    Json metadata;
    if (!json_parse_file(instance_dir + L"\\amalgam-dependencies.json", metadata, err)) return false;
    for (const auto& item : metadata.get("entries").items()) {
        const std::string file_name = item.get("file").as_str();
        const std::string project = item.get("project").as_str();
        const std::string source = item.get("source").as_str("modrinth");
        const std::string current_id = item.get("version_id").as_str();
        if (file_name.empty() || project.empty() || current_id.empty() || !safe_filename(file_name)) continue;
        ModInfo info;
        std::string lookup_error;
        if (!project_files(cfg, project, source, info, &lookup_error)) continue;
        const FileInfo* candidate = pick(info.files, loader, game_version);
        if (!candidate || candidate->id == current_id) continue;
        UpdateEntry update;
        update.file = file_name;
        update.project = project;
        update.source = source;
        update.current_version = current_id;
        update.latest_version = candidate->version_number.empty() ? candidate->id : candidate->version_number;
        update.latest_file = candidate->filename;
        update.changelog = candidate->changelog;
        out.push_back(std::move(update));
    }
    return true;
}

}  // namespace aml::mods
