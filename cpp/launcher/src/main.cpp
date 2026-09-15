#include "ai.h"
#include "account_manager.h"
#include "auth.h"
#include "crash_report.h"
#include "bedrock.h"
#include "config.h"
#include "import_pack.h"
#include "instances.h"
#include "java.h"
#include "launch.h"
#include "official_launcher_bridge.h"
#include "model.h"
#include "mods.h"
#include "net.h"
#include "online_config.h"
#include "readiness.h"
#include "provider_config.h"
#include "services.h"
#include "ui.h"
#include "updater.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <io.h>
#include <string>
#include <vector>

namespace {

bool has_arg(int argc, wchar_t** argv, const wchar_t* needle) {
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], needle) == 0) return true;
    }
    return false;
}

std::wstring value_after_arg(int argc, wchar_t** argv, const wchar_t* needle) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (_wcsicmp(argv[i], needle) == 0) return argv[i + 1];
    }
    return {};
}

// The launcher is a GUI executable so normal double-click starts the polished
// window.  Attach to the caller's terminal only for documented CLI commands;
// without this, stdio succeeds but has nowhere visible to go in PowerShell or
// cmd.exe.
bool bind_cli_stream(DWORD handle_kind, FILE* stream, int mode) {
    const HANDLE handle = GetStdHandle(handle_kind);
    if (!handle || handle == INVALID_HANDLE_VALUE) return false;
    const int descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(handle), mode);
    if (descriptor == -1) return false;
    if (_dup2(descriptor, _fileno(stream)) == -1) {
        _close(descriptor);
        return false;
    }
    // The process is about to exit after a CLI command, so retaining the
    // temporary descriptor avoids closing the inherited console/pipe handle.
    return true;
}

void attach_cli_output() {
    AttachConsole(ATTACH_PARENT_PROCESS);
    const bool output_bound = bind_cli_stream(STD_OUTPUT_HANDLE, stdout, _O_TEXT);
    bind_cli_stream(STD_ERROR_HANDLE, stderr, _O_TEXT);
    bind_cli_stream(STD_INPUT_HANDLE, stdin, _O_TEXT);
    if (!output_bound) {
        FILE* stream = nullptr;
        freopen_s(&stream, "CONOUT$", "w", stdout);
        freopen_s(&stream, "CONOUT$", "w", stderr);
        freopen_s(&stream, "CONIN$", "r", stdin);
    }
    if (GetConsoleOutputCP() != 0) SetConsoleOutputCP(CP_UTF8);
}

bool is_cli_invocation(int argc, wchar_t** argv) {
    // --safe-mode is a windowed-launcher flag (handled below), not a CLI
    // command, so it must not short-circuit into the CLI dispatch.
    if (argc == 2 && _wcsicmp(argv[1], L"--safe-mode") == 0) return false;
    return has_arg(argc, argv, L"--versions") || has_arg(argc, argv, L"--check-java") ||
           has_arg(argc, argv, L"--check-official-launcher") ||
           has_arg(argc, argv, L"--official-handoff-probe") ||
           has_arg(argc, argv, L"--java-install") ||
           has_arg(argc, argv, L"--check-prereqs") || has_arg(argc, argv, L"--login") ||
           has_arg(argc, argv, L"--logout") || has_arg(argc, argv, L"--doctor") ||
           has_arg(argc, argv, L"--ui-snapshot") ||
           has_arg(argc, argv, L"--server-transport-probe") ||
           has_arg(argc, argv, L"--client-bridge-probe") ||
           has_arg(argc, argv, L"--inspect") || has_arg(argc, argv, L"--launch") ||
           has_arg(argc, argv, L"--check-update") ||
           has_arg(argc, argv, L"--mods-search") || has_arg(argc, argv, L"--mods-install") ||
           has_arg(argc, argv, L"--pack-install") ||
           has_arg(argc, argv, L"--ai-chat") || has_arg(argc, argv, L"--ai-image") ||
           has_arg(argc, argv, L"--ai-vision") || has_arg(argc, argv, L"--bedrock-info") ||
           has_arg(argc, argv, L"--bedrock-launch") || has_arg(argc, argv, L"--bedrock-install");
}

// The launcher doubles as its own diagnostics tool, but until --help existed
// the only way to discover a command was to read the source: an unknown flag
// printed a warning and then opened the window anyway.
void print_cli_help() {
    std::printf(
        "Amalgam Launcher %s\n"
        "\n"
        "Usage:\n"
        "  amalgam_launcher.exe                    open the graphical launcher\n"
        "  amalgam_launcher.exe --page <page>      open it on a specific page\n"
        "  amalgam_launcher.exe <command> [args]   run one command below\n"
        "\n"
        "Commands:\n"
        "  --versions [loader]                 list launchable Minecraft versions\n"
        "  --inspect <mc_id> [loader]          show what a launch would install\n"
        "  --launch <mc_id> [loader] [--dry-run] [--wait] [--print-command]\n"
        "  --check-java                        list detected Java runtimes\n"
        "  --java-install <8|11|17|21|25>      download a managed Java runtime\n"
        "  --check-official-launcher           locate the official Minecraft Launcher\n"
        "  --doctor [--online]                 report readiness to play\n"
        "  --check-prereqs                     verify bundled runtime prerequisites\n"
        "  --mods-search <query> [loader] [game_version] [facet]\n"
        "  --mods-install <slug> [source] [loader] [game_version] [mods_dir]\n"
        "  --pack-install <slug> [source] [loader] [game_version] [instances_dir]\n"
        "  --ai-chat <message> [model]         ask the Amalgam AI\n"
        "  --ai-image <prompt> [out.png]       generate an image\n"
        "  --ai-vision <image> [model]         describe an image\n"
        "  --bedrock-info | --bedrock-launch | --bedrock-install <file>\n"
        "  --login | --logout                  manage the Amalgam account session\n"
        "\n"
        "UI flags:\n"
        "  --safe-mode                         start without automatic network calls\n"
        "  --help, -h                          show this help\n"
        "  --version                           show the launcher version\n"
        "\n"
        "Release and QA tooling:\n"
        "  --check-update [url]                validate a signed update manifest\n"
        "  --ui-snapshot <out.png> [w] [h] [page]\n"
        "  --server-transport-probe <dir> <java.exe>\n"
        "  --client-bridge-probe <dir>\n"
        "  --official-handoff-probe <dir>\n",
        aml::ui::launcher_version());
}

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

// Official packages carry only client-safe public settings in
// launcher.json.template.  On a clean install, seed the private per-user
// launcher.json from that template before falling back to local defaults.
// This keeps provider/admin secrets out of release archives while ensuring
// Microsoft sign-in and the Supabase-backed catalog work on first launch.
bool load_launcher_config(const std::wstring& dir, aml::config::Config& cfg,
                          bool persist_if_missing = false) {
    const std::wstring config_path = dir + L"\\launcher.json";
    if (aml::config::load(config_path, cfg)) return true;

    const bool loaded_template =
        aml::config::load(dir + L"\\launcher.json.template", cfg);
    if (cfg.base_dir.empty()) cfg.base_dir = dir + L"\\instances";
    if (cfg.assets_dir.empty()) cfg.assets_dir = dir + L"\\assets";
    if (cfg.java_cache_dir.empty()) cfg.java_cache_dir = dir + L"\\runtimes\\java";
    if (cfg.loader.empty()) cfg.loader = "auto";
    cfg.addon = true;

    if (persist_if_missing && !aml::config::save(config_path, cfg)) return false;
    return loaded_template || persist_if_missing;
}

