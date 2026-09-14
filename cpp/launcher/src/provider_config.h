#pragma once

#include "config.h"
#include "mods.h"
#include "supabase.h"

namespace aml::provider_config {

// Build the provider configuration from the launcher's protected local config
// and, when available, the current Amalgam account session. Provider secrets
// stay on the user's machine or in the Supabase Edge Function environment.
inline mods::ApiCfg make(const config::Config& cfg) {
    std::string access_token;
    if (auto* client = supabase::SupabaseManager::instance().client()) {
        access_token = client->current_access_token();
    }
    return mods::make_api_cfg(cfg.modrinth_token, cfg.curseforge_key,
                              cfg.supabase_url, cfg.supabase_anon_key,
                              access_token);
}

}  // namespace aml::provider_config
