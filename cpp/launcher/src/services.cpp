#include "services.h"

#include "json.h"

#include "net.h"
#include "server_manager.h"
#include "essentials_address.h"
#include "supabase.h"
#include "java.h"

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <objbase.h>
#include <wincrypt.h>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <random>
#include <chrono>
#include <fstream>
#include <filesystem>
#include <iterator>
#include <cerrno>
#include <deque>
#include <system_error>
#include <thread>

#pragma comment(lib, "ws2_32.lib")

namespace aml::services {

namespace {

bool valid_server_identifier(const std::string& value) {
    if (value.empty() || value == "." || value == "..") return false;
    for (const unsigned char c : value) {
        if (c < 32 || c == '/' || c == '\\' || c == ':') return false;
    }
    return true;
}

// True when a process on this machine already listens on the port. Binding is
// the server's first act, so a conflict otherwise kills it right after a start
// that already reported success.
bool port_answers_locally(int port) {
    ULONG size = 0;
    if (GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) !=
        ERROR_INSUFFICIENT_BUFFER) {
        return false;
    }
    std::vector<uint8_t> storage(size);
    auto* table = reinterpret_cast<PMIB_TCPTABLE_OWNER_PID>(storage.data());
    if (GetExtendedTcpTable(table, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) !=
        NO_ERROR) {
        return false;
    }
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        if (ntohs(static_cast<u_short>(table->table[i].dwLocalPort)) == port) return true;
    }
    return false;
}

bool resolve_server_relative_path(const std::filesystem::path& root,
                                  const std::string& requested,
                                  std::filesystem::path* resolved,
                                  std::string* error) {
    if (requested.empty()) {
        if (error) *error = "Server file path cannot be empty";
        return false;
    }
    const std::filesystem::path relative(net::to_wide(requested));
    if (relative.is_absolute() || relative.has_root_name() || relative.has_root_directory()) {
        if (error) *error = "Server file path must stay inside the server directory";
        return false;
    }
    const auto normalized_root = std::filesystem::weakly_canonical(root);
    const auto normalized = std::filesystem::weakly_canonical(root / relative);
    if (normalized == normalized_root) {
        if (error) *error = "Server file path must name a file inside the server directory";
        return false;
    }
    auto root_it = normalized_root.begin();
    auto path_it = normalized.begin();
    for (; root_it != normalized_root.end() && path_it != normalized.end(); ++root_it, ++path_it) {
        if (*root_it != *path_it) {
            if (error) *error = "Server file path must stay inside the server directory";
            return false;
        }
    }
    if (root_it != normalized_root.end()) {
        if (error) *error = "Server file path must stay inside the server directory";
        return false;
    }
    if (resolved) *resolved = normalized;
    return true;
}

// ---------------------------------------------------------------------------
// Helper functions
// ---------------------------------------------------------------------------

std::string generate_uuid() {
    GUID guid;
    if (CoCreateGuid(&guid) != S_OK) {
        static std::mt19937_64 gen(std::chrono::steady_clock::now().time_since_epoch().count());
        std::uniform_int_distribution<uint64_t> dis;
        uint64_t v[2] = {dis(gen), dis(gen)};
        char buf[37];
        snprintf(buf, sizeof(buf), "%08llX-%04llX-%04llX-%04llX-%012llX",
                 v[0] >> 32, (v[0] >> 16) & 0xFFFF, v[0] & 0xFFFF,
                 (v[1] >> 48) & 0xFFFF, v[1] & 0xFFFFFFFFFFFFULL);
        return buf;
    }
    char buf[39];
    snprintf(buf, sizeof(buf), "%08lX-%04hX-%04hX-%02hhX%02hhX-%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX",
             guid.Data1, guid.Data2, guid.Data3,
             guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
             guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
    return buf;
}

std::string generate_random_string(size_t length) {
    static const char kAlphanum[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    static std::mt19937 gen(std::random_device{}());
    static std::uniform_int_distribution<> dis(0, sizeof(kAlphanum) - 2);
    std::string result;
    result.reserve(length);
    for (size_t i = 0; i < length; ++i) {
        result += kAlphanum[dis(gen)];
    }
    return result;
}

std::string base64_encode(const std::string& data) {
    DWORD len = static_cast<DWORD>(data.size());
    DWORD out_len = 0;
    if (!CryptBinaryToStringA(reinterpret_cast<const BYTE*>(data.data()), len,
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &out_len)) {
        return "";
    }
    std::string result(out_len, '\0');
    if (!CryptBinaryToStringA(reinterpret_cast<const BYTE*>(data.data()), len,
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                              &result[0], &out_len)) {
        return "";
    }
    return result;
}

std::string base64_decode(const std::string& data) {
    DWORD len = static_cast<DWORD>(data.size());
    DWORD out_len = 0;
    if (!CryptStringToBinaryA(data.c_str(), len, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                              nullptr, &out_len, nullptr, nullptr)) {
        return "";
    }
    std::string result(out_len, '\0');
    if (!CryptStringToBinaryA(data.c_str(), len, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                              reinterpret_cast<BYTE*>(&result[0]), &out_len,
                              nullptr, nullptr)) {
        return "";
    }
    return result;
}

std::string url_encode(const std::string& data) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;
    for (char c : data) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        } else {
            escaped << '%' << std::setw(2) << int(static_cast<unsigned char>(c));
        }
    }
    return escaped.str();
}

std::string url_decode(const std::string& data) {
    std::ostringstream decoded;
    for (size_t i = 0; i < data.size(); ++i) {
        if (data[i] == '%') {
            if (i + 2 < data.size()) {
                std::string hex = data.substr(i + 1, 2);
                char decoded_char = static_cast<char>(std::stoi(hex, nullptr, 16));
                decoded << decoded_char;
                i += 2;
            }
        } else if (data[i] == '+') {
            decoded << ' ';
        } else {
            decoded << data[i];
        }
    }
    return decoded.str();
}

int64_t current_timestamp() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

int64_t current_millis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string stream_failure(const std::filesystem::path& path, const char* operation) {
    std::string message = std::string(operation) + " '" + net::to_utf8(path.wstring()) + "'";
    if (errno != 0) {
        message += ": " + std::error_code(errno, std::generic_category()).message();
    }
    return message;
}

// Simple JSON helper for service responses
Json::Value parse_json_response(const std::string& response, std::string* error) {
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errs;
    std::istringstream iss(response);
    if (!Json::parseFromStream(builder, iss, &root, &errs)) {
        if (error) *error = "JSON parse error: " + errs;
        return Json::Value();
    }
    return root;
}

// HTTP request helper
std::string make_http_request(const std::string& method, const std::string& url,
                              const std::string& body = "",
                              const std::map<std::string, std::string>& headers = {},
                              std::string* error = nullptr, int /*timeout_seconds*/ = 30) {
    std::wstring wurl = net::to_wide(url);
    std::vector<std::wstring> wheaders;
    for (const auto& h : headers) {
        wheaders.push_back(net::to_wide(h.first + ": " + h.second));
    }
    std::vector<uint8_t> response_bytes;
    std::vector<uint8_t> body_bytes(body.begin(), body.end());
    std::string err;
    if (!net::request(wurl, net::to_wide(method), wheaders, body_bytes,
                      response_bytes, &err)) {
        if (error) *error = err;
        return "";
    }
    return std::string(response_bytes.begin(), response_bytes.end());
}

struct RemoteStorageLocation {
    std::string bucket;
    std::string path;
};

std::string storage_backend_error() {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    if (!supabase.is_initialized()) {
        return "Remote storage is unavailable: no storage backend is configured";
    }

    auto* client = supabase.client();
    if (!client) {
        return "Remote storage is unavailable: no storage client is configured";
    }
    if (!client->is_authenticated()) {
        return "Remote storage is unavailable: storage backend is not authenticated";
    }
    return "";
}

aml::supabase::SupabaseClient* configured_storage_client() {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    if (!storage_backend_error().empty()) return nullptr;
    return supabase.client();
}

std::string make_remote_storage_id(const std::string& bucket, const std::string& path) {
    return bucket + ":" + path;
}

bool parse_remote_storage_id(const std::string& id, RemoteStorageLocation* location) {
    const size_t separator = id.find(':');
    if (separator == std::string::npos || separator == 0 || separator + 1 >= id.size()) {
        return false;
    }

    if (location) {
        location->bucket = id.substr(0, separator);
        location->path = id.substr(separator + 1);
    }
    return true;
}

std::string storage_object_path(const aml::supabase::SupabaseClient::StorageItem& item) {
    return item.path.empty() ? item.name : item.path;
}

StorageType storage_type_for_bucket(const std::string& bucket) {
    if (bucket == "profiles") return StorageType::Profile;
    if (bucket == "worlds") return StorageType::World;
    if (bucket == "backups") return StorageType::Backup;
    if (bucket == "mods") return StorageType::Mod;
    if (bucket == "configs") return StorageType::Config;
    if (bucket == "screenshots") return StorageType::Screenshot;
    if (bucket == "logs") return StorageType::Log;
    return StorageType::Cache;
}

std::string object_name_from_path(const std::string& path) {
    const size_t separator = path.find_last_of("/\\");
    return separator == std::string::npos ? path : path.substr(separator + 1);
}

std::string reported_object_path(const std::string& bucket, const std::string& path,
                                 const std::string& full_path,
                                 const std::string& fallback) {
    const std::string bucket_prefix = bucket + "/";
    if (!path.empty()) {
        return path;
    }
    if (!full_path.empty()) {
        return full_path.rfind(bucket_prefix, 0) == 0
            ? full_path.substr(bucket_prefix.size()) : full_path;
    }
    return fallback;
}

}  // namespace

// ---------------------------------------------------------------------------
// NodeInfo <-> servers::Node conversion helpers
// ---------------------------------------------------------------------------

