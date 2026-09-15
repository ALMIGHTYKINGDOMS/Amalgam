#include "essentials_session.h"
#include "essentials_address.h"
#include "supabase.h"

#include <algorithm>
#include <random>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <ctime>
#include <set>

namespace aml::essentials {

namespace {

std::string make_hex_id(int length) {
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<int> dist(0, 15);
    std::ostringstream oss;
    for (int i = 0; i < length; i++) oss << std::hex << dist(rng);
    return oss.str();
}

std::string invite_status_str(InviteStatus s) {
    switch (s) {
        case InviteStatus::Pending:  return "pending";
        case InviteStatus::Accepted: return "accepted";
        case InviteStatus::Declined: return "declined";
        case InviteStatus::Expired:  return "expired";
        case InviteStatus::Cancelled: return "cancelled";
    }
    return "pending";
}

InviteStatus invite_status_from_str(const std::string& s) {
    if (s == "accepted") return InviteStatus::Accepted;
    if (s == "declined") return InviteStatus::Declined;
    if (s == "expired")  return InviteStatus::Expired;
    if (s == "cancelled") return InviteStatus::Cancelled;
    return InviteStatus::Pending;
}

InviteStatus invite_status_from_int(int v) {
    if (v < 0 || v > 4) return InviteStatus::Pending;
    return static_cast<InviteStatus>(v);
}

}  // namespace

// ===========================================================================
// InviteManager
// ===========================================================================

InviteManager::InviteManager() = default;

InviteManager::~InviteManager() {
    shutdown();
}

InviteManager& InviteManager::instance() {
    static InviteManager mgr;
    return mgr;
}

bool InviteManager::initialize() {
    std::lock_guard<std::mutex> lock(mu_);
    if (running_) return true;
    running_ = true;
    poll_thread_ = std::thread(&InviteManager::poll_invites, this);
    return true;
}

void InviteManager::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!running_) return;
        running_ = false;
    }
    if (poll_thread_.joinable()) poll_thread_.join();
}

bool InviteManager::send_invite(const std::string& target_user_id, const EssentialsSession& session) {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    if (!supabase.is_authenticated()) return false;

    EssentialsInvite invite;
    invite.id = make_hex_id(16);
    invite.host_user_id = supabase.get_current_user().id;
    invite.host_username = supabase.get_current_user().username.empty()
        ? supabase.get_current_user().email
        : supabase.get_current_user().username;
    invite.target_user_id = target_user_id;
    invite.session_id = session.id;
    invite.session_address = session.address;
    invite.world_name = session.world_name;
    invite.minecraft_version = session.minecraft_version;
    invite.loader = session.loader;
    invite.loader_version = session.loader_version;
    invite.mod_count = static_cast<int>(session.manifest.mods.size());
    invite.status = InviteStatus::Pending;
    invite.created_at = std::time(nullptr);
    invite.expires_at = invite.created_at + 3600;

    Json body;
    body.set("invite_id", invite.id);
    body.set("target_user_id", target_user_id);
    body.set("session_id", session.id);
    body.set("world_name", session.world_name);
    body.set("minecraft_version", session.minecraft_version);
    body.set("loader", session.loader);
    body.set("loader_version", session.loader_version);
    body.set("mod_count", invite.mod_count);

    auto result = supabase.client()->rpc("send_session_invite", body);
    if (!result.success) return false;

    std::lock_guard<std::mutex> lock(mu_);
    sent_invites_.push_back(invite);
    return true;
}

bool InviteManager::accept_invite(const std::string& invite_id, EssentialsInvite* accepted) {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    if (!supabase.is_authenticated()) return false;

    Json body;
    body.set("invite_id", invite_id);
    auto result = supabase.client()->rpc("accept_session_invite", body);

    if (!result.success) return false;

    std::lock_guard<std::mutex> lock(mu_);
    for (auto& inv : received_invites_) {
        if (inv.id == invite_id) {
            inv.status = InviteStatus::Accepted;
            if (!result.data.empty()) {
                const Json& row = result.data.front();
                inv.session_address = row.get("session_address").as_str();
                inv.join_token = row.get("join_token").as_str();
            }
            if (accepted) *accepted = inv;
            break;
        }
    }
    return true;
}

