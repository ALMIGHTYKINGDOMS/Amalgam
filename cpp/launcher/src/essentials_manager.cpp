#include "essentials_manager.h"
#include <algorithm>

namespace aml::essentials {

// ---------------------------------------------------------------------------
// FriendsManager
// ---------------------------------------------------------------------------

FriendsManager& FriendsManager::instance() {
    static FriendsManager inst;
    return inst;
}

FriendsManager::FriendsManager() = default;

FriendsManager::~FriendsManager() {
    shutdown();
}

bool FriendsManager::initialize() {
    if (running_) return true;

    running_ = true;
    refresh_friends();

    presence_thread_ = std::thread([this]() {
        while (running_) {
            std::this_thread::sleep_for(std::chrono::seconds(60));
            if (!running_) break;
            refresh_friends();
        }
    });

    return true;
}

void FriendsManager::shutdown() {
    running_ = false;
    if (presence_thread_.joinable()) {
        presence_thread_.join();
    }
}

std::vector<EssentialsFriend> FriendsManager::get_friends() {
    std::lock_guard<std::mutex> lock(mu_);
    return friends_;
}

bool FriendsManager::send_request(const std::string& user_id, const std::string& message) {
    auto& supa = aml::supabase::SupabaseManager::instance();
    return supa.send_friend_request(user_id, message);
}

bool FriendsManager::accept_request(const std::string& request_id) {
    auto& supa = aml::supabase::SupabaseManager::instance();
    bool ok = supa.accept_friend_request(request_id);
    if (ok) refresh_friends();
    return ok;
}

bool FriendsManager::decline_request(const std::string& request_id) {
    auto& supa = aml::supabase::SupabaseManager::instance();
    return supa.reject_friend_request(request_id);
}

bool FriendsManager::remove_friend(const std::string& user_id) {
    auto& supa = aml::supabase::SupabaseManager::instance();
    bool ok = supa.remove_friend(user_id);
    if (ok) refresh_friends();
    return ok;
}

bool FriendsManager::block_user(const std::string& user_id) {
    auto& supa = aml::supabase::SupabaseManager::instance();
    bool ok = supa.block_user(user_id);
    if (ok) refresh_friends();
    return ok;
}

bool FriendsManager::unblock_user(const std::string& user_id) {
    auto& supa = aml::supabase::SupabaseManager::instance();
    bool ok = supa.unblock_user(user_id);
    if (ok) refresh_friends();
    return ok;
}

std::vector<std::string> FriendsManager::get_blocked_users() {
    std::lock_guard<std::mutex> lock(mu_);
    return blocked_users_;
}

std::vector<EssentialsFriendRequest> FriendsManager::get_pending_requests() {
    std::lock_guard<std::mutex> lock(mu_);
    return pending_requests_;
}

std::vector<EssentialsFriend> FriendsManager::search_users(const std::string& query, int limit) {
    auto& supa = aml::supabase::SupabaseManager::instance();
    auto profiles = supa.search_users(query, limit);

    std::vector<EssentialsFriend> results;
    results.reserve(profiles.size());
    for (auto& p : profiles) {
        EssentialsFriend f;
        f.user_id = p.user_id;
        f.username = p.minecraft_username.empty() ? p.display_name : p.minecraft_username;
        f.display_name = p.display_name;
        f.avatar_url = p.avatar_url;
        f.status = FriendStatus::Offline;
        f.last_seen = p.last_seen_at;
        results.push_back(std::move(f));
    }
    return results;
}

void FriendsManager::seed_fixture_friends(
    const std::vector<EssentialsFriend>& friends) {
    std::lock_guard<std::mutex> lock(mu_);
    friends_ = friends;
    for (auto& cb : callbacks_) cb();
}

void FriendsManager::on_friends_changed(FriendsChangedCallback cb) {
    callbacks_.push_back(std::move(cb));
}

void FriendsManager::refresh_friends() {
    auto& supa = aml::supabase::SupabaseManager::instance();
    auto raw = supa.get_friends();
    auto raw_requests = supa.get_friend_requests();
    const auto blocked = supa.get_blocked_users();

    std::vector<EssentialsFriend> converted;
    converted.reserve(raw.size());
    for (auto& f : raw) {
        EssentialsFriend ef;
        ef.user_id = f.user_id_b.empty() ? f.user_id_a : f.user_id_b;
        if (std::find(blocked.begin(), blocked.end(), ef.user_id) != blocked.end()) continue;
        ef.username = f.friend_minecraft_username.empty()
            ? f.friend_display_name
            : f.friend_minecraft_username;
        if (ef.username.empty()) ef.username = ef.user_id;
        ef.display_name = f.friend_display_name.empty() ? ef.username : f.friend_display_name;
        ef.avatar_url = f.friend_avatar_url;
        ef.status = FriendStatus::Offline;
        ef.last_seen = f.updated_at;
        converted.push_back(std::move(ef));
    }

    std::vector<EssentialsFriendRequest> requests;
    requests.reserve(raw_requests.size());
    for (auto& r : raw_requests) {
        EssentialsFriendRequest er;
        er.id = r.id;
        er.sender_id = r.sender_id;
        er.sender_username = r.sender_username;
        er.sender_avatar_url = r.sender_avatar_url;
        er.receiver_id = r.receiver_id;
        er.status = InviteStatus::Pending;
        er.message = r.message;
        er.created_at = r.created_at;
        requests.push_back(std::move(er));
    }

    {
        std::lock_guard<std::mutex> lock(mu_);
        friends_ = std::move(converted);
        pending_requests_ = std::move(requests);
        blocked_users_ = blocked;
    }

    for (auto& cb : callbacks_) cb();
}

void FriendsManager::poll_presence() {
    // Presence polling handled by PresenceManager heartbeat
}

// ---------------------------------------------------------------------------
// PresenceManager
// ---------------------------------------------------------------------------

PresenceManager& PresenceManager::instance() {
    static PresenceManager inst;
    return inst;
}

PresenceManager::PresenceManager() = default;

PresenceManager::~PresenceManager() {
    shutdown();
}

bool PresenceManager::initialize() {
    if (running_) return true;

    running_ = true;

    auto& supa = aml::supabase::SupabaseManager::instance();
    supa.update_presence("online");

    self_presence_.status = FriendStatus::Online;
    self_presence_.last_seen = static_cast<int64_t>(std::time(nullptr));

    heartbeat_thread_ = std::thread([this]() {
        while (running_) {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            if (!running_) break;

            auto& supa = aml::supabase::SupabaseManager::instance();
            std::string status_str;
            {
                std::lock_guard<std::mutex> lock(mu_);
                switch (self_presence_.status) {
                    case FriendStatus::Online:    status_str = "online"; break;
                    case FriendStatus::Playing:   status_str = "in_game"; break;
                    case FriendStatus::Hosting:   status_str = "in_game"; break;
                    case FriendStatus::Away:       status_str = "away"; break;
                    default:                       status_str = "online"; break;
                }
            }
            supa.update_presence(status_str, self_presence_.status_message);

            auto raw = supa.get_friends_presence();
            std::map<std::string, EssentialsPresence> updated;
            for (auto& p : raw) {
                EssentialsPresence ep;
                ep.user_id = p.user_id;
                if (p.status == "online")          ep.status = FriendStatus::Online;
                else if (p.status == "in_game")    ep.status = FriendStatus::Playing;
                else if (p.status == "away")       ep.status = FriendStatus::Away;
                else                               ep.status = FriendStatus::Offline;
                ep.status_message = p.status_message;
                ep.last_seen = p.last_seen_at;
                updated[ep.user_id] = ep;
            }

            {
                std::lock_guard<std::mutex> lock(mu_);
                friends_presence_ = std::move(updated);
            }

            for (auto& cb : callbacks_) {
                for (auto& [uid, ep] : friends_presence_) {
                    cb(uid, ep);
                }
            }
        }
    });

    return true;
}

void PresenceManager::shutdown() {
    running_ = false;
    {
        auto& supa = aml::supabase::SupabaseManager::instance();
        supa.update_presence("offline");
    }
    if (heartbeat_thread_.joinable()) {
        heartbeat_thread_.join();
    }
}

bool PresenceManager::set_status(FriendStatus status, const std::string& message,
                                 const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mu_);
    self_presence_.status = status;
    self_presence_.status_message = message;
    self_presence_.session_id = session_id;
    self_presence_.last_seen = static_cast<int64_t>(std::time(nullptr));