namespace {

services::NodeInfo node_to_info(const aml::servers::Node& n) {
    services::NodeInfo info;
    info.id = n.id;
    info.name = n.name;
    info.host = n.host;
    info.port = n.port;
    info.region = n.metadata.count("region") ? n.metadata.at("region") : "default";
    info.zone = n.metadata.count("zone") ? n.metadata.at("zone") : "default";
    info.online = (n.status == aml::servers::NodeStatus::Online);
    info.last_heartbeat = n.last_heartbeat;
    info.storage_capacity = static_cast<uint64_t>(n.storage_gb) * 1024ULL * 1024ULL * 1024ULL;
    info.storage_used = static_cast<uint64_t>(n.storage_gb * n.storage_usage * 1024.0 * 1024.0 * 1024.0);
    info.storage_available = info.storage_capacity > info.storage_used
                                ? info.storage_capacity - info.storage_used : 0;
    info.cpu_cores = n.cpu_cores;
    info.memory_total = static_cast<uint64_t>(n.memory_mb) * 1024ULL * 1024ULL;
    info.memory_used = static_cast<uint64_t>(n.memory_mb * n.memory_usage * 1024.0 * 1024.0);
    if (n.metadata.count("tags")) {
        std::string tags_str = n.metadata.at("tags");
        size_t pos = 0;
        while ((pos = tags_str.find(',')) != std::string::npos) {
            info.tags.push_back(tags_str.substr(0, pos));
            tags_str.erase(0, pos + 1);
        }
        if (!tags_str.empty()) info.tags.push_back(tags_str);
    }
    info.metadata = n.metadata;
    return info;
}

aml::servers::Node info_to_node(const services::NodeInfo& info) {
    aml::servers::Node n;
    n.id = info.id;
    n.name = info.name;
    n.host = info.host;
    n.port = info.port;
    n.cpu_cores = info.cpu_cores;
    n.memory_mb = static_cast<int>(info.memory_total / (1024ULL * 1024ULL));
    n.storage_gb = static_cast<int>(info.storage_capacity / (1024ULL * 1024ULL * 1024ULL));
    n.status = info.online ? aml::servers::NodeStatus::Online : aml::servers::NodeStatus::Offline;
    n.last_heartbeat = info.last_heartbeat;
    n.registered_at = info.last_heartbeat;
    if (!info.tags.empty()) {
        std::string tags;
        for (size_t i = 0; i < info.tags.size(); ++i) {
            if (i > 0) tags += ",";
            tags += info.tags[i];
        }
        n.metadata["tags"] = tags;
    }
    n.metadata["region"] = info.region;
    n.metadata["zone"] = info.zone;
    for (const auto& kv : info.metadata) {
        n.metadata[kv.first] = kv.second;
    }
    return n;
}

services::ClusterInfo cluster_to_info(const aml::servers::Cluster& c, const aml::servers::ServerManager& mgr) {
    services::ClusterInfo info;
    info.id = c.id;
    info.name = c.name;
    info.created_at = c.created_at;
    switch (c.status) {
        case aml::servers::ClusterStatus::Active: info.status = "active"; break;
        case aml::servers::ClusterStatus::Degraded: info.status = "degraded"; break;
        case aml::servers::ClusterStatus::Offline: info.status = "offline"; break;
        default: info.status = "unknown"; break;
    }
    info.metadata = c.metadata;
    for (const auto& nid : c.node_ids) {
        aml::servers::Node n = mgr.get_node(nid);
        if (!n.id.empty()) {
            info.nodes.push_back(node_to_info(n));
        }
    }
    return info;
}

}  // namespace

// ---------------------------------------------------------------------------
// AuthService Implementation
// ---------------------------------------------------------------------------

AuthService::AuthService(const ServiceConfig& config) : config_(config) {
    // Load stored tokens if available
    // In production, this would load from secure storage
}

AuthService::~AuthService() {
    clear_tokens();
}

std::string AuthService::make_request(const std::string& method, const std::string& endpoint,
                                       const std::string& body, std::string* error) {
    std::string url = config_.endpoint + endpoint;
    
    std::map<std::string, std::string> headers;
    if (!current_access_token_.empty()) {
        headers["Authorization"] = "Bearer " + current_access_token_;
    }
    headers["Content-Type"] = "application/json";
    headers["Accept"] = "application/json";
    
    return make_http_request(method, url, body, headers, error, config_.timeout_seconds);
}

std::string AuthService::build_auth_header() const {
    if (current_access_token_.empty()) return "";
    return "Bearer " + current_access_token_;
}

bool AuthService::store_tokens(const std::string& access_token, const std::string& refresh_token, int64_t /*expires_in*/) {
    current_access_token_ = access_token;
    current_refresh_token_ = refresh_token;
    // Store in secure storage (DPAPI)
    return true;
}

void AuthService::clear_tokens() {
    current_access_token_.clear();
    current_refresh_token_.clear();
    current_user_ = UserProfile();
    authenticated_ = false;
}

AuthResponse AuthService::login(const LoginRequest& request) {
    AuthResponse response;
    
    Json::Value root;
    root["username"] = request.username_or_email;
    root["password"] = request.password;
    root["remember_me"] = request.remember_me;
    
    std::string body = root.toStyledString();
    std::string result = make_request("POST", "/api/auth/login", body, &response.error);
    
    if (result.empty()) {
        response.success = false;
        return response;
    }
    
    Json::Value parsed = parse_json_response(result, &response.error);
    if (parsed.isNull()) {
        response.success = false;
        return response;
    }
    
    response.success = true;
    response.user.id = parsed["user"]["id"].asString();
    response.user.username = parsed["user"]["username"].asString();
    response.user.email = parsed["user"]["email"].asString();
    response.user.display_name = parsed["user"]["display_name"].asString();
    response.user.avatar_url = parsed["user"]["avatar_url"].asString();
    
    // Parse roles and permissions
    for (const auto& role : parsed["user"]["roles"]) {
        response.user.roles.push_back(role.asString());
    }
    for (const auto& perm : parsed["user"]["permissions"]) {
        response.user.permissions.push_back(perm.asString());
    }
    
    response.access_token = parsed["access_token"].asString();
    response.refresh_token = parsed["refresh_token"].asString();
    response.expires_in = parsed["expires_in"].asInt64();
    
    // Store tokens
    store_tokens(response.access_token, response.refresh_token, response.expires_in);
    current_user_ = response.user;
    authenticated_ = true;
    
    // Notify callbacks
    for (auto& cb : auth_success_callbacks_) {
        cb(current_user_);
    }
    
    return response;
}

AuthResponse AuthService::register_user(const RegistrationRequest& request) {
    AuthResponse response;
    
    Json::Value root;
    root["username"] = request.username;
    root["email"] = request.email;
    root["password"] = request.password;
    root["display_name"] = request.display_name;
    root["invite_code"] = request.invite_code;
    
    std::string body = root.toStyledString();
    std::string result = make_request("POST", "/api/auth/register", body, &response.error);
    
    if (result.empty()) {
        response.success = false;
        return response;
    }
    
    Json::Value parsed = parse_json_response(result, &response.error);
    if (parsed.isNull()) {
        response.success = false;
        return response;
    }
    
    response.success = true;
    response.user.id = parsed["user"]["id"].asString();
    response.user.username = parsed["user"]["username"].asString();
    response.user.email = parsed["user"]["email"].asString();
    response.user.display_name = parsed["user"]["display_name"].asString();
    
    response.access_token = parsed["access_token"].asString();
    response.refresh_token = parsed["refresh_token"].asString();
    response.expires_in = parsed["expires_in"].asInt64();
    
    store_tokens(response.access_token, response.refresh_token, response.expires_in);
    current_user_ = response.user;
    authenticated_ = true;
    
    for (auto& cb : auth_success_callbacks_) {
        cb(current_user_);
    }
    
    return response;
}

TokenRefreshResponse AuthService::refresh_token(const std::string& refresh_token) {
    TokenRefreshResponse response;
    
    Json::Value root;
    root["refresh_token"] = refresh_token;
    
    std::string body = root.toStyledString();
    std::string result = make_request("POST", "/api/auth/refresh", body, &response.error);
    
    if (result.empty()) {
        response.success = false;
        return response;
    }
    
    Json::Value parsed = parse_json_response(result, &response.error);
    if (parsed.isNull()) {
        response.success = false;
        return response;
    }
    
    response.success = true;
    response.access_token = parsed["access_token"].asString();
    response.refresh_token = parsed["refresh_token"].asString();
    response.expires_in = parsed["expires_in"].asInt64();
    
    store_tokens(response.access_token, response.refresh_token, response.expires_in);
    
    return response;
}

bool AuthService::logout(const std::string& /*access_token*/) {
    std::string error;
    std::string result = make_request("POST", "/api/auth/logout", "", &error);
    
    clear_tokens();
    
    for (auto& cb : logout_callbacks_) {
        cb();
    }
    
    return true;
}

UserProfile AuthService::get_user_profile(const std::string& /*access_token*/) {
    std::string error;
    std::string result = make_request("GET", "/api/auth/me", "", &error);
    
    if (result.empty()) {
        return UserProfile();
    }
    
    Json::Value parsed = parse_json_response(result, &error);
    if (parsed.isNull()) {
        return UserProfile();
    }
    
    UserProfile profile;
    profile.id = parsed["id"].asString();
    profile.username = parsed["username"].asString();
    profile.email = parsed["email"].asString();
    profile.display_name = parsed["display_name"].asString();
    profile.avatar_url = parsed["avatar_url"].asString();
    
    for (const auto& role : parsed["roles"]) {
        profile.roles.push_back(role.asString());
    }
    for (const auto& perm : parsed["permissions"]) {
        profile.permissions.push_back(perm.asString());
    }
    
    return profile;
}

bool AuthService::update_user_profile(const std::string& /*access_token*/, const UserProfile& updates) {
    Json::Value root;
    if (!updates.display_name.empty()) root["display_name"] = updates.display_name;
    if (!updates.avatar_url.empty()) root["avatar_url"] = updates.avatar_url;
    
    std::string body = root.toStyledString();
    std::string error;
    std::string result = make_request("PUT", "/api/auth/me", body, &error);
    
    if (result.empty()) {
        return false;
    }
    
    return true;
}

bool AuthService::change_password(const std::string& /*access_token*/, 
                                 const std::string& current_password, 
                                 const std::string& new_password) {
    Json::Value root;
    root["current_password"] = current_password;
    root["new_password"] = new_password;
    
    std::string body = root.toStyledString();
    std::string error;
    std::string result = make_request("POST", "/api/auth/change-password", body, &error);
    
    return !result.empty();
}

bool AuthService::validate_token(const std::string& access_token) {
    std::string error;
    std::string result = make_request("GET", "/api/auth/validate", "", &error);
    return !result.empty();
}

bool AuthService::invalidate_token(const std::string& access_token) {
    Json::Value root;
    root["token"] = access_token;
    
    std::string body = root.toStyledString();
    std::string error;
    std::string result = make_request("POST", "/api/auth/invalidate", body, &error);
    
    return !result.empty();
}

bool AuthService::invalidate_all_tokens(const std::string& user_id) {
    Json::Value root;
    root["user_id"] = user_id;
    
    std::string body = root.toStyledString();
    std::string error;
    std::string result = make_request("POST", "/api/auth/invalidate-all", body, &error);
    
    return !result.empty();
}

bool AuthService::setup_2fa(const std::string& access_token, std::string* secret_out, std::string* qr_code_out) {
    std::string error;
    std::string result = make_request("POST", "/api/auth/2fa/setup", "", &error);
    
    if (result.empty()) {
        return false;
    }
    
    Json::Value parsed = parse_json_response(result, &error);
    if (parsed.isNull()) {
        return false;
    }
    
    if (secret_out) *secret_out = parsed["secret"].asString();
    if (qr_code_out) *qr_code_out = parsed["qr_code"].asString();
    
    return true;
}

