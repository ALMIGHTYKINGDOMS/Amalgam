#include "official_launcher_bridge.h"

#include "extract.h"
#include "json.h"
#include "net.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <filesystem>
#include <algorithm>
#include <cctype>
#include <ctime>
#include <sstream>

namespace aml::official_launcher {

namespace {

std::wstring env_path(const wchar_t* name) {
    wchar_t value[32768]{};
    const DWORD length = GetEnvironmentVariableW(name, value, 32768);
    return length > 0 && length < 32768 ? std::wstring(value, length) : std::wstring();
}

std::wstring appdata_path() {
    wchar_t value[MAX_PATH]{};
    const HRESULT result = SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, value);
    return result == S_OK ? std::wstring(value) : std::wstring();
}

std::wstring launcher_profiles_path() {
    const std::wstring override_path = env_path(L"AMALGAM_LAUNCHER_PROFILES_PATH");
    if (!override_path.empty()) return override_path;
    const std::wstring appdata = appdata_path();
    return appdata.empty() ? std::wstring() : appdata + L"\\.minecraft\\launcher_profiles.json";
}

std::string utc_timestamp() {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
    gmtime_s(&utc, &now);
    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S.000Z", &utc);
    return buffer;
}

// Search for the Minecraft Launcher in common desktop and Store locations.
// NOTE: Microsoft.MinecraftJavaEdition is the game package. The Store
// launcher package exposes GameLaunchHelper.exe as its full-trust entry point.
std::wstring find_appx_launcher() {
    const std::wstring local = env_path(L"LOCALAPPDATA");
    const wchar_t* package_names[] = {
        L"Microsoft.4297127D64EC6",
        L"Microsoft.4297127D64EC6_8wekyb3d8bbwe",
    };

    auto find_in_package = [](const std::wstring& base) -> std::wstring {
        if (base.empty()) return {};
        const std::wstring dirs_to_check[] = {
            base,
            base + L"\\Game",
            base + L"\\MinecraftLauncher",
            base + L"\\LocalCache\\Local\\game",
        };
        const wchar_t* executable_names[] = {
            L"GameLaunchHelper.exe",
            L"MinecraftLauncher.exe",
        };
        for (const auto& dir : dirs_to_check) {
            for (const auto* name : executable_names) {
                const std::wstring exe = dir + L"\\" + name;
                if (net::file_exists(exe)) return exe;
            }
        }
        return {};
    };

    // Some older Store layouts expose the package under the user's package
    // cache. Keep this cheap probe for those versions.
    for (const auto* pkg : package_names) {
        const std::wstring candidate = find_in_package(local + L"\\Packages\\" + pkg);
        if (!candidate.empty()) return candidate;
    }

    // Resolve the actual package install location. This helper is launched
    // hidden and its output is treated as data; trim line endings only so
    // spaces in "Program Files\\WindowsApps" remain intact.
    std::string output;
    std::string error;
    const bool ran = extract::run_capture(
        L"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe",
        L"-NoProfile -NonInteractive -Command \""
        L"Get-AppxPackage -Name 'Microsoft.4297127D64EC6' | "
        L"Select-Object -ExpandProperty InstallLocation\"",
        &output, &error, 15000);
    std::string location;
    std::istringstream lines(output);
    std::getline(lines, location);
    while (!location.empty() && std::isspace(static_cast<unsigned char>(location.back())))
        location.pop_back();
    size_t first = 0;
    while (first < location.size() && std::isspace(static_cast<unsigned char>(location[first]))) ++first;
    if (first > 0) location.erase(0, first);
    if (ran && !location.empty()) {
        const std::wstring candidate = find_in_package(net::to_wide(location));
        if (!candidate.empty()) return candidate;
    }
    return std::wstring();
}

}  // namespace

std::wstring FindOfficialLauncher() {
    const std::wstring local = env_path(L"LOCALAPPDATA");
    const std::wstring program_files = env_path(L"PROGRAMFILES");
    const std::wstring program_files_x86 = env_path(L"PROGRAMFILES(X86)");
    const std::wstring candidates[] = {
        local + L"\\Programs\\Minecraft Launcher\\MinecraftLauncher.exe",
        program_files + L"\\Minecraft Launcher\\MinecraftLauncher.exe",
        program_files_x86 + L"\\Minecraft Launcher\\MinecraftLauncher.exe",
    };
    for (const auto& candidate : candidates) {
        if (!candidate.empty() && net::file_exists(candidate)) return candidate;
    }
    // Try to find the Store/AppX launcher — returns the actual .exe path,
    // NOT the game URI.
    const std::wstring appx = find_appx_launcher();
    if (!appx.empty()) return appx;
    return std::wstring();
}

