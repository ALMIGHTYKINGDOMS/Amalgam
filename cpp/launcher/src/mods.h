#pragma once

#include <cstdint>
#include <cctype>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace aml::mods {

using Progress = std::function<bool(uint64_t done, uint64_t total)>;

enum class ProjectType {
    Mod,
    Modpack,
    ResourcePack,
    Shader,
    Datapack,
    Plugin,
    Unknown,
};

struct SearchResult {
    std::string slug;
    std::string title;
    std::string description;
    std::string icon_url;
    ProjectType type = ProjectType::Unknown;
    int64_t downloads = 0;
    int64_t follows = 0;
    std::vector<std::string> loaders;
    std::vector<std::string> categories;
    std::string date_modified;
    std::string source = "modrinth";
};

struct DepInfo {
    std::string project_id;
    std::string version_id;
    bool required = true;
};

struct FileInfo {
    std::string id;       // modrinth version id / curseforge file id
    std::string filename;
    std::string url;
    std::vector<std::string> loaders;
    std::vector<std::string> game_versions;
    bool primary = false;
    int64_t size = 0;
    std::string sha1;
    std::string version_number;
    std::string version_name;
    std::string changelog;
    std::string date_published;
    std::vector<DepInfo> dependencies;
};

struct ModInfo {
    std::string slug;
    std::string title;
    std::string description;
    std::string icon_url;
    std::string body;
    std::string author;
    std::string license;
    std::string date_published;
    std::string date_updated;
    std::string source_url;
    std::string website_url;
    std::string issues_url;
    ProjectType type = ProjectType::Unknown;
    std::vector<std::string> loaders;
    std::vector<std::string> categories;
    std::vector<std::string> gallery_urls;
    std::vector<FileInfo> files;
};

// A provider release is the source of truth for a modpack's Minecraft target.
// The launcher may have a current profile/filter selected, but a new pack
// profile must be allowed to resolve its own loader and game version.
struct CompatibleRelease {
    std::string file_id;
    std::string loader;
    std::string game_version;
};

struct UpdateInfo {
    bool available = false;
    std::string current_version;
    std::string latest_version;
    std::string latest_file_id;
    std::string latest_filename;
    std::string latest_url;
    std::string latest_sha1;
    int64_t latest_size = 0;
    std::string latest_published;
    std::string changelog;
    std::string project_url;
};

struct UpdateEntry {
    std::string file;
    std::string project;
    std::string source;
    std::string current_version;
    std::string latest_version;
    std::string latest_file;
    std::string changelog;
};

struct ApiCfg {
    std::string modrinth_token;
    std::string curseforge_key;
    // Official builds do not ship the CurseForge provider key. When these
    // fields are populated the launcher sends the tightly-scoped request to
    // an Amalgam Edge Function that keeps CURSEFORGE_API_KEY server-side.
    std::string curseforge_proxy_url;
    std::string curseforge_proxy_key;
    std::string curseforge_proxy_access_token;
};

inline ApiCfg make_api_cfg(const std::string& modrinth_token,
                           const std::string& curseforge_key,
                           const std::string& supabase_url = {},
                           const std::string& supabase_publishable_key = {},
                           const std::string& supabase_access_token = {}) {
    ApiCfg cfg;
    cfg.modrinth_token = modrinth_token;
    cfg.curseforge_key = curseforge_key;
    cfg.curseforge_proxy_key = supabase_publishable_key;
    cfg.curseforge_proxy_access_token = supabase_access_token;
    if (!supabase_url.empty() && !supabase_publishable_key.empty()) {
        std::string root = supabase_url;
        while (root.size() > 1 && root.back() == '/') root.pop_back();
        cfg.curseforge_proxy_url = root + "/functions/v1/curseforge-catalog";
    }
    return cfg;
}

struct Match {
    std::string slug;   // project identifier
    std::string title;
    std::string source = "modrinth"; // "modrinth" | "curseforge"
    std::string loader;               // optional requested game loader
};

enum class Facet {
    Fabric = 0,
    Forge,
    NeoForge,
    Quilt,
    Bukkit,
    Spigot,
    Paper,
    Modpack,
    ResourcePack,
    Shader,
    Datapack,
    Optimization,
    Weapons,
    Armor,
    Cosmetic,
    Magic,
    Technology,
    Adventure,
    Storage,
    Food,
    Utility,
    BedrockAddon,
};

