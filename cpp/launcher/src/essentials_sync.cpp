#include "essentials_sync.h"
#include "essentials_session.h"

#include "instances.h"
#include "supabase.h"
#include "net.h"
#include "json.h"
#include "config.h"
#include "launch.h"
#include "mods.h"
#include "provider_config.h"

#include <algorithm>
#include <filesystem>
#include <thread>
#include <chrono>
#include <sstream>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

namespace fs = std::filesystem;

namespace aml::essentials {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

bool local_tcp_open(uint16_t port) {
    SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == INVALID_SOCKET) return false;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    const bool open = connect(socket, reinterpret_cast<const sockaddr*>(&address),
                              sizeof(address)) == 0;
    closesocket(socket);
    return open;
}

bool start_server_jar(const std::wstring& directory, const std::string& java_path,
                      int memory_mb, uintptr_t& process_handle, std::string* error) {
    const std::wstring java = java_path.empty() ? L"java" : net::to_wide(java_path);
    std::wstring command = java + L" -Xmx" + std::to_wstring(std::max(512, memory_mb)) +
                           L"M -jar server.jar nogui";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, nullptr,
                        directory.c_str(), &startup, &process)) {
        if (error) *error = "Could not start server.jar (Win32 error " +
                            std::to_string(GetLastError()) + ")";
        return false;
    }
    CloseHandle(process.hThread);
    process_handle = reinterpret_cast<uintptr_t>(process.hProcess);
    return true;
}

void stop_server_process(uintptr_t& process_handle) {
    if (!process_handle) return;
    HANDLE process = reinterpret_cast<HANDLE>(process_handle);
    TerminateProcess(process, 0);
    WaitForSingleObject(process, 5000);
    CloseHandle(process);
    process_handle = 0;
}

std::wstring inst_dir_parent_w(const instances::Instance& inst) {
    fs::path p(inst.directory);
    return p.parent_path().wstring();
}

}  // namespace

// ---------------------------------------------------------------------------
// CompatCheck::summary
// ---------------------------------------------------------------------------

std::string CompatCheck::summary() const {
    std::ostringstream ss;
    ss << "Overall: " << compat_level_name(overall);
    ss << " | Version: " << compat_level_name(version_level);
    ss << " | Loader: " << compat_level_name(loader_level);
    ss << " | Mods: " << compat_level_name(mods_level);
    if (!missing_mods.empty()) ss << " | Missing: " << missing_mods.size();
    if (!outdated_mods.empty()) ss << " | Outdated: " << outdated_mods.size();
    if (!extra_mods.empty()) ss << " | Extra: " << extra_mods.size();
    return ss.str();
}

// ---------------------------------------------------------------------------
// ProfileSync
// ---------------------------------------------------------------------------

ProfileSync& ProfileSync::instance() {
    static ProfileSync s;
    return s;
}

