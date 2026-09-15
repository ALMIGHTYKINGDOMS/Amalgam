#include "launch.h"

#include "auth.h"
#include "extract.h"
#include "instances.h"
#include "java.h"
#include "json.h"
#include "model.h"
#include "net.h"
#include "performance.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <random>
#include <sstream>

namespace aml::launch {

namespace {

std::wstring join(const std::vector<std::wstring>& parts) {
    std::wstring out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += L" ";
        out += extract::quote(parts[i]);
    }
    return out;
}

std::wstring join_classpath(const std::vector<std::wstring>& parts) {
    std::wstring out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += L';';
        out += parts[i];
    }
    return out;
}

std::wstring lib_path_from_name(const std::string& name) {
    size_t c1 = name.find(':');
    size_t c2 = c1 == std::string::npos ? std::string::npos : name.find(':', c1 + 1);
    if (c1 == std::string::npos || c2 == std::string::npos) return std::wstring();
    std::string group = name.substr(0, c1);
    std::string artifact = name.substr(c1 + 1, c2 - c1 - 1);
    std::string version = name.substr(c2 + 1);
    size_t at = version.find('@');
    if (at != std::string::npos) version = version.substr(0, at);
    std::string path = group;
    std::replace(path.begin(), path.end(), '.', '/');
    path += "/" + artifact + "/" + version + "/" + artifact + "-" + version + ".jar";
    return net::to_wide(path);
}

std::wstring native_rel(const std::wstring& rel) {
    std::wstring out = rel;
    for (wchar_t& c : out) {
        if (c == L'/') c = L'\\';
    }
    return out;
}

bool safe_local_relative(const std::wstring& value) {
    if (value.empty() || value[0] == L'\\' || value[0] == L'/' ||
        (value.size() >= 2 && value[1] == L':')) return false;
    std::wstring segment;
    for (size_t i = 0; i <= value.size(); ++i) {
        wchar_t c = i < value.size() ? value[i] : L'\\';
        if (c == L'\\' || c == L'/') {
            if (segment.empty() || segment == L"..") return false;
            segment.clear();
        } else {
            if (c < 32 || c == L':') return false;
            segment += c;
        }
    }
    return true;
}

std::string uuid_hex() {
    static std::mt19937_64 rng(std::random_device{}());
    static const char* hex = "0123456789abcdef";
    char buf[33];
    for (int i = 0; i < 32; ++i) buf[i] = hex[rng() % 16];
    buf[32] = '\0';
    return std::string(buf);
}

std::string replace_macro(const std::string& arg, const std::string& key, const std::string& value) {
    std::string out = arg;
    std::string macro = "${" + key + "}";
    for (;;) {
        size_t pos = out.find(macro);
        if (pos == std::string::npos) break;
        out.replace(pos, macro.size(), value);
    }
    return out;
}

