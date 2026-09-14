#include "essentials_address.h"

#include "json.h"
#include "supabase.h"

#include <algorithm>
#include <cctype>
#include <random>

namespace aml::essentials {

namespace {

const char* kReserved[] = {
    "admin", "support", "official", "api", "login", "server", "amalgam"
};

}  // namespace

std::string EssentialsAddressResolver::NormalizeAlias(const std::string& value) {
    std::string alias = value;
    const std::string suffix = ".amalgam-essentials";
    if (alias.size() > suffix.size() &&
        alias.compare(alias.size() - suffix.size(), suffix.size(), suffix) == 0) {
        alias.resize(alias.size() - suffix.size());
    }
    std::transform(alias.begin(), alias.end(), alias.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return alias;
}

std::string EssentialsAddressResolver::ToAddress(const std::string& alias) {
    return NormalizeAlias(alias) + ".amalgam-essentials";
}

bool EssentialsAddressResolver::IsReserved(const std::string& alias) {
    const std::string normalized = NormalizeAlias(alias);
    for (const char* reserved : kReserved) {
        if (normalized == reserved) return true;
    }
    return false;
}

bool EssentialsAddressResolver::IsValidAlias(const std::string& alias) {
    const std::string normalized = NormalizeAlias(alias);
    if (normalized.size() < 3 || normalized.size() > 32 || IsReserved(normalized)) return false;
    if (!std::isalnum(static_cast<unsigned char>(normalized.front())) ||
        !std::isalnum(static_cast<unsigned char>(normalized.back()))) return false;
    for (char c : normalized) {
        if (!(std::islower(static_cast<unsigned char>(c)) ||
              std::isdigit(static_cast<unsigned char>(c)) || c == '-')) return false;
    }
    return true;
}

std::string EssentialsAddressResolver::Generate(const std::string& preferred_name) {
    std::string base = NormalizeAlias(preferred_name);
    std::string filtered;
    for (char c : base) {
        if (std::islower(static_cast<unsigned char>(c)) ||
            std::isdigit(static_cast<unsigned char>(c)) || c == '-') {
            filtered.push_back(c);
        }
    }
    if (filtered.size() < 3) filtered = "world";
    if (filtered.size() > 25) filtered.resize(25);
    if (filtered.back() == '-') filtered.pop_back();
    static thread_local std::mt19937 generator{std::random_device{}()};
    static constexpr char hex[] = "0123456789abcdef";
    std::uniform_int_distribution<int> digit(0, 15);
    filtered += "-";
    for (int i = 0; i < 4; ++i) filtered.push_back(hex[digit(generator)]);
    return filtered;
}

std::vector<std::string> EssentialsAddressResolver::Suggestions(const std::string& value) {
    const std::string base = NormalizeAlias(value);
    std::vector<std::string> suggestions;
    if (!base.empty()) suggestions.push_back(base + "2");
    if (!base.empty()) suggestions.push_back(base + "-world");
    if (suggestions.empty()) suggestions.push_back("my-world");
    return suggestions;
}

bool EssentialsAddressResolver::IsAvailable(const std::string& alias) const {
    const std::string normalized = NormalizeAlias(alias);
    if (!IsValidAlias(normalized)) return false;
    auto& supabase = supabase::SupabaseManager::instance();
    if (!supabase.is_authenticated() || !supabase.client()) return false;
    Json args = Json::obj();
    args.set("session_alias", Json::str(normalized));
    auto result = supabase.client()->rpc("is_essentials_alias_available", args);
    return result.success && !result.data.empty() && result.data.front().as_bool();
}

AddressResolveResult EssentialsAddressResolver::Resolve(const std::string& value) const {
    AddressResolveResult result;
    const std::string address = ToAddress(value);
    if (!IsValidAlias(NormalizeAlias(value))) {
        result.error = "Invalid or reserved Amalgam address";
        return result;
    }
    auto& supabase = supabase::SupabaseManager::instance();
    if (!supabase.is_authenticated() || !supabase.client()) {
        result.error = "Sign in before joining an Amalgam world";
        return result;
    }
    Json args = Json::obj();
    args.set("session_address", Json::str(address));
    auto response = supabase.client()->rpc("resolve_essentials_address", args);
    if (!response.success || response.data.empty()) {
        result.error = "This Amalgam world is currently offline or does not exist";
        return result;
    }
    const Json& row = response.data.front();
    if (row.get("kind").as_str("essentials") != "essentials") {
        result.error = "That address belongs to a dedicated server";
        return result;
    }
    result.success = true;
    result.alias = row.get("alias").as_str();
    result.address = row.get("address").as_str(address);
    result.session_id = row.get("session_id").as_str();
    result.host_user_id = row.get("host_user_id").as_str();
    result.status = row.get("status").as_str();
    result.kind = row.get("kind").as_str("essentials");
    result.server_id = row.get("server_id").as_str();
    return result;
}

std::string ServerAddressResolver::NormalizeAlias(const std::string& value) {
    std::string alias = value;
    const std::string suffix = ".amalgam";
    if (alias.size() > suffix.size() &&
        alias.compare(alias.size() - suffix.size(), suffix.size(), suffix) == 0) {
        alias.resize(alias.size() - suffix.size());
    }
    std::transform(alias.begin(), alias.end(), alias.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return alias;
}

std::string ServerAddressResolver::ToAddress(const std::string& alias) {
    return NormalizeAlias(alias) + ".amalgam";
}

std::string ServerAddressResolver::Generate(const std::string& preferred_name) {
    return EssentialsAddressResolver::Generate(preferred_name);
}

bool ServerAddressResolver::IsAvailable(const std::string& alias) const {
    const std::string normalized = NormalizeAlias(alias);
    if (!EssentialsAddressResolver::IsValidAlias(normalized)) return false;
    auto& supabase = supabase::SupabaseManager::instance();
    if (!supabase.is_authenticated() || !supabase.client()) return false;
    Json args = Json::obj();
    args.set("server_alias", Json::str(normalized));
    auto result = supabase.client()->rpc("is_server_alias_available", args);
    return result.success && !result.data.empty() && result.data.front().as_bool();
}

AddressResolveResult ServerAddressResolver::Resolve(const std::string& value) const {
    AddressResolveResult result;
    const std::string address = ToAddress(value);
    if (!EssentialsAddressResolver::IsValidAlias(NormalizeAlias(value))) {
        result.error = "Invalid or reserved server address";
        return result;
    }
    auto& supabase = supabase::SupabaseManager::instance();
    if (!supabase.is_authenticated() || !supabase.client()) {
        result.error = "Sign in before joining an Amalgam server";
        return result;
    }
    Json args = Json::obj();
    args.set("session_address", Json::str(address));
    auto response = supabase.client()->rpc("resolve_essentials_address", args);
    if (!response.success || response.data.empty()) {
        result.error = "This Amalgam server is currently offline or does not exist";
        return result;
    }
    const Json& row = response.data.front();
    if (row.get("kind").as_str() != "server") {
        result.error = "That address belongs to an Essentials world";
        return result;
    }
    result.success = true;
    result.kind = "server";
    result.server_id = row.get("server_id").as_str();
    result.alias = row.get("alias").as_str();
    result.address = row.get("address").as_str(address);
    result.host_user_id = row.get("host_user_id").as_str();
    result.status = row.get("status").as_str();
    return result;
}

}  // namespace aml::essentials
