#include "config.h"

#include "json.h"
#include "net.h"

#include <windows.h>
#include <wincrypt.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

namespace aml::config {

namespace {

constexpr char kSecretPrefix[] = "dpapi:v1:";

std::wstring resolve_config_path(const std::wstring& config_path, const std::string& value) {
    if (value.empty()) return L"";
    std::filesystem::path path(net::to_wide(value));
    if (path.is_absolute()) return path.lexically_normal().wstring();
    return (std::filesystem::path(config_path).parent_path() / path).lexically_normal().wstring();
}

std::string base64(const BYTE* data, DWORD size) {
    DWORD required = 0;
    if (!CryptBinaryToStringA(data, size, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                              nullptr, &required))
        return {};
    std::string out(required, '\0');
    if (!CryptBinaryToStringA(data, size, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                              out.data(), &required))
        return {};
    if (!out.empty() && out.back() == '\0') out.pop_back();
    return out;
}

bool from_base64(const std::string& value, std::vector<BYTE>& out) {
    DWORD required = 0;
    if (!CryptStringToBinaryA(value.c_str(), static_cast<DWORD>(value.size()),
                              CRYPT_STRING_BASE64, nullptr, &required, nullptr, nullptr))
        return false;
    out.resize(required);
    return CryptStringToBinaryA(value.c_str(), static_cast<DWORD>(value.size()),
                                CRYPT_STRING_BASE64, out.data(), &required, nullptr, nullptr) == TRUE;
}

bool protect_secret(const std::string& plain, std::string& encoded) {
    DATA_BLOB input{static_cast<DWORD>(plain.size()),
                    reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()))};
    DATA_BLOB output{};
    if (!CryptProtectData(&input, L"Amalgam launcher credential", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &output))
        return false;
    encoded = base64(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return !encoded.empty();
}

bool unprotect_secret(const std::string& encoded, std::string& plain) {
    std::vector<BYTE> bytes;
    if (!from_base64(encoded, bytes)) return false;
    DATA_BLOB input{static_cast<DWORD>(bytes.size()), bytes.data()};
    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &output))
        return false;
    plain.assign(reinterpret_cast<const char*>(output.pbData), output.cbData);
    LocalFree(output.pbData);
    return true;
}

std::string read_secret(const Json& root, const char* key, bool& needs_migration,
                        bool& unreadable) {
    std::string value = root.get(key).as_str();
    if (value.empty()) return {};
    constexpr size_t prefix_size = sizeof(kSecretPrefix) - 1;
    if (value.rfind(kSecretPrefix, 0) != 0) {
        needs_migration = true;
        return value;
    }
    std::string plain;
    if (!unprotect_secret(value.substr(prefix_size), plain)) {
        unreadable = true;
        return {};
    }
    return plain;
}

bool write_secret(Json& root, const char* key, const std::string& value) {
    if (value.empty()) {
        root.set(key, Json::str(""));
        return true;
    }
    std::string protected_value;
    if (!protect_secret(value, protected_value)) return false;
    root.set(key, Json::str(std::string(kSecretPrefix) + protected_value));
    return true;
}

}  // namespace

