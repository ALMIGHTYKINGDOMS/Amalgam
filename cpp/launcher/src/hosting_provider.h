#pragma once

#include "server_types.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace aml::hosting {

// server::ServerMetrics, server::ServerConsoleEntry come from server_types.h
using aml::server::ServerMetrics;
using aml::server::ServerConsoleEntry;

// ---------------------------------------------------------------------------
// Provider capability flags
// ---------------------------------------------------------------------------
// Different hosting providers support different functionality. The launcher
// only displays UI for capabilities the active provider advertises.

struct ProviderCapabilities {
    bool console            = false;
    bool files              = false;
    bool sftp               = false;
    bool backups            = false;
    bool metrics            = false;
    bool modpacks           = false;
    bool custom_domains     = false;
    bool scheduled_tasks    = false;
    bool regions            = false;
    bool server_software    = false;
    bool plan_upgrades      = false;
    bool snapshots          = false;
    bool sub_users          = false;
    bool metrics_history    = false;
    bool server_reinstall   = false;
    bool start_stop         = false;
    bool restart            = false;
};

// ---------------------------------------------------------------------------
// Cloud plan definition (remotely configurable)
// ---------------------------------------------------------------------------

struct CloudPlanFeature {
    std::string text;           // e.g. "4 GB RAM"
    bool included = true;
};

struct CloudPlan {
    std::string id;
    std::string name;           // e.g. "Starter"
    std::string tagline;        // e.g. "For small vanilla servers"
    bool recommended = false;

    // Resource specs (displayed in UI, authoritative from backend)
    int ram_mb = 2048;
    int storage_mb = 20480;
    std::string cpu_label;      // e.g. "Shared CPU" / "High-performance"
    int max_players = 10;

    // Pricing
    double price_monthly = 0.0;
    double price_hourly = 0.0;  // 0 = not available
    std::string currency = "USD";
    bool available = true;

    // Features displayed on the plan card
    std::vector<CloudPlanFeature> features;

    // Supported software (empty = all)
    std::vector<std::string> supported_software;

    // Maximum resource limits for slider/validation
    int max_ram_mb = 16384;
    int max_storage_mb = 163840;
    int max_players_slider = 100;
};

// ---------------------------------------------------------------------------
// Hosting region
// ---------------------------------------------------------------------------

struct HostingRegion {
    std::string id;
    std::string name;           // e.g. "US West"
    std::string display_name;
    std::string country_code;   // e.g. "US"
    std::string city;           // e.g. "Los Angeles"
    int latency_ms = -1;        // estimated latency, -1 = unknown
    bool recommended = false;
    bool available = true;
    double price_multiplier = 1.0;  // some regions cost more
};

// ---------------------------------------------------------------------------
// Server software option for deployment
// ---------------------------------------------------------------------------

struct CloudServerSoftware {
    std::string id;             // e.g. "paper"
    std::string name;           // e.g. "Paper"
    std::string category;       // e.g. "plugins" / "vanilla" / "modded"
    bool supports_mods = false;
    bool supports_plugins = false;
    bool requires_version = true;
    std::vector<std::string> supported_versions;
};

// ---------------------------------------------------------------------------
// Game type (Java / Bedrock)
// ---------------------------------------------------------------------------

enum class CloudGameType {
    Java,
    Bedrock
};

// ---------------------------------------------------------------------------
// Cloud server status
// ---------------------------------------------------------------------------

enum class CloudServerStatus {
    Provisioning,   // being created
    Starting,
    Running,
    Stopping,
    Stopped,
    Error,
    Suspended,      // billing issue
    Migrating,
    Updating
};

inline const char* cloud_status_label(CloudServerStatus s) {
    switch (s) {
        case CloudServerStatus::Provisioning: return "Provisioning";
        case CloudServerStatus::Starting:     return "Starting";
        case CloudServerStatus::Running:      return "Running";
        case CloudServerStatus::Stopping:     return "Stopping";
        case CloudServerStatus::Stopped:      return "Stopped";
        case CloudServerStatus::Error:        return "Error";
        case CloudServerStatus::Suspended:    return "Suspended";
        case CloudServerStatus::Migrating:    return "Migrating";
        case CloudServerStatus::Updating:     return "Updating";
    }
    return "Unknown";
}