int cli_versions(int argc, wchar_t** argv) {
    std::string err;
    std::string loader;
    for (int i = 2; i < argc; ++i) {
        std::string a = aml::net::to_utf8(argv[i]);
        if (a == "fabric" || a == "quilt" || a == "forge" || a == "neoforge") loader = a;
    }
    std::vector<aml::model::ManifestEntry> all = aml::model::fetch_manifest(&err);
    if (!err.empty()) {
        std::printf("manifest error: %s\n", err.c_str());
        return 1;
    }
    std::vector<aml::model::ManifestEntry> filtered;
    for (const auto& e : all) {
        int rk = aml::model::rank(e.id);
        if (rk < 112) continue;
        if (!loader.empty()) {
            int floor = 0;
            if (loader == "fabric") floor = 1140;   // 1.14.0+
            else if (loader == "quilt") floor = 1144;  // 1.14.4+
            else if (loader == "neoforge") floor = 1201;  // 1.20.1+
            if (rk < floor) continue;
        }
        filtered.push_back(e);
    }
    std::sort(filtered.begin(), filtered.end(), [](const aml::model::ManifestEntry& a,
                                                   const aml::model::ManifestEntry& b) {
        int ra = aml::model::rank(a.id);
        int rb = aml::model::rank(b.id);
        if (ra != rb) return ra > rb;
        return a.id > b.id;
    });
    int count = 0;
    for (const auto& e : filtered) {
        std::printf("%s\t%s\n", e.id.c_str(), e.type.c_str());
        if (++count >= 40) break;
    }
    std::printf("total compatible: %d\n", static_cast<int>(filtered.size()));
    return 0;
}

std::wstring executable_dir() {
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring path = self;
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : path.substr(0, slash);
}

int cli_java() {
    std::vector<aml::java::Install> list = aml::java::scan_installed();
    aml::java::JavaRuntimeManager manager(
        executable_dir() + L"\\runtimes\\java");
    for (const auto& runtime : manager.GetInstalledRuntimes()) {
        const bool present = std::any_of(list.begin(), list.end(),
            [&runtime](const aml::java::Install& install) {
                return install.home == runtime.home;
            });
        if (!present) {
            aml::java::Install install;
            install.major = runtime.major;
            install.home = runtime.home;
            install.exe = runtime.executable;
            list.push_back(std::move(install));
        }
    }
    if (list.empty()) {
        std::printf("no java runtimes found\n");
        return 1;
    }
    for (const auto& j : list) {
        std::wprintf(L"Java %d  %s\n", j.major, j.home.c_str());
    }
    return 0;
}

int cli_java_install(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::printf("usage: --java-install <8|11|17|21|25>\n");
        return 2;
    }
    const int major = _wtoi(argv[2]);
    if (major != 8 && major != 11 && major != 17 && major != 21 && major != 25) {
        std::printf("unsupported managed Java version\n");
        return 2;
    }
    aml::java::JavaRuntimeManager manager(executable_dir() + L"\\runtimes\\java");
    auto progress = [](uint64_t done, uint64_t total) {
        static int last = -1;
        const int pct = total > 0 ? static_cast<int>(done * 100 / total) : -1;
        if (pct >= 0 && pct != last && (pct % 10 == 0 || pct == 100)) {
            last = pct;
            std::printf("download=%d%%\n", pct);
        }
        return true;
    };
    const auto result = manager.DownloadJava(major, aml::java::Architecture::X64, progress);
    if (!result.success) {
        std::printf("Java install failed: %s\n", result.error.c_str());
        return 1;
    }
    std::wprintf(L"Java %d installed at %s\n", result.runtime.major,
                 result.runtime.home.c_str());
    return 0;
}

int cli_official_launcher() {
    const std::wstring path = aml::official_launcher::FindOfficialLauncher();
    if (path.empty()) {
        std::printf("official-launcher=NOT_FOUND\n");
        return 1;
    }
    std::wprintf(L"official-launcher=OK\npath=%s\n", path.c_str());
    return 0;
}

