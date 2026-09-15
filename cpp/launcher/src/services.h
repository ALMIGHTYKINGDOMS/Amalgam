#pragma once

#include <string>
#include <vector>
#include <map>
#include <functional>
#include <memory>
#include <mutex>
#include <atomic>
#include <Windows.h>

#include "net.h"

namespace aml::services {

// ---------------------------------------------------------------------------
// Service Types
// ---------------------------------------------------------------------------

enum class ServiceType {
    Authentication,
    Storage,
    ServerManagement,
    Analytics,
    Social,
    ContentDelivery
};

// ---------------------------------------------------------------------------
// Service Configuration
// ---------------------------------------------------------------------------

struct ServiceConfig {
    std::string name;
    std::string endpoint;
    std::string api_key;
    std::string secret;
    bool enabled = true;
    int timeout_seconds = 30;
    int retry_count = 3;
};

// ---------------------------------------------------------------------------
// Authentication Service
// ---------------------------------------------------------------------------

struct UserProfile {
    std::string id;
    std::string username;
    std::string email;
    std::string display_name;
    std::string avatar_url;
    std::vector<std::string> roles;
    std::vector<std::string> permissions;
    std::string access_token;
    std::string refresh_token;
    int64_t token_expires_at = 0;
    int64_t created_at = 0;
    int64_t last_login_at = 0;
    bool email_verified = false;
    bool two_factor_enabled = false;
};

struct RegistrationRequest {
    std::string username;
    std::string email;
    std::string password;
    std::string display_name;
    std::string invite_code;
};

struct LoginRequest {
    std::string username_or_email;
    std::string password;
    bool remember_me = false;
};

struct AuthResponse {
    UserProfile user;
    std::string access_token;
    std::string refresh_token;
    int64_t expires_in = 0;
    std::string error;
    bool success = false;
};

struct TokenRefreshResponse {
    std::string access_token;
    std::string refresh_token;
    int64_t expires_in = 0;
    std::string error;
    bool success = false;
};

class AuthService {
public:
    AuthService(const ServiceConfig& config);
    ~AuthService();
    
    // Authentication
    AuthResponse login(const LoginRequest& request);
    AuthResponse register_user(const RegistrationRequest& request);
    TokenRefreshResponse refresh_token(const std::string& refresh_token);
    bool logout(const std::string& access_token);
    
    // User management
    UserProfile get_user_profile(const std::string& access_token);
    bool update_user_profile(const std::string& access_token, const UserProfile& updates);
    bool change_password(const std::string& access_token, 
                        const std::string& current_password, 
                        const std::string& new_password);
    
    // Session management
    bool validate_token(const std::string& access_token);
    bool invalidate_token(const std::string& access_token);
    bool invalidate_all_tokens(const std::string& user_id);
    
    // Two-factor authentication
    bool setup_2fa(const std::string& access_token, std::string* secret_out, std::string* qr_code_out);
    bool verify_2fa(const std::string& access_token, const std::string& code);
    bool disable_2fa(const std::string& access_token, const std::string& code);
    
    // Password reset
    bool request_password_reset(const std::string& email_or_username);
    bool verify_password_reset_token(const std::string& token);
    bool complete_password_reset(const std::string& token, const std::string& new_password);
    
    // Social authentication
    std::string get_oauth_url(const std::string& provider, const std::string& redirect_uri);
    AuthResponse exchange_oauth_code(const std::string& provider, const std::string& code, 
                                     const std::string& redirect_uri);
    
    // Status
    bool is_authenticated() const;
    const UserProfile& current_user() const;
    const std::string& current_access_token() const;
    
    // Events
    void on_auth_success(const std::function<void(const UserProfile&)>& callback);
    void on_auth_failure(const std::function<void(const std::string&)>& callback);
    void on_logout(const std::function<void()>& callback);
    
private:
    ServiceConfig config_;
    UserProfile current_user_;
    std::string current_access_token_;
    std::string current_refresh_token_;
    
    std::vector<std::function<void(const UserProfile&)>> auth_success_callbacks_;
    std::vector<std::function<void(const std::string&)>> auth_failure_callbacks_;
    std::vector<std::function<void()>> logout_callbacks_;
    
    mutable std::mutex mutex_;
    std::atomic<bool> authenticated_{false};
    