CompatCheck ProfileSync::check_compatibility(const instances::Instance& local,
                                             const EssentialsManifest& host) {
    CompatCheck c;
    c.local_minecraft_version = local.minecraft_version;
    c.host_minecraft_version = host.minecraft_version;
    c.local_loader = local.loader;
    c.host_loader = host.loader;
    c.local_loader_version = local.loader_version;
    c.host_loader_version = host.loader_version;

    // Version comparison
    if (local.minecraft_version == host.minecraft_version) {
        c.version_level = CompatibilityLevel::Match;
    } else {
        // Different major = Incompatible, different minor = MajorMismatch
        auto dot1 = local.minecraft_version.find('.');
        auto dot2 = host.minecraft_version.find('.');
        std::string major1 = dot1 != std::string::npos ? local.minecraft_version.substr(0, dot1) : local.minecraft_version;
        std::string major2 = dot2 != std::string::npos ? host.minecraft_version.substr(0, dot2) : host.minecraft_version;
        c.version_level = (major1 == major2) ? CompatibilityLevel::MajorMismatch : CompatibilityLevel::Incompatible;
    }

    // Loader comparison
    if (local.loader == host.loader) {
        c.loader_level = (local.loader_version == host.loader_version)
                            ? CompatibilityLevel::Match
                            : CompatibilityLevel::MinorMismatch;
    } else {
        c.loader_level = CompatibilityLevel::MajorMismatch;
    }

    // Mod comparison
    auto local_mods = scan_local_mods(local);
    std::map<std::string, ModEntry> local_by_name;
    for (auto& m : local_mods) local_by_name[m.name] = m;

    for (const auto& host_mod : host.mods) {
        auto it = local_by_name.find(host_mod.name);
        if (it == local_by_name.end()) {
            c.missing_mods.push_back(host_mod);
        } else if (it->second.version != host_mod.version) {
            c.outdated_mods.push_back(host_mod);
        }
    }
    for (auto& [name, m] : local_by_name) {
        bool found = false;
        for (const auto& hm : host.mods) {
            if (hm.name == name) { found = true; break; }
        }
        if (!found) c.extra_mods.push_back(m);
    }

    // Mods level
    if (c.missing_mods.empty() && c.outdated_mods.empty()) {
        c.mods_level = c.extra_mods.empty() ? CompatibilityLevel::Match : CompatibilityLevel::MinorMismatch;
    } else {
        c.mods_level = c.missing_mods.empty() ? CompatibilityLevel::MinorMismatch : CompatibilityLevel::MajorMismatch;
    }

    // Overall
    auto worst = [](CompatibilityLevel a, CompatibilityLevel b) {
        return static_cast<int>(a) > static_cast<int>(b) ? a : b;
    };
    c.overall = worst(worst(c.version_level, c.loader_level), c.mods_level);
    return c;
}

SyncPlan ProfileSync::build_sync_plan(const instances::Instance& local,
                                      const EssentialsManifest& host,
                                      SyncMode mode) {
    SyncPlan plan;
    plan.mode = mode;
    plan.source_profile_id = local.id;
    plan.target_profile_name = local.name + " (" + host.profile_name + ")";

    const auto local_mods = scan_local_mods(local);
    std::map<std::string, ModEntry> local_by_name;
    for (const auto& mod : local_mods) local_by_name[mod.name] = mod;
    for (const auto& m : host.mods) {
        auto it = local_by_name.find(m.name);
        if (it == local_by_name.end()) {
            plan.mods_to_download.push_back(m);
        } else if (it->second.version != m.version || it->second.hash != m.hash) {
            plan.mods_to_update.push_back(m);
        }
    }
    plan.total_download_size = 0; // Provider artifacts are not downloaded by Essentials.
    return plan;
}

bool ProfileSync::execute_sync(const SyncPlan& plan,
                               const std::function<void(float, const std::string&)>& on_progress) {
    if (plan.mods_to_download.empty() && plan.mods_to_update.empty()) {
        if (on_progress) on_progress(1.0f, "Nothing to sync");
        return true;
    }

    // Combine download + update lists; both need installation.
    std::vector<ModEntry> all_needed;
    all_needed.insert(all_needed.end(), plan.mods_to_download.begin(), plan.mods_to_download.end());
    all_needed.insert(all_needed.end(), plan.mods_to_update.begin(), plan.mods_to_update.end());

    // Resolve the mods directory from the source profile.
    std::wstring mods_dir;
    if (!plan.source_profile_id.empty()) {
        std::wstring instances_dir = net::get_local_app_data_path() + L"\\Amalgam\\Instances";
        std::error_code ec;
        for (auto& entry : std::filesystem::directory_iterator(instances_dir, ec)) {
            if (ec || !entry.is_directory()) continue;
            instances::Instance inst;
            if (instances::load(entry.path().wstring(), inst, nullptr) && inst.id == plan.source_profile_id) {
                mods_dir = entry.path().wstring() + L"\\mods";
                break;
            }
        }
    }
    if (mods_dir.empty()) {
        if (on_progress) on_progress(0.0f, "Could not locate instance mods directory");
        return false;
    }
    net::mkdirs(mods_dir);

    // Load API config for Modrinth/CurseForge downloads.
    config::Config cfg;
    config::load(net::get_local_app_data_path() + L"\\Amalgam\\launcher.json", cfg);
    mods::ApiCfg api = provider_config::make(cfg);

    bool all_ok = true;
    int done = 0;
    const int total = static_cast<int>(all_needed.size());
    for (const auto& mod : all_needed) {
        if (on_progress) {
            float pct = total > 0 ? static_cast<float>(done) / total : 0.0f;
            on_progress(pct, "Downloading " + mod.name + " " + mod.version);
        }

        std::string source = mod.source.empty() ? "modrinth" : mod.source;
        std::vector<std::string> log_lines;
        std::string err;
        bool ok = mods::install_mod(api, mod.name, source, "", "", mods_dir,
                                    log_lines, &err);
        if (!ok) {
            all_ok = false;
            if (on_progress) {
                on_progress(static_cast<float>(done) / std::max(total, 1),
                    "Failed to download " + mod.name + ": " + err);
            }
        }
        ++done;
    }

    if (on_progress) {
        on_progress(1.0f, all_ok
            ? "Sync complete — " + std::to_string(total) + " mod(s) ready"
            : "Sync partially failed — " + std::to_string(total) + " mod(s) needed");
    }
    return all_ok;
}

