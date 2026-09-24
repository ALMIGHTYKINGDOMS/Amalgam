#include "config.h"
#include "supabase.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <type_traits>
#include <utility>

namespace {

template <typename T, typename = void>
struct has_supabase_service_key_member : std::false_type {};

template <typename T>
struct has_supabase_service_key_member<
    T, std::void_t<decltype(std::declval<T&>().supabase_service_key)>> : std::true_type {};

template <typename T, typename = void>
struct has_service_key_member : std::false_type {};

template <typename T>
struct has_service_key_member<
    T, std::void_t<decltype(std::declval<T&>().service_key)>> : std::true_type {};

static_assert(!has_supabase_service_key_member<aml::config::Config>::value,
              "desktop configuration must not retain a Supabase service-role key");
static_assert(!has_service_key_member<aml::supabase::SupabaseConfig>::value,
              "desktop Supabase client configuration must not retain a service-role key");

std::wstring temporary_path(const wchar_t* suffix) {
    wchar_t directory[MAX_PATH]{};
    wchar_t file[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, directory) == 0 ||
        GetTempFileNameW(directory, L"aml", 0, file) == 0)
        return {};
    std::wstring path = file;
    DeleteFileW(path.c_str());
    return path + suffix;
}

std::string read_text(const std::wstring& path) {
    std::ifstream in(std::filesystem::path(path), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool write_text(const std::wstring& path, const std::string& text) {
    std::ofstream out(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    out << text;
    return static_cast<bool>(out);
}

}  // namespace

int main() {
    const std::wstring secure_path = temporary_path(L"-secure.json");
    const std::wstring legacy_path = temporary_path(L"-legacy.json");
    const std::wstring legacy_service_key_path = temporary_path(L"-legacy-service-key.json");
    const std::wstring unreadable_path = temporary_path(L"-unreadable.json");
    if (secure_path.empty() || legacy_path.empty() || legacy_service_key_path.empty() ||
        unreadable_path.empty()) {
        std::cerr << "could not create temporary config paths\n";
        return 1;
    }

    const std::string modrinth_secret = "modrinth-test-secret";
    const std::string curseforge_secret = "curseforge-test-secret";
    const std::string ai_secret = "ai-test-secret";
    aml::config::Config original;
    original.base_dir = L"C:\\Profiles";
    original.microsoft_client_id = "8a5f9e26-7d6a-4b18-8a77-e47be79a1439";
    original.launch_mode = "microsoft";
    original.auto_translate_project_text = true;
    original.translation_target_language = "Spanish";
    original.theme = "dracula";
    original.theme_font_size = 18.0f;
    original.high_contrast_mode = true;
    original.reduced_motion = true;
    original.color_vision_palette = true;
    original.color_vision_profile = "blue_yellow";
    original.keyboard_navigation = true;
    original.language = "en";
    original.date_format = "DD/MM/YYYY";
    original.time_format = "12-hour";
    original.theme_custom_colors["accent"] = "#1A73E8";
    original.modrinth_token = modrinth_secret;
    original.curseforge_key = curseforge_secret;
    original.admin_salt = "00112233445566778899aabbccddeeff";
    original.admin_verifier = "ffeeddccbbaa99887766554433221100";
    original.admin_iterations = 210000;
    if (!aml::config::save(secure_path, original)) {
        std::cerr << "could not save encrypted config\n";
        return 1;
    }

    const std::string raw = read_text(secure_path);
    if (raw.find("dpapi:v1:") == std::string::npos ||
        raw.find(modrinth_secret) != std::string::npos ||
        raw.find(curseforge_secret) != std::string::npos ||
        raw.find(ai_secret) != std::string::npos) {
        std::cerr << "config secrets were not protected at rest\n";
        return 1;
    }

    aml::config::Config restored;
    if (!aml::config::load(secure_path, restored) || restored.has_unreadable_secrets ||
        restored.secrets_need_migration || restored.modrinth_token != modrinth_secret ||
         restored.curseforge_key != curseforge_secret ||
         restored.admin_salt != original.admin_salt ||
         restored.admin_verifier != original.admin_verifier ||
         restored.admin_iterations != original.admin_iterations ||
         restored.microsoft_client_id != original.microsoft_client_id ||
        restored.launch_mode != original.launch_mode ||
        !restored.auto_translate_project_text ||
        restored.translation_target_language != original.translation_target_language ||
        restored.theme != original.theme ||
        restored.theme_font_size != original.theme_font_size ||
        !restored.high_contrast_mode || !restored.reduced_motion ||
        !restored.color_vision_palette ||
        restored.color_vision_profile != original.color_vision_profile ||
        !restored.keyboard_navigation || restored.language != "en" ||
        restored.date_format != original.date_format ||
        restored.time_format != original.time_format ||
        restored.theme_custom_colors["accent"] != original.theme_custom_colors["accent"] ||
        !restored.ai_providers.empty()) {
        std::cerr << "encrypted config did not round trip\n";
        return 1;
    }

    std::tm example{};
    example.tm_year = 124;  // 2024
    example.tm_mon = 0;     // January
    example.tm_mday = 5;
    example.tm_hour = 15;
    example.tm_min = 7;
    if (aml::config::format_local_date_time(example, restored) != "05/01/2024 03:07 PM") {
        std::cerr << "date/time preferences were not applied\n";
        return 1;
    }

    aml::config::Config malformed_preferences;
    malformed_preferences.theme = "made-up";
    malformed_preferences.theme_font_size = 1000.0f;
    malformed_preferences.color_vision_profile = "unsupported";
    malformed_preferences.language = "es";
    malformed_preferences.date_format = "invalid";
    malformed_preferences.time_format = "invalid";
    malformed_preferences.screen_reader_support = true;
    aml::config::normalize_presentation_preferences(malformed_preferences);
    if (malformed_preferences.theme != "default_dark" ||
        malformed_preferences.theme_font_size != 20.0f ||
        malformed_preferences.color_vision_profile != "red_green" ||
        malformed_preferences.language != "en" ||
        malformed_preferences.date_format != "YYYY-MM-DD" ||
        malformed_preferences.time_format != "24-hour" ||
        malformed_preferences.screen_reader_support) {
        std::cerr << "presentation preference normalization was not safe\n";
        return 1;
    }

    if (!write_text(legacy_path,
                    R"({"modrinth_token":"legacy-secret"})")) {
        std::cerr << "could not create legacy config\n";
        return 1;
    }
    aml::config::Config legacy;
    if (!aml::config::load(legacy_path, legacy) || !legacy.secrets_need_migration ||
        legacy.has_unreadable_secrets || legacy.modrinth_token != "legacy-secret" ||
        !legacy.ai_providers.empty()) {
        std::cerr << "legacy config migration was not detected\n";
        return 1;
    }
    if (!aml::config::save(legacy_path, legacy) || read_text(legacy_path).find("legacy-secret") != std::string::npos) {
        std::cerr << "legacy config was not migrated safely\n";
        return 1;
    }

    // A legacy service-role value may look like an encrypted secret, but the
    // desktop launcher must not even attempt to decrypt or retain it.
    constexpr const char* legacy_service_key_fixture = "dpapi:v1:not-base64";
    if (!write_text(legacy_service_key_path,
                    R"({"supabase_service_key":"dpapi:v1:not-base64"})")) {
        std::cerr << "could not create legacy Supabase service-key config\n";
        return 1;
    }
    aml::config::Config legacy_service_key;
    if (!aml::config::load(legacy_service_key_path, legacy_service_key) ||
        !legacy_service_key.legacy_supabase_service_key_ignored ||
        legacy_service_key.has_unreadable_secrets ||
        legacy_service_key.secrets_need_migration) {
        std::cerr << "legacy Supabase service key was not safely ignored\n";
        return 1;
    }
    if (!aml::config::save(legacy_service_key_path, legacy_service_key)) {
        std::cerr << "could not rewrite legacy Supabase service-key config\n";
        return 1;
    }
    const std::string migrated_service_key_config = read_text(legacy_service_key_path);
    if (migrated_service_key_config.find("supabase_service_key") != std::string::npos ||
        migrated_service_key_config.find(legacy_service_key_fixture) != std::string::npos) {
        std::cerr << "legacy Supabase service key was retained on save\n";
        return 1;
    }

    if (!write_text(unreadable_path, R"({"curseforge_key":"dpapi:v1:not-base64"})")) {
        std::cerr << "could not create unreadable config\n";
        return 1;
    }
    aml::config::Config unreadable;
    if (!aml::config::load(unreadable_path, unreadable) || !unreadable.has_unreadable_secrets ||
        aml::config::save(unreadable_path, unreadable)) {
        std::cerr << "unreadable protected config was not preserved safely\n";
        return 1;
    }

    DeleteFileW(secure_path.c_str());
    DeleteFileW(legacy_path.c_str());
    DeleteFileW(legacy_service_key_path.c_str());
    DeleteFileW(unreadable_path.c_str());
    return 0;
}