bool write_bytes(const std::wstring& path, const std::vector<uint8_t>& bytes, std::string* err) {
    std::ofstream of(path, std::ios::binary | std::ios::trunc);
    if (!of.is_open()) {
        if (err) *err = "cannot write " + net::to_utf8(path);
        return false;
    }
    of.write(reinterpret_cast<const char*>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
    of.close();
    return true;
}

std::string fetch_text(const std::wstring& url, std::string* err,
                       const std::string& expected_sha1 = std::string(), int64_t expected_size = -1) {
    std::vector<uint8_t> bytes;
    if (!net::get(url, bytes, err)) return std::string();
    if (expected_size >= 0 && static_cast<int64_t>(bytes.size()) != expected_size) {
        if (err) *err = "metadata size mismatch";
        return std::string();
    }
    if (!expected_sha1.empty() && net::sha1_hex(bytes.data(), bytes.size()) != expected_sha1) {
        if (err) *err = "metadata sha1 mismatch";
        return std::string();
    }
    return std::string(bytes.begin(), bytes.end());
}

bool download_maven_artifact(const std::wstring& url, const std::wstring& path,
                             std::function<bool(uint64_t, uint64_t)> progress,
                             std::string* err) {
    std::vector<uint8_t> checksum_bytes;
    if (!net::get(url + L".sha1", checksum_bytes, err)) return false;
    std::string checksum(checksum_bytes.begin(), checksum_bytes.end());
    while (!checksum.empty() && std::isspace(static_cast<unsigned char>(checksum.back())))
        checksum.pop_back();
    size_t first_space = checksum.find_first_of(" \t\r\n");
    if (first_space != std::string::npos) checksum.resize(first_space);
    if (checksum.size() != 40 ||
        !std::all_of(checksum.begin(), checksum.end(), [](unsigned char c) { return std::isxdigit(c) != 0; })) {
        if (err) *err = "Maven checksum is invalid";
        return false;
    }
    for (char& c : checksum) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return net::download(url, path, std::move(progress), err, checksum, -1);
}

std::wstring resolve_java_impl(int major, const launch::Options& opt,
                               const std::function<void(const std::wstring&)>& log,
                               std::string* err) {
    if (!opt.java_path.empty()) {
        std::wstring exe = opt.java_path;
        size_t slash = exe.find_last_of(L"\\/");
        if (slash != std::wstring::npos &&
            exe.substr(slash + 1) == L"java.exe") {
            exe = exe.substr(0, slash);
        }
        if (net::file_exists(exe + L"\\bin\\java.exe")) {
            log(L"[launch] per-instance java at " + exe);
            return exe;
        }
    }
    for (const auto& kv : opt.java_overrides) {
        if (kv.first == major && !kv.second.empty() &&
            net::file_exists(kv.second + L"\\bin\\java.exe")) {
            return kv.second;
        }
    }
    const std::wstring java_root = java::managed_root(opt.java_cache_dir);
    net::mkdirs(java_root);
    return java::resolve(major, java::scan_installed(), java_root,
                         [&](uint64_t done, uint64_t total) {
                             static int last = -1;
                             int pct = total > 0 ? static_cast<int>(done * 100 / total) : -1;
                             if (pct >= 0 && pct != last && pct % 10 == 0) {
                                 last = pct;
                                 log(L"[launch] java download " + std::to_wstring(pct) + L"%");
                             }
                             return true;
                         },
                         err);
}

int run_installer(const std::wstring& java_home, const std::wstring& installer,
                  const std::wstring& target, const std::function<void(const std::wstring&)>& log,
                  std::string* err) {
    // Forge/NeoForge installers require a launcher_profiles.json to exist in the target
    std::wstring profiles = target + L"\\launcher_profiles.json";
    if (!net::file_exists(profiles)) {
        Json root = Json::obj();
        root.set("authenticationDatabase", Json::obj());
        root.set("clientToken", Json::str("a0000000000000000000000000000000"));
        Json launcher_version = Json::obj();
        launcher_version.set("format", Json::num(21));
        launcher_version.set("name", Json::str("Amalgam"));
        launcher_version.set("profilesFormat", Json::num(2));
        root.set("launcherVersion", launcher_version);
        Json profile = Json::obj();
        profile.set("created", Json::str("2020-01-01T00:00:00.000Z"));
        profile.set("icon", Json::str("Creeper"));
        profile.set("lastUsed", Json::str("2020-01-01T00:00:00.000Z"));
        profile.set("lastVersionId", Json::str(""));
        profile.set("name", Json::str("Amalgam"));
        profile.set("type", Json::str("custom"));
        profile.set("gameDir", Json::str(net::to_utf8(target)));
        Json profile_map = Json::obj();
        profile_map.set("Amalgam", profile);
        root.set("profiles", profile_map);
        Json selected = Json::obj();
        selected.set("account", Json::str("a0000000000000000000000000000000"));
        selected.set("profile", Json::str("a0000000000000000000000000000000"));
        root.set("selectedUser", selected);
        std::string json_error;
        if (!json_write_file(profiles, root, &json_error)) {
            if (err) *err = json_error;
            return -1;
        }
    }
    std::wstring exe = java_home + L"\\bin\\java.exe";
    std::wstring args = L"-jar " + extract::quote(installer) + L" --installClient " +
                        extract::quote(target);
    log(L"[launch] running loader installer");
    int code = -1;
    std::string installer_output;
    if (!extract::run_command(exe, args, target, 300000, &code, err, &installer_output)) return code;
    std::istringstream lines(installer_output);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() > 2000) line.resize(2000);
        if (!line.empty()) log(L"[installer] " + net::to_wide(line));
    }
    return code;
}