int cli_prereqs() {
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring exe = self;
    size_t slash = exe.find_last_of(L"\\/");
    std::wstring dir = slash == std::wstring::npos ? L"." : exe.substr(0, slash);
    auto exists = [](const std::wstring& path) {
        return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    };
    bool tar = exists(L"C:\\Windows\\System32\\tar.exe");
    bool dll = exists(dir + L"\\amalgam.dll");
    HMODULE sqlite = LoadLibraryExW(L"winsqlite3.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE vcruntime = LoadLibraryW(L"VCRUNTIME140.dll");
    HMODULE vcruntime_1 = LoadLibraryW(L"VCRUNTIME140_1.dll");
    HMODULE cpp = LoadLibraryW(L"MSVCP140.dll");
    std::printf("tar=%s\n", tar ? "OK" : "MISSING");
    std::printf("amalgam.dll=%s\n", dll ? "OK" : "MISSING");
    std::printf("winsqlite3=%s\n", sqlite ? "OK" : "OPTIONAL");
    std::printf("msvc-runtime=%s\n", vcruntime && vcruntime_1 && cpp ? "OK" : "MISSING");
    if (sqlite) FreeLibrary(sqlite);
    if (vcruntime) FreeLibrary(vcruntime);
    if (vcruntime_1) FreeLibrary(vcruntime_1);
    if (cpp) FreeLibrary(cpp);
    return tar && dll && sqlite && vcruntime && vcruntime_1 && cpp ? 0 : 1;
}

std::wstring arg_at(int argc, wchar_t** argv, int idx);

int cli_official_handoff_probe(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::printf("usage: --official-handoff-probe <temporary-root>\\n");
        return 1;
    }
    const std::filesystem::path supplied_root(arg_at(argc, argv, 2));
    if (supplied_root.empty()) return 1;
    const std::filesystem::path probe_root = supplied_root /
        (L"amalgam-official-handoff-probe-" + std::to_wstring(GetCurrentProcessId()));
    const std::filesystem::path profiles_path = probe_root / L"launcher_profiles.json";
    const std::filesystem::path game_dir = probe_root / L"instances" / L"Forsaken World - test";
    std::error_code ec;
    std::filesystem::remove_all(probe_root, ec);
    std::filesystem::create_directories(game_dir, ec);
    if (ec) {
        std::printf("handoff probe setup failed: %s\\n", ec.message().c_str());
        return 2;
    }

    // This fixture intentionally contains unrelated user data and settings.
    // The probe proves registration preserves both while adding one Amalgam
    // installation and an atomic recovery copy.
    aml::Json fixture = aml::Json::obj();
    aml::Json profiles = aml::Json::obj();
    aml::Json unrelated = aml::Json::obj();
    unrelated.set("name", aml::Json::str("Unrelated Vanilla"));
    unrelated.set("type", aml::Json::str("custom"));
    unrelated.set("lastVersionId", aml::Json::str("1.21"));
    unrelated.set("gameDir", aml::Json::str("C:\\\\Users\\\\David\\\\Worlds\\\\vanilla"));
    profiles.set("unrelated-profile", unrelated);
    fixture.set("profiles", profiles);
    fixture.set("selectedUser", aml::Json::obj());
    fixture.set("settings", aml::Json::obj());
    fixture["settings"].set("keepLauncherOpen", aml::Json::boolean(true));
    fixture.set("version", aml::Json::num(6));
    std::string error;
    if (!aml::json_write_file(profiles_path.wstring(), fixture, &error)) {
        std::printf("handoff probe fixture failed: %s\\n", error.c_str());
        std::filesystem::remove_all(probe_root, ec);
        return 2;
    }

    SetEnvironmentVariableW(L"AMALGAM_LAUNCHER_PROFILES_PATH", profiles_path.wstring().c_str());
    aml::official_launcher::AmalgamProfile profile;
    profile.profile_id = "forge-probe";
    profile.name = "Amalgam - Forsaken World";
    profile.minecraft_version = "1.20.1";
    profile.loader_type = "forge";
    profile.loader_version = "47.2.17";
    profile.game_directory = game_dir.wstring();
    profile.mods_directory = (game_dir / L"mods").wstring();
    profile.config_directory = (game_dir / L"config").wstring();
    profile.saves_directory = (game_dir / L"saves").wstring();
    profile.resource_pack_directory = (game_dir / L"resourcepacks").wstring();
    profile.shader_directory = (game_dir / L"shaderpacks").wstring();

    const bool prepared = aml::official_launcher::PrepareProfile(profile, &error);
    const bool registered = prepared && aml::official_launcher::RegisterInstallation(profile, &error);
    aml::Json result;
    const bool parsed = registered && aml::json_parse_file(profiles_path.wstring(), result, &error);
    const aml::Json& result_profiles = result.get("profiles");
    const aml::Json& registered_profile = result_profiles.get("amalgam-forge-probe");
    const bool preserved_unrelated = result_profiles.get("unrelated-profile").get("lastVersionId").as_str() == "1.21" &&
                                     result.get("settings").get("keepLauncherOpen").as_bool(false);
    const bool exact_registration = registered_profile.get("lastVersionId").as_str() ==
                                        "1.20.1-forge-47.2.17" &&
                                    registered_profile.get("gameDir").as_str() ==
                                        aml::net::to_utf8(game_dir.wstring()) &&
                                    result.get("selectedUser").get("profile").as_str() ==
                                        "amalgam-forge-probe";
    const bool backup_present = aml::net::file_exists(profiles_path.wstring() + L".amalgam-backup");
    const bool temp_absent = !aml::net::file_exists(profiles_path.wstring() + L".amalgam-tmp");
    SetEnvironmentVariableW(L"AMALGAM_LAUNCHER_PROFILES_PATH", nullptr);
    std::filesystem::remove_all(probe_root, ec);

    if (!prepared || !registered || !parsed || !preserved_unrelated || !exact_registration ||
        !backup_present || !temp_absent) {
        std::printf("official handoff probe failed: prepared=%s registered=%s parsed=%s preserved=%s exact=%s backup=%s temp_clean=%s error=%s\\n",
                    prepared ? "yes" : "no", registered ? "yes" : "no", parsed ? "yes" : "no",
                    preserved_unrelated ? "yes" : "no", exact_registration ? "yes" : "no",
                    backup_present ? "yes" : "no", temp_absent ? "yes" : "no", error.c_str());
        return 3;
    }
    std::printf("official handoff: READY (isolated gameDir, exact loader, preserved settings, atomic backup)\\n");
    return 0;
}

int cli_login() {
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring exe_dir = self;
    const size_t slash = exe_dir.find_last_of(L"\\/");
    exe_dir = slash == std::wstring::npos ? L"." : exe_dir.substr(0, slash);
    aml::config::Config config;
    load_launcher_config(exe_dir, config);
    aml::auth::Account account;
    std::string err;
    if (!aml::auth::login_device(account, config.microsoft_client_id, [](const std::wstring& message) {
            std::wprintf(L"%s\n", message.c_str());
        }, &err)) {
        std::printf("login failed: %s\n", err.c_str());
        return 1;
    }
    if (!aml::auth::save(account, &err)) {
        std::printf("account save failed: %s\n", err.c_str());
        return 1;
    }
    std::printf("authenticated account: %s\n", account.username.c_str());
    return 0;
}

int cli_logout() {
    std::string err;
    if (!aml::auth::logout(&err)) {
        std::printf("logout failed: %s\n", err.c_str());
        return 1;
    }
    std::printf("account removed\n");
    return 0;
}

std::wstring arg_at(int argc, wchar_t** argv, int idx) {
    return idx < argc ? argv[idx] : std::wstring();
}

int cli_inspect(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::printf("usage: --inspect <mc_id> [loader]\n");
        return 1;
    }
    std::string mc = aml::net::to_utf8(arg_at(argc, argv, 2));
    std::string loader = argc >= 4 ? aml::net::to_utf8(arg_at(argc, argv, 3)) : std::string();
    std::string err;

    if (!loader.empty()) {
        std::string lv;
        if (!aml::model::resolve_loader_version(mc, loader, &lv, &err)) {
            std::printf("loader resolve failed: %s\n", err.c_str());
            return 1;
        }
        std::printf("loader=%s version=%s\n", loader.c_str(), lv.c_str());
        if (aml::model::is_installer_loader(loader)) {
            std::string iurl = aml::model::loader_installer_url(loader, lv);
            std::printf("installerUrl=%s\n", iurl.empty() ? "(none)" : iurl.c_str());
            return 0;
        }
        std::string url = aml::model::loader_profile_url(mc, loader, lv);
        std::vector<uint8_t> bytes;
        if (!aml::net::get(aml::net::to_wide(url), bytes, &err)) {
            std::printf("profile fetch failed: %s\n", err.c_str());
            return 1;
        }
        std::string text(bytes.begin(), bytes.end());
        aml::Json j = aml::Json::parse(text, &err);
        if (!err.empty()) {
            std::printf("json parse failed: %s\n", err.c_str());
            return 1;
        }
        aml::model::VersionJson vj;
        std::string inherits = j.get("inheritsFrom").as_str();
        if (!inherits.empty()) {
            if (!aml::model::merge_inherited(inherits, j, &vj, &err)) {
                std::printf("merge inherited failed: %s\n", err.c_str());
                return 1;
            }
            std::printf("inherits=%s merged mainClass=%s libs=%d java=%d assets=%s clientUrl=%s\n",
                        inherits.c_str(), vj.main_class.c_str(),
                        static_cast<int>(vj.libraries.size()), vj.java_major,
                        vj.assets_name.c_str(), vj.client.url.c_str());
        } else {
            aml::model::parse_version(j, vj);
            std::printf("mainClass=%s libs=%d java=%d assets=%s\n", vj.main_class.c_str(),
                        static_cast<int>(vj.libraries.size()), vj.java_major,
                        vj.assets_name.c_str());
        }
        return 0;
    }

    auto all = aml::model::fetch_manifest(&err);
    if (!err.empty()) {
        std::printf("manifest failed: %s\n", err.c_str());
        return 1;
    }
    for (const auto& e : all) {
        if (e.id == mc) {
            std::vector<uint8_t> bytes;
            if (!aml::net::get(aml::net::to_wide(e.url), bytes, &err)) {
                std::printf("fetch failed: %s\n", err.c_str());
                return 1;
            }
            std::string text(bytes.begin(), bytes.end());
            aml::Json j = aml::Json::parse(text, &err);
            if (!err.empty()) {
                std::printf("json parse failed: %s\n", err.c_str());
                return 1;
            }
            aml::model::VersionJson vj;
            aml::model::parse_version(j, vj);
            std::printf("id=%s type=%s mainClass=%s libs=%d java=%d assets=%s clientUrl=%s\n",
                        vj.id.c_str(), e.type.c_str(), vj.main_class.c_str(),
                        static_cast<int>(vj.libraries.size()), vj.java_major,
                        vj.assets_name.c_str(), vj.client.url.c_str());
            return 0;
        }
    }
    std::printf("version %s not found\n", mc.c_str());
    return 1;
}

