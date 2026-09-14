#pragma once

#include "config.h"

#include <string>

namespace aml::admin_auth {

bool configured(const config::Config& cfg);
bool set_password(config::Config& cfg, const std::string& password, std::string* error = nullptr);
bool verify_password(const config::Config& cfg, const std::string& password);
void clear_password(config::Config& cfg);

}  // namespace aml::admin_auth
