#pragma once

#include <string>
#include <vector>
#include <map>
#include <functional>
#include <memory>
#include <mutex>
#include <atomic>
#include <thread>

#include "net.h"
#include "json.h"
#include "bedrock.h"

namespace aml::supabase {

struct SupabaseSecuritySettings;

struct TurnServer {
    std::vector<std::string> urls;
    std::string username;
    std::string credential;
};

struct TurnCredentials {
    std::vector<TurnServer> servers;
    int ttl = 0;
    std::string error;
    bool success = false;
};

struct EssentialsSignal {
    std::string id;
    std::string session_id;
    std::string sender_id;
    std::string recipient_id;
    std::string kind;
    std::string payload;
    int64_t created_at = 0;
};

// ---------------------------------------------------------------------------
// Supabase Configuration
// ---------------------------------------------------------------------------

struct SupabaseConfig {
    std::string project_url;
    std::string anon_key;
    std::string service_key;
    int timeout_seconds = 30;
};

// ---------------------------------------------------------------------------
// Supabase Client
// ---------------------------------------------------------------------------

class SupabaseClient {
public:
    SupabaseClient(const SupabaseConfig& config);
    ~SupabaseClient();
    
    // Initialize with project details
    bool initialize(const std::string& project_ref = "");
    
    // Authentication
    struct AuthUser {
        std::string id;
        std::string email;
        std::string phone;
        std::map<std::string, std::string> user_metadata;
        std::string access_token;
        std::string refresh_token;
        int64_t expires_at = 0;
        int64_t created_at = 0;
        int64_t updated_at = 0;
        int64_t last_login_at = 0;
    };
    
    struct AuthResponse {
        AuthResponse() = default;
        AuthResponse(bool ok, const std::string& message)
            : error(message), success(ok) {}
        AuthResponse(bool ok, const std::string& message,
                     const std::string& access, const std::string& refresh,
                     int64_t lifetime)
            : access_token(access), refresh_token(refresh), expires_in(lifetime),
              error(message), success(ok) {}
        AuthUser user;
        std::string access_token;
        std::string refresh_token;
        int64_t expires_in = 0;
        int64_t expires_at = 0;
        std::string error;
        bool success = false;
    };
    
    AuthResponse sign_up(const std::string& email, const std::string& password,
                         const std::map<std::string, std::string>& metadata = {});
    AuthResponse sign_in(const std::string& email, const std::string& password);
    AuthResponse sign_in_with_otp(const std::string& email);
    AuthResponse resend_signup_confirmation(const std::string& email);
    AuthResponse verify_otp(const std::string& email, const std::string& token,
                            const std::string& type = "signup");
    AuthResponse refresh_token(const std::string& refresh_token);
    bool sign_out(const std::string& access_token);
    AuthUser get_user(const std::string& access_token);
    AuthUser update_user(const std::string& access_token, 
                        const std::map<std::string, std::string>& updates);
    AuthResponse change_password(const std::string& current_password, const std::string& new_password);
    AuthResponse request_password_reset(const std::string& email);
    AuthResponse enable_2fa(const std::string& code);
    AuthResponse disable_2fa(const std::string& code);
    AuthResponse verify_2fa_code(const std::string& code);
    AuthResponse request_account_deletion();
    AuthResponse confirm_account_deletion(const std::string& confirmation_code);
    SupabaseSecuritySettings get_security_settings() const;
    
    // OAuth
    std::string get_oauth_url(const std::string& provider, const std::string& redirect_to);
    AuthResponse exchange_oauth_code(const std::string& provider, const std::string& code,
                                     const std::string& redirect_to);
    
    // Storage
    struct StorageUploadOptions {
        std::string bucket;
        std::string path;
        std::vector<uint8_t> data;
        std::string content_type;
        std::map<std::string, std::string> metadata;
        bool upsert = false;
    };
    
    struct StorageUploadResult {
        std::string id;
        std::string path;
        std::string full_path;
        std::string error;
        bool success = false;
    };
    
    struct StorageDownloadResult {
        std::vector<uint8_t> data;
        std::string error;
        bool success = false;
    };
    
    struct StorageListOptions {
        std::string bucket;
        std::string prefix;
        int limit = 100;
        int offset = 0;
        std::string sort_by;
        bool sort_order_asc = true;
    };
    
    struct StorageItem {
        std::string name;
        std::string id;
        std::string path;
        int64_t updated_at = 0;
        int64_t created_at = 0;
        uint64_t size = 0;
        std::string mime_type;
        std::map<std::string, std::string> metadata;
    };
    