bool InviteManager::decline_invite(const std::string& invite_id) {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    if (!supabase.is_authenticated()) return false;

    Json body;
    body.set("invite_id", invite_id);
    auto result = supabase.client()->rpc("decline_session_invite", body);

    if (!result.success) return false;

    std::lock_guard<std::mutex> lock(mu_);
    for (auto& inv : received_invites_) {
        if (inv.id == invite_id) {
            inv.status = InviteStatus::Declined;
            break;
        }
    }
    return true;
}

bool InviteManager::cancel_invite(const std::string& invite_id) {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    if (!supabase.is_authenticated()) return false;

    Json body;
    body.set("invite_id", invite_id);
    auto result = supabase.client()->rpc("cancel_session_invite", body);

    if (!result.success) return false;

    std::lock_guard<std::mutex> lock(mu_);
    for (auto& inv : sent_invites_) {
        if (inv.id == invite_id) {
            inv.status = InviteStatus::Cancelled;
            break;
        }
    }
    return true;
}

std::vector<EssentialsInvite> InviteManager::get_received_invites() {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    if (supabase.is_authenticated()) {
        auto result = supabase.client()->rpc("get_received_session_invites");
        if (result.success) {
            std::lock_guard<std::mutex> lock(mu_);
            received_invites_.clear();
            for (const auto& item : result.data) {
                EssentialsInvite inv;
                inv.id = item["id"].asString();
                inv.host_user_id = item["host_user_id"].asString();
                inv.host_username = item["host_username"].asString();
                inv.target_user_id = item["target_user_id"].asString();
                inv.session_id = item["session_id"].asString();
                inv.session_address = item["session_address"].asString();
                inv.world_name = item["world_name"].asString();
                inv.minecraft_version = item["minecraft_version"].asString();
                inv.loader = item["loader"].asString();
                inv.loader_version = item["loader_version"].asString();
                inv.mod_count = item["mod_count"].asInt();
                inv.status = invite_status_from_int(item["status"].asInt());
                inv.created_at = item["created_at"].asInt64();
                inv.expires_at = item["expires_at"].asInt64();
                received_invites_.push_back(inv);
            }
        }
    }
    std::lock_guard<std::mutex> lock(mu_);
    return received_invites_;
}

std::vector<EssentialsInvite> InviteManager::get_sent_invites() {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    if (supabase.is_authenticated()) {
        auto result = supabase.client()->rpc("get_sent_session_invites");
        if (result.success) {
            std::lock_guard<std::mutex> lock(mu_);
            sent_invites_.clear();
            for (const auto& item : result.data) {
                EssentialsInvite inv;
                inv.id = item["id"].asString();
                inv.host_user_id = item["host_user_id"].asString();
                inv.target_user_id = item["target_user_id"].asString();
                inv.session_id = item["session_id"].asString();
                inv.session_address = item["session_address"].asString();
                inv.world_name = item["world_name"].asString();
                inv.minecraft_version = item["minecraft_version"].asString();
                inv.loader = item["loader"].asString();
                inv.loader_version = item["loader_version"].asString();
                inv.mod_count = item["mod_count"].asInt();
                inv.status = invite_status_from_int(item["status"].asInt());
                inv.created_at = item["created_at"].asInt64();
                inv.expires_at = item["expires_at"].asInt64();
                sent_invites_.push_back(std::move(inv));
            }
        }
    }
    std::lock_guard<std::mutex> lock(mu_);
    return sent_invites_;
}

void InviteManager::on_invite_received(InviteReceivedCallback cb) {
    std::lock_guard<std::mutex> lock(mu_);
    callbacks_.push_back(std::move(cb));
}

