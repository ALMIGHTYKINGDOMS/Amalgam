#pragma once

#include "server_types.h"
#include "supabase.h"

#include <string>
#include <vector>
#include <map>
#include <functional>
#include <mutex>
#include <thread>
#include <atomic>
#include <ctime>

namespace aml::sync { class SyncManager; }

namespace aml::servers {

// Forward declarations for Server types
struct Server;
struct ServerLog;
struct ServerBackup;
struct Node;
struct Cluster;
struct ServerStats;
struct NodeStats;

// ---------------------------------------------------------------------------
// Server Status
// ---------------------------------------------------------------------------

enum class ServerStatus {
    Stopped = 0,
    Starting = 1,
    Running = 2,
    Stopping = 3,
    Restarting = 4,
    Error = 5,
    Maintenance = 6
};

inline const char* server_status_name(ServerStatus status) {
    switch (status) {
        case ServerStatus::Stopped: return "Stopped";
        case ServerStatus::Starting: return "Starting";
        case ServerStatus::Running: return "Running";
        case ServerStatus::Stopping: return "Stopping";
        case ServerStatus::Restarting: return "Restarting";
        case ServerStatus::Error: return "Error";
        case ServerStatus::Maintenance: return "Maintenance";
        default: return "Unknown";
    }
}

// ---------------------------------------------------------------------------
// Node Status
// ---------------------------------------------------------------------------

enum class NodeStatus {
    Online = 0,
    Offline = 1,
    Maintenance = 2,
    Overloaded = 3
};

inline const char* node_status_name(NodeStatus status) {
    switch (status) {
        case NodeStatus::Online: return "Online";
        case NodeStatus::Offline: return "Offline";
        case NodeStatus::Maintenance: return "Maintenance";
        case NodeStatus::Overloaded: return "Overloaded";
        default: return "Unknown";
    }
}

// ---------------------------------------------------------------------------
// Cluster Status
// ---------------------------------------------------------------------------

enum class ClusterStatus {
    Active = 0,
    Degraded = 1,
    Offline = 2
};

inline const char* cluster_status_name(ClusterStatus status) {
    switch (status) {
        case ClusterStatus::Active: return "Active";
        case ClusterStatus::Degraded: return "Degraded";
        case ClusterStatus::Offline: return "Offline";
        default: return "Unknown";
    }
}

// ---------------------------------------------------------------------------
// Backup Status
// ---------------------------------------------------------------------------

enum class BackupStatus {
    Pending = 0,
    InProgress = 1,
    Completed = 2,
    Failed = 3,
    Restoring = 4,
    Restored = 5
};

inline const char* backup_status_name(BackupStatus status) {
    switch (status) {
        case BackupStatus::Pending: return "Pending";
        case BackupStatus::InProgress: return "In Progress";
        case BackupStatus::Completed: return "Completed";
        case BackupStatus::Failed: return "Failed";
        case BackupStatus::Restoring: return "Restoring";
        case BackupStatus::Restored: return "Restored";
        default: return "Unknown";
    }
}

// ---------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------

struct Server {
    std::string id;
    std::string user_id;
    std::string name;
    std::string host;
    int port = 25565;
    std::string version;
    std::string type; // vanilla, paper, spigot, fabric, forge, etc.
    std::string motd;
    int max_players = 20;
    int current_players = 0;
    bool online = false;
    int ping_ms = -1;
    
    // Node/Cluster assignment
    std::string node_id;
    std::string cluster_id;
    
    // Status and timestamps
    ServerStatus status = ServerStatus::Stopped;
    int64_t last_ping = 0;
    int64_t last_started = 0;
    int64_t created_at = 0;
    int64_t updated_at = 0;
    int64_t assigned_at = 0;
    
    // Additional metadata
    std::map<std::string, std::string> metadata;
};

// ---------------------------------------------------------------------------
// Server Log
// ---------------------------------------------------------------------------

struct ServerLog {
    std::string id;
    std::string server_id;
    int64_t timestamp = 0;
    std::string type; // info, warning, error, command
    std::string message;
    std::string source; // console, command, system
};

// ---------------------------------------------------------------------------
// Server Backup
// ---------------------------------------------------------------------------

struct ServerBackup {
    std::string id;
    std::string server_id;
    std::string label;
    int64_t timestamp = 0;
    uint64_t size_bytes = 0;
    std::string path;
    BackupStatus status = BackupStatus::Pending;
};

// ---------------------------------------------------------------------------
// Node
// ---------------------------------------------------------------------------

struct Node {
    std::string id;
    std::string user_id;
    std::string name;
    std::string host;
    int port = 22;
    std::string cluster_id;
    
    // Hardware specs
    int cpu_cores = 1;
    int memory_mb = 1024;
    int storage_gb = 50;
    
    // Resource usage
    double cpu_usage = 0.0;
    double memory_usage = 0.0;
    double storage_usage = 0.0;
    
    // Status and timestamps
    NodeStatus status = NodeStatus::Offline;
    int64_t registered_at = 0;
    int64_t last_heartbeat = 0;
    int64_t last_updated = 0;
    
    // Additional metadata
    std::map<std::string, std::string> metadata;
};

// ---------------------------------------------------------------------------
// Cluster
// ---------------------------------------------------------------------------

struct Cluster {
    std::string id;
    std::string user_id;
    std::string name;
    std::string description;
    std::vector<std::string> node_ids;
    
