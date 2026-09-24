#include "cloud_provider.h"
#include "config.h"
#include "net.h"

#include <algorithm>
#include <ctime>

namespace aml::hosting {

// ---------------------------------------------------------------------------
// WebsiteCatalogProvider
// ---------------------------------------------------------------------------

WebsiteCatalogProvider::WebsiteCatalogProvider() {}

std::string WebsiteCatalogProvider::provider_name() const {
    return "Amalgam Cloud Website Handoff";
}

std::string WebsiteCatalogProvider::provider_version() const {
    return "website-handoff-1";
}

ProviderCapabilities WebsiteCatalogProvider::capabilities() const {
    ProviderCapabilities caps;
    // The launcher consumes only the read-only plan catalog. Hosted-server
    // capabilities are intentionally false because management happens on the
    // website, not through this client.
    caps = ProviderCapabilities{};
    return caps;
}

static ProviderError website_managed_error() {
    return {"WEBSITE_MANAGED",
            "Amalgam Cloud hosting is managed on the Amalgam website. Open the website to deploy or manage a server.",
            false};
}

// ---------------------------------------------------------------------------
// IHostingProvider compatibility surface — hosted operations stay website-managed
// ---------------------------------------------------------------------------

bool WebsiteCatalogProvider::get_plans(std::vector<CloudPlan>& plans, ProviderError* err) {
    plans.clear();
    if (err) *err = website_managed_error();
    return false;
}

bool WebsiteCatalogProvider::get_regions(std::vector<HostingRegion>& regions, ProviderError* err) {
    regions.clear();
    if (err) *err = website_managed_error();
    return false;
}

bool WebsiteCatalogProvider::get_supported_software(CloudGameType /*game*/,
                                                 std::vector<CloudServerSoftware>& software,
                                                 ProviderError* err) {
    software.clear();
    if (err) *err = website_managed_error();
    return false;
}

bool WebsiteCatalogProvider::create_server(const DeploymentConfig& config,
                                        std::string& server_id,
                                        ProviderError* err) {
    (void)config; (void)server_id;
    if (err) *err = website_managed_error();
    return false;
}

bool WebsiteCatalogProvider::delete_server(const std::string& server_id,
                                       ProviderError* err) {
    (void)server_id;
    if (err) *err = website_managed_error();
    return false;
}

bool WebsiteCatalogProvider::start_server(const std::string& server_id,
                                      ProviderError* err) {
    (void)server_id;
    if (err) *err = website_managed_error();
    return false;
}

bool WebsiteCatalogProvider::stop_server(const std::string& server_id,
                                     ProviderError* err) {
    (void)server_id;
    if (err) *err = website_managed_error();
    return false;
}

bool WebsiteCatalogProvider::restart_server(const std::string& server_id,
                                        ProviderError* err) {
    (void)server_id;
    if (err) *err = website_managed_error();
    return false;
}

bool WebsiteCatalogProvider::get_server(const std::string& server_id,
                                    CloudServer& server,
                                    ProviderError* err) {
    (void)server_id; (void)server;
    if (err) *err = website_managed_error();
    return false;
}

bool WebsiteCatalogProvider::list_servers(std::vector<CloudServer>& servers,
                                       ProviderError* err) {
    servers.clear();
    if (err) *err = website_managed_error();
    return false;
}

bool WebsiteCatalogProvider::get_metrics(const std::string& server_id,
                                     ServerMetrics& metrics,
                                     ProviderError* err) {
    (void)server_id; (void)metrics;
    if (err) *err = website_managed_error();
    return false;
}

bool WebsiteCatalogProvider::get_console(const std::string& server_id,
                                     std::vector<ServerConsoleEntry>& entries,
                                     int limit,
                                     ProviderError* err) {
    (void)server_id; (void)entries; (void)limit;
    if (err) *err = website_managed_error();
    return false;
}

bool WebsiteCatalogProvider::send_command(const std::string& server_id,
                                      const std::string& command,
                                      std::string* response,
                                      ProviderError* err) {
    (void)server_id; (void)command; (void)response;
    if (err) *err = website_managed_error();
    return false;
}

bool WebsiteCatalogProvider::list_files(const std::string& server_id,
                                    const std::string& path,
                                    std::vector<std::pair<std::string, bool>>& entries,
                                    ProviderError* err) {
    (void)server_id; (void)path; (void)entries;
    if (err) *err = website_managed_error();
    return false;
}

bool WebsiteCatalogProvider::upload_file(const std::string& server_id,
                                     const std::string& path,
                                     const std::vector<uint8_t>& content,
                                     ProviderError* err) {
    (void)server_id; (void)path; (void)content;
    if (err) *err = website_managed_error();
    return false;
}

bool WebsiteCatalogProvider::download_file(const std::string& server_id,
                                       const std::string& path,
                                       std::vector<uint8_t>& content,
                                       ProviderError* err) {
    (void)server_id; (void)path; (void)content;
    if (err) *err = website_managed_error();
    return false;
}

bool WebsiteCatalogProvider::delete_file(const std::string& server_id,
                                     const std::string& path,
                                     ProviderError* err) {
    (void)server_id; (void)path;
    if (err) *err = website_managed_error();
    return false;
}

bool WebsiteCatalogProvider::create_backup(const std::string& server_id,
                                       const std::string& name,
                                       std::string* backup_id,
                                       ProviderError* err) {
    (void)server_id; (void)name; (void)backup_id;
    if (err) *err = website_managed_error();
    return false;
}

bool WebsiteCatalogProvider::list_backups(const std::string& server_id,
                                      std::vector<std::map<std::string, std::string>>& backups,
                                      ProviderError* err) {
    (void)server_id;
    backups.clear();
    if (err) *err = website_managed_error();
    return false;
}

bool WebsiteCatalogProvider::restore_backup(const std::string& server_id,
                                        const std::string& backup_id,
                                        ProviderError* err) {
    (void)server_id; (void)backup_id;
    if (err) *err = website_managed_error();
    return false;
}

bool WebsiteCatalogProvider::validate_modpack(const std::string& profile_id,
                                          std::vector<std::string>& warnings,
                                          ProviderError* err) {
    (void)profile_id; (void)warnings;
    if (err) *err = website_managed_error();
    return false;
}

bool WebsiteCatalogProvider::deploy_modpack(const std::string& server_id,
                                        const std::string& profile_id,
                                        ProviderError* err) {
    (void)server_id; (void)profile_id;
    if (err) *err = website_managed_error();
    return false;
}

bool WebsiteCatalogProvider::get_subscription(const std::string& server_id,
                                          Subscription& sub,
                                          ProviderError* err) {
    (void)server_id; (void)sub;
    if (err) *err = website_managed_error();
    return false;
}

bool WebsiteCatalogProvider::get_invoices(const std::string& server_id,
                                      std::vector<Invoice>& invoices,
                                      ProviderError* err) {
    (void)server_id;
    invoices.clear();
    if (err) *err = website_managed_error();
    return false;
}

bool WebsiteCatalogProvider::upgrade_plan(const std::string& server_id,
                                      const std::string& new_plan_id,
                                      ProviderError* err) {
    (void)server_id; (void)new_plan_id;
    if (err) *err = website_managed_error();
    return false;
}

bool WebsiteCatalogProvider::get_provisioning_status(const std::string& job_id,
                                                  ProvisioningProgress& progress,
                                                  ProviderError* err) {
    (void)job_id; (void)progress;
    if (err) *err = website_managed_error();
    return false;
}

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------

static WebsiteCatalogProvider s_website_catalog_provider;

IHostingProvider* cloud_provider() {
    return &s_website_catalog_provider;
}

}  // namespace aml::hosting