int cli_launch(int argc, wchar_t** argv) {
    // Flags may appear anywhere, so the loader is the second argument that is
    // not a flag. Reading argv[3] directly made --launch <id> --wait and
    // --launch <id> --print-command fail with "unknown loader".
    std::vector<std::wstring> operands;
    for (int i = 2; i < argc; ++i) {
        if (argv[i][0] != L'-') operands.push_back(argv[i]);
    }
    if (operands.empty()) {
        std::printf("usage: --launch <mc_id> [loader] [--dry-run] [--wait]\n");
        return 1;
    }
    std::string mc = aml::net::to_utf8(operands[0]);
    std::string loader = operands.size() > 1 ? aml::net::to_utf8(operands[1]) : "auto";
    const bool dry = has_arg(argc, argv, L"--dry-run");
    const bool wait = has_arg(argc, argv, L"--wait");
    const bool print_command = has_arg(argc, argv, L"--print-command");
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring exe_dir = self;
    size_t slash = exe_dir.find_last_of(L"\\/");
    exe_dir = slash == std::wstring::npos ? L"." : exe_dir.substr(0, slash);

    aml::launch::Options opt;
    opt.mc_id = mc;
    opt.loader = loader;
    opt.base_dir = exe_dir;
    opt.assets_dir = exe_dir + L"\\assets";
    opt.java_cache_dir = exe_dir + L"\\runtimes\\java";
    opt.bridges_dir = exe_dir + L"\\bridges";
    opt.dll_path = exe_dir + L"\\amalgam.dll";
    // A dry-run still needs a launcher argument, but real launches replace
    // this with the authenticated Minecraft profile before the JVM starts.
    opt.username = L"Player";
    opt.addon = true;
    opt.dry_run = dry;
    opt.require_account = !dry;
    opt.wait_for_exit = wait;

    std::string err;
    aml::launch::Result res;
    bool ok = aml::launch::run(opt, [](const std::wstring& line) {
        std::wprintf(L"%s\n", line.c_str());
    }, &res, &err);
    std::printf("%s\n", ok ? "LAUNCH OK" : "LAUNCH FAILED");
    if (!err.empty()) std::printf("error: %s\n", err.c_str());
    if (print_command && !res.command_line.empty())
        std::wprintf(L"command: %s\n", res.command_line.c_str());
    return ok ? 0 : 1;
}

aml::mods::ApiCfg cli_api_cfg() {
    aml::mods::ApiCfg cfg;
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring p = self;
    size_t slash = p.find_last_of(L"\\/");
    std::wstring dir = slash == std::wstring::npos ? L"." : p.substr(0, slash);
    aml::config::Config c;
    if (load_launcher_config(dir, c)) {
        // CLI catalog commands run without the GUI startup path, so restore the
        // protected Amalgam session here as well. This lets authenticated
        // CurseForge proxy requests behave the same in --mods-search/install
        // and in Discover without ever shipping the provider secret.
        if (!c.supabase_url.empty() && !c.supabase_anon_key.empty()) {
            auto& supabase = aml::supabase::SupabaseManager::instance();
            supabase.initialize(c.supabase_url, c.supabase_anon_key,
                                c.supabase_service_key);
            auto& accounts = aml::account::AccountManager::instance();
            auto session = accounts.get_current_session();
            if (!session.id.empty()) {
                if (session.is_expired() && accounts.refresh_current_session())
                    session = accounts.get_current_session();
                if (!session.access_token.empty())
                    supabase.auto_login(session.access_token, session.refresh_token);
            }
        }
        cfg = aml::provider_config::make(c);
    }
    return cfg;
}

int cli_doctor(int argc, wchar_t** argv) {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring exe = self;
    const size_t slash = exe.find_last_of(L"\\/");
    const std::wstring exe_dir = slash == std::wstring::npos ? L"." : exe.substr(0, slash);
    aml::config::Config config;
    load_launcher_config(exe_dir, config);

    aml::readiness::Options options;
    options.launcher_dir = exe_dir;
        options.java_cache_dir = config.java_cache_dir.empty() ? exe_dir + L"\\runtimes\\java"
                                                           : config.java_cache_dir;
    options.provider_api = aml::provider_config::make(config);
    options.verify_providers = has_arg(argc, argv, L"--online");
    const aml::readiness::Report report = aml::readiness::run(options);
    for (const auto& check : report.checks) {
        std::printf("%s\t%s\t%s\n", aml::readiness::state_name(check.state),
                    check.label.c_str(), check.detail.c_str());
    }
    std::printf("JAVA_PLAY=%s\n", report.java_play_ready() ? "READY" : "ACTION_REQUIRED");
    return report.java_play_ready() ? 0 : 2;
}

// Point the updater at a server you control: --check-update prints the
// parsed manifest or the exact rejection reason. Loopback HTTP requires
// AMALGAM_TEST_LOOPBACK_HTTP=1; production always validates the HTTPS feed.
int cli_check_update(int argc, wchar_t* argv[]) {
    std::wstring url = value_after_arg(argc, argv, L"--check-update");
    if (url.empty()) url = aml::updater::default_manifest_url();
    aml::updater::UpdateInfo info;
    std::string err;
    if (!aml::updater::fetch_manifest(url, info, &err)) {
        std::printf("UPDATE-CHECK REJECTED: %s\n", err.c_str());
        return 1;
    }
    std::printf("UPDATE-CHECK OK version=%s channel=%s size=%lld\n", info.version.c_str(),
                info.channel.c_str(), static_cast<long long>(info.size));
    return 0;
}