bool AuthService::verify_2fa(const std::string& access_token, const std::string& code) {
    Json::Value root;
    root["code"] = code;
    
    std::string body = root.toStyledString();
    std::string error;
    std::string result = make_request("POST", "/api/auth/2fa/verify", body, &error);
    
    return !result.empty();
}

bool AuthService::disable_2fa(const std::string& access_token, const std::string& code) {
    Json::Value root;
    root["code"] = code;
    
    std::string body = root.toStyledString();
    std::string error;
    std::string result = make_request("POST", "/api/auth/2fa/disable", body, &error);
    
    return !result.empty();
}

bool AuthService::request_password_reset(const std::string& email_or_username) {
    Json::Value root;
    root["email_or_username"] = email_or_username;
    
    std::string body = root.toStyledString();
    std::string error;
    std::string result = make_request("POST", "/api/auth/password-reset/request", body, &error);
    
    return !result.empty();
}

bool AuthService::verify_password_reset_token(const std::string& token) {
    Json::Value root;
    root["token"] = token;
    
    std::string body = root.toStyledString();
    std::string error;
    std::string result = make_request("POST", "/api/auth/password-reset/verify", body, &error);
    
    return !result.empty();
}

bool AuthService::complete_password_reset(const std::string& token, const std::string& new_password) {
    Json::Value root;
    root["token"] = token;
    root["new_password"] = new_password;
    
    std::string body = root.toStyledString();
    std::string error;
    std::string result = make_request("POST", "/api/auth/password-reset/complete", body, &error);
    
    return !result.empty();
}

std::string AuthService::get_oauth_url(const std::string& provider, const std::string& redirect_uri) {
    std::string state = generate_random_string(32);
    // In production, store state for CSRF protection
    
    if (provider == "microsoft") {
        return std::string("https://login.microsoftonline.com/consumers/oauth2/v2.0/authorize?") +
               "client_id=" + config_.api_key +
               "&response_type=code" +
               "&redirect_uri=" + url_encode(redirect_uri) +
               "&scope=openid%20profile%20email" +
               "&state=" + state +
               "&response_mode=query";
    } else if (provider == "google") {
        return std::string("https://accounts.google.com/o/oauth2/v2/auth?") +
               "client_id=" + config_.api_key +
               "&response_type=code" +
               "&redirect_uri=" + url_encode(redirect_uri) +
               "&scope=openid%20profile%20email" +
               "&state=" + state +
               "&access_type=offline";
    } else if (provider == "github") {
        return std::string("https://github.com/login/oauth/authorize?") +
               "client_id=" + config_.api_key +
               "&redirect_uri=" + url_encode(redirect_uri) +
               "&scope=user%20email" +
               "&state=" + state;
    }
    
    return "";
}

AuthResponse AuthService::exchange_oauth_code(const std::string& provider, const std::string& code, 
                                              const std::string& redirect_uri) {
    AuthResponse response;
    
    Json::Value root;
    root["provider"] = provider;
    root["code"] = code;
    root["redirect_uri"] = redirect_uri;
    
    std::string body = root.toStyledString();
    std::string result = make_request("POST", "/api/auth/oauth/exchange", body, &response.error);
    
    if (result.empty()) {
        response.success = false;
        return response;
    }
    
    Json::Value parsed = parse_json_response(result, &response.error);
    if (parsed.isNull()) {
        response.success = false;
        return response;
    }
    
    response.success = true;
    response.user.id = parsed["user"]["id"].asString();
    response.user.username = parsed["user"]["username"].asString();
    response.user.email = parsed["user"]["email"].asString();
    response.user.display_name = parsed["user"]["display_name"].asString();
    
    response.access_token = parsed["access_token"].asString();
    response.refresh_token = parsed["refresh_token"].asString();
    response.expires_in = parsed["expires_in"].asInt64();
    
    store_tokens(response.access_token, response.refresh_token, response.expires_in);
    current_user_ = response.user;
    authenticated_ = true;
    
    for (auto& cb : auth_success_callbacks_) {
        cb(current_user_);
    }
    
    return response;
}

bool AuthService::is_authenticated() const {
    return authenticated_;
}

const UserProfile& AuthService::current_user() const {
    return current_user_;
}

const std::string& AuthService::current_access_token() const {
    return current_access_token_;
}

void AuthService::on_auth_success(const std::function<void(const UserProfile&)>& callback) {
    auth_success_callbacks_.push_back(callback);
}

void AuthService::on_auth_failure(const std::function<void(const std::string&)>& callback) {
    auth_failure_callbacks_.push_back(callback);
}

void AuthService::on_logout(const std::function<void()>& callback) {
    logout_callbacks_.push_back(callback);
}

// ---------------------------------------------------------------------------
// StorageService Implementation
// ---------------------------------------------------------------------------

StorageService::StorageService(const ServiceConfig& config) : config_(config) {
    // Initialize local cache directory
    local_cache_dir_ = net::to_utf8(net::get_local_app_data_path()) + "\\Amalgam\\Storage\\Cache";
    net::mkdirs(net::to_wide(local_cache_dir_));
}

StorageService::~StorageService() {}

std::string StorageService::make_request(const std::string& method, const std::string& endpoint,
                                         const std::string& body, std::string* error) {
    std::string url = config_.endpoint + endpoint;
    
    std::map<std::string, std::string> headers;
    if (!config_.api_key.empty()) {
        headers["X-API-Key"] = config_.api_key;
    }
    headers["Content-Type"] = "application/json";
    
    return make_http_request(method, url, body, headers, error, config_.timeout_seconds);
}

std::string StorageService::build_auth_header() const {
    if (config_.api_key.empty()) return "";
    return "Bearer " + config_.api_key;
}

std::string StorageService::get_storage_type_path(StorageType type) const {
    switch (type) {
        case StorageType::Profile: return "profiles";
        case StorageType::World: return "worlds";
        case StorageType::Backup: return "backups";
        case StorageType::Mod: return "mods";
        case StorageType::Config: return "configs";
        case StorageType::Screenshot: return "screenshots";
        case StorageType::Log: return "logs";
        case StorageType::Cache: return "cache";
        default: return "other";
    }
}

UploadResult StorageService::upload(const UploadRequest& request) {
    UploadResult result;

    auto* client = configured_storage_client();
    if (!client) {
        result.error = storage_backend_error();
        return result;
    }
    if (request.name.empty()) {
        result.error = "Remote storage upload requires an object path";
        return result;
    }

    std::ifstream source(request.path, std::ios::binary | std::ios::ate);
    if (!source) {
        result.error = "Source file not found: " + request.path;
        return result;
    }

    const std::streamoff end = source.tellg();
    if (end < 0) {
        result.error = "Failed to determine source file size: " + request.path;
        return result;
    }

    std::vector<uint8_t> data(static_cast<size_t>(end));
    source.seekg(0, std::ios::beg);
    if (end > 0 && !source.read(reinterpret_cast<char*>(data.data()), end)) {
        result.error = "Failed to read source file: " + request.path;
        return result;
    }

    const std::string bucket = get_storage_type_path(request.type);
    aml::supabase::SupabaseClient::StorageUploadOptions options;
    options.bucket = bucket;
    options.path = request.name;
    options.data = std::move(data);
    options.upsert = true;
    options.metadata = request.metadata;
    const auto content_type = request.metadata.find("content_type");
    options.content_type = content_type == request.metadata.end()
        ? "application/octet-stream" : content_type->second;

    if (request.progress_callback) request.progress_callback(0.0f);
    const auto remote = client->upload_file(options);
    if (!remote.success) {
        result.error = remote.error.empty() ? "Remote storage upload failed" : remote.error;
        return result;
    }

    const std::string actual_path = reported_object_path(
        bucket, remote.path, remote.full_path, request.name);
    result.success = true;
    result.item.id = make_remote_storage_id(bucket, actual_path);
    result.item.name = object_name_from_path(actual_path);
    result.item.type = request.type;
    result.item.path = actual_path;
    result.item.size_bytes = options.data.size();
    result.item.created_at = current_timestamp();
    result.item.updated_at = current_timestamp();
    result.item.metadata = request.metadata;
    if (request.progress_callback) request.progress_callback(1.0f);
    return result;
}

UploadResult StorageService::upload_async(const UploadRequest& request, 
                                          const std::function<void(const UploadResult&)>& callback) {
    UploadResult result = upload(request);
    if (callback) callback(result);
    return result;
}

DownloadResult StorageService::download(const DownloadRequest& request) {
    DownloadResult result;

    auto* client = configured_storage_client();
    if (!client) {
        result.error = storage_backend_error();
        return result;
    }
    if (request.target_path.empty()) {
        result.error = "Remote storage download requires a target path";
        return result;
    }

    RemoteStorageLocation location;
    if (!parse_remote_storage_id(request.item_id, &location)) {
        result.error = "Remote storage download requires an object ID returned by storage upload/list";
        return result;
    }

    if (request.progress_callback) request.progress_callback(0.0f);
    const auto remote = client->download_file(location.bucket, location.path);
    if (!remote.success) {
        result.error = remote.error.empty() ? "Remote storage download failed" : remote.error;
        return result;
    }

    std::filesystem::path target(request.target_path);
    std::error_code ec;
    if (!target.parent_path().empty()) {
        std::filesystem::create_directories(target.parent_path(), ec);
    }
    if (ec) {
        result.error = "Failed to create target directory: " + ec.message();
        return result;
    }

    std::ofstream output(target, std::ios::binary);
    if (!output) {
        result.error = "Failed to open download target";
        return result;
    }
    if (!remote.data.empty()) {
        output.write(reinterpret_cast<const char*>(remote.data.data()),
                     static_cast<std::streamsize>(remote.data.size()));
    }
    if (!output.good()) {
        result.error = "Failed to write download target";
        return result;
    }

    result.success = true;
    result.item.id = request.item_id;
    result.item.name = object_name_from_path(location.path);
    result.item.type = storage_type_for_bucket(location.bucket);
    result.item.path = net::to_utf8(target.wstring());
    result.item.size_bytes = remote.data.size();
    result.item.updated_at = current_timestamp();
    if (request.progress_callback) request.progress_callback(1.0f);
    return result;
}

DownloadResult StorageService::download_async(const DownloadRequest& request, 
                                              const std::function<void(const DownloadResult&)>& callback) {
    DownloadResult result = download(request);
    if (callback) callback(result);
    return result;
}

