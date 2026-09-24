#include "server_manager.h"
#include "supabase.h"
#include "server_types.h"
#include "services.h"
#include "net.h"
#include "json.h"

#include <windows.h>
#include <shellapi.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <random>
#include <chrono>

#include <filesystem>

#pragma comment(lib, "ws2_32.lib")

namespace aml::servers {

// Include the Server struct definition from server_manager.h
// (It's defined in the header, so we don't need to redefine it here)

static uint64_t calculate_directory_size(const std::wstring& dir) {
    uint64_t total = 0;
    std::error_code ec;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, ec)) {
        if (entry.is_regular_file(ec)) {
            total += entry.file_size(ec);
        }
    }
    return total;
}

// ---------------------------------------------------------------------------
// Server Manager Implementation
// ---------------------------------------------------------------------------

ServerManager::ServerManager() {
    load_servers();
    load_nodes();
    load_clusters();
    start_monitoring();
    start_node_monitoring();
}

ServerManager::~ServerManager() {
    stop_node_monitoring();
    stop_monitoring();
    save_servers();
    save_nodes();
    save_clusters();
}

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------

ServerManager& ServerManager::instance() {
    static ServerManager manager;
    return manager;
}

// ---------------------------------------------------------------------------
// Server CRUD Operations
// ---------------------------------------------------------------------------

std::vector<Server> ServerManager::get_servers() const {
    std::lock_guard<std::mutex> lock(servers_mu_);
    return servers_;
}

Server ServerManager::get_server(const std::string& server_id) const {
    std::lock_guard<std::mutex> lock(servers_mu_);
    for (const auto& server : servers_) {
        if (server.id == server_id) {
            return server;
        }
    }
    return Server();
}

Server ServerManager::create_server(const Server& server) {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    
    if (!supabase.is_authenticated()) {
        return Server();
    }
    
    // Generate ID if not provided
    Server new_server = server;
    if (new_server.id.empty()) {
        new_server.id = generate_server_id();
    }
    
    // Set default values
    new_server.user_id = supabase.get_current_user().id;
    new_server.created_at = std::time(nullptr);
    new_server.updated_at = new_server.created_at;
    new_server.status = ServerStatus::Stopped;
    new_server.last_ping = 0;
    new_server.ping_ms = -1;
    new_server.online = false;
    
    // Create in Supabase
    auto created = supabase.create_server(convert_to_supabase(new_server));
    if (created.id.empty()) {
        return Server();
    }
    
    // Add to local cache
    {
        std::lock_guard<std::mutex> lock(servers_mu_);
        servers_.push_back(new_server);
    }
    
    save_servers();
    return new_server;
}

bool ServerManager::update_server(const Server& server) {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    
    if (!supabase.is_authenticated()) {
        return false;
    }
    
    // Update in Supabase
    bool success = supabase.update_server(convert_to_supabase(server));
    if (!success) {
        return false;
    }
    
    // Update local cache
    {
        std::lock_guard<std::mutex> lock(servers_mu_);
        for (auto& s : servers_) {
            if (s.id == server.id) {
                s = server;
                s.updated_at = std::time(nullptr);
                break;
            }
        }
    }
    
    save_servers();
    return true;
}

bool ServerManager::delete_server(const std::string& server_id) {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    
    if (!supabase.is_authenticated()) {
        return false;
    }
    
    // Delete from Supabase
    bool success = supabase.delete_server(server_id);
    if (!success) {
        return false;
    }
    
    // Remove from local cache
    {
        std::lock_guard<std::mutex> lock(servers_mu_);
        servers_.erase(std::remove_if(servers_.begin(), servers_.end(),
            [&server_id](const Server& s) { return s.id == server_id; }), servers_.end());
    }
    
    save_servers();
    return true;
}

// ---------------------------------------------------------------------------
// Server Actions
// ---------------------------------------------------------------------------

bool ServerManager::start_server(const std::string& server_id) {
    Server server = get_server(server_id);
    if (server.id.empty()) {
        return false;
    }

    // A missing local service cannot start a server. Fail before publishing a
    // successful asynchronous start request.
    if (!aml::services::ServiceManager::instance().servers()) {
        return false;
    }
    
    server.status = ServerStatus::Starting;
    server.last_started = std::time(nullptr);
    
    if (!update_server(server)) {
        return false;
    }
    
    // This method is currently reached only through the Admin mutation worker.
    // Complete the local lifecycle operation on that owned worker rather than
    // spawning an unowned detached tail that could outlive the manager or its
    // authenticated session.
    std::string error;
    auto& svc = aml::services::ServiceManager::instance();
    auto* server_service = svc.servers();
    const bool ok = server_service &&
        server_service->start_local_server(server_id, "", 2048, &error);

    Server updated = get_server(server_id);
    if (updated.id.empty()) return false;
    if (ok) {
        // Process creation is not a health check. Keep the server in a
        // transitional state until check_server_health verifies it.
        updated.status = ServerStatus::Starting;
        updated.online = false;
        updated.ping_ms = -1;
    } else {
        updated.status = ServerStatus::Error;
        updated.online = false;
        updated.ping_ms = -1;
    }
    return update_server(updated) && ok;
}

bool ServerManager::stop_server(const std::string& server_id) {
    Server server = get_server(server_id);
    if (server.id.empty()) {
        return false;
    }

    if (!aml::services::ServiceManager::instance().servers()) {
        return false;
    }
    
    server.status = ServerStatus::Stopping;
    
    if (!update_server(server)) {
        return false;
    }
    
    std::string error;
    auto& svc = aml::services::ServiceManager::instance();
    auto* server_service = svc.servers();
    const bool ok = server_service && server_service->stop_local_server(server_id, &error);

    Server updated = get_server(server_id);
    if (updated.id.empty()) return false;
    updated.status = ok ? ServerStatus::Stopped : ServerStatus::Error;
    updated.online = false;
    updated.ping_ms = -1;
    return update_server(updated) && ok;
}