int cli_ui_snapshot(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::printf("usage: --ui-snapshot <out.png> [width] [height] [home|discover|library|profile|project|downloads|settings|account|servers|server-detail|server-console|bedrock|essentials|admin|java|backups|logs|config|theme|performance|social|mods]\n");
        return 1;
    }
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring exe = self;
    const size_t slash = exe.find_last_of(L"\\/");
    const std::wstring exe_dir = slash == std::wstring::npos ? L"." : exe.substr(0, slash);
    aml::config::Config cfg;
    if (!load_launcher_config(exe_dir, cfg)) {
        cfg.base_dir = exe_dir;
        cfg.assets_dir = exe_dir + L"\\assets";
        cfg.java_cache_dir = exe_dir + L"\\runtimes\\java";
        cfg.loader = "auto";
        cfg.addon = true;
    }
    auto positive_dimension = [](const std::wstring& value, int fallback) {
        if (value.empty()) return fallback;
        wchar_t* end = nullptr;
        const long parsed = std::wcstol(value.c_str(), &end, 10);
        return end && *end == L'\0' && parsed >= 320 && parsed <= 7680
            ? static_cast<int>(parsed) : fallback;
    };
    aml::ui::RunOptions options;
    options.fixture_mode = true;
    options.capture_path = arg_at(argc, argv, 2);
    options.capture_after_frames = 180;
    options.initial_width = positive_dimension(arg_at(argc, argv, 3), 1440);
    options.initial_height = positive_dimension(arg_at(argc, argv, 4), 900);
    const std::string route = lowercase(aml::net::to_utf8(arg_at(argc, argv, 5)));
    if (route == "discover") {
        options.fixture_tab = 16;
        options.fixture_sidebar = 2;
    } else if (route == "library") {
        options.fixture_tab = 6;
        options.fixture_sidebar = 3;
    } else if (route == "profile") {
        options.fixture_tab = 6;
        options.fixture_sidebar = 3;
        options.fixture_profile_detail = true;
    } else if (route == "project") {
        options.fixture_tab = 1;
        options.fixture_sidebar = 2;
        options.fixture_project_detail = true;
    } else if (route == "downloads") {
        options.fixture_tab = 17;
        options.fixture_sidebar = 17;
    } else if (route == "settings") {
        options.fixture_tab = 4;
        options.fixture_sidebar = 12;
        options.fixture_settings_section = 3;
    } else if (route == "settings0") {
        options.fixture_tab = 4;
        options.fixture_sidebar = 12;
        options.fixture_settings_section = 0;
    } else if (route == "admin") {
        options.fixture_tab = 4;
        options.fixture_sidebar = 12;
        options.fixture_settings_section = 12;
    } else if (route == "account") {
        options.fixture_tab = 15;
        options.fixture_sidebar = 12;
    } else if (route == "servers") {
        options.fixture_tab = 8;
        options.fixture_sidebar = 8;
    } else if (route == "server-detail") {
        options.fixture_tab = 8;
        options.fixture_sidebar = 8;
        options.fixture_server_detail_tab = 0;
    } else if (route == "server-console") {
        options.fixture_tab = 8;
        options.fixture_sidebar = 8;
        options.fixture_server_detail_tab = 1;
    } else if (route == "cloud") {
        options.fixture_tab = 8;
        options.fixture_sidebar = 8;
        options.fixture_cloud = true;
    } else if (route == "bedrock") {
        options.fixture_tab = 3;
        options.fixture_sidebar = 17;
    } else if (route == "essentials") {
        options.fixture_tab = 23;
        options.fixture_sidebar = 23;
    } else if (route == "java") {
        options.fixture_tab = 11;
        options.fixture_sidebar = 12;
    } else if (route == "backups") {
        options.fixture_tab = 12;
        options.fixture_sidebar = 12;
    } else if (route == "logs") {
        options.fixture_tab = 13;
        options.fixture_sidebar = 12;
    } else if (route == "config") {
        options.fixture_tab = 14;
        options.fixture_sidebar = 12;
    } else if (route == "theme") {
        options.fixture_tab = 22;
        options.fixture_sidebar = 12;
    } else if (route == "performance") {
        options.fixture_tab = 21;
        options.fixture_sidebar = 12;
    } else if (route == "social") {
        options.fixture_tab = 23;
        options.fixture_sidebar = 23;
    } else if (route == "mods") {
        options.fixture_tab = 20;
        options.fixture_sidebar = 2;
    } else if (!route.empty() && route != "home") {
        std::printf("unknown ui snapshot route: %s\n", route.c_str());
        return 1;
    }
    const bool ok = aml::ui::run_window(&cfg, options);
    std::wprintf(L"ui snapshot %s: %s\n", ok ? L"created" : L"failed",
                 options.capture_path.c_str());
    return ok ? 0 : 1;
}

int cli_server_transport_probe(int argc, wchar_t** argv) {
    if (argc < 4) {
        std::printf("usage: --server-transport-probe <temporary-root> <java.exe>\n");
        return 1;
    }
    const std::wstring root = arg_at(argc, argv, 2);
    const std::wstring java_path = arg_at(argc, argv, 3);
    if (root.empty() || java_path.empty()) return 1;
    SetEnvironmentVariableW(L"AMALGAM_SERVER_ROOT", root.c_str());

    // Use the same accessor the Servers page uses, so this probe also proves
    // the launcher actually constructs the supervisor it drives.
    aml::services::ServerManager* manager = aml::services::local_server_manager();
    if (!manager) {
        SetEnvironmentVariableW(L"AMALGAM_SERVER_ROOT", nullptr);
        std::printf("server transport start failed: no local server supervisor\n");
        return 2;
    }
    std::string error;
    if (!manager->start_local_server("probe", aml::net::to_utf8(java_path), 512, &error)) {
        SetEnvironmentVariableW(L"AMALGAM_SERVER_ROOT", nullptr);
        std::printf("server transport start failed: %s\n", error.c_str());
        return 2;
    }

    // The supervisor owns local run state: the server reads as running exactly
    // while its process is alive, and as stopped once the stop is clean.
    const bool running_while_up = manager->is_local_server_running("probe");

    const std::vector<uint8_t> file_payload = {'o', 'k'};
    std::string file_error;
    std::vector<uint8_t> roundtrip;
    const bool safe_upload = manager->upload_server_file("probe", "safe.txt",
                                                        file_payload, &file_error);
    const bool traversal_upload = manager->upload_server_file("probe", "../escaped.txt",
                                                              file_payload, &file_error);
    const bool safe_download = manager->download_server_file("probe", "safe.txt",
                                                            &roundtrip, &file_error);
    const bool safe_delete = manager->delete_server_file("probe", "safe.txt", &file_error);
    const std::filesystem::path escaped = std::filesystem::path(root) / L"escaped.txt";
    if (!safe_upload || traversal_upload || !safe_download || roundtrip != file_payload ||
        !safe_delete || std::filesystem::exists(escaped)) {
        manager->stop_local_server("probe", nullptr);
        SetEnvironmentVariableW(L"AMALGAM_SERVER_ROOT", nullptr);
        std::printf("server file boundary verification failed: upload=%s traversal=%s download=%s delete=%s\n",
                    safe_upload ? "yes" : "no", traversal_upload ? "accepted" : "rejected",
                    safe_download ? "yes" : "no", safe_delete ? "yes" : "no");
        return 3;
    }

    std::string response;
    if (!manager->send_command("probe", "status", &response, &error)) {
        manager->stop_local_server("probe", nullptr);
        SetEnvironmentVariableW(L"AMALGAM_SERVER_ROOT", nullptr);
        std::printf("server transport command failed: %s\n", error.c_str());
        return 4;
    }

    bool observed = false;
    for (int attempt = 0; attempt < 100 && !observed; ++attempt) {
        const auto logs = manager->get_console_logs("probe", 100, nullptr);
        for (const auto& entry : logs) {
            if (entry.message.find("ECHO:status") != std::string::npos) {
                observed = true;
                break;
            }
        }
        if (!observed) Sleep(20);
    }
    const bool stopped = manager->stop_local_server("probe", &error);
    const bool running_after_stop = manager->is_local_server_running("probe");
    SetEnvironmentVariableW(L"AMALGAM_SERVER_ROOT", nullptr);
    if (!observed || !stopped || !running_while_up || running_after_stop) {
        std::printf("server transport verification failed: observed=%s stopped=%s up=%s after_stop=%s %s\n",
                    observed ? "yes" : "no", stopped ? "yes" : "no",
                    running_while_up ? "yes" : "no", running_after_stop ? "yes" : "no",
                    error.c_str());
        return 5;
    }
    std::printf("server transport: READY (files, path boundary, stdin, stdout, graceful stop)\n");
    std::printf("server run state: running_while_up=1 running_after_clean_stop=0\n");
    return 0;
}

