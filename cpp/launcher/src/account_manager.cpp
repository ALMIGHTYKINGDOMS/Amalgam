#include "account_manager.h"
#include "supabase.h"
#include "auth.h"
#include "net.h"
#include "json.h"
#include "config.h"
#include "instances.h"

#include <windows.h>
#include <wincrypt.h>
#include <filesystem>
#include <cstdio>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace aml::account {

namespace {

std::wstring session_file_path() {
    const std::wstring base = net::get_local_app_data_path();
    return base.empty() ? L"account-sessions.json"
                        : base + L"\\Amalgam\\account-sessions.json";
}

std::string hex_encode(const BYTE* data, DWORD size) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.resize(static_cast<size_t>(size) * 2);
    for (DWORD i = 0; i < size; ++i) {
        out[i * 2] = kHex[data[i] >> 4];
        out[i * 2 + 1] = kHex[data[i] & 0x0f];
    }
    return out;
}

bool hex_decode(const std::string& text, std::vector<BYTE>& out) {
    if (text.size() % 2 != 0) return false;
    auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    out.resize(text.size() / 2);
    for (size_t i = 0; i < out.size(); ++i) {
        const int high = digit(text[i * 2]);
        const int low = digit(text[i * 2 + 1]);
        if (high < 0 || low < 0) return false;
        out[i] = static_cast<BYTE>((high << 4) | low);
    }
    return true;
}

int64_t activity_timestamp(const Json& row) {
    for (const char* key : {"timestamp", "occurred_at", "created_at"}) {
        const int64_t value = row.get(key).as_int();
        if (value > 0) return value;
    }
    return 0;
}

AccountActivity activity_from_row(const Json& row) {
    AccountActivity activity;
    activity.id = row.get("id").as_str();
    activity.type = row.get("type").as_str();
    if (activity.type.empty()) activity.type = row.get("action").as_str();
    activity.description = row.get("description").as_str();
    if (activity.description.empty()) activity.description = row.get("message").as_str();
    if (activity.description.empty()) activity.description = activity.type;
    activity.timestamp = activity_timestamp(row);
    activity.ip_address = row.get("ip_address").as_str();
    activity.user_agent = row.get("user_agent").as_str();
    return activity;
}

}  // namespace

// ---------------------------------------------------------------------------
// Account Manager Implementation
// ---------------------------------------------------------------------------

AccountManager::AccountManager() {
    load_sessions();
    // Network session restoration happens after Supabase is initialized from
    // launcher.json in ui.cpp. The constructor only restores protected local data.
}

AccountManager::~AccountManager() {
    save_sessions();
}

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------

AccountManager& AccountManager::instance() {
    static AccountManager manager;
    return manager;
}

// ---------------------------------------------------------------------------
// Session Management
// ---------------------------------------------------------------------------

bool AccountManager::create_session(const std::string& email, const std::string& access_token, 
                                   const std::string& refresh_token, int64_t expires_at) {
    // Check if session already exists
    for (auto& session : sessions_) {
        if (session.email == email) {
            // Update existing session
            session.access_token = access_token;
            session.refresh_token = refresh_token;
            session.expires_at = expires_at;
            session.last_used = std::time(nullptr);
            current_session_id_ = session.id;
            save_sessions();
            return true;
        }
    }
    
    // Create new session
    AccountSession new_session;
    new_session.id = generate_session_id();
    new_session.email = email;
    new_session.access_token = access_token;
    new_session.refresh_token = refresh_token;
    new_session.expires_at = expires_at;
    new_session.created_at = std::time(nullptr);
    new_session.last_used = std::time(nullptr);
    
    sessions_.push_back(new_session);
    current_session_id_ = new_session.id;
    
    save_sessions();
    return true;
}

bool AccountManager::set_current_session(const std::string& session_id) {
    for (auto& session : sessions_) {
        if (session.id == session_id) {
            current_session_id_ = session_id;
            session.last_used = std::time(nullptr);
            save_sessions();
            return true;
        }
    }
    return false;
}

bool AccountManager::end_session(const std::string& session_id) {
    for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
        if (it->id == session_id) {
            if (current_session_id_ == session_id) {
                current_session_id_.clear();
            }
            sessions_.erase(it);
            save_sessions();
            return true;
        }
    }
    return false;
}

bool AccountManager::end_current_session() {
    if (current_session_id_.empty()) {
        return false;
    }
    return end_session(current_session_id_);
}