// Normalizes a provider label supplied by UI/AI input. Empty means unsupported.
std::string canonical_source(const std::string& source);
// Accept pasted CurseForge keys without surrounding whitespace, quotes, or
// an Authorization/header prefix.
inline std::string normalize_curseforge_key(const std::string& value) {
    size_t begin = 0;
    size_t end = value.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
    std::string normalized = value.substr(begin, end - begin);
    if (normalized.size() >= 2 && normalized.front() == '"' && normalized.back() == '"')
        normalized = normalized.substr(1, normalized.size() - 2);
    if (normalized.rfind("Bearer ", 0) == 0 || normalized.rfind("bearer ", 0) == 0)
        normalized.erase(0, 7);
    if (normalized.rfind("x-api-key:", 0) == 0 || normalized.rfind("X-API-Key:", 0) == 0)
        normalized.erase(0, 10);
    begin = 0;
    end = normalized.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(normalized[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(normalized[end - 1]))) --end;
    return normalized.substr(begin, end - begin);
}
// True when the public Supabase endpoint/key needed to reach the shared
// CurseForge proxy are present. A signed-in Amalgam access token is still
// required before the proxy can be used.
bool curseforge_proxy_configured(const ApiCfg& cfg);
// True when CurseForge can be reached either with a per-user key or through
// the authenticated Amalgam backend proxy.
bool curseforge_available(const ApiCfg& cfg);
// Internal provider request shared by catalog browsing and CurseForge pack
// import. The Edge Function validates the URL path before forwarding it.
bool curseforge_json(const ApiCfg& cfg, const std::string& url, std::string& json,
                     std::string* err = nullptr);
// Performs a small provider-specific catalog request without downloading content.
// Modrinth works without a token; CurseForge requires a user key or backend proxy.
bool test_provider(const ApiCfg& cfg, const std::string& source, std::string* err = nullptr);
bool search(const ApiCfg& cfg, const std::string& query, const std::string& loader,
            const std::string& game_version, Facet facet, std::vector<SearchResult>& out,
            std::string* err, int page = 0, const std::string& source = "");
bool project_files(const ApiCfg& cfg, const std::string& slug, const std::string& source,
                   ModInfo& out, std::string* err);
bool check_project_update(const ApiCfg& cfg, const std::string& slug, const std::string& source,
                          const std::string& loader, const std::string& game_version,
                          const std::string& current_version, UpdateInfo& out, std::string* err);
// resolve compatible file for loader+game version; returns empty id when none
std::string pick_file(const ModInfo& mod, const std::string& loader,
                      const std::string& game_version);
// Select the best downloadable release. Preferred values are soft hints:
// an exact game version is preferred, then an exact loader, with a safe
// fallback to the provider's newest release when either hint is unavailable.
bool select_compatible_release(const ModInfo& mod, const std::string& preferred_loader,
                               const std::string& preferred_game_version,
                               CompatibleRelease& out, std::string* err = nullptr);
// CurseForge may omit downloadUrl from a file listing; resolve it through the
// provider's authenticated download-url endpoint when necessary.
bool resolve_download_url(const ApiCfg& cfg, const std::string& project_id,
                          const std::string& source, FileInfo& file, std::string* err);
// recursively install mod + required dependencies into mods_dir (jar files)
bool install_mod(const ApiCfg& cfg, const std::string& slug, const std::string& source,
                 const std::string& loader, const std::string& game_version,
                 const std::wstring& mods_dir, std::vector<std::string>& log_lines,
                 std::string* err, const Progress& progress = {});
bool update_owned(const ApiCfg& cfg, const std::wstring& instance_dir, const std::string& loader,
                  const std::string& game_version, std::vector<std::string>& log_lines,
                  std::string* err, const std::set<std::string>& selected_files = {},
                  const Progress& progress = {});
bool preview_owned(const ApiCfg& cfg, const std::wstring& instance_dir, const std::string& loader,
                   const std::string& game_version, std::vector<UpdateEntry>& out,
                   std::string* err);

}  // namespace aml::mods
