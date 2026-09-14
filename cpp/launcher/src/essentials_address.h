#pragma once

#include <string>
#include <vector>

namespace aml::essentials {

struct AddressResolveResult {
    bool success = false;
    std::string error;
    std::string alias;
    std::string address;
    std::string session_id;
    std::string host_user_id;
    std::string status;
    std::string kind;
    std::string server_id;
};

class EssentialsAddressResolver {
public:
    AddressResolveResult Resolve(const std::string& address) const;
    bool IsAvailable(const std::string& alias) const;

    static std::string NormalizeAlias(const std::string& alias);
    static std::string ToAddress(const std::string& alias);
    static bool IsValidAlias(const std::string& alias);
    static bool IsReserved(const std::string& alias);
    static std::string Generate(const std::string& preferred_name);
    static std::vector<std::string> Suggestions(const std::string& alias);
};

class ServerAddressResolver {
public:
    AddressResolveResult Resolve(const std::string& address) const;
    bool IsAvailable(const std::string& alias) const;

    static std::string NormalizeAlias(const std::string& alias);
    static std::string ToAddress(const std::string& alias);
    static std::string Generate(const std::string& preferred_name);
};

}  // namespace aml::essentials
