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
    return "Amalgam Cloud Website Catalog";
}

std::string WebsiteCatalogProvider::provider_version() const {
    return "website-catalog-1";
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
// Read-only Amalgam Cloud plan catalog
// Pricing: amalgam-mc.com/plans
// ---------------------------------------------------------------------------

static std::vector<CloudPlan> make_reference_plans() {
    std::vector<CloudPlan> plans;

    auto add = [&](const char* id, const char* name, const char* tagline,
                   bool rec, int ram, const char* cpu, int storage,
                   int players, double price,
                   int backups,
                   std::initializer_list<CloudPlanFeature> feats) {
        (void)backups;
        CloudPlan p;
        p.id = id;
        p.name = name;
        p.tagline = tagline;
        p.recommended = rec;
        p.ram_mb = ram;
        p.cpu_label = cpu;
        p.storage_mb = storage;
        p.max_players = players;
        p.price_monthly = price;
        p.max_ram_mb = 32768;
        p.max_storage_mb = 327680;
        p.max_players_slider = 200;
        p.features.assign(feats);
        p.available = true;
        plans.push_back(std::move(p));
    };

    add("cloud_4", "Cloud 4", "AMALGAM CLOUD 4", false,
        4096, "Shared CPU", 25600, 10, 12.99, 3,
        {{"4 GB RAM"}, {"25 GB NVMe"}, {"3 Backups"},
         {"DDoS protection"}, {"Automatic backups"},
         {"Java & Bedrock support"}});

    add("cloud_8", "Cloud 8", "MOST POPULAR", true,
        8192, "Higher CPU", 51200, 25, 21.99, 7,
        {{"8 GB RAM"}, {"50 GB NVMe"}, {"7 Backups"},
         {"DDoS protection"}, {"Automatic backups"},
         {"Modded support"}, {"Java & Bedrock support"}});

    add("cloud_12", "Cloud 12", "HIGH PERFORMANCE", false,
        12288, "High-performance CPU", 102400, 50, 31.99, 14,
        {{"12 GB RAM"}, {"100 GB NVMe"}, {"14 Backups"},
         {"DDoS protection"}, {"Priority resources"},
         {"Automatic backups"}, {"Modded support"},
         {"Java & Bedrock support"}});

    return plans;
}

// ---------------------------------------------------------------------------
// IHostingProvider compatibility surface — hosted operations stay website-managed
// ---------------------------------------------------------------------------

bool WebsiteCatalogProvider::get_plans(std::vector<CloudPlan>& plans, ProviderError* err) {
    (void)err;
    // The landing page uses this read-only reference catalog.
    plans = make_reference_plans();
    return true;
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
    (void)err;
    // Never fabricate hosted servers in the launcher.
    servers.clear();
    return true;
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
    (void)server_id; (void)err; (void)invoices;
    invoices.clear();
    return true;
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
