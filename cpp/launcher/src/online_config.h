#pragma once

#include <string>

namespace aml::online {

// ---------------------------------------------------------------------------
// OnlineConfig
//
// Central, client-safe configuration for talking to the Amalgam backend.
// ONLY public values belong here:
//   • public website URL
//   • public API URL
//   • Supabase project URL + publishable (anon) key
//
// NEVER store here: service-role keys, provider admin keys, Pterodactyl/Wings
// credentials, billing secrets, database passwords, or SSH credentials.
//
// The launcher is a secure client.  Everything sensitive stays on the backend.
// ---------------------------------------------------------------------------

struct OnlineConfig {
    // Public website (account management, plans, checkout, billing pages).
    std::string website_url = "https://amalgam-mc.com/";

    // The branded Amalgam network players join in game. Keep the canonical
    // hostname here; Minecraft's default Java port is implied when no port is
    // supplied, matching the public Amalgam Network address.
    std::string network_name = "Amalgam Network";
    std::string network_address = "play.amalgam-network.com";

    // Amalgam API backend (Replit or equivalent). The launcher talks ONLY to
    // this endpoint; it never talks to infrastructure providers directly.
    std::string api_url;

    // Supabase public project URL + publishable (anon) key.
    std::string supabase_url;
    std::string supabase_publishable_key;

    // Derived helpers. All browser links go through page_url() so neither a
    // custom website URL without a trailing slash nor a scheme-less host can
    // produce a link the browser cannot open.
    std::string page_url(const std::string& path) const {
        std::string root = website_url.empty() ? "https://amalgam-mc.com/" : website_url;
        if (root.rfind("http://", 0) != 0 && root.rfind("https://", 0) != 0)
            root.insert(0, "https://");
        while (root.size() > 1 && root.back() == '/') root.pop_back();
        if (path.empty()) return root;
        if (path.front() == '/') return root + path;
        return root + "/" + path;
    }

    std::string plans_url() const { return page_url("plans"); }
    std::string cloud_url() const { return page_url("cloud"); }
    std::string cloud_account_url() const { return page_url("account/cloud"); }
    std::string billing_url() const { return page_url("account/billing"); }
    std::string checkout_url(const std::string& checkout_token) const {
        return page_url("checkout/" + checkout_token);
    }
    std::string manage_membership_url() const { return page_url("account/membership"); }
    std::string terms_url() const { return page_url("terms"); }
    std::string privacy_url() const { return page_url("privacy"); }
    std::string microsoft_url() const { return page_url("microsoft"); }

    // The browser is the canonical surface for account recovery and for
    // users who prefer the official website's account UI.  Keep these links
    // on the same configured origin as the rest of the launcher so a custom
    // or stale host cannot silently split account flows across products.
    // `return=launcher` is a fixed, non-user-controlled hint; it never carries
    // credentials or a redirect destination.
    std::string login_url() const { return page_url("login?return=launcher"); }
    std::string register_url() const { return page_url("register?return=launcher"); }
    std::string password_reset_url() const {
        return page_url("forgot-password?return=launcher");
    }
    std::string account_url() const { return page_url("account"); }
};

// The single source of truth for online endpoints.  Populated at startup from
// config (launcher.json) with production-safe defaults for the public URL.
OnlineConfig& config();

}  // namespace aml::online