bool ServerManager::restart_server(const std::string& server_id) {
    Server server = get_server(server_id);
    if (server.id.empty()) {
        return false;
    }

    if (!aml::services::ServiceManager::instance().servers()) {
        return false;
    }
    
    server.status = ServerStatus::Restarting;
    
    if (!update_server(server)) {
        return false;
    }
    
    auto& svc = aml::services::ServiceManager::instance();
    std::string error;
    auto* server_service = svc.servers();
    const bool stopped = server_service && server_service->stop_local_server(server_id, &error);

    Server updated = get_server(server_id);
    if (updated.id.empty()) return false;
    if (!stopped) {
        updated.status = ServerStatus::Error;
        updated.online = false;
        updated.ping_ms = -1;
        update_server(updated);
        return false;
    }

    const bool ok = server_service &&
        server_service->start_local_server(server_id, "", 2048, &error);
    updated = get_server(server_id);
    if (updated.id.empty()) return false;
    if (ok) {
        updated.status = ServerStatus::Starting;
        updated.online = false;
        updated.ping_ms = -1;
    } else {
        updated.status = ServerStatus::Error;
        updated.online = false;
        updated.ping_ms = -1;
    }
    return update_server(updated) && ok;
}

bool ServerManager::send_command(const std::string& server_id, const std::string& command) {
    Server server = get_server(server_id);
    if (server.id.empty() || !server.online) {
        return false;
    }
    
    std::string response;
    std::string error;
    auto& svc = aml::services::ServiceManager::instance();
    auto* server_service = svc.servers();
    bool ok = server_service &&
              server_service->send_command(server_id, command, &response, &error);
    
    ServerLog log;
    log.timestamp = std::time(nullptr);
    log.type = "command";
    log.message = ">" + command;
    log.message = ok ? "Command executed: " + command
                     : "Command failed: " + command + (error.empty() ? "" : " (" + error + ")");
    log.server_id = server_id;
    
    {
        std::lock_guard<std::mutex> lock(logs_mu_);
        logs_.push_back(log);
    }
    
    return ok;
}

// ---------------------------------------------------------------------------
// Server Monitoring
// ---------------------------------------------------------------------------

void ServerManager::start_monitoring() {
    if (monitoring_) {
        return;
    }
    
    monitoring_ = true;
    monitor_thread_ = std::thread([this]() {
        while (monitoring_) {
            check_server_health();
            // Keep shutdown responsive without changing the 30-second health
            // cadence. The singleton destructor otherwise waits for a naked
            // long sleep before it can join this worker.
            for (int second = 0; second < 30 && monitoring_; ++second) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    });
}

void ServerManager::stop_monitoring() {
    monitoring_ = false;
    if (monitor_thread_.joinable()) {
        monitor_thread_.join();
    }
}

void ServerManager::check_server_health() {
    auto& service_manager = aml::services::ServiceManager::instance();
    auto* server_service = service_manager.servers();

    // Fall back to the existing TCP probe when the local service is not
    // configured. A successful process launch alone is never a health result.
    auto tcp_probe = [](const Server& server, int& ping_ms) {
        ping_ms = -1;
        SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == INVALID_SOCKET) return false;

        DWORD timeout = 3000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));

        struct sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<u_short>(server.port));
        std::string host = server.host.empty() ? "127.0.0.1" : server.host;
        if (host == "localhost") host = "127.0.0.1";

        struct addrinfo hints = {}, *ai_result = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host.c_str(), nullptr, &hints, &ai_result) != 0 || !ai_result) {
            closesocket(sock);
            return false;
        }
        addr.sin_addr = ((struct sockaddr_in*)ai_result->ai_addr)->sin_addr;
        freeaddrinfo(ai_result);

        auto start = std::chrono::steady_clock::now();
        int connect_result = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
        auto end = std::chrono::steady_clock::now();
        if (connect_result == 0) {
            ping_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                end - start).count());
        }
        closesocket(sock);
        return connect_result == 0;
    };

    // Snapshot under the lock, then perform potentially multi-second probes
    // without blocking the server list, lifecycle actions, or UI reads.
    std::vector<Server> probe_targets;
    {
        std::lock_guard<std::mutex> lock(servers_mu_);
        probe_targets = servers_;
    }

    struct HealthUpdate {
        Server observed;
        ServerStatus expected_status = ServerStatus::Stopped;
    };
    std::vector<HealthUpdate> health_updates;
    for (auto server : probe_targets) {
        if (server.status == ServerStatus::Stopping) continue;
        const bool should_probe = server.status == ServerStatus::Running ||
                                  server.status == ServerStatus::Starting ||
                                  server.status == ServerStatus::Restarting ||
                                  server.online;
        if (!should_probe) continue;

        const ServerStatus previous_status = server.status;
        int measured_ping = -1;
        bool healthy = false;
        if (server_service) {
            aml::services::ServerInfo probe;
            probe.id = server.id;
            probe.host = server.host;
            probe.port = server.port;
            // This lets the service probe an unregistered remote server with
            // the supplied endpoint without claiming a process.
            probe.online = true;
            healthy = server_service->ping_server(server.id, &probe);
            if (healthy) measured_ping = probe.ping_ms;
        } else {
            healthy = tcp_probe(server, measured_ping);
        }

        if (healthy) {
            server.status = ServerStatus::Running;
            server.online = true;
            server.ping_ms = measured_ping;
            server.last_ping = std::time(nullptr);
        } else {
            server.online = false;
            server.ping_ms = -1;
            // A stopped server failing a probe is an expected result. An
            // active server that fails verification must not remain Running.
            server.status = previous_status == ServerStatus::Stopped
                ? ServerStatus::Stopped : ServerStatus::Error;
        }
        health_updates.push_back({std::move(server), previous_status});
    }

    bool applied_update = false;
    {
        std::lock_guard<std::mutex> lock(servers_mu_);
        for (const auto& update : health_updates) {
            const auto current = std::find_if(servers_.begin(), servers_.end(),
                [&](const Server& server) { return server.id == update.observed.id; });
            // A lifecycle operation changed this server while the probe was in
            // flight. Its explicit state wins over an older health sample.
            if (current == servers_.end() || current->status != update.expected_status ||
                current->status == ServerStatus::Stopping) {
                continue;
            }
            current->status = update.observed.status;
            current->online = update.observed.online;
            current->ping_ms = update.observed.ping_ms;
            current->last_ping = update.observed.last_ping;
            applied_update = true;
        }
    }

    // Health monitoring is local telemetry. It intentionally never mirrors
    // status through an ambient authenticated provider session: explicit
    // Admin lifecycle actions own remote state transitions and their scoped
    // account worker. This prevents a monitor tick from writing under a
    // replaced account or during shutdown.
    if (applied_update) save_servers();
}