int cli_client_bridge_probe(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::printf("usage: --client-bridge-probe <temporary-root>\n");
        return 1;
    }
    const std::filesystem::path supplied_root(arg_at(argc, argv, 2));
    if (supplied_root.empty()) return 1;
    const std::filesystem::path probe_root = supplied_root /
        (L"amalgam-client-bridge-probe-" + std::to_wstring(GetCurrentProcessId()));
    const std::filesystem::path profile_root = probe_root / L"profile";
    std::error_code filesystem_error;
    std::filesystem::create_directories(profile_root / L"mods", filesystem_error);
    if (filesystem_error) {
        std::printf("client bridge setup failed: %s\n", filesystem_error.message().c_str());
        return 2;
    }

    aml::config::Config config;
    config.base_dir = probe_root.wstring();
    aml::config::Server server;
    server.name = "Integration Server";
    server.address = L"127.0.0.1:25565";
    config.servers.push_back(server);

    aml::instances::Instance profile;
    profile.id = "bridge-probe";
    profile.name = "Bridge Probe";
    profile.minecraft_version = "1.21.1";
    profile.loader = "fabric";
    profile.loader_version = "0.16.14";
    profile.directory = profile_root.wstring();

    std::string error;
    const bool prepared = aml::ui::prepare_client_bridge_for_profile(config, profile, &error);
    const std::filesystem::path bridge_root = profile_root / L".amalgam" / L"client";
    auto read_text = [](const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    };
    const std::string profile_text = read_text(bridge_root / L"shared_profile.txt");
    const std::string server_text = read_text(bridge_root / L"shared_servers.txt");
    const auto& online = aml::online::config();
    const std::string network_row = online.network_name + "|" + online.network_address;
    const bool verified = prepared &&
        profile_text.find("profile_id=bridge-probe") != std::string::npos &&
        profile_text.find("minecraft_version=1.21.1") != std::string::npos &&
        profile_text.find("loader=fabric") != std::string::npos &&
        server_text.find("Integration Server|127.0.0.1:25565") != std::string::npos &&
        server_text.find(network_row) != std::string::npos &&
        std::filesystem::exists(bridge_root / L"shared_friends.txt") &&
        std::filesystem::exists(bridge_root / L"shared_cosmetics.txt");

    std::filesystem::remove_all(probe_root, filesystem_error);
    if (!verified) {
        std::printf("client bridge verification failed: %s\n", error.c_str());
        return 3;
    }
    std::printf("client bridge: READY (profile, mods, friends, servers, cosmetics)\n");
    return 0;
}

int cli_mods_search(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::printf("usage: --mods-search <query> [loader] [game_version] [facet]\n");
        return 1;
    }
    std::string query = aml::net::to_utf8(arg_at(argc, argv, 2));
    std::string loader = argc >= 4 ? aml::net::to_utf8(arg_at(argc, argv, 3)) : std::string();
    std::string gv = argc >= 5 ? aml::net::to_utf8(arg_at(argc, argv, 4)) : std::string();
    aml::mods::Facet facet = static_cast<aml::mods::Facet>(0);
    if (argc >= 6) {
        std::string f = aml::net::to_utf8(arg_at(argc, argv, 5));
        if (f == "shader") facet = aml::mods::Facet::Shader;
        else if (f == "pack") facet = aml::mods::Facet::Modpack;
        else if (f == "resourcepack") facet = aml::mods::Facet::ResourcePack;
        else if (f == "datapack") facet = aml::mods::Facet::Datapack;
    }
    std::string err;
    std::vector<aml::mods::SearchResult> results;
    if (!aml::mods::search(cli_api_cfg(), query, loader, gv, facet, results, &err)) {
        std::printf("search failed: %s\n", err.c_str());
        return 1;
    }
    for (const auto& r : results) {
        std::printf("%s\t%s\t%s\t%s\t%lld dl\t%s\n", r.source.c_str(), r.slug.c_str(),
                    r.title.c_str(), r.description.c_str(), static_cast<long long>(r.downloads),
                    r.icon_url.c_str());
    }
    std::printf("total: %d\n", static_cast<int>(results.size()));
    return 0;
}

int cli_mods_install(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::printf("usage: --mods-install <slug> [source] [loader] [game_version] [mods_dir]\n");
        return 1;
    }
    std::string slug = aml::net::to_utf8(arg_at(argc, argv, 2));
    // Keep the original Modrinth-first argument shape working, while allowing
    // a caller to select the same CurseForge path exposed by the UI.
    int next_arg = 3;
    std::string source = "modrinth";
    if (argc > next_arg) {
        std::string requested_source = lowercase(aml::net::to_utf8(arg_at(argc, argv, next_arg)));
        if (requested_source == "modrinth" || requested_source == "curseforge") {
            source = requested_source;
            ++next_arg;
        }
    }
    std::string loader = argc > next_arg
        ? aml::net::to_utf8(arg_at(argc, argv, next_arg++)) : std::string();
    std::string gv = argc > next_arg
        ? aml::net::to_utf8(arg_at(argc, argv, next_arg++)) : std::string();
    std::string dir = argc > next_arg
        ? aml::net::to_utf8(arg_at(argc, argv, next_arg)) : "mods";
    std::vector<std::string> lines;
    std::string err;
    if (!aml::mods::install_mod(cli_api_cfg(), slug, source, loader, gv,
                                aml::net::to_wide(dir), lines, &err)) {
        std::printf("install failed: %s\n", err.c_str());
        return 1;
    }
    for (const auto& l : lines) std::printf("%s\n", l.c_str());
    return 0;
}