    struct StorageListResult {
        std::vector<StorageItem> items;
        std::string error;
        bool success = false;
    };
    
    StorageUploadResult upload_file(const StorageUploadOptions& options);
    StorageDownloadResult download_file(const std::string& bucket, const std::string& path);
    StorageListResult list_files(const StorageListOptions& options);
    bool delete_file(const std::string& bucket, const std::string& path);
    bool delete_files(const std::string& bucket, const std::vector<std::string>& paths);
    
    // Database (PostgREST)
    struct DBQueryOptions {
        std::string table;
        std::string select = "*";
        std::map<std::string, std::string> eq_filters;
        std::map<std::string, std::string> gt_filters;
        std::map<std::string, std::string> lt_filters;
        std::map<std::string, std::string> like_filters;
        std::string order_by;
        bool order_asc = true;
        int limit = 100;
        int offset = 0;
    };
    
    struct DBInsertOptions {
        std::string table;
        std::vector<Json> records;
        bool upsert = false;
        std::string on_conflict;
    };
    
    struct DBUpdateOptions {
        std::string table;
        Json updates;
        std::map<std::string, std::string> eq_filters;
    };
    
    struct DBDeleteOptions {
        std::string table;
        std::map<std::string, std::string> eq_filters;
    };
    
    struct DBResult {
        std::vector<Json> data;
        int count = 0;
        std::string error;
        bool success = false;
    };
    
    DBResult select(const DBQueryOptions& options);
    DBResult insert(const DBInsertOptions& options);
    DBResult update(const DBUpdateOptions& options);
    DBResult remove(const DBDeleteOptions& options);
    // Invoke a PostgREST SQL function. Functions are used for moderation and
    // publishing so clients cannot bypass the approval state machine.
    DBResult rpc(const std::string& function_name, const Json::Value& args = Json::Value());
    
    // Realtime
    struct RealtimeChannel {
        std::string id;
        std::string topic;
        std::function<void(const Json::Value&)> callback;
    };
    
    RealtimeChannel subscribe(const std::string& table, 
                              const std::string& filter = "",
                              const std::function<void(const Json::Value&)>& callback = nullptr);
    bool unsubscribe(const std::string& channel_id);
    void unsubscribe_all();
    bool send(const std::string& channel_id, const Json::Value& data);
    
    // Edge Functions
    struct EdgeFunctionResult {
        Json::Value data;
        std::string error;
        bool success = false;
    };
    
    EdgeFunctionResult call_edge_function(const std::string& function_name,
                                           const Json::Value& body = Json::Value());
    TurnCredentials request_turn_credentials(int ttl_seconds = 3600);
    
    // Status
    bool is_authenticated() const;
    bool is_current_user_staff();
    const AuthUser& current_user() const;
    const std::string& current_access_token() const;
    const std::string& project_url() const { return config_.project_url; }
    void set_access_token(const std::string& access_token);
    void set_session(const AuthUser& user);
    
    // Events
    void on_auth_state_change(const std::function<void(bool, const AuthUser&)>& callback);
    void on_realtime_message(const std::function<void(const std::string&, const Json::Value&)>& callback);
    
private:
    SupabaseConfig config_;
    AuthUser current_user_;
    std::string current_access_token_;
    std::string current_refresh_token_;
    
    std::vector<std::function<void(bool, const AuthUser&)>> auth_callbacks_;
    std::vector<std::function<void(const std::string&, const Json::Value&)>> realtime_callbacks_;

    struct PollingSubscription {
        std::shared_ptr<std::atomic<bool>> stop;
        std::thread worker;
    };
    std::map<std::string, std::unique_ptr<PollingSubscription>> realtime_subscriptions_;
    
    mutable std::mutex mutex_;
    std::atomic<bool> authenticated_{false};
    