// ---------------------------------------------------------------------------
// Server Statistics
// ---------------------------------------------------------------------------

ServerStats ServerManager::get_stats() const {
    std::lock_guard<std::mutex> lock(servers_mu_);
    
    ServerStats stats;
    stats.total_servers = static_cast<int>(servers_.size());
    
    for (const auto& server : servers_) {
        switch (server.status) {
            case ServerStatus::Running: stats.running++; break;
            case ServerStatus::Stopped: stats.stopped++; break;
            case ServerStatus::Starting: stats.starting++; break;
            case ServerStatus::Stopping: stats.stopping++; break;
            case ServerStatus::Restarting: stats.restarting++; break;
            case ServerStatus::Error: stats.error++; break;
        }
        
        if (server.online) stats.online++;
        stats.total_players += server.current_players;
        stats.max_players += server.max_players;
    }
    
    return stats;
}

std::vector<ServerLog> ServerManager::get_logs(const std::string& server_id, int limit) const {
    std::lock_guard<std::mutex> lock(logs_mu_);
    
    std::vector<ServerLog> filtered_logs;
    for (const auto& log : logs_) {
        if (server_id.empty() || log.server_id == server_id) {
            filtered_logs.push_back(log);
        }
    }
    
    // Sort by timestamp descending
    std::sort(filtered_logs.begin(), filtered_logs.end(), 
        [](const ServerLog& a, const ServerLog& b) {
            return a.timestamp > b.timestamp;
        });
    
    // Limit results
    if (limit > 0 && filtered_logs.size() > static_cast<size_t>(limit)) {
        filtered_logs.resize(limit);
    }
    
    return filtered_logs;
}

// ---------------------------------------------------------------------------
// Server Backups
// ---------------------------------------------------------------------------

std::vector<ServerBackup> ServerManager::get_backups(const std::string& server_id) const {
    std::lock_guard<std::mutex> lock(backups_mu_);
    
    std::vector<ServerBackup> filtered_backups;
    for (const auto& backup : backups_) {
        if (backup.server_id == server_id) {
            filtered_backups.push_back(backup);
        }
    }
    
    // Sort by timestamp descending
    std::sort(filtered_backups.begin(), filtered_backups.end(),
        [](const ServerBackup& a, const ServerBackup& b) {
            return a.timestamp > b.timestamp;
        });
    
    return filtered_backups;
}

ServerBackup ServerManager::create_backup(const std::string& server_id, const std::string& label) {
    Server server = get_server(server_id);
    if (server.id.empty()) {
        return ServerBackup();
    }
    
    ServerBackup backup;
    backup.id = generate_backup_id();
    backup.server_id = server_id;
    backup.timestamp = std::time(nullptr);
    backup.label = label.empty() ? "Automatic Backup" : label;
    backup.status = BackupStatus::InProgress;
    
    // Determine server directory from app data
    std::wstring server_dir = aml::net::get_local_app_data_path() +
        L"\\amalgam\\servers\\" + aml::net::to_wide(server.name);
    std::wstring world_dir = server_dir + L"\\world";
    std::wstring backup_dir = server_dir + L"\\backups\\" +
        aml::net::to_wide(backup.label);
    
    aml::net::mkdirs(server_dir + L"\\backups");
    
    // Copy world directory if it exists
    if (aml::net::directory_exists(world_dir)) {
        std::error_code ec;
        std::filesystem::copy(world_dir, backup_dir,
                              std::filesystem::copy_options::recursive, ec);
        if (ec) {
            backup.status = BackupStatus::Failed;
            backup.size_bytes = 0;
        } else {
            backup.size_bytes = calculate_directory_size(backup_dir);
            backup.status = BackupStatus::Completed;
        }
    } else {
        backup.size_bytes = 0;
        backup.status = BackupStatus::Completed;
    }
    
    backup.path = aml::net::to_utf8(backup_dir);
    
    {
        std::lock_guard<std::mutex> lock(backups_mu_);
        backups_.push_back(backup);
    }
    
    return backup;
}

bool ServerManager::restore_backup(const std::string& backup_id) {
    ServerBackup backup;
    {
        std::lock_guard<std::mutex> lock(backups_mu_);
        for (const auto& b : backups_) {
            if (b.id == backup_id) {
                backup = b;
                break;
            }
        }
    }
    
    if (backup.id.empty()) {
        return false;
    }
    
    // Find the server for this backup
    Server server = get_server(backup.server_id);
    if (server.id.empty()) {
        return false;
    }
    
    std::wstring backup_dir = aml::net::to_wide(backup.path);
    std::wstring server_dir = aml::net::get_local_app_data_path() +
        L"\\amalgam\\servers\\" + aml::net::to_wide(server.name);
    std::wstring world_dir = server_dir + L"\\world";
    
    // Remove current world directory
    if (aml::net::directory_exists(world_dir)) {
        std::error_code ec;
        std::filesystem::remove_all(world_dir, ec);
    }
    
    // Copy backup to world directory
    if (aml::net::directory_exists(backup_dir)) {
        std::error_code ec;
        std::filesystem::copy(backup_dir, world_dir,
                              std::filesystem::copy_options::recursive, ec);
        if (ec) {
            return false;
        }
    }
    
    // Update backup status
    backup.status = BackupStatus::Restored;
    {
        std::lock_guard<std::mutex> lock(backups_mu_);
        for (auto& b : backups_) {
            if (b.id == backup.id) {
                b = backup;
                break;
            }
        }
    }
    
    return true;
}

bool ServerManager::delete_backup(const std::string& backup_id) {
    ServerBackup backup;
    {
        std::lock_guard<std::mutex> lock(backups_mu_);
        for (const auto& b : backups_) {
            if (b.id == backup_id) {
                backup = b;
                break;
            }
        }
    }
    
    if (backup.id.empty()) {
        return false;
    }
    
    // Remove backup files from disk
    if (!backup.path.empty() && aml::net::directory_exists(aml::net::to_wide(backup.path))) {
        std::error_code ec;
        std::filesystem::remove_all(aml::net::to_wide(backup.path), ec);
    }
    
    // Remove from memory
    {
        std::lock_guard<std::mutex> lock(backups_mu_);
        backups_.erase(std::remove_if(backups_.begin(), backups_.end(),
            [&backup_id](const ServerBackup& b) { return b.id == backup_id; }), backups_.end());
    }
    
    return true;
}