// Full modpack install path — same code path as the GUI (do_modpack_install).
// usage: --pack-install <slug> [source] [loader] [game_version] [instances_dir]
int cli_pack_install(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::printf("usage: --pack-install <slug> [source] [loader] [game_version] [instances_dir]\n");
        return 1;
    }
    aml::mods::SearchResult project;
    project.slug = aml::net::to_utf8(arg_at(argc, argv, 2));
    project.source = argc > 3 ? aml::net::to_utf8(arg_at(argc, argv, 3)) : "modrinth";
    std::string preferred_loader = argc > 4 ? aml::net::to_utf8(arg_at(argc, argv, 4)) : std::string();
    std::string preferred_version = argc > 5 ? aml::net::to_utf8(arg_at(argc, argv, 5)) : std::string();
    std::wstring instances_dir = argc > 6
        ? aml::net::to_wide(aml::net::to_utf8(arg_at(argc, argv, 6)))
        : aml::net::get_local_app_data_path() + L"\\instances";

    std::string err;
    aml::mods::ApiCfg cfg = cli_api_cfg();
    aml::mods::ModInfo info;
    std::printf("[1/6] fetching project metadata...\n");
    if (!aml::mods::project_files(cfg, project.slug, project.source, info, &err)) {
        std::printf("FAIL metadata: %s\n", err.c_str());
        return 1;
    }
    std::printf("      title=%s files=%d\n", info.title.c_str(), static_cast<int>(info.files.size()));

    std::printf("[2/6] selecting compatible release...\n");
    aml::mods::CompatibleRelease release;
    if (!aml::mods::select_compatible_release(info, preferred_loader, preferred_version,
                                              release, &err)) {
        std::printf("FAIL release: %s\n", err.c_str());
        return 1;
    }
    std::printf("      game=%s loader=%s file=%s\n", release.game_version.c_str(),
                release.loader.c_str(), release.file_id.c_str());

    const aml::mods::FileInfo* selected = nullptr;
    for (const auto& f : info.files) if (f.id == release.file_id) { selected = &f; break; }
    aml::mods::FileInfo selected_file;
    if (selected) selected_file = *selected;
    std::printf("[3/6] resolving download url...\n");
    if (!selected || !aml::mods::resolve_download_url(cfg, project.slug, project.source,
                                                      selected_file, &err)) {
        std::printf("FAIL url: %s\n", err.c_str());
        return 1;
    }
    std::printf("      url=%s\n", selected_file.url.substr(0, 100).c_str());

    const std::wstring downloads_dir = aml::net::get_local_app_data_path() + L"\\downloads";
    aml::net::mkdirs(downloads_dir);
    std::wstring archive_path = downloads_dir + L"\\.cli-pack-test.archive";
    std::printf("[4/6] downloading archive...\n");
    if (!aml::net::download(aml::net::to_wide(selected_file.url), archive_path,
                            [&](uint64_t done, uint64_t total) {
                                if (total) std::printf("\r      %lld / %lld (%d%%)",
                                    static_cast<long long>(done), static_cast<long long>(total),
                                    static_cast<int>(done * 100 / total));
                                return true;
                            }, &err,
                            selected_file.sha1, selected_file.size > 0 ? selected_file.size : -1)) {
        std::printf("\nFAIL download: %s\n", err.c_str());
        return 1;
    }
    std::printf("\n      ok (%lld bytes)\n", static_cast<long long>(aml::net::file_size(archive_path)));

    std::printf("[5/6] importing pack...\n");
    aml::instances::Instance imported;
    bool ok = aml::import_pack::archive(archive_path, instances_dir, cfg, imported,
                                        [&](float p, const std::string& label) {
                                            std::printf("\r      %d%% %s        ",
                                                static_cast<int>(p * 100), label.c_str());
                                            return true;
                                        }, &err);
    DeleteFileW(archive_path.c_str());
    if (!ok) {
        std::printf("\nFAIL import: %s\n", err.c_str());
        return 1;
    }
    std::printf("\n      profile=%s\n", aml::net::to_utf8(imported.directory).c_str());

    std::printf("[6/6] saving metadata...\n");
    imported.pack_source = project.source;
    imported.pack_project = project.slug;
    if (imported.minecraft_version.empty()) imported.minecraft_version = release.game_version;
    if (imported.loader.empty() || imported.loader == "auto") imported.loader = release.loader;
    if (!aml::instances::save(imported, &err)) {
        std::printf("FAIL save: %s\n", err.c_str());
        return 1;
    }
    std::printf("OK  profile ready: %s (%s / %s)\n", imported.name.c_str(),
                imported.minecraft_version.c_str(), imported.loader.c_str());
    return 0;
}

int cli_ai_chat(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::printf("usage: --ai-chat <message> [model]\n");
        return 1;
    }
    std::string msg = aml::net::to_utf8(arg_at(argc, argv, 2));
    aml::ai::ChatRequest req;
    req.messages.push_back({"user", msg});
    std::string reply, err;
    if (!aml::ai::chat(req, reply, &err)) {
        std::printf("ai chat failed: %s\n", err.c_str());
        return 1;
    }
    std::printf("%s\n", reply.c_str());
    return 0;
}

int cli_ai_image(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::printf("usage: --ai-image <prompt> [out.png]\n");
        return 1;
    }
    std::string prompt = aml::net::to_utf8(arg_at(argc, argv, 2));
    std::string out = argc >= 4 ? aml::net::to_utf8(arg_at(argc, argv, 3)) : "ai-image.png";
    aml::ai::ImageRequest req;
    req.prompt = prompt;
    std::vector<uint8_t> png;
    std::string err;
    if (!aml::ai::image(req, png, &err)) {
        std::printf("ai image failed: %s\n", err.c_str());
        return 1;
    }
    FILE* f = nullptr;
    if (_wfopen_s(&f, aml::net::to_wide(out).c_str(), L"wb") != 0 || !f) {
        std::printf("cannot write %s\n", out.c_str());
        return 1;
    }
    fwrite(png.data(), 1, png.size(), f);
    fclose(f);
    std::printf("wrote %s (%zu bytes)\n", out.c_str(), png.size());
    return 0;
}

int cli_bedrock(int argc, wchar_t** argv) {
    // --bedrock-info | --bedrock-launch | --bedrock-install <file>
    if (has_arg(argc, argv, L"--bedrock-info")) {
        std::string err;
        std::wstring dir = aml::bedrock::detect(&err);
        if (dir.empty()) {
            std::printf("bedrock: %s\n", err.c_str());
            return 1;
        }
        std::printf("bedrock installed, data dir: %S\n", dir.c_str());
        return 0;
    }
    if (has_arg(argc, argv, L"--bedrock-launch")) {
        std::string err;
        if (!aml::bedrock::launch(&err)) {
            std::printf("bedrock: %s\n", err.c_str());
            return 1;
        }
        std::printf("bedrock launching\n");
        return 0;
    }
    if (has_arg(argc, argv, L"--bedrock-install")) {
        if (argc < 3) {
            std::printf("usage: --bedrock-install <addon.mcpack>\n");
            return 1;
        }
        std::wstring file = arg_at(argc, argv, 2);
        std::string err;
        if (!aml::bedrock::install_addon(file, &err)) {
            std::printf("bedrock: %s\n", err.c_str());
            return 1;
        }
        std::printf("bedrock addon queued for import: %S\n", file.c_str());
        return 0;
    }
    std::printf("usage: --bedrock-info | --bedrock-launch | --bedrock-install <file>\n");
    return 1;
}

