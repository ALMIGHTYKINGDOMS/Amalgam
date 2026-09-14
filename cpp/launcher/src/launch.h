#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace aml::launch {

struct Options {
    std::string mc_id;
    std::string loader = "auto";
    std::wstring base_dir;
    std::wstring instance_dir;
    std::wstring assets_dir;
    std::wstring java_cache_dir;
    std::wstring username;
    std::string performance_profile = "auto";
    std::string auth_uuid;
    std::string auth_access_token;
    std::string auth_session;
    std::string auth_user_type;
    int width = 854;
    int height = 480;
    std::string extra_jvm;
    std::wstring test_server;
    bool addon = true;
    std::wstring dll_path;
    std::wstring bridges_dir;
    std::vector<std::pair<int, std::wstring>> java_overrides;
    std::wstring java_path;          // per-instance java home (or java.exe path)
    int memory_mb = 0;               // per-instance heap; 0 = default
    std::wstring loader_version;     // pinned loader version; empty = resolve latest
    bool wait_for_exit = false;
    bool dry_run = false;
    bool require_account = true;
};

struct Result {
    std::wstring command_line;
    DWORD pid = 0;
    int exit_code = 0;
    std::wstring loader_version;
    std::wstring instance_dir;
    std::wstring java_home;
};

bool run(const Options& opt, const std::function<void(const std::wstring&)>& log, Result* out,
         std::string* err);

}  // namespace aml::launch
