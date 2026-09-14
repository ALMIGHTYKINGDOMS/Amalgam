#include "mods.h"

#include <iostream>

namespace {

bool test_pick_file() {
    aml::mods::ModInfo mod;
    mod.files = {
        {"old", "old.jar", "", {"fabric"}, {"1.21.8"}, false, 10, ""},
        {"new", "new.jar", "", {"fabric"}, {"1.21.8"}, true, 20, ""},
        {"neo", "neo.jar", "", {"neoforge"}, {"1.21.8"}, true, 20, ""},
    };
    return aml::mods::pick_file(mod, "fabric", "1.21.8") == "new" &&
           aml::mods::pick_file(mod, "neoforge", "1.21.8") == "neo" &&
           aml::mods::pick_file(mod, "forge", "1.21.8").empty();
}

bool test_select_pack_release_ignores_stale_vanilla_filter() {
    aml::mods::ModInfo pack;
    pack.files = {
        {"forge-1201", "pack-forge.zip", "https://example.invalid/pack.zip",
         {"forge"}, {"1.20.1"}, true, 100, "sha1"},
    };
    aml::mods::CompatibleRelease release;
    std::string err;
    const bool ok = aml::mods::select_compatible_release(
        pack, "vanilla", "1.20.1", release, &err);
    return ok && err.empty() && release.file_id == "forge-1201" &&
           release.loader == "forge" && release.game_version == "1.20.1";
}

bool test_source_name() {
    aml::mods::Match match;
    match.source = "curseforge";
    return match.source == "curseforge";
}

bool test_provider_normalization() {
    return aml::mods::canonical_source("Modrinth") == "modrinth" &&
           aml::mods::canonical_source("CURSEFORGE") == "curseforge" &&
           aml::mods::canonical_source("unknown").empty();
}

bool test_provider_validation() {
    aml::mods::ApiCfg cfg;
    std::string err;
    return !aml::mods::test_provider(cfg, "unknown", &err) &&
           err == "unsupported content provider";
}

bool test_curseforge_key_normalization() {
    return aml::mods::normalize_curseforge_key("  \"Bearer cf-key\"\r\n") == "cf-key" &&
           aml::mods::normalize_curseforge_key(" x-api-key: cf-key ") == "cf-key" &&
           aml::mods::normalize_curseforge_key("\t\n").empty();
}

bool test_secure_curseforge_proxy_configuration() {
    const auto signed_out = aml::mods::make_api_cfg(
        "", "", "https://example.supabase.co/", "publishable", "");
    const auto signed_in = aml::mods::make_api_cfg(
        "", "", "https://example.supabase.co/", "publishable", "user-token");
    const auto personal = aml::mods::make_api_cfg("", " personal-key ");
    return signed_out.curseforge_proxy_url ==
               "https://example.supabase.co/functions/v1/curseforge-catalog" &&
           aml::mods::curseforge_proxy_configured(signed_out) &&
           !aml::mods::curseforge_available(signed_out) &&
           aml::mods::curseforge_available(signed_in) &&
           aml::mods::curseforge_available(personal);
}

bool test_secure_curseforge_proxy_requires_sign_in() {
    const auto signed_out = aml::mods::make_api_cfg(
        "", "", "https://example.supabase.co/", "publishable", "");
    std::string error;
    return !aml::mods::test_provider(signed_out, "curseforge", &error) &&
           error == "Sign in to an Amalgam account to use the shared CurseForge catalog";
}

}  // namespace

int main() {
    if (!test_pick_file() || !test_select_pack_release_ignores_stale_vanilla_filter() ||
        !test_source_name() || !test_provider_normalization() ||
        !test_provider_validation() || !test_curseforge_key_normalization() ||
        !test_secure_curseforge_proxy_configuration() ||
        !test_secure_curseforge_proxy_requires_sign_in()) {
        std::cerr << "FAILED: mods\n";
        return 1;
    }
    return 0;
}