    // Internal helpers
    std::string make_request(const std::string& method, const std::string& endpoint,
                            const std::string& body = "", std::string* error = nullptr);
    std::string build_auth_header() const;
    bool store_tokens(const std::string& access_token, const std::string& refresh_token, int64_t expires_in);
    void clear_tokens();
};

// ---------------------------------------------------------------------------
// Storage Service
// ---------------------------------------------------------------------------

enum class StorageType {
    Profile,
    World,
    Backup,
    Mod,
    Config,
    Screenshot,
    Log,
    Cache
};

struct StorageItem {
    std::string id;
    std::string name;
    StorageType type;
    std::string path;
    std::string url;
    uint64_t size_bytes = 0;
    std::string mime_type;
    std::string checksum;
    int64_t created_at = 0;
    int64_t updated_at = 0;
    int64_t accessed_at = 0;
    std::map<std::string, std::string> metadata;
};

struct UploadRequest {
    StorageType type;
    std::string name;
    std::string path; // Local path to upload
    std::map<std::string, std::string> metadata;
    std::function<void(float)> progress_callback;
};

struct UploadResult {
    StorageItem item;
    std::string error;
    bool success = false;
};

struct DownloadRequest {
    std::string item_id;
    std::string target_path; // Where to save
    std::function<void(float)> progress_callback;
};

struct DownloadResult {
    StorageItem item;
    std::string error;
    bool success = false;
};

struct ListRequest {
    StorageType type;
    std::string parent_id;
    int limit = 100;
    int offset = 0;
    std::string sort_by;
    bool sort_descending = false;
};

struct ListResult {
    std::vector<StorageItem> items;
    int total_count = 0;
    int offset = 0;
    int limit = 0;
    std::string error;
    bool success = false;
};

class StorageService {
public:
    StorageService(const ServiceConfig& config);
    ~StorageService();
    
    // Upload
    UploadResult upload(const UploadRequest& request);
    UploadResult upload_async(const UploadRequest& request, 
                            const std::function<void(const UploadResult&)>& callback);
    
    // Download
    DownloadResult download(const DownloadRequest& request);
    DownloadResult download_async(const DownloadRequest& request, 
                                  const std::function<void(const DownloadResult&)>& callback);
    
    // List
    ListResult list(const ListRequest& request);
    
    // Manage
    bool delete_item(const std::string& item_id, std::string* error = nullptr);
    bool rename_item(const std::string& item_id, const std::string& new_name, std::string* error = nullptr);
    bool move_item(const std::string& item_id, const std::string& new_parent_id, std::string* error = nullptr);
    
    // Metadata
    bool get_metadata(const std::string& item_id, std::map<std::string, std::string>* metadata, std::string* error = nullptr);
    bool update_metadata(const std::string& item_id, const std::map<std::string, std::string>& metadata, std::string* error = nullptr);
    
    // Quota
    struct QuotaInfo {
        uint64_t total_bytes = 0;
        uint64_t used_bytes = 0;
        uint64_t available_bytes = 0;
        int max_items = 0;
        int current_items = 0;
    };
    
    QuotaInfo get_quota(std::string* error = nullptr);
    
    // Local cache
    std::string get_local_cache_path(StorageType type) const;
    bool clear_local_cache(StorageType type, std::string* error = nullptr);
    
private:
    ServiceConfig config_;
    mutable std::mutex mutex_;
    
    std::string local_cache_dir_;
    
