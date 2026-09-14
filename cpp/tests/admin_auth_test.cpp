#include "admin_auth.h"

#include <iostream>
#include <string>

int main() {
    aml::config::Config cfg;
    std::string error;
    if (aml::admin_auth::configured(cfg)) {
        std::cerr << "empty config unexpectedly has an Admin password\n";
        return 1;
    }
    if (aml::admin_auth::set_password(cfg, "short", &error) || error.empty()) {
        std::cerr << "weak Admin password was accepted\n";
        return 1;
    }
    if (!aml::admin_auth::set_password(cfg, "correct horse battery", &error) ||
        !aml::admin_auth::configured(cfg) ||
        !aml::admin_auth::verify_password(cfg, "correct horse battery") ||
        aml::admin_auth::verify_password(cfg, "wrong password")) {
        std::cerr << "Admin password verification failed\n";
        return 1;
    }
    const std::string salt = cfg.admin_salt;
    const std::string verifier = cfg.admin_verifier;
    if (salt.empty() || verifier.empty() || salt == verifier) {
        std::cerr << "Admin verifier was not generated correctly\n";
        return 1;
    }
    aml::admin_auth::clear_password(cfg);
    if (aml::admin_auth::configured(cfg) || aml::admin_auth::verify_password(cfg, "correct horse battery")) {
        std::cerr << "cleared Admin password remained usable\n";
        return 1;
    }
    return 0;
}