std::wstring find_installed_version_json(const std::wstring& instance, const std::string& loader,
                                         const std::string& loader_version) {
    std::wstring versions_dir = instance + L"\\versions";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((versions_dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return std::wstring();
    std::wstring best;
    std::wstring loader_text = net::to_wide(loader);
    std::wstring version_text = net::to_wide(loader_version);
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        std::wstring cand = versions_dir + L"\\" + fd.cFileName + L"\\" + fd.cFileName + L".json";
        std::wstring name = fd.cFileName;
        bool matches_loader = name.find(loader_text) != std::wstring::npos;
        bool matches_version = version_text.empty() || name.find(version_text) != std::wstring::npos;
        if (net::file_exists(cand) && matches_loader && matches_version) {
            best = cand;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (!best.empty()) return best;
    // Some Forge installers encode the loader in a different order; never select
    // the vanilla profile when a loader profile is present.
    h = FindFirstFileW((versions_dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return best;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY && wcscmp(fd.cFileName, L".") != 0 &&
            wcscmp(fd.cFileName, L"..") != 0) {
            std::wstring name = fd.cFileName;
            std::wstring cand = versions_dir + L"\\" + name + L"\\" + name + L".json";
            if (net::file_exists(cand) && name.find(loader_text) != std::wstring::npos) {
                best = cand;
                break;
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return best;
}

}  // namespace

bool run(const Options& opt, const std::function<void(const std::wstring&)>& log, Result* out,
         std::string* err) {
    using namespace model;

    if (opt.mc_id.empty()) {
        if (err) *err = "no version selected";
        return false;
    }

    auth::Account authenticated_account;
    std::string account_error;
    bool authenticated = false;
    // Direct JVM launches may be used for local preparation or dry runs. Only
    // an explicit caller request may load/refresh Minecraft credentials; the
    // normal Play path registers the profile and lets the official launcher
    // own Microsoft authentication.
    if (opt.require_account) {
        authenticated = auth::ensure_valid(authenticated_account, log, &account_error);
    }
    if (!authenticated && opt.require_account) {
        if (err) *err = account_error.empty() ? "Minecraft account required for direct launch; use the official Minecraft Launcher for sign-in" : account_error;
        return false;
    }

    int game_rank = rank(opt.mc_id);
    std::string loader = opt.loader;
    if (loader == "auto") {
        // An id that does not parse as a version cannot be satisfied by any
        // loader; falling back to forge would report a loader problem for what
        // is really a typo in the version.
        if (game_rank == 0) {
            if (err) *err = "unknown Minecraft version: " + opt.mc_id;
            return false;
        }
        loader = game_rank >= 1140 ? "fabric" : "forge";
    }
    if (loader == "vanilla") loader.clear();

    std::string loader_version;
    if (!loader.empty()) {
        if (!opt.loader_version.empty()) {
            loader_version = net::to_utf8(opt.loader_version);
            log(L"[launch] loader " + net::to_wide(loader) + L" " + opt.loader_version +
                L" (pinned)");
            if (out) out->loader_version = opt.loader_version;
        } else if (resolve_loader_version(opt.mc_id, loader, &loader_version, err)) {
            log(L"[launch] loader " + net::to_wide(loader) + L" " + net::to_wide(loader_version));
            if (out) out->loader_version = net::to_wide(loader_version);
        } else {
            log(L"[launch] requested loader unavailable: " + net::to_wide(*err));
            return false;
        }
    }

    std::wstring instance = opt.instance_dir.empty()
                                ? opt.base_dir + L"\\instances\\" + net::to_wide(opt.mc_id) +
                                      (loader.empty() ? L"" : L"-" + net::to_wide(loader))
                                : opt.instance_dir;
    net::mkdirs(instance);
    if (out) out->instance_dir = instance;

    std::wstring json_path;
    std::wstring local_jar;
    Json profile_json;

    if (is_installer_loader(loader)) {
        std::wstring pin_path = instance + L"\\.amalgam-install.json";
        Json pin;
        std::string pin_err;
        bool installed = json_parse_file(pin_path, pin, &pin_err) &&
                         pin.get("mc_id").as_str() == opt.mc_id &&
                         pin.get("loader").as_str() == loader &&
                         pin.get("loader_version").as_str() == loader_version;
        json_path = find_installed_version_json(instance, loader, loader_version);
        if (installed && !json_path.empty()) {
            log(L"[launch] loader already installed, skipping installer");
        } else {
            std::wstring java_home = resolve_java_impl(default_java_major(game_rank), opt, log, err);
            if (java_home.empty()) {
                if (err && err->empty()) *err = "no Java for loader installer";
                return false;
            }
            log(L"[launch] installer java at " + java_home);
            std::wstring installer_path = instance + L"\\loader-installer.jar";
            if (!net::file_exists(installer_path) || net::file_size(installer_path) == 0) {
                std::string url = loader_installer_url(loader, loader_version);
                if (url.empty()) {
                    if (err) *err = "no installer url";
                    return false;
                }
                log(L"[launch] downloading loader installer");
                if (!download_maven_artifact(net::to_wide(url), installer_path, nullptr, err)) return false;
            }
            int code = run_installer(java_home, installer_path, instance, log, err);
            json_path = find_installed_version_json(instance, loader, loader_version);
            // Installers may exit nonzero (e.g. failing to inject the launcher profile) but still
            // produce the version json + artifacts; treat that as success.
            if (code != 0 && json_path.empty()) {
                if (err && err->empty()) *err = "installer exited " + std::to_string(code);
                return false;
            }
            if (json_path.empty()) {
                if (err) *err = "installer produced no version json";
                return false;
            }
            if (code != 0) {
                log(L"[launch] installer finished with exit " + std::to_wstring(code) +
                    L", using produced version json");
            }
            Json p = Json::obj();
            p.set("mc_id", Json::str(opt.mc_id));
            p.set("loader", Json::str(loader));
            p.set("loader_version", Json::str(loader_version));
            std::string write_err;
            if (!json_write_file(pin_path, p, &write_err)) {
                if (err) *err = write_err;
                return false;
            }
            log(L"[launch] installer state pinned");
        }
        size_t dir_end = json_path.find_last_of(L"\\");
        std::wstring jdir = json_path.substr(0, dir_end);
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((jdir + L"\\*.jar").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            local_jar = jdir + L"\\" + fd.cFileName;
            FindClose(h);
        }
        // Forge installers drop the vanilla client jar in versions/<mc>/
        std::wstring base_jar = instance + L"\\versions\\" + net::to_wide(opt.mc_id) + L"\\" +
                                net::to_wide(opt.mc_id) + L".jar";
        if (net::file_exists(base_jar) && net::file_size(base_jar) > 0) local_jar = base_jar;
        if (!json_parse_file(json_path, profile_json, err)) return false;
    } else {
        json_path = instance + L"\\profile.json";
        bool use_cached = json_parse_file(json_path, profile_json, nullptr);
        Json pin;
        std::string pin_error;
        if (use_cached && json_parse_file(instance + L"\\.amalgam-profile.json", pin, &pin_error)) {
            use_cached = pin.get("mc_id").as_str() == opt.mc_id &&
                         pin.get("loader").as_str() == loader &&
                         pin.get("loader_version").as_str() == loader_version;
        }
        if (!use_cached) {
            if (!loader.empty()) {
                std::string url = loader_profile_url(opt.mc_id, loader, loader_version);
                if (url.empty()) {
                    if (err) *err = "no loader profile url";
                    return false;
                }
                std::string text = fetch_text(net::to_wide(url), err);
                if (text.empty()) return false;
                if (!write_bytes(json_path, std::vector<uint8_t>(text.begin(), text.end()), err)) return false;
            } else {
                std::vector<ManifestEntry> manifest = fetch_manifest(err);
                const ManifestEntry* entry = nullptr;
                for (const ManifestEntry& e : manifest) {
                    if (e.id == opt.mc_id) {
                        entry = &e;
                        break;
                    }
                }
                if (!entry) {
                    if (err) *err = "version not in manifest";
                    return false;
                }
                std::string text = fetch_text(net::to_wide(entry->url), err, entry->sha1, entry->size);
                if (text.empty()) return false;
                if (!write_bytes(json_path, std::vector<uint8_t>(text.begin(), text.end()), err)) return false;
            }
            Json profile_pin = Json::obj();
            profile_pin.set("mc_id", Json::str(opt.mc_id));
            profile_pin.set("loader", Json::str(loader));
            profile_pin.set("loader_version", Json::str(loader_version));
            std::string pin_write_error;
            if (!json_write_file(instance + L"\\.amalgam-profile.json", profile_pin, &pin_write_error)) {
                if (err) *err = pin_write_error;
                return false;
            }
            if (!json_parse_file(json_path, profile_json, err)) return false;
        } else {
            log(L"[launch] cached profile.json reused");
        }
    }

    VersionJson vj;
    if (is_installer_loader(loader)) {
        std::string inherits = profile_json.get("inheritsFrom").as_str();
        if (!inherits.empty()) {
            log(L"[launch] inherits from " + net::to_wide(inherits));
            if (!merge_inherited(inherits, profile_json, &vj, err)) return false;
        } else {
            parse_version(profile_json, vj);
        }
    } else {
        std::string inherits = profile_json.get("inheritsFrom").as_str();
        if (!inherits.empty()) {
            log(L"[launch] inherits from " + net::to_wide(inherits));
            if (!merge_inherited(inherits, profile_json, &vj, err)) return false;
        } else {
            parse_version(profile_json, vj);
        }
    }
    if (vj.main_class.empty()) {
        if (err) *err = "version json has no mainClass";
        return false;
    }

    int java_major = vj.java_major > 0 ? vj.java_major : default_java_major(game_rank);
    log(L"[launch] requires Java " + std::to_wstring(java_major));
    std::wstring java_home = resolve_java_impl(java_major, opt, log, err);
    if (java_home.empty()) {
        if (err && err->empty()) *err = "no usable Java " + std::to_string(java_major);
        return false;
    }
    if (out) out->java_home = java_home;
    log(L"[launch] java at " + java_home);

    std::wstring libs_dir = opt.base_dir + L"\\libraries";
    net::mkdirs(libs_dir);
    std::wstring natives_dir = instance + L"\\natives";
    net::mkdirs(natives_dir);

    std::wstring jar_path = local_jar;
    if (jar_path.empty()) jar_path = instance + L"\\" + net::to_wide(opt.mc_id) + L".jar";
    if (net::verify_file(jar_path, vj.client.sha1, vj.client.size)) {
        log(L"[launch] client jar present and verified");
    } else if (!vj.client.url.empty()) {
        log(L"[launch] downloading client jar");
        if (!net::download(net::to_wide(vj.client.url), jar_path, nullptr, err, vj.client.sha1,
                           vj.client.size)) {
            log(L"[launch]   client jar failed: " + net::to_wide(*err));
            return false;
        }
    } else if (local_jar.empty()) {
        log(L"[launch] no client jar source, continuing");
    }

    std::vector<std::wstring> classpath;
    std::wstring instance_libs = instance + L"\\libraries";
    for (const Lib& lib : vj.libraries) {
        if (!lib.natives.empty()) {
            const Download* native = nullptr;
            for (const auto& candidate : lib.natives) {
                if (candidate.first.find("natives-windows") != std::string::npos) {
                    native = &candidate.second;
                    break;
                }
            }
            if (native) {
                if (!safe_local_relative(native_rel(net::to_wide(native->path)))) {
                    if (err) *err = "loader supplied an unsafe native path";
                    return false;
                }
                std::wstring native_rel_path = native_rel(net::to_wide(native->path));
                std::wstring native_dst = libs_dir + L"\\" + native_rel_path;
                if (!net::verify_file(native_dst, native->sha1, native->size)) {
                    if (!net::mkdirs(extract::parent_of(native_dst)) ||
                        !net::download(net::to_wide(native->url), native_dst, nullptr, err,
                                       native->sha1, native->size)) {
                        log(L"[launch] native download failed: " + native_rel_path);
                        return false;
                    }
                }
                std::string native_error;
                if (!extract::zip(native_dst, natives_dir, &native_error)) {
                    if (err) *err = native_error;
                    return false;
                }
                log(L"[launch] extracted native classifier " + native_rel_path);
            }
        }
        bool is_native = lib.name.find(":natives-windows") != std::string::npos;
        std::wstring rel = lib.artifact.path.empty() ? lib_path_from_name(lib.name)
                                                     : net::to_wide(lib.artifact.path);
        std::wstring rel_url = rel;
        for (wchar_t& c : rel_url) {
            if (c == L'\\') c = L'/';
        }
        rel = native_rel(rel);
        if (!rel.empty() && !safe_local_relative(rel)) {
            if (err) *err = "loader supplied an unsafe library path";
            return false;
        }
        if (rel.empty() && lib.artifact.url.empty() && lib.maven_url.empty()) continue;
        std::wstring dst = libs_dir + L"\\" + rel;
        if (!net::verify_file(dst, lib.artifact.sha1, lib.artifact.size)) {
            // Forge/NeoForge installers place libs in <instance>/libraries with empty urls
            std::wstring ilocal = instance_libs + L"\\" + rel;
            if (net::verify_file(ilocal, lib.artifact.sha1, lib.artifact.size)) {
                dst = ilocal;
            } else {
                std::wstring url;
                if (!lib.artifact.url.empty()) {
                    url = net::to_wide(lib.artifact.url);
                } else if (!lib.maven_url.empty()) {
                    url = net::to_wide(lib.maven_url) + rel_url;
                }
                if (!url.empty()) {
                    net::mkdirs(extract::parent_of(dst));
                    if (!net::download(url, dst, nullptr, err, lib.artifact.sha1,
                                       lib.artifact.size)) {
                        log(L"[launch]   lib download failed: " + rel + L" " +
                            net::to_wide(*err));
                        return false;
                    }
                } else {
                    log(L"[launch]   lib missing without url: " + rel);
                    if (!is_native && lib.natives.empty()) {
                        if (err) *err = "required library unavailable: " + net::to_utf8(rel);
                        return false;
                    }
                }
            }
        }
        if (net::file_exists(dst)) {
            if (is_native) {
                std::string ex_err;
                if (!extract::zip(dst, natives_dir, &ex_err)) {
                    if (err) *err = ex_err.empty() ? "native extraction failed" : ex_err;
                    return false;
                }
                log(L"[launch] extracted natives from " + rel);
            } else {
                classpath.push_back(dst);
            }
        }
    }
    classpath.push_back(jar_path);

    if (!vj.assets_name.empty()) {
        std::wstring idx_path = opt.assets_dir + L"\\indexes\\" + net::to_wide(vj.assets_name) +
                                L".json";
        net::mkdirs(extract::parent_of(idx_path));
        if (!net::verify_file(idx_path, vj.assets_index.sha1, vj.assets_index.size)) {
            if (vj.assets_index.url.empty()) {
                log(L"[launch] no asset index url for " + net::to_wide(vj.assets_name));
            } else {
                log(L"[launch] downloading asset index " + net::to_wide(vj.assets_name));
                if (!net::download(net::to_wide(vj.assets_index.url), idx_path, nullptr, err,
                                   vj.assets_index.sha1, vj.assets_index.size)) return false;
            }
        }
        Json idx;
        std::string idx_err;
        if (json_parse_file(idx_path, idx, &idx_err)) {
            bool virtual_index = idx.get("virtual").as_bool(false);
            int total = static_cast<int>(idx.get("objects").size());
            int done = 0;
            for (const Json::Pair& kv2 : idx.get("objects").pairs()) {
                std::string hash = kv2.second.get("hash").as_str();
                int64_t obj_size = kv2.second.get("size").as_int(-1);
                ++done;
                if (hash.empty()) continue;
                std::wstring obj_path = opt.assets_dir + L"\\objects\\" +
                                        net::to_wide(hash.substr(0, 2)) + L"\\" +
                                        net::to_wide(hash);
                if (!net::verify_file(obj_path, hash, obj_size)) {
                    net::mkdirs(extract::parent_of(obj_path));
                    std::wstring url = L"https://resources.download.minecraft.net/" +
                                       net::to_wide(hash.substr(0, 2)) + L"/" +
                                       net::to_wide(hash);
                    if (!net::download(url, obj_path, nullptr, err, hash, obj_size)) {
                        log(L"[launch]   asset failed: " + net::to_wide(kv2.first) + L" " +
                            net::to_wide(*err));
                        return false;
                    }
                }
                if (!virtual_index) {
                    std::wstring dst = opt.assets_dir + L"\\" + net::to_wide(vj.assets_name) +
                                       L"\\" + net::to_wide(kv2.first);
                    if (!net::file_exists(dst)) {
                        net::mkdirs(extract::parent_of(dst));
                        if (!CopyFileW(obj_path.c_str(), dst.c_str(), FALSE)) {
                            if (err) *err = "cannot materialize asset: " + kv2.first;
                            return false;
                        }
                    }
                }
            }
            if (total > 0) log(L"[launch] assets ok (" + std::to_wstring(total) + L" entries)");
        }
    }

    // Real play requires an authenticated account and therefore replaces this
    // dry-run fallback with the official Minecraft profile name below.
    std::wstring launch_username = opt.username.empty() ? L"Player" : opt.username;
    std::string launch_uuid = opt.auth_uuid.empty() ? uuid_hex() : opt.auth_uuid;
    std::string launch_access_token = opt.auth_access_token.empty() ? "0" : opt.auth_access_token;
    std::string launch_session = opt.auth_session.empty() ? launch_access_token : opt.auth_session;
    std::string launch_user_type = opt.auth_user_type.empty() ? "legacy" : opt.auth_user_type;
    if (authenticated) {
        launch_username = net::to_wide(authenticated_account.username);
        launch_uuid = authenticated_account.uuid;
        launch_access_token = authenticated_account.access_token;
        launch_session = authenticated_account.access_token;
        launch_user_type = "msa";
        log(L"[launch] using authenticated account " + launch_username);
    } else if (!account_error.empty()) {
        log(L"[launch] authenticated account unavailable: " + net::to_wide(account_error));
    }

    std::vector<std::wstring> jvm;
    if (opt.addon && (opt.dll_path.empty() || !net::file_exists(opt.dll_path))) {
        if (err) *err = "amalgam.dll is missing; client injection cannot continue";
        return false;
    }
    std::wstring library_macro_dir = is_installer_loader(loader) ? instance_libs : libs_dir;
    if (opt.addon && !opt.dll_path.empty() && net::file_exists(opt.dll_path)) {
        jvm.push_back(L"-agentpath:" + opt.dll_path);
        jvm.push_back(L"-Damalgam.dll.path=" + opt.dll_path);
        log(L"[launch] injecting addon dll");
        std::wstring mods_dir = instance + L"\\mods";
        net::mkdirs(mods_dir);
        std::wstring key = loader.empty() ? L"vanilla" : net::to_wide(loader);
        std::wstring bridge = opt.bridges_dir + L"\\amalgam-" + key + L"-" +
                              net::to_wide(opt.mc_id) + L".jar";
        if (!net::file_exists(bridge) && loader == "quilt") {
            // Quilt Loader runs Fabric mods (fabric.mod.json) natively; the fabric bridge
            // jar is valid under quilt too.
            std::wstring fb = opt.bridges_dir + L"\\amalgam-fabric-" + net::to_wide(opt.mc_id) +
                              L".jar";
            if (net::file_exists(fb)) bridge = fb;
        }
        if (net::file_exists(bridge)) {
            std::wstring target = mods_dir + L"\\amalgam.jar";
            if (!CopyFileW(bridge.c_str(), target.c_str(), FALSE) &&
                !CopyFileW(bridge.c_str(), target.c_str(), TRUE)) {
                if (err) *err = "cannot copy bridge mod";
                return false;
            }
            log(L"[launch] bridge mod copied");
        } else {
            log(L"[launch] no bridge jar for this version (menu-only mode)");
            if (opt.require_account && opt.addon) {
                if (err) *err = "no Amalgam bridge is available for this loader/version";
                return false;
            }
        }
        if (loader == "fabric" || loader == "quilt") {
            // Quilt Loader loads fabric-api.jar directly (Quilted Fabric API only covers
            // <=1.20.1; for 1.21+ the plain fabric-api jar is the correct dependency).
            std::wstring api_jar = mods_dir + L"\\fabric-api.jar";
            std::wstring api_url;
            std::string api_sha1;
            int64_t api_size = -1;
            {
                std::vector<uint8_t> resp;
                std::string merr;
                std::wstring query =
                    L"https://api.modrinth.com/v2/project/fabric-api/version?game_versions=%5B%22" +
                    net::to_wide(opt.mc_id) + L"%22%5D&loaders=%5B%22fabric%22%5D";
                if (net::get(query, resp, &merr)) {
                    std::string text(resp.begin(), resp.end());
                    Json j = Json::parse(text, &merr);
                    if (j.is(Json::Type::Arr) && j.size() > 0) {
                        const Json& first = j.at(0);
                        const Json* files = first.find("files");
                        if (files && files->is(Json::Type::Arr) && files->size() > 0) {
                            const Json& f = files->at(0);
                            api_url = net::to_wide(f.get("url").as_str());
                            api_sha1 = f.get("hashes").get("sha1").as_str();
                            api_size = f.get("size").as_int(-1);
                        }
                    }
                }
            }
            if (api_url.empty()) {
                if (err) *err = "no Fabric API version for " + opt.mc_id;
                return false;
            }
            if (net::verify_file(api_jar, api_sha1, api_size)) {
                log(L"[launch] verified cached fabric-api");
            } else {
                log(L"[launch] downloading fabric-api for " + net::to_wide(opt.mc_id));
                if (net::download(api_url, api_jar, nullptr, err, api_sha1, api_size)) {
                    log(L"[launch] fabric-api downloaded");
                } else {
                    if (err && err->empty()) *err = "Fabric API download failed";
                    return false;
                }
            }
        }
    }

    const std::string performance_profile = performance::normalize_profile(opt.performance_profile);
    const performance::Tuning tuning = performance::make_tuning(
        performance_profile, performance::physical_memory_mb(), java_major);
    const bool explicit_performance_profile = performance_profile != "auto" &&
                                              performance_profile != "custom";
    int heap_mb = opt.memory_mb > 0 && !explicit_performance_profile
                      ? opt.memory_mb
                      : tuning.heap_mb;
    if (heap_mb <= 0) heap_mb = game_rank >= 1170 ? 4096 : 2048;
    jvm.push_back(L"-Xmx" + std::to_wstring(heap_mb) + L"m");
    jvm.push_back(L"-Djava.library.path=" + natives_dir);
    if (explicit_performance_profile) {
        std::string performance_error;
        if (!performance::apply_game_options(instance, performance_profile, &performance_error)) {
            if (err) *err = performance_error;
            return false;
        }
        log(L"[launch] performance profile: " + net::to_wide(performance::profile_label(performance_profile)));
    }
    for (const std::string& arg : tuning.jvm_args) jvm.push_back(net::to_wide(arg));
    for (const std::string& a : vj.jvm_args) {
        if (a.rfind("-Xmx", 0) == 0 || a.rfind("-Xms", 0) == 0 ||
            a.rfind("-Djava.library.path", 0) == 0 || a == "-cp" || a == "-classpath") {
            continue;
        }
        std::string s = a;
        s = replace_macro(s, "natives_directory", net::to_utf8(natives_dir));
        s = replace_macro(s, "library_directory", net::to_utf8(library_macro_dir));
        s = replace_macro(s, "classpath_separator", ";");
        s = replace_macro(s, "version_name", opt.mc_id);
        if (s.find("${") != std::string::npos) continue;
        jvm.push_back(net::to_wide(s));
    }
    if (!opt.extra_jvm.empty()) {
        std::wstring extra = net::to_wide(opt.extra_jvm);
        size_t pos = 0;
        while (pos <= extra.size()) {
            size_t sp = extra.find(L' ', pos);
            if (sp == std::wstring::npos) sp = extra.size();
            if (sp > pos) jvm.push_back(extra.substr(pos, sp - pos));
            if (sp == extra.size()) break;
            pos = sp + 1;
        }
    }
    if (is_installer_loader(loader)) {
        std::vector<std::wstring> module_paths;
        for (size_t i = 0; i + 1 < jvm.size(); ++i) {
            if (jvm[i] != L"-p" && jvm[i] != L"--module-path") continue;
            std::wstring value = jvm[i + 1];
            size_t start = 0;
            while (start <= value.size()) {
                size_t end = value.find(L';', start);
                std::wstring path = value.substr(start, end == std::wstring::npos
                                                           ? std::wstring::npos : end - start);
                for (wchar_t& c : path) if (c == L'/') c = L'\\';
                if (!path.empty()) module_paths.push_back(path);
                if (end == std::wstring::npos) break;
                start = end + 1;
            }
        }
        classpath.erase(std::remove_if(classpath.begin(), classpath.end(),
                                       [&](const std::wstring& path) {
            std::wstring normalized = path;
            for (wchar_t& c : normalized) if (c == L'/') c = L'\\';
            return std::find(module_paths.begin(), module_paths.end(), normalized) != module_paths.end();
        }), classpath.end());
    }
    jvm.push_back(L"-cp");
    jvm.push_back(join_classpath(classpath));

    std::vector<std::wstring> game;
    for (const std::string& a : vj.game_args) {
        std::string s = a;
        s = replace_macro(s, "auth_player_name", net::to_utf8(launch_username));
        s = replace_macro(s, "version_name", opt.mc_id);
        s = replace_macro(s, "assets_index_name", vj.assets_name);
        s = replace_macro(s, "game_directory", net::to_utf8(instance));
        s = replace_macro(s, "assets_root", net::to_utf8(opt.assets_dir));
        s = replace_macro(s, "game_assets", net::to_utf8(opt.assets_dir));
        s = replace_macro(s, "auth_uuid", launch_uuid);
        s = replace_macro(s, "auth_access_token", launch_access_token);
        s = replace_macro(s, "auth_session", launch_session);
        s = replace_macro(s, "user_type", launch_user_type);
        s = replace_macro(s, "user_properties", "{}");
        s = replace_macro(s, "version_type", "release");
        s = replace_macro(s, "launcher_name", "Amalgam");
        s = replace_macro(s, "launcher_version", "1");
        s = replace_macro(s, "resolution_width", std::to_string(opt.width));
        s = replace_macro(s, "resolution_height", std::to_string(opt.height));
        if (s.find("${") != std::string::npos) continue;
        game.push_back(net::to_wide(s));
    }
    if (!opt.test_server.empty() && game_rank >= 1200) {
        game.push_back(L"--quickPlayMultiplayer");
        game.push_back(opt.test_server);
    }
    auto add_default_arg = [&](const wchar_t* name, const std::wstring& value) {
        if (std::find(game.begin(), game.end(), name) != game.end()) return;
        game.push_back(name);
        game.push_back(value);
    };
    add_default_arg(L"--version", net::to_wide(opt.mc_id));
    add_default_arg(L"--gameDir", instance);
    add_default_arg(L"--assetsDir", opt.assets_dir);
    add_default_arg(L"--assetIndex", net::to_wide(vj.assets_name));
    add_default_arg(L"--username", launch_username);
    add_default_arg(L"--uuid", net::to_wide(launch_uuid));
    add_default_arg(L"--accessToken", net::to_wide(launch_access_token));
    add_default_arg(L"--userType", net::to_wide(launch_user_type));

    std::vector<std::wstring> all;
    all.push_back(java_home + L"\\bin\\" + (opt.wait_for_exit ? L"java.exe" : L"javaw.exe"));
    all.insert(all.end(), jvm.begin(), jvm.end());
    all.push_back(net::to_wide(vj.main_class));
    all.insert(all.end(), game.begin(), game.end());

    std::wstring cmdline = join(all);
    if (out) out->command_line = cmdline;

    std::wstring mutable_cmd(cmdline.size() + 1, L'\0');
    wcscpy_s(mutable_cmd.data(), mutable_cmd.size(), cmdline.c_str());
    if (opt.dry_run) {
        if (out) out->command_line = cmdline;
        log(L"[launch] dry run, not spawning game");
        return true;
    }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags |= STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    HANDLE child_log = INVALID_HANDLE_VALUE;
    std::wstring child_log_path;
    if (opt.wait_for_exit) {
        wchar_t temp_dir[MAX_PATH]{};
        DWORD temp_len = GetTempPathW(MAX_PATH, temp_dir);
        if (temp_len > 0 && temp_len < MAX_PATH) {
            child_log_path = std::wstring(temp_dir) + L"amalgam-game-" +
                             std::to_wstring(GetCurrentProcessId()) + L".log";
            SECURITY_ATTRIBUTES attributes{};
            attributes.nLength = sizeof(attributes);
            attributes.bInheritHandle = TRUE;
            child_log = CreateFileW(child_log_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                    &attributes, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (child_log != INVALID_HANDLE_VALUE) {
                si.dwFlags = STARTF_USESTDHANDLES;
                si.hStdOutput = child_log;
                si.hStdError = child_log;
            }
        }
    }
    if (!CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr,
                        child_log != INVALID_HANDLE_VALUE, CREATE_NO_WINDOW, nullptr,
                        instance.c_str(), &si, &pi)) {
        if (child_log != INVALID_HANDLE_VALUE) CloseHandle(child_log);
        if (!child_log_path.empty()) DeleteFileW(child_log_path.c_str());
        if (err) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "game spawn failed, err=%lu", GetLastError());
            *err = buf;
        }
        return false;
    }
    CloseHandle(pi.hThread);
    if (out) out->pid = pi.dwProcessId;
    log(L"[launch] started pid " + std::to_wstring(out ? out->pid : 0));
    if (opt.wait_for_exit) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        if (out) out->exit_code = static_cast<int>(code);
        log(L"[launch] game exited with code " + std::to_wstring(code));
        std::string child_text;
        if (child_log != INVALID_HANDLE_VALUE) CloseHandle(child_log);
        if (!child_log_path.empty()) {
            std::ifstream child_output(child_log_path, std::ios::binary);
            std::string line;
            while (std::getline(child_output, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.size() > 2000) line.resize(2000);
                child_text += line + "\n";
                log(L"[game] " + net::to_wide(line));
            }
            child_output.close();
            DeleteFileW(child_log_path.c_str());
        }
        if (code != 0) {
            std::string cause = "unknown startup/runtime failure";
            if (child_text.find("ModLoadingException") != std::string::npos ||
                child_text.find("missing mods.toml") != std::string::npos)
                cause = "mod loading or dependency failure";
            else if (child_text.find("MixinApplyError") != std::string::npos ||
                     child_text.find("mixin") != std::string::npos)
                cause = "mixin compatibility failure";
            else if (child_text.find("UnsupportedClassVersionError") != std::string::npos)
                cause = "wrong Java major version";
            else if (child_text.find("OpenAL") != std::string::npos)
                cause = "audio device/OpenAL initialization failure";
            else if (child_text.find("OutOfMemoryError") != std::string::npos)
                cause = "Java memory exhaustion";
            if (err) *err = "game exited with code " + std::to_string(code) + ": " + cause;
            CloseHandle(pi.hProcess);
            return false;
        }
    }
    CloseHandle(pi.hProcess);
    if (!opt.wait_for_exit && net::file_exists(instance + L"\\instance.json")) {
        instances::Instance inst;
        if (instances::load(instance, inst, nullptr)) {
            inst.last_played = static_cast<int64_t>(time(nullptr));
            std::string save_err;
            instances::save(inst, &save_err);
        }
    }
    return true;
}

}  // namespace aml::launch