bool load(const std::wstring& path, Config& out) {
    Json j;
    std::string err;
    if (!json_parse_file(path, j, &err)) return false;
    out.java_overrides.clear();
    out.ai_providers.clear();
    out.servers.clear();
    out.secrets_need_migration = false;
    out.has_unreadable_secrets = false;
    out.base_dir = resolve_config_path(path, j.get("base_dir").as_str());
    out.assets_dir = resolve_config_path(path, j.get("assets_dir").as_str());
    out.java_cache_dir = resolve_config_path(path, j.get("java_cache_dir").as_str());
    out.username = net::to_wide(j.get("username").as_str());
    out.width = static_cast<int>(j.get("width").as_int(854));
    out.height = static_cast<int>(j.get("height").as_int(480));
    out.window_x = static_cast<int>(j.get("window_x").as_int(-1));
    out.window_y = static_cast<int>(j.get("window_y").as_int(-1));
    out.window_maximized = j.get("window_maximized").as_bool(false);
    out.loader = j.get("loader").as_str("auto");
    out.performance_profile = j.get("performance_profile").as_str("auto");
    out.extra_jvm = j.get("extra_jvm").as_str();
    out.auto_translate_project_text = j.get("auto_translate_project_text").as_bool(false);
    out.translation_target_language = j.get("translation_target_language").as_str("English");
    if (out.translation_target_language.empty()) out.translation_target_language = "English";
    out.microsoft_client_id = j.get("microsoft_client_id").as_str();
    out.test_server = net::to_wide(j.get("test_server").as_str());
    out.addon = j.get("addon").as_bool(true);
    out.advanced_mode = j.get("advanced_mode").as_bool(false);
    out.admin_salt = j.get("admin_salt").as_str();
    out.admin_verifier = j.get("admin_verifier").as_str();
    const int64_t admin_iterations = j.get("admin_iterations").as_int(210000);
    out.admin_iterations = admin_iterations >= 100000 && admin_iterations <= 10000000
                               ? static_cast<uint32_t>(admin_iterations)
                               : 210000;
    out.modrinth_token = read_secret(j, "modrinth_token", out.secrets_need_migration,
                                     out.has_unreadable_secrets);
    out.curseforge_key = read_secret(j, "curseforge_key", out.secrets_need_migration,
                                       out.has_unreadable_secrets);
    out.supabase_url = j.get("supabase_url").as_str();
    out.supabase_anon_key = read_secret(j, "supabase_anon_key", out.secrets_need_migration,
                                        out.has_unreadable_secrets);
    out.supabase_service_key = read_secret(j, "supabase_service_key", out.secrets_need_migration,
                                           out.has_unreadable_secrets);
    out.website_url = j.get("website_url").as_str("https://amalgam-mc.com/");
    out.api_url = j.get("api_url").as_str();
    // Legacy external AI settings are intentionally ignored. The production
    // AI path uses only the Amalgam-installed local runtimes and models.
    const Json& providers = j.get("ai_providers");
    for (const Json& p : providers.items()) {
        AiProvider ap;
        ap.name = p.get("name").as_str();
        ap.base_url = p.get("base_url").as_str();
        ap.api_key = read_secret(p, "api_key", out.secrets_need_migration,
                                 out.has_unreadable_secrets);
        ap.chat_model = p.get("chat_model").as_str();
        ap.image_model = p.get("image_model").as_str();
        if (ap.name.empty() && ap.base_url.empty() && ap.api_key.empty() &&
            ap.chat_model.empty())
            continue;
        out.ai_providers.push_back(ap);
    }
    // Do not synthesize an external provider from legacy fields. Local AI
    // uses installed models and runtimes, so an empty provider list is valid.
    const Json& overrides = j.get("java_overrides");
    for (const Json::Pair& p : overrides.pairs()) {
        int major = std::atoi(p.first.c_str());
        if (major > 0) out.java_overrides.emplace_back(major, net::to_wide(p.second.as_str()));
    }
    for (const Json& server : j.get("servers").items()) {
        Server entry;
        entry.name = server.get("name").as_str();
        entry.address = net::to_wide(server.get("address").as_str());
        entry.type = static_cast<int>(server.get("type").as_int(0));
        entry.profile = server.get("profile").as_str();
        if (!entry.name.empty() && !entry.address.empty()) out.servers.push_back(std::move(entry));
    }
    out.perf_hardware_acceleration = j.get("perf_hardware_acceleration").as_bool(true);
    out.perf_preloading = j.get("perf_preloading").as_bool(true);
    out.perf_compression = j.get("perf_compression").as_bool(true);
    out.perf_max_cache_gb = static_cast<int>(j.get("perf_max_cache_gb").as_int(5));
    out.perf_max_memory_mb = static_cast<int>(j.get("perf_max_memory_mb").as_int(4096));
    out.social_notifications = j.get("social_notifications").as_bool(true);
    out.social_show_offline = j.get("social_show_offline").as_bool(true);
    out.social_auto_accept = j.get("social_auto_accept").as_bool(false);
    out.mod_auto_update = j.get("mod_auto_update").as_bool(false);
    out.mod_check_on_startup = j.get("mod_check_on_startup").as_bool(true);
    out.mod_show_beta = j.get("mod_show_beta").as_bool(false);
    const Json& colors = j.get("theme_custom_colors");
    for (const Json::Pair& p : colors.pairs()) {
        if (!p.first.empty()) out.theme_custom_colors[p.first] = p.second.as_str();
    }
    out.theme_font_size = static_cast<float>(j.get("theme_font_size").as_num(14.0));
    return true;
}