    auto& supa = aml::supabase::SupabaseManager::instance();
    std::string status_str;
    switch (status) {
        case FriendStatus::Online:    status_str = "online"; break;
        case FriendStatus::Playing:   status_str = "in_game"; break;
        case FriendStatus::Hosting:   status_str = "in_game"; break;
        case FriendStatus::Away:       status_str = "away"; break;
        default:                       status_str = "online"; break;
    }
    return supa.update_presence(status_str, message);
}

bool PresenceManager::set_playing(const std::string& profile_name, const std::string& game_version,
                                  const std::string& loader) {
    std::lock_guard<std::mutex> lock(mu_);
    self_presence_.status = FriendStatus::Playing;
    self_presence_.current_profile_name = profile_name;
    self_presence_.current_game_version = game_version;
    self_presence_.current_loader = loader;
    self_presence_.last_seen = static_cast<int64_t>(std::time(nullptr));

    auto& supa = aml::supabase::SupabaseManager::instance();
    return supa.update_presence("in_game", "Playing " + game_version);
}

bool PresenceManager::set_hosting(const std::string& session_id, const std::string& world_name) {
    std::lock_guard<std::mutex> lock(mu_);
    self_presence_.status = FriendStatus::Hosting;
    self_presence_.session_id = session_id;
    self_presence_.status_message = "Hosting " + world_name;
    self_presence_.last_seen = static_cast<int64_t>(std::time(nullptr));

    auto& supa = aml::supabase::SupabaseManager::instance();
    return supa.update_presence("in_game", "Hosting " + world_name);
}

bool PresenceManager::set_offline() {
    std::lock_guard<std::mutex> lock(mu_);
    self_presence_.status = FriendStatus::Offline;
    self_presence_.status_message.clear();
    self_presence_.session_id.clear();
    self_presence_.last_seen = static_cast<int64_t>(std::time(nullptr));

    auto& supa = aml::supabase::SupabaseManager::instance();
    return supa.update_presence("offline");
}

EssentialsPresence PresenceManager::get_user_presence(const std::string& user_id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = friends_presence_.find(user_id);
    if (it != friends_presence_.end()) return it->second;

    EssentialsPresence p;
    p.user_id = user_id;
    p.status = FriendStatus::Offline;
    return p;
}

std::vector<EssentialsPresence> PresenceManager::get_friends_presence() {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<EssentialsPresence> result;
    result.reserve(friends_presence_.size());
    for (auto& [_, p] : friends_presence_) {
        result.push_back(p);
    }
    return result;
}

void PresenceManager::on_presence_changed(PresenceChangedCallback cb) {
    callbacks_.push_back(std::move(cb));
}

}  // namespace aml::essentials