int cli_ai_vision(int argc, wchar_t** argv) {
    // --ai-vision <image> [model]
    if (argc < 3) {
        std::printf("usage: --ai-vision <image> [model]\n");
        return 1;
    }
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring p = self;
    size_t slash = p.find_last_of(L"\\/");
    std::wstring dir = slash == std::wstring::npos ? L"." : p.substr(0, slash);
    std::wstring img = arg_at(argc, argv, 2);
    FILE* f = nullptr;
    if (_wfopen_s(&f, img.c_str(), L"rb") != 0 || !f) {
        std::printf("cannot open %S\n", img.c_str());
        return 1;
    }
    std::vector<uint8_t> data;
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        data.insert(data.end(), buf, buf + n);
    }
    fclose(f);
    if (data.empty() || data.size() > 64ull * 1024 * 1024) {
        std::printf("image is empty or too large\n");
        return 1;
    }
    aml::ai::VisionRequest req;
    req.prompt = "describe this image in one short phrase";
    aml::ai::VisionItem item;
    std::wstring ext = img.substr(img.find_last_of(L'.') == std::wstring::npos
                                      ? img.size() : img.find_last_of(L'.'));
    if (_wcsicmp(ext.c_str(), L".jpg") == 0 || _wcsicmp(ext.c_str(), L".jpeg") == 0)
        item.mime = "image/jpeg";
    else if (_wcsicmp(ext.c_str(), L".webp") == 0)
        item.mime = "image/webp";
    else if (_wcsicmp(ext.c_str(), L".gif") == 0)
        item.mime = "image/gif";
    else if (_wcsicmp(ext.c_str(), L".bmp") == 0)
        item.mime = "image/bmp";
    else
        item.mime = "image/png";
    item.data = data;
    req.items.push_back(item);
    std::string reply, err;
    if (!aml::ai::vision(req, reply, &err)) {
        std::printf("ai vision failed: %s\n", err.c_str());
        return 1;
    }
    std::printf("%s\n", reply.c_str());
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE prev, PWSTR cmdline, int show) {
    aml::crash::install();
    aml::updater::init_from_embedded_key();
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (has_arg(argc, argv, L"--help") || has_arg(argc, argv, L"-h") ||
        has_arg(argc, argv, L"-?")) {
        attach_cli_output();
        print_cli_help();
        std::fflush(stdout);
        if (argv) LocalFree(argv);
        return 0;
    }
    if (has_arg(argc, argv, L"--version")) {
        attach_cli_output();
        std::printf("%s\n", aml::ui::launcher_version());
        std::fflush(stdout);
        if (argv) LocalFree(argv);
        return 0;
    }
    if (is_cli_invocation(argc, argv)) {
        attach_cli_output();
        int result = 1;
        if (has_arg(argc, argv, L"--versions")) result = cli_versions(argc, argv);
        else if (has_arg(argc, argv, L"--check-official-launcher")) result = cli_official_launcher();
        else if (has_arg(argc, argv, L"--official-handoff-probe")) result = cli_official_handoff_probe(argc, argv);
        else if (has_arg(argc, argv, L"--java-install")) result = cli_java_install(argc, argv);
        else if (has_arg(argc, argv, L"--check-java")) result = cli_java();
        else if (has_arg(argc, argv, L"--check-prereqs")) result = cli_prereqs();
        else if (has_arg(argc, argv, L"--login")) result = cli_login();
        else if (has_arg(argc, argv, L"--logout")) result = cli_logout();
        else if (has_arg(argc, argv, L"--doctor")) result = cli_doctor(argc, argv);
        else if (has_arg(argc, argv, L"--ui-snapshot")) result = cli_ui_snapshot(argc, argv);
        else if (has_arg(argc, argv, L"--server-transport-probe"))
            result = cli_server_transport_probe(argc, argv);
        else if (has_arg(argc, argv, L"--client-bridge-probe"))
            result = cli_client_bridge_probe(argc, argv);
        else if (has_arg(argc, argv, L"--inspect")) result = cli_inspect(argc, argv);
        else if (has_arg(argc, argv, L"--check-update")) result = cli_check_update(argc, argv);
        else if (has_arg(argc, argv, L"--launch")) result = cli_launch(argc, argv);
        else if (has_arg(argc, argv, L"--mods-search")) result = cli_mods_search(argc, argv);
        else if (has_arg(argc, argv, L"--mods-install")) result = cli_mods_install(argc, argv);
        else if (has_arg(argc, argv, L"--pack-install")) result = cli_pack_install(argc, argv);
        else if (has_arg(argc, argv, L"--ai-chat")) result = cli_ai_chat(argc, argv);
        else if (has_arg(argc, argv, L"--ai-image")) result = cli_ai_image(argc, argv);
        else if (has_arg(argc, argv, L"--ai-vision")) result = cli_ai_vision(argc, argv);
        else result = cli_bedrock(argc, argv);
        std::fflush(stdout);
        std::fflush(stderr);
        if (argv) LocalFree(argv);
        return result;
    }
    // Read windowed flags before releasing CommandLineToArgvW's storage.
    // The previous order left --safe-mode reading freed memory.
    const bool safe_mode = has_arg(argc, argv, L"--safe-mode");
    const std::string initial_page = lowercase(
        aml::net::to_utf8(value_after_arg(argc, argv, L"--page")));
    // An unrecognized flag almost always means a user typed the launcher into
    // a terminal expecting --help/--version style output (or misspelled a
    // command). Without console output the process appears to hang silently.
    // The full command list lives in is_cli_invocation(); attaching to the
    // caller's terminal here is harmless for double-click launches (there is
    // no parent console to attach to). Must run before argv is freed.
    if (argc > 1 && argv[1][0] == L'-' &&
        !has_arg(argc, argv, L"--page") &&
        !has_arg(argc, argv, L"--safe-mode")) {
        attach_cli_output();
        std::wprintf(
            L"Amalgam Launcher: unknown option '%s'.\n"
            L"This command starts the graphical launcher. Run it without "
            L"arguments for the GUI, or use a documented CLI command such as "
            L"--versions, --check-prereqs, --doctor, --launch, --mods-search.\n"
            L"Run --help for the full command list.\n",
            argv[1]);
        std::fflush(stdout);
    }
    if (argv) LocalFree(argv);
    (void)instance;
    (void)prev;
    (void)cmdline;
    (void)show;

    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring path = self;
    size_t slash = path.find_last_of(L"\\/");
    std::wstring exe_dir =
        slash == std::wstring::npos ? L"." : path.substr(0, slash);

    aml::config::Config cfg;
    if (!load_launcher_config(exe_dir, cfg, true)) return 1;

    aml::crash::uninstall();
    aml::ui::RunOptions options;
    options.safe_mode = safe_mode;
    options.initial_page = initial_page;
    return aml::ui::run_window(&cfg, options) ? 0 : 1;
}