void InviteManager::poll_invites() {
    while (running_) {
        for (int i = 0; i < 10 && running_; ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!running_) break;

        auto& supabase = aml::supabase::SupabaseManager::instance();
        if (!supabase.is_authenticated()) continue;

        auto result = supabase.client()->rpc("get_received_session_invites");
        if (!result.success) continue;

        std::vector<EssentialsInvite> new_invites;
        std::vector<InviteReceivedCallback> cbs;

        {
            std::lock_guard<std::mutex> lock(mu_);
            std::set<std::string> existing_ids;
            for (const auto& inv : received_invites_) existing_ids.insert(inv.id);

            for (const auto& item : result.data) {
                EssentialsInvite inv;
                inv.id = item["id"].asString();
                inv.host_user_id = item["host_user_id"].asString();
                inv.host_username = item["host_username"].asString();
                inv.target_user_id = item["target_user_id"].asString();
                inv.session_id = item["session_id"].asString();
                inv.session_address = item["session_address"].asString();
                inv.world_name = item["world_name"].asString();
                inv.minecraft_version = item["minecraft_version"].asString();
                inv.loader = item["loader"].asString();
                inv.loader_version = item["loader_version"].asString();
                inv.mod_count = item["mod_count"].asInt();
                inv.status = invite_status_from_int(item["status"].asInt());
                inv.created_at = item["created_at"].asInt64();
                inv.expires_at = item["expires_at"].asInt64();

                if (existing_ids.find(inv.id) == existing_ids.end() &&
                    inv.status == InviteStatus::Pending) {
                    new_invites.push_back(inv);
                }
            }

            received_invites_.clear();
            for (const auto& item : result.data) {
                EssentialsInvite inv;
                inv.id = item["id"].asString();
                inv.host_user_id = item["host_user_id"].asString();
                inv.host_username = item["host_username"].asString();
                inv.target_user_id = item["target_user_id"].asString();
                inv.session_id = item["session_id"].asString();
                inv.session_address = item["session_address"].asString();
                inv.world_name = item["world_name"].asString();
                inv.minecraft_version = item["minecraft_version"].asString();
                inv.loader = item["loader"].asString();
                inv.loader_version = item["loader_version"].asString();
                inv.mod_count = item["mod_count"].asInt();
                inv.status = invite_status_from_int(item["status"].asInt());
                inv.created_at = item["created_at"].asInt64();
                inv.expires_at = item["expires_at"].asInt64();
                received_invites_.push_back(inv);
            }

            cbs = callbacks_;
        }

        for (const auto& inv : new_invites) {
            for (const auto& cb : cbs) cb(inv);
        }
    }
}

// ===========================================================================
// SessionManager
// ===========================================================================

SessionManager::SessionManager() = default;

SessionManager::~SessionManager() {
    shutdown();
}

SessionManager& SessionManager::instance() {
    static SessionManager mgr;
    return mgr;
}

bool SessionManager::initialize() {
    std::lock_guard<std::mutex> lock(mu_);
    running_ = true;
    return true;
}

void SessionManager::shutdown() {
    std::lock_guard<std::mutex> lock(mu_);
    running_ = false;
    sessions_.clear();
    manifests_.clear();
    bans_.clear();
    notifications_.clear();
}

std::string SessionManager::generate_session_id() {
    return make_hex_id(16);
}

std::string SessionManager::generate_join_token() {
    return make_hex_id(32);
}

// ---------------------------------------------------------------------------
// Host session
// ---------------------------------------------------------------------------

bool SessionManager::create_session(const HostOptions& options, EssentialsSession& out_session) {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    if (!supabase.is_authenticated()) return false;

    auto user = supabase.get_current_user();

    EssentialsAddressResolver resolver;
    std::string alias = options.alias.empty()
        ? EssentialsAddressResolver::Generate(options.world_name)
        : EssentialsAddressResolver::NormalizeAlias(options.alias);
    if (options.alias.empty()) {
        for (int attempt = 0; attempt < 5 && !resolver.IsAvailable(alias); ++attempt)
            alias = EssentialsAddressResolver::Generate(options.world_name);
    }
    if (!EssentialsAddressResolver::IsValidAlias(alias) || !resolver.IsAvailable(alias))
        return false;

    EssentialsSession session;
    session.id = generate_session_id();
    session.alias = alias;
    session.address = EssentialsAddressResolver::ToAddress(alias);
    session.status = "starting";
    session.join_token = generate_join_token();
    session.host_user_id = user.id;
    session.host_username = user.username.empty() ? user.email : user.username;
    session.profile_id = options.profile_id;
    session.profile_name = options.world_name;
    session.world_name = options.world_name;
    session.privacy = options.privacy;
    session.player_limit = options.player_limit;
    session.player_count = 1;
    session.state = SessionState::Starting;
    session.connected_players = {user.id};
    session.created_at = std::time(nullptr);
    session.expires_at = session.created_at + 86400;

    Json body;
    body.set("session_id", session.id);
    body.set("host_user_id", session.host_user_id);
    body.set("world_name", session.world_name);
    body.set("privacy", static_cast<int>(session.privacy));
    body.set("player_limit", session.player_limit);
    body.set("join_token", Json::str(session.join_token));
    body.set("session_alias", Json::str(session.alias));
    body.set("session_address", Json::str(session.address));

    auto result = supabase.client()->rpc("create_session", body);
    if (!result.success) return false;

    {
        std::lock_guard<std::mutex> lock(mu_);
        sessions_[session.id] = session;
        current_session_id_ = session.id;
    }

    {
        EssentialsNotification note;
        note.id = make_hex_id(16);
        note.title = "Session Created";
        note.body = "You are hosting: " + session.world_name;
        note.session_id = session.id;
        note.created_at = std::time(nullptr);
        note.read = false;
        std::lock_guard<std::mutex> lock(mu_);
        notifications_.push_back(note);
    }

    out_session = session;
    return true;
}

