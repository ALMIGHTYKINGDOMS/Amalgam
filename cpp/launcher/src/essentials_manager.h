#pragma once

#include "essentials.h"
#include "supabase.h"
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <atomic>
#include <thread>
#include <functional>
#include <ctime>

namespace aml::essentials {

// ---------------------------------------------------------------------------
// Friends Manager
// ---------------------------------------------------------------------------

class FriendsManager {
public:
    static FriendsManager& instance();

    bool initialize();
    void shutdown();

    // Friends
    std::vector<EssentialsFriend> get_friends();
    bool send_request(const std::string& user_id, const std::string& message = "");
    bool accept_request(const std::string& request_id);
    bool decline_request(const std::string& request_id);
    bool remove_friend(const std::string& user_id);
    bool block_user(const std::string& user_id);
    bool unblock_user(const std::string& user_id);
    std::vector<std::string> get_blocked_users();
    std::vector<EssentialsFriendRequest> get_pending_requests();

    // Search
    std::vector<EssentialsFriend> search_users(const std::string& query, int limit = 20);

    // Visual-review fixture seeding (never called in the normal launcher).
    void seed_fixture_friends(const std::vector<EssentialsFriend>& friends);

    // Callbacks
    using FriendsChangedCallback = std::function<void()>;
    void on_friends_changed(FriendsChangedCallback cb);

private:
    FriendsManager();
    ~FriendsManager();
    FriendsManager(const FriendsManager&) = delete;
    FriendsManager& operator=(const FriendsManager&) = delete;

    void refresh_friends();
    void poll_presence();

    std::vector<EssentialsFriend> friends_;
    std::vector<EssentialsFriendRequest> pending_requests_;
    std::vector<std::string> blocked_users_;
    mutable std::mutex mu_;
    std::atomic<bool> running_{false};
    std::thread presence_thread_;
    std::vector<FriendsChangedCallback> callbacks_;
};

// ---------------------------------------------------------------------------
// Presence Manager
// ---------------------------------------------------------------------------

class PresenceManager {
public:
    static PresenceManager& instance();

    bool initialize();
    void shutdown();

    // Self presence
    bool set_status(FriendStatus status, const std::string& message = "",
                    const std::string& session_id = "");
    bool set_playing(const std::string& profile_name, const std::string& game_version,
                     const std::string& loader);
    bool set_hosting(const std::string& session_id, const std::string& world_name);
    bool set_offline();

    // Others
    EssentialsPresence get_user_presence(const std::string& user_id);
    std::vector<EssentialsPresence> get_friends_presence();

    // Callbacks
    using PresenceChangedCallback = std::function<void(const std::string& user_id, const EssentialsPresence&)>;
    void on_presence_changed(PresenceChangedCallback cb);

private:
    PresenceManager();
    ~PresenceManager();
    PresenceManager(const PresenceManager&) = delete;
    PresenceManager& operator=(const PresenceManager&) = delete;

    void heartbeat_loop();

    EssentialsPresence self_presence_;
    std::map<std::string, EssentialsPresence> friends_presence_;
    mutable std::mutex mu_;
    std::atomic<bool> running_{false};
    std::thread heartbeat_thread_;
    std::vector<PresenceChangedCallback> callbacks_;
};

}  // namespace aml::essentials
