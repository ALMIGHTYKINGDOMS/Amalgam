#include "auth.h"

#include <windows.h>

#include <iostream>
#include <string>

namespace {

// auth::save/load/logout resolve through LOCALAPPDATA.  A test must never
// overwrite or remove the account that a developer uses to launch Minecraft,
// so give this process a fresh, uniquely-owned application-data root first.
class ScopedTestLocalAppData {
public:
    bool create(std::string* err) {
        const DWORD original_length = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
        if (original_length > 0) {
            original_.resize(original_length);
            const DWORD copied = GetEnvironmentVariableW(
                L"LOCALAPPDATA", original_.data(), original_length);
            if (copied == 0 || copied >= original_length) {
                if (err) *err = "could not read LOCALAPPDATA";
                original_.clear();
                return false;
            }
            original_.resize(copied);
            had_original_ = true;
        }

        wchar_t temp_dir[MAX_PATH]{};
        const DWORD temp_length = GetTempPathW(MAX_PATH, temp_dir);
        if (temp_length == 0 || temp_length >= MAX_PATH) {
            if (err) *err = "could not resolve a temporary directory";
            return false;
        }
        wchar_t candidate[MAX_PATH]{};
        if (GetTempFileNameW(temp_dir, L"aml", 0, candidate) == 0 ||
            !DeleteFileW(candidate) || !CreateDirectoryW(candidate, nullptr)) {
            if (err) *err = "could not create an isolated temporary account directory";
            return false;
        }
        root_ = candidate;
        if (!SetEnvironmentVariableW(L"LOCALAPPDATA", root_.c_str())) {
            if (err) *err = "could not isolate LOCALAPPDATA for the test";
            return false;
        }
        active_ = true;
        return true;
    }

    ~ScopedTestLocalAppData() {
        if (active_) {
            if (had_original_) SetEnvironmentVariableW(L"LOCALAPPDATA", original_.c_str());
            else SetEnvironmentVariableW(L"LOCALAPPDATA", nullptr);
        }
        if (!root_.empty()) {
            // The path comes directly from GetTempFileNameW and belongs only
            // to this test process; never sweep a shared user directory.
            DeleteFileW((root_ + L"\\Amalgam\\account.json").c_str());
            RemoveDirectoryW((root_ + L"\\Amalgam").c_str());
            RemoveDirectoryW(root_.c_str());
        }
    }

private:
    std::wstring root_;
    std::wstring original_;
    bool had_original_ = false;
    bool active_ = false;
};

}  // namespace

int main() {
    ScopedTestLocalAppData isolated_account_root;
    std::string isolation_error;
    if (!isolated_account_root.create(&isolation_error)) {
        std::cerr << "could not isolate auth test storage: " << isolation_error << "\n";
        return 1;
    }

    // valid_client_id(): valid GUID
    if (!aml::auth::valid_client_id("f63edc17-8c97-4b03-9887-e63b23444a93")) {
        std::cerr << "valid_client_id() rejected valid GUID\n";
        return 1;
    }
    // valid_client_id(): empty string
    if (aml::auth::valid_client_id("")) {
        std::cerr << "valid_client_id() accepted empty string\n";
        return 1;
    }
    // valid_client_id(): too short
    if (aml::auth::valid_client_id("f63edc17-8c97-4b03-9887")) {
        std::cerr << "valid_client_id() accepted short string\n";
        return 1;
    }
    // valid_client_id(): missing dash at position 8
    if (aml::auth::valid_client_id("f63edc178c97-4b03-9887-e63b23444a93")) {
        std::cerr << "valid_client_id() accepted missing dash\n";
        return 1;
    }
    // valid_client_id(): non-hex character
    if (aml::auth::valid_client_id("f63edc1z-8c97-4b03-9887-e63b23444a93")) {
        std::cerr << "valid_client_id() accepted non-hex char\n";
        return 1;
    }
    // valid_client_id(): all dashes
    if (aml::auth::valid_client_id("--------------------")) {
        std::cerr << "valid_client_id() accepted all dashes\n";
        return 1;
    }
    // Account struct defaults
    {
        aml::auth::Account a;
        if (!a.username.empty() || !a.uuid.empty() || a.expires_at != 0) {
            std::cerr << "Account defaults incorrect\n";
            return 1;
        }
    }
    // The sandbox must start with no account file.
    {
        aml::auth::Account a;
        std::string err;
        if (aml::auth::load(a, &err)) {
            std::cerr << "fresh isolated account storage unexpectedly loaded an account\n";
            return 1;
        }
    }
    // save() + load() round-trip
    {
        aml::auth::Account a;
        a.username = "TestUser";
        a.uuid = "00000000-0000-0000-0000-000000000001";
        a.access_token = "test_access_token_value";
        a.refresh_token = "test_refresh_token_value";
        a.microsoft_client_id = "f63edc17-8c97-4b03-9887-e63b23444a93";
        a.expires_at = 9999999999LL;
        std::string err;
        if (!aml::auth::save(a, &err)) {
            std::cerr << "save() failed: " << err << "\n";
            return 1;
        }
        aml::auth::Account loaded;
        if (!aml::auth::load(loaded, &err)) {
            std::cerr << "load() failed after save: " << err << "\n";
            return 1;
        }
        if (loaded.username != "TestUser" || loaded.uuid != "00000000-0000-0000-0000-000000000001") {
            std::cerr << "round-trip username/uuid mismatch\n";
            return 1;
        }
        if (loaded.microsoft_client_id != a.microsoft_client_id) {
            std::cerr << "round-trip client_id mismatch\n";
            return 1;
        }
        if (loaded.expires_at != a.expires_at) {
            std::cerr << "round-trip expires_at mismatch\n";
            return 1;
        }
    }
    // logout(): cleanup only the isolated account file, then prove it is gone.
    {
        std::string err;
        if (!aml::auth::logout(&err)) {
            std::cerr << "logout() failed: " << err << "\n";
            return 1;
        }
        aml::auth::Account logged_out;
        if (aml::auth::load(logged_out, &err)) {
            std::cerr << "logout() did not remove the isolated account\n";
            return 1;
        }
    }
    std::cout << "auth_test passed\n";
    return 0;
}