    // Internal helpers
    std::string make_request(const std::string& method, const std::string& endpoint,
                            const std::string& body = "", std::string* error = nullptr);
    std::string build_auth_header() const;
    std::string get_storage_type_path(StorageType type) const;
};

// ---------------------------------------------------------------------------
// Server Management Service
// ---------------------------------------------------------------------------

struct ServerInfo {
    std::string id;
    std::string name;
    std::string alias;
    std::string address;
    std::string host;
    int port = 25565;
    std::string version;
    std::string type; // vanilla, paper, spigot, forge, fabric, etc.
    std::string motd;
    int max_players = 20;
    int online_players = 0;
    std::vector<std::string> player_list;
    bool online = false;
    int64_t last_ping = 0;
    int64_t created_at = 0;
    int ping_ms = 0;
    std::string icon_url;
    std::map<std::string, std::string> properties;
    HANDLE process_handle = nullptr;
    DWORD process_id = 0;
    // V2 fields
    int64_t uptime_s = 0;
    std::string java_version;
    int allocated_ram_mb = 0;
    float cpu_percent = 0.0f;
    float ram_percent = 0.0f;
    float tps = 20.0f;
    std::string local_address;
    std::string public_address;
    bool lan_reachable = false;
    std::string world_name;
    std::string game_mode;
    std::string difficulty;
    std::string seed;
    uint64_t world_size_bytes = 0;
    bool auto_restart = true;
    std::string deployment; // "local" | "cloud"
    std::string platform;  // "Java Edition" | "Bedrock"
    // sparkline history (last 60 samples)
    std::vector<float> cpu_history;
    std::vector<float> ram_history;
    std::vector<float> tps_history;
};

struct ServerCreateRequest {
    std::string name;
    std::string alias;
    std::string version;
    std::string type;
    int max_players = 20;
    std::string motd;
    bool enable_whitelist = false;
    bool online_mode = true;
    bool allow_flight = false;
    bool allow_nether = true;
    std::string world_name = "world";
    std::string seed;
    std::string game_mode = "survival";
    bool enable_command_blocks = false;
    std::vector<std::string> plugins;
    std::vector<std::string> mods;
    bool eula_accepted = false;
};

struct ServerControlRequest {
    std::string server_id;
    std::string action; // start, stop, restart, suspend, resume
    std::map<std::string, std::string> parameters;
};

struct ServerControlResult {
    bool success = false;
    std::string message;
    std::string error;
    int exit_code = 0;
};

struct ServerConsoleEntry {
    int64_t timestamp = 0;
    std::string level; // info, warning, error
    std::string source;
    std::string message;
};

class ServerManager {
public:
    ServerManager(const ServiceConfig& config);
    ~ServerManager();
    
    // Server lifecycle
    ServerInfo create_server(const ServerCreateRequest& request, std::string* error = nullptr);
    bool delete_server(const std::string& server_id, std::string* error = nullptr);
    bool update_server(const std::string& server_id, const ServerCreateRequest& updates, std::string* error = nullptr);
    
    // Server control
    ServerControlResult control_server(const ServerControlRequest& request);
    ServerControlResult start_server(const std::string& server_id);
    ServerControlResult stop_server(const std::string& server_id);
    ServerControlResult restart_server(const std::string& server_id);
    
    // Server status
    ServerInfo get_server_info(const std::string& server_id, std::string* error = nullptr);
    std::vector<ServerInfo> list_servers(std::string* error = nullptr);
    bool ping_server(const std::string& server_id, ServerInfo* info = nullptr);
    
    // Server console
    std::vector<ServerConsoleEntry> get_console_logs(const std::string& server_id, 
                                                    int limit = 100, 
                                                    std::string* error = nullptr);
    bool send_command(const std::string& server_id, const std::string& command, 
                     std::string* response = nullptr, std::string* error = nullptr);
    
    // Server files
    bool upload_server_file(const std::string& server_id, const std::string& path, 
                           const std::vector<uint8_t>& content, std::string* error = nullptr);
    bool download_server_file(const std::string& server_id, const std::string& path, 
                             std::vector<uint8_t>* content, std::string* error = nullptr);
    bool delete_server_file(const std::string& server_id, const std::string& path, std::string* error = nullptr);
    
    // Server backups
    bool create_backup(const std::string& server_id, const std::string& name, 
                      std::string* backup_id = nullptr, std::string* error = nullptr);
    bool restore_backup(const std::string& server_id, const std::string& backup_id, 
                       std::string* error = nullptr);
    std::vector<std::map<std::string, std::string>> list_backups(const std::string& server_id, 
                                                               std::string* error = nullptr);
    
    // Server templates
    std::vector<std::map<std::string, std::string>> list_templates(std::string* error = nullptr);
    ServerCreateRequest get_template(const std::string& template_id, std::string* error = nullptr);
    
    // Local server management
    bool start_local_server(const std::string& server_id, const std::string& java_path, 
                           int ram_mb, std::string* error = nullptr);
    bool stop_local_server(const std::string& server_id, std::string* error = nullptr);

    // The supervised process is the only owner of local run state. A stage
    // persisted elsewhere is a hint that must be reconciled against this.
    bool is_local_server_running(const std::string& server_id) const;
    
