#pragma once

#include "essentials.h"
#include "essentials_tcp_bridge.h"
#include "instances.h"
#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <mutex>
#include <memory>

namespace aml::essentials {

// ---------------------------------------------------------------------------
// Profile Sync - compares local profiles against host manifests
// ---------------------------------------------------------------------------

class ProfileSync {
public:
    static ProfileSync& instance();

    CompatCheck check_compatibility(const instances::Instance& local,
                                    const EssentialsManifest& host_manifest);

    SyncPlan build_sync_plan(const instances::Instance& local,
                             const EssentialsManifest& host_manifest,
                             SyncMode mode = SyncMode::TemporaryProfile);

    bool execute_sync(const SyncPlan& plan,
                      const std::function<void(float progress, const std::string& status)>& on_progress);

    std::string hash_file(const std::wstring& path);

    std::vector<ModEntry> scan_local_mods(const instances::Instance& local);

    std::string create_synced_profile(const instances::Instance& original,
                                      const SyncPlan& plan,
                                      std::string* err);

    bool cleanup_temporary_profile(const std::string& profile_id);

private:
    ProfileSync() = default;
    ~ProfileSync() = default;
    ProfileSync(const ProfileSync&) = delete;
    ProfileSync& operator=(const ProfileSync&) = delete;
};

// ---------------------------------------------------------------------------
// Join Manager - orchestrates the full join flow
// ---------------------------------------------------------------------------

class JoinManager {
public:
    static JoinManager& instance();

    JoinResult analyze_and_prepare(const std::string& session_id,
                                   const std::string& join_token);

    SyncPlan get_current_sync_plan() const;
    CompatCheck get_current_compat() const;

    bool execute_join(const std::function<void(float, const std::string&)>& on_progress);

    void cancel_join();

    bool is_joining() const;
    float get_progress() const;
    std::string get_status_text() const;

private:
    JoinManager() = default;
    ~JoinManager() = default;
    JoinManager(const JoinManager&) = delete;
    JoinManager& operator=(const JoinManager&) = delete;

    CompatCheck compat_;
    SyncPlan sync_plan_;
    std::atomic<bool> joining_{false};
    std::atomic<float> progress_{0.0f};
    std::string status_text_;
    std::string session_id_;
    std::string join_token_;
    std::string host_user_id_;
    std::string source_profile_id_;
    std::unique_ptr<TcpDataChannelBridge> bridge_;
};

// ---------------------------------------------------------------------------
// World Host - manages hosting a world as a multiplayer session
// ---------------------------------------------------------------------------

class WorldHost {
public:
    static WorldHost& instance();

    bool start_hosting(const HostOptions& options,
                       const std::function<void(float, const std::string&)>& on_progress);

    bool stop_hosting();

    EssentialsSession get_current_session() const;
    bool is_hosting() const;

    bool kick_player(const std::string& user_id);
    bool ban_player(const std::string& user_id, const std::string& reason = "");
    bool unban_player(const std::string& user_id);
    bool update_privacy(SessionPrivacy privacy);
    bool update_player_limit(int limit);

    std::vector<WorldEntry> list_worlds(const instances::Instance& profile);

    using HostStateChangedCallback = std::function<void(SessionState)>;
    using PlayerChangedCallback = std::function<void(const std::string& user_id, const std::string& username, bool joined)>;
    void on_host_state_changed(HostStateChangedCallback cb);
    void on_player_changed(PlayerChangedCallback cb);

private:
    WorldHost() = default;
    ~WorldHost() = default;
    WorldHost(const WorldHost&) = delete;
    WorldHost& operator=(const WorldHost&) = delete;

    void notify_state(SessionState state);
    void notify_player(const std::string& user_id, const std::string& username, bool joined);

    EssentialsSession current_session_;
    std::atomic<bool> hosting_{false};
    mutable std::mutex mu_;
    std::vector<HostStateChangedCallback> state_callbacks_;
    std::vector<PlayerChangedCallback> player_callbacks_;
    std::unique_ptr<TcpDataChannelBridge> bridge_;
    uintptr_t server_process_ = 0;
};

// ---------------------------------------------------------------------------
// Connection Manager - handles network connectivity to sessions
// ---------------------------------------------------------------------------

class ConnectionManager {
public:
    static ConnectionManager& instance();

    bool connect(const std::string& session_id, const std::string& join_token);
    bool disconnect();

    ConnectionStatus get_status() const;
    bool is_connected() const;
    int get_ping() const;

    using ConnectionChangedCallback = std::function<void(ConnectionType)>;
    using DataReceivedCallback = std::function<void(const std::string&)>;
    void on_connection_changed(ConnectionChangedCallback cb);
    void on_data_received(DataReceivedCallback cb);

private:
    ConnectionManager() = default;
    ~ConnectionManager() = default;
    ConnectionManager(const ConnectionManager&) = delete;
    ConnectionManager& operator=(const ConnectionManager&) = delete;

    ConnectionStatus status_;
    std::atomic<bool> connected_{false};
    mutable std::mutex mu_;
    std::vector<ConnectionChangedCallback> conn_callbacks_;
    std::vector<DataReceivedCallback> data_callbacks_;
    std::unique_ptr<TcpDataChannelBridge> bridge_;
};

}  // namespace aml::essentials