bool IsOfficialLauncherInstalled() {
    return !FindOfficialLauncher().empty();
}

AmalgamProfile FromInstance(const instances::Instance& instance, int java_version) {
    AmalgamProfile profile;
    profile.profile_id = instance.id;
    profile.name = instance.name;
    profile.minecraft_version = instance.minecraft_version;
    profile.loader_type = instance.loader;
    profile.loader_version = instance.loader_version;
    profile.java_version = java_version;
    profile.java_executable = instance.java_path.empty() ? L"" : net::to_wide(instance.java_path);
    profile.game_directory = instance.directory;
    profile.mods_directory = instance.directory + L"\\mods";
    profile.config_directory = instance.directory + L"\\config";
    profile.saves_directory = instance.directory + L"\\saves";
    profile.resource_pack_directory = instance.directory + L"\\resourcepacks";
    profile.shader_directory = instance.directory + L"\\shaderpacks";
    if (instance.memory_mb > 0) profile.memory_max_mb = instance.memory_mb;
    return profile;
}

bool PrepareProfile(const AmalgamProfile& profile, std::string* error) {
    if (profile.profile_id.empty() || profile.game_directory.empty() ||
        profile.minecraft_version.empty()) {
        if (error) *error = "profile is missing an id, game directory, or Minecraft version";
        return false;
    }
    const std::wstring directories[] = {
        profile.game_directory, profile.mods_directory, profile.config_directory,
        profile.saves_directory, profile.resource_pack_directory,
        profile.shader_directory, profile.game_directory + L"\\screenshots",
        profile.game_directory + L"\\kubejs", profile.game_directory + L"\\defaultconfigs",
    };
    for (const auto& directory : directories) {
        if (!net::mkdirs(directory)) {
            if (error) *error = "cannot create profile directory: " + net::to_utf8(directory);
            return false;
        }
    }
    if (!profile.bridge_jar_path.empty()) {
        const std::wstring target = profile.mods_directory + L"\\amalgam.jar";
        if (!net::file_exists(profile.bridge_jar_path) ||
            (!CopyFileW(profile.bridge_jar_path.c_str(), target.c_str(), FALSE) &&
             !CopyFileW(profile.bridge_jar_path.c_str(), target.c_str(), TRUE))) {
            if (error) *error = "cannot install the Amalgam bridge mod";
            return false;
        }
    }
    Json metadata = Json::obj();
    metadata.set("profileId", Json::str(profile.profile_id));
    metadata.set("name", Json::str(profile.name));
    metadata.set("minecraftVersion", Json::str(profile.minecraft_version));
    metadata.set("loaderType", Json::str(profile.loader_type));
    metadata.set("loaderVersion", Json::str(profile.loader_version));
    metadata.set("javaVersion", Json::num(profile.java_version));
    metadata.set("launcherBridgeEnabled", Json::boolean(profile.launcher_bridge_enabled));
    return json_write_file(profile.game_directory + L"\\amalgam-profile.json", metadata, error);
}

