#include "bedrock.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>

int main() {
    namespace fs = std::filesystem;
    auto temporary_path = [](const wchar_t* suffix) -> std::wstring {
        wchar_t directory[MAX_PATH]{};
        wchar_t file[MAX_PATH]{};
        if (GetTempPathW(MAX_PATH, directory) == 0 ||
            GetTempFileNameW(directory, L"aml", 0, file) == 0)
            return {};
        std::wstring path = file;
        DeleteFileW(path.c_str());
        return path + suffix;
    };
    if (aml::bedrock::uwp_family() != L"Microsoft.MinecraftUWP_8wekyb3d8bbwe") {
        std::cerr << "FAILED: Bedrock package family changed unexpectedly\n";
        return 1;
    }
    fs::path root = temporary_path(L"_bedrock_test");
    fs::remove_all(root);
    fs::create_directories(root);
    fs::path valid = root / "example.mcpack";
    const char valid_bytes[] = {'P', 'K', 3, 4, 'a', 'd', 'd', 'o', 'n'};
    std::ofstream valid_file(valid, std::ios::binary);
    valid_file.write(valid_bytes, sizeof(valid_bytes));
    valid_file.close();
    std::string error;
    if (!aml::bedrock::validate_addon_file(valid.wstring(), &error)) {
        std::cerr << "FAILED: valid addon rejected: " << error << "\n";
        return 1;
    }
    fs::path invalid = root / "example.txt";
    std::ofstream invalid_file(invalid, std::ios::binary);
    invalid_file << "not an addon";
    invalid_file.close();
    if (aml::bedrock::validate_addon_file(invalid.wstring(), &error)) {
        std::cerr << "FAILED: invalid addon accepted\n";
        return 1;
    }
    fs::remove_all(root);
    return 0;
}