    // Events
    void on_server_started(const std::function<void(const std::string&)>& callback);
    void on_server_stopped(const std::function<void(const std::string&)>& callback);
    void on_server_output(const std::function<void(const std::string&, const std::string&)>& callback);
    
private:
    struct LocalServerTransport;
    ServiceConfig config_;
    mutable std::mutex mutex_;
    
    std::vector<std::function<void(const std::string&)>> server_started_callbacks_;
    std::vector<std::function<void(const std::string&)>> server_stopped_callbacks_;
    std::vector<std::function<void(const std::string&, const std::string&)>> server_output_callbacks_;
    
    std::map<std::string, ServerInfo> running_servers_;
    std::map<std::string, std::unique_ptr<LocalServerTransport>> local_transports_;
    
    // Internal helpers
    std::string make_request(const std::string& method, const std::string& endpoint,
                            const std::string& body = "", std::string* error = nullptr);
    std::string build_auth_header() const;
    std::string get_server_path(const std::string& server_id) const;

    // Releases everything the supervisor holds for a server: its console
    // transport, the reader thread and the process handle. The caller stops the
    // process first; this only forgets it.
    void forget_local_server(const std::string& server_id);
};

// ---------------------------------------------------------------------------
// Node/Cluster Service for Distributed Storage
// ---------------------------------------------------------------------------

struct NodeInfo {
    std::string id;
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
};

struct ClusterInfo {
    std::string id;
    std::string name;
    std::vector<NodeInfo> nodes;
    int64_t created_at = 0;
    std::string status;
    std::map<std::string, std::string> metadata;
};

struct StorageNode {
    std::string node_id;
    std::string path;
    StorageType type;
    uint64_t size_bytes = 0;
    std::string checksum;
    int64_t created_at = 0;
    std::map<std::string, std::string> metadata;
};

class NodeService {
public:
    NodeService(const ServiceConfig& config);
    ~NodeService();
    
    // Node management
    NodeInfo register_node(const NodeInfo& node, std::string* error = nullptr);
    bool deregister_node(const std::string& node_id, std::string* error = nullptr);
    NodeInfo get_node(const std::string& node_id, std::string* error = nullptr);
    std::vector<NodeInfo> list_nodes(const std::string& region = "", 
                                     const std::vector<std::string>& tags = {}, 
                                     std::string* error = nullptr);
    bool update_node(const std::string& node_id, const NodeInfo& updates, std::string* error = nullptr);
    
    // Node health
    bool ping_node(const std::string& node_id, int* latency_ms = nullptr, std::string* error = nullptr);
    NodeInfo get_node_health(const std::string& node_id, std::string* error = nullptr);
    
    // Cluster management
    ClusterInfo create_cluster(const std::string& name, const std::vector<std::string>& node_ids, 
                              std::string* error = nullptr);
    bool delete_cluster(const std::string& cluster_id, std::string* error = nullptr);
    ClusterInfo get_cluster(const std::string& cluster_id, std::string* error = nullptr);
    std::vector<ClusterInfo> list_clusters(std::string* error = nullptr);
    bool add_node_to_cluster(const std::string& cluster_id, const std::string& node_id, 
                            std::string* error = nullptr);
    bool remove_node_from_cluster(const std::string& cluster_id, const std::string& node_id, 
                                  std::string* error = nullptr);
    
    // Distributed storage
    bool store_on_node(const std::string& node_id, const StorageNode& item, 
                      const std::vector<uint8_t>& data, std::string* error = nullptr);
    bool retrieve_from_node(const std::string& node_id, const std::string& path, 
                           std::vector<uint8_t>* data, std::string* error = nullptr);
    bool delete_from_node(const std::string& node_id, const std::string& path, std::string* error = nullptr);
    
    // Replication
    bool replicate_to_cluster(const std::string& cluster_id, const StorageNode& item, 
                             const std::vector<uint8_t>& data, int replication_factor = 2, 
                             std::string* error = nullptr);
    std::vector<StorageNode> find_replicas(const std::string& item_id, std::string* error = nullptr);
    
    // Load balancing
    std::string get_best_node(StorageType type, uint64_t size_bytes, 
                             const std::vector<std::string>& preferred_regions = {}, 
                             std::string* error = nullptr);
    