inline bool cloud_status_is_active(CloudServerStatus s) {
    return s == CloudServerStatus::Running || s == CloudServerStatus::Starting;
}

// ---------------------------------------------------------------------------
// Cloud server instance (what the user owns)
// ---------------------------------------------------------------------------

struct CloudServer {
    std::string id;
    std::string name;
    std::string address;            // e.g. "play.example.com"
    int port = 25565;
    int bedrock_port = 19132;
    CloudServerStatus status = CloudServerStatus::Stopped;
    CloudGameType game_type = CloudGameType::Java;
    std::string plan_id;
    std::string region_id;
    std::string software_id;
    std::string minecraft_version;
    std::string software_version;

    // Runtime info
    int players_online = 0;
    int max_players = 20;
    float cpu_percent = 0.0f;
    float ram_percent = 0.0f;
    int ram_mb_used = 0;
    int ram_mb_total = 0;
    int storage_mb_used = 0;
    int storage_mb_total = 0;
    std::string uptime;             // e.g. "3d 14h"

    // Linked profile (if deployed from a profile)
    std::string linked_profile_id;

    // Status detail
    std::string status_message;
    std::string created_at;
    std::string next_billing_date;

    // Feature support (inherited from provider capabilities)
    ProviderCapabilities capabilities;
};

// ---------------------------------------------------------------------------
// Deployment wizard state
// ---------------------------------------------------------------------------

struct DeploymentConfig {
    // Step 1 - Plan
    std::string plan_id;

    // Step 2 - Game
    CloudGameType game_type = CloudGameType::Java;
    std::string software_id;
    std::string minecraft_version;

    // Step 3 - Region
    std::string region_id;

    // Step 4 - Configure
    std::string server_name;
    int max_players = 20;
    int allocated_ram_mb = 4096;
    int storage_mb = 20480;
    std::string difficulty;         // e.g. "normal"
    std::string gamemode;           // e.g. "survival"
    bool online_mode = true;
    bool whitelist = false;

    // Optional imports
    std::string import_profile_id;  // from Library
    std::string import_local_path;  // local server import
    std::string import_modpack_id;  // modpack to install

    // Filled by backend after validation
    std::string estimated_cost;
    bool valid = false;
    std::string validation_error;
};

// ---------------------------------------------------------------------------
// Provisioning job status
// ---------------------------------------------------------------------------

enum class ProvisioningStage {
    Allocating,
    InstallingJava,
    InstallingMinecraft,
    Configuring,
    ApplyingSettings,
    Starting,
    HealthCheck,
    Complete,
    Failed
};

struct ProvisioningProgress {
    ProvisioningStage stage = ProvisioningStage::Allocating;
    float progress = 0.0f;          // 0..1
    std::string stage_label;        // human-readable
    std::string detail;             // current action
    std::string error;              // non-empty on failure
    bool complete = false;
    std::string server_id;          // filled on success
    std::string server_address;     // filled on success
};

// ---------------------------------------------------------------------------
// Billing models (prepared, not tightly coupled)
// ---------------------------------------------------------------------------

struct Subscription {
    std::string id;
    std::string server_id;
    std::string plan_id;
    std::string status;             // active / past_due / cancelled
    double amount = 0.0;
    std::string currency = "USD";
    std::string billing_cycle;      // "monthly" / "hourly"
    std::string next_billing_date;
    std::string created_at;
    std::string cancelled_at;
};

struct Invoice {
    std::string id;
    std::string subscription_id;
    double amount = 0.0;
    std::string currency = "USD";
    std::string status;             // pending / paid / failed / refunded
    std::string description;
    std::string date;
    std::string paid_at;
};

struct Transaction {
    std::string id;
    std::string invoice_id;
    double amount = 0.0;
    std::string currency = "USD";
    std::string status;             // pending / completed / failed / refunded
    std::string payment_method;
    std::string created_at;
};

// ---------------------------------------------------------------------------
// Hosting provider interface (abstract)
//
// The Amalgam Launcher talks ONLY to the Amalgam hosting backend API.
// The backend handles provider adapters internally.
// This interface defines the contract the launcher expects.
// ---------------------------------------------------------------------------

struct ProviderError {
    std::string code;
    std::string message;
    bool retryable = false;
};

using ProviderCallback = std::function<void(bool success, const ProviderError& error)>;

