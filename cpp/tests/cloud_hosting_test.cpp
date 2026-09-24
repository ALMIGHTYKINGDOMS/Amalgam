#include "cloud_provider.h"
#include "entitlements.h"
#include "hosting_provider.h"
#include "server_types.h"
#include "online_config.h"

#include <cassert>
#include <string>
#include <vector>

namespace {

using namespace aml::hosting;

bool test_dev_provider_basics() {
    auto* provider = cloud_provider();
    assert(provider != nullptr);
    assert(!provider->is_dev_mode());
    assert(!provider->is_connected());

    // Provider info
    assert(!provider->provider_name().empty());
    assert(!provider->provider_version().empty());
    return true;
}

bool test_dev_provider_capabilities() {
    auto* provider = cloud_provider();
    auto caps = provider->capabilities();
    // The launcher exposes no hosted-server controls; management is on the website.
    assert(!caps.console);
    assert(!caps.files);
    assert(!caps.backups);
    assert(!caps.metrics);
    assert(!caps.regions);
    assert(!caps.plan_upgrades);
    assert(!caps.sub_users);
    assert(!caps.custom_domains);
    return true;
}

bool test_dev_provider_plans() {
    auto* provider = cloud_provider();
    std::vector<CloudPlan> plans;
    ProviderError err;
    assert(!provider->get_plans(plans, &err));
    assert(plans.empty());
    assert(err.code == "WEBSITE_MANAGED");
    return true;
}

bool test_dev_provider_regions() {
    auto* provider = cloud_provider();
    std::vector<HostingRegion> regions;
    ProviderError err;
    assert(!provider->get_regions(regions, &err));
    assert(regions.empty());
    assert(err.code == "WEBSITE_MANAGED");
    return true;
}

bool test_dev_provider_software() {
    auto* provider = cloud_provider();
    std::vector<CloudServerSoftware> java;
    ProviderError err;
    assert(!provider->get_supported_software(CloudGameType::Java, java, &err));
    assert(java.empty());
    assert(err.code == "WEBSITE_MANAGED");
    return true;
}

bool test_dev_provider_no_fake_servers() {
    auto* provider = cloud_provider();
    std::vector<CloudServer> servers;
    ProviderError list_error;
    assert(!provider->list_servers(servers, &list_error));
    assert(servers.empty());
    assert(list_error.code == "WEBSITE_MANAGED");

    // Mutating operations must fail with a clear website-managed error.
    std::string server_id;
    DeploymentConfig cfg;
    cfg.plan_id = "cloud_8";
    ProviderError err;
    assert(!provider->create_server(cfg, server_id, &err));
    assert(err.code == "WEBSITE_MANAGED");
    assert(!err.message.empty());

    err = {};
    assert(!provider->start_server("fake-id", &err));
    assert(err.code == "WEBSITE_MANAGED");
    return true;
}

bool test_dev_provider_backups_empty() {
    auto* provider = cloud_provider();
    std::vector<std::map<std::string, std::string>> backups;
    ProviderError err;
    assert(!provider->list_backups("fake-id", backups, &err));
    assert(backups.empty());
    assert(err.code == "WEBSITE_MANAGED");
    return true;
}

bool test_dev_provider_invoices_are_website_managed() {
    auto* provider = cloud_provider();
    std::vector<Invoice> invoices;
    ProviderError err;
    assert(!provider->get_invoices("fake-id", invoices, &err));
    assert(invoices.empty());
    assert(err.code == "WEBSITE_MANAGED");
    return true;
}

bool test_website_links_are_canonical() {
    aml::online::OnlineConfig config;
    config.website_url = "https://amalgam-mc.com";
    assert(config.plans_url() == "https://amalgam-mc.com/plans");
    assert(config.cloud_url() == "https://amalgam-mc.com/cloud");
    assert(config.cloud_account_url() == "https://amalgam-mc.com/account/cloud");

    config.website_url = "https://example.test///";
    assert(config.page_url("plans") == "https://example.test/plans");
    assert(config.page_url("/account/membership") == "https://example.test/account/membership");

    // A configured host without a scheme must still become a link the shell can
    // open, and an empty value must fall back to the public site.
    config.website_url = "example.test";
    assert(config.page_url("plans") == "https://example.test/plans");
    config.website_url = "";
    assert(config.page_url("") == "https://amalgam-mc.com");
    return true;
}

bool test_account_links_share_the_official_origin() {
    aml::online::OnlineConfig config;
    config.website_url = "https://amalgam-mc.com/";
    assert(config.login_url() == "https://amalgam-mc.com/login?return=launcher");
    assert(config.register_url() == "https://amalgam-mc.com/register?return=launcher");
    assert(config.password_reset_url() ==
           "https://amalgam-mc.com/forgot-password?return=launcher");
    assert(config.account_url() == "https://amalgam-mc.com/account");

    // A configured origin must not move auth links to a second host or leave
    // duplicate separators that break the browser route.
    config.website_url = "https://accounts.example.test///";
    assert(config.login_url() ==
           "https://accounts.example.test/login?return=launcher");
    assert(config.register_url() ==
           "https://accounts.example.test/register?return=launcher");
    return true;
}

bool test_network_identity_is_canonical() {
    const aml::online::OnlineConfig config;
    assert(config.network_name == "Amalgam Network");
    assert(config.network_address == "play.amalgam-network.com");
    return true;
}

bool test_entitlements_model() {
    // Default entitlements: free plan, no premium features, zero quotas.
    aml::entitlements::AmalgamEntitlements e;
    assert(!e.valid);
    assert(e.plan.empty() || e.plan == "free");
    assert(e.turn_monthly_bytes == 0);
    assert(e.max_cloud_servers == 0);

    // Manager defaults: not refreshing, no remaining TURN without quota.
    auto& mgr = aml::entitlements::EntitlementManager::instance();
    assert(!mgr.refreshing());
    assert(mgr.turn_remaining_bytes() == 0);
    assert(mgr.turn_usage_fraction() == 0.0f);
    assert(!mgr.is_plus());
    return true;
}

bool test_entitlements_turn_math() {
    auto& mgr = aml::entitlements::EntitlementManager::instance();
    // invalidate ensures snapshot is not stale from other tests
    mgr.invalidate();
    auto e = mgr.snapshot();
    assert(!e.valid);
    return true;
}

}  // namespace

int main() {
    if (!test_dev_provider_basics() ||
        !test_dev_provider_capabilities() ||
        !test_dev_provider_plans() ||
        !test_dev_provider_regions() ||
        !test_dev_provider_software() ||
        !test_dev_provider_no_fake_servers() ||
        !test_dev_provider_backups_empty() ||
        !test_dev_provider_invoices_are_website_managed() ||
        !test_website_links_are_canonical() ||
        !test_account_links_share_the_official_origin() ||
        !test_network_identity_is_canonical() ||
        !test_entitlements_model() ||
        !test_entitlements_turn_math()) {
        return 1;
    }
    return 0;
}