ListResult StorageService::list(const ListRequest& request) {
    ListResult result;
    result.limit = request.limit;
    result.offset = request.offset;

    auto* client = configured_storage_client();
    if (!client) {
        result.error = storage_backend_error();
        return result;
    }

    aml::supabase::SupabaseClient::StorageListOptions options;
    options.bucket = get_storage_type_path(request.type);
    options.prefix = request.parent_id;
    options.limit = request.limit;
    options.offset = request.offset;
    options.sort_by = request.sort_by;
    options.sort_order_asc = !request.sort_descending;

    const auto remote = client->list_files(options);
    if (!remote.success) {
        result.error = remote.error.empty() ? "Remote storage listing failed" : remote.error;
        return result;
    }

    for (const auto& remote_item : remote.items) {
        const std::string path = storage_object_path(remote_item);
        if (path.empty()) continue;

        StorageItem item;
        item.id = make_remote_storage_id(options.bucket, path);
        item.name = remote_item.name.empty() ? object_name_from_path(path) : remote_item.name;
        item.type = request.type;
        item.path = path;
        item.size_bytes = remote_item.size;
        item.mime_type = remote_item.mime_type;
        item.created_at = remote_item.created_at;
        item.updated_at = remote_item.updated_at;
        item.metadata = remote_item.metadata;
        result.items.push_back(std::move(item));
    }

    result.total_count = static_cast<int>(result.items.size());
    result.success = true;
    return result;
}

bool StorageService::delete_item(const std::string& item_id, std::string* error) {
    auto* client = configured_storage_client();
    if (!client) {
        if (error) *error = storage_backend_error();
        return false;
    }

    RemoteStorageLocation location;
    if (!parse_remote_storage_id(item_id, &location)) {
        if (error) *error = "Remote storage delete requires an object ID returned by storage upload/list";
        return false;
    }

    if (client->delete_file(location.bucket, location.path)) return true;
    if (error) *error = "Remote storage delete failed";
    return false;
}

bool StorageService::rename_item(const std::string& item_id, const std::string& new_name, std::string* error) {
    (void)item_id;
    (void)new_name;
    const std::string backend_error = storage_backend_error();
    if (error) {
        *error = backend_error.empty()
            ? "Remote storage rename is unavailable: the configured storage client exposes no rename operation"
            : backend_error;
    }
    return false;
}

bool StorageService::move_item(const std::string& item_id, const std::string& new_parent_id, std::string* error) {
    (void)item_id;
    (void)new_parent_id;
    const std::string backend_error = storage_backend_error();
    if (error) {
        *error = backend_error.empty()
            ? "Remote storage move is unavailable: the configured storage client exposes no move operation"
            : backend_error;
    }
    return false;
}

bool StorageService::get_metadata(const std::string& item_id, std::map<std::string, std::string>* metadata, std::string* error) {
    if (metadata) metadata->clear();

    auto* client = configured_storage_client();
    if (!client) {
        if (error) *error = storage_backend_error();
        return false;
    }

    RemoteStorageLocation location;
    if (!parse_remote_storage_id(item_id, &location)) {
        if (error) *error = "Remote storage metadata requires an object ID returned by storage upload/list";
        return false;
    }

    aml::supabase::SupabaseClient::StorageListOptions options;
    options.bucket = location.bucket;
    options.prefix = location.path;
    options.limit = 100;
    const auto remote = client->list_files(options);
    if (!remote.success) {
        if (error) *error = remote.error.empty() ? "Remote storage metadata lookup failed" : remote.error;
        return false;
    }

    const std::string requested_name = object_name_from_path(location.path);
    for (const auto& item : remote.items) {
        const std::string path = storage_object_path(item);
        if (path != location.path && object_name_from_path(path) != requested_name) continue;
        if (metadata) *metadata = item.metadata;
        return true;
    }

    if (error) *error = "Remote storage item metadata was not found";
    return false;
}

bool StorageService::update_metadata(const std::string& item_id, const std::map<std::string, std::string>& metadata, std::string* error) {
    (void)item_id;
    (void)metadata;
    const std::string backend_error = storage_backend_error();
    if (error) {
        *error = backend_error.empty()
            ? "Remote storage metadata update is unavailable: the configured storage client exposes no metadata update operation"
            : backend_error;
    }
    return false;
}

StorageService::QuotaInfo StorageService::get_quota(std::string* error) {
    const std::string backend_error = storage_backend_error();
    if (error) {
        *error = backend_error.empty()
            ? "Remote storage quota is unavailable: the configured storage client exposes no quota operation"
            : backend_error;
    }
    return QuotaInfo();
}

std::string StorageService::get_local_cache_path(StorageType type) const {
    return net::to_utf8((std::filesystem::path(local_cache_dir_) / get_storage_type_path(type)).wstring());
}

bool StorageService::clear_local_cache(StorageType type, std::string* error) {
    std::filesystem::path dir = std::filesystem::path(local_cache_dir_) / get_storage_type_path(type);
    
    std::error_code ec;
    return std::filesystem::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// ServerManager Implementation
// ---------------------------------------------------------------------------

struct ServerManager::LocalServerTransport {
    HANDLE stdin_write = nullptr;
    HANDLE stdout_read = nullptr;
    std::thread reader;
    std::atomic_bool stop_reader{false};
    std::mutex write_mutex;
    std::mutex output_mutex;
    std::deque<std::string> output_lines;
};

ServerManager::ServerManager(const ServiceConfig& config) : config_(config) {}

ServerManager::~ServerManager() {
    std::vector<std::string> running;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running.reserve(running_servers_.size());
        for (const auto& entry : running_servers_) running.push_back(entry.first);
    }
    for (const auto& id : running) {
        std::string ignored;
        stop_local_server(id, &ignored);
    }
}

std::string ServerManager::make_request(const std::string& method, const std::string& endpoint,
                                         const std::string& body, std::string* error) {
    std::string url = config_.endpoint + endpoint;
    
    std::map<std::string, std::string> headers;
    if (!config_.api_key.empty()) {
        headers["X-API-Key"] = config_.api_key;
    }
    headers["Content-Type"] = "application/json";
    
    return make_http_request(method, url, body, headers, error, config_.timeout_seconds);
}

std::string ServerManager::build_auth_header() const {
    if (config_.api_key.empty()) return "";
    return "Bearer " + config_.api_key;
}

std::string ServerManager::get_server_path(const std::string& server_id) const {
    const bool valid_id = valid_server_identifier(server_id);
    wchar_t test_root[32768]{};
    const DWORD test_root_size = GetEnvironmentVariableW(
        L"AMALGAM_SERVER_ROOT", test_root,
        static_cast<DWORD>(std::size(test_root)));
    if (test_root_size > 0 && test_root_size < std::size(test_root)) {
        const std::filesystem::path root(test_root);
        return net::to_utf8((root / (valid_id ? net::to_wide(server_id)
                                                : L"__invalid_server_id__")).wstring());
    }
    // The launcher persists imported/custom server directories in servers.json.
    // Resolve that authoritative path first so console/start/backup actions do
    // not silently fall back to a similarly named folder under LocalAppData.
    const std::wstring registry_path = net::get_local_app_data_path() +
                                       L"\\amalgam\\servers.json";
    Json registry;
    std::string registry_error;
    if (valid_id && json_parse_file(registry_path, registry, &registry_error) && registry.isArray()) {
        for (size_t i = 0; i < registry.size(); ++i) {
            const Json& entry = registry[i];
            if (entry.get("name").as_str() != server_id) continue;
            const std::string configured = entry.get("server_directory").as_str();
            if (!configured.empty()) return configured;
        }
    }
    return net::to_utf8((std::filesystem::path(net::get_local_app_data_path()) /
                         L"Amalgam" / L"Servers" /
                         (valid_id ? net::to_wide(server_id) : L"__invalid_server_id__")).wstring());
}

ServerInfo ServerManager::create_server(const ServerCreateRequest& request, std::string* error) {
    ServerInfo info;
    if (!request.eula_accepted) {
        if (error) *error = "Minecraft EULA acceptance is required before creating a server";
        return info;
    }
    info.id = generate_uuid();
    info.name = request.name;
    auto& supabase = aml::supabase::SupabaseManager::instance();
    aml::essentials::ServerAddressResolver address_resolver;
    info.alias = request.alias.empty()
        ? aml::essentials::ServerAddressResolver::Generate(request.name)
        : aml::essentials::ServerAddressResolver::NormalizeAlias(request.alias);
    info.address = aml::essentials::ServerAddressResolver::ToAddress(info.alias);
    if (supabase.is_authenticated() && !address_resolver.IsAvailable(info.alias)) {
        if (error) *error = "Server address is unavailable or Supabase address migration is missing";
        return ServerInfo();
    }
    info.version = request.version;
    info.type = request.type;
    info.motd = request.motd;
    info.max_players = request.max_players;
    info.online = false;
    info.created_at = current_timestamp();
    
    // Create server directory
    std::filesystem::path server_dir = std::filesystem::path(get_server_path(info.id));
    std::error_code ec;
    if (!std::filesystem::create_directories(server_dir, ec) || ec) {
        if (error) {
            *error = "Failed to create server directory '" + net::to_utf8(server_dir.wstring()) + "'";
            if (ec) {
                *error += ": " + ec.message();
            } else {
                *error += ": directory already exists";
            }
        }
        return ServerInfo();
    }

    auto fail_creation = [&](std::string message) -> ServerInfo {
        std::error_code cleanup_ec;
        std::filesystem::remove_all(server_dir, cleanup_ec);
        if (cleanup_ec) {
            message += "; failed to remove partial server directory: " + cleanup_ec.message();
        }
        if (error) *error = std::move(message);
        return ServerInfo();
    };

    // start_local_server has no artifact download step and only runs server.jar.
    const std::filesystem::path artifact_path = server_dir / "server.jar";
    std::error_code artifact_ec;
    if (!std::filesystem::is_regular_file(artifact_path, artifact_ec)) {
        if (artifact_ec) {
            return fail_creation("Failed to inspect runnable server artifact '" +
                                 net::to_utf8(artifact_path.wstring()) + "': " + artifact_ec.message());
        }
        return fail_creation("Cannot create server: runnable server artifact '" +
                             net::to_utf8(artifact_path.wstring()) + "' is missing");
    }
    const uintmax_t artifact_size = std::filesystem::file_size(artifact_path, artifact_ec);
    if (artifact_ec) {
        return fail_creation("Failed to inspect runnable server artifact '" +
                             net::to_utf8(artifact_path.wstring()) + "': " + artifact_ec.message());
    }
    if (artifact_size == 0) {
        return fail_creation("Cannot create server: runnable server artifact '" +
                             net::to_utf8(artifact_path.wstring()) + "' is empty");
    }
    {
        std::ifstream artifact(artifact_path, std::ios::binary);
        if (!artifact) {
            return fail_creation(stream_failure(artifact_path, "Failed to open runnable server artifact"));
        }
    }

    // Create server.properties
    const std::filesystem::path props_path = server_dir / "server.properties";
    std::ofstream props(props_path);
    if (!props.is_open()) {
        return fail_creation(stream_failure(props_path, "Failed to open server properties for writing"));
    }
    props << "server-name=" << info.name << "\n";
    props << "amalgam-alias=" << info.alias << "\n";
    props << "amalgam-address=" << info.address << "\n";
    props << "motd=" << info.motd << "\n";
    props << "max-players=" << info.max_players << "\n";
    props << "gamemode=" << request.game_mode << "\n";
    props << "enable-command-block=" << (request.enable_command_blocks ? "true" : "false") << "\n";
    props << "online-mode=" << (request.online_mode ? "true" : "false") << "\n";
    props << "pvp=true\n";
    props << "view-distance=10\n";
    props << "simulation-distance=10\n";
    props << "level-name=" << (request.world_name.empty() ? "world" : request.world_name) << "\n";
    props << "level-seed=" << request.seed << "\n";
    props << "allow-flight=false\n";
    props << "allow-nether=true\n";
    props << "white-list=false\n";
    props.flush();
    if (!props) {
        const std::string message = stream_failure(props_path, "Failed to write server properties");
        props.close();
        return fail_creation(message);
    }
    props.close();
    if (!props) {
        return fail_creation(stream_failure(props_path, "Failed to close server properties after writing"));
    }
    
    // Create eula.txt
    const std::filesystem::path eula_path = server_dir / "eula.txt";
    std::ofstream eula(eula_path);
    if (!eula.is_open()) {
        return fail_creation(stream_failure(eula_path, "Failed to open EULA for writing"));
    }
    eula << "eula=true\n";
    eula.flush();
    if (!eula) {
        const std::string message = stream_failure(eula_path, "Failed to write EULA");
        eula.close();
        return fail_creation(message);
    }
    eula.close();
    if (!eula) {
        return fail_creation(stream_failure(eula_path, "Failed to close EULA after writing"));
    }
    
    if (supabase.is_authenticated()) {
        aml::supabase::SupabaseServer remote;
        remote.id = info.id;
        remote.name = info.name;
        remote.alias = info.alias;
        remote.address = info.address;
        remote.host = "127.0.0.1";
        remote.port = info.port;
        remote.version = info.version;
        remote.type = info.type;
        remote.motd = info.motd;
        remote.max_players = info.max_players;
        remote.online = false;
        if (supabase.create_server(remote).id.empty()) {
            return fail_creation("Server was created locally but could not be registered for its .amalgam address");
        }
    }
    return info;
}

