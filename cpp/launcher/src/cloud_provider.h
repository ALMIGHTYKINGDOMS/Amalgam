#pragma once

#include "hosting_provider.h"

#include <atomic>
#include <mutex>

namespace aml::hosting {

// ---------------------------------------------------------------------------
// Website catalog provider
//
// The launcher does not provision or manage Amalgam Cloud servers. This
// provider supplies the approved read-only plan catalog for the Cloud landing
// page; all deployment, billing, and management happen on the website.
// Mutating operations deliberately fail with a website-managed explanation.
// ---------------------------------------------------------------------------

class WebsiteCatalogProvider : public IHostingProvider {
public:
    WebsiteCatalogProvider();

    // IHostingProvider
    std::string provider_name() const override;
    std::string provider_version() const override;
    bool is_connected() const override { return false; }
    bool is_dev_mode() const override { return false; }
    ProviderCapabilities capabilities() const override;

    bool get_plans(std::vector<CloudPlan>& plans, ProviderError* err) override;
    bool get_regions(std::vector<HostingRegion>& regions, ProviderError* err) override;
    bool get_supported_software(CloudGameType game,
                                 std::vector<CloudServerSoftware>& software,
                                 ProviderError* err) override;

    bool create_server(const DeploymentConfig& config,
                        std::string& server_id,
                        ProviderError* err) override;
    bool delete_server(const std::string& server_id,
                       ProviderError* err) override;
    bool start_server(const std::string& server_id,
                      ProviderError* err) override;
    bool stop_server(const std::string& server_id,
                     ProviderError* err) override;
    bool restart_server(const std::string& server_id,
                        ProviderError* err) override;

    bool get_server(const std::string& server_id,
                    CloudServer& server,
                    ProviderError* err) override;
    bool list_servers(std::vector<CloudServer>& servers,
                      ProviderError* err) override;
    bool get_metrics(const std::string& server_id,
                     ServerMetrics& metrics,
                     ProviderError* err) override;

    bool get_console(const std::string& server_id,
                     std::vector<ServerConsoleEntry>& entries,
                     int limit,
                     ProviderError* err) override;
    bool send_command(const std::string& server_id,
                      const std::string& command,
                      std::string* response,
                      ProviderError* err) override;

    bool list_files(const std::string& server_id,
                    const std::string& path,
                    std::vector<std::pair<std::string, bool>>& entries,
                    ProviderError* err) override;
    bool upload_file(const std::string& server_id,
                     const std::string& path,
                     const std::vector<uint8_t>& content,
                     ProviderError* err) override;
    bool download_file(const std::string& server_id,
                       const std::string& path,
                       std::vector<uint8_t>& content,
                       ProviderError* err) override;
    bool delete_file(const std::string& server_id,
                     const std::string& path,
                     ProviderError* err) override;

    bool create_backup(const std::string& server_id,
                       const std::string& name,
                       std::string* backup_id,
                       ProviderError* err) override;
    bool list_backups(const std::string& server_id,
                      std::vector<std::map<std::string, std::string>>& backups,
                      ProviderError* err) override;
    bool restore_backup(const std::string& server_id,
                        const std::string& backup_id,
                        ProviderError* err) override;

    bool validate_modpack(const std::string& profile_id,
                          std::vector<std::string>& warnings,
                          ProviderError* err) override;
    bool deploy_modpack(const std::string& server_id,
                        const std::string& profile_id,
                        ProviderError* err) override;

    bool get_subscription(const std::string& server_id,
                          Subscription& sub,
                          ProviderError* err) override;
    bool get_invoices(const std::string& server_id,
                      std::vector<Invoice>& invoices,
                      ProviderError* err) override;
    bool upgrade_plan(const std::string& server_id,
                      const std::string& new_plan_id,
                      ProviderError* err) override;

    bool get_provisioning_status(const std::string& job_id,
                                  ProvisioningProgress& progress,
                                  ProviderError* err) override;
};

// ---------------------------------------------------------------------------
// Read-only catalog accessor
// ---------------------------------------------------------------------------

IHostingProvider* cloud_provider();

}  // namespace aml::hosting
