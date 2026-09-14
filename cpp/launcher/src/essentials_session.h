#pragma once

#include "essentials.h"
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <atomic>
#include <thread>
#include <functional>

namespace aml::essentials {

// ---------------------------------------------------------------------------
// Invite Manager
// ---------------------------------------------------------------------------

class InviteManager {
public:
    static InviteManager& instance();

    bool initialize();
    void shutdown();

    bool send_invite(const std::string& target_user_id, const EssentialsSession& session);
    bool accept_invite(const std::string& invite_id, EssentialsInvite* accepted = nullptr);
    bool decline_invite(const std::string& invite_id);
    bool cancel_invite(const std::string& invite_id);
    std::vector<EssentialsInvite> get_received_invites();
    std::vector<EssentialsInvite> get_sent_invites();

    using InviteReceivedCallback = std::function<void(const EssentialsInvite&)>;
    void on_invite_received(InviteReceivedCallback cb);

private:
    InviteManager();
    ~InviteManager();
    InviteManager(const InviteManager&) = delete;
    InviteManager& operator=(const InviteManager&) = delete;

    void poll_invites();

    std::vector<EssentialsInvite> received_invites_;
    std::vector<EssentialsInvite> sent_invites_;
    mutable std::mutex mu_;
    std::atomic<bool> running_{false};
    std::thread poll_thread_;
    std::vector<InviteReceivedCallback> callbacks_;
};

// ---------------------------------------------------------------------------
// Session Manager
// ---------------------------------------------------------------------------

class SessionManager {
public:
    static SessionManager& instance();

    bool initialize();
    void shutdown();

    // Host session
    bool create_session(const HostOptions& options, EssentialsSession& out_session);
    bool start_session(const std::string& session_id);
    bool stop_session(const std::string& session_id);
    bool update_session(const EssentialsSession& session);

    // Join session
    bool join_session(const std::string& session_id, const std::string& join_token);
    bool leave_session(const std::string& session_id);

    // Session queries
    EssentialsSession get_session(const std::string& session_id);
    std::vector<EssentialsSession> get_active_sessions();
    bool is_host(const std::string& session_id) const;
    std::string get_current_session_id() const;

    // Player management
    bool kick_player(const std::string& session_id, const std::string& user_id);
    bool ban_player(const std::string& session_id, const std::string& user_id, const std::string& reason = "");
    bool unban_player(const std::string& session_id, const std::string& user_id);
    std::vector<SessionBanEntry> get_bans(const std::string& session_id);

    // Session manifest
    bool create_manifest(const std::string& session_id, const EssentialsManifest& manifest);
    bool fetch_manifest(const std::string& session_id, const std::string& join_token,
                        EssentialsManifest& out_manifest);
    EssentialsManifest get_manifest(const std::string& session_id);

    // Notifications
    std::vector<EssentialsNotification> get_notifications();
    void clear_notification(const std::string& notification_id);

    // Visual-review fixture seeding (never called in the normal launcher).
    void seed_fixture_notifications(const std::vector<EssentialsNotification>& notifications);

    // Callbacks
    using SessionStateChangedCallback = std::function<void(const std::string& session_id, SessionState state)>;
    using PlayerJoinedCallback = std::function<void(const std::string& session_id, const std::string& user_id, const std::string& username)>;
    using PlayerLeftCallback = std::function<void(const std::string& session_id, const std::string& user_id)>;
    void on_session_state_changed(SessionStateChangedCallback cb);
    void on_player_joined(PlayerJoinedCallback cb);
    void on_player_left(PlayerLeftCallback cb);

private:
    SessionManager();
    ~SessionManager();
    SessionManager(const SessionManager&) = delete;
    SessionManager& operator=(const SessionManager&) = delete;

    std::string generate_session_id();
    std::string generate_join_token();

    std::map<std::string, EssentialsSession> sessions_;
    std::map<std::string, EssentialsManifest> manifests_;
    std::map<std::string, std::vector<SessionBanEntry>> bans_;
    std::vector<EssentialsNotification> notifications_;
    std::string current_session_id_;
    mutable std::mutex mu_;
    std::atomic<bool> running_{false};
    std::vector<SessionStateChangedCallback> state_callbacks_;
    std::vector<PlayerJoinedCallback> join_callbacks_;
    std::vector<PlayerLeftCallback> leave_callbacks_;
};

}  // namespace aml::essentials