bool ServerManager::delete_server(const std::string& server_id, std::string* error) {
    std::filesystem::path server_dir = std::filesystem::path(get_server_path(server_id));
    
    std::error_code ec;
    return std::filesystem::remove_all(server_dir, ec);
}

bool ServerManager::update_server(const std::string& server_id, const ServerCreateRequest& updates, std::string* error) {
    ServerInfo info = get_server_info(server_id, error);
    if (info.id.empty()) {
        return false;
    }
    
    std::filesystem::path props_path = std::filesystem::path(get_server_path(server_id)) / "server.properties";
    
    std::ofstream props(props_path);
    if (!props.is_open()) {
        if (error) *error = stream_failure(props_path, "Failed to open server properties for writing");
        return false;
    }

    props << "server-name=" << (updates.name.empty() ? info.name : updates.name) << "\n";
    props << "motd=" << (updates.motd.empty() ? info.motd : updates.motd) << "\n";
    props << "max-players=" << (updates.max_players <= 0 ? info.max_players : updates.max_players) << "\n";
    props << "gamemode=" << (updates.game_mode.empty() ? "survival" : updates.game_mode) << "\n";
    props << "online-mode=" << (updates.online_mode ? "true" : "false") << "\n";
    props << "pvp=" << "true\n";
    props << "enable-command-block=" << (updates.enable_command_blocks ? "true" : "false") << "\n";
    props << "view-distance=10\n";
    props << "simulation-distance=10\n";
    props << "level-name=world\n";
    props << "level-seed=\n";
    props << "allow-flight=false\n";
    props << "allow-nether=true\n";
    props << "white-list=false\n";
    props.flush();
    if (!props) {
        if (error) *error = stream_failure(props_path, "Failed to write server properties");
        props.close();
        return false;
    }
    props.close();
    if (!props) {
        if (error) *error = stream_failure(props_path, "Failed to close server properties after writing");
        return false;
    }
    
    return true;
}

ServerControlResult ServerManager::control_server(const ServerControlRequest& request) {
    ServerControlResult result;
    
    if (request.action == "start") {
        return start_server(request.server_id);
    } else if (request.action == "stop") {
        return stop_server(request.server_id);
    } else if (request.action == "restart") {
        return restart_server(request.server_id);
    }
    
    result.error = "Unknown action: " + request.action;
    return result;
}

ServerControlResult ServerManager::start_server(const std::string& server_id) {
    ServerControlResult result;
    
    std::string err;
    if (!start_local_server(server_id, "", 2048, &err)) {
        result.error = err;
        return result;
    }
    
    result.success = true;
    result.message = "Server started";
    
    return result;
}

ServerControlResult ServerManager::stop_server(const std::string& server_id) {
    ServerControlResult result;
    
    std::string err;
    if (!stop_local_server(server_id, &err)) {
        result.error = err;
        return result;
    }
    
    result.success = true;
    result.message = "Server stopped";
    
    return result;
}

ServerControlResult ServerManager::restart_server(const std::string& server_id) {
    ServerControlResult result;
    
    ServerInfo info = get_server_info(server_id, &result.error);
    if (info.id.empty()) {
        result.error = "Server not found";
        return result;
    }
    
    // Stop then start
    stop_server(server_id);
    return start_server(server_id);
}

ServerInfo ServerManager::get_server_info(const std::string& server_id, std::string* error) {
    ServerInfo info;
    info.id = server_id;
    info.name = server_id;

    Json registry;
    std::string registry_error;
    const std::wstring registry_path = net::get_local_app_data_path() +
                                       L"\\amalgam\\servers.json";
    if (json_parse_file(registry_path, registry, &registry_error) && registry.isArray()) {
        for (size_t i = 0; i < registry.size(); ++i) {
            const Json& entry = registry[i];
            if (entry.get("name").as_str() != server_id) continue;
            info.name = entry.get("name").as_str(server_id);
            info.version = entry.get("minecraft_version").as_str("1.20.1");
            info.port = static_cast<int>(entry.get("port").as_int(25565));
            info.max_players = static_cast<int>(entry.get("max_players").as_int(20));
            info.allocated_ram_mb =
                static_cast<int>(entry.get("allocated_ram_mb").as_int(2048));
            break;
        }
    }
    
    std::filesystem::path server_dir = std::filesystem::path(get_server_path(server_id));
    if (!std::filesystem::exists(server_dir)) {
        if (error) *error = "Server directory not found";
        return ServerInfo();
    }
    
    // Read server.properties
    std::filesystem::path props_path = server_dir / "server.properties";
    if (std::filesystem::exists(props_path)) {
        std::ifstream props(props_path);
        std::string line;
        while (std::getline(props, line)) {
            size_t eq = line.find('=');
            if (eq != std::string::npos) {
                std::string key = line.substr(0, eq);
                std::string value = line.substr(eq + 1);
                
                if (key == "server-name") info.name = value;
                else if (key == "amalgam-alias") info.alias = value;
                else if (key == "amalgam-address") info.address = value;
                else if (key == "motd") info.motd = value;
                else if (key == "max-players") info.max_players = std::stoi(value);
                else if (key == "gamemode") info.properties["gamemode"] = value;
            }
        }
    }
    
    info.host = "localhost";
    if (info.alias.empty()) info.alias = aml::essentials::ServerAddressResolver::Generate(info.name);
    if (info.address.empty()) info.address = aml::essentials::ServerAddressResolver::ToAddress(info.alias);
    if (info.port <= 0) info.port = 25565;
    info.online = false;
    info.ping_ms = 0;
    
    return info;
}

std::vector<ServerInfo> ServerManager::list_servers(std::string* error) {
    std::vector<ServerInfo> servers;
    
    std::filesystem::path servers_dir = std::filesystem::path(net::get_local_app_data_path()) / L"Amalgam\\Servers";
    
    if (!std::filesystem::exists(servers_dir)) {
        return servers;
    }
    
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(servers_dir, ec)) {
        if (ec) break;
        if (!entry.is_directory(ec)) continue;
        
        ServerInfo info;
        info.id = net::to_utf8(entry.path().filename().wstring());
        info.name = info.id;
        info.host = "localhost";
        info.port = 25565;
        info.online = false;
        
        // Try to read server.properties
        std::filesystem::path props_path = entry.path() / "server.properties";
        if (std::filesystem::exists(props_path)) {
            std::ifstream props(props_path);
            std::string line;
            while (std::getline(props, line)) {
                size_t eq = line.find('=');
                if (eq != std::string::npos) {
                    std::string key = line.substr(0, eq);
                    std::string value = line.substr(eq + 1);
                    
                    if (key == "server-name") info.name = value;
                    else if (key == "amalgam-alias") info.alias = value;
                    else if (key == "amalgam-address") info.address = value;
                    else if (key == "motd") info.motd = value;
                    else if (key == "max-players") info.max_players = std::stoi(value);
                }
            }
        }
        
        if (info.alias.empty()) info.alias = aml::essentials::ServerAddressResolver::Generate(info.name);
        if (info.address.empty()) info.address = aml::essentials::ServerAddressResolver::ToAddress(info.alias);
        servers.push_back(info);
    }
    
    return servers;
}

bool ServerManager::ping_server(const std::string& server_id, ServerInfo* info) {
    auto it = running_servers_.find(server_id);
    ServerInfo* target = info ? info : (it != running_servers_.end() ? &it->second : nullptr);
    if (!target) {
        if (info) {
            *info = get_server_info(server_id);
        }
        return false;
    }
    
    if (!target->online && (!it->second.process_handle || WaitForSingleObject(it->second.process_handle, 0) == WAIT_OBJECT_0)) {
        target->online = false;
        target->ping_ms = 0;
        return false;
    }
    
    auto start = std::chrono::steady_clock::now();
    
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        target->online = false;
        target->ping_ms = 0;
        return false;
    }
    
    DWORD timeout = 3000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));
    
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(target->port));
    
    std::string host = target->host.empty() ? "127.0.0.1" : target->host;
    if (host == "localhost" || host == "127.0.0.1") {
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    } else {
        struct addrinfo hints = {}, *result = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host.c_str(), nullptr, &hints, &result) == 0 && result) {
            addr.sin_addr = ((struct sockaddr_in*)result->ai_addr)->sin_addr;
            freeaddrinfo(result);
        } else {
            closesocket(sock);
            target->online = false;
            target->ping_ms = 0;
            return false;
        }
    }
    
    int connect_result = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
    auto end = std::chrono::steady_clock::now();
    int ping_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
    
    if (connect_result == 0) {
        target->online = true;
        target->ping_ms = ping_ms;
        target->last_ping = current_timestamp();
    } else {
        target->online = false;
        target->ping_ms = 0;
    }
    
    closesocket(sock);
    return target->online;
}