class IHostingProvider {
public:
    virtual ~IHostingProvider() = default;

    // Provider info
    virtual std::string provider_name() const = 0;
    virtual std::string provider_version() const = 0;
    virtual bool is_connected() const = 0;
    virtual bool is_dev_mode() const = 0;
    virtual ProviderCapabilities capabilities() const = 0;

    // Plans
    virtual bool get_plans(std::vector<CloudPlan>& plans, ProviderError* err = nullptr) = 0;

    // Regions
    virtual bool get_regions(std::vector<HostingRegion>& regions, ProviderError* err = nullptr) = 0;

    // Supported software
    virtual bool get_supported_software(CloudGameType game,
                                         std::vector<CloudServerSoftware>& software,
                                         ProviderError* err = nullptr) = 0;

    // Server lifecycle
    virtual bool create_server(const DeploymentConfig& config,
                                std::string& server_id,
                                ProviderError* err = nullptr) = 0;

    virtual bool delete_server(const std::string& server_id,
                               ProviderError* err = nullptr) = 0;

    virtual bool start_server(const std::string& server_id,
                              ProviderError* err = nullptr) = 0;

    virtual bool stop_server(const std::string& server_id,
                             ProviderError* err = nullptr) = 0;

    virtual bool restart_server(const std::string& server_id,
                                ProviderError* err = nullptr) = 0;

    // Server info & status
    virtual bool get_server(const std::string& server_id,
                            CloudServer& server,
                            ProviderError* err = nullptr) = 0;

    virtual bool list_servers(std::vector<CloudServer>& servers,
                              ProviderError* err = nullptr) = 0;

    virtual bool get_metrics(const std::string& server_id,
                             ServerMetrics& metrics,
                             ProviderError* err = nullptr) = 0;

    // Console
    virtual bool get_console(const std::string& server_id,
                             std::vector<ServerConsoleEntry>& entries,
                             int limit = 100,
                             ProviderError* err = nullptr) = 0;

    virtual bool send_command(const std::string& server_id,
                              const std::string& command,
                              std::string* response = nullptr,
                              ProviderError* err = nullptr) = 0;

    // Files
    virtual bool list_files(const std::string& server_id,
                            const std::string& path,
                            std::vector<std::pair<std::string, bool>>& entries,
                            ProviderError* err = nullptr) = 0;

    virtual bool upload_file(const std::string& server_id,
                             const std::string& path,
                             const std::vector<uint8_t>& content,
                             ProviderError* err = nullptr) = 0;

    virtual bool download_file(const std::string& server_id,
                               const std::string& path,
                               std::vector<uint8_t>& content,
                               ProviderError* err = nullptr) = 0;

    virtual bool delete_file(const std::string& server_id,
                             const std::string& path,
                             ProviderError* err = nullptr) = 0;

    // Backups
    virtual bool create_backup(const std::string& server_id,
                               const std::string& name,
                               std::string* backup_id = nullptr,
                               ProviderError* err = nullptr) = 0;

    virtual bool list_backups(const std::string& server_id,
                              std::vector<std::map<std::string, std::string>>& backups,
                              ProviderError* err = nullptr) = 0;

    virtual bool restore_backup(const std::string& server_id,
                                const std::string& backup_id,
                                ProviderError* err = nullptr) = 0;

    // Modpack deployment
    virtual bool validate_modpack(const std::string& profile_id,
                                  std::vector<std::string>& warnings,
                                  ProviderError* err = nullptr) = 0;

    virtual bool deploy_modpack(const std::string& server_id,
                                const std::string& profile_id,
                                ProviderError* err = nullptr) = 0;

    // Billing (prepared, not connected until provider exists)
    virtual bool get_subscription(const std::string& server_id,
                                  Subscription& sub,
                                  ProviderError* err = nullptr) = 0;

    virtual bool get_invoices(const std::string& server_id,
                              std::vector<Invoice>& invoices,
                              ProviderError* err = nullptr) = 0;

    virtual bool upgrade_plan(const std::string& server_id,
                              const std::string& new_plan_id,
                              ProviderError* err = nullptr) = 0;

    // Provisioning progress (async polling)
    virtual bool get_provisioning_status(const std::string& job_id,
                                          ProvisioningProgress& progress,
                                          ProviderError* err = nullptr) = 0;
};

}  // namespace aml::hosting