    // Monitoring
    struct NodeMetrics {
        std::string node_id;
        int64_t timestamp = 0;
        double cpu_usage = 0.0;
        double memory_usage = 0.0;
        double storage_usage = 0.0;
        int active_connections = 0;
        int requests_per_second = 0;
        int errors_per_second = 0;
        std::map<std::string, double> custom_metrics;
    };
    
    NodeMetrics get_node_metrics(const std::string& node_id, std::string* error = nullptr);
    std::vector<NodeMetrics> get_cluster_metrics(const std::string& cluster_id, std::string* error = nullptr);
    
    // Events
    void on_node_registered(const std::function<void(const NodeInfo&)>& callback);
    void on_node_deregistered(const std::function<void(const std::string&)>& callback);
    void on_node_status_changed(const std::function<void(const NodeInfo&)>& callback);
    
private:
    ServiceConfig config_;
    mutable std::mutex mutex_;
    
    std::vector<std::function<void(const NodeInfo&)>> node_registered_callbacks_;
    std::vector<std::function<void(const std::string&)>> node_deregistered_callbacks_;
    std::vector<std::function<void(const NodeInfo&)>> node_status_changed_callbacks_;
    
    // Internal helpers
    std::string make_request(const std::string& method, const std::string& endpoint,
                            const std::string& body = "", std::string* error = nullptr);
    std::string build_auth_header() const;
};

// ---------------------------------------------------------------------------
// Content Delivery Service
// ---------------------------------------------------------------------------

struct CDNConfig {
    std::string base_url;
    std::string endpoint;
    std::string distribution_id;
    std::vector<std::string> origins;
    bool enabled = true;
    int ttl_seconds = 3600;
};

struct CDNUploadRequest {
    std::string path;
    std::vector<uint8_t> content;
    std::string content_type;
    std::map<std::string, std::string> headers;
    bool public_read = false;
};

struct CDNUploadResult {
    std::string url;
    std::string etag;
    int64_t size_bytes = 0;
    std::string error;
    bool success = false;
};

class CDNService {
public:
    CDNService(const CDNConfig& config);
    ~CDNService();
    
    CDNUploadResult upload(const CDNUploadRequest& request);
    bool invalidate(const std::string& path, std::string* error = nullptr);
    bool purge_cache(const std::vector<std::string>& paths, std::string* error = nullptr);
    
    std::string get_url(const std::string& path) const;
    
private:
    CDNConfig config_;
    mutable std::mutex mutex_;
};

// ---------------------------------------------------------------------------
// Service Manager (Singleton)
// ---------------------------------------------------------------------------

class ServiceManager {
public:
    static ServiceManager& instance();
    
    // Initialize services
    void initialize(const std::map<ServiceType, ServiceConfig>& configs);
    void shutdown();
    
    // Get services
    AuthService* auth() { return auth_service_.get(); }
    StorageService* storage() { return storage_service_.get(); }
    ServerManager* servers() { return server_manager_.get(); }
    NodeService* nodes() { return node_service_.get(); }
    CDNService* cdn() { return cdn_service_.get(); }
    
    // Configure individual services
    void configure_auth(const ServiceConfig& config);
    void configure_storage(const ServiceConfig& config);
    void configure_servers(const ServiceConfig& config);
    void configure_nodes(const ServiceConfig& config);
    void configure_cdn(const CDNConfig& config);
    
    // Status
    bool is_initialized() const { return initialized_; }
    std::map<ServiceType, bool> service_status() const;
    
private:
    ServiceManager();
    ~ServiceManager();
    
    std::unique_ptr<AuthService> auth_service_;
    std::unique_ptr<StorageService> storage_service_;
    std::unique_ptr<ServerManager> server_manager_;
    std::unique_ptr<NodeService> node_service_;
    std::unique_ptr<CDNService> cdn_service_;
    
    std::atomic<bool> initialized_{false};
    mutable std::mutex mutex_;
    
    // Prevent copying
    ServiceManager(const ServiceManager&) = delete;
    ServiceManager& operator=(const ServiceManager&) = delete;
};

// The launcher's single local server supervisor. ServiceManager::servers()
// returns null until something initializes it, and a null manager turns every
// local server action (start, stop, console, backups) into a silent no-op, so
// consumers ask for the manager through here instead of racing to construct it.
// Called from the UI thread before the Servers page resolves any run state.
ServerManager* local_server_manager();

}  // namespace aml::services