std::vector<ServerConsoleEntry> ServerManager::get_console_logs(const std::string& server_id, 
                                                                int limit, 
                                                                std::string* error) {
    std::vector<ServerConsoleEntry> entries;
    limit = std::clamp(limit, 1, 5000);

    // Live process output is available immediately, including bootstrap lines
    // emitted before Minecraft creates logs/latest.log.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto transport_it = local_transports_.find(server_id);
        if (transport_it != local_transports_.end() && transport_it->second) {
            std::lock_guard<std::mutex> output_lock(transport_it->second->output_mutex);
            const auto& lines = transport_it->second->output_lines;
            const size_t first = lines.size() > static_cast<size_t>(limit)
                ? lines.size() - static_cast<size_t>(limit) : 0;
            for (size_t i = first; i < lines.size(); ++i) {
                ServerConsoleEntry entry;
                entry.timestamp = current_timestamp();
                entry.message = lines[i];
                entry.level = lines[i].find("[ERROR]") != std::string::npos ? "error" :
                              lines[i].find("[WARN]") != std::string::npos ? "warning" : "info";
                entries.push_back(std::move(entry));
            }
        }
    }
    if (!entries.empty()) return entries;
    
    std::filesystem::path log_path = std::filesystem::path(get_server_path(server_id)) / "logs" / "latest.log";
    
    if (!std::filesystem::exists(log_path)) {
        return entries;
    }
    
    std::ifstream log(log_path);
    std::string line;
    std::deque<std::string> tail;
    while (std::getline(log, line)) {
        tail.push_back(line);
        if (tail.size() > static_cast<size_t>(limit)) tail.pop_front();
    }

    for (const auto& log_line : tail) {
        ServerConsoleEntry entry;
        entry.timestamp = current_timestamp();
        entry.message = log_line;
        
        // Simple log parsing
        if (log_line.find("[INFO]") != std::string::npos) {
            entry.level = "info";
        } else if (log_line.find("[WARN]") != std::string::npos) {
            entry.level = "warning";
        } else if (log_line.find("[ERROR]") != std::string::npos) {
            entry.level = "error";
        } else {
            entry.level = "info";
        }
        
        entries.push_back(entry);
    }
    
    return entries;
}

bool ServerManager::send_command(const std::string& server_id, const std::string& command, 
                                std::string* response, std::string* error) {
    if (response) response->clear();
    if (command.empty()) {
        if (error) *error = "Command cannot be empty";
        return false;
    }
    if (command.size() > 4096 || command.find('\n') != std::string::npos ||
        command.find('\r') != std::string::npos) {
        if (error) *error = "Command must be one line and no more than 4096 characters";
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = running_servers_.find(server_id);
    if (it == running_servers_.end()) {
        if (error) *error = "Server is not running";
        return false;
    }
    
    auto& server = it->second;
    if (!server.process_handle || !server.online) {
        if (error) *error = "Server is not running";
        return false;
    }

    auto transport_it = local_transports_.find(server_id);
    if (transport_it == local_transports_.end() || !transport_it->second ||
        !transport_it->second->stdin_write) {
        if (error) *error = "Server command channel is not available";
        return false;
    }

    std::lock_guard<std::mutex> write_lock(transport_it->second->write_mutex);
    const std::string payload = command + "\n";
    size_t offset = 0;
    while (offset < payload.size()) {
        DWORD written = 0;
        const DWORD remaining = static_cast<DWORD>(payload.size() - offset);
        if (!WriteFile(transport_it->second->stdin_write, payload.data() + offset,
                       remaining, &written, nullptr) || written == 0) {
            if (error) *error = "Failed to write command to the local server process";
            return false;
        }
        offset += written;
    }
    if (response) *response = "Command sent to local server";
    return true;
}

bool ServerManager::upload_server_file(const std::string& server_id, const std::string& path, 
                                       const std::vector<uint8_t>& content, std::string* error) {
    std::filesystem::path target;
    if (!resolve_server_relative_path(std::filesystem::path(get_server_path(server_id)), path,
                                      &target, error)) return false;

    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    if (ec) {
        if (error) *error = "Failed to create target directory";
        return false;
    }
    
    std::ofstream file(target, std::ios::binary);
    if (!file) {
        if (error) *error = "Failed to open file for writing";
        return false;
    }
    
    file.write(reinterpret_cast<const char*>(content.data()), content.size());
    return true;
}

bool ServerManager::download_server_file(const std::string& server_id, const std::string& path, 
                                         std::vector<uint8_t>* content, std::string* error) {
    std::filesystem::path source;
    if (!resolve_server_relative_path(std::filesystem::path(get_server_path(server_id)), path,
                                      &source, error)) return false;

    if (!std::filesystem::exists(source)) {
        if (error) *error = "File not found";
        return false;
    }
    
    std::ifstream file(source, std::ios::binary | std::ios::ate);
    if (!file) {
        if (error) *error = "Failed to open file for reading";
        return false;
    }
    
    size_t size = file.tellg();
    file.seekg(0, std::ios::beg);
    
    content->resize(size);
    file.read(reinterpret_cast<char*>(content->data()), size);
    
    return true;
}

bool ServerManager::delete_server_file(const std::string& server_id, const std::string& path, std::string* error) {
    std::filesystem::path target;
    if (!resolve_server_relative_path(std::filesystem::path(get_server_path(server_id)), path,
                                      &target, error)) return false;

    std::error_code ec;
    return std::filesystem::remove(target, ec);
}

bool ServerManager::create_backup(const std::string& server_id, const std::string& name, 
                                 std::string* backup_id, std::string* error) {
    std::string id = generate_uuid();
    if (backup_id) *backup_id = id;
    
    std::filesystem::path backup_dir = std::filesystem::path(net::get_local_app_data_path()) /
                                      L"Amalgam" / L"Backups" / net::to_wide(id);
    std::filesystem::path server_dir = std::filesystem::path(get_server_path(server_id));
    
    std::error_code ec;
    std::filesystem::create_directories(backup_dir, ec);
    if (ec) {
        if (error) *error = "Failed to create backup directory";
        return false;
    }
    
    // Copy server directory to backup
    std::filesystem::copy(server_dir, backup_dir, std::filesystem::copy_options::recursive, ec);
    if (ec) {
        if (error) *error = "Failed to copy server files";
        return false;
    }
    
    return true;
}

bool ServerManager::restore_backup(const std::string& server_id, const std::string& backup_id, 
                                  std::string* error) {
    if (!valid_server_identifier(backup_id)) {
        if (error) *error = "Backup ID is invalid";
        return false;
    }
    std::filesystem::path backup_dir = std::filesystem::path(net::get_local_app_data_path()) /
                                      L"Amalgam" / L"Backups" / net::to_wide(backup_id);
    std::filesystem::path server_dir = std::filesystem::path(get_server_path(server_id));
    
    std::error_code ec;
    std::filesystem::create_directories(server_dir, ec);
    if (ec) {
        if (error) *error = "Failed to create server directory";
        return false;
    }
    
    // Copy backup to server
    std::filesystem::copy(backup_dir, server_dir, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        if (error) *error = "Failed to restore backup files";
        return false;
    }
    
    return true;
}

std::vector<std::map<std::string, std::string>> ServerManager::list_backups(const std::string& server_id, 
                                                                           std::string* error) {
    std::vector<std::map<std::string, std::string>> backups;
    
    std::filesystem::path backups_dir = std::filesystem::path(net::get_local_app_data_path()) / L"Amalgam\\Backups";
    
    if (!std::filesystem::exists(backups_dir)) {
        return backups;
    }
    
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(backups_dir, ec)) {
        if (ec) break;
        if (!entry.is_directory(ec)) continue;
        
        std::map<std::string, std::string> backup;
        backup["id"] = net::to_utf8(entry.path().filename().wstring());
        backup["name"] = backup["id"];
        backup["created_at"] = std::to_string(std::filesystem::last_write_time(entry.path(), ec).time_since_epoch().count() / 10000000);
        
        // Calculate size
        uint64_t size = 0;
        for (const auto& f : std::filesystem::recursive_directory_iterator(entry.path(), ec)) {
            if (ec) break;
            if (f.is_regular_file(ec)) {
                size += f.file_size(ec);
            }
        }
        backup["size_bytes"] = std::to_string(size);
        
        backups.push_back(backup);
    }
    
    return backups;
}

std::vector<std::map<std::string, std::string>> ServerManager::list_templates(std::string* error) {
    if (error) *error = "Server templates are unavailable: no configured template source";
    return {};
}

ServerCreateRequest ServerManager::get_template(const std::string& template_id, std::string* error) {
    if (template_id.empty()) {
        if (error) *error = "Template ID is required";
    } else if (error) {
        *error = "Server template '" + template_id + "' is unavailable: no configured template source";
    }
    return ServerCreateRequest();
}

void ServerManager::set_local_java_root(const std::wstring& root) {
    std::lock_guard<std::mutex> lock(mutex_);
    configured_java_root_ = root;
}

std::wstring ServerManager::local_java_root() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return java::managed_root(configured_java_root_);
}

std::wstring ServerManager::resolve_local_server_java(const std::string& minecraft_version,
                                                     std::string* error) const {
    java::JavaRuntimeManager runtime_manager(local_java_root());
    java::MinecraftVersion minecraft{minecraft_version.empty() ? "1.20.1" : minecraft_version};
    const int required_major = runtime_manager.ResolveRequiredJava(
        minecraft, java::LoaderType::Vanilla, java::ServerType::Local);
    std::string resolve_error;
    const std::wstring home = runtime_manager.Resolve(
        required_major, java::scan_installed(), {}, &resolve_error);
    if (home.empty()) {
        if (error) *error = resolve_error.empty()
            ? "Unable to resolve a compatible managed Java runtime"
            : resolve_error;
        return std::wstring();
    }
    return home + L"\\bin\\java.exe";
}