bool SessionManager::start_session(const std::string& session_id) {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    if (!supabase.is_authenticated()) return false;

    Json body;
    body.set("session_id", session_id);
    auto result = supabase.client()->rpc("start_session", body);

    std::vector<SessionStateChangedCallback> cbs;
    if (!result.success) return false;

    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = sessions_.find(session_id);
        if (it != sessions_.end()) {
            it->second.state = SessionState::Online;
            it->second.status = "online";
        }
        cbs = state_callbacks_;
    }

    for (const auto& cb : cbs) cb(session_id, SessionState::Online);
    return result.success;
}

bool SessionManager::stop_session(const std::string& session_id) {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    if (!supabase.is_authenticated()) return false;

    Json body;
    body.set("session_id", session_id);
    auto result = supabase.client()->rpc("stop_session", body);

    std::vector<SessionStateChangedCallback> cbs;
    if (!result.success) return false;

    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = sessions_.find(session_id);
        if (it != sessions_.end()) {
            it->second.state = SessionState::Ended;
            it->second.status = "offline";
        }
        if (current_session_id_ == session_id) current_session_id_.clear();
        cbs = state_callbacks_;
    }

    for (const auto& cb : cbs) cb(session_id, SessionState::Ended);
    return result.success;
}

bool SessionManager::update_session(const EssentialsSession& session) {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    if (!supabase.is_authenticated()) return false;

    Json body;
    body.set("session_id", session.id);
    body.set("world_name", session.world_name);
    body.set("privacy", static_cast<int>(session.privacy));
    body.set("player_limit", session.player_limit);
    body.set("state", static_cast<int>(session.state));
    auto result = supabase.client()->rpc("update_session", body);
    if (!result.success) return false;

    {
        std::lock_guard<std::mutex> lock(mu_);
        sessions_[session.id] = session;
        sessions_[session.id].status = session.state == SessionState::Online ? "online" : "offline";
    }
    return true;
}

// ---------------------------------------------------------------------------
// Join session
// ---------------------------------------------------------------------------

bool SessionManager::join_session(const std::string& session_id, const std::string& join_token) {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    if (!supabase.is_authenticated()) return false;

    auto user = supabase.get_current_user();

    Json body;
    body.set("session_id", session_id);
    body.set("join_token", join_token);
    auto result = supabase.client()->rpc("join_session", body);

    if (!result.success) return false;

    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = sessions_.find(session_id);
        if (it != sessions_.end()) {
            auto& s = it->second;
            if (std::find(s.connected_players.begin(), s.connected_players.end(), user.id)
                == s.connected_players.end()) {
                s.connected_players.push_back(user.id);
                s.player_count = static_cast<int>(s.connected_players.size());
            }
        }
        if (!result.data.empty()) {
            sessions_[session_id].host_user_id = result.data.front().get("host_user_id").as_str();
        }
        current_session_id_ = session_id;
    }

    {
        EssentialsNotification note;
        note.id = make_hex_id(16);
        note.title = "Joined Session";
        note.body = "You joined a session";
        note.session_id = session_id;
        note.from_user_id = user.id;
        note.created_at = std::time(nullptr);
        note.read = false;
        std::lock_guard<std::mutex> lock(mu_);
        notifications_.push_back(note);
    }

    std::vector<PlayerJoinedCallback> cbs;
    {
        std::lock_guard<std::mutex> lock(mu_);
        cbs = join_callbacks_;
    }
    for (const auto& cb : cbs) cb(session_id, user.id, user.username.empty() ? user.email : user.username);

    return result.success;
}

bool SessionManager::leave_session(const std::string& session_id) {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    if (!supabase.is_authenticated()) return false;

    auto user = supabase.get_current_user();

    Json body;
    body.set("session_id", session_id);
    auto result = supabase.client()->rpc("leave_session", body);

    std::vector<PlayerLeftCallback> cbs;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = sessions_.find(session_id);
        if (it != sessions_.end()) {
            auto& s = it->second;
            s.connected_players.erase(
                std::remove(s.connected_players.begin(), s.connected_players.end(), user.id),
                s.connected_players.end());
            s.player_count = static_cast<int>(s.connected_players.size());
        }
        if (current_session_id_ == session_id) current_session_id_.clear();
        cbs = leave_callbacks_;
    }

    for (const auto& cb : cbs) cb(session_id, user.id);
    return result.success;
}