bool AccountManager::end_all_sessions() {
    sessions_.clear();
    current_session_id_.clear();
    save_sessions();
    return true;
}

AccountSession AccountManager::get_current_session() const {
    if (current_session_id_.empty()) {
        return AccountSession();
    }
    
    for (const auto& session : sessions_) {
        if (session.id == current_session_id_) {
            return session;
        }
    }
    return AccountSession();
}

std::vector<AccountSession> AccountManager::get_all_sessions() const {
    return sessions_;
}

bool AccountManager::has_valid_session() const {
    if (current_session_id_.empty()) {
        return false;
    }
    
    for (const auto& session : sessions_) {
        if (session.id == current_session_id_) {
            // Check if token is expired (with 5 minute buffer)
            if (session.expires_at - 300 > std::time(nullptr)) {
                return true;
            }
        }
    }
    return false;
}

bool AccountManager::refresh_current_session() {
    if (current_session_id_.empty()) {
        return false;
    }

    auto& supabase = aml::supabase::SupabaseManager::instance();
    
    auto session = get_current_session();
    
    if (session.refresh_token.empty()) {
        return false;
    }
    
    auto response = supabase.refresh_token(session.refresh_token);
    if (response.success) {
        session.access_token = response.access_token;
        session.refresh_token = response.refresh_token;
        session.expires_at = response.expires_at > 0
            ? response.expires_at
            : std::time(nullptr) + response.expires_in;
        
        // Update session in list
        for (auto& s : sessions_) {
            if (s.id == session.id) {
                s = session;
                break;
            }
        }
        
        save_sessions();
        return true;
    }
    
    return false;
}

// ---------------------------------------------------------------------------
// Account Profile Management
// ---------------------------------------------------------------------------

bool AccountManager::update_profile(const AccountProfile& profile) {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    
    auto current_user = supabase.get_current_user();
    if (current_user.id.empty()) {
        return false;
    }
    
    // Update in Supabase
    std::map<std::string, std::string> updates;
    if (!profile.display_name.empty()) {
        updates["display_name"] = profile.display_name;
    }
    if (!profile.bio.empty()) {
        updates["bio"] = profile.bio;
    }
    if (!profile.avatar_url.empty()) {
        updates["avatar_url"] = profile.avatar_url;
    }
    
    // Update preferences
    std::map<std::string, std::string> metadata = current_user.metadata;
    metadata["theme"] = profile.theme;
    metadata["language"] = profile.language;
    metadata["newsletter"] = profile.receive_newsletter ? "true" : "false";
    metadata["beta_features"] = profile.enable_beta_features ? "true" : "false";
    
    auto response = supabase.update_user(updates, metadata);
    return response.success;
}

AccountProfile AccountManager::get_profile() const {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    auto user = supabase.get_current_user();
    
    AccountProfile profile;
    profile.email = user.email;
    profile.username = user.username;
    profile.display_name = user.display_name;
    profile.avatar_url = user.avatar_url;
    profile.bio = user.metadata.count("bio") ? user.metadata.at("bio") : "";
    profile.theme = user.metadata.count("theme") ? user.metadata.at("theme") : "system";
    profile.language = user.metadata.count("language") ? user.metadata.at("language") : "en";
    profile.receive_newsletter = user.metadata.count("newsletter") && 
                                  user.metadata.at("newsletter") == "true";
    profile.enable_beta_features = user.metadata.count("beta_features") && 
                                    user.metadata.at("beta_features") == "true";
    profile.created_at = user.created_at;
    profile.last_login_at = user.last_login_at;
    
    return profile;
}

// ---------------------------------------------------------------------------
// Security Management
// ---------------------------------------------------------------------------

bool AccountManager::change_password(const std::string& current_password, const std::string& new_password) {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    auto response = supabase.change_password(current_password, new_password);
    return response.success;
}

bool AccountManager::request_password_reset(const std::string& email) {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    auto response = supabase.request_password_reset(email);
    return response.success;
}

bool AccountManager::enable_2fa(const std::string& code) {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    auto response = supabase.enable_2fa(code);
    return response.success;
}

bool AccountManager::disable_2fa(const std::string& code) {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    auto response = supabase.disable_2fa(code);
    return response.success;
}

bool AccountManager::verify_2fa_code(const std::string& code) {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    auto response = supabase.verify_2fa_code(code);
    return response.success;
}

aml::supabase::SupabaseSecuritySettings AccountManager::get_security_settings() const {
    return aml::supabase::SupabaseManager::instance().get_security_settings();
}