bool ServerManager::start_local_server(const std::string& server_id, const std::string& java_path, 
                                       int ram_mb, std::string* error) {
    ServerInfo info = get_server_info(server_id, error);
    if (info.id.empty()) {
        return false;
    }
    
    if (is_local_server_running(server_id)) {
        if (error) *error = "Server is already running";
        return false;
    }
    // A process that exited on its own leaves a record with no live process
    // behind it; release that record so this start owns the server again.
    forget_local_server(server_id);
    
    std::filesystem::path server_dir = std::filesystem::path(get_server_path(server_id));
    
    std::wstring jar_path = (server_dir / "server.jar").wstring();
    if (!net::file_exists(jar_path)) {
        if (error) *error = "server.jar not found at " + net::to_utf8(jar_path);
        return false;
    }

    // A port another process answers on makes the server exit as soon as it
    // binds, which used to look like a successful start that silently stopped.
    // Refuse here so the caller gets the reason instead of a dead process.
    if (info.port > 0 && port_answers_locally(info.port)) {
        if (error) *error = "port " + std::to_string(info.port) + " is already in use";
        return false;
    }
    
    std::wstring wjava;
    if (!java_path.empty()) {
        wjava = net::to_wide(java_path);
    } else {
        wjava = resolve_local_server_java(info.version, error);
        if (wjava.empty()) return false;
    }
    const int memory_mb = ram_mb > 0 ? ram_mb : 2048;
    std::wstring full_cmd = L"\"" + wjava + L"\" -Xms512M -Xmx" +
                            std::to_wstring(memory_mb) + L"M -jar server.jar nogui";

    SECURITY_ATTRIBUTES pipe_security{};
    pipe_security.nLength = sizeof(pipe_security);
    pipe_security.bInheritHandle = TRUE;
    HANDLE stdout_read = nullptr;
    HANDLE stdout_write = nullptr;
    HANDLE stdin_read = nullptr;
    HANDLE stdin_write = nullptr;
    if (!CreatePipe(&stdout_read, &stdout_write, &pipe_security, 0) ||
        !SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0) ||
        !CreatePipe(&stdin_read, &stdin_write, &pipe_security, 0) ||
        !SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0)) {
        const DWORD pipe_error = GetLastError();
        if (stdout_read) CloseHandle(stdout_read);
        if (stdout_write) CloseHandle(stdout_write);
        if (stdin_read) CloseHandle(stdin_read);
        if (stdin_write) CloseHandle(stdin_write);
        if (error) *error = "Failed to create local server console pipes: error " +
                            std::to_string(pipe_error);
        return false;
    }

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = stdin_read;
    si.hStdOutput = stdout_write;
    si.hStdError = stdout_write;
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> mutable_command(full_cmd.begin(), full_cmd.end());
    mutable_command.push_back(L'\0');
    
    BOOL result = CreateProcessW(
        wjava.c_str(),
        mutable_command.data(),
        nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP | CREATE_UNICODE_ENVIRONMENT,
        nullptr, server_dir.wstring().c_str(),
        &si, &pi
    );

    // The parent must never retain the child sides; otherwise the reader will
    // not observe EOF after Java exits.
    CloseHandle(stdout_write);
    CloseHandle(stdin_read);
    
    if (!result) {
        const DWORD process_error = GetLastError();
        CloseHandle(stdout_read);
        CloseHandle(stdin_write);
        if (error) *error = "Failed to start server process: error " + std::to_string(process_error);
        return false;
    }
    
    CloseHandle(pi.hThread);
    
    info.process_handle = pi.hProcess;
    info.process_id = pi.dwProcessId;
    info.online = true;
    info.last_ping = current_timestamp();
    info.allocated_ram_mb = memory_mb;

    auto transport = std::make_unique<LocalServerTransport>();
    transport->stdin_write = stdin_write;
    transport->stdout_read = stdout_read;
    LocalServerTransport* transport_ptr = transport.get();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_servers_[server_id] = info;
        local_transports_[server_id] = std::move(transport);
    }

    transport_ptr->reader = std::thread([this, server_id, transport_ptr]() {
        std::string pending;
        char buffer[4096];
        auto publish_line = [this, &server_id, transport_ptr](std::string line) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) return;
            {
                std::lock_guard<std::mutex> output_lock(transport_ptr->output_mutex);
                transport_ptr->output_lines.push_back(line);
                while (transport_ptr->output_lines.size() > 2000)
                    transport_ptr->output_lines.pop_front();
            }
            std::vector<std::function<void(const std::string&, const std::string&)>> callbacks;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                callbacks = server_output_callbacks_;
            }
            for (auto& callback : callbacks) callback(server_id, line);
        };

        while (!transport_ptr->stop_reader.load()) {
            DWORD read = 0;
            if (!ReadFile(transport_ptr->stdout_read, buffer, sizeof(buffer), &read, nullptr) ||
                read == 0) {
                break;
            }
            pending.append(buffer, buffer + read);
            size_t newline = std::string::npos;
            while ((newline = pending.find('\n')) != std::string::npos) {
                publish_line(pending.substr(0, newline));
                pending.erase(0, newline + 1);
            }
        }
        if (!pending.empty()) publish_line(std::move(pending));
    });

    auto& supabase = aml::supabase::SupabaseManager::instance();
    if (supabase.is_authenticated()) {
        aml::supabase::SupabaseServer remote;
        remote.id = info.id;
        remote.name = info.name;
        remote.alias = info.alias;
        remote.address = info.address;
        remote.host = "127.0.0.1";
        remote.port = info.port;
        remote.version = info.version;
        remote.type = info.type;
        remote.motd = info.motd;
        remote.max_players = info.max_players;
        remote.online = true;
        supabase.update_server(remote);
    }
    
    std::vector<std::function<void(const std::string&)>> started_callbacks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        started_callbacks = server_started_callbacks_;
    }
    for (auto& cb : started_callbacks) {
        cb(server_id);
    }
    
    return true;
}

bool ServerManager::stop_local_server(const std::string& server_id, std::string* error) {
    ServerInfo server;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = running_servers_.find(server_id);
        if (it == running_servers_.end()) {
            if (error) *error = "Server is not running";
            return false;
        }
        server = it->second;
    }

    std::string command_response;
    std::string command_error;
    const bool graceful_requested = send_command(server_id, "stop", &command_response,
                                                 &command_error);
    bool forced = false;
    if (server.process_handle) {
        DWORD wait_result = graceful_requested
            ? WaitForSingleObject(server.process_handle, 30000)
            : WAIT_TIMEOUT;
        if (wait_result == WAIT_TIMEOUT) {
            forced = true;
            TerminateProcess(server.process_handle, 1);
            wait_result = WaitForSingleObject(server.process_handle, 5000);
        }
        if (wait_result != WAIT_OBJECT_0) {
            if (error) *error = "Local server process did not exit";
            return false;
        }
    }

    forget_local_server(server_id);

    server.online = false;
    auto& supabase = aml::supabase::SupabaseManager::instance();
    if (supabase.is_authenticated()) {
        aml::supabase::SupabaseServer remote;
        remote.id = server.id;
        remote.name = server.name;
        remote.alias = server.alias;
        remote.address = server.address;
        remote.online = false;
        supabase.update_server(remote);
    }
    std::vector<std::function<void(const std::string&)>> stopped_callbacks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_callbacks = server_stopped_callbacks_;
    }
    for (auto& cb : stopped_callbacks) {
        cb(server_id);
    }
    if (forced && error) {
        *error = graceful_requested
            ? "Server did not respond to stop within 30 seconds and was force-closed"
            : (command_error.empty() ? "Server was force-closed" : command_error);
    }
    return true;
}

bool ServerManager::is_local_server_running(const std::string& server_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = running_servers_.find(server_id);
    if (it == running_servers_.end() || !it->second.process_handle) return false;
    // A supervised server is running while its process handle is still live.
    return WaitForSingleObject(it->second.process_handle, 0) != WAIT_OBJECT_0;
}

void ServerManager::forget_local_server(const std::string& server_id) {
    ServerInfo info;
    std::unique_ptr<LocalServerTransport> transport;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto running = running_servers_.find(server_id);
        if (running != running_servers_.end()) {
            info = running->second;
            running_servers_.erase(running);
        }
        auto transport_it = local_transports_.find(server_id);
        if (transport_it != local_transports_.end()) {
            transport = std::move(transport_it->second);
            local_transports_.erase(transport_it);
        }
    }
    if (transport) {
        transport->stop_reader = true;
        if (transport->stdin_write) {
            CloseHandle(transport->stdin_write);
            transport->stdin_write = nullptr;
        }
        if (transport->reader.joinable()) {
            CancelSynchronousIo(static_cast<HANDLE>(transport->reader.native_handle()));
        }
        if (transport->stdout_read) {
            CloseHandle(transport->stdout_read);
            transport->stdout_read = nullptr;
        }
        if (transport->reader.joinable()) transport->reader.join();
    }
    if (info.process_handle) CloseHandle(info.process_handle);
}

void ServerManager::on_server_started(const std::function<void(const std::string&)>& callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    server_started_callbacks_.push_back(callback);
}

void ServerManager::on_server_stopped(const std::function<void(const std::string&)>& callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    server_stopped_callbacks_.push_back(callback);
}

void ServerManager::on_server_output(const std::function<void(const std::string&, const std::string&)>& callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    server_output_callbacks_.push_back(callback);
}

// ---------------------------------------------------------------------------
// NodeService Implementation
// ---------------------------------------------------------------------------

NodeService::NodeService(const ServiceConfig& config) : config_(config) {}

NodeService::~NodeService() {}

std::string NodeService::make_request(const std::string& method, const std::string& endpoint,
                                       const std::string& body, std::string* error) {
    std::string url = config_.endpoint + endpoint;
    
    std::map<std::string, std::string> headers;
    if (!config_.api_key.empty()) {
        headers["X-API-Key"] = config_.api_key;
    }
    headers["Content-Type"] = "application/json";
    
    return make_http_request(method, url, body, headers, error, config_.timeout_seconds);
}

std::string NodeService::build_auth_header() const {
    if (config_.api_key.empty()) return "";
    return "Bearer " + config_.api_key;
}

NodeInfo NodeService::register_node(const NodeInfo& node, std::string* error) {
    auto& mgr = aml::servers::ServerManager::instance();
    aml::servers::Node n = info_to_node(node);
    n = mgr.register_node(n);
    if (n.id.empty()) {
        if (error) *error = "Failed to register node";
        return NodeInfo();
    }
    return node_to_info(n);
}

bool NodeService::deregister_node(const std::string& node_id, std::string* error) {
    auto& mgr = aml::servers::ServerManager::instance();
    if (!mgr.deregister_node(node_id)) {
        if (error) *error = "Failed to deregister node";
        return false;
    }
    return true;
}

NodeInfo NodeService::get_node(const std::string& node_id, std::string* error) {
    auto& mgr = aml::servers::ServerManager::instance();
    aml::servers::Node n = mgr.get_node(node_id);
    if (n.id.empty()) {
        if (error) *error = "Node not found";
        return NodeInfo();
    }
    return node_to_info(n);
}

std::vector<NodeInfo> NodeService::list_nodes(const std::string& region,
                                               const std::vector<std::string>& tags,
                                               std::string* error) {
    auto& mgr = aml::servers::ServerManager::instance();
    auto nodes = mgr.get_nodes();
    std::vector<NodeInfo> result;
    result.reserve(nodes.size());
    for (const auto& n : nodes) {
        NodeInfo info = node_to_info(n);
        if (!region.empty() && info.region != region) continue;
        if (!tags.empty()) {
            bool has_tag = false;
            for (const auto& t : tags) {
                for (const auto& nt : info.tags) {
                    if (nt == t) { has_tag = true; break; }
                }
                if (has_tag) break;
            }
            if (!has_tag) continue;
        }
        result.push_back(std::move(info));
    }
    return result;
}