// ---------------------------------------------------------------------------
// Node Management
// ---------------------------------------------------------------------------

std::vector<Node> ServerManager::get_nodes() const {
    std::lock_guard<std::mutex> lock(nodes_mu_);
    return nodes_;
}

Node ServerManager::get_node(const std::string& node_id) const {
    std::lock_guard<std::mutex> lock(nodes_mu_);
    for (const auto& node : nodes_) {
        if (node.id == node_id) {
            return node;
        }
    }
    return Node();
}

Node ServerManager::get_node_by_name(const std::string& name) const {
    std::lock_guard<std::mutex> lock(nodes_mu_);
    for (const auto& node : nodes_) {
        if (node.name == name) {
            return node;
        }
    }
    return Node();
}

bool ServerManager::is_node_online(const std::string& node_id) const {
    std::lock_guard<std::mutex> lock(nodes_mu_);
    for (const auto& node : nodes_) {
        if (node.id == node_id) {
            if (node.status != NodeStatus::Online) return false;
            auto now = static_cast<int64_t>(std::time(nullptr));
            return (now - node.last_heartbeat) < 60;
        }
    }
    return false;
}

Node ServerManager::register_node(const Node& node) {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    
    if (!supabase.is_authenticated()) {
        return Node();
    }
    
    // Generate ID if not provided
    Node new_node = node;
    if (new_node.id.empty()) {
        new_node.id = generate_node_id();
    }
    
    // Set default values
    new_node.user_id = supabase.get_current_user().id;
    // Registration is not a health check. Keep the node unverified until a
    // real probe succeeds.
    new_node.status = NodeStatus::Offline;
    new_node.registered_at = std::time(nullptr);
    new_node.last_heartbeat = 0;
    new_node.cpu_usage = 0.0;
    new_node.memory_usage = 0.0;
    new_node.storage_usage = 0.0;
    new_node.metadata["health_status"] = "unverified";
    new_node.metadata["metrics_status"] = "unavailable";
    
    // Register in Supabase
    auto created = supabase.register_node(convert_to_supabase_node(new_node));
    if (created.id.empty()) {
        return Node();
    }
    
    // Add to local cache
    {
        std::lock_guard<std::mutex> lock(nodes_mu_);
        nodes_.push_back(new_node);
    }
    
    save_nodes();
    return new_node;
}

bool ServerManager::update_node(const Node& node) {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    
    if (!supabase.is_authenticated()) {
        return false;
    }
    
    // Update in Supabase
    bool success = supabase.update_node(convert_to_supabase_node(node));
    if (!success) {
        return false;
    }
    
    // Update local cache
    {
        std::lock_guard<std::mutex> lock(nodes_mu_);
        for (auto& n : nodes_) {
            if (n.id == node.id) {
                n = node;
                n.last_updated = std::time(nullptr);
                break;
            }
        }
    }
    
    save_nodes();
    return true;
}

bool ServerManager::deregister_node(const std::string& node_id) {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    
    if (!supabase.is_authenticated()) {
        return false;
    }
    
    // Deregister from Supabase
    bool success = supabase.deregister_node(node_id);
    if (!success) {
        return false;
    }
    
    // Remove from local cache
    {
        std::lock_guard<std::mutex> lock(nodes_mu_);
        nodes_.erase(std::remove_if(nodes_.begin(), nodes_.end(),
            [&node_id](const Node& n) { return n.id == node_id; }), nodes_.end());
    }
    
    save_nodes();
    return true;
}

// ---------------------------------------------------------------------------
// Node Monitoring
// ---------------------------------------------------------------------------

void ServerManager::start_node_monitoring() {
    if (node_monitoring_) {
        return;
    }
    
    node_monitoring_ = true;
    node_monitor_thread_ = std::thread([this]() {
        while (node_monitoring_) {
            check_node_health();
            for (int second = 0; second < 15 && node_monitoring_; ++second) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    });
}

void ServerManager::stop_node_monitoring() {
    node_monitoring_ = false;
    if (node_monitor_thread_.joinable()) {
        node_monitor_thread_.join();
    }
}

void ServerManager::check_node_health() {
    auto& service_manager = aml::services::ServiceManager::instance();
    auto* node_service = service_manager.nodes();

    std::vector<Node> health_updates;
    {
        std::lock_guard<std::mutex> lock(nodes_mu_);
        health_updates = nodes_;
    }

    for (auto& node : health_updates) {
        // There is no local resource probe for nodes. Keep the numeric fields
        // neutral for existing API consumers and record their availability
        // explicitly instead of presenting zero or fabricated usage as real.
        node.cpu_usage = 0.0;
        node.memory_usage = 0.0;
        node.storage_usage = 0.0;
        node.metadata["metrics_status"] = "unavailable";

        int latency_ms = 0;
        const bool healthy = node_service &&
                             node_service->ping_node(node.id, &latency_ms, nullptr);
        if (healthy) {
            node.status = NodeStatus::Online;
            node.last_heartbeat = std::time(nullptr);
            node.metadata["health_status"] = "verified";
        } else {
            node.status = NodeStatus::Offline;
            // Keep the last successful heartbeat, if any. A failed probe is
            // not a new heartbeat.
            node.metadata["health_status"] = "unverified";
        }
    }

    {
        std::lock_guard<std::mutex> lock(nodes_mu_);
        for (const auto& update : health_updates) {
            for (auto& node : nodes_) {
                if (node.id == update.id) {
                    node = update;
                    break;
                }
            }
        }
    }

    // Node probes are likewise local telemetry. Do not issue account-scoped
    // remote writes from this persistent monitoring thread.
    if (!health_updates.empty()) save_nodes();
}

NodeStats ServerManager::get_node_stats() const {
    std::lock_guard<std::mutex> lock(nodes_mu_);
    
    NodeStats stats;
    stats.total_nodes = static_cast<int>(nodes_.size());
    
    for (const auto& node : nodes_) {
        switch (node.status) {
            case NodeStatus::Online: stats.online++; break;
            case NodeStatus::Offline: stats.offline++; break;
            case NodeStatus::Maintenance: stats.maintenance++; break;
        }
        
        stats.total_cpu += node.cpu_cores;
        stats.total_memory += node.memory_mb;
        stats.total_storage += node.storage_gb;
        stats.used_storage += node.storage_gb * node.storage_usage;
    }
    
    return stats;
}

// ---------------------------------------------------------------------------
// Server-Node Assignment
// ---------------------------------------------------------------------------

bool ServerManager::assign_server_to_node(const std::string& server_id, const std::string& node_id) {
    Server server = get_server(server_id);
    Node node = get_node(node_id);
    
    if (server.id.empty() || node.id.empty()) {
        return false;
    }
    
    // Check if node has capacity
    if (node.status != NodeStatus::Online) {
        return false;
    }
    
    // Count servers on this node
    int server_count = 0;
    for (const auto& s : servers_) {
        if (s.node_id == node_id) {
            server_count++;
        }
    }
    
    // Keep the node's advertised service capacity bounded.
    if (server_count >= 10) {
        return false;
    }
    
    // Assign server to node
    server.node_id = node_id;
    server.assigned_at = std::time(nullptr);
    
    return update_server(server);
}

bool ServerManager::migrate_server(const std::string& server_id, const std::string& new_node_id) {
    Server server = get_server(server_id);
    if (server.id.empty()) {
        return false;
    }
    
    // Check if server is running
    if (server.status == ServerStatus::Running) {
        // Stop server first
        if (!stop_server(server_id)) {
            return false;
        }
        
        // Wait for the service to report a stopped process before moving it.
        bool stopped = false;
        for (int attempt = 0; attempt < 30; ++attempt) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            const Server current = get_server(server_id);
            if (current.id.empty()) return false;
            if (current.status == ServerStatus::Stopped || !current.online) {
                stopped = true;
                break;
            }
        }
        if (!stopped) return false;
    }
    
    // Assign to new node
    return assign_server_to_node(server_id, new_node_id);
}

