#include "model.h"

#include "net.h"

#include <algorithm>
#include <cstdlib>
#include <regex>

namespace aml::model {

namespace {

constexpr wchar_t kManifestUrl[] = L"https://piston-meta.mojang.com/mc/game/version_manifest_v2.json";

bool safe_download_path(const std::string& path) {
    if (path.empty()) return true;
    if (path[0] == '/' || path[0] == '\\' || path.find(':') != std::string::npos)
        return false;
    std::string segment;
    for (size_t i = 0; i <= path.size(); ++i) {
        const char c = i < path.size() ? path[i] : '/';
        if (c == '/' || c == '\\') {
            if (segment.empty() || segment == "..") return false;
            segment.clear();
        } else {
            if (static_cast<unsigned char>(c) < 0x20) return false;
            segment += c;
        }
    }
    return true;
}

bool parse_download(const Json& j, Download& out) {
    out.url = j.get("url").as_str();
    out.sha1 = j.get("sha1").as_str();
    out.path = j.get("path").as_str();
    out.size = j.get("size").as_int(-1);
    return safe_download_path(out.path);
}

}  // namespace

bool rules_allow(const Json& j) {
    if (!j.is(Json::Type::Arr) || j.size() == 0) return true;
    // Default DISALLOW: rules present but none matching (e.g. other OS, unset
    // features) excludes the element — matches upstream launcher semantics.
    bool allow = false;
    for (const Json& rule : j.items()) {
        std::string action = rule.get("action").as_str("allow");
        bool match = true;
        const Json& os = rule.get("os");
        if (os.is(Json::Type::Obj)) {
            std::string name = os.get("name").as_str();
            if (!name.empty() && name != "windows") match = false;
            std::string arch = os.get("arch").as_str();
            if (match && !arch.empty()) {
                bool x64 = sizeof(void*) == 8;
                if (arch == "x86" && x64) match = false;
                if (arch == "x64" && !x64) match = false;
            }
        }
        if (rule.get("features").is(Json::Type::Obj)) {
            // Feature-gated arguments (is_demo_user, has_custom_resolution, ...) never
            // apply to Amalgam launches: no demo account, default resolution.
            match = false;
        }
        if (match) allow = action != "disallow";
    }
    return allow;
}

std::vector<ManifestEntry> fetch_manifest(std::string* err) {
    std::vector<ManifestEntry> out;
    std::vector<uint8_t> bytes;
    if (!net::get(kManifestUrl, bytes, err)) return out;
    std::string text(bytes.begin(), bytes.end());
    Json j = Json::parse(text, err);
    if (!j.is(Json::Type::Obj)) return out;
    const Json& versions = j.get("versions");
    out.reserve(versions.size());
    for (const Json& v : versions.items()) {
        ManifestEntry e;
        e.id = v.get("id").as_str();
        e.type = v.get("type").as_str();
        e.url = v.get("url").as_str();
        e.sha1 = v.get("sha1").as_str();
        e.size = v.get("size").as_int(-1);
        if (!e.id.empty()) out.push_back(std::move(e));
    }
    return out;
}

int rank(const std::string& id) {
    size_t first_dot = id.find('.');
    if (first_dot == std::string::npos) return 0;
    size_t second_dot = id.find('.', first_dot + 1);
    int major = std::atoi(id.substr(0, first_dot).c_str());
    int minor = std::atoi(id.substr(first_dot + 1, second_dot == std::string::npos
                                                    ? std::string::npos
                                                    : second_dot - first_dot - 1)
                              .c_str());
    int patch = second_dot == std::string::npos
                    ? 0
                    : std::atoi(id.substr(second_dot + 1).c_str());
    if (major < 1 || minor < 0 || patch < 0) return 0;
    return major * 1000 + minor * 10 + patch;
}

std::string first_segment(const std::string& id) {
    size_t dot = id.find('.');
    return dot == std::string::npos ? id : id.substr(0, dot);
}

int default_java_major(int game_rank) {
    if (game_rank >= 1205) return 21;
    if (game_rank >= 1200) return 17;
    if (game_rank >= 1180) return 17;
    if (game_rank >= 1170) return 16;
    return 8;
}

bool parse_version(const Json& j, VersionJson& out) {
    out.id = j.get("id").as_str();
    out.main_class = j.get("mainClass").as_str();
    out.assets_name = j.get("assets").as_str();
    const Json& idx = j.get("assetIndex");
    if (idx.is(Json::Type::Obj)) {
        if (!parse_download(idx, out.assets_index)) return false;
        out.assets_index.path = out.assets_name + ".json";
    }
    const Json& dl = j.get("downloads");
    if (dl.is(Json::Type::Obj) && !parse_download(dl.get("client"), out.client)) return false;
    const Json& jv = j.get("javaVersion");
    if (jv.is(Json::Type::Obj)) out.java_major = static_cast<int>(jv.get("majorVersion").as_int(0));
    const Json& libs = j.get("libraries");
    if (libs.is(Json::Type::Arr)) {
        for (const Json& l : libs.items()) {
            const Json& rules = l.get("rules");
            if (!rules_allow(rules)) continue;
            const Json& download = l.get("downloads");
            Lib lib;
            for (const Json::Pair& p : l.pairs()) {
                if (p.first == "name") lib.name = p.second.as_str();
                if (p.first == "extract") {
                    for (const Json& excl : p.second.get("exclude").items()) {
                        lib.extract_exclude.push_back(excl.as_str());
                    }
                }
            }
            if (download.is(Json::Type::Obj)) {
                parse_download(download.get("artifact"), lib.artifact);
                const Json& natives = download.get("natives");
                if (natives.is(Json::Type::Obj)) {
                    for (const Json::Pair& np : natives.pairs()) {
                        Download d;
                        if (!parse_download(np.second, d)) return false;
                        d.path = np.second.get("path").as_str();
                        lib.natives.emplace_back(np.first, std::move(d));
                    }
                }
                const Json& classifiers = download.get("classifiers");
                if (classifiers.is(Json::Type::Obj)) {
                    for (const Json::Pair& cp : classifiers.pairs()) {
                        if (cp.first.find("natives-windows") == std::string::npos) continue;
                        Download d;
                        if (!parse_download(cp.second, d)) return false;
                        lib.natives.emplace_back(cp.first, std::move(d));
                    }
                }
            } else {
                lib.maven_url = l.get("url").as_str();
            }
            if (!lib.name.empty() && lib.artifact.url.empty() && lib.maven_url.empty() &&
                lib.natives.empty() && lib.artifact.path.empty()) {
                continue;
            }
            out.libraries.push_back(std::move(lib));
        }
    }
    const Json& args = j.get("arguments");
    if (args.is(Json::Type::Obj)) {
        auto collect = [](const Json& arg, std::vector<std::string>& out) {
            if (arg.is(Json::Type::Str)) {
                out.push_back(arg.as_str());
            } else if (arg.is(Json::Type::Obj)) {
                const Json& rules = arg.get("rules");
                if (!rules_allow(rules)) return;
                const Json& value = arg.get("value");
                if (value.is(Json::Type::Str)) {
                    out.push_back(value.as_str());
                } else if (value.is(Json::Type::Arr)) {
                    for (const Json& s : value.items()) {
                        if (s.is(Json::Type::Str)) out.push_back(s.as_str());
                    }
                }
            }
        };
        for (const Json& a : args.get("game").items()) collect(a, out.game_args);
        for (const Json& a : args.get("jvm").items()) {
            if (a.is(Json::Type::Str)) {
                std::string s = a.as_str();
                if (s.rfind("rg.gradle", 0) == 0) continue;
                out.jvm_args.push_back(s);
            } else {
                collect(a, out.jvm_args);
            }
        }
    }
    const Json& legacy_args = j.get("minecraftArguments");
    if (legacy_args.is(Json::Type::Str) && out.game_args.empty()) {
        std::string s = legacy_args.as_str();
        std::string cur;
        for (size_t i = 0; i <= s.size(); ++i) {
            char c = i < s.size() ? s[i] : ' ';
            if (c == ' ') {
                if (!cur.empty()) {
                    out.game_args.push_back(cur);
                    cur.clear();
                }
            } else {
                cur += c;
            }
        }
    }
    return true;
}

bool resolve_loader_version(const std::string& mc_id, const std::string& loader,
                            std::string* loader_version, std::string* err) {
    if (loader == "quilt") {
        // The bridge is built and tested against this version-independent Quilt loader.
        *loader_version = "0.20.0-beta.9";
        return true;
    }
    if (loader == "fabric" || loader == "quilt") {
        std::wstring url = (loader == "quilt"
                                ? L"https://meta.quiltmc.org/v3/versions/loader/"
                                : L"https://meta.fabricmc.net/v2/versions/loader/") +
                           net::to_wide(mc_id);
        std::vector<uint8_t> bytes;
        if (!net::get(url, bytes, err)) {
            // fabric-meta answers 400 (not 404) for an unknown game version;
            // surface what the user controls instead of the raw HTTP status.
            if (err) *err = "no " + loader + " loader for Minecraft " + mc_id;
            return false;
        }
        std::string text(bytes.begin(), bytes.end());
        Json j = Json::parse(text, err);
        if (!j.is(Json::Type::Arr) || j.size() == 0) {
            if (err) *err = "no " + loader + " loader for " + mc_id;
            return false;
        }
        for (const Json& item : j.items()) {
            const Json& loader_obj = item.get("loader");
            bool stable = loader_obj.get("stable").as_bool(false);
            std::string v = loader_obj.get("version").as_str();
            if (v.empty()) continue;
            if (stable) {
                *loader_version = v;
                return true;
            }
        }
        *loader_version = j.at(0).get("loader").get("version").as_str();
        return !loader_version->empty();
    }
    std::string base;
    std::string filter_prefix;
    if (loader == "forge") {
        base = "https://maven.minecraftforge.net/net/minecraftforge/forge/maven-metadata.xml";
        filter_prefix = mc_id + "-";
    } else if (loader == "neoforge") {
        base = "https://maven.neoforged.net/releases/net/neoforged/neoforge/maven-metadata.xml";
        // NeoForge versions drop the leading "1." of the MC version: 1.21.11 -> 21.11.x,
        // 1.20.1 -> 20.1.x.
        size_t dot = mc_id.find('.');
        if (dot != std::string::npos && mc_id.compare(0, 2, "1.") == 0) {
            filter_prefix = mc_id.substr(2) + ".";
        } else if (dot != std::string::npos) {
            std::string second = mc_id.substr(dot + 1,
                                              mc_id.find('.', dot + 1) == std::string::npos
                                                  ? std::string::npos
                                                  : mc_id.find('.', dot + 1) - dot - 1);
            filter_prefix = mc_id.substr(0, dot) + "." + second + ".";
        } else {
            filter_prefix = mc_id + ".";
        }
    } else {
        if (err) *err = "unknown loader";
        return false;
    }
    std::vector<uint8_t> bytes;
    if (!net::get(net::to_wide(base), bytes, err)) return false;
    std::string xml(bytes.begin(), bytes.end());
    std::regex re("<version>([^<]+)</version>");
    std::string best;
    std::string best_unstable;
    for (std::sregex_iterator it(xml.begin(), xml.end(), re), end; it != end; ++it) {
        std::string v = (*it)[1].str();
        if (v.rfind(filter_prefix, 0) != 0) continue;
        if (v.find("-pre") != std::string::npos || v.find("-rc") != std::string::npos ||
            v.find("beta") != std::string::npos || v.find("alpha") != std::string::npos ||
            v.find("snapshot") != std::string::npos) {
            best_unstable = v;  // remember latest non-stable in case no stable exists
            continue;
        }
        best = v;
    }
    if (best.empty()) best = best_unstable;
    if (best.empty()) {
        if (err) *err = std::string("no stable ") + loader + " for " + mc_id;
        return false;
    }
    *loader_version = best;
    return true;
}

std::string loader_profile_url(const std::string& mc_id, const std::string& loader,
                               const std::string& loader_version) {
    if (loader == "fabric") {
        return "https://meta.fabricmc.net/v2/versions/loader/" + mc_id + "/" + loader_version +
               "/profile/json";
    }
    if (loader == "quilt") {
        return "https://meta.quiltmc.org/v3/versions/loader/" + mc_id + "/" + loader_version +
               "/profile/json";
    }
    if (loader == "forge") {
        return "https://maven.minecraftforge.net/net/minecraftforge/forge/" + loader_version +
               "/forge-" + loader_version + ".json";
    }
    if (loader == "neoforge") {
        return "https://maven.neoforged.net/releases/net/neoforged/neoforge/" + loader_version +
               "/neoforge-" + loader_version + ".json";
    }
    return std::string();
}

std::string loader_installer_url(const std::string& loader, const std::string& loader_version) {
    if (loader == "forge") {
        return "https://maven.minecraftforge.net/net/minecraftforge/forge/" + loader_version +
               "/forge-" + loader_version + "-installer.jar";
    }
    if (loader == "neoforge") {
        return "https://maven.neoforged.net/releases/net/neoforged/neoforge/" + loader_version +
               "/neoforge-" + loader_version + "-installer.jar";
    }
    return std::string();
}

bool is_installer_loader(const std::string& loader) {
    return loader == "forge" || loader == "neoforge";
}

bool merge_inherited(const std::string& mc_id, const Json& profile, VersionJson* out,
                     std::string* err) {
    VersionJson base;
    {
        std::vector<ManifestEntry> manifest = fetch_manifest(err);
        const ManifestEntry* entry = nullptr;
        for (const ManifestEntry& e : manifest) {
            if (e.id == mc_id) {
                entry = &e;
                break;
            }
        }
        if (!entry) {
            if (err && err->empty()) *err = "base version " + mc_id + " not in manifest";
            return false;
        }
        std::vector<uint8_t> bytes;
        if (!net::get(net::to_wide(entry->url), bytes, err)) return false;
        std::string text(bytes.begin(), bytes.end());
        Json j = Json::parse(text, err);
        if (err && !err->empty()) return false;
        parse_version(j, base);
    }

    VersionJson p;
    parse_version(profile, p);

    *out = base;
    if (!p.main_class.empty()) out->main_class = p.main_class;
    if (!p.jvm_args.empty()) {
        for (const std::string& a : p.jvm_args) out->jvm_args.push_back(a);
    }
    if (!p.game_args.empty()) out->game_args = p.game_args;
    for (const Lib& l : p.libraries) {
        bool dup = false;
        for (const Lib& existing : out->libraries) {
            if (existing.name == l.name) {
                dup = true;
                break;
            }
        }
        if (!dup) out->libraries.push_back(l);
    }
    if (!p.assets_name.empty()) {
        out->assets_name = p.assets_name;
        out->assets_index = p.assets_index;
    }
    if (p.java_major > 0) out->java_major = p.java_major;
    if (!p.client.url.empty()) out->client = p.client;
    out->id = p.id.empty() ? base.id : p.id;
    return true;
}

}  // namespace aml::model