    // Internal helpers
    std::string make_auth_request(const std::string& endpoint,
                                  const Json::Value& body = Json::Value(),
                                  std::string* error = nullptr,
                                  const std::string& method = "POST") const;
    std::string make_storage_request(const std::string& method, const std::string& endpoint,
                                      const std::vector<uint8_t>& data = {},
                                      std::string* error = nullptr) const;
    std::string make_rest_request(const std::string& method, const std::string& endpoint,
                                  const Json::Value& body = Json::Value(),
                                  std::string* error = nullptr) const;
    std::string get_auth_header() const;
    std::string get_bearer_header() const;
    std::string url_encode(const std::string& value) const;
    Json::Value parse_response(const std::string& response, std::string* error = nullptr) const;
};

// ---------------------------------------------------------------------------
// Supabase Tables (Type-safe wrappers)
// ---------------------------------------------------------------------------

// Users table
struct SupabaseUser {
    std::string id;
    std::string email;
    std::string username;
    std::string display_name;
    std::string avatar_url;
    std::vector<std::string> roles;
    std::map<std::string, std::string> metadata;
    int64_t created_at = 0;
    int64_t updated_at = 0;
    int64_t last_login_at = 0;
};

// Security Settings
struct SupabaseSecuritySettings {
    bool two_factor_enabled = false;
    std::string two_factor_method; // email, auth_app, sms
    std::vector<std::string> trusted_devices;
    std::vector<std::string> recent_logins;
    int64_t last_password_change = 0;
};

// Servers table
struct SupabaseServer {
    std::string id;
    std::string user_id;
    std::string name;
    std::string alias;
    std::string address;
    std::string host;
    int port = 25565;
    std::string version;
    std::string type; // vanilla, paper, spigot, forge, fabric, etc.
    std::string motd;
    int max_players = 20;
    bool online = false;
    int ping_ms = 0;
    std::map<std::string, std::string> properties;
    int64_t created_at = 0;
    int64_t updated_at = 0;
};

// Profiles table
struct SupabaseProfile {
    std::string id;
    std::string user_id;
    std::string name;
    std::string minecraft_version;
    std::string loader;
    std::string loader_version;
    std::string java_path;
    int memory_mb = 2048;
    std::vector<std::string> mods;
    std::vector<std::string> resource_packs;
    std::vector<std::string> data_packs;
    std::map<std::string, std::string> settings;
    int64_t created_at = 0;
    int64_t updated_at = 0;
    int64_t last_played_at = 0;
};

// Storage items table
struct SupabaseStorageItem {
    std::string id;
    std::string user_id;
    std::string node_id;
    std::string path;
    std::string name;
    std::string type; // profile, world, backup, mod, config, screenshot, log
    uint64_t size_bytes = 0;
    std::string checksum;
    std::string mime_type;
    std::map<std::string, std::string> metadata;
    int64_t created_at = 0;
    int64_t updated_at = 0;
};

// Nodes table
struct SupabaseNode {
    std::string id;
    std::string user_id;
    std::string name;
    std::string host;
    int port = 0;
    std::string region;
    std::string zone;
    bool online = false;
    int64_t last_heartbeat = 0;
    uint64_t storage_capacity = 0;
    uint64_t storage_used = 0;
    uint64_t storage_available = 0;
    int cpu_cores = 0;
    uint64_t memory_total = 0;
    uint64_t memory_used = 0;
    std::vector<std::string> tags;
    std::map<std::string, std::string> metadata;
    int64_t created_at = 0;
    int64_t updated_at = 0;
};

// Social: Friendships
struct SupabaseFriendship {
    std::string id;
    std::string user_id_a;
    std::string user_id_b;
    std::string status; // "pending", "accepted", "blocked"
    // Optional profile fields returned by the social edge function. Keeping
    // them on the friendship row avoids a client-side query per friend.
    std::string friend_display_name;
    std::string friend_avatar_url;
    std::string friend_minecraft_username;
    int64_t created_at = 0;
    int64_t updated_at = 0;
};

// Social: Friend Requests
struct SupabaseFriendRequest {
    std::string id;
    std::string sender_id;
    std::string sender_username;
    std::string sender_avatar_url;
    std::string receiver_id;
    std::string status; // "pending", "accepted", "rejected"
    std::string message;
    int64_t created_at = 0;
};

// Social: Conversations (DM threads)
struct SupabaseConversation {
    std::string id;
    std::vector<std::string> participant_ids;
    std::string last_message_content;
    std::string last_message_sender_id;
    int64_t last_message_at = 0;
    int64_t created_at = 0;
    int unread_count = 0;
};

// Social: Messages
struct SupabaseMessage {
    std::string id;
    std::string conversation_id;
    std::string sender_id;
    std::string sender_username;
    std::string content;
    bool is_read = false;
    int64_t created_at = 0;
};

// Social: Parties
struct SupabaseParty {
    std::string id;
    std::string owner_id;
    std::string owner_username;
    std::string name;
    bool is_public = true;
    std::string invite_code;
    int max_members = 10;
    int member_count = 0;
    int64_t created_at = 0;
};

// Social: Party Members
struct SupabasePartyMember {
    std::string party_id;
    std::string user_id;
    std::string username;
    std::string role; // "owner", "admin", "member"
    int64_t joined_at = 0;
};

// Social: User Presence
struct SupabasePresence {
    std::string user_id;
    std::string status; // "online", "offline", "in_game", "away"
    std::string status_message;
    std::string current_server_id;
    int64_t last_seen_at = 0;
    int64_t updated_at = 0;
};

// Social: Public Profile
struct SupabasePublicProfile {
    std::string user_id;
    std::string display_name;
    std::string avatar_url;
    std::string bio;
    std::string minecraft_username;
    bool is_public = true;
    int friend_count = 0;
    int64_t play_time_seconds = 0;
    int64_t last_seen_at = 0;
};

// ---------------------------------------------------------------------------
// Subscription table (synced from Whop via backend)
// ---------------------------------------------------------------------------
struct SupabaseSubscription {
    std::string id;
    std::string user_id;
    std::string plan_id;           // "free" | "amalgam_plus" | "cloud_4" | "cloud_8" | "cloud_12"
    std::string plan_label;        // "FREE" | "AMALGAM+" | "Cloud 4" | ...
    std::string status;            // "active" | "cancelled" | "past_due" | "trialing"
    double amount = 0.0;           // monthly price in USD
    std::string currency = "USD";
    int64_t current_period_start = 0;
    int64_t current_period_end = 0;   // billing cycle end (Unix seconds)
    int64_t created_at = 0;
    int64_t cancelled_at = 0;
    std::string provider;          // "whop" | "stripe"
    std::string external_id;       // Whop subscription ID
    bool valid = false;             // row contains a usable subscription
    bool query_succeeded = false;   // table request completed, including no-row free state
};

// TURN usage table
struct SupabaseTurnUsage {
    std::string user_id;
    uint64_t used_bytes = 0;
    uint64_t monthly_bytes = 0;    // quota for the current cycle
    int64_t period_start = 0;
    int64_t period_end = 0;        // reset time (Unix seconds)
    bool valid = false;
    bool query_succeeded = false;   // distinguishes an empty free account from an outage
};

// ---------------------------------------------------------------------------
// Supabase Service Manager
// ---------------------------------------------------------------------------

class SupabaseManager {
public:
    static SupabaseManager& instance();
    
