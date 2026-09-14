#pragma once

#include "instances.h"

#include <functional>
#include <string>

namespace aml::official_launcher {

struct AmalgamProfile {
    std::string profile_id;
    std::string name;
    std::string minecraft_version;
    std::string loader_type;
    std::string loader_version;
    int java_version = 0;
    std::wstring java_executable;
    std::wstring game_directory;
    std::wstring mods_directory;
    std::wstring config_directory;
    std::wstring saves_directory;
    std::wstring resource_pack_directory;
    std::wstring shader_directory;
    std::wstring native_agent_path;
    std::wstring bridge_jar_path;
    int memory_min_mb = 512;
    int memory_max_mb = 4096;
    bool launcher_bridge_enabled = true;
};

bool IsOfficialLauncherInstalled();
std::wstring FindOfficialLauncher();
std::wstring FindMinecraftDirectory();
AmalgamProfile FromInstance(const instances::Instance& instance, int java_version);
bool PrepareProfile(const AmalgamProfile& profile, std::string* error = nullptr);
bool RegisterInstallation(const AmalgamProfile& profile, std::string* error = nullptr);
bool OpenOfficialLauncher();
bool OpenProfileFolder(const AmalgamProfile& profile);

// Install a mod loader (Forge/NeoForge/Fabric/Quilt) into the official
// launcher's .minecraft/ directory so the registered profile can find
// the required version JSON and libraries.
typedef std::function<bool(float, const std::string&)> InstallProgress;
bool InstallLoader(const AmalgamProfile& profile, InstallProgress progress,
                   std::string* error = nullptr);

}  // namespace aml::official_launcher