// ---------------------------------------------------------------------------
// Linked Accounts (Microsoft, etc.)
// ---------------------------------------------------------------------------

bool AccountManager::link_microsoft_account(const std::string& microsoft_token) {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    auto current_user = supabase.get_current_user();
    
    if (current_user.id.empty()) {
        return false;
    }
    
    if (microsoft_token.empty()) return false;
    microsoft_token_ = microsoft_token;

    // Store only the link state remotely. The Microsoft token remains local to
    // the current process and is never copied into user metadata.
    std::map<std::string, std::string> metadata = current_user.metadata;
    metadata["microsoft_linked"] = "true";
    metadata["microsoft_linked_at"] = std::to_string(std::time(nullptr));
    
    auto response = supabase.update_user({}, metadata);
    return response.success;
}

bool AccountManager::unlink_microsoft_account() {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    auto current_user = supabase.get_current_user();
    
    if (current_user.id.empty()) {
        return false;
    }
    
    // Remove Microsoft token from user metadata
    std::map<std::string, std::string> metadata = current_user.metadata;
    metadata.erase("microsoft_linked");
    metadata.erase("microsoft_linked_at");
    
    auto response = supabase.update_user({}, metadata);
    return response.success;
}

bool AccountManager::is_microsoft_linked() const {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    auto current_user = supabase.get_current_user();
    
    return !microsoft_token_.empty() && current_user.metadata.count("microsoft_linked") &&
           current_user.metadata.at("microsoft_linked") == "true";
}

std::string AccountManager::get_microsoft_token() const {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    auto current_user = supabase.get_current_user();
    
    return microsoft_token_;
}

// ---------------------------------------------------------------------------
// Session Persistence
// ---------------------------------------------------------------------------

std::string AccountManager::generate_session_id() const {
    // Generate a random session ID
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 15);
    
    const char* hex_chars = "0123456789abcdef";
    std::string session_id;
    
    for (int i = 0; i < 32; ++i) {
        session_id += hex_chars[dis(gen)];
    }
    
    return session_id;
}

bool AccountManager::save_sessions() const {
    Json root = Json::obj();
    root.set("current_session_id", Json::str(current_session_id_));
    Json sessions = Json::arr();
    for (const auto& session : sessions_) {
        Json item = Json::obj();
        item.set("id", Json::str(session.id));
        item.set("email", Json::str(session.email));
        item.set("access_token", Json::str(protect_data(session.access_token)));
        item.set("refresh_token", Json::str(protect_data(session.refresh_token)));
        item.set("expires_at", Json::num(static_cast<double>(session.expires_at)));
        item.set("created_at", Json::num(static_cast<double>(session.created_at)));
        item.set("last_used", Json::num(static_cast<double>(session.last_used)));
        sessions.push(std::move(item));
    }
    root.set("sessions", std::move(sessions));

    const std::wstring path = session_file_path();
    const std::filesystem::path parent = std::filesystem::path(path).parent_path();
    if (!parent.empty() && !net::mkdirs(parent.wstring())) return false;
    const std::wstring temp = path + L".tmp";
    std::string error;
    if (!json_write_file(temp, root, &error)) return false;
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp.c_str());
        return false;
    }
    return true;
}

bool AccountManager::load_sessions() {
    const std::wstring path = session_file_path();
    if (!net::file_exists(path)) return true;
    Json root;
    std::string error;
    if (!json_parse_file(path, root, &error) || !root.is(Json::Type::Obj)) return false;
    current_session_id_ = root.get("current_session_id").as_str();
    sessions_.clear();
    for (const Json& item : root.get("sessions").items()) {
        AccountSession session;
        session.id = item.get("id").as_str();
        session.email = item.get("email").as_str();
        session.access_token = unprotect_data(item.get("access_token").as_str());
        session.refresh_token = unprotect_data(item.get("refresh_token").as_str());
        session.expires_at = item.get("expires_at").as_int();
        session.created_at = item.get("created_at").as_int();
        session.last_used = item.get("last_used").as_int();
        if (!session.id.empty() && !session.access_token.empty()) sessions_.push_back(std::move(session));
    }
    return true;
}

std::string AccountManager::protect_data(const std::string& data) const {
    if (data.empty()) {
        return "";
    }
    
    DATA_BLOB input_blob = { static_cast<DWORD>(data.size()),
                             reinterpret_cast<BYTE*>(const_cast<char*>(data.data())) };
    DATA_BLOB output_blob = { 0, nullptr };
    
    if (CryptProtectData(&input_blob, L"Amalgam account token", nullptr, nullptr, nullptr, 0, &output_blob)) {
        std::string encrypted = "dpapi:v1:" + hex_encode(output_blob.pbData, output_blob.cbData);
        LocalFree(output_blob.pbData);
        return encrypted;
    }
    return "";
}