    // Initialize
    bool initialize(const std::string& project_url = "",
                   const std::string& anon_key = "",
                   const std::string& service_key = "");
    void shutdown();
    
    // Get client
    SupabaseClient* client();
    
    // Convenience methods
    SupabaseClient::AuthResponse sign_up(const std::string& email, const std::string& password,
                                          const std::map<std::string, std::string>& metadata = {});
    SupabaseClient::AuthResponse sign_in(const std::string& email, const std::string& password);
    SupabaseClient::AuthResponse resend_signup_confirmation(const std::string& email);
    bool sign_out();
    SupabaseUser get_current_user();
    
    // User management
    SupabaseClient::AuthResponse update_user(const std::map<std::string, std::string>& updates,
                                           const std::map<std::string, std::string>& metadata = {});
    SupabaseClient::AuthResponse change_password(const std::string& current_password, const std::string& new_password);
    SupabaseClient::AuthResponse request_password_reset(const std::string& email);
    SupabaseClient::AuthResponse refresh_token(const std::string& refresh_token);
    SupabaseClient::AuthResponse enable_2fa(const std::string& code);
    SupabaseClient::AuthResponse disable_2fa(const std::string& code);
    SupabaseClient::AuthResponse verify_2fa_code(const std::string& code);
    SupabaseClient::AuthResponse request_account_deletion();
    SupabaseClient::AuthResponse confirm_account_deletion(const std::string& confirmation_code);
    SupabaseSecuritySettings get_security_settings() const;
    
    // Server management
    std::vector<SupabaseServer> get_servers();
    SupabaseServer create_server(const SupabaseServer& server);
    bool update_server(const SupabaseServer& server);
    bool delete_server(const std::string& server_id);
    
    // Profile management
    std::vector<SupabaseProfile> get_profiles();
    SupabaseProfile create_profile(const SupabaseProfile& profile);
    bool update_profile(const SupabaseProfile& profile);
    bool delete_profile(const std::string& profile_id);
    
    // Storage
    SupabaseClient::StorageUploadResult upload_to_storage(
        const std::string& bucket, const std::string& path,
        const std::vector<uint8_t>& data, const std::string& content_type = "");
    SupabaseClient::StorageDownloadResult download_from_storage(
        const std::string& bucket, const std::string& path);
    std::vector<SupabaseStorageItem> list_storage_items(const std::string& type = "");
    
    // Node management
    std::vector<SupabaseNode> get_nodes();
    SupabaseNode register_node(const SupabaseNode& node);
    bool update_node(const SupabaseNode& node);
    bool deregister_node(const std::string& node_id);
    
