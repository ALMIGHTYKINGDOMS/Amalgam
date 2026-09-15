#include "sync_manager.h"
#include "supabase.h"
#include "server_manager.h"
#include "account_manager.h"
#include "storage_manager.h"
#include "instances.h"
#include "net.h"
#include "json.h"

#include <windows.h>
#include <exception>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <utility>

namespace aml::sync {

namespace {

bool sync_storage_bucket(const std::string& bucket, const std::wstring& local_dir) {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    auto* client = supabase.client();
    if (!client || !supabase.is_authenticated()) return false;

    aml::supabase::SupabaseClient::StorageListOptions options;
    options.bucket = bucket;
    options.limit = 1000;
    auto listed = client->list_files(options);
    if (!listed.success) return false;

    if (!net::mkdirs(local_dir)) return false;
    bool success = true;
    for (const auto& item : listed.items) {
        // Storage paths are not trusted as Windows paths. Keep the synced cache
        // flat and use the server-reported file name only.
        std::wstring filename = std::filesystem::path(net::to_wide(item.name)).filename().wstring();
        if (filename.empty() || filename == L"." || filename == L"..") {
            success = false;
            continue;
        }

        auto downloaded = client->download_file(bucket, item.path.empty() ? item.name : item.path);
        if (!downloaded.success) {
            success = false;
            continue;
        }

        std::ofstream file(std::filesystem::path(local_dir) / filename, std::ios::binary);
        if (!file) {
            success = false;
            continue;
        }
        file.write(reinterpret_cast<const char*>(downloaded.data.data()),
                   static_cast<std::streamsize>(downloaded.data.size()));
        if (!file.good()) success = false;
    }
    return success;
}

void apply_remote_profile(const aml::supabase::SupabaseProfile& remote,
                          aml::instances::Instance& local) {
    local.id = remote.id;
    local.name = remote.name;
    local.minecraft_version = remote.minecraft_version;
    local.loader = remote.loader;
    local.loader_version = remote.loader_version;
    local.java_path = remote.java_path;
    local.memory_mb = remote.memory_mb;
    local.last_played = remote.last_played_at;
}

bool profile_from_operation(const SyncOperation& operation,
                            aml::supabase::SupabaseProfile& profile) {
    if (operation.data.empty()) return false;
    std::string error;
    Json data = Json::parse(operation.data, &error);
    if (!data.isObject()) return false;

    profile.id = operation.id.empty() ? data.get("id").as_str() : operation.id;
    profile.name = data.get("name").as_str();
    profile.minecraft_version = data.get("minecraft_version").as_str();
    profile.loader = data.get("loader").as_str("auto");
    profile.loader_version = data.get("loader_version").as_str();
    profile.java_path = data.get("java_path").as_str();
    profile.memory_mb = static_cast<int>(data.get("memory_mb").as_int(2048));
    profile.updated_at = data.get("updated_at").as_int(static_cast<int64_t>(std::time(nullptr)));
    profile.last_played_at = data.get("last_played_at").as_int();
    return !profile.id.empty() && !profile.name.empty() && !profile.minecraft_version.empty();
}

void remember_sync_error(std::mutex& state_mu, SyncState& sync_state,
                         const std::string& error) {
    std::lock_guard<std::mutex> lock(state_mu);
    sync_state.last_error = error.empty() ? "Synchronization failed" : error;
}

bool probe_connection() {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    auto* client = supabase.client();
    if (!client || !client->is_authenticated()) return false;

    // A successful authenticated query is the only point at which the sync
    // manager can claim that the control plane is reachable.
    aml::supabase::SupabaseClient::DBQueryOptions options;
    options.table = "servers";
    options.select = "id";
    options.limit = 1;
    return client->select(options).success;
}

} // namespace

// ---------------------------------------------------------------------------
// Sync Manager Implementation
// ---------------------------------------------------------------------------

SyncManager::SyncManager() {
    initialize();
}

SyncManager::~SyncManager() {
    shutdown();
}

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------

SyncManager& SyncManager::instance() {
    static SyncManager manager;
    return manager;
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

bool SyncManager::initialize() {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    
    if (!supabase.is_initialized()) {
        return false;
    }
    
    // Initialization does not verify the connection. Preserve completed sync
    // statistics and wait for a real sync/probe before reporting online.
    {
        std::lock_guard<std::mutex> lock(state_mu_);
        sync_state_.is_online = false;
        sync_state_.sync_interval = 30; // 30 seconds
        sync_state_.is_syncing = false;
        sync_state_.last_error.clear();
    }
    
    // Start sync timer
    start_sync_timer();
    
    // Subscribe to real-time updates
    subscribe_to_updates();
    
    return true;
}

void SyncManager::shutdown() {
    // Stop sync timer
    stop_sync_timer();

    // Unsubscribe from updates
    unsubscribe_from_updates();
    
    // Clear pending operations
    {
        std::lock_guard<std::mutex> lock(pending_mu_);
        pending_operations_.clear();
    }

    std::lock_guard<std::mutex> lock(state_mu_);
    sync_state_.is_online = false;
    sync_state_.is_syncing = false;
}

// ---------------------------------------------------------------------------
// Sync Timer
// ---------------------------------------------------------------------------

void SyncManager::start_sync_timer() {
    if (sync_timer_active_) {
        return;
    }
    
    sync_timer_active_ = true;
    sync_timer_thread_ = std::thread([this]() {
        while (sync_timer_active_) {
            int interval = 30;
            {
                std::lock_guard<std::mutex> lock(state_mu_);
                interval = sync_state_.sync_interval;
            }

            // Sleep in short steps so shutdown is immediate instead of waiting
            // out a full sync interval.
            for (int i = 0; i < interval && sync_timer_active_; ++i)
                std::this_thread::sleep_for(std::chrono::seconds(1));

            if (!sync_timer_active_) break;

            // The periodic sync is the only owner of online state: it marks the
            // manager offline on failure and recovers on the first sync that
            // succeeds and passes its connection probe.
            perform_sync(SyncType::Periodic);
        }
    });
}

void SyncManager::stop_sync_timer() {
    sync_timer_active_ = false;
    if (sync_timer_thread_.joinable()) {
        sync_timer_thread_.join();
    }
}

// ---------------------------------------------------------------------------
// Real-time Subscriptions
// ---------------------------------------------------------------------------

void SyncManager::subscribe_to_updates() {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    
    if (!supabase.is_authenticated()) {
        return;
    }
    
    // Subscribe to servers
    supabase.subscribe_to_servers([this](const aml::supabase::SupabaseServer& server) {
        handle_server_update(server);
    });
    
    // Subscribe to profiles
    supabase.subscribe_to_profiles([this](const aml::supabase::SupabaseProfile& profile) {
        handle_profile_update(profile);
    });
    
    // Subscribe to nodes
    // Note: This would need to be implemented in SupabaseManager
    
    // Subscribe to account activity
    supabase.subscribe_to_account_activity([this](const std::string& activity) {
        handle_account_activity(activity);
    });
}

void SyncManager::unsubscribe_from_updates() {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    supabase.unsubscribe_all();
}

// ---------------------------------------------------------------------------
// Sync Operations
// ---------------------------------------------------------------------------

bool SyncManager::perform_sync(SyncType type) {
    {
        std::lock_guard<std::mutex> lock(state_mu_);
        if (sync_state_.is_syncing) {
            return false;
        }
        sync_state_.is_syncing = true;
    }

    bool success = false;
    std::string exception_error;
    
    try {
        switch (type) {
            case SyncType::Full:
                success = perform_full_sync();
                break;
            case SyncType::Periodic:
                success = perform_periodic_sync();
                break;
            case SyncType::Manual:
                success = perform_manual_sync();
                break;
            case SyncType::OnDemand:
                success = perform_on_demand_sync();
                break;
        }

        if (success) {
            success = probe_connection();
        }
    } catch (const std::exception& e) {
        exception_error = e.what();
        success = false;
    }

    // These values describe completed work, so update them only after the
    // sync and its connection probe have returned.
    const int64_t completed_at = std::time(nullptr);
    {
        std::lock_guard<std::mutex> lock(state_mu_);
        sync_state_.is_syncing = false;
        sync_state_.total_syncs++;
        sync_state_.last_sync_time = completed_at;
        if (success) {
            sync_state_.successful_syncs++;
            sync_state_.last_error.clear();
            sync_state_.consecutive_failures = 0;
            sync_state_.is_online = true;
        } else {
            sync_state_.failed_syncs++;
            sync_state_.consecutive_failures++;
            sync_state_.is_online = false;
            if (!exception_error.empty()) {
                sync_state_.last_error = exception_error;
            } else if (sync_state_.last_error.empty()) {
                sync_state_.last_error = "Synchronization failed";
            }
        }
    }
    return success;
}

bool SyncManager::perform_full_sync() {
    // Full sync includes all data types
    bool success = true;
    
    success &= sync_servers();
    success &= sync_profiles();
    success &= sync_nodes();
    success &= sync_backups();
    success &= sync_modpacks();
    
    return success;
}

bool SyncManager::perform_periodic_sync() {
    // Periodic sync only syncs changed data
    bool success = true;
    
    success &= sync_servers();
    success &= sync_profiles();
    
    return success;
}

bool SyncManager::perform_manual_sync() {
    // Manual sync is the same as full sync
    return perform_full_sync();
}

bool SyncManager::perform_on_demand_sync() {
    // On-demand sync only syncs the requested data types
    std::lock_guard<std::mutex> lock(on_demand_mu_);
    
    bool success = true;
    for (const auto& type : on_demand_sync_types_) {
        switch (type) {
            case SyncDataType::Servers:
                success &= sync_servers();
                break;
            case SyncDataType::Profiles:
                success &= sync_profiles();
                break;
            case SyncDataType::Nodes:
                success &= sync_nodes();
                break;
            case SyncDataType::Backups:
                success &= sync_backups();
                break;
            case SyncDataType::Modpacks:
                success &= sync_modpacks();
                break;
            case SyncDataType::Friends:
            case SyncDataType::Messages:
            case SyncDataType::Parties:
            case SyncDataType::Presence:
                break;
        }
    }
    
    on_demand_sync_types_.clear();
    return success;
}

// ---------------------------------------------------------------------------
// Data Type Sync
// ---------------------------------------------------------------------------

bool SyncManager::sync_servers() {
    auto& server_manager = aml::servers::ServerManager::instance();
    auto& supabase = aml::supabase::SupabaseManager::instance();
    
    if (!supabase.is_authenticated()) {
        return false;
    }
    
    // Get servers from Supabase
    std::vector<aml::servers::Server> servers;
    try {
        const auto supa_servers = supabase.get_servers();

        // Convert before touching the replacement cache. A failed read or
        // conversion must leave the last-known-good server list intact.
        servers.reserve(supa_servers.size());
        for (const auto& supa_server : supa_servers) {
            servers.push_back(server_manager.convert_from_supabase(supa_server));
        }
    } catch (const std::exception& e) {
        remember_sync_error(state_mu_, sync_state_, e.what());
        return false;
    }
    
    // Update server manager
    {
        std::lock_guard<std::mutex> lock(server_manager.servers_mu_);
        server_manager.servers_ = std::move(servers);
    }
    
    return true;
}

bool SyncManager::sync_profiles() {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    
    if (!supabase.is_authenticated()) {
        return false;
    }
    
    // Get profiles from Supabase and reconcile the instance metadata locally.
    std::vector<aml::supabase::SupabaseProfile> profiles;
    try {
        profiles = supabase.get_profiles();
    } catch (const std::exception& e) {
        remember_sync_error(state_mu_, sync_state_, e.what());
        return false;
    }

    const std::wstring instances_dir = net::get_local_app_data_path() + L"\\instances";
    std::string scan_error;
    auto locals = aml::instances::scan(instances_dir, &scan_error);
    if (!scan_error.empty()) {
        remember_sync_error(state_mu_, sync_state_, scan_error);
        return false;
    }

    bool success = true;
    for (const auto& remote : profiles) {
        auto local_it = std::find_if(locals.begin(), locals.end(),
            [&remote](const aml::instances::Instance& local) {
                return local.id == remote.id;
            });

        if (local_it == locals.end()) {
            aml::instances::Instance source;
            source.id = remote.id;
            source.name = remote.name;
            source.minecraft_version = remote.minecraft_version;
            source.loader = remote.loader;
            source.loader_version = remote.loader_version;
            source.java_path = remote.java_path;
            source.memory_mb = remote.memory_mb;
            source.last_played = remote.last_played_at;
            aml::instances::Instance created;
            std::string operation_error;
            if (!aml::instances::create(instances_dir, source, created, &operation_error)) {
                remember_sync_error(state_mu_, sync_state_, operation_error);
                success = false;
                continue;
            }
            locals.push_back(std::move(created));
            continue;
        }

        apply_remote_profile(remote, *local_it);
        std::string operation_error;
        if (!aml::instances::save(*local_it, &operation_error)) {
            remember_sync_error(state_mu_, sync_state_, operation_error);
            success = false;
        }
    }

    return success;
}

bool SyncManager::sync_nodes() {
    auto& server_manager = aml::servers::ServerManager::instance();
    auto& supabase = aml::supabase::SupabaseManager::instance();
    
    if (!supabase.is_authenticated()) {
        return false;
    }
    
    // Get nodes from Supabase
    std::vector<aml::servers::Node> nodes;
    try {
        const auto supa_nodes = supabase.get_nodes();

        // Build the complete replacement before touching the local cache.
        nodes.reserve(supa_nodes.size());
        for (const auto& supa_node : supa_nodes) {
            nodes.push_back(server_manager.convert_from_supabase_node(supa_node));
        }
    } catch (const std::exception& e) {
        remember_sync_error(state_mu_, sync_state_, e.what());
        return false;
    }
    
    // Update server manager
    {
        std::lock_guard<std::mutex> lock(server_manager.nodes_mu_);
        server_manager.nodes_ = std::move(nodes);
    }
    
    return true;
}

bool SyncManager::sync_backups() {
    return sync_storage_bucket("backups", net::get_local_app_data_path() + L"\\backups");
}

bool SyncManager::sync_modpacks() {
    return sync_storage_bucket("modpacks", net::get_local_app_data_path() + L"\\modpacks");
}

// ---------------------------------------------------------------------------
// Real-time Update Handlers
// ---------------------------------------------------------------------------

void SyncManager::handle_server_update(const aml::supabase::SupabaseServer& supa_server) {
    auto& server_manager = aml::servers::ServerManager::instance();
    auto server = server_manager.convert_from_supabase(supa_server);
    
    // Update or add server
    {
        std::lock_guard<std::mutex> lock(server_manager.servers_mu_);
        bool found = false;
        for (auto& s : server_manager.servers_) {
            if (s.id == server.id) {
                s = server;
                found = true;
                break;
            }
        }
        if (!found) {
            server_manager.servers_.push_back(server);
        }
    }
    
    // Notify subscribers
    notify_subscribers(SyncDataType::Servers, server.id);
}

void SyncManager::handle_profile_update(const aml::supabase::SupabaseProfile& supa_profile) {
    // Reconcile the changed row through the same local representation used by
    // full sync. This keeps realtime and periodic sync behavior consistent.
    sync_profiles();
    notify_subscribers(SyncDataType::Profiles, supa_profile.id);
}

void SyncManager::handle_node_update(const aml::supabase::SupabaseNode& supa_node) {
    auto& server_manager = aml::servers::ServerManager::instance();
    auto node = server_manager.convert_from_supabase_node(supa_node);
    
    // Update or add node
    {
        std::lock_guard<std::mutex> lock(server_manager.nodes_mu_);
        bool found = false;
        for (auto& n : server_manager.nodes_) {
            if (n.id == node.id) {
                n = node;
                found = true;
                break;
            }
        }
        if (!found) {
            server_manager.nodes_.push_back(node);
        }
    }
    
    // Notify subscribers
    notify_subscribers(SyncDataType::Nodes, node.id);
}

void SyncManager::handle_account_activity(const std::string& /*activity*/) {
    // Parse activity and notify subscribers
    // For now, we'll just notify that account activity occurred
    notify_subscribers(SyncDataType::AccountActivity, "");
}

// ---------------------------------------------------------------------------
// Conflict Resolution
// ---------------------------------------------------------------------------

ConflictResolution SyncManager::resolve_conflict(
    const SyncConflict& conflict) {
    
    // Default conflict resolution strategy
    switch (conflict.resolution_strategy) {
        case ConflictResolutionStrategy::ServerWins:
            return ConflictResolution{true, conflict.server_value, "Server value used"};
            
        case ConflictResolutionStrategy::ClientWins:
            return ConflictResolution{true, conflict.client_value, "Client value used"};
            
        case ConflictResolutionStrategy::LatestWins:
            if (conflict.server_timestamp > conflict.client_timestamp) {
                return ConflictResolution{true, conflict.server_value, "Server value is newer"};
            } else {
                return ConflictResolution{true, conflict.client_value, "Client value is newer"};
            }
            
        case ConflictResolutionStrategy::Merge:
            // For merge, we'll concatenate strings or take the union of arrays
            if (conflict.data_type == SyncDataType::Servers) {
                // For servers, we'll use the server value
                return ConflictResolution{true, conflict.server_value, "Merged - server value used"};
            } else {
                // For other types, we'll use the client value
                return ConflictResolution{true, conflict.client_value, "Merged - client value used"};
            }
            
        default:
            return ConflictResolution{false, "", "Unknown resolution strategy"};
    }
}

// ---------------------------------------------------------------------------
// Offline Support
// ---------------------------------------------------------------------------

bool SyncManager::queue_operation(const SyncOperation& operation) {
    std::lock_guard<std::mutex> lock(pending_mu_);
    pending_operations_.push_back(operation);
    return true;
}

bool SyncManager::process_pending_operations() {
    if (pending_operations_.empty()) {
        return true;
    }
    
    auto& supabase = aml::supabase::SupabaseManager::instance();
    if (!supabase.is_authenticated()) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(pending_mu_);
    bool all_success = true;
    std::vector<SyncOperation> remaining;

    for (const auto& operation : pending_operations_) {
        bool success = false;
        
        switch (operation.data_type) {
            case SyncDataType::Servers:
                if (operation.operation_type == SyncOperationType::Delete) {
                    success = supabase.delete_server(operation.id);
                }
                break;
            case SyncDataType::Profiles:
                if (operation.operation_type == SyncOperationType::Delete) {
                    success = supabase.delete_profile(operation.id);
                } else {
                    aml::supabase::SupabaseProfile profile;
                    if (profile_from_operation(operation, profile)) {
                        success = operation.operation_type == SyncOperationType::Create
                            ? !supabase.create_profile(profile).id.empty()
                            : supabase.update_profile(profile);
                    }
                }
                break;
            case SyncDataType::Nodes:
                if (operation.operation_type == SyncOperationType::Delete) {
                    success = supabase.deregister_node(operation.id);
                }
                break;
            case SyncDataType::Backups:
                if (operation.operation_type == SyncOperationType::Delete) {
                    success = aml::storage::StorageManager::instance().delete_backup(operation.id);
                }
                break;
            case SyncDataType::Modpacks:
                if (operation.operation_type == SyncOperationType::Delete) {
                    success = aml::storage::StorageManager::instance().delete_modpack(operation.id);
                }
                break;
            case SyncDataType::Friends:
            case SyncDataType::Messages:
            case SyncDataType::Parties:
            case SyncDataType::Presence:
                // These data types have no write API in SyncManager yet.
                success = false;
                break;
            default:
                success = false;
                break;
        }
        
        if (!success) {
            all_success = false;
            remaining.push_back(operation);
        }
    }

    pending_operations_ = std::move(remaining);
    
    return all_success;
}

std::vector<SyncOperation> SyncManager::get_pending_operations() const {
    std::lock_guard<std::mutex> lock(pending_mu_);
    return pending_operations_;
}

// ---------------------------------------------------------------------------
// Subscription Management
// ---------------------------------------------------------------------------

bool SyncManager::subscribe(const SyncDataType type, const std::function<void(const std::string&)>& callback) {
    std::lock_guard<std::mutex> lock(subscribers_mu_);
    subscribers_[type].push_back(callback);
    return true;
}

bool SyncManager::unsubscribe(const SyncDataType type, const std::function<void(const std::string&)>& callback) {
    std::lock_guard<std::mutex> lock(subscribers_mu_);
    auto& callbacks = subscribers_[type];
    // std::function has no portable equality for captured callables. The
    // subscription API therefore removes the type's callback set as a whole.
    // Callers can re-register the callbacks they still need.
    (void)callback;
    callbacks.clear();
    return true;
}

void SyncManager::notify_subscribers(const SyncDataType type, const std::string& id) {
    std::lock_guard<std::mutex> lock(subscribers_mu_);
    auto it = subscribers_.find(type);
    if (it != subscribers_.end()) {
        for (const auto& callback : it->second) {
            try {
                callback(id);
            } catch (...) {
                // Ignore callback errors
            }
        }
    }
}

// ---------------------------------------------------------------------------
// On-demand Sync
// ---------------------------------------------------------------------------

bool SyncManager::request_sync(const SyncDataType type) {
    std::lock_guard<std::mutex> lock(on_demand_mu_);
    on_demand_sync_types_.push_back(type);
    
    // If we're not currently syncing, start an on-demand sync
    bool already_syncing = false;
    {
        std::lock_guard<std::mutex> state_lock(state_mu_);
        already_syncing = sync_state_.is_syncing;
    }
    if (!already_syncing) {
        std::thread([this]() {
            perform_sync(SyncType::OnDemand);
        }).detach();
    }
    
    return true;
}

// ---------------------------------------------------------------------------
// Sync State
// ---------------------------------------------------------------------------

SyncState SyncManager::get_sync_state() const {
    std::lock_guard<std::mutex> lock(state_mu_);
    return sync_state_;
}

bool SyncManager::is_online() const {
    std::lock_guard<std::mutex> lock(state_mu_);
    return sync_state_.is_online;
}

bool SyncManager::is_syncing() const {
    std::lock_guard<std::mutex> lock(state_mu_);
    return sync_state_.is_syncing;
}

std::string SyncManager::get_last_error() const {
    std::lock_guard<std::mutex> lock(state_mu_);
    return sync_state_.last_error;
}

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

SyncStats SyncManager::get_stats() const {
    SyncStats stats;

    {
        std::lock_guard<std::mutex> lock(state_mu_);
        stats.total_syncs = sync_state_.total_syncs;
        stats.successful_syncs = sync_state_.successful_syncs;
        stats.failed_syncs = sync_state_.failed_syncs;
        stats.last_sync_time = sync_state_.last_sync_time;
        stats.consecutive_failures = sync_state_.consecutive_failures;
        stats.is_online = sync_state_.is_online;
        stats.is_syncing = sync_state_.is_syncing;
    }

    {
        std::lock_guard<std::mutex> lock(pending_mu_);
        stats.pending_operations = static_cast<int>(pending_operations_.size());
    }
    
    return stats;
}

}  // namespace aml::sync