bool RegisterInstallation(const AmalgamProfile& profile, std::string* error) {
    const std::wstring path = launcher_profiles_path();
    if (path.empty()) {
        if (error) *error = "Windows AppData path is unavailable";
        return false;
    }
    Json root = Json::obj();
    if (net::file_exists(path)) {
        std::string parse_error;
        if (!json_parse_file(path, root, &parse_error) || !root.isObject()) {
            if (error) *error = parse_error.empty() ? "official launcher profile file is invalid" : parse_error;
            return false;
        }
    }
    // Ensure required top-level fields exist.
    if (!root.get("authenticationDatabase").isObject())
        root.set("authenticationDatabase", Json::obj());
    if (root.get("clientToken").isNull())
        root.set("clientToken", Json::str("a0000000000000000000000000000000"));
    if (!root.get("launcherVersion").isObject()) {
        Json lv = Json::obj();
        lv.set("format", Json::num(21));
        lv.set("name", Json::str("2.1.1589"));
        lv.set("profilesFormat", Json::num(2));
        root.set("launcherVersion", lv);
    }
    Json profiles = root.get("profiles");
    if (!profiles.isObject()) profiles = Json::obj();
    const std::string key = "amalgam-" + profile.profile_id;
    Json installation = Json::obj();
    installation.set("name", Json::str(profile.name));
    installation.set("type", Json::str("custom"));
    installation.set("icon", Json::str("Grass"));
    const std::string timestamp = utc_timestamp();
    installation.set("created", Json::str(timestamp));
    installation.set("lastUsed", Json::str(timestamp));
    installation.set("gameDir", Json::str(net::to_utf8(profile.game_directory)));
    if (!profile.java_executable.empty()) {
        installation.set("javaDir", Json::str(net::to_utf8(
            std::filesystem::path(profile.java_executable).parent_path().parent_path().wstring())));
    }
    std::string java_args = "-Xms" + std::to_string(profile.memory_min_mb) + "M -Xmx" +
                            std::to_string(profile.memory_max_mb) + "M";
    if (!profile.native_agent_path.empty()) {
        const std::string agent = net::to_utf8(profile.native_agent_path);
        // The official launcher stores JVM arguments as one command-line string.
        // Quote the path so an installed launcher under "Program Files" remains valid.
        java_args += " -agentpath:\"" + agent + "\"";
        java_args += " -Damalgam.dll.path=\"" + agent + "\"";
    }
    installation.set("javaArgs", Json::str(java_args));
    const std::string version_id = profile.loader_version.empty()
        ? profile.minecraft_version
        : profile.minecraft_version + "-" + profile.loader_type + "-" + profile.loader_version;
    installation.set("lastVersionId", Json::str(version_id));
    profiles.set(key, std::move(installation));
    root.set("profiles", std::move(profiles));
    // Set selectedUser.profile so the official launcher selects the Amalgam
    // installation on startup without disturbing the user's account token.
    Json selected = root.get("selectedUser");
    if (!selected.isObject()) selected = Json::obj();
    selected.set("profile", Json::str(key));
    root.set("selectedUser", selected);
    // Keep a small bridge marker in the installation entry so the launcher can
    // explain the handoff/recovery path without relying on a fake launch state.
    profiles = root.get("profiles");
    if (profiles.isObject()) {
        Json selected_installation = profiles.get(key);
        if (selected_installation.isObject()) {
            selected_installation.set("amalgamBridge", Json::boolean(true));
            profiles.set(key, selected_installation);
            root.set("profiles", profiles);
        }
    }
    const std::wstring parent = std::filesystem::path(path).parent_path().wstring();
    if (!net::mkdirs(parent)) {
        if (error) *error = "cannot create the official launcher profile directory";
        return false;
    }

    // Never risk corrupting the user's official launcher state. Keep one
    // recovery copy and replace the file atomically after the complete JSON
    // document has been written.
    const std::wstring backup = path + L".amalgam-backup";
    if (net::file_exists(path)) CopyFileW(path.c_str(), backup.c_str(), FALSE);
    const std::wstring temporary = path + L".amalgam-tmp";
    if (!json_write_file(temporary, root, error)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        if (error) *error = "cannot update the official Minecraft Launcher profile file";
        return false;
    }
    return true;
}

bool OpenOfficialLauncher() {
    const std::wstring launcher = FindOfficialLauncher();
    if (launcher.empty()) return false;
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", launcher.c_str(),
                                                    nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}

bool OpenProfileFolder(const AmalgamProfile& profile) {
    if (profile.game_directory.empty() || !net::directory_exists(profile.game_directory)) return false;
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", profile.game_directory.c_str(),
                                                    nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}

std::wstring FindMinecraftDirectory() {
    const std::wstring appdata = appdata_path();
    if (appdata.empty()) return std::wstring();
    return appdata + L"\\.minecraft";
}

// ── Loader installation into the official launcher's .minecraft/ ───────
//
// The official launcher resolves profiles via `lastVersionId` which must
// reference a version directory inside .minecraft/versions/.  For modded
// profiles the loader's version JSON, patched client, and libraries must
// be installed there BEFORE we register the profile.
//
// Forge / NeoForge: run the official installer with --installClient.
// Fabric / Quilt:   drop the loader profile JSON into versions/<id>/.
// Vanilla:          nothing to install.