// ---------------------------------------------------------------------------
// Session queries
// ---------------------------------------------------------------------------

EssentialsSession SessionManager::get_session(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = sessions_.find(session_id);
    return it != sessions_.end() ? it->second : EssentialsSession{};
}

std::vector<EssentialsSession> SessionManager::get_active_sessions() {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<EssentialsSession> result;
    for (const auto& [id, s] : sessions_) {
        if (s.state != SessionState::Ended) result.push_back(s);
    }
    return result;
}

bool SessionManager::is_host(const std::string& session_id) const {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    std::lock_guard<std::mutex> lock(mu_);
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) return false;
    return it->second.host_user_id == supabase.get_current_user().id;
}

std::string SessionManager::get_current_session_id() const {
    std::lock_guard<std::mutex> lock(mu_);
    return current_session_id_;
}

// ---------------------------------------------------------------------------
// Player management
// ---------------------------------------------------------------------------

bool SessionManager::kick_player(const std::string& session_id, const std::string& user_id) {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    if (!supabase.is_authenticated()) return false;

    Json body;
    body.set("session_id", session_id);
    body.set("user_id", user_id);
    auto result = supabase.client()->rpc("kick_player", body);

    std::vector<PlayerLeftCallback> cbs;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = sessions_.find(session_id);
        if (it != sessions_.end()) {
            auto& s = it->second;
            s.connected_players.erase(
                std::remove(s.connected_players.begin(), s.connected_players.end(), user_id),
                s.connected_players.end());
            s.player_count = static_cast<int>(s.connected_players.size());
        }
        cbs = leave_callbacks_;
    }

    for (const auto& cb : cbs) cb(session_id, user_id);
    return result.success;
}

bool SessionManager::ban_player(const std::string& session_id, const std::string& user_id, const std::string& reason) {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    if (!supabase.is_authenticated()) return false;

    Json body;
    body.set("session_id", session_id);
    body.set("user_id", user_id);
    body.set("reason", reason);
    auto result = supabase.client()->rpc("ban_player", body);

    {
        std::lock_guard<std::mutex> lock(mu_);
        SessionBanEntry ban;
        ban.user_id = user_id;
        ban.banned_at = std::time(nullptr);
        ban.reason = reason;
        bans_[session_id].push_back(ban);

        auto it = sessions_.find(session_id);
        if (it != sessions_.end()) {
            auto& s = it->second;
            if (std::find(s.banned_players.begin(), s.banned_players.end(), user_id)
                == s.banned_players.end()) {
                s.banned_players.push_back(user_id);
            }
            s.connected_players.erase(
                std::remove(s.connected_players.begin(), s.connected_players.end(), user_id),
                s.connected_players.end());
            s.player_count = static_cast<int>(s.connected_players.size());
        }
    }

    std::vector<PlayerLeftCallback> cbs;
    {
        std::lock_guard<std::mutex> lock(mu_);
        cbs = leave_callbacks_;
    }
    for (const auto& cb : cbs) cb(session_id, user_id);

    return result.success;
}

bool SessionManager::unban_player(const std::string& session_id, const std::string& user_id) {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    if (!supabase.is_authenticated()) return false;

    Json body;
    body.set("session_id", session_id);
    body.set("user_id", user_id);
    auto result = supabase.client()->rpc("unban_player", body);

    {
        std::lock_guard<std::mutex> lock(mu_);
        auto& ban_list = bans_[session_id];
        ban_list.erase(
            std::remove_if(ban_list.begin(), ban_list.end(),
                [&](const SessionBanEntry& b) { return b.user_id == user_id; }),
            ban_list.end());

        auto it = sessions_.find(session_id);
        if (it != sessions_.end()) {
            auto& bp = it->second.banned_players;
            bp.erase(std::remove(bp.begin(), bp.end(), user_id), bp.end());
        }
    }
    return result.success;
}

std::vector<SessionBanEntry> SessionManager::get_bans(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = bans_.find(session_id);
    return it != bans_.end() ? it->second : std::vector<SessionBanEntry>{};
}

