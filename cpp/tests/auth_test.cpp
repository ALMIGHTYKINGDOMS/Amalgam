#include "auth.h"

#include <iostream>
#include <string>

int main() {
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
    // load(): no account file returns false
    {
        aml::auth::Account a;
        std::string err;
        // This may or may not fail depending on whether an account file exists.
        // We just verify it doesn't crash.
        aml::auth::load(a, &err);
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
    // logout(): cleanup
    {
        std::string err;
        aml::auth::logout(&err);
    }
    std::cout << "auth_test passed\n";
    return 0;
}