namespace {

// Detect whether the loader version is already installed by checking for
// the version JSON inside .minecraft/versions/.
bool loader_already_installed(const std::wstring& mc_dir, const std::string& loader,
                              const std::string& loader_version, const std::string& mc_version) {
    if (loader.empty() || loader_version.empty() || mc_version.empty()) return false;
    // Every supported profile installer uses this canonical ID. Do not use a
    // substring search here: `1.20.1-forge-47.2.0` must never satisfy a
    // request for Forge 47.4.1 merely because both directories contain
    // "forge" and "1.20.1".
    const std::wstring version_id = net::to_wide(
        mc_version + "-" + loader + "-" + loader_version);
    const std::wstring version_dir = mc_dir + L"\\versions\\" + version_id;
    const std::wstring version_json = version_dir + L"\\" + version_id + L".json";
    return net::file_exists(version_json);
}

// Forge / NeoForge: download the installer JAR and run --installClient.
bool install_installer_loader(const std::wstring& java_exe, const std::wstring& mc_dir,
                              const std::string& loader, const std::string& loader_version,
                              const std::string& mc_version,
                              InstallProgress progress, std::string* err) {
    const std::wstring versions = mc_dir + L"\\versions";
    net::mkdirs(versions);

    // Ensure a minimal launcher_profiles.json exists so the installer does not abort.
    const std::wstring profiles_json = mc_dir + L"\\launcher_profiles.json";
    if (!net::file_exists(profiles_json)) {
        Json root = Json::obj();
        root.set("authenticationDatabase", Json::obj());
        root.set("clientToken", Json::str("a0000000000000000000000000000000"));
        Json lv = Json::obj();
        lv.set("format", Json::num(21));
        lv.set("name", Json::str("2.1.1589"));
        lv.set("profilesFormat", Json::num(2));
        root.set("launcherVersion", lv);
        Json profiles = Json::obj();
        root.set("profiles", profiles);
        std::string write_err;
        json_write_file(profiles_json, root, &write_err);
    }

    if (progress) progress(0.1f, "Downloading " + loader + " installer");

    // Build the installer download URL.
    const std::string installer_name = loader + "-" + loader_version + "-installer.jar";
    std::string maven_path;
    if (loader == "forge") {
        maven_path = "net/minecraftforge/forge/" + loader_version + "/" + installer_name;
    } else if (loader == "neoforge") {
        maven_path = "net/neoforged/neoforge/" + loader_version + "/" + installer_name;
    } else {
        if (err) *err = "unsupported installer loader: " + loader;
        return false;
    }
    const std::wstring installer_dir = mc_dir + L"\\.amalgam-installers";
    net::mkdirs(installer_dir);
    const std::wstring installer_path = installer_dir + L"\\" + net::to_wide(installer_name);

    if (!net::file_exists(installer_path) || net::file_size(installer_path) == 0) {
        std::string base_url;
        if (loader == "forge") {
            base_url = "https://maven.minecraftforge.net/" + maven_path;
        } else {
            base_url = "https://maven.neoforged.net/releases/" + maven_path;
        }
        std::string dl_err;
        if (!net::download(net::to_wide(base_url), installer_path, nullptr, &dl_err)) {
            if (err) *err = "failed to download " + loader + " installer: " + dl_err;
            return false;
        }
    }

    if (progress) progress(0.4f, "Running " + loader + " installer");

    // Resolve Java for the installer.
    if (java_exe.empty()) {
        if (err) *err = "no Java runtime available for " + loader + " installer";
        return false;
    }
    const std::wstring java_bin = java_exe;
    std::wstring java_dir = java_bin;
    {   size_t slash = java_dir.find_last_of(L"\\/");
        if (slash != std::wstring::npos) java_dir = java_dir.substr(0, slash); }
    // Strip trailing \bin if present.
    if (java_dir.size() > 4 && java_dir.substr(java_dir.size() - 4) == L"\\bin")
        java_dir = java_dir.substr(0, java_dir.size() - 4);

    // Run the installer.  The Forge/NeoForge installer accepts --installClient
    // and writes version JSON + patched client into the target directory.
    const std::wstring args = L"-jar \"" + installer_path + L"\" --installClient \"" +
                              mc_dir + L"\"";
    std::string installer_output;
    int exit_code = -1;
    if (!extract::run_command(java_dir + L"\\bin\\java.exe", args, mc_dir,
                              300000, &exit_code, err, &installer_output)) {
        if (err && err->empty()) *err = "failed to launch " + loader + " installer";
        return false;
    }
    // Log installer output lines.
    {   std::istringstream lines(installer_output);
        std::string line;
        while (std::getline(lines, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.size() > 2000) line.resize(2000);
            if (progress) progress(0.8f, line.empty() ? "Installing loader..." : line);
        }
    }

    // For Forge/NeoForge the installer may exit nonzero while still producing
    // valid version JSON (e.g. when it fails to write a launcher profile but
    // the core installation succeeded).  Accept it if the version JSON exists.
    if (exit_code != 0) {
        const std::wstring version_id = net::to_wide(mc_version + "-" + loader + "-" + loader_version);
        const std::wstring json_check = versions + L"\\" + version_id + L"\\" + version_id + L".json";
        if (!net::file_exists(json_check)) {
            if (err && err->empty()) *err = loader + " installer failed (exit " +
                std::to_string(exit_code) + ")";
            return false;
        }
    }
    return true;
}

// Fabric / Quilt: download the loader profile JSON from the official API
// and place it in .minecraft/versions/<id>/<id>.json.
bool install_profile_loader(const std::wstring& mc_dir, const std::string& loader,
                            const std::string& loader_version, const std::string& mc_version,
                            InstallProgress progress, std::string* err) {
    const std::string version_id = mc_version + "-" + loader + "-" + loader_version;
    const std::wstring version_dir = mc_dir + L"\\versions\\" + net::to_wide(version_id);
    const std::wstring version_json = version_dir + L"\\" + net::to_wide(version_id) + L".json";
    // Already installed?
    if (net::file_exists(version_json)) return true;

    if (progress) progress(0.1f, "Downloading " + loader + " loader profile");

    std::string base_url;
    if (loader == "fabric") {
        base_url = "https://meta.fabricmc.net/v2/versions/loader/" + mc_version +
                   "/" + loader_version + "/profile/json";
    } else if (loader == "quilt") {
        base_url = "https://meta.quiltmc.org/v3/versions/loader/" + mc_version +
                   "/" + loader_version + "/profile/json";
    } else {
        if (err) *err = "unsupported profile loader: " + loader;
        return false;
    }

    std::vector<uint8_t> bytes;
    std::string dl_err;
    if (!net::get(net::to_wide(base_url), bytes, &dl_err)) {
        if (err) *err = "failed to download " + loader + " profile: " + dl_err;
        return false;
    }
    std::string text(bytes.begin(), bytes.end());
    if (text.empty()) {
        if (err) *err = loader + " returned an empty profile";
        return false;
    }
    // The profile JSON from the API uses the original version ID.  Rewrite
    // the "id" field so the official launcher sees our canonical name.
    Json profile = Json::parse(text, err);
    if (profile.isNull()) {
        if (err && err->empty()) *err = "failed to parse " + loader + " profile JSON";
        return false;
    }
    profile.set("id", Json::str(version_id));
    // Also update the "inheritsFrom" if present to reference the MC version.
    if (!profile.get("inheritsFrom").as_str().empty()) {
        // Keep inheritsFrom as-is — it references the base MC version which is correct.
    }

    if (progress) progress(0.5f, "Installing " + loader + " loader");

    net::mkdirs(version_dir);
    std::string write_err;
    if (!json_write_file(version_json, profile, &write_err)) {
        if (err) *err = "failed to write " + loader + " version JSON: " + write_err;
        return false;
    }
    return true;
}

}  // anonymous namespace