// ---------------------------------------------------------------------------
// Session manifest
// ---------------------------------------------------------------------------

bool SessionManager::create_manifest(const std::string& session_id, const EssentialsManifest& manifest) {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    if (!supabase.is_authenticated()) return false;

    Json body;
    body.set("session_id", session_id);
    body.set("minecraft_version", manifest.minecraft_version);
    body.set("loader", manifest.loader);
    body.set("loader_version", manifest.loader_version);
    Json mods = Json::arr();
    for (const auto& mod : manifest.mods) {
        Json item = Json::obj();
        item.set("id", Json::str(mod.id));
        item.set("name", Json::str(mod.name));
        item.set("version", Json::str(mod.version));
        item.set("hash", Json::str(mod.hash));
        item.set("source", Json::str(mod.source));
        item.set("enabled", Json::boolean(mod.enabled));
        mods.push(std::move(item));
    }
    body.set("mods", std::move(mods));
    auto result = supabase.client()->rpc("upsert_session_manifest", body);

    {
        std::lock_guard<std::mutex> lock(mu_);
        manifests_[session_id] = manifest;
        auto it = sessions_.find(session_id);
        if (it != sessions_.end()) {
            it->second.manifest = manifest;
            it->second.minecraft_version = manifest.minecraft_version;
            it->second.loader = manifest.loader;
            it->second.loader_version = manifest.loader_version;
        }
    }
    return result.success;
}

bool SessionManager::fetch_manifest(const std::string& session_id,
                                    const std::string& join_token,
                                    EssentialsManifest& out_manifest) {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    if (!supabase.is_authenticated()) return false;

    Json body;
    body.set("session_id", session_id);
    body.set("join_token", join_token);
    auto result = supabase.client()->rpc("get_session_manifest", body);
    if (!result.success || result.data.empty()) return false;

    const Json& row = result.data.front();
    EssentialsManifest manifest;
    manifest.session_id = row.get("session_id").as_str(session_id);
    manifest.host_user_id = row.get("host_user_id").as_str();
    manifest.profile_id = row.get("profile_id").as_str();
    manifest.profile_name = row.get("profile_name").as_str();
    manifest.minecraft_version = row.get("minecraft_version").as_str();
    manifest.loader = row.get("loader").as_str();
    manifest.loader_version = row.get("loader_version").as_str();
    for (const auto& item : row.get("mods").items()) {
        ModEntry mod;
        mod.name = item.get("name").as_str();
        mod.id = item.get("mod_id").as_str();
        mod.version = item.get("version").as_str();
        mod.hash = item.get("hash").as_str();
        mod.source = item.get("source").as_str();
        mod.enabled = item.get("enabled").as_bool(true);
        if (!mod.name.empty()) manifest.mods.push_back(std::move(mod));
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        manifests_[session_id] = manifest;
        if (sessions_.count(session_id)) sessions_[session_id].manifest = manifest;
    }
    out_manifest = std::move(manifest);
    return true;
}

EssentialsManifest SessionManager::get_manifest(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = manifests_.find(session_id);
    return it != manifests_.end() ? it->second : EssentialsManifest{};
}

// ---------------------------------------------------------------------------
// Notifications
// ---------------------------------------------------------------------------

std::vector<EssentialsNotification> SessionManager::get_notifications() {
    std::lock_guard<std::mutex> lock(mu_);
    return notifications_;
}

void SessionManager::clear_notification(const std::string& notification_id) {
    std::lock_guard<std::mutex> lock(mu_);
    notifications_.erase(
        std::remove_if(notifications_.begin(), notifications_.end(),
            [&](const EssentialsNotification& n) { return n.id == notification_id; }),
        notifications_.end());
}

void SessionManager::seed_fixture_notifications(
    const std::vector<EssentialsNotification>& notifications) {
    std::lock_guard<std::mutex> lock(mu_);
    notifications_ = notifications;
}

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

void SessionManager::on_session_state_changed(SessionStateChangedCallback cb) {
    std::lock_guard<std::mutex> lock(mu_);
    state_callbacks_.push_back(std::move(cb));
}

void SessionManager::on_player_joined(PlayerJoinedCallback cb) {
    std::lock_guard<std::mutex> lock(mu_);
    join_callbacks_.push_back(std::move(cb));
}

void SessionManager::on_player_left(PlayerLeftCallback cb) {
    std::lock_guard<std::mutex> lock(mu_);
    leave_callbacks_.push_back(std::move(cb));
}

}  // namespace aml::essentials
