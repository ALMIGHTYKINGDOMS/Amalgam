#include "bedrock.h"

#include "extract.h"
#include "json.h"
#include "net.h"

#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <random>
#include <chrono>
#include <cctype>
#include <mutex>
#include <set>

namespace aml::bedrock {

namespace {

// Known UWP package family + launch AUMID for Bedrock on Windows.  The
// Microsoft Store package registers the game entry point as `!Game`; `!App`
// is not present on current Minecraft for Windows installs.
const wchar_t* kFamily = L"Microsoft.MinecraftUWP_8wekyb3d8bbwe";
const wchar_t* kAumid = L"Microsoft.MinecraftUWP_8wekyb3d8bbwe!Game";

// Bedrock profile directory structure
const wchar_t* kProfilesDir = L"profiles";

std::wstring get_env(const wchar_t* name) {
    wchar_t buf[32768];
    DWORD n = GetEnvironmentVariableW(name, buf, 32768);
    if (n == 0 || n >= 32768) return L"";
    return buf;
}

// Modern Windows builds store Bedrock data under %APPDATA%\Minecraft Bedrock\Users\Shared.
std::wstring roaming_data_dir() {
    std::wstring base = get_env(L"APPDATA");
    if (base.empty()) return L"";
    std::wstring d = base + L"\\Minecraft Bedrock\\Users\\Shared\\games\\com.mojang";
    if (net::directory_exists(d)) return d;
    return L"";
}

// Older Windows 10 layout stored data inside the UWP package LocalState.
std::wstring uwp_data_dir() {
    std::wstring base = get_env(L"LOCALAPPDATA");
    if (base.empty()) return L"";
    const std::filesystem::path packages = std::filesystem::path(base) / L"Packages";
    std::error_code ec;
    if (std::filesystem::exists(packages, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(packages, ec)) {
            if (!entry.is_directory()) continue;
            const std::wstring name = entry.path().filename().wstring();
            if (name.find(L"Minecraft") == std::wstring::npos) continue;
            const std::wstring candidate = (entry.path() / L"LocalState" / L"games" / L"com.mojang").wstring();
            if (net::directory_exists(candidate)) return candidate;
        }
    }
    return L"";
}

// Generate a UUID v4 string
std::string generate_uuid() {
    static std::mt19937_64 gen(static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::uniform_int_distribution<uint64_t> dis;
    uint64_t v4[2];
    v4[0] = dis(gen);
    v4[1] = dis(gen);
    // Set version (4) and variant
    v4[0] = (v4[0] & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    v4[0] = (v4[0] & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;
    char buf[37];
    snprintf(buf, sizeof(buf), "%08llX-%04llX-%04llX-%04llX-%012llX",
             v4[0] >> 32, (v4[0] >> 16) & 0xFFFF, v4[0] & 0xFFFF,
             (v4[1] >> 48) & 0xFFFF, v4[1] & 0xFFFFFFFFFFFFULL);
    return std::string(buf);
}

// Hash function for duplicate detection
uint64_t pack_hash(const BedrockPackEntry& pack) {
    std::hash<std::string> h;
    return h(pack.uuid) ^ h(pack.version) ^ (static_cast<uint64_t>(pack.type) << 16);
}

bool query_package_registered(std::string* err) {
    std::string output;
    std::string command_error;
    std::wstring powershell = L"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
    std::wstring args = L"-NoProfile -NonInteractive -Command \"$p=Get-AppxPackage | "
                        L"Where-Object {$_.Name -eq 'Microsoft.MinecraftUWP' -or "
                        L"$_.PackageFamilyName -eq 'Microsoft.MinecraftUWP_8wekyb3d8bbwe'} | "
                        L"Select-Object -First 1 -ExpandProperty PackageFamilyName; if ($p) { $p }\"";
    if (!extract::run_capture(powershell, args, &output, &command_error)) {
        if (err) *err = command_error.empty()
            ? "Bedrock UWP package detection unavailable"
            : "Bedrock UWP package detection unavailable: " + command_error;
        return false;
    }

    std::string normalized = output;
    for (char& c : normalized)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (normalized.find("microsoft.minecraft") == std::string::npos) {
        if (err) *err = "Bedrock UWP package is not installed";
        return false;
    }
    return true;
}

bool cached_package_registered(std::string* err) {
    struct Cache {
        std::mutex mutex;
        uint64_t checked_at = 0;
        bool checked = false;
        bool registered = false;
        std::string error;
    };
    static Cache cache;

    const uint64_t now = GetTickCount64();
    {
        std::lock_guard<std::mutex> lock(cache.mutex);
        if (cache.checked && now - cache.checked_at < 1000) {
            if (err) *err = cache.error;
            return cache.registered;
        }
    }

    std::string probe_error;
    const bool registered = query_package_registered(&probe_error);
    {
        std::lock_guard<std::mutex> lock(cache.mutex);
        cache.checked = true;
        cache.checked_at = now;
        cache.registered = registered;
        cache.error = probe_error;
    }
    if (err) *err = probe_error;
    return registered;
}

bool is_extension(const std::wstring& path, const wchar_t* extension) {
    return _wcsicmp(std::filesystem::path(path).extension().c_str(), extension) == 0;
}

bool safe_path_component(const std::wstring& value) {
    if (value.empty() || value == L"." || value == L"..") return false;
    for (wchar_t c : value) {
        if (c < 32 || c == L'\\' || c == L'/' || c == L':' || c == L'*' ||
            c == L'?' || c == L'"' || c == L'<' || c == L'>' || c == L'|') {
            return false;
        }
    }
    return true;
}

std::string sanitized_component(const std::string& value, const char* fallback) {
    std::string out;
    for (unsigned char c : value) {
        if (c < 32 || c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|') {
            out.push_back('_');
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    if (out.empty() || out == "." || out == "..") out = fallback;
    return out;
}

bool absolute_path(const std::filesystem::path& input, std::filesystem::path& output,
                   std::string* err) {
    std::error_code ec;
    output = std::filesystem::absolute(input, ec);
    if (ec) {
        if (err) *err = "cannot resolve absolute path: " + ec.message();
        return false;
    }
    return true;
}

bool data_root_path(std::filesystem::path& output, std::string* err) {
    std::wstring raw = data_dir(err);
    if (raw.empty()) return false;
    return absolute_path(std::filesystem::path(raw), output, err);
}

bool profile_root_path(const BedrockProfile& target, std::filesystem::path& output,
                       std::string* err) {
    if (target.id.empty()) {
        if (err) *err = "a Bedrock profile must be selected";
        return false;
    }
    const std::wstring id = net::to_wide(target.id);
    if (!safe_path_component(id)) {
        if (err) *err = "invalid Bedrock profile id";
        return false;
    }

    std::filesystem::path root;
    if (!data_root_path(root, err)) return false;
    return absolute_path(root / kProfilesDir / id, output, err);
}

bool profile_backup_path(const BedrockProfile& profile, const BedrockBackupEntry& backup,
                         std::filesystem::path& output, std::string* err) {
    if (!backup.profile_id.empty() && backup.profile_id != profile.id) {
        if (err) *err = "backup does not belong to the selected Bedrock profile";
        return false;
    }
    if (backup.id.empty() || !safe_path_component(net::to_wide(backup.id))) {
        if (err) *err = "invalid Bedrock backup id";
        return false;
    }

    std::filesystem::path root;
    if (!profile_root_path(profile, root, err)) return false;
    return absolute_path(root / L"backups" / net::to_wide(backup.id), output, err);
}

bool ensure_directory(const std::filesystem::path& path, std::string* err,
                      const char* description) {
    if (!net::mkdirs(path.wstring())) {
        if (err) *err = std::string("cannot create ") + description + ": " + path.string();
        return false;
    }
    return true;
}

bool make_temp_directory(const wchar_t* prefix, std::filesystem::path& output,
                         std::string* err) {
    std::error_code ec;
    const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
    if (ec) {
        if (err) *err = "cannot locate temporary directory: " + ec.message();
        return false;
    }
    for (int attempt = 0; attempt < 16; ++attempt) {
        output = base / (std::wstring(prefix) + std::to_wstring(GetCurrentProcessId()) +
                         L"_" + std::to_wstring(GetTickCount64()) + L"_" +
                         std::to_wstring(attempt));
        ec.clear();
        if (std::filesystem::exists(output, ec)) {
            if (ec) break;
            continue;
        }
        if (net::mkdirs(output.wstring())) return true;
    }
    if (err) *err = "cannot create temporary extraction directory";
    return false;
}

bool remove_tree(const std::filesystem::path& path, std::string* err) {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    if (ec) {
        if (err) *err = "cannot remove temporary directory: " + ec.message();
        return false;
    }
    return true;
}

bool unique_child(const std::filesystem::path& parent, const std::string& preferred,
                  std::filesystem::path& output, std::string* err) {
    const std::string base = sanitized_component(preferred, "pack");
    std::error_code ec;
    for (int suffix = 0; suffix < 10000; ++suffix) {
        const std::string name = suffix == 0 ? base : base + "-" + std::to_string(suffix + 1);
        output = parent / net::to_wide(name);
        const bool exists = std::filesystem::exists(output, ec);
        if (ec) {
            if (err) *err = "cannot inspect target path: " + ec.message();
            return false;
        }
        if (!exists) return true;
    }
    if (err) *err = "too many duplicate Bedrock content names";
    return false;
}

bool validate_archive_signature(const std::wstring& archive_file, std::string* err) {
    if (!net::file_exists(archive_file)) {
        if (err) *err = "archive file not found";
        return false;
    }
    HANDLE input = CreateFileW(archive_file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    LARGE_INTEGER size{};
    const bool valid_size = input != INVALID_HANDLE_VALUE && GetFileSizeEx(input, &size) &&
                            size.QuadPart > 0 && size.QuadPart <= 256LL * 1024 * 1024;
    if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
    if (!valid_size) {
        if (err) *err = "archive is empty or exceeds the 256 MiB limit";
        return false;
    }

    std::ifstream archive(archive_file, std::ios::binary);
    unsigned char signature[4]{};
    archive.read(reinterpret_cast<char*>(signature), sizeof(signature));
    if (!archive || signature[0] != 'P' || signature[1] != 'K') {
        if (err) *err = "archive is not a zip archive";
        return false;
    }
    return true;
}

bool find_named_files(const std::filesystem::path& root, const wchar_t* filename,
                      std::vector<std::filesystem::path>& output, std::string* err) {
    output.clear();
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec) || ec) {
        if (err) *err = ec ? "cannot inspect extraction directory: " + ec.message()
                           : "extraction directory does not exist";
        return false;
    }
    std::filesystem::recursive_directory_iterator it(root, ec);
    const std::filesystem::recursive_directory_iterator end;
    while (it != end) {
        if (ec) {
            if (err) *err = "cannot enumerate extraction directory: " + ec.message();
            return false;
        }
        std::error_code entry_ec;
        if (it->is_regular_file(entry_ec)) {
            if (entry_ec) {
                if (err) *err = "cannot inspect extracted file: " + entry_ec.message();
                return false;
            }
            if (_wcsicmp(it->path().filename().c_str(), filename) == 0)
                output.push_back(it->path());
        } else if (entry_ec) {
            if (err) *err = "cannot inspect extracted entry: " + entry_ec.message();
            return false;
        }
        it.increment(ec);
    }
    if (ec) {
        if (err) *err = "cannot finish enumerating extraction directory: " + ec.message();
        return false;
    }
    return true;
}

bool copy_directory(const std::filesystem::path& source,
                    const std::filesystem::path& destination, std::string* err) {
    std::error_code ec;
    std::filesystem::copy(source, destination, std::filesystem::copy_options::recursive, ec);
    if (ec) {
        if (err) *err = "copy failed: " + ec.message();
        return false;
    }
    if (!std::filesystem::is_directory(destination, ec) || ec) {
        if (err) *err = ec ? "cannot verify copied directory: " + ec.message()
                           : "copy did not create a directory";
        return false;
    }
    return true;
}

bool parse_manifest_document(const std::filesystem::path& manifest_path,
                             BedrockManifest& manifest, std::string* err) {
    std::ifstream input(manifest_path);
    if (!input) {
        if (err) *err = "cannot read manifest.json";
        return false;
    }
    const std::string content((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());
    std::string parse_error;
    const aml::Json document = aml::Json::parse(content, &parse_error);
    if (!parse_error.empty() || !document.isObject()) {
        if (err) *err = parse_error.empty() ? "manifest.json is not an object" :
                                              "invalid manifest.json: " + parse_error;
        return false;
    }
    if (!document.isMember("header") || !document["header"].isObject()) {
        if (err) *err = "manifest.json has no valid header";
        return false;
    }

    const aml::Json& header = document["header"];
    manifest = BedrockManifest();
    manifest.uuid = header.get("uuid").asString();
    manifest.header_name = header.get("name").asString();
    manifest.header_description = header.get("description").asString();
    if (header.isMember("version")) {
        const aml::Json& version = header["version"];
        manifest.header_version = version.isArray() && version.size() > 0
            ? version.at(0).asString() : version.asString();
    }
    if (manifest.uuid.empty() || manifest.header_name.empty() || manifest.header_version.empty()) {
        if (err) *err = "manifest.json is missing header uuid, name, or version";
        return false;
    }

    if (document.isMember("format_version"))
        manifest.format_version = document["format_version"].asString();
    manifest.manifest_version = manifest.format_version;
    if (header.isMember("min_engine_version")) {
        const aml::Json& version = header["min_engine_version"];
        if (version.isArray()) {
            for (const auto& value : version)
                manifest.game_versions.push_back(value.asString());
        } else if (!version.asString().empty()) {
            manifest.game_versions.push_back(version.asString());
        }
    }

    if (!document.isMember("modules") || !document["modules"].isArray() ||
        document["modules"].size() == 0) {
        if (err) *err = "manifest.json has no modules";
        return false;
    }
    for (const auto& module : document["modules"]) {
        if (!module.isObject()) continue;
        const std::string type = module.get("type").asString();
        if (type != "resources" && type != "data") continue;
        if (manifest.type.empty()) {
            manifest.type = type;
            manifest.pack_type = type == "resources"
                ? BedrockPackType::Resource : BedrockPackType::Behavior;
            manifest.module_name = module.get("name").asString();
            if (manifest.module_name.empty())
                manifest.module_name = module.get("description").asString();
            manifest.module_description = module.get("description").asString();
            if (module.isMember("version")) {
                const aml::Json& version = module["version"];
                manifest.module_version = version.isArray() && version.size() > 0
                    ? version.at(0).asString() : version.asString();
            }
        }
    }
    if (manifest.type.empty()) {
        if (err) *err = "manifest.json has no supported data or resources module";
        return false;
    }

    if (document.isMember("dependencies") && document["dependencies"].isArray()) {
        for (const auto& dependency : document["dependencies"]) {
            const std::string uuid = dependency.isObject()
                ? dependency.get("uuid").asString() : dependency.asString();
            if (!uuid.empty()) manifest.dependencies.push_back(uuid);
        }
    }
    manifest.valid = true;
    return true;
}

}  // namespace

const std::wstring& uwp_family() {
    static const std::wstring s = kFamily;
    return s;
}

std::wstring detect(std::string* err) {
    if (err) err->clear();
    std::string package_error;
    if (!package_registered(&package_error)) {
        if (err) *err = package_error.empty()
            ? "Bedrock UWP package unavailable or not installed"
            : package_error;
        return L"";
    }

    // A data directory is content state only. The UWP package probe above is
    // the installation decision; a package may exist before first launch.
    std::wstring d = roaming_data_dir();
    if (d.empty()) d = uwp_data_dir();
    if (d.empty()) {
        if (err) *err = "Bedrock UWP package is installed, but its com.mojang data directory is unavailable";
        return L"";
    }
    return d;
}

bool installed() {
    return package_registered(nullptr);
}

bool package_registered(std::string* err) {
    return cached_package_registered(err);
}

bool launch(std::string* err) {
    // shell:AppsFolder activation works for UWP apps without elevated privileges.
    std::wstring path = std::wstring(L"shell:AppsFolder\\") + kAumid;
    HINSTANCE r = ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(r) <= 32) {
        if (err) {
            *err = "failed to launch Bedrock: " +
                   std::to_string(static_cast<long>(reinterpret_cast<INT_PTR>(r)));
        }
        return false;
    }
    return true;
}

std::wstring data_dir(std::string* err) {
    return detect(err);
}

bool validate_addon_file(const std::wstring& addon_file, std::string* err) {
    if (!net::file_exists(addon_file)) {
        if (err) *err = "addon file not found";
        return false;
    }
    size_t dot = addon_file.find_last_of(L'.');
    std::wstring ext = dot == std::wstring::npos ? L"" : addon_file.substr(dot);
    if (_wcsicmp(ext.c_str(), L".mcpack") != 0 && _wcsicmp(ext.c_str(), L".mcaddon") != 0) {
        if (err) *err = "addon must be a .mcpack or .mcaddon archive";
        return false;
    }
    HANDLE input = CreateFileW(addon_file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    LARGE_INTEGER size{};
    bool valid_size = input != INVALID_HANDLE_VALUE && GetFileSizeEx(input, &size) &&
                      size.QuadPart > 0 && size.QuadPart <= 256LL * 1024 * 1024;
    if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
    if (!valid_size) {
        if (err) *err = "addon is empty or exceeds the 256 MiB limit";
        return false;
    }
    std::ifstream archive(addon_file, std::ios::binary);
    unsigned char signature[4]{};
    archive.read(reinterpret_cast<char*>(signature), sizeof(signature));
    if (!archive || signature[0] != 'P' || signature[1] != 'K') {
        if (err) *err = "addon is not a zip archive";
        return false;
    }
    return true;
}

bool install_addon(const std::wstring& addon_file, std::string* err) {
    if (!validate_addon_file(addon_file, err)) return false;
    std::wstring dir = data_dir(err);
    if (dir.empty()) return false;
    HINSTANCE imported = ShellExecuteW(nullptr, L"open", addon_file.c_str(), nullptr, nullptr,
                                       SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(imported) > 32) return true;
    // Import folder: Bedrock watches com.mojang for .mcaddon/.mcpack and imports
    // them on next launch (manual "Import" in Settings->Storage works too).
    if (!net::mkdirs(dir)) {
        if (err) *err = "cannot create com.mojang dir";
        return false;
    }
    size_t sep = addon_file.find_last_of(L"\\/");
    std::wstring name =
        sep == std::wstring::npos ? addon_file : addon_file.substr(sep + 1);
    std::wstring dst = dir + L"\\" + name;
    size_t dot = name.find_last_of(L'.');
    std::wstring stem = dot == std::wstring::npos ? name : name.substr(0, dot);
    std::wstring suffix = dot == std::wstring::npos ? L"" : name.substr(dot);
    int duplicate = 2;
    while (net::file_exists(dst))
        dst = dir + L"\\" + stem + L"-" + std::to_wstring(duplicate++) + suffix;
    if (!CopyFileW(addon_file.c_str(), dst.c_str(), FALSE)) {
        if (err) *err = "copy addon failed (winerr " +
                        std::to_string(static_cast<long>(GetLastError())) + ")";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Profile Manager Implementation
// ---------------------------------------------------------------------------

BedrockProfileManager::BedrockProfileManager(const std::wstring& profiles_root)
    : root_(profiles_root) {
}

std::vector<BedrockProfile> BedrockProfileManager::scan(std::string* err) {
    std::vector<BedrockProfile> profiles;
    std::error_code ec;
    if (!std::filesystem::exists(root_, ec)) {
        std::filesystem::create_directories(root_, ec);
        if (ec) {
            if (err) *err = "Cannot create profiles directory: " + ec.message();
            return profiles;
        }
    }
    for (const auto& entry : std::filesystem::directory_iterator(root_, ec)) {
        if (!entry.is_directory(ec)) continue;
        BedrockProfile profile;
        profile.id = entry.path().filename().string();
        if (load(profile.id, profile, err)) {
            profiles.push_back(profile);
        }
    }
    return profiles;
}

bool BedrockProfileManager::create(const BedrockProfile& source, BedrockProfile& out, std::string* err) {
    out.id = generate_uuid();
    out.name = source.name.empty() ? ("Profile " + std::to_string(time(nullptr))) : source.name;
    out.minecraft_version = source.minecraft_version.empty() ? "Unavailable" : source.minecraft_version;
    out.created = std::to_string(time(nullptr));
    
    std::filesystem::path profile_path = std::filesystem::path(root_) / out.id;
    std::error_code ec;
    if (!std::filesystem::create_directories(profile_path, ec)) {
        if (err) *err = "Cannot create profile directory: " + ec.message();
        return false;
    }
    
    // Create required subdirectories
    std::filesystem::create_directories(profile_path / "behavior_packs", ec);
    std::filesystem::create_directories(profile_path / "resource_packs", ec);
    std::filesystem::create_directories(profile_path / "saves", ec);
    std::filesystem::create_directories(profile_path / "backups", ec);
    
    if (save(out, err)) return true;
    
    // Cleanup on failure
    std::filesystem::remove_all(profile_path, ec);
    return false;
}

bool BedrockProfileManager::duplicate(const BedrockProfile& source, const std::string& new_name,
                                    BedrockProfile& out, std::string* err) {
    BedrockProfile dup = source;
    dup.name = new_name.empty() ? (source.name + " Copy") : new_name;
    dup.id = generate_uuid();
    dup.last_played.clear();
    dup.last_played_ts = 0;
    
    // Copy pack files
    std::filesystem::path src_path = std::filesystem::path(root_) / source.id;
    std::filesystem::path dst_path = std::filesystem::path(root_) / dup.id;
    std::error_code ec;
    
    if (!std::filesystem::exists(src_path, ec)) {
        if (err) *err = "Source profile not found";
        return false;
    }
    
    std::filesystem::create_directories(dst_path, ec);
    if (ec) {
        if (err) *err = "Cannot create profile directory";
        return false;
    }
    
    // Copy directories
    if (std::filesystem::exists(src_path / "behavior_packs", ec)) {
        std::filesystem::copy(src_path / "behavior_packs", dst_path / "behavior_packs",
                              std::filesystem::copy_options::recursive, ec);
    }
    if (std::filesystem::exists(src_path / "resource_packs", ec)) {
        std::filesystem::copy(src_path / "resource_packs", dst_path / "resource_packs",
                              std::filesystem::copy_options::recursive, ec);
    }
    if (std::filesystem::exists(src_path / "saves", ec)) {
        std::filesystem::copy(src_path / "saves", dst_path / "saves",
                              std::filesystem::copy_options::recursive, ec);
    }
    
    if (save(dup, err)) {
        out = dup;
        return true;
    }
    
    // Cleanup on failure
    std::filesystem::remove_all(dst_path, ec);
    return false;
}

bool BedrockProfileManager::remove(const BedrockProfile& target, std::string* err) {
    std::filesystem::path profile_path = std::filesystem::path(root_) / target.id;
    std::error_code ec;
    std::filesystem::remove_all(profile_path, ec);
    if (ec && err) *err = ec.message();
    return !ec;
}

bool BedrockProfileManager::save(const BedrockProfile& profile, std::string* err) {
    std::string json = "{\n";
    json += "  \"id\": \"" + profile.id + "\",\n";
    json += "  \"name\": \"" + profile.name + "\",\n";
    json += "  \"minecraft_version\": \"" + profile.minecraft_version + "\",\n";
    json += "  \"created\": \"" + profile.created + "\",\n";
    json += "  \"last_played\": \"" + profile.last_played + "\",\n";
    json += "  \"favorite\": " + std::string(profile.favorite ? "true" : "false") + "\n";
    json += "}\n";
    
    std::filesystem::path path = std::filesystem::path(root_) / profile.id / "profile.json";
    std::ofstream file(path);
    if (!file) {
        if (err) *err = "Cannot write profile file";
        return false;
    }
    file << json;
    return true;
}

bool BedrockProfileManager::load(const std::string& id, BedrockProfile& out, std::string* err) {
    std::filesystem::path path = std::filesystem::path(root_) / id / "profile.json";
    std::ifstream file(path);
    if (!file) {
        if (err) *err = "Profile file not found";
        return false;
    }
    
    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
    
    // Very basic JSON parsing (production would use a proper JSON library)
    out.id = id;
    auto find_val = [&](const char* key) -> std::string {
        std::string needle = "\"" + std::string(key) + "\": \"";
        size_t pos = content.find(needle);
        if (pos == std::string::npos) return "";
        pos += needle.length();
        size_t end = content.find("\"", pos);
        return end == std::string::npos ? "" : content.substr(pos, end - pos);
    };
    
    out.name = find_val("name");
    out.minecraft_version = find_val("minecraft_version");
    out.created = find_val("created");
    out.last_played = find_val("last_played");
    
    size_t fav_pos = content.find("\"favorite\":");
    if (fav_pos != std::string::npos) {
        out.favorite = content.find("true", fav_pos) != std::string::npos;
    }
    
    return true;
}

// ---------------------------------------------------------------------------
// Manifest Parser Implementation
// ---------------------------------------------------------------------------

BedrockManifest parse_manifest(const std::wstring& archive_path, std::string* err) {
    BedrockManifest manifest;
    if (!is_extension(archive_path, L".mcpack") &&
        !is_extension(archive_path, L".mcaddon") &&
        !is_extension(archive_path, L".mcworld") &&
        !is_extension(archive_path, L".zip")) {
        if (err) *err = "unsupported Bedrock archive extension";
        return manifest;
    }
    if (!validate_archive_signature(archive_path, err)) return manifest;

    std::filesystem::path temp_dir;
    if (!make_temp_directory(L"amalgam_manifest_", temp_dir, err)) return manifest;
    auto cleanup = [&]() {
        std::string cleanup_error;
        if (!remove_tree(temp_dir, &cleanup_error)) {
            if (err) {
                if (!err->empty()) *err += "; ";
                *err += cleanup_error;
            }
            return false;
        }
        return true;
    };

    std::string extraction_error;
    if (!extract::zip(archive_path, temp_dir.wstring(), &extraction_error)) {
        if (err) *err = extraction_error.empty() ? "archive extraction failed" : extraction_error;
        cleanup();
        return manifest;
    }

    if (is_extension(archive_path, L".mcworld")) {
        std::vector<std::filesystem::path> levels;
        if (!find_named_files(temp_dir, L"level.dat", levels, err)) {
            cleanup();
            return manifest;
        }
        if (levels.empty()) {
            if (err) *err = ".mcworld archive has no level.dat";
            cleanup();
            return manifest;
        }
        if (levels.size() != 1) {
            if (err) *err = ".mcworld archive contains multiple level.dat files";
            cleanup();
            return manifest;
        }
        manifest.valid = true;
        manifest.pack_type = BedrockPackType::World;
        manifest.type = "world";
        manifest.header_name = net::to_utf8(std::filesystem::path(archive_path).stem().wstring());
        if (!cleanup()) manifest = BedrockManifest();
        return manifest;
    }

    std::vector<std::filesystem::path> manifests;
    if (!find_named_files(temp_dir, L"manifest.json", manifests, err)) {
        cleanup();
        return manifest;
    }
    if (!manifests.empty()) {
        if (!parse_manifest_document(manifests.front(), manifest, err)) {
            cleanup();
            return BedrockManifest();
        }
        if (!cleanup()) return BedrockManifest();
        return manifest;
    }

    if (!is_extension(archive_path, L".mcaddon")) {
        if (err) *err = "archive has no manifest.json";
        cleanup();
        return manifest;
    }

    std::vector<std::filesystem::path> nested_packs;
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(temp_dir, ec);
    const std::filesystem::recursive_directory_iterator end;
    while (it != end) {
        if (ec) {
            if (err) *err = "cannot enumerate addon archive: " + ec.message();
            cleanup();
            return manifest;
        }
        std::error_code entry_ec;
        if (it->is_regular_file(entry_ec)) {
            if (entry_ec) {
                if (err) *err = "cannot inspect addon entry: " + entry_ec.message();
                cleanup();
                return manifest;
            }
            if (is_extension(it->path().wstring(), L".mcpack"))
                nested_packs.push_back(it->path());
        } else if (entry_ec) {
            if (err) *err = "cannot inspect addon entry: " + entry_ec.message();
            cleanup();
            return manifest;
        }
        it.increment(ec);
    }
    if (ec) {
        if (err) *err = "cannot finish enumerating addon archive: " + ec.message();
        cleanup();
        return manifest;
    }
    if (nested_packs.empty()) {
        if (err) *err = ".mcaddon archive contains no pack manifest or .mcpack file";
        cleanup();
        return manifest;
    }
    for (size_t i = 0; i < nested_packs.size(); ++i) {
        std::string nested_error;
        if (!validate_addon_file(nested_packs[i].wstring(), &nested_error)) {
            if (err) *err = "invalid nested .mcpack: " + nested_error;
            cleanup();
            return BedrockManifest();
        }
        const std::filesystem::path nested_dir = temp_dir / (L".nested_" + std::to_wstring(i));
        if (!ensure_directory(nested_dir, err, "nested extraction directory")) {
            cleanup();
            return manifest;
        }
        if (!extract::zip(nested_packs[i].wstring(), nested_dir.wstring(), &nested_error)) {
            if (err) *err = nested_error.empty() ? "nested pack extraction failed" : nested_error;
            cleanup();
            return manifest;
        }
        std::vector<std::filesystem::path> nested_manifests;
        if (!find_named_files(nested_dir, L"manifest.json", nested_manifests, err) ||
            nested_manifests.empty()) {
            if (err && err->empty()) *err = "nested .mcpack has no manifest.json";
            cleanup();
            return manifest;
        }
        if (!parse_manifest_document(nested_manifests.front(), manifest, err)) {
            cleanup();
            return BedrockManifest();
        }
        if (!cleanup()) return BedrockManifest();
        return manifest;
    }
    if (err) *err = ".mcaddon archive contains no valid pack manifest";
    cleanup();
    return manifest;
}

// ---------------------------------------------------------------------------
// Import Manager Implementation
// ---------------------------------------------------------------------------

BedrockImportResult import_addon(const std::wstring& archive_path,
                                 const BedrockProfile& target,
                                 std::string* err) {
    BedrockImportResult result;
    if (err) err->clear();
    auto fail = [&](const std::string& message) {
        result.error = message;
        if (err) *err = message;
        return result;
    };

    if (!is_extension(archive_path, L".mcpack") &&
        !is_extension(archive_path, L".mcaddon")) {
        return fail("addon must be a .mcpack or .mcaddon archive");
    }
    std::string validation_error;
    if (!validate_addon_file(archive_path, &validation_error))
        return fail(validation_error);

    std::filesystem::path profile_root;
    if (!profile_root_path(target, profile_root, err)) {
        result.error = err ? *err : "invalid Bedrock profile target";
        return result;
    }

    std::string manifest_error;
    const BedrockManifest summary = parse_manifest(archive_path, &manifest_error);
    if (!summary.valid)
        return fail(manifest_error.empty() ? "addon manifest validation failed" : manifest_error);
    if (summary.pack_type != BedrockPackType::Behavior &&
        summary.pack_type != BedrockPackType::Resource) {
        return fail("addon manifest is not a behavior or resource pack");
    }

    std::filesystem::path staging;
    if (!make_temp_directory(L"amalgam_addon_", staging, err)) {
        result.error = err ? *err : "cannot create addon staging directory";
        return result;
    }
    std::vector<std::filesystem::path> installed_dirs;
    auto rollback = [&]() {
        bool ok = true;
        for (const auto& path : installed_dirs) {
            std::error_code cleanup_ec;
            std::filesystem::remove_all(path, cleanup_ec);
            if (cleanup_ec) ok = false;
        }
        return ok;
    };
    auto fail_after_staging = [&](const std::string& message) {
        std::string full = message;
        if (!rollback()) {
            if (!full.empty()) full += "; ";
            full += "failed to roll back partial addon import";
        }
        std::string cleanup_error;
        if (!remove_tree(staging, &cleanup_error)) {
            if (!full.empty()) full += "; ";
            full += cleanup_error;
        }
        result.success = false;
        result.error = full;
        if (err) *err = full;
        return result;
    };

    std::string extraction_error;
    if (!extract::zip(archive_path, staging.wstring(), &extraction_error))
        return fail_after_staging(extraction_error.empty() ? "addon extraction failed" : extraction_error);

    std::set<std::wstring> root_names;
    std::vector<std::filesystem::path> pack_roots;
    auto collect_roots = [&](const std::filesystem::path& root) {
        std::vector<std::filesystem::path> manifest_files;
        std::string find_error;
        if (!find_named_files(root, L"manifest.json", manifest_files, &find_error)) {
            if (!find_error.empty()) extraction_error = find_error;
            return false;
        }
        for (const auto& manifest_file : manifest_files) {
            const std::filesystem::path pack_root = manifest_file.parent_path();
            if (root_names.insert(pack_root.wstring()).second)
                pack_roots.push_back(pack_root);
        }
        return true;
    };

    if (is_extension(archive_path, L".mcpack")) {
        if (!collect_roots(staging))
            return fail_after_staging(extraction_error);
        if (pack_roots.size() != 1)
            return fail_after_staging(pack_roots.empty()
                ? "mcpack archive has no manifest.json"
                : "mcpack archive contains multiple pack manifests");
    } else {
        if (!collect_roots(staging))
            return fail_after_staging(extraction_error);

        std::vector<std::filesystem::path> nested_packs;
        std::error_code walk_ec;
        std::filesystem::recursive_directory_iterator it(staging, walk_ec);
        const std::filesystem::recursive_directory_iterator end;
        while (it != end) {
            if (walk_ec)
                return fail_after_staging("cannot enumerate .mcaddon contents: " + walk_ec.message());
            std::error_code entry_ec;
            if (it->is_regular_file(entry_ec)) {
                if (entry_ec)
                    return fail_after_staging("cannot inspect .mcaddon entry: " + entry_ec.message());
                if (is_extension(it->path().wstring(), L".mcpack"))
                    nested_packs.push_back(it->path());
            } else if (entry_ec) {
                return fail_after_staging("cannot inspect .mcaddon entry: " + entry_ec.message());
            }
            it.increment(walk_ec);
        }
        if (walk_ec)
            return fail_after_staging("cannot finish enumerating .mcaddon contents: " + walk_ec.message());
        for (size_t i = 0; i < nested_packs.size(); ++i) {
            std::string nested_error;
            if (!validate_addon_file(nested_packs[i].wstring(), &nested_error))
                return fail_after_staging("invalid nested .mcpack: " + nested_error);
            const std::filesystem::path nested_dir = staging / (L".nested_" + std::to_wstring(i));
            if (!ensure_directory(nested_dir, &nested_error, "nested extraction directory"))
                return fail_after_staging(nested_error);
            if (!extract::zip(nested_packs[i].wstring(), nested_dir.wstring(), &nested_error))
                return fail_after_staging(nested_error.empty() ? "nested pack extraction failed" : nested_error);
            if (!collect_roots(nested_dir))
                return fail_after_staging(extraction_error.empty()
                    ? "nested .mcpack has no manifest.json" : extraction_error);
        }
        if (pack_roots.empty())
            return fail_after_staging("mcaddon archive contains no pack manifest or .mcpack file");
    }

    if (!ensure_directory(profile_root, err, "Bedrock profile directory"))
        return fail_after_staging(err ? *err : "cannot create Bedrock profile directory");

    for (const auto& pack_root : pack_roots) {
        BedrockManifest manifest;
        std::string parse_error;
        if (!parse_manifest_document(pack_root / L"manifest.json", manifest, &parse_error))
            return fail_after_staging(parse_error.empty() ? "pack manifest validation failed" : parse_error);
        if (manifest.pack_type != BedrockPackType::Behavior &&
            manifest.pack_type != BedrockPackType::Resource)
            return fail_after_staging("pack manifest is not a behavior or resource pack");

        const std::filesystem::path pack_parent = profile_root /
            (manifest.pack_type == BedrockPackType::Resource ? L"resource_packs" : L"behavior_packs");
        std::string directory_error;
        if (!ensure_directory(pack_parent, &directory_error, "Bedrock pack directory"))
            return fail_after_staging(directory_error);

        std::filesystem::path destination;
        if (!unique_child(pack_parent,
                          manifest.uuid.empty() ? net::to_utf8(pack_root.filename().wstring()) : manifest.uuid,
                          destination, &directory_error))
            return fail_after_staging(directory_error);
        if (!copy_directory(pack_root, destination, &directory_error))
            return fail_after_staging(directory_error);
        installed_dirs.push_back(destination);
        std::error_code verify_ec;
        if (!std::filesystem::is_regular_file(destination / L"manifest.json", verify_ec) || verify_ec)
            return fail_after_staging(verify_ec ? "cannot verify extracted manifest: " + verify_ec.message()
                                                : "extracted pack has no manifest.json");

        if (!result.success) {
            result.pack.profile_id = target.id;
            result.pack.profile_name = target.name;
            result.pack.uuid = manifest.uuid;
            result.pack.name = manifest.header_name;
            result.pack.version = manifest.header_version;
            result.pack.type = manifest.pack_type;
            result.pack.filename = net::to_utf8(destination.filename().wstring());
            result.pack.author = manifest.module_name;
            result.pack.description = manifest.header_description;
            result.pack.source = "imported";
            result.pack.size_bytes = net::file_size(archive_path);
        }
        result.success = true;
    }

    std::string cleanup_error;
    if (!remove_tree(staging, &cleanup_error)) {
        rollback();
        result.success = false;
        result.error = cleanup_error;
        if (err) *err = cleanup_error;
        return result;
    }
    return result;
}

std::vector<BedrockImportResult> import_addons(const std::vector<std::wstring>& paths,
                                              const BedrockProfile& target,
                                              std::string* err) {
    std::vector<BedrockImportResult> results;
    for (const auto& path : paths) {
        results.push_back(import_addon(path, target, err));
    }
    return results;
}

namespace {

BedrockImportResult import_world_archive_impl(const std::wstring& world_path,
                                              const BedrockProfile& target,
                                              const std::string& target_name,
                                              std::string* err) {
    BedrockImportResult result;
    if (err) err->clear();
    auto fail = [&](const std::string& message) {
        result.error = message;
        if (err) *err = message;
        return result;
    };

    if (!is_extension(world_path, L".mcworld"))
        return fail("world must be a .mcworld archive");
    if (!validate_archive_signature(world_path, err))
        return fail(err ? *err : "invalid .mcworld archive");

    std::string manifest_error;
    const BedrockManifest manifest = parse_manifest(world_path, &manifest_error);
    if (!manifest.valid || manifest.pack_type != BedrockPackType::World)
        return fail(manifest_error.empty() ? "world archive validation failed" : manifest_error);

    std::filesystem::path profile_root;
    if (!profile_root_path(target, profile_root, err))
        return fail(err ? *err : "invalid Bedrock profile target");
    std::filesystem::path staging;
    if (!make_temp_directory(L"amalgam_world_", staging, err))
        return fail(err ? *err : "cannot create world staging directory");
    std::vector<std::filesystem::path> installed;
    auto fail_after_staging = [&](const std::string& message) {
        std::string full = message;
        for (const auto& path : installed) {
            std::error_code cleanup_ec;
            std::filesystem::remove_all(path, cleanup_ec);
            if (cleanup_ec) {
                if (!full.empty()) full += "; ";
                full += "failed to roll back partial world import";
            }
        }
        std::string cleanup_error;
        if (!remove_tree(staging, &cleanup_error)) {
            if (!full.empty()) full += "; ";
            full += cleanup_error;
        }
        result.error = full;
        if (err) *err = full;
        return result;
    };

    std::string extraction_error;
    if (!extract::zip(world_path, staging.wstring(), &extraction_error))
        return fail_after_staging(extraction_error.empty() ? "world extraction failed" : extraction_error);
    std::vector<std::filesystem::path> levels;
    if (!find_named_files(staging, L"level.dat", levels, &extraction_error))
        return fail_after_staging(extraction_error);
    if (levels.empty())
        return fail_after_staging(".mcworld archive has no level.dat");
    if (levels.size() != 1)
        return fail_after_staging(".mcworld archive contains multiple level.dat files");

    const std::filesystem::path world_parent = profile_root / L"saves";
    if (!ensure_directory(world_parent, &extraction_error, "Bedrock world directory"))
        return fail_after_staging(extraction_error);
    std::filesystem::path destination;
    if (!unique_child(world_parent, target_name, destination, &extraction_error))
        return fail_after_staging(extraction_error);
    if (!copy_directory(levels.front().parent_path(), destination, &extraction_error))
        return fail_after_staging(extraction_error);
    installed.push_back(destination);
    std::error_code verify_ec;
    if (!std::filesystem::is_regular_file(destination / L"level.dat", verify_ec) || verify_ec)
        return fail_after_staging(verify_ec ? "cannot verify extracted world: " + verify_ec.message()
                                            : "extracted world has no level.dat at its root");

    result.success = true;
    result.pack.name = net::to_utf8(destination.filename().wstring());
    result.pack.filename = result.pack.name;
    result.pack.type = BedrockPackType::World;
    result.pack.source = "imported";
    result.pack.size_bytes = net::file_size(world_path);
    std::string cleanup_error;
    if (!remove_tree(staging, &cleanup_error)) {
        for (const auto& path : installed) {
            std::error_code cleanup_ec;
            std::filesystem::remove_all(path, cleanup_ec);
            if (cleanup_ec && err) {
                if (!err->empty()) *err += "; ";
                *err += "failed to roll back partial world import";
            }
        }
        result.success = false;
        result.error = cleanup_error;
        if (err) *err = cleanup_error;
    }
    return result;
}

}  // namespace

BedrockImportResult import_world(const std::wstring& world_path,
                                 const BedrockProfile& target,
                                 std::string* err) {
    std::string name = net::to_utf8(std::filesystem::path(world_path).stem().wstring());
    return import_world_archive_impl(world_path, target, name, err);
}

// ---------------------------------------------------------------------------
// World Manager Implementation
// ---------------------------------------------------------------------------

std::vector<BedrockWorldEntry> list_worlds(const BedrockProfile& profile,
                                           std::string* err) {
    std::vector<BedrockWorldEntry> worlds;
    if (err) err->clear();
    std::filesystem::path profile_root;
    if (!profile_root_path(profile, profile_root, err)) return worlds;
    const std::filesystem::path world_dir = profile_root / L"saves";
    std::error_code ec;
    if (!std::filesystem::exists(world_dir, ec)) {
        if (ec && err) *err = "cannot inspect Bedrock world directory: " + ec.message();
        return worlds;
    }
    if (!std::filesystem::is_directory(world_dir, ec) || ec) {
        if (err) *err = ec ? "cannot inspect Bedrock world directory: " + ec.message()
                           : "Bedrock world path is not a directory: " + world_dir.string();
        return worlds;
    }

    std::filesystem::directory_iterator it(world_dir, ec);
    const std::filesystem::directory_iterator end;
    while (it != end) {
        if (ec) {
            if (err) *err = "cannot enumerate Bedrock worlds: " + ec.message();
            return worlds;
        }
        const auto entry = *it;
        std::error_code entry_ec;
        if (!entry.is_directory(entry_ec)) {
            if (entry_ec) {
                if (err) *err = "cannot inspect Bedrock world: " + entry_ec.message();
                return worlds;
            }
            it.increment(ec);
            continue;
        }
        BedrockWorldEntry world;
        world.name = entry.path().filename().string();
        world.folder = world.name;
        world.profile_id = profile.id;
        world.profile_name = profile.name;
        worlds.push_back(world);
        it.increment(ec);
    }
    if (ec && err) *err = "cannot finish enumerating Bedrock worlds: " + ec.message();
    return worlds;
}

bool backup_world(const BedrockProfile& profile, const std::string& world_folder,
                  std::wstring& out_path, std::string* err) {
    if (err) err->clear();
    out_path.clear();
    if (world_folder.empty() || !safe_path_component(net::to_wide(world_folder))) {
        if (err) *err = "invalid Bedrock world folder";
        return false;
    }

    std::filesystem::path profile_root;
    if (!profile_root_path(profile, profile_root, err)) return false;
    const std::filesystem::path world_dir = profile_root / L"saves" / net::to_wide(world_folder);
    const std::filesystem::path backup_dir = profile_root / L"backups";
    std::error_code ec;
    if (!std::filesystem::is_directory(world_dir, ec) || ec) {
        if (err) *err = ec ? "cannot inspect Bedrock world: " + ec.message()
                           : "Bedrock world directory not found: " + world_dir.string();
        return false;
    }
    if (!ensure_directory(backup_dir, err, "Bedrock backup directory")) {
        out_path.clear();
        return false;
    }
    
    // Generate backup filename with timestamp
    time_t now = time(nullptr);
    struct tm* tm_info = localtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", tm_info);
    
    const std::string backup_name = sanitized_component(profile.name, "profile") + "_" +
                                    std::string(ts) + "_" +
                                    sanitized_component(world_folder, "world");
    std::filesystem::path destination;
    if (!unique_child(backup_dir, backup_name, destination, err)) return false;

    std::filesystem::copy(world_dir, destination,
                          std::filesystem::copy_options::recursive, ec);
    if (ec) {
        if (err) *err = "World backup failed: " + ec.message();
        return false;
    }
    if (!std::filesystem::is_directory(destination, ec) || ec) {
        if (err) *err = ec ? "cannot verify world backup: " + ec.message()
                           : "world backup directory was not created";
        return false;
    }
    out_path = destination.wstring();
    return true;
}

// ---------------------------------------------------------------------------
// Storage / Activity
// ---------------------------------------------------------------------------

BedrockStorageInfo compute_storage(const BedrockProfile& profile) {
    BedrockStorageInfo info;
    std::filesystem::path profile_root;
    if (!profile_root_path(profile, profile_root, nullptr)) return info;

    auto dir_size = [&](const wchar_t* subdir) -> uint64_t {
        const std::filesystem::path p = profile_root / subdir;
        std::error_code exists_ec;
        if (!std::filesystem::is_directory(p, exists_ec) || exists_ec) return 0;
        uint64_t total = 0;
        std::error_code ec;
        std::filesystem::recursive_directory_iterator it(p, ec);
        const std::filesystem::recursive_directory_iterator end;
        while (it != end) {
            if (ec) return total;
            std::error_code entry_ec;
            if (it->is_regular_file(entry_ec) && !entry_ec)
                total += it->file_size(entry_ec);
            if (entry_ec) return total;
            it.increment(ec);
        }
        return total;
    };

    info.behavior_bytes = dir_size(L"behavior_packs");
    info.resource_bytes = dir_size(L"resource_packs");
    info.world_bytes = dir_size(L"saves");
    info.other_bytes = 0;
    info.total_bytes = info.behavior_bytes + info.resource_bytes + info.world_bytes + info.other_bytes;
    
    return info;
}

// ---------------------------------------------------------------------------
// Health Scanner
// ---------------------------------------------------------------------------

std::vector<BedrockHealthIssue> scan_health(const BedrockProfile& profile,
                                             std::string* err) {
    std::vector<BedrockHealthIssue> issues;
    if (err) err->clear();
    std::filesystem::path profile_root;
    if (!profile_root_path(profile, profile_root, err)) {
        BedrockHealthIssue issue;
        issue.severity = "error";
        issue.message = err && !err->empty() ? *err : "Bedrock profile root unavailable";
        issue.auto_fixable = false;
        issues.push_back(issue);
        return issues;
    }

    // Check required directories
    std::error_code ec;
    if (!std::filesystem::is_directory(profile_root / L"behavior_packs", ec) || ec) {
        BedrockHealthIssue issue;
        issue.severity = "warning";
        issue.message = "Behavior packs directory missing";
        issues.push_back(issue);
    }
    ec.clear();
    if (!std::filesystem::is_directory(profile_root / L"resource_packs", ec) || ec) {
        BedrockHealthIssue issue;
        issue.severity = "warning";
        issue.message = "Resource packs directory missing";
        issues.push_back(issue);
    }
    ec.clear();
    if (!std::filesystem::is_directory(profile_root / L"saves", ec) || ec) {
        BedrockHealthIssue issue;
        issue.severity = "warning";
        issue.message = "Worlds directory missing";
        issues.push_back(issue);
    }
    
    return issues;
}

bool auto_repair(const BedrockProfile& profile, std::string* err) {
    if (err) err->clear();
    std::filesystem::path root;
    if (!profile_root_path(profile, root, err)) return false;
    const wchar_t* directories[] = {L"behavior_packs", L"resource_packs", L"saves", L"backups"};
    for (const wchar_t* directory : directories) {
        std::string directory_error;
        if (!ensure_directory(root / directory, &directory_error, "Bedrock profile directory")) {
            if (err) *err = directory_error;
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Sync Manager
// ---------------------------------------------------------------------------

bool sync_to_bedrock(const BedrockProfile& profile, std::string* err) {
    if (err) err->clear();
    std::filesystem::path target_root;
    if (!data_root_path(target_root, err)) return false;
    std::filesystem::path profile_root;
    if (!profile_root_path(profile, profile_root, err)) return false;

    for (const auto& pack : profile.packs) {
        std::filesystem::path live_parent;
        std::filesystem::path profile_parent;
        if (pack.type == BedrockPackType::Behavior) {
            live_parent = target_root / L"behavior_packs";
            profile_parent = profile_root / L"behavior_packs";
        } else if (pack.type == BedrockPackType::Resource) {
            live_parent = target_root / L"resource_packs";
            profile_parent = profile_root / L"resource_packs";
        } else {
            if (err) *err = "unsupported Bedrock pack type for sync: " + pack.name;
            return false;
        }

        const std::string source_name = !pack.filename.empty() ? pack.filename :
            (!pack.uuid.empty() ? pack.uuid : pack.name);
        const std::wstring source_component = net::to_wide(source_name);
        if (!safe_path_component(source_component)) {
            if (err) *err = "invalid Bedrock pack path: " + source_name;
            return false;
        }
        std::filesystem::path source;
        if (!absolute_path(profile_parent / source_component, source, err)) return false;
        std::error_code source_ec;
        if (!std::filesystem::is_directory(source, source_ec) || source_ec) {
            if (err) *err = source_ec ? "cannot inspect profile pack: " + source_ec.message()
                                      : "profile pack directory not found: " + source.string();
            return false;
        }
        std::string directory_error;
        if (!ensure_directory(live_parent, &directory_error, "Bedrock live pack directory")) {
            if (err) *err = directory_error;
            return false;
        }
        std::filesystem::path destination;
        if (!absolute_path(live_parent / source_component, destination, err)) return false;
        std::error_code copy_ec;
        std::filesystem::copy(source, destination,
                              std::filesystem::copy_options::recursive |
                                  std::filesystem::copy_options::overwrite_existing,
                              copy_ec);
        if (copy_ec) {
            if (err) *err = "failed to sync pack " + pack.name + ": " + copy_ec.message();
            return false;
        }
        if (!std::filesystem::is_directory(destination, copy_ec) || copy_ec) {
            if (err) *err = copy_ec ? "cannot verify synced pack: " + copy_ec.message()
                                    : "sync did not create pack directory: " + destination.string();
            return false;
        }
    }
    return true;
}

bool sync_from_bedrock(BedrockProfile& profile, std::string* err) {
    if (err) err->clear();
    std::filesystem::path source_root;
    if (!data_root_path(source_root, err)) return false;
    std::filesystem::path profile_root;
    if (!profile_root_path(profile, profile_root, err)) return false;
    std::string directory_error;
    if (!ensure_directory(profile_root, &directory_error, "Bedrock profile directory")) {
        if (err) *err = directory_error;
        return false;
    }

    const wchar_t* directories[] = {L"behavior_packs", L"resource_packs"};
    const BedrockPackType types[] = {BedrockPackType::Behavior, BedrockPackType::Resource};
    for (size_t type_index = 0; type_index < 2; ++type_index) {
        const std::filesystem::path source_dir = source_root / directories[type_index];
        std::error_code exists_ec;
        if (!std::filesystem::exists(source_dir, exists_ec)) {
            if (exists_ec) {
                if (err) *err = "cannot inspect Bedrock pack directory: " + exists_ec.message();
                return false;
            }
            continue;
        }
        if (!std::filesystem::is_directory(source_dir, exists_ec) || exists_ec) {
            if (err) *err = exists_ec ? "cannot inspect Bedrock pack directory: " + exists_ec.message()
                                      : "Bedrock pack path is not a directory: " + source_dir.string();
            return false;
        }

        std::error_code iterator_ec;
        std::filesystem::directory_iterator it(source_dir, iterator_ec);
        const std::filesystem::directory_iterator end;
        while (it != end) {
            if (iterator_ec) {
                if (err) *err = "cannot enumerate Bedrock pack directory: " + iterator_ec.message();
                return false;
            }
            const auto entry = *it;
            std::error_code entry_ec;
            if (!entry.is_directory(entry_ec)) {
                if (entry_ec) {
                    if (err) *err = "cannot inspect Bedrock pack entry: " + entry_ec.message();
                    return false;
                }
                it.increment(iterator_ec);
                continue;
            }

            const std::filesystem::path manifest_path = entry.path() / L"manifest.json";
            if (!std::filesystem::is_regular_file(manifest_path, entry_ec)) {
                if (entry_ec) {
                    if (err) *err = "cannot inspect pack manifest: " + entry_ec.message();
                    return false;
                }
                it.increment(iterator_ec);
                continue;
            }
            BedrockManifest manifest;
            std::string manifest_error;
            if (!parse_manifest_document(manifest_path, manifest, &manifest_error)) {
                if (err) *err = "invalid Bedrock pack manifest: " + manifest_error;
                return false;
            }
            if (manifest.pack_type != types[type_index]) {
                if (err) *err = "pack manifest type does not match its directory";
                return false;
            }
            const std::wstring source_name = entry.path().filename().wstring();
            if (!safe_path_component(source_name)) {
                if (err) *err = "invalid Bedrock pack directory name";
                return false;
            }
            const std::filesystem::path destination_parent = profile_root / directories[type_index];
            if (!ensure_directory(destination_parent, &directory_error, "Bedrock profile pack directory")) {
                if (err) *err = directory_error;
                return false;
            }
            std::filesystem::path destination = destination_parent / source_name;
            std::error_code copy_ec;
            std::filesystem::copy(entry.path(), destination,
                                  std::filesystem::copy_options::recursive |
                                      std::filesystem::copy_options::overwrite_existing,
                                  copy_ec);
            if (copy_ec) {
                if (err) *err = "failed to copy Bedrock pack: " + copy_ec.message();
                return false;
            }
            if (!std::filesystem::is_directory(destination, copy_ec) || copy_ec) {
                if (err) *err = copy_ec ? "cannot verify copied Bedrock pack: " + copy_ec.message()
                                        : "copied Bedrock pack directory is missing";
                return false;
            }

            bool found = false;
            for (const auto& existing : profile.packs) {
                if (existing.filename == net::to_utf8(source_name) && existing.type == types[type_index]) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                BedrockPackEntry pack;
                pack.uuid = manifest.uuid;
                pack.name = manifest.header_name;
                pack.version = manifest.header_version;
                pack.type = types[type_index];
                pack.filename = net::to_utf8(source_name);
                pack.source = "com.mojang";
                pack.enabled = true;
                pack.description = manifest.header_description;
                profile.packs.push_back(pack);
            }
            it.increment(iterator_ec);
        }
        if (iterator_ec) {
            if (err) *err = "cannot finish enumerating Bedrock pack directory: " + iterator_ec.message();
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Backup Manager
// ---------------------------------------------------------------------------

std::vector<BedrockBackupEntry> list_backups(const BedrockProfile& profile,
                                              std::string* err) {
    std::vector<BedrockBackupEntry> backups;
    if (err) err->clear();
    std::filesystem::path profile_root;
    if (!profile_root_path(profile, profile_root, err)) return backups;
    const std::filesystem::path backup_dir = profile_root / L"backups";
    std::error_code ec;
    if (!std::filesystem::exists(backup_dir, ec)) {
        if (ec && err) *err = "cannot inspect Bedrock backup directory: " + ec.message();
        return backups;
    }
    if (!std::filesystem::is_directory(backup_dir, ec) || ec) {
        if (err) *err = ec ? "cannot inspect Bedrock backup directory: " + ec.message()
                           : "Bedrock backup path is not a directory: " + backup_dir.string();
        return backups;
    }

    std::filesystem::directory_iterator it(backup_dir, ec);
    const std::filesystem::directory_iterator end;
    while (it != end) {
        if (ec) {
            if (err) *err = "cannot enumerate Bedrock backups: " + ec.message();
            return backups;
        }
        const auto entry = *it;
        std::error_code entry_ec;
        if (!entry.is_directory(entry_ec)) {
            if (entry_ec) {
                if (err) *err = "cannot inspect Bedrock backup: " + entry_ec.message();
                return backups;
            }
            it.increment(ec);
            continue;
        }
        BedrockBackupEntry backup;
        backup.id = entry.path().filename().string();
        backup.profile_id = profile.id;
        backup.path = net::to_utf8(entry.path().wstring());
        backup.label = backup.id;
        backup.timestamp = backup.id;
        backup.size_bytes = 0;
        
        // Calculate size
        std::filesystem::recursive_directory_iterator files(entry.path(), ec);
        const std::filesystem::recursive_directory_iterator files_end;
        while (files != files_end) {
            if (ec) {
                if (err) *err = "cannot enumerate Bedrock backup contents: " + ec.message();
                return backups;
            }
            std::error_code file_ec;
            if (files->is_regular_file(file_ec) && !file_ec)
                backup.size_bytes += files->file_size(file_ec);
            if (file_ec) {
                if (err) *err = "cannot inspect Bedrock backup file: " + file_ec.message();
                return backups;
            }
            files.increment(ec);
        }
        backups.push_back(backup);
        it.increment(ec);
    }
    if (ec && err) *err = "cannot finish enumerating Bedrock backups: " + ec.message();
    return backups;
}

bool create_backup(const BedrockProfile& profile, const std::string& label,
                   std::wstring& out_path, std::string* err) {
    if (err) err->clear();
    out_path.clear();
    std::filesystem::path profile_root;
    if (!profile_root_path(profile, profile_root, err)) return false;
    const std::filesystem::path backup_dir = profile_root / L"backups";
    const std::filesystem::path src_dir = profile_root / L"saves";
    if (!ensure_directory(backup_dir, err, "Bedrock backup directory")) return false;

    std::error_code ec;
    if (!std::filesystem::is_directory(src_dir, ec) || ec) {
        if (err) *err = ec ? "cannot inspect Bedrock worlds: " + ec.message()
                           : "Bedrock worlds directory not found: " + src_dir.string();
        return false;
    }

    time_t now = time(nullptr);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", localtime(&now));

    const std::string preferred = label.empty() ? ("backup_" + std::string(ts)) : label;
    std::filesystem::path destination;
    if (!unique_child(backup_dir, preferred, destination, err)) return false;
    std::filesystem::copy(src_dir, destination,
                          std::filesystem::copy_options::recursive, ec);
    if (ec) {
        if (err) *err = "Backup failed: " + ec.message();
        return false;
    }
    if (!std::filesystem::is_directory(destination, ec) || ec) {
        if (err) *err = ec ? "cannot verify Bedrock backup: " + ec.message()
                           : "Bedrock backup directory was not created";
        return false;
    }
    out_path = destination.wstring();
    return true;
}

bool restore_backup(const BedrockProfile& profile, const BedrockBackupEntry& backup,
                    std::string* err) {
    if (err) err->clear();
    std::filesystem::path src;
    if (!profile_backup_path(profile, backup, src, err)) return false;
    std::filesystem::path profile_root;
    if (!profile_root_path(profile, profile_root, err)) return false;
    const std::filesystem::path dst = profile_root / L"saves";
    std::error_code ec;
    if (!std::filesystem::is_directory(src, ec) || ec) {
        if (err) *err = ec ? "cannot inspect Bedrock backup: " + ec.message()
                           : "Bedrock backup not found: " + src.string();
        return false;
    }
    std::filesystem::remove_all(dst, ec);
    if (ec) {
        if (err) *err = "Restore failed while removing current worlds: " + ec.message();
        return false;
    }
    if (!ensure_directory(profile_root, err, "Bedrock profile directory")) return false;
    std::filesystem::copy(src, dst,
                          std::filesystem::copy_options::recursive, ec);
    if (ec) {
        if (err) *err = "Restore failed: " + ec.message();
        return false;
    }
    if (!std::filesystem::is_directory(dst, ec) || ec) {
        if (err) *err = ec ? "cannot verify restored worlds: " + ec.message()
                           : "restored worlds directory was not created";
        return false;
    }
    return true;
}

bool delete_backup(const BedrockProfile& profile, const BedrockBackupEntry& backup,
                   std::string* err) {
    if (err) err->clear();
    std::filesystem::path path;
    if (!profile_backup_path(profile, backup, path, err)) return false;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        if (err) *err = ec ? "cannot inspect Bedrock backup: " + ec.message()
                           : "Bedrock backup not found: " + path.string();
        return false;
    }
    const uintmax_t removed = std::filesystem::remove_all(path, ec);
    if (ec) {
        if (err) *err = "cannot delete Bedrock backup: " + ec.message();
        return false;
    }
    if (removed == 0) {
        if (err) *err = "Bedrock backup was not deleted";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Dependency Resolver
// ---------------------------------------------------------------------------

std::vector<BedrockDependencyInfo> resolve_dependencies(
    const std::vector<BedrockPackEntry>& packs,
    const std::string& target_uuid) {
    std::vector<BedrockDependencyInfo> deps;
    if (target_uuid.empty()) return deps;

    const BedrockPackEntry* target = nullptr;
    for (const auto& pack : packs) {
        if (pack.uuid == target_uuid) {
            target = &pack;
            break;
        }
    }
    if (!target || target->profile_id.empty() || target->filename.empty()) return deps;

    BedrockProfile profile;
    profile.id = target->profile_id;
    std::filesystem::path profile_root;
    if (!profile_root_path(profile, profile_root, nullptr)) return deps;
    const wchar_t* pack_directory = target->type == BedrockPackType::Resource
        ? L"resource_packs" : target->type == BedrockPackType::Behavior
        ? L"behavior_packs" : nullptr;
    if (!pack_directory || !safe_path_component(net::to_wide(target->filename))) return deps;

    BedrockManifest manifest;
    std::string manifest_error;
    const std::filesystem::path active_manifest = profile_root / pack_directory /
        net::to_wide(target->filename) / L"manifest.json";
    const std::filesystem::path disabled_manifest = profile_root / L"disabled_packs" /
        pack_directory / net::to_wide(target->filename) / L"manifest.json";
    const std::filesystem::path first_manifest = target->enabled
        ? active_manifest : disabled_manifest;
    const std::filesystem::path second_manifest = target->enabled
        ? disabled_manifest : active_manifest;
    if (!parse_manifest_document(first_manifest, manifest, &manifest_error) &&
        !parse_manifest_document(second_manifest, manifest, &manifest_error)) {
        return deps;
    }
    std::set<std::string> seen;
    for (const auto& dependency_uuid : manifest.dependencies) {
        if (dependency_uuid.empty() || !seen.insert(dependency_uuid).second) continue;
        BedrockDependencyInfo dependency;
        dependency.uuid = dependency_uuid;
        dependency.required = true;
        dependency.name = dependency_uuid;
        for (const auto& pack : packs) {
            if (pack.uuid == dependency_uuid) {
                dependency.installed = true;
                if (!pack.name.empty()) dependency.name = pack.name;
                break;
            }
        }
        deps.push_back(std::move(dependency));
    }
    return deps;
}

// ---------------------------------------------------------------------------
// Import Manager for World Files
// ---------------------------------------------------------------------------

bool import_world_file(const std::wstring& source, const BedrockProfile& profile,
                       const std::string& target_name, std::string* err) {
    if (err) err->clear();
    std::error_code source_ec;
    const bool source_is_directory = std::filesystem::is_directory(source, source_ec);
    if (source_ec) {
        if (err) *err = "cannot inspect world source: " + source_ec.message();
        return false;
    }
    if (!source_is_directory)
        return import_world_archive_impl(source, profile, target_name, err).success;

    std::filesystem::path profile_root;
    if (!profile_root_path(profile, profile_root, err)) return false;
    const std::filesystem::path level_dat = std::filesystem::path(source) / L"level.dat";
    std::error_code level_ec;
    if (!std::filesystem::is_regular_file(level_dat, level_ec) || level_ec) {
        if (err) *err = level_ec ? "cannot inspect world level.dat: " + level_ec.message()
                                 : "world directory has no level.dat";
        return false;
    }
    const std::filesystem::path saves_dir = profile_root / L"saves";
    std::string directory_error;
    if (!ensure_directory(saves_dir, &directory_error, "Bedrock world directory")) {
        if (err) *err = directory_error;
        return false;
    }
    std::filesystem::path destination;
    if (!unique_child(saves_dir, target_name, destination, &directory_error)) {
        if (err) *err = directory_error;
        return false;
    }
    if (!copy_directory(std::filesystem::path(source), destination, &directory_error)) {
        if (err) *err = directory_error;
        return false;
    }
    std::error_code verify_ec;
    if (!std::filesystem::is_regular_file(destination / L"level.dat", verify_ec) || verify_ec) {
        std::error_code cleanup_ec;
        std::filesystem::remove_all(destination, cleanup_ec);
        if (err) *err = verify_ec ? "cannot verify copied world: " + verify_ec.message()
                                  : "copied world has no level.dat";
        if (cleanup_ec && err) *err += "; failed to remove partial world import: " + cleanup_ec.message();
        return false;
    }
    return true;
}

bool export_world(const BedrockProfile& profile, const std::string& world_folder,
                  const std::wstring& dest_path, std::string* err) {
    if (err) err->clear();
    if (world_folder.empty() || !safe_path_component(net::to_wide(world_folder))) {
        if (err) *err = "invalid Bedrock world folder";
        return false;
    }

    std::filesystem::path profile_root;
    if (!profile_root_path(profile, profile_root, err)) return false;
    const std::filesystem::path saves_dir = profile_root / L"saves";
    const std::filesystem::path world_dir = saves_dir / net::to_wide(world_folder);
    std::error_code ec;
    if (!std::filesystem::is_directory(world_dir, ec) || ec) {
        if (err) *err = ec ? "cannot inspect Bedrock world: " + ec.message()
                           : "World directory not found: " + world_dir.string();
        return false;
    }

    std::wstring tar_args = L"-a -c -f \"" + dest_path +
                       L"\" -C \"" + saves_dir.wstring() + L"\" \"" +
                       net::to_wide(world_folder) + L"\"";
    int exit_code = -1;
    std::string run_err;
    if (!extract::run_command(L"C:\\Windows\\System32\\tar.exe", tar_args,
                              saves_dir.wstring(), 120000, &exit_code, &run_err)) {
        if (err) *err = run_err.empty() ? "Failed to launch tar" : run_err;
        return false;
    }
    if (exit_code != 0) {
        if (err) *err = "tar exited with code " + std::to_string(exit_code);
        return false;
    }

    return true;
}

}  // namespace aml::bedrock