// ---------------------------------------------------------------------------
// Cluster Management
// ---------------------------------------------------------------------------

Cluster ServerManager::create_cluster(const Cluster& cluster) {
    Cluster new_cluster = cluster;
    if (new_cluster.id.empty()) {
        new_cluster.id = generate_cluster_id();
    }
    
    new_cluster.created_at = std::time(nullptr);
    new_cluster.status = ClusterStatus::Active;
    
    {
        std::lock_guard<std::mutex> lock(clusters_mu_);
        clusters_.push_back(new_cluster);
    }
    
    save_clusters();
    return new_cluster;
}

bool ServerManager::update_cluster(const Cluster& cluster) {
    {
        std::lock_guard<std::mutex> lock(clusters_mu_);
        for (auto& c : clusters_) {
            if (c.id == cluster.id) {
                c = cluster;
                c.updated_at = std::time(nullptr);
                return true;
            }
        }
    }
    return false;
}

bool ServerManager::delete_cluster(const std::string& cluster_id) {
    {
        std::lock_guard<std::mutex> lock(clusters_mu_);
        clusters_.erase(std::remove_if(clusters_.begin(), clusters_.end(),
            [&cluster_id](const Cluster& c) { return c.id == cluster_id; }), clusters_.end());
    }
    
    save_clusters();
    return true;
}

std::vector<Cluster> ServerManager::get_clusters() const {
    std::lock_guard<std::mutex> lock(clusters_mu_);
    return clusters_;
}

bool ServerManager::add_node_to_cluster(const std::string& cluster_id, const std::string& node_id) {
    Cluster cluster;
    Node node;
    
    {
        std::lock_guard<std::mutex> lock(clusters_mu_);
        for (auto& c : clusters_) {
            if (c.id == cluster_id) {
                cluster = c;
                break;
            }
        }
    }
    
    {
        std::lock_guard<std::mutex> lock(nodes_mu_);
        for (auto& n : nodes_) {
            if (n.id == node_id) {
                node = n;
                break;
            }
        }
    }
    
    if (cluster.id.empty() || node.id.empty()) {
        return false;
    }
    
    // Check if node is already in cluster
    for (const auto& n : cluster.node_ids) {
        if (n == node_id) {
            return true; // Already in cluster
        }
    }
    
    // Add node to cluster
    cluster.node_ids.push_back(node_id);
    node.cluster_id = cluster_id;
    
    {
        std::lock_guard<std::mutex> lock_clusters(clusters_mu_);
        for (auto& c : clusters_) {
            if (c.id == cluster.id) {
                c = cluster;
                break;
            }
        }
    }
    
    {
        std::lock_guard<std::mutex> lock_nodes(nodes_mu_);
        for (auto& n : nodes_) {
            if (n.id == node.id) {
                n = node;
                break;
            }
        }
    }
    
    save_clusters();
    save_nodes();
    return true;
}

bool ServerManager::remove_node_from_cluster(const std::string& cluster_id, const std::string& node_id) {
    Cluster cluster;
    Node node;
    
    {
        std::lock_guard<std::mutex> lock(clusters_mu_);
        for (auto& c : clusters_) {
            if (c.id == cluster_id) {
                cluster = c;
                break;
            }
        }
    }
    
    {
        std::lock_guard<std::mutex> lock(nodes_mu_);
        for (auto& n : nodes_) {
            if (n.id == node_id) {
                node = n;
                break;
            }
        }
    }
    
    if (cluster.id.empty() || node.id.empty()) {
        return false;
    }
    
    // Remove node from cluster
    cluster.node_ids.erase(std::remove(cluster.node_ids.begin(), cluster.node_ids.end(), node_id), cluster.node_ids.end());
    node.cluster_id.clear();
    
    {
        std::lock_guard<std::mutex> lock_clusters(clusters_mu_);
        for (auto& c : clusters_) {
            if (c.id == cluster.id) {
                c = cluster;
                break;
            }
        }
    }
    
    {
        std::lock_guard<std::mutex> lock_nodes(nodes_mu_);
        for (auto& n : nodes_) {
            if (n.id == node.id) {
                n = node;
                break;
            }
        }
    }
    
    save_clusters();
    save_nodes();
    return true;
}

// ---------------------------------------------------------------------------
// Real-time Subscriptions
// ---------------------------------------------------------------------------