    // Realtime
    void subscribe_to_servers(const std::function<void(const SupabaseServer&)>& callback);
    void subscribe_to_profiles(const std::function<void(const SupabaseProfile&)>& callback);
    void subscribe_to_bedrock_profiles(const std::function<void(const aml::bedrock::BedrockProfile&)>& callback);
    void subscribe_to_account_activity(const std::function<void(const std::string&)>& callback);
    void unsubscribe(const std::string& channel_id);
    void unsubscribe_all();
    
    // Status
    bool is_initialized() const;
    bool is_authenticated() const;
    std::string get_project_url() const;
    
    // Auto-login
    bool auto_login(const std::string& access_token, const std::string& refresh_token);

    bool publish_essentials_signal(const EssentialsSignal& signal);
    std::vector<EssentialsSignal> poll_essentials_signals(
        const std::string& session_id, const std::string& recipient_id,
        int limit = 100);
    bool remove_essentials_signal(const std::string& signal_id);
    
    // Subscriptions (synced from Whop by backend)
    SupabaseSubscription get_subscription();
    std::vector<SupabaseSubscription> get_all_subscriptions();
    SupabaseTurnUsage get_turn_usage();

    // Bedrock management
    std::vector<aml::bedrock::BedrockProfile> get_bedrock_profiles();
    aml::bedrock::BedrockProfile create_bedrock_profile(const aml::bedrock::BedrockProfile& profile);
    bool update_bedrock_profile(const aml::bedrock::BedrockProfile& profile);
    bool delete_bedrock_profile(const std::string& profile_id);
    
    // Social: Friends
    std::vector<SupabaseFriendship> get_friends();
    std::vector<SupabaseFriendRequest> get_friend_requests();
    bool send_friend_request(const std::string& receiver_id, const std::string& message = "");
    bool accept_friend_request(const std::string& request_id);
    bool reject_friend_request(const std::string& request_id);
    bool remove_friend(const std::string& friend_id);
    bool block_user(const std::string& user_id);
    bool unblock_user(const std::string& user_id);
    std::vector<std::string> get_blocked_users();
    std::vector<SupabasePublicProfile> search_users(const std::string& query, int limit = 20);
    
    // Social: Messaging
    std::vector<SupabaseConversation> get_conversations();
    std::vector<SupabaseMessage> get_messages(const std::string& conversation_id, int limit = 50, int offset = 0);
    SupabaseMessage send_message(const std::string& conversation_id, const std::string& content);
    bool mark_messages_read(const std::string& conversation_id);
    SupabaseConversation get_or_create_conversation(const std::string& other_user_id);
    
    // Social: Parties
    std::vector<SupabaseParty> get_parties();
    SupabaseParty create_party(const std::string& name, bool is_public = true, int max_members = 10);
    bool join_party(const std::string& invite_code);
    bool leave_party(const std::string& party_id);
    bool disband_party(const std::string& party_id);
    std::vector<SupabasePartyMember> get_party_members(const std::string& party_id);
    
    // Social: Presence
    bool update_presence(const std::string& status, const std::string& status_message = "", const std::string& server_id = "");
    SupabasePresence get_user_presence(const std::string& user_id);
    std::vector<SupabasePresence> get_friends_presence();
    
    // Social: Realtime
    void subscribe_to_messages(const std::function<void(const SupabaseMessage&)>& callback);
    void subscribe_to_presence(const std::function<void(const SupabasePresence&)>& callback);
    void subscribe_to_friend_requests(const std::function<void(const SupabaseFriendRequest&)>& callback);
    void subscribe_to_party_updates(const std::function<void(const std::string&, const std::string&)>& callback);

    // Data Collection: batch insert events, metrics, errors, feature usage
    bool insert_events(const Json::Value& events_json);
    bool insert_metrics(const Json::Value& metrics_json);
    bool insert_errors(const Json::Value& errors_json);
    bool insert_feature_usage(const Json::Value& usage_json);
    bool record_audit(const std::string& action, const std::string& resource_type,
                      const std::string& resource_id = "",
                      const Json::Value& changes = Json::Value(),
                      const Json::Value& metadata = Json::Value(),
                      const std::string& source = "launcher");

private:
    SupabaseManager();
    ~SupabaseManager();
    
    std::unique_ptr<SupabaseClient> client_;
    std::atomic<bool> initialized_{false};
    mutable std::mutex mutex_;
    
    // Prevent copying
    SupabaseManager(const SupabaseManager&) = delete;
    SupabaseManager& operator=(const SupabaseManager&) = delete;
};

}  // namespace aml::supabase
