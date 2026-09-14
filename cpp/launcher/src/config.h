#pragma once

#include <string>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace aml::config {

// Legacy provider-shaped data is retained only so older launcher.json files
// can be read without data loss. The production AI path ignores these fields
// and uses the bundled runtimes/models discovered by ai.cpp.
struct AiProvider {
    std::string name;
    std::string base_url;
    std::string api_key;
    std::string chat_model;
    std::string image_model;
};

struct Server {
    std::string name;
    std::wstring address;
    // 0 = Java, 1 = Bedrock, 2 = Modded (Java Forge/Fabric/NeoForge),
    // 3 = Bedrock Modded (addons/modpacks).
    int type = 0;
    // Optional profile/instance id to launch with this server.
    std::string profile;
};

struct Config {
    std::wstring base_dir;
    std::wstring assets_dir;
    std::wstring java_cache_dir;
    std::wstring username;
    int width = 854;
    int height = 480;
    int window_x = -1;
    int window_y = -1;
    bool window_maximized = false;
    std::string loader = "auto";
    std::string performance_profile = "auto";
    std::string extra_jvm;
    // Project pages can optionally translate public provider descriptions with
    // an AI provider configured by the player. Disabled by default so content
    // is never sent to a provider without an explicit choice.
    bool auto_translate_project_text = false;
    std::string translation_target_language = "English";
    // Public identifier of the Microsoft Entra application used for the
    // device-code sign-in.  This is intentionally not encrypted: a client ID
    // identifies the launcher, not a player or a secret.
    std::string microsoft_client_id;
    std::wstring test_server;
    bool addon = true;
    bool advanced_mode = false;
    std::vector<std::pair<int, std::wstring>> java_overrides;
    // Secrets are held in memory as plaintext, but config::save protects them
    // with Windows DPAPI before they are persisted.  Older plaintext files are
    // accepted once and migrated on the next successful save.
    std::string modrinth_token;
    std::string curseforge_key;
    std::string supabase_url;
    std::string supabase_anon_key;
    std::string supabase_service_key;
    // Public client-safe endpoints for the Amalgam online services. Never
    // store secrets here — only URLs that are safe to ship in the client.
    std::string website_url;
    std::string api_url;
    // Retained only for compatibility with older configuration structure;
    // the built-in local AI runtime does not use remote provider settings.
    std::vector<AiProvider> ai_providers;
    std::vector<Server> servers;
    // The Admin password is represented by a salted PBKDF2 verifier. It is
    // intentionally separate from provider credentials, which use DPAPI.
    std::string admin_salt;
    std::string admin_verifier;
    uint32_t admin_iterations = 210000;

    // Player-facing callers can surface these as an actionable warning rather
    // than silently dropping credentials that were protected for another
    // Windows account or an unavailable DPAPI profile.
    bool secrets_need_migration = false;
    bool has_unreadable_secrets = false;

    // Theme / Appearance
    std::string theme = "default_dark";

    // Accessibility
    bool keyboard_navigation = false;
    bool screen_reader_support = false;

    // Localization
    std::string language = "en";
    std::string date_format = "YYYY-MM-DD";
    std::string time_format = "24-hour";

    // Performance settings
    bool perf_hardware_acceleration = true;
    bool perf_preloading = true;
    bool perf_compression = true;
    int perf_max_cache_gb = 5;
    int perf_max_memory_mb = 4096;

    // Social settings
    bool social_notifications = true;
    bool social_show_offline = true;
    bool social_auto_accept = false;

    // Mod manager settings
    bool mod_auto_update = false;
    bool mod_check_on_startup = true;
    bool mod_show_beta = false;

    // Theme custom colors (role -> "#RRGGBBAA")
    std::map<std::string, std::string> theme_custom_colors;
    float theme_font_size = 14.0f;
};

bool load(const std::wstring& path, Config& out);
bool save(const std::wstring& path, const Config& c);

}  // namespace aml::config