bool NodeService::update_node(const std::string& node_id, const NodeInfo& updates, std::string* error) {
    auto& mgr = aml::servers::ServerManager::instance();
    aml::servers::Node n = mgr.get_node(node_id);
    if (n.id.empty()) {
        if (error) *error = "Node not found";
        return false;
    }
    if (!updates.name.empty()) n.name = updates.name;
    if (!updates.host.empty()) n.host = updates.host;
    if (updates.port > 0) n.port = updates.port;
    n.metadata["region"] = updates.region;
    n.metadata["zone"] = updates.zone;
    if (!mgr.update_node(n)) {
        if (error) *error = "Failed to update node";
        return false;
    }
    return true;
}

bool NodeService::ping_node(const std::string& node_id, int* latency_ms, std::string* error) {
    auto& mgr = aml::servers::ServerManager::instance();
    aml::servers::Node n = mgr.get_node(node_id);
    if (n.id.empty()) {
        if (error) *error = "Node not found";
        return false;
    }
    std::string host = n.host.empty() ? "127.0.0.1" : n.host;
    int port = n.port > 0 ? n.port : 22;

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        if (latency_ms) *latency_ms = 0;
        return false;
    }

    DWORD timeout = 3000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));

    if (host == "localhost" || host == "127.0.0.1") {
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    } else {
        struct addrinfo hints = {}, *ai_result = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host.c_str(), nullptr, &hints, &ai_result) == 0 && ai_result) {
            addr.sin_addr = ((struct sockaddr_in*)ai_result->ai_addr)->sin_addr;
            freeaddrinfo(ai_result);
        } else {
            closesocket(sock);
            if (latency_ms) *latency_ms = 0;
            return false;
        }
    }

    auto start = std::chrono::steady_clock::now();
    int rc = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
    auto end = std::chrono::steady_clock::now();
    int ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
    closesocket(sock);

    if (rc == 0) {
        if (latency_ms) *latency_ms = ms;
        return true;
    }
    if (latency_ms) *latency_ms = 0;
    return false;
}

NodeInfo NodeService::get_node_health(const std::string& node_id, std::string* error) {
    (void)node_id;
    if (error) *error = "Node health is unavailable: no node health transport is configured";
    return NodeInfo();
}

ClusterInfo NodeService::create_cluster(const std::string& name, const std::vector<std::string>& node_ids,
                                       std::string* error) {
    auto& mgr = aml::servers::ServerManager::instance();
    aml::servers::Cluster c;
    c.name = name;
    c.node_ids = node_ids;
    c = mgr.create_cluster(c);
    if (c.id.empty()) {
        if (error) *error = "Failed to create cluster";
        return ClusterInfo();
    }
    return cluster_to_info(c, mgr);
}

bool NodeService::delete_cluster(const std::string& cluster_id, std::string* error) {
    auto& mgr = aml::servers::ServerManager::instance();
    if (!mgr.delete_cluster(cluster_id)) {
        if (error) *error = "Failed to delete cluster";
        return false;
    }
    return true;
}

ClusterInfo NodeService::get_cluster(const std::string& cluster_id, std::string* error) {
    auto& mgr = aml::servers::ServerManager::instance();
    auto clusters = mgr.get_clusters();
    for (const auto& c : clusters) {
        if (c.id == cluster_id) {
            return cluster_to_info(c, mgr);
        }
    }
    if (error) *error = "Cluster not found";
    return ClusterInfo();
}

std::vector<ClusterInfo> NodeService::list_clusters(std::string* error) {
    auto& mgr = aml::servers::ServerManager::instance();
    auto clusters = mgr.get_clusters();
    std::vector<ClusterInfo> result;
    result.reserve(clusters.size());
    for (const auto& c : clusters) {
        result.push_back(cluster_to_info(c, mgr));
    }
    return result;
}

bool NodeService::add_node_to_cluster(const std::string& cluster_id, const std::string& node_id,
                                      std::string* error) {
    auto& mgr = aml::servers::ServerManager::instance();
    if (!mgr.add_node_to_cluster(cluster_id, node_id)) {
        if (error) *error = "Failed to add node to cluster";
        return false;
    }
    return true;
}

bool NodeService::remove_node_from_cluster(const std::string& cluster_id, const std::string& node_id,
                                            std::string* error) {
    auto& mgr = aml::servers::ServerManager::instance();
    if (!mgr.remove_node_from_cluster(cluster_id, node_id)) {
        if (error) *error = "Failed to remove node from cluster";
        return false;
    }
    return true;
}

bool NodeService::store_on_node(const std::string& node_id, const StorageNode& item,
                                const std::vector<uint8_t>& data, std::string* error) {
    (void)node_id;
    (void)item;
    (void)data;
    if (error) *error = "Node storage is unavailable: no node storage transport is configured";
    return false;
}

bool NodeService::retrieve_from_node(const std::string& node_id, const std::string& path,
                                     std::vector<uint8_t>* data, std::string* error) {
    (void)node_id;
    (void)path;
    (void)data;
    if (error) *error = "Node storage is unavailable: no node storage transport is configured";
    return false;
}

bool NodeService::delete_from_node(const std::string& node_id, const std::string& path, std::string* error) {
    (void)node_id;
    (void)path;
    if (error) *error = "Node storage is unavailable: no node storage transport is configured";
    return false;
}

bool NodeService::replicate_to_cluster(const std::string& cluster_id, const StorageNode& item, 
                                       const std::vector<uint8_t>& data, int replication_factor, 
                                       std::string* error) {
    (void)cluster_id;
    (void)item;
    (void)data;
    (void)replication_factor;
    if (error) *error = "Cluster replication is unavailable: no node replication transport is configured";
    return false;
}

std::vector<StorageNode> NodeService::find_replicas(const std::string& item_id, std::string* error) {
    (void)item_id;
    if (error) *error = "Replica discovery is unavailable: no node replication transport is configured";
    return {};
}

std::string NodeService::get_best_node(StorageType type, uint64_t size_bytes, 
                                       const std::vector<std::string>& preferred_regions, 
                                       std::string* error) {
    // Select the node with the most available storage.
    auto nodes = list_nodes("", {}, error);
    if (nodes.empty()) return "";
    
    // Find node with most available space
    NodeInfo* best = &nodes[0];
    for (auto& node : nodes) {
        if (node.storage_available > best->storage_available) {
            best = &node;
        }
    }
    
    return best->id;
}

NodeService::NodeMetrics NodeService::get_node_metrics(const std::string& node_id, std::string* error) {
    (void)node_id;
    if (error) *error = "Node metrics are unavailable: no node metrics transport is configured";
    return NodeMetrics();
}

std::vector<NodeService::NodeMetrics> NodeService::get_cluster_metrics(const std::string& cluster_id, std::string* error) {
    (void)cluster_id;
    if (error) *error = "Cluster metrics are unavailable: no node metrics transport is configured";
    return {};
}

void NodeService::on_node_registered(const std::function<void(const NodeInfo&)>& callback) {
    node_registered_callbacks_.push_back(callback);
}

void NodeService::on_node_deregistered(const std::function<void(const std::string&)>& callback) {
    node_deregistered_callbacks_.push_back(callback);
}

void NodeService::on_node_status_changed(const std::function<void(const NodeInfo&)>& callback) {
    node_status_changed_callbacks_.push_back(callback);
}

// ---------------------------------------------------------------------------
// CDNService Implementation
// ---------------------------------------------------------------------------

CDNService::CDNService(const CDNConfig& config) : config_(config) {}

CDNService::~CDNService() {}

CDNUploadResult CDNService::upload(const CDNUploadRequest& request) {
    CDNUploadResult result;
    result.error = "CDN delivery is not configured in the local beta.";
    (void)request;
    return result;
}

bool CDNService::invalidate(const std::string& path, std::string* error) {
    if (error) *error = "CDN delivery is not configured in the local beta.";
    (void)path;
    return false;
}

bool CDNService::purge_cache(const std::vector<std::string>& paths, std::string* error) {
    if (error) *error = "CDN delivery is not configured in the local beta.";
    (void)paths;
    return false;
}

std::string CDNService::get_url(const std::string& path) const {
    return config_.base_url + "/" + path;
}

// ---------------------------------------------------------------------------
// ServiceManager Implementation
// ---------------------------------------------------------------------------

ServiceManager::ServiceManager() {}

ServiceManager::~ServiceManager() {
    shutdown();
}

ServiceManager& ServiceManager::instance() {
    static ServiceManager instance;
    return instance;
}

void ServiceManager::initialize(const std::map<ServiceType, ServiceConfig>& configs) {
    if (initialized_) return;
    
    for (const auto& [type, config] : configs) {
        switch (type) {
            case ServiceType::Authentication:
                auth_service_ = std::make_unique<AuthService>(config);
                break;
            case ServiceType::Storage:
                storage_service_ = std::make_unique<StorageService>(config);
                break;
            case ServiceType::ServerManagement:
                server_manager_ = std::make_unique<ServerManager>(config);
                break;
            case ServiceType::Analytics:
            case ServiceType::Social:
            case ServiceType::ContentDelivery:
                // These optional service adapters are intentionally not
                // initialized until a real endpoint is configured. They must
                // not appear as available features in the launcher UI.
                break;
        }
    }
    
    initialized_ = true;
}

void ServiceManager::shutdown() {
    auth_service_.reset();
    storage_service_.reset();
    server_manager_.reset();
    node_service_.reset();
    cdn_service_.reset();
    initialized_ = false;
}

void ServiceManager::configure_auth(const ServiceConfig& config) {
    auth_service_ = std::make_unique<AuthService>(config);
}

void ServiceManager::configure_storage(const ServiceConfig& config) {
    storage_service_ = std::make_unique<StorageService>(config);
}

void ServiceManager::configure_servers(const ServiceConfig& config) {
    server_manager_ = std::make_unique<ServerManager>(config);
}

ServerManager* local_server_manager() {
    auto& services = ServiceManager::instance();
    if (!services.servers()) {
        // The local supervisor talks to a process it starts itself, so it needs
        // no endpoint or credentials. This is unconditional because the
        // accessor's contract is that a supervisor exists: callers act on the
        // result, and a null one used to turn their failures into no-ops.
        services.configure_servers(ServiceConfig{});
    }
    return services.servers();
}

void ServiceManager::configure_nodes(const ServiceConfig& config) {
    node_service_ = std::make_unique<NodeService>(config);
}

void ServiceManager::configure_cdn(const CDNConfig& config) {
    cdn_service_ = std::make_unique<CDNService>(config);
}

std::map<ServiceType, bool> ServiceManager::service_status() const {
    std::map<ServiceType, bool> status;
    status[ServiceType::Authentication] = auth_service_ != nullptr;
    status[ServiceType::Storage] = storage_service_ != nullptr;
    status[ServiceType::ServerManagement] = server_manager_ != nullptr;
    status[ServiceType::Analytics] = false;
    status[ServiceType::Social] = false;
    status[ServiceType::ContentDelivery] = cdn_service_ != nullptr;
    return status;
}

}  // namespace aml::services

