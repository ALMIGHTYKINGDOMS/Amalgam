#include "config.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

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
    const std::wstring unreadable_path = temporary_path(L"-unreadable.json");
    if (secure_path.empty() || legacy_path.empty() || unreadable_path.empty()) {
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
        !restored.ai_providers.empty()) {
        std::cerr << "encrypted config did not round trip\n";
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
    DeleteFileW(unreadable_path.c_str());
    return 0;
}