bool save(const std::wstring& path, const Config& c) {
    // Never replace a protected credential that belongs to another Windows
    // profile with an empty value.  The UI offers an explicit recovery action
    // for that rare case, while command-line saves fail safely.
    if (c.has_unreadable_secrets) return false;
    Json j = Json::obj();
    j.set("base_dir", Json::str(net::to_utf8(c.base_dir)));
    j.set("assets_dir", Json::str(net::to_utf8(c.assets_dir)));
    j.set("java_cache_dir", Json::str(net::to_utf8(c.java_cache_dir)));
    j.set("username", Json::str(net::to_utf8(c.username)));
    j.set("width", Json::num(c.width));
    j.set("height", Json::num(c.height));
    j.set("window_x", Json::num(c.window_x));
    j.set("window_y", Json::num(c.window_y));
    j.set("window_maximized", Json::boolean(c.window_maximized));
    j.set("loader", Json::str(c.loader));
    j.set("performance_profile", Json::str(c.performance_profile));
    j.set("extra_jvm", Json::str(c.extra_jvm));
    j.set("auto_translate_project_text", Json::boolean(c.auto_translate_project_text));
    j.set("translation_target_language", Json::str(c.translation_target_language));
    j.set("microsoft_client_id", Json::str(c.microsoft_client_id));
    j.set("test_server", Json::str(net::to_utf8(c.test_server)));
    j.set("addon", Json::boolean(c.addon));
    j.set("advanced_mode", Json::boolean(c.advanced_mode));
    j.set("admin_salt", Json::str(c.admin_salt));
    j.set("admin_verifier", Json::str(c.admin_verifier));
    j.set("admin_iterations", Json::num(c.admin_iterations));
    if (!write_secret(j, "modrinth_token", c.modrinth_token)) return false;
    if (!write_secret(j, "curseforge_key", c.curseforge_key)) return false;
    j.set("supabase_url", Json::str(c.supabase_url));
    if (!write_secret(j, "supabase_anon_key", c.supabase_anon_key)) return false;
    if (!write_secret(j, "supabase_service_key", c.supabase_service_key)) return false;
    j.set("website_url", Json::str(c.website_url));
    j.set("api_url", Json::str(c.api_url));

    Json ov = Json::obj();
    for (const auto& kv : c.java_overrides) {
        ov.set(std::to_string(kv.first), Json::str(net::to_utf8(kv.second)));
    }
    j.set("java_overrides", ov);
    Json provs = Json::arr();
    for (const auto& ap : c.ai_providers) {
        Json p = Json::obj();
        p.set("name", Json::str(ap.name));
        p.set("base_url", Json::str(ap.base_url));
        if (!write_secret(p, "api_key", ap.api_key)) return false;
        p.set("chat_model", Json::str(ap.chat_model));
        p.set("image_model", Json::str(ap.image_model));
        provs.push(p);
    }
    j.set("ai_providers", provs);
    Json servers = Json::arr();
    for (const auto& server : c.servers) {
        Json entry = Json::obj();
        entry.set("name", Json::str(server.name));
        entry.set("address", Json::str(net::to_utf8(server.address)));
        entry.set("type", Json::num(server.type));
        if (!server.profile.empty()) entry.set("profile", Json::str(server.profile));
        servers.push(entry);
    }
    j.set("servers", servers);
    j.set("perf_hardware_acceleration", Json::boolean(c.perf_hardware_acceleration));
    j.set("perf_preloading", Json::boolean(c.perf_preloading));
    j.set("perf_compression", Json::boolean(c.perf_compression));
    j.set("perf_max_cache_gb", Json::num(c.perf_max_cache_gb));
    j.set("perf_max_memory_mb", Json::num(c.perf_max_memory_mb));
    j.set("social_notifications", Json::boolean(c.social_notifications));
    j.set("social_show_offline", Json::boolean(c.social_show_offline));
    j.set("social_auto_accept", Json::boolean(c.social_auto_accept));
    j.set("mod_auto_update", Json::boolean(c.mod_auto_update));
    j.set("mod_check_on_startup", Json::boolean(c.mod_check_on_startup));
    j.set("mod_show_beta", Json::boolean(c.mod_show_beta));
    Json colors = Json::obj();
    for (const auto& kv : c.theme_custom_colors) {
        colors.set(kv.first, Json::str(kv.second));
    }
    j.set("theme_custom_colors", colors);
    j.set("theme_font_size", Json::num(static_cast<double>(c.theme_font_size)));
    std::string err;
    return json_write_file(path, j, &err);
}

}  // namespace aml::config
