#pragma once

#include <string>
#include <vector>
#include <map>
#include <functional>
#include <mutex>
#include <thread>
#include <atomic>
#include <ctime>

#include "supabase.h"

namespace aml::sync {

// ---------------------------------------------------------------------------
// Sync Types
// ---------------------------------------------------------------------------

enum class SyncType {
    Full,       // Sync all data
    Periodic,   // Regular periodic sync
    Manual,     // User-initiated sync
    OnDemand    // Sync specific data types
};

enum class SyncDataType {
    Servers,
    Profiles,
    Nodes,
    Backups,
    Modpacks,
    AccountActivity,
    Friends,
    Messages,
    Parties,
    Presence,
    All
};

inline const char* sync_data_type_name(SyncDataType type) {
    switch (type) {
        case SyncDataType::Servers: return "Servers";
        case SyncDataType::Profiles: return "Profiles";
        case SyncDataType::Nodes: return "Nodes";
        case SyncDataType::Backups: return "Backups";
        case SyncDataType::Modpacks: return "Modpacks";
        case SyncDataType::AccountActivity: return "Account Activity";
        case SyncDataType::Friends: return "Friends";
        case SyncDataType::Messages: return "Messages";
        case SyncDataType::Parties: return "Parties";
        case SyncDataType::Presence: return "Presence";
        default: return "All";
    }
}

// ---------------------------------------------------------------------------
// Sync State
// ---------------------------------------------------------------------------

struct SyncState {
    bool is_online = false;
    bool is_syncing = false;
    int64_t last_sync_time = 0;
    int sync_interval = 30; // seconds
    std::string last_error;
    int consecutive_failures = 0;
    int total_syncs = 0;
    int successful_syncs = 0;
    int failed_syncs = 0;
};

// ---------------------------------------------------------------------------
// Sync Statistics
// ---------------------------------------------------------------------------

struct SyncStats {
    int total_syncs = 0;
    int successful_syncs = 0;
    int failed_syncs = 0;
    int64_t last_sync_time = 0;
    int consecutive_failures = 0;
    bool is_online = false;
    bool is_syncing = false;
};

// ---------------------------------------------------------------------------
// Sync Operation
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Conflict Resolution
// ---------------------------------------------------------------------------

enum class ConflictResolutionStrategy {
    ServerWins,    // Server value always wins
    ClientWins,    // Client value always wins
    LatestWins,    // Most recent value wins
    Merge          // Merge values
};

struct SyncConflict {
    SyncDataType data_type;
    std::string id;
    std::string server_value;
    std::string client_value;
    int64_t server_timestamp = 0;
    int64_t client_timestamp = 0;
    ConflictResolutionStrategy resolution_strategy = ConflictResolutionStrategy::LatestWins;
};

struct ConflictResolution {
    bool success = false;
    std::string resolved_value;
    std::string message;
};

// ---------------------------------------------------------------------------
// Sync Manager
// ---------------------------------------------------------------------------

class SyncManager {
public:
    static SyncManager& instance();
    
    // Initialization
    bool initialize();
    void shutdown();
    
    // Sync Operations
    bool perform_sync(SyncType type);
    bool perform_full_sync();
    bool perform_periodic_sync();
    bool perform_manual_sync();
    bool perform_on_demand_sync();
    
    // Data Type Sync
    bool sync_servers();
    bool sync_profiles();
    bool sync_nodes();
    bool sync_backups();
    bool sync_modpacks();
    
    // Real-time Update Handlers
    void handle_server_update(const aml::supabase::SupabaseServer& server);
    void handle_profile_update(const aml::supabase::SupabaseProfile& profile);
    void handle_node_update(const aml::supabase::SupabaseNode& node);
    void handle_account_activity(const std::string& activity);
    
    // Conflict Resolution
    ConflictResolution resolve_conflict(const SyncConflict& conflict);
    
    // Subscription Management
    bool subscribe(SyncDataType type, const std::function<void(const std::string&)>& callback);
    bool unsubscribe(SyncDataType type, const std::function<void(const std::string&)>& callback);
    void notify_subscribers(SyncDataType type, const std::string& id);
    
    // On-demand Sync
    bool request_sync(SyncDataType type);
    
    // Sync State
    SyncState get_sync_state() const;
    bool is_online() const;
    bool is_syncing() const;
    std::string get_last_error() const;
    
    // Statistics
    SyncStats get_stats() const;

private:
    SyncManager();
    ~SyncManager();
    
    // Prevent copying
    SyncManager(const SyncManager&) = delete;
    SyncManager& operator=(const SyncManager&) = delete;
    
    // Sync Timer
    void start_sync_timer();
    void stop_sync_timer();
    
    // Real-time Subscriptions
    void subscribe_to_updates();
    void unsubscribe_from_updates();
    
    // State
    mutable std::mutex state_mu_;
    SyncState sync_state_;
    
    // Subscribers
    mutable std::mutex subscribers_mu_;
    std::map<SyncDataType, std::vector<std::function<void(const std::string&)>>> subscribers_;
    
    // On-demand Sync Types
    mutable std::mutex on_demand_mu_;
    std::vector<SyncDataType> on_demand_sync_types_;
    
    // Threads
    std::atomic<bool> sync_timer_active_{false};
    std::thread sync_timer_thread_;
};

}  // namespace aml::sync