void ServerManager::subscribe_to_servers(const std::function<void(const Server&)>& callback) {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    
    supabase.subscribe_to_servers([this, callback](const aml::supabase::SupabaseServer& supa_server) {
        Server server = convert_from_supabase(supa_server);
        
        {
            std::lock_guard<std::mutex> lock(servers_mu_);
            bool found = false;
            for (auto& s : servers_) {
                if (s.id == server.id) {
                    s = server;
                    found = true;
                    break;
                }
            }
            if (!found) {
                servers_.push_back(server);
            }
        }
        
        callback(server);
    });
}

void ServerManager::subscribe_to_nodes(const std::function<void(const Node&)>& callback) {
    // In a real implementation, this would subscribe to node updates
    // For now, we'll use a polling approach
    
    polling_callbacks_.push_back(callback);
    
    if (!polling_active_) {
        polling_active_ = true;
        polling_thread_ = std::thread([this]() {
            while (polling_active_) {
                std::this_thread::sleep_for(std::chrono::seconds(5));
                
                auto nodes = get_nodes();
                for (const auto& node : nodes) {
                    for (const auto& callback : polling_callbacks_) {
                        callback(node);
                    }
                }
            }
        });
    }
}

void ServerManager::unsubscribe_all() {
    polling_active_ = false;
    if (polling_thread_.joinable()) {
        polling_thread_.join();
    }
    polling_callbacks_.clear();
}

// ---------------------------------------------------------------------------
// Data Persistence
// ---------------------------------------------------------------------------

std::string ServerManager::generate_server_id() const {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 15);
    
    const char* hex_chars = "0123456789abcdef";
    std::string id;
    
    for (int i = 0; i < 16; ++i) {
        id += hex_chars[dis(gen)];
    }
    
    return id;
}

std::string ServerManager::generate_node_id() const {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 15);
    
    const char* hex_chars = "0123456789abcdef";
    std::string id;
    
    for (int i = 0; i < 16; ++i) {
        id += hex_chars[dis(gen)];
    }
    
    return id;
}

std::string ServerManager::generate_backup_id() const {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 15);
    
    const char* hex_chars = "0123456789abcdef";
    std::string id;
    
    for (int i = 0; i < 16; ++i) {
        id += hex_chars[dis(gen)];
    }
    
    return id;
}

std::string ServerManager::generate_cluster_id() const {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 15);
    
    const char* hex_chars = "0123456789abcdef";
    std::string id;
    
    for (int i = 0; i < 16; ++i) {
        id += hex_chars[dis(gen)];
    }
    
    return id;
}

static std::wstring config_file_path(const std::string& filename) {
    std::wstring base = aml::net::get_local_app_data_path();
    return base + L"\\amalgam\\" + aml::net::to_wide(filename);
}