std::string ProfileSync::hash_file(const std::wstring& path) {
    return net::sha1_file(path);
}

std::vector<ModEntry> ProfileSync::scan_local_mods(const instances::Instance& local) {
    std::vector<ModEntry> mods;
    auto content = instances::list_content(local, nullptr);
    for (const auto& entry : content) {
        if (entry.type == instances::ContentType::Mod) {
            ModEntry m;
            m.name = entry.filename;
            m.hash = entry.sha1;
            m.enabled = entry.enabled;
            m.source = entry.owner_source;
            m.id = entry.owner_project;
            // Extract version from filename if possible (e.g. "mod-1.2.3.jar")
            auto fn = entry.filename;
            auto dash = fn.rfind('-');
            auto dot = fn.rfind('.');
            if (dash != std::string::npos && dot != std::string::npos && dash < dot) {
                m.version = fn.substr(dash + 1, dot - dash - 1);
            }
            mods.push_back(std::move(m));
        }
    }
    return mods;
}

std::string ProfileSync::create_synced_profile(const instances::Instance& original,
                                               const SyncPlan& plan,
                                               std::string* err) {
    std::wstring parent = inst_dir_parent_w(original);
    instances::Instance synced;
    if (!instances::duplicate(original, parent, plan.target_profile_name, synced, err)) {
        return {};
    }
    return synced.id;
}