std::string AccountManager::unprotect_data(const std::string& data) const {
    if (data.empty()) {
        return "";
    }
    
    const std::string prefix = "dpapi:v1:";
    if (data.rfind(prefix, 0) != 0) return "";
    std::vector<BYTE> encoded;
    if (!hex_decode(data.substr(prefix.size()), encoded)) return "";
    DATA_BLOB input_blob = { static_cast<DWORD>(encoded.size()), encoded.data() };
    DATA_BLOB output_blob = { 0, nullptr };
    
    if (CryptUnprotectData(&input_blob, nullptr, nullptr, nullptr, nullptr, 0, &output_blob)) {
        std::string decrypted(reinterpret_cast<char*>(output_blob.pbData), output_blob.cbData);
        LocalFree(output_blob.pbData);
        return decrypted;
    }
    
    return "";
}

// ---------------------------------------------------------------------------
// Account Statistics and Usage
// ---------------------------------------------------------------------------

AccountStats AccountManager::get_account_stats() const {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    auto current_user = supabase.get_current_user();
    
    AccountStats stats;
    stats.total_sessions = static_cast<int>(sessions_.size());
    stats.current_session_id = current_session_id_;
    stats.account_created_at = current_user.created_at;
    stats.last_login_at = current_user.last_login_at;
    
    const std::wstring instances_dir = net::get_local_app_data_path() + L"\\instances";
    const auto instances = aml::instances::scan(instances_dir, nullptr);
    stats.total_instances = static_cast<int>(instances.size());
    for (const auto& instance : instances) {
        if (!instance.pack_source.empty() || !instance.pack_project.empty() ||
            !instance.pack_version.empty()) {
            ++stats.total_modpacks;
        }
    }

    return stats;
}

std::vector<AccountActivity> AccountManager::get_recent_activity() const {
    std::vector<AccountActivity> activities;
    auto& supabase = aml::supabase::SupabaseManager::instance();
    if (auto* client = supabase.client(); client && client->is_authenticated()) {
        const auto user = client->current_user();
        if (!user.id.empty()) {
            aml::supabase::SupabaseClient::DBQueryOptions query;
            query.table = "account_activity";
            query.eq_filters["user_id"] = user.id;
            query.order_by = "created_at";
            query.order_asc = false;
            query.limit = 20;
            const auto result = client->select(query);
            if (result.success) {
                for (const auto& row : result.data) {
                    AccountActivity activity = activity_from_row(row);
                    if (activity.timestamp > 0) activities.push_back(std::move(activity));
                }
            }
        }
    }

    // Local timestamps remain useful when the activity table is unavailable or empty.
    if (activities.empty()) {
        for (const auto& session : sessions_) {
            if (session.created_at > 0) {
                activities.push_back({"session-" + session.id + "-created", "login",
                                      "Signed in", session.created_at, {}, {}});
            }
            if (session.last_used > session.created_at) {
                activities.push_back({"session-" + session.id + "-used", "session",
                                      "Used this launcher session", session.last_used, {}, {}});
            }
        }
        const std::wstring instances_dir = net::get_local_app_data_path() + L"\\instances";
        for (const auto& instance : aml::instances::scan(instances_dir, nullptr)) {
            if (instance.last_played > 0) {
                activities.push_back({"instance-" + instance.id, "launch",
                                      "Played " + instance.name, instance.last_played, {}, {}});
            }
        }
    }
    std::sort(activities.begin(), activities.end(), [](const auto& a, const auto& b) {
        return a.timestamp > b.timestamp;
    });
    if (activities.size() > 20) activities.resize(20);
    return activities;
}

// ---------------------------------------------------------------------------
// Account Deletion
// ---------------------------------------------------------------------------

bool AccountManager::request_account_deletion() {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    auto response = supabase.request_account_deletion();
    
    if (response.success) {
        // End all local sessions
        end_all_sessions();
    }
    
    return response.success;
}

bool AccountManager::confirm_account_deletion(const std::string& confirmation_code) {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    auto response = supabase.confirm_account_deletion(confirmation_code);
    
    if (response.success) {
        // End all local sessions
        end_all_sessions();
    }
    
    return response.success;
}

}  // namespace aml::account