    // Load balancing
    std::string load_balancing_strategy = "round_robin";
    int max_servers_per_node = 10;
    
    // Status and timestamps
    ClusterStatus status = ClusterStatus::Active;
    int64_t created_at = 0;
    int64_t updated_at = 0;
    
    // Additional metadata
    std::map<std::string, std::string> metadata;
};

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

struct ServerStats {
    int total_servers = 0;
    int running = 0;
    int stopped = 0;
    int starting = 0;
    int stopping = 0;
    int restarting = 0;
    int error = 0;
    int online = 0;
    int total_players = 0;
    int max_players = 0;
};

struct NodeStats {
    int total_nodes = 0;
    int online = 0;
    int offline = 0;
    int maintenance = 0;
    int total_cpu = 0;
    int total_memory = 0;
    int total_storage = 0;
    double used_storage = 0.0;
};

// ---------------------------------------------------------------------------
// Server Manager
// ---------------------------------------------------------------------------

class ServerManager {
public:
    static ServerManager& instance();
    
    // Server CRUD
    std::vector<Server> get_servers() const;
    Server get_server(const std::string& server_id) const;
    Server create_server(const Server& server);
    bool update_server(const Server& server);
    bool delete_server(const std::string& server_id);
    
    // Server Actions
    bool start_server(const std::string& server_id);
    bool stop_server(const std::string& server_id);
    bool restart_server(const std::string& server_id);
    bool send_command(const std::string& server_id, const std::string& command);
    
    // Server Monitoring
    void start_monitoring();
    void stop_monitoring();
    void check_server_health();
    ServerStats get_stats() const;
    
    // Server Logs
    std::vector<ServerLog> get_logs(const std::string& server_id = "", int limit = 100) const;
    
    // Server Backups
    std::vector<ServerBackup> get_backups(const std::string& server_id) const;
    ServerBackup create_backup(const std::string& server_id, const std::string& label = "");
    bool restore_backup(const std::string& backup_id);
    bool delete_backup(const std::string& backup_id);
    
    // Node Management
    std::vector<Node> get_nodes() const;
    Node get_node(const std::string& node_id) const;
    Node get_node_by_name(const std::string& name) const;
    bool is_node_online(const std::string& node_id) const;
    Node register_node(const Node& node);
    bool update_node(const Node& node);
    bool deregister_node(const std::string& node_id);
    
    // Node Monitoring
    void start_node_monitoring();
    void stop_node_monitoring();
    void check_node_health();
    NodeStats get_node_stats() const;
    
    // Server-Node Assignment
    bool assign_server_to_node(const std::string& server_id, const std::string& node_id);
    bool migrate_server(const std::string& server_id, const std::string& new_node_id);
    
    // Cluster Management
    Cluster create_cluster(const Cluster& cluster);
    bool update_cluster(const Cluster& cluster);
    bool delete_cluster(const std::string& cluster_id);
    std::vector<Cluster> get_clusters() const;
    bool add_node_to_cluster(const std::string& cluster_id, const std::string& node_id);
    bool remove_node_from_cluster(const std::string& cluster_id, const std::string& node_id);
    
    // Real-time Subscriptions
    void subscribe_to_servers(const std::function<void(const Server&)>& callback);
    void subscribe_to_nodes(const std::function<void(const Node&)>& callback);
    void unsubscribe_all();
    
    // State
    bool is_monitoring() const { return monitoring_; }
    bool is_node_monitoring() const { return node_monitoring_; }

private:
    friend class aml::sync::SyncManager;
    ServerManager();
    ~ServerManager();
    
    // Prevent copying
    ServerManager(const ServerManager&) = delete;
    ServerManager& operator=(const ServerManager&) = delete;
    
    // ID Generation
    std::string generate_server_id() const;
    std::string generate_node_id() const;
    std::string generate_backup_id() const;
    std::string generate_cluster_id() const;
    
    // Data Persistence
    bool save_servers() const;
    bool load_servers();
    bool save_nodes() const;
    bool load_nodes();
    bool save_clusters() const;
    bool load_clusters();
    
    // Conversion Functions
    aml::supabase::SupabaseServer convert_to_supabase(const Server& server) const;
    Server convert_from_supabase(const aml::supabase::SupabaseServer& supa_server) const;
    aml::supabase::SupabaseNode convert_to_supabase_node(const Node& node) const;
    Node convert_from_supabase_node(const aml::supabase::SupabaseNode& supa_node) const;
    
    // State
    mutable std::mutex servers_mu_;
    mutable std::mutex nodes_mu_;
    mutable std::mutex clusters_mu_;
    mutable std::mutex logs_mu_;
    mutable std::mutex backups_mu_;
    
    std::vector<Server> servers_;
    std::vector<Node> nodes_;
    std::vector<Cluster> clusters_;
    std::vector<ServerLog> logs_;
    std::vector<ServerBackup> backups_;
    
    // Monitoring
    std::atomic<bool> monitoring_{false};
    std::atomic<bool> node_monitoring_{false};
    std::thread monitor_thread_;
    std::thread node_monitor_thread_;
    
    // Polling for real-time updates (fallback)
    std::atomic<bool> polling_active_{false};
    std::thread polling_thread_;
    std::vector<std::function<void(const Node&)>> polling_callbacks_;
};

}  // namespace aml::servers