bool ProfileSync::cleanup_temporary_profile(const std::string& profile_id) {
    // We need the instance to remove it. Scan for it.
    auto base = net::get_local_app_data_path() + L"\\instances";
    auto all = instances::scan(base, nullptr);
    for (auto& inst : all) {
        if (inst.id == profile_id) {
            std::string err;
            return instances::remove(inst, &err);
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// JoinManager
// ---------------------------------------------------------------------------

JoinManager& JoinManager::instance() {
    static JoinManager s;
    return s;
}

JoinResult JoinManager::analyze_and_prepare(const std::string& session_id,
                                             const std::string& join_token) {
    JoinResult r;
    r.session_id = session_id;

    auto& supa = supabase::SupabaseManager::instance();
    if (!supa.is_authenticated()) {
        r.error = "Sign in before joining a multiplayer session";
        return r;
    }

    auto& sessions = SessionManager::instance();
    if (!sessions.join_session(session_id, join_token)) {
        r.error = "The session token is invalid, expired, or the session is unavailable";
        return r;
    }

    EssentialsManifest manifest;
    if (!sessions.fetch_manifest(session_id, join_token, manifest)) {
        r.error = "The host profile manifest could not be retrieved";
        return r;
    }

    // Get local profiles to find a compatible one
    auto base = net::get_local_app_data_path() + L"\\instances";
    auto locals = instances::scan(base, nullptr);

    const instances::Instance* best = nullptr;
    for (auto& inst : locals) {
        if (inst.minecraft_version == manifest.minecraft_version &&
            inst.loader == manifest.loader) {
            best = &inst;
            break;
        }
    }
    if (locals.empty()) {
        r.error = "No local profiles found";
        return r;
    }
    if (!best) best = &locals.front();

    auto& ps = ProfileSync::instance();
    compat_ = ps.check_compatibility(*best, manifest);
    if (compat_.version_level == CompatibilityLevel::Incompatible ||
        compat_.loader_level == CompatibilityLevel::MajorMismatch) {
        r.error = "This Minecraft profile is incompatible with the host";
        return r;
    }
    sync_plan_ = ps.build_sync_plan(*best, manifest, SyncMode::TemporaryProfile);

    host_user_id_ = manifest.host_user_id;
    source_profile_id_ = best->id;
    r.success = true;
    r.connection_type = ConnectionType::None;
    session_id_ = session_id;
    join_token_ = join_token;
    return r;
}

SyncPlan JoinManager::get_current_sync_plan() const { return sync_plan_; }
CompatCheck JoinManager::get_current_compat() const { return compat_; }

bool JoinManager::execute_join(const std::function<void(float, const std::string&)>& on_progress) {
    if (joining_) return false;
    joining_ = true;
    progress_ = 0.0f;
    status_text_ = "Preparing join...";

    std::thread([this, on_progress]() {
        auto& ps = ProfileSync::instance();
        const SyncPlan plan = sync_plan_;
        const std::string source_profile_id = source_profile_id_;
        const std::string session_id = session_id_;
        const std::string host_user_id = host_user_id_;
        if (plan.mode == SyncMode::TemporaryProfile &&
            (!plan.mods_to_download.empty() || !plan.mods_to_update.empty())) {
            status_text_ = "Synchronizing host profile...";
            progress_ = 0.3f;
            if (on_progress) on_progress(progress_, status_text_);
            if (!ps.execute_sync(plan, on_progress)) {
                status_text_ = "Profile synchronization failed";
                joining_ = false;
                return;
            }
        }
        if (!joining_) { status_text_ = "Cancelled"; return; }

        // Create the temporary profile from the profile selected during analysis.
        std::string target_id = source_profile_id;
        if (plan.mode == SyncMode::TemporaryProfile) {
            status_text_ = "Creating synced profile...";
            progress_ = 0.6f;
            if (on_progress) on_progress(progress_, status_text_);
            auto base = net::get_local_app_data_path() + L"\\instances";
            auto locals = instances::scan(base, nullptr);
            auto source = std::find_if(locals.begin(), locals.end(),
                [&source_profile_id](const instances::Instance& inst) {
                    return inst.id == source_profile_id;
                });
            if (source == locals.end()) {
                status_text_ = "The selected Minecraft profile disappeared";
                joining_ = false;
                return;
            }
            std::string err;
            target_id = ps.create_synced_profile(*source, plan, &err);
            if (target_id.empty()) {
                status_text_ = "Failed to create profile: " + err;
                joining_ = false;
                return;
            }
        }
        if (!joining_) { status_text_ = "Cancelled"; return; }

        // Step 3: Establish the local TCP adapter before launching Minecraft.
        status_text_ = "Setting up relay...";
        progress_ = 0.7f;
        if (on_progress) on_progress(progress_, status_text_);
        bridge_ = std::make_unique<TcpDataChannelBridge>();
        if (!bridge_->start_guest(session_id, host_user_id)) {
            status_text_ = "Relay setup failed: " + bridge_->error();
            joining_ = false;
            return;
        }
        if (!joining_) { bridge_->stop(); bridge_.reset(); status_text_ = "Cancelled"; return; }

        status_text_ = "Launching Minecraft...";
        progress_ = 0.8f;
        if (on_progress) on_progress(progress_, status_text_);

        auto base = net::get_local_app_data_path() + L"\\instances";
        auto locals = instances::scan(base, nullptr);
        bool launched = false;
        for (auto& inst : locals) {
            if (inst.id == target_id) {
                launch::Options opt;
                opt.mc_id = inst.minecraft_version;
                opt.loader = inst.loader;
                opt.instance_dir = inst.directory;
                opt.test_server = net::to_wide(
                    "127.0.0.1:" + std::to_string(bridge_->local_port()));
                opt.wait_for_exit = true;

                std::wstring dll_path = L"amalgam.dll";
                opt.dll_path = dll_path;

                std::string launch_err;
                launch::Result result;
                auto log_fn = [](const std::wstring&) {};
                if (!launch::run(opt, log_fn, &result, &launch_err)) {
                    status_text_ = "Launch failed: " + launch_err;
                    bridge_->stop();
                    bridge_.reset();
                    joining_ = false;
                    return;
                }
                launched = true;
                break;
            }
        }

        if (!launched) {
            status_text_ = "The synchronized Minecraft profile disappeared";
            bridge_->stop();
            bridge_.reset();
            joining_ = false;
            return;
        }

        progress_ = 1.0f;
        status_text_ = "Joined!";
        bridge_->stop();
        bridge_.reset();
        joining_ = false;
    }).detach();

    return true;
}

void JoinManager::cancel_join() {
    joining_ = false;
    status_text_ = "Cancelled";
    progress_ = 0.0f;
}

bool JoinManager::is_joining() const { return joining_; }
float JoinManager::get_progress() const { return progress_; }
std::string JoinManager::get_status_text() const { return status_text_; }

// ---------------------------------------------------------------------------
// WorldHost
// ---------------------------------------------------------------------------

WorldHost& WorldHost::instance() {
    static WorldHost s;
    return s;
}

bool WorldHost::start_hosting(const HostOptions& options,
                              const std::function<void(float, const std::string&)>& on_progress) {
    if (hosting_) return false;
    auto& sessions = SessionManager::instance();
    EssentialsSession session;
    if (!sessions.create_session(options, session)) {
        if (on_progress) on_progress(0.0f, "Could not create the multiplayer session");
        return false;
    }
    if (on_progress) on_progress(0.1f, "Preparing session manifest...");

    // Find the instance to get version info
    auto base = net::get_local_app_data_path() + L"\\instances";
    auto locals = instances::scan(base, nullptr);
    bool found_profile = false;
    std::wstring profile_directory;
    for (auto& inst : locals) {
        if (inst.id == options.profile_id) {
            found_profile = true;
            profile_directory = inst.directory;
            session.minecraft_version = inst.minecraft_version;
            session.loader = inst.loader;
            session.loader_version = inst.loader_version;

            // Build manifest
            auto& ps = ProfileSync::instance();
            session.manifest.session_id = session.id;
            session.manifest.host_user_id = session.host_user_id;
            session.manifest.profile_id = inst.id;
            session.manifest.profile_name = inst.name;
            session.manifest.minecraft_version = inst.minecraft_version;
            session.manifest.loader = inst.loader;
            session.manifest.loader_version = inst.loader_version;
            session.manifest.mods = ps.scan_local_mods(inst);
            break;
        }
    }
    if (!found_profile) {
        sessions.stop_session(session.id);
        if (on_progress) on_progress(0.0f, "The selected host profile no longer exists");
        return false;
    }
    if (!sessions.create_manifest(session.id, session.manifest)) {
        sessions.stop_session(session.id);
        if (on_progress) on_progress(0.0f, "Could not publish the session manifest");
        return false;
    }
    const auto server_jar = fs::path(profile_directory) / "server.jar";
    if (!local_tcp_open(options.server_port)) {
        if (!fs::exists(server_jar)) {
            sessions.stop_session(session.id);
            if (on_progress) on_progress(0.0f,
                "Start a Minecraft server on 127.0.0.1:" + std::to_string(options.server_port) +
                " or place server.jar in the host profile");
            return false;
        }
        std::string start_error;
        if (!start_server_jar(profile_directory, options.java_path, options.memory_mb,
                              server_process_, &start_error)) {
            sessions.stop_session(session.id);
            if (on_progress) on_progress(0.0f, start_error);
            return false;
        }
        for (int i = 0; i < 60 && !local_tcp_open(options.server_port); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (!local_tcp_open(options.server_port)) {
            stop_server_process(server_process_);
            sessions.stop_session(session.id);
            if (on_progress) on_progress(0.0f, "server.jar did not open TCP port 25565");
            return false;
        }
    }
    if (on_progress) on_progress(0.5f, "Starting local TCP adapter...");
    bridge_ = std::make_unique<TcpDataChannelBridge>();
    if (!bridge_->start_host(session.id, options.server_port)) {
        sessions.stop_session(session.id);
        stop_server_process(server_process_);
        bridge_.reset();
        if (on_progress) on_progress(0.0f, "Could not start the host TCP adapter");
        return false;
    }
    if (!sessions.start_session(session.id)) {
        bridge_->stop();
        bridge_.reset();
        stop_server_process(server_process_);
        sessions.stop_session(session.id);
        if (on_progress) on_progress(0.0f, "Could not mark the session online");
        return false;
    }

    session.state = SessionState::Online;
    session.status = "online";
    {
        std::lock_guard<std::mutex> lock(mu_);
        current_session_ = session;
        hosting_ = true;
    }
    notify_state(SessionState::Online);

    if (on_progress) on_progress(1.0f, "Hosting!");
    return true;
}

bool WorldHost::stop_hosting() {
    if (!hosting_) return false;
    notify_state(SessionState::Stopping);
    const std::string session_id = current_session_.id;
    SessionManager::instance().stop_session(session_id);
    if (bridge_) {
        bridge_->stop();
        bridge_.reset();
    }
    stop_server_process(server_process_);
    current_session_.state = SessionState::Ended;
    hosting_ = false;
    notify_state(SessionState::Ended);
    return true;
}

EssentialsSession WorldHost::get_current_session() const {
    std::lock_guard<std::mutex> lock(mu_);
    return current_session_;
}

bool WorldHost::is_hosting() const { return hosting_; }

bool WorldHost::kick_player(const std::string& user_id) {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    if (supabase.is_authenticated() && !current_session_.id.empty()) {
        Json body;
        body.set("session_id", current_session_.id);
        body.set("user_id", user_id);
        auto result = supabase.client()->rpc("kick_player", body);
        if (!result.success) return false;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto& players = current_session_.connected_players;
        players.erase(std::remove(players.begin(), players.end(), user_id), players.end());
        current_session_.player_count = static_cast<int>(players.size()) + 1;
    }
    notify_player(user_id, "", false);
    return true;
}

bool WorldHost::ban_player(const std::string& user_id, const std::string& reason) {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    if (supabase.is_authenticated() && !current_session_.id.empty()) {
        Json body;
        body.set("session_id", current_session_.id);
        body.set("user_id", user_id);
        body.set("reason", reason);
        auto result = supabase.client()->rpc("ban_player", body);
        if (!result.success) return false;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        current_session_.banned_players.push_back(user_id);
    }
    return kick_player(user_id);
}

bool WorldHost::unban_player(const std::string& user_id) {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    if (supabase.is_authenticated() && !current_session_.id.empty()) {
        Json body;
        body.set("session_id", current_session_.id);
        body.set("user_id", user_id);
        auto result = supabase.client()->rpc("unban_player", body);
        if (!result.success) return false;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        current_session_.banned_players.erase(
            std::remove(current_session_.banned_players.begin(),
                        current_session_.banned_players.end(), user_id),
            current_session_.banned_players.end());
    }
    return true;
}

bool WorldHost::update_privacy(SessionPrivacy privacy) {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    if (supabase.is_authenticated() && !current_session_.id.empty()) {
        Json body;
        body.set("session_id", current_session_.id);
        body.set("privacy", static_cast<int>(privacy));
        auto result = supabase.client()->rpc("update_session", body);
        if (!result.success) return false;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        current_session_.privacy = privacy;
    }
    return true;
}

bool WorldHost::update_player_limit(int limit) {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    if (supabase.is_authenticated() && !current_session_.id.empty()) {
        Json body;
        body.set("session_id", current_session_.id);
        body.set("player_limit", limit);
        auto result = supabase.client()->rpc("update_session", body);
        if (!result.success) return false;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        current_session_.player_limit = limit;
    }
    return true;
}

std::vector<WorldEntry> WorldHost::list_worlds(const instances::Instance& profile) {
    std::vector<WorldEntry> worlds;
    fs::path saves = fs::path(profile.directory) / "saves";
    std::error_code ec;
    if (!fs::exists(saves, ec)) return worlds;
    for (auto& entry : fs::directory_iterator(saves, ec)) {
        if (ec || !entry.is_directory()) continue;
        WorldEntry w;
        w.name = entry.path().filename().string();
        w.path = entry.path().string();
        w.last_modified = std::chrono::duration_cast<std::chrono::seconds>(
            fs::last_write_time(entry, ec).time_since_epoch()).count();
        if (!ec) {
            for (auto& f : fs::recursive_directory_iterator(entry.path(), ec)) {
                if (ec) break;
                if (!ec) w.size += f.file_size(ec);
            }
        }
        worlds.push_back(std::move(w));
    }
    return worlds;
}

void WorldHost::on_host_state_changed(HostStateChangedCallback cb) {
    std::lock_guard<std::mutex> lock(mu_);
    state_callbacks_.push_back(std::move(cb));
}

void WorldHost::on_player_changed(PlayerChangedCallback cb) {
    std::lock_guard<std::mutex> lock(mu_);
    player_callbacks_.push_back(std::move(cb));
}

void WorldHost::notify_state(SessionState state) {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& cb : state_callbacks_) cb(state);
}

void WorldHost::notify_player(const std::string& user_id, const std::string& username, bool joined) {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& cb : player_callbacks_) cb(user_id, username, joined);
}

// ---------------------------------------------------------------------------
// ConnectionManager
// ---------------------------------------------------------------------------

ConnectionManager& ConnectionManager::instance() {
    static ConnectionManager s;
    return s;
}

bool ConnectionManager::connect(const std::string& session_id, const std::string& join_token) {
    if (session_id.empty()) return false;

    status_.type = ConnectionType::Relay;
    status_.remote_address.clear();
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& cb : conn_callbacks_) cb(ConnectionType::Relay);
    }

    // Create a guest TcpDataChannelBridge and start it in a background thread.
    bridge_ = std::make_unique<TcpDataChannelBridge>();
    if (!bridge_->start_guest(session_id, join_token)) {
        status_.type = ConnectionType::Failed;
        status_.remote_address = bridge_->error();
        connected_ = false;
        bridge_.reset();
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& cb : conn_callbacks_) cb(ConnectionType::Failed);
        return false;
    }

    // Wait for the data channel to become ready (up to 10 seconds).
    for (int i = 0; i < 200 && !bridge_->is_ready(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!bridge_->is_ready()) {
        status_.type = ConnectionType::Failed;
        status_.remote_address = "Timed out waiting for data channel";
        connected_ = false;
        bridge_.reset();
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& cb : conn_callbacks_) cb(ConnectionType::Failed);
        return false;
    }

    connected_ = true;
    status_.type = ConnectionType::Direct;
    status_.remote_address = "localhost:" + std::to_string(bridge_->local_port());
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& cb : conn_callbacks_) cb(ConnectionType::Direct);
    }
    return true;
}

bool ConnectionManager::disconnect() {
    if (!connected_ && !bridge_) return false;
    connected_ = false;
    status_.type = ConnectionType::None;
    status_.ping_ms = 0;
    status_.remote_address.clear();
    if (bridge_) {
        bridge_->stop();
        bridge_.reset();
    }

    {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& cb : conn_callbacks_) cb(ConnectionType::None);
    }
    return true;
}

ConnectionStatus ConnectionManager::get_status() const { return status_; }
bool ConnectionManager::is_connected() const { return connected_; }
int ConnectionManager::get_ping() const { return status_.ping_ms; }

void ConnectionManager::on_connection_changed(ConnectionChangedCallback cb) {
    std::lock_guard<std::mutex> lock(mu_);
    conn_callbacks_.push_back(std::move(cb));
}

void ConnectionManager::on_data_received(DataReceivedCallback cb) {
    std::lock_guard<std::mutex> lock(mu_);
    data_callbacks_.push_back(std::move(cb));
}

}  // namespace aml::essentials