bool InstallLoader(const AmalgamProfile& profile, InstallProgress progress,
                   std::string* error) {
    // Vanilla profiles need no loader installation.
    if (profile.loader_type.empty() || profile.loader_type == "vanilla") return true;
    if (profile.minecraft_version.empty() || profile.loader_version.empty()) {
        if (error) *error = "the profile is missing an exact Minecraft or loader version";
        return false;
    }
    if (profile.loader_type != "forge" && profile.loader_type != "neoforge" &&
        profile.loader_type != "fabric" && profile.loader_type != "quilt") {
        if (error) *error = "unsupported client loader: " + profile.loader_type;
        return false;
    }

    const std::wstring mc_dir = FindMinecraftDirectory();
    if (mc_dir.empty()) {
        if (error) *error = "cannot locate the Minecraft data directory";
        return false;
    }

    // Check if already installed.
    if (loader_already_installed(mc_dir, profile.loader_type, profile.loader_version,
                                profile.minecraft_version)) {
        if (progress) progress(1.0f, profile.loader_type + " already installed");
        return true;
    }

    // Determine Java for the installer.
    std::wstring java_exe = profile.java_executable;

    const bool installer_based = (profile.loader_type == "forge" || profile.loader_type == "neoforge");
    if (installer_based) {
        return install_installer_loader(java_exe, mc_dir, profile.loader_type,
                                        profile.loader_version, profile.minecraft_version,
                                        progress, error);
    } else {
        // Fabric, Quilt, and other profile-based loaders.
        return install_profile_loader(mc_dir, profile.loader_type, profile.loader_version,
                                      profile.minecraft_version, progress, error);
    }
}

}  // namespace aml::official_launcher