bool ServerManager::save_servers() const {
    try {
        std::lock_guard<std::mutex> lock(servers_mu_);
        Json arr = Json::arr();
        for (const auto& s : servers_) {
            Json obj = Json::obj();
            obj["id"] = s.id;
            obj["user_id"] = s.user_id;
            obj["name"] = s.name;
            obj["host"] = s.host;
            obj["port"] = s.port;
            obj["version"] = s.version;
            obj["type"] = s.type;
            obj["motd"] = s.motd;
            obj["max_players"] = s.max_players;
            obj["current_players"] = s.current_players;
            obj["online"] = s.online;
            obj["ping_ms"] = s.ping_ms;
            obj["node_id"] = s.node_id;
            obj["cluster_id"] = s.cluster_id;
            obj["status"] = static_cast<int>(s.status);
            obj["last_ping"] = s.last_ping;
            obj["last_started"] = s.last_started;
            obj["created_at"] = s.created_at;
            obj["updated_at"] = s.updated_at;
            obj["assigned_at"] = s.assigned_at;
            Json meta = Json::obj();
            for (const auto& kv : s.metadata) {
                meta[kv.first] = kv.second;
            }
            obj["metadata"] = meta;
            arr.push(obj);
        }
        std::string err;
        if (!aml::json_write_file(config_file_path("servers.json"), arr, &err)) {
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool ServerManager::load_servers() {
    try {
        std::wstring path = config_file_path("servers.json");
        if (!aml::net::file_exists(path)) {
            return true;
        }
        Json root;
        std::string err;
        if (!aml::json_parse_file(path, root, &err) || !root.isArray()) {
            return false;
        }
        std::lock_guard<std::mutex> lock(servers_mu_);
        servers_.clear();
        for (const auto& item : root.items()) {
            Server s;
            s.id = item.get("id").as_str();
            s.user_id = item.get("user_id").as_str();
            s.name = item.get("name").as_str();
            s.host = item.get("host").as_str();
            s.port = item.get("port").asInt();
            s.version = item.get("version").as_str();
            s.type = item.get("type").as_str();
            s.motd = item.get("motd").as_str();
            s.max_players = item.get("max_players").asInt();
            s.current_players = item.get("current_players").asInt();
            s.online = item.get("online").as_bool();
            s.ping_ms = item.get("ping_ms").asInt();
            s.node_id = item.get("node_id").as_str();
            s.cluster_id = item.get("cluster_id").as_str();
            s.status = static_cast<ServerStatus>(item.get("status").asInt());
            s.last_ping = item.get("last_ping").as_int();
            s.last_started = item.get("last_started").as_int();
            s.created_at = item.get("created_at").as_int();
            s.updated_at = item.get("updated_at").as_int();
            s.assigned_at = item.get("assigned_at").as_int();
            if (item.isMember("metadata") && item.get("metadata").isObject()) {
                for (const auto& kv : item.get("metadata").pairs()) {
                    s.metadata[kv.first] = kv.second.as_str();
                }
            }
            servers_.push_back(s);
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool ServerManager::save_nodes() const {
    try {
        std::lock_guard<std::mutex> lock(nodes_mu_);
        Json arr = Json::arr();
        for (const auto& n : nodes_) {
            Json obj = Json::obj();
            obj["id"] = n.id;
            obj["user_id"] = n.user_id;
            obj["name"] = n.name;
            obj["host"] = n.host;
            obj["port"] = n.port;
            obj["cluster_id"] = n.cluster_id;
            obj["cpu_cores"] = n.cpu_cores;
            obj["memory_mb"] = n.memory_mb;
            obj["storage_gb"] = n.storage_gb;
            obj["cpu_usage"] = n.cpu_usage;
            obj["memory_usage"] = n.memory_usage;
            obj["storage_usage"] = n.storage_usage;
            obj["status"] = static_cast<int>(n.status);
            obj["registered_at"] = n.registered_at;
            obj["last_heartbeat"] = n.last_heartbeat;
            obj["last_updated"] = n.last_updated;
            Json meta = Json::obj();
            for (const auto& kv : n.metadata) {
                meta[kv.first] = kv.second;
            }
            obj["metadata"] = meta;
            arr.push(obj);
        }
        std::string err;
        if (!aml::json_write_file(config_file_path("nodes.json"), arr, &err)) {
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool ServerManager::load_nodes() {
    try {
        std::wstring path = config_file_path("nodes.json");
        if (!aml::net::file_exists(path)) {
            return true;
        }
        Json root;
        std::string err;
        if (!aml::json_parse_file(path, root, &err) || !root.isArray()) {
            return false;
        }
        std::lock_guard<std::mutex> lock(nodes_mu_);
        nodes_.clear();
        for (const auto& item : root.items()) {
            Node n;
            n.id = item.get("id").as_str();
            n.user_id = item.get("user_id").as_str();
            n.name = item.get("name").as_str();
            n.host = item.get("host").as_str();
            n.port = item.get("port").asInt();
            n.cluster_id = item.get("cluster_id").as_str();
            n.cpu_cores = item.get("cpu_cores").asInt();
            n.memory_mb = item.get("memory_mb").asInt();
            n.storage_gb = item.get("storage_gb").asInt();
            n.cpu_usage = item.get("cpu_usage").as_num();
            n.memory_usage = item.get("memory_usage").as_num();
            n.storage_usage = item.get("storage_usage").as_num();
            n.status = static_cast<NodeStatus>(item.get("status").asInt());
            n.registered_at = item.get("registered_at").as_int();
            n.last_heartbeat = item.get("last_heartbeat").as_int();
            n.last_updated = item.get("last_updated").as_int();
            if (item.isMember("metadata") && item.get("metadata").isObject()) {
                for (const auto& kv : item.get("metadata").pairs()) {
                    n.metadata[kv.first] = kv.second.as_str();
                }
            }
            nodes_.push_back(n);
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool ServerManager::save_clusters() const {
    try {
        std::lock_guard<std::mutex> lock(clusters_mu_);
        Json arr = Json::arr();
        for (const auto& c : clusters_) {
            Json obj = Json::obj();
            obj["id"] = c.id;
            obj["user_id"] = c.user_id;
            obj["name"] = c.name;
            obj["description"] = c.description;
            Json node_ids = Json::arr();
            for (const auto& nid : c.node_ids) {
                node_ids.push(nid);
            }
            obj["node_ids"] = node_ids;
            obj["load_balancing_strategy"] = c.load_balancing_strategy;
            obj["max_servers_per_node"] = c.max_servers_per_node;
            obj["status"] = static_cast<int>(c.status);
            obj["created_at"] = c.created_at;
            obj["updated_at"] = c.updated_at;
            Json meta = Json::obj();
            for (const auto& kv : c.metadata) {
                meta[kv.first] = kv.second;
            }
            obj["metadata"] = meta;
            arr.push(obj);
        }
        std::string err;
        if (!aml::json_write_file(config_file_path("clusters.json"), arr, &err)) {
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool ServerManager::load_clusters() {
    try {
        std::wstring path = config_file_path("clusters.json");
        if (!aml::net::file_exists(path)) {
            return true;
        }
        Json root;
        std::string err;
        if (!aml::json_parse_file(path, root, &err) || !root.isArray()) {
            return false;
        }
        std::lock_guard<std::mutex> lock(clusters_mu_);
        clusters_.clear();
        for (const auto& item : root.items()) {
            Cluster c;
            c.id = item.get("id").as_str();
            c.user_id = item.get("user_id").as_str();
            c.name = item.get("name").as_str();
            c.description = item.get("description").as_str();
            if (item.isMember("node_ids") && item.get("node_ids").isArray()) {
                for (const auto& nid : item.get("node_ids").items()) {
                    c.node_ids.push_back(nid.as_str());
                }
            }
            c.load_balancing_strategy = item.get("load_balancing_strategy").as_str();
            c.max_servers_per_node = item.get("max_servers_per_node").asInt();
            c.status = static_cast<ClusterStatus>(item.get("status").asInt());
            c.created_at = item.get("created_at").as_int();
            c.updated_at = item.get("updated_at").as_int();
            if (item.isMember("metadata") && item.get("metadata").isObject()) {
                for (const auto& kv : item.get("metadata").pairs()) {
                    c.metadata[kv.first] = kv.second.as_str();
                }
            }
            clusters_.push_back(c);
        }
        return true;
    } catch (...) {
        return false;
    }
}

// ---------------------------------------------------------------------------
// Conversion Functions
// ---------------------------------------------------------------------------

aml::supabase::SupabaseServer ServerManager::convert_to_supabase(const Server& server) const {
    aml::supabase::SupabaseServer supa_server;
    supa_server.id = server.id;
    supa_server.user_id = server.user_id;
    supa_server.name = server.name;
    supa_server.host = server.host;
    supa_server.port = server.port;
    supa_server.version = server.version;
    supa_server.type = server.type;
    supa_server.motd = server.motd;
    supa_server.max_players = server.max_players;
    supa_server.online = server.online;
    supa_server.ping_ms = server.ping_ms;
    
    // Store additional fields in properties
    supa_server.properties["node_id"] = server.node_id;
    supa_server.properties["cluster_id"] = server.cluster_id;
    supa_server.properties["status"] = std::to_string(static_cast<int>(server.status));
    supa_server.properties["current_players"] = std::to_string(server.current_players);
    supa_server.properties["last_ping"] = std::to_string(server.last_ping);
    supa_server.properties["last_started"] = std::to_string(server.last_started);
    supa_server.properties["assigned_at"] = std::to_string(server.assigned_at);
    
    supa_server.created_at = server.created_at;
    supa_server.updated_at = server.updated_at;
    return supa_server;
}

Server ServerManager::convert_from_supabase(const aml::supabase::SupabaseServer& supa_server) const {
    Server server;
    server.id = supa_server.id;
    server.user_id = supa_server.user_id;
    server.name = supa_server.name;
    server.host = supa_server.host;
    server.port = supa_server.port;
    server.version = supa_server.version;
    server.type = supa_server.type;
    server.motd = supa_server.motd;
    server.max_players = supa_server.max_players;
    server.online = supa_server.online;
    server.ping_ms = supa_server.ping_ms;
    
    // Load additional fields from properties
    server.node_id = supa_server.properties.count("node_id") ? supa_server.properties.at("node_id") : "";
    server.cluster_id = supa_server.properties.count("cluster_id") ? supa_server.properties.at("cluster_id") : "";
    server.status = supa_server.properties.count("status") ? 
        static_cast<ServerStatus>(std::stoi(supa_server.properties.at("status"))) : ServerStatus::Stopped;
    server.current_players = supa_server.properties.count("current_players") ? 
        std::stoi(supa_server.properties.at("current_players")) : 0;
    server.last_ping = supa_server.properties.count("last_ping") ? 
        std::stoll(supa_server.properties.at("last_ping")) : 0;
    server.last_started = supa_server.properties.count("last_started") ? 
        std::stoll(supa_server.properties.at("last_started")) : 0;
    server.assigned_at = supa_server.properties.count("assigned_at") ? 
        std::stoll(supa_server.properties.at("assigned_at")) : 0;
    
    server.created_at = supa_server.created_at;
    server.updated_at = supa_server.updated_at;
    return server;
}

aml::supabase::SupabaseNode ServerManager::convert_to_supabase_node(const Node& node) const {
    aml::supabase::SupabaseNode supa_node;
    supa_node.id = node.id;
    // Note: user_id is not in SupabaseNode, so we'll use metadata
    supa_node.name = node.name;
    supa_node.host = node.host;
    supa_node.port = node.port;
    // Note: cluster_id is not in SupabaseNode, so we'll use metadata
    supa_node.online = (node.status == NodeStatus::Online);
    supa_node.last_heartbeat = node.last_heartbeat;
    supa_node.storage_capacity = static_cast<uint64_t>(node.storage_gb) * 1024 * 1024 * 1024; // GB to bytes
    supa_node.cpu_cores = node.cpu_cores;
    supa_node.memory_total = static_cast<uint64_t>(node.memory_mb);

    const auto metrics_status = node.metadata.find("metrics_status");
    const bool metrics_unavailable = metrics_status != node.metadata.end() &&
                                     metrics_status->second == "unavailable";
    if (metrics_unavailable) {
        // The Supabase numeric columns are not optional. Zero is only the
        // transport value; metrics_status prevents it from being interpreted
        // as a measured zero by this client.
        supa_node.storage_used = 0;
        supa_node.storage_available = 0;
        supa_node.memory_used = 0;
    } else {
        supa_node.storage_used = static_cast<uint64_t>(
            node.storage_gb * node.storage_usage * 1024 * 1024 * 1024);
        supa_node.storage_available = supa_node.storage_capacity >= supa_node.storage_used
            ? supa_node.storage_capacity - supa_node.storage_used : 0;
        supa_node.memory_used = static_cast<uint64_t>(node.memory_mb * node.memory_usage);
    }
    
    // Store additional fields in metadata
    for (const auto& kv : node.metadata) {
        supa_node.metadata[kv.first] = kv.second;
    }
    supa_node.metadata["user_id"] = node.user_id;
    supa_node.metadata["cluster_id"] = node.cluster_id;
    supa_node.metadata["status"] = std::to_string(static_cast<int>(node.status));
    if (metrics_unavailable) {
        supa_node.metadata.erase("cpu_usage");
    } else {
        supa_node.metadata["cpu_usage"] = std::to_string(node.cpu_usage);
    }
    
    supa_node.created_at = node.registered_at;
    supa_node.updated_at = node.last_updated;
    return supa_node;
}

Node ServerManager::convert_from_supabase_node(const aml::supabase::SupabaseNode& supa_node) const {
    Node node;
    node.id = supa_node.id;
    node.user_id = supa_node.metadata.count("user_id") ? supa_node.metadata.at("user_id") : "";
    node.name = supa_node.name;
    node.host = supa_node.host;
    node.port = supa_node.port;
    node.cluster_id = supa_node.metadata.count("cluster_id") ? supa_node.metadata.at("cluster_id") : "";
    node.status = supa_node.online ? NodeStatus::Online : NodeStatus::Offline;
    if (supa_node.metadata.count("status")) {
        node.status = static_cast<NodeStatus>(std::stoi(supa_node.metadata.at("status")));
    }
    node.cpu_cores = supa_node.cpu_cores;
    node.memory_mb = static_cast<int>(supa_node.memory_total);
    node.storage_gb = static_cast<int>(supa_node.storage_capacity / (1024 * 1024 * 1024));
    for (const auto& kv : supa_node.metadata) {
        node.metadata[kv.first] = kv.second;
    }
    const auto metrics_status = node.metadata.find("metrics_status");
    const bool metrics_unavailable = metrics_status != node.metadata.end() &&
                                     metrics_status->second == "unavailable";
    if (metrics_unavailable) {
        node.cpu_usage = 0.0;
        node.memory_usage = 0.0;
        node.storage_usage = 0.0;
    } else {
        node.cpu_usage = 0.0;
        const auto cpu_usage = node.metadata.find("cpu_usage");
        if (cpu_usage != node.metadata.end()) {
            try {
                node.cpu_usage = std::stod(cpu_usage->second);
            } catch (...) {
                node.cpu_usage = 0.0;
            }
        }
        node.memory_usage = supa_node.memory_total > 0
            ? static_cast<double>(supa_node.memory_used) / supa_node.memory_total : 0.0;
        node.storage_usage = supa_node.storage_capacity > 0
            ? static_cast<double>(supa_node.storage_used) / supa_node.storage_capacity : 0.0;
    }
    node.registered_at = supa_node.created_at;
    node.last_heartbeat = supa_node.last_heartbeat;
    node.last_updated = supa_node.updated_at;
    return node;
}

}  // namespace aml::servers
