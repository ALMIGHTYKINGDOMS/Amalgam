#include "supabase.h"

#include "net.h"

#include <windows.h>
#include <objbase.h>
#include <wincrypt.h>
#include <sstream>
#include <iomanip>
#include <random>
#include <chrono>
#include <numeric>
#include <set>
#include <stdexcept>

namespace aml::supabase {

namespace {

// Every account call needs the project URL and publishable key from
// launcher.json.  Without them there is no client and no request is made, so
// returning a default AuthResponse (empty error) leaves the UI reporting an
// empty string as if the credentials were wrong.  This wording is what
// humanize_error() renders as "account services are not configured".
const char kNotConfigured[] = "Supabase client not initialized: no project configured";

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

std::string base64_encode(const std::vector<uint8_t>& data) {
    DWORD len = static_cast<DWORD>(data.size());
    DWORD out_len = 0;
    if (!CryptBinaryToStringA(data.data(), len,
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &out_len)) {
        return "";
    }
    std::string result(out_len, '\0');
    if (!CryptBinaryToStringA(data.data(), len,
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                              &result[0], &out_len)) {
        return "";
    }
    return result;
}

std::string url_encode(const std::string& value) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;
    for (char c : value) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        } else {
            escaped << '%' << std::setw(2) << int(static_cast<unsigned char>(c));
        }
    }
    return escaped.str();
}

Json::Value parse_json(const std::string& json, std::string* error = nullptr) {
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errs;
    std::istringstream iss(json);
    if (!Json::parseFromStream(builder, iss, &root, &errs)) {
        if (error) *error = "JSON parse error: " + errs;
        return Json::Value();
    }
    return root;
}

std::string json_to_string(const Json::Value& value) {
    Json::StreamWriterBuilder builder;
    return Json::writeString(builder, value);
}

std::string storage_metadata_value(const Json& value) {
    if (value.is(Json::Type::Str)) return value.asString();
    if (value.is(Json::Type::Bool)) return value.asBool() ? "true" : "false";
    return value.dump();
}

bool append_db_rows(const Json::Value& parsed, std::vector<Json>& data,
                    std::string* error) {
    const Json::Value* rows = nullptr;
    if (parsed.isArray()) {
        rows = &parsed;
    } else if (parsed.isObject() && parsed.isMember("data") &&
               parsed["data"].isArray()) {
        rows = &parsed["data"];
    }

    if (!rows) {
        if (error && error->empty()) {
            *error = "Invalid PostgREST response: expected an array of rows";
        }
        return false;
    }

    for (const auto& item : *rows) data.push_back(item);
    return true;
}

[[noreturn]] void throw_read_error(const char* resource, const std::string& error) {
    throw std::runtime_error(error.empty()
        ? "Failed to read " + std::string(resource) + " from Supabase"
        : error);
}

void fill_auth_user(SupabaseClient::AuthUser& user, const Json::Value& value) {
    user.id = value["id"].asString();
    user.email = value["email"].asString();
    user.phone = value["phone"].asString();
    const Json::Value& metadata = value["user_metadata"];
    if (metadata.isObject()) {
        for (const auto& key : metadata.getMemberNames()) {
            user.user_metadata[key] = metadata[key].asString();
        }
    }
}

int64_t auth_expiry(const Json::Value& value) {
    const int64_t expires_at = value["expires_at"].asInt64();
    if (expires_at > 0) return expires_at;
    const int64_t expires_in = value["expires_in"].asInt64();
    return expires_in > 0
        ? std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()) + expires_in
        : 0;
}

// HTTP request helper
std::string make_http_request(const std::string& method, const std::string& url,
                              const std::string& body = "",
                              const std::map<std::string, std::string>& headers = {},
                               std::string* error = nullptr, int timeout_ms = 30000) {
    // The shared network layer owns the process-wide WinINet timeout policy.
    // Keep this argument for the existing call contract while making that
    // ownership explicit to warning-clean builds.
    (void)timeout_ms;
    std::wstring wurl = net::to_wide(url);
    std::vector<std::wstring> wheaders;
    for (const auto& h : headers) {
        wheaders.push_back(net::to_wide(h.first + ": " + h.second));
    }
    std::vector<uint8_t> body_bytes(body.begin(), body.end());
    std::vector<uint8_t> response_bytes;
    std::string err;
    if (!net::request(wurl, net::to_wide(method), wheaders, body_bytes,
                      response_bytes, &err)) {
        if (error) *error = err;
        return "";
    }
    return std::string(response_bytes.begin(), response_bytes.end());
}

}  // namespace

// ---------------------------------------------------------------------------
// SupabaseClient Implementation
// ---------------------------------------------------------------------------

SupabaseClient::SupabaseClient(const SupabaseConfig& config) : config_(config) {
    // Use the supplied public client endpoint/key configuration.
    if (config_.project_url.empty()) {
        config_.project_url.clear();
    }
}

SupabaseClient::~SupabaseClient() {
    unsubscribe_all();
}

bool SupabaseClient::initialize(const std::string& project_ref) {
    if (!project_ref.empty()) {
        config_.project_url = "https://" + project_ref + ".supabase.co";
    }
    return !config_.project_url.empty() && !config_.anon_key.empty();
}

std::string SupabaseClient::get_auth_header() const {
    return "Bearer " + config_.anon_key;
}

std::string SupabaseClient::get_bearer_header() const {
    std::lock_guard<std::mutex> lock(auth_mutex_);
    if (current_access_token_.empty()) {
        return get_auth_header();
    }
    return "Bearer " + current_access_token_;
}

std::string SupabaseClient::url_encode(const std::string& value) const {
    return aml::supabase::url_encode(value);
}

Json::Value SupabaseClient::parse_response(const std::string& response, std::string* error) const {
    return parse_json(response, error);
}

std::string SupabaseClient::make_auth_request(const std::string& endpoint,
                                               const Json::Value& body,
                                               std::string* error,
                                               const std::string& method) const {
    std::string url = config_.project_url + "/auth/v1/" + endpoint;
    
    std::map<std::string, std::string> headers;
    headers["apikey"] = config_.anon_key;
    headers["Authorization"] = get_bearer_header();
    headers["Content-Type"] = "application/json";
    headers["Prefer"] = "return=representation";
    
    std::string body_str = method == "GET" ? std::string() : json_to_string(body);
    return make_http_request(method, url, body_str, headers, error, config_.timeout_seconds * 1000);
}

std::string SupabaseClient::make_storage_request(const std::string& method,
                                                   const std::string& endpoint,
                                                   const std::vector<uint8_t>& data,
                                                   std::string* error) const {
    std::string url = config_.project_url + "/storage/v1/" + endpoint;
    
    std::map<std::string, std::string> headers;
    headers["apikey"] = config_.anon_key;
    headers["Authorization"] = get_bearer_header();
    headers["Content-Type"] = "application/octet-stream";
    headers["Prefer"] = "return=representation";
    
    std::string body_str;
    if (!data.empty()) {
        body_str = std::string(data.begin(), data.end());
    }
    
    return make_http_request(method, url, body_str, headers, error, config_.timeout_seconds * 1000);
}

std::string SupabaseClient::make_rest_request(const std::string& method,
                                              const std::string& endpoint,
                                              const Json::Value& body,
                                              std::string* error) const {
    std::string url = config_.project_url + "/rest/v1/" + endpoint;
    
    std::map<std::string, std::string> headers;
    headers["apikey"] = config_.anon_key;
    headers["Authorization"] = get_bearer_header();
    headers["Content-Type"] = "application/json";
    headers["Prefer"] = "return=representation";
    
    std::string body_str = json_to_string(body);
    return make_http_request(method, url, body_str, headers, error, config_.timeout_seconds * 1000);
}

// ---------------------------------------------------------------------------
// Authentication
// ---------------------------------------------------------------------------

SupabaseClient::AuthResponse SupabaseClient::sign_up(
    const std::string& email, const std::string& password,
    const std::map<std::string, std::string>& metadata) {
    AuthResponse response;
    
    Json::Value body;
    body["email"] = email;
    body["password"] = password;
    
    if (!metadata.empty()) {
        Json::Value meta;
        for (const auto& [key, value] : metadata) {
            meta[key] = value;
        }
        body["options"]["data"] = meta;
    }
    
    std::string result = make_auth_request("signup", body, &response.error);
    if (result.empty()) {
        response.success = false;
        return response;
    }
    
    Json::Value parsed = parse_response(result, &response.error);
    if (parsed.isNull()) {
        response.success = false;
        return response;
    }
    
    response.success = true;
    fill_auth_user(response.user, parsed["user"]);
    response.user.access_token = parsed["access_token"].asString();
    response.user.refresh_token = parsed["refresh_token"].asString();
    response.user.expires_at = auth_expiry(parsed);
    response.access_token = response.user.access_token;
    response.refresh_token = response.user.refresh_token;
    response.expires_at = response.user.expires_at;
    
    commit_session(response.user, !response.user.access_token.empty());
    
    return response;
}

SupabaseClient::AuthResponse SupabaseClient::sign_in(
    const std::string& email, const std::string& password) {
    AuthResponse response;
    
    Json::Value body;
    body["email"] = email;
    body["password"] = password;
    body["grant_type"] = "password";
    
    const std::string result = make_auth_request("token", body, &response.error);
    if (result.empty()) {
        response.success = false;
        return response;
    }
    
    Json::Value parsed = parse_response(result, &response.error);
    if (parsed.isNull()) {
        response.success = false;
        return response;
    }
    
    response.success = true;
    fill_auth_user(response.user, parsed["user"]);
    response.user.access_token = parsed["access_token"].asString();
    response.user.refresh_token = parsed["refresh_token"].asString();
    response.user.expires_at = auth_expiry(parsed);
    response.access_token = response.user.access_token;
    response.refresh_token = response.user.refresh_token;
    response.expires_at = response.user.expires_at;
    
    commit_session(response.user, !response.user.access_token.empty());
    
    return response;
}

SupabaseClient::AuthResponse SupabaseClient::sign_in_with_otp(const std::string& email) {
    AuthResponse response;
    
    Json::Value body;
    body["email"] = email;
    body["options"]["should_create_user"] = false;
    
    std::string result = make_auth_request("otp", body, &response.error);
    if (result.empty()) {
        response.success = false;
        return response;
    }
    
    Json::Value parsed = parse_response(result, &response.error);
    if (parsed.isNull()) {
        response.success = false;
        return response;
    }
    
    // OTP sent successfully
    response.success = true;
    return response;
}

SupabaseClient::AuthResponse SupabaseClient::resend_signup_confirmation(
    const std::string& email) {
    AuthResponse response;
    Json::Value body;
    body["email"] = email;
    body["type"] = "signup";

    std::string result = make_auth_request("resend", body, &response.error);
    if (result.empty()) {
        response.success = response.error.empty();
        if (!response.success) response.error = "Failed to resend verification email";
        return response;
    }
    response.success = true;
    return response;
}

SupabaseClient::AuthResponse SupabaseClient::verify_otp(
    const std::string& email, const std::string& token, const std::string& type) {
    AuthResponse response;
    
    Json::Value body;
    body["email"] = email;
    body["token"] = token;
    body["type"] = type;
    
    std::string result = make_auth_request("verify", body, &response.error);
    if (result.empty()) {
        response.success = false;
        return response;
    }
    
    Json::Value parsed = parse_response(result, &response.error);
    if (parsed.isNull()) {
        response.success = false;
        return response;
    }
    
    response.success = true;
    fill_auth_user(response.user, parsed["user"]);
    response.user.access_token = parsed["access_token"].asString();
    response.user.refresh_token = parsed["refresh_token"].asString();
    response.user.expires_at = auth_expiry(parsed);
    response.access_token = response.user.access_token;
    response.refresh_token = response.user.refresh_token;
    response.expires_at = response.user.expires_at;
    
    commit_session(response.user, !response.user.access_token.empty());
    
    return response;
}

SupabaseClient::AuthResponse SupabaseClient::refresh_token(const std::string& refresh_token) {
    AuthResponse response;
    
    Json::Value body;
    body["refresh_token"] = refresh_token;
    
    std::map<std::string, std::string> headers;
    headers["apikey"] = config_.anon_key;
    headers["Authorization"] = get_auth_header();
    headers["Content-Type"] = "application/json";
    const std::string result = make_http_request(
        "POST", config_.project_url + "/auth/v1/token?grant_type=refresh_token",
        json_to_string(body), headers, &response.error, config_.timeout_seconds * 1000);
    if (result.empty()) {
        response.success = false;
        return response;
    }
    
    Json::Value parsed = parse_response(result, &response.error);
    if (parsed.isNull()) {
        response.success = false;
        return response;
    }
    
    response.success = true;
    response.access_token = parsed["access_token"].asString();
    response.refresh_token = parsed["refresh_token"].asString();
    response.expires_at = auth_expiry(parsed);
    response.expires_in = parsed["expires_in"].asInt64();
    fill_auth_user(response.user, parsed["user"]);
    response.user.access_token = response.access_token;
    response.user.refresh_token = response.refresh_token;
    response.user.expires_at = response.expires_at;
    
    commit_session(response.user, false);
    
    return response;
}

bool SupabaseClient::sign_out(const std::string& access_token, SignOutScope scope) {
    // The client keeps the active token internally; retain the parameter for
    // API compatibility with callers that pass the token they are retiring.
    (void)access_token;
    std::string error;
    const char* endpoint = scope == SignOutScope::Global
        ? "logout?scope=global"
        : "logout?scope=local";
    make_auth_request(endpoint, Json::Value(), &error);
    // Logout is normally a 204 response, so a successful request has no body.
    // The network helper records transport and non-2xx failures in error.
    const bool remote_success = error.empty();
    
    // Always clear local credentials, even when the remote request fails.
    clear_session();
    
    return remote_success;
}

SupabaseClient::AuthUser SupabaseClient::get_user(const std::string& access_token) {
    std::string error;
    std::string result = make_auth_request("user", Json::Value(), &error, "GET");
    
    if (result.empty()) {
        return AuthUser();
    }
    
    Json::Value parsed = parse_response(result, &error);
    if (parsed.isNull()) {
        return AuthUser();
    }
    
    AuthUser user;
    fill_auth_user(user, parsed);
    user.access_token = access_token;
    
    return user;
}

SupabaseClient::AuthUser SupabaseClient::update_user(
    const std::string& access_token, 
    const std::map<std::string, std::string>& updates) {
    Json::Value body;
    for (const auto& [key, value] : updates) {
        body[key] = value;
    }
    
    std::string error;
    std::string result = make_auth_request("user", body, &error, "PUT");
    
    if (result.empty()) {
        return AuthUser();
    }
    
    Json::Value parsed = parse_response(result, &error);
    if (parsed.isNull()) {
        return AuthUser();
    }
    
    AuthUser user;
    user.id = parsed["id"].asString();
    user.email = parsed["email"].asString();
    user.phone = parsed["phone"].asString();
    user.access_token = access_token;
    
    // Update current user
    {
        std::lock_guard<std::mutex> lock(auth_mutex_);
        for (const auto& [key, value] : updates) {
            if (key == "email") current_user_.email = value;
            if (key == "password") {/* password updated */}
        }
    }
    
    return user;
}

SupabaseClient::AuthResponse SupabaseClient::change_password(
    const std::string& current_password, const std::string& new_password) {
    (void)current_password;
    AuthResponse response;
    Json::Value body;
    body["new_password"] = new_password;

    std::map<std::string, std::string> headers;
    headers["apikey"] = config_.anon_key;
    headers["Authorization"] = get_bearer_header();
    headers["Content-Type"] = "application/json";
    std::string error;
    const std::string result = make_http_request(
        "PUT", config_.project_url + "/auth/v1/user", json_to_string(body),
        headers, &error, config_.timeout_seconds * 1000);
    
    if (result.empty()) {
        response.error = error.empty() ? "Unknown error" : error;
        return response;
    }
    
    Json::Value parsed = parse_response(result, &error);
    if (parsed.isNull()) {
        response.error = error.empty() ? "Invalid response" : error;
        return response;
    }
    response.success = true;
    fill_auth_user(response.user, parsed);
    {
        std::lock_guard<std::mutex> lock(auth_mutex_);
        response.user.access_token = current_access_token_;
        response.user.refresh_token = current_refresh_token_;
        response.user.expires_at = current_user_.expires_at;
        current_user_ = response.user;
    }
    return response;
}

SupabaseClient::AuthResponse SupabaseClient::request_password_reset(const std::string& email) {
    Json::Value body;
    body["email"] = email;
    
    std::string error;
    std::string result = make_auth_request("recover", body, &error);
    
    if (result.empty()) {
        return AuthResponse{error.empty(), error.empty() ? "" : error};
    }
    
    return AuthResponse{true, "Password reset email sent"};
}

SupabaseClient::AuthResponse SupabaseClient::enable_2fa(const std::string& code) {
    Json::Value body;
    body["code"] = code;
    body["type"] = "totp";
    
    std::string error;
    std::string result = make_auth_request("user/2fa/enable", body, &error);
    
    if (result.empty()) {
        return AuthResponse{false, error.empty() ? "Unknown error" : error};
    }
    
    return AuthResponse{true, "2FA enabled successfully"};
}

SupabaseClient::AuthResponse SupabaseClient::disable_2fa(const std::string& code) {
    Json::Value body;
    body["code"] = code;
    
    std::string error;
    std::string result = make_auth_request("user/2fa/disable", body, &error);
    
    if (result.empty()) {
        return AuthResponse{false, error.empty() ? "Unknown error" : error};
    }
    
    return AuthResponse{true, "2FA disabled successfully"};
}

SupabaseClient::AuthResponse SupabaseClient::verify_2fa_code(const std::string& code) {
    Json::Value body;
    body["code"] = code;
    body["type"] = "totp";
    
    std::string error;
    std::string result = make_auth_request("user/2fa/verify", body, &error);
    
    if (result.empty()) {
        return AuthResponse{false, error.empty() ? "Unknown error" : error};
    }
    
    Json::Value parsed = parse_response(result, &error);
    if (parsed.isNull()) {
        return AuthResponse{false, error.empty() ? "Invalid response" : error};
    }
    
    return AuthResponse{true, "2FA verified", parsed["access_token"].asString(), 
                     parsed["refresh_token"].asString(), 
                     parsed["expires_in"].asInt64()};
}

SupabaseClient::AuthResponse SupabaseClient::request_account_deletion() {
    std::string error;
    std::string result = make_auth_request("user/delete", Json::Value(), &error);
    
    if (result.empty()) {
        return AuthResponse{false, error.empty() ? "Unknown error" : error};
    }
    
    return AuthResponse{true, "Account deletion requested"};
}

SupabaseClient::AuthResponse SupabaseClient::confirm_account_deletion(const std::string& confirmation_code) {
    Json::Value body;
    body["code"] = confirmation_code;
    
    std::string error;
    std::string result = make_auth_request("user/delete/confirm", body, &error);
    
    if (result.empty()) {
        return AuthResponse{false, error.empty() ? "Unknown error" : error};
    }
    
    return AuthResponse{true, "Account deletion confirmed"};
}

SupabaseSecuritySettings SupabaseClient::get_security_settings() const {
    return get_security_settings(nullptr);
}

SupabaseSecuritySettings SupabaseClient::get_security_settings(std::string* error_out) const {
    std::string error;
    std::string result = make_auth_request("user/security", Json::Value(), &error, "GET");

    SupabaseSecuritySettings settings;

    if (result.empty()) {
        if (error_out) {
            *error_out = error.empty()
                ? "The account service returned no security details."
                : error;
        }
        return settings;
    }

    Json::Value parsed = parse_response(result, &error);
    if (parsed.isNull()) {
        if (error_out) {
            *error_out = error.empty()
                ? "The account service returned unreadable security details."
                : error;
        }
        return settings;
    }
    
    settings.two_factor_enabled = parsed["two_factor_enabled"].asBool();
    settings.two_factor_method = parsed["two_factor_method"].asString();
    
    // Parse trusted devices
    if (parsed["trusted_devices"].isArray()) {
        for (const auto& device : parsed["trusted_devices"]) {
            settings.trusted_devices.push_back(device.asString());
        }
    }
    
    // Parse recent logins
    if (parsed["recent_logins"].isArray()) {
        for (const auto& login : parsed["recent_logins"]) {
            settings.recent_logins.push_back(login.asString());
        }
    }
    
    settings.last_password_change = parsed["last_password_change"].asInt64();
    
    if (error_out) error_out->clear();
    return settings;
}

std::string SupabaseClient::get_oauth_url(const std::string& provider, 
                                          const std::string& redirect_to) {
    std::string state = generate_uuid();
    
    std::string url = config_.project_url + "/auth/v1/authorize?";
    url += "provider=" + provider;
    url += "&redirect_to=" + url_encode(redirect_to);
    url += "&skip_browser_redirect=true";
    
    return url;
}

SupabaseClient::AuthResponse SupabaseClient::exchange_oauth_code(
    const std::string& provider, const std::string& code,
    const std::string& redirect_to) {
    AuthResponse response;
    
    Json::Value body;
    body["provider"] = provider;
    body["code"] = code;
    body["redirect_to"] = redirect_to;
    
    std::string result = make_auth_request("callback", body, &response.error);
    if (result.empty()) {
        response.success = false;
        return response;
    }
    
    Json::Value parsed = parse_response(result, &response.error);
    if (parsed.isNull()) {
        response.success = false;
        return response;
    }
    
    response.success = true;
    response.user.id = parsed["user"]["id"].asString();
    response.user.email = parsed["user"]["email"].asString();
    response.access_token = parsed["access_token"].asString();
    response.refresh_token = parsed["refresh_token"].asString();
    response.expires_at = parsed["expires_at"].asInt64();
    
    response.user.access_token = response.access_token;
    response.user.refresh_token = response.refresh_token;
    response.user.expires_at = response.expires_at;
    commit_session(response.user, !response.user.access_token.empty());
    
    return response;
}

// ---------------------------------------------------------------------------
// Storage
// ---------------------------------------------------------------------------

SupabaseClient::StorageUploadResult SupabaseClient::upload_file(
    const StorageUploadOptions& options) {
    StorageUploadResult result;
    
    std::string url = "object/" + options.bucket + "/" + url_encode(options.path);
    
    std::map<std::string, std::string> headers;
    headers["apikey"] = config_.anon_key;
    headers["Authorization"] = get_bearer_header();
    headers["Content-Type"] = options.content_type.empty() ? "application/octet-stream" : options.content_type;
    headers["Prefer"] = "return=representation";
    
    // Add metadata if present
    if (!options.metadata.empty()) {
        Json::Value meta;
        for (const auto& [key, value] : options.metadata) {
            meta[key] = value;
        }
        headers["X-Upload-Metadata"] = json_to_string(meta);
    }
    
    std::string response = make_storage_request(
        options.upsert ? "POST" : "POST", url, options.data, &result.error);
    
    if (response.empty()) {
        result.success = false;
        return result;
    }
    
    Json::Value parsed = parse_response(response, &result.error);
    if (parsed.isNull()) {
        result.success = false;
        return result;
    }
    
    result.success = true;
    result.path = parsed["path"].asString();
    result.full_path = parsed["full_path"].asString();
    result.id = parsed["id"].asString();
    
    return result;
}

SupabaseClient::StorageDownloadResult SupabaseClient::download_file(
    const std::string& bucket, const std::string& path) {
    StorageDownloadResult result;
    
    std::string url = "object/" + bucket + "/" + url_encode(path);
    
    std::map<std::string, std::string> headers;
    headers["apikey"] = config_.anon_key;
    headers["Authorization"] = get_bearer_header();
    headers["Prefer"] = "return=bytes";
    
    std::string response = make_storage_request("GET", url, {}, &result.error);
    if (response.empty()) {
        result.success = false;
        return result;
    }
    
    // The response is the raw file data
    result.data = std::vector<uint8_t>(response.begin(), response.end());
    result.success = true;
    
    return result;
}

SupabaseClient::StorageListResult SupabaseClient::list_files(
    const StorageListOptions& options) {
    StorageListResult result;
    
    std::string url = "object/list/" + options.bucket;
    if (!options.prefix.empty()) {
        url += "?prefix=" + url_encode(options.prefix);
    }
    if (options.limit > 0) {
        url += std::string(url.find('?') != std::string::npos ? "&" : "?") +
               "limit=" + std::to_string(options.limit);
    }
    if (options.offset > 0) {
        url += "&offset=" + std::to_string(options.offset);
    }
    if (!options.sort_by.empty()) {
        url += "&sort_by=" + url_encode(options.sort_by);
        url += std::string("&sort_order=") + (options.sort_order_asc ? "asc" : "desc");
    }
    
    std::map<std::string, std::string> headers;
    headers["apikey"] = config_.anon_key;
    headers["Authorization"] = get_bearer_header();
    
    std::string response = make_storage_request("GET", url, {}, &result.error);
    if (response.empty()) {
        result.success = false;
        return result;
    }
    
    Json::Value parsed = parse_response(response, &result.error);
    if (parsed.isNull()) {
        result.success = false;
        return result;
    }
    
    result.success = true;
    for (const auto& item : parsed) {
        StorageItem storage_item;
        storage_item.name = item["name"].asString();
        storage_item.id = item["id"].asString();
        storage_item.updated_at = item["updated_at"].asInt64();
        storage_item.created_at = item["created_at"].asInt64();
        storage_item.path = item["path"].asString();
        if (storage_item.path.empty()) storage_item.path = storage_item.name;

        // The list endpoint returns names relative to the requested prefix.
        // Avoid adding the prefix twice when a backend returns a full name.
        if (!options.prefix.empty()) {
            std::string prefix = options.prefix;
            if (prefix.back() != '/') prefix += '/';
            if (storage_item.path.compare(0, prefix.size(), prefix) != 0) {
                storage_item.path = prefix + storage_item.path;
            }
        }

        // Keep compatibility with responses that expose these fields at the
        // top level, but only read actual values.
        if (item.isMember("size") && item["size"].is(Json::Type::Num)) {
            storage_item.size = item["size"].asUInt64();
        }
        if (item.isMember("mime_type") && item["mime_type"].is(Json::Type::Str)) {
            storage_item.mime_type = item["mime_type"].asString();
        }
        
        // Supabase stores the object size and MIME type inside metadata.
        const Json& metadata = item["metadata"];
        if (metadata.isObject()) {
            for (const auto& meta : metadata.getMemberNames()) {
                storage_item.metadata[meta] = storage_metadata_value(metadata[meta]);
            }
            if (metadata.isMember("size") && metadata["size"].is(Json::Type::Num)) {
                storage_item.size = metadata["size"].asUInt64();
            }
            if (metadata.isMember("mimetype") && metadata["mimetype"].is(Json::Type::Str)) {
                storage_item.mime_type = metadata["mimetype"].asString();
            }
        }
        
        result.items.push_back(storage_item);
    }
    
    return result;
}

bool SupabaseClient::delete_file(const std::string& bucket, const std::string& path) {
    std::string error;
    std::string url = "object/" + bucket + "/" + url_encode(path);
    
    std::map<std::string, std::string> headers;
    headers["apikey"] = config_.anon_key;
    headers["Authorization"] = get_bearer_header();
    
    std::string response = make_storage_request("DELETE", url, {}, &error);
    return !response.empty();
}

bool SupabaseClient::delete_files(const std::string& bucket, const std::vector<std::string>& paths) {
    for (const auto& path : paths) {
        if (!delete_file(bucket, path)) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Database (PostgREST)
// ---------------------------------------------------------------------------

SupabaseClient::DBResult SupabaseClient::select(const DBQueryOptions& options) {
    DBResult result;
    
    std::string url = options.table;
    
    // Add select
    if (options.select != "*") {
        url += "?select=" + url_encode(options.select);
    }
    
    // Add filters
    std::vector<std::string> conditions;
    for (const auto& [key, value] : options.eq_filters) {
        conditions.push_back(key + "=eq." + url_encode(value));
    }
    for (const auto& [key, value] : options.gt_filters) {
        conditions.push_back(key + "=gt." + url_encode(value));
    }
    for (const auto& [key, value] : options.lt_filters) {
        conditions.push_back(key + "=lt." + url_encode(value));
    }
    for (const auto& [key, value] : options.like_filters) {
        conditions.push_back(key + "=like.*" + url_encode(value) + "*");
    }
    
    if (!conditions.empty()) {
        url += std::string(url.find('?') != std::string::npos ? "&" : "?") + "and=(" +
               std::accumulate(conditions.begin(), conditions.end(), std::string(), 
                               [](const std::string& a, const std::string& b) {
                                   return a.empty() ? b : a + "," + b;
                               }) + ")";
    }
    
    // Add order
    if (!options.order_by.empty()) {
        url += std::string(url.find('?') != std::string::npos ? "&" : "?") +
               "order=" + url_encode(options.order_by) + "." + (options.order_asc ? "asc" : "desc");
    }
    
    // Add limit and offset
    if (options.limit > 0) {
        url += std::string(url.find('?') != std::string::npos ? "&" : "?") +
               "limit=" + std::to_string(options.limit);
    }
    if (options.offset > 0) {
        url += std::string(url.find('?') != std::string::npos ? "&" : "?") +
               "offset=" + std::to_string(options.offset);
    }
    
    std::map<std::string, std::string> headers;
    headers["apikey"] = config_.anon_key;
    headers["Authorization"] = get_bearer_header();
    headers["Prefer"] = "count=exact";
    
    std::string response = make_rest_request("GET", url, Json::Value(), &result.error);
    if (response.empty()) {
        if (result.error.empty()) result.error = "Empty PostgREST response";
        result.success = false;
        return result;
    }
    
    Json::Value parsed = parse_response(response, &result.error);
    if (!append_db_rows(parsed, result.data, &result.error)) {
        result.success = false;
        return result;
    }
    
    // Parse count if present
    if (parsed.isObject() && parsed.isMember("count")) {
        result.count = parsed["count"].asInt();
    }

    result.success = true;
    
    return result;
}

SupabaseClient::DBResult SupabaseClient::insert(const DBInsertOptions& options) {
    DBResult result;
    
    std::string url = options.table;
    
    Json::Value body = Json::arrayValue;
    for (const auto& record : options.records) {
        body.append(record);
    }
    
    std::map<std::string, std::string> headers;
    headers["apikey"] = config_.anon_key;
    headers["Authorization"] = get_bearer_header();
    headers["Prefer"] = "return=representation";
    
    if (options.upsert) {
        headers["Prefer"] = "resolution=merge-duplicates,return=representation";
        if (!options.on_conflict.empty()) {
            url += "?on_conflict=" + url_encode(options.on_conflict);
        }
    }
    
    std::string response = make_rest_request("POST", url, body, &result.error);
    if (response.empty()) {
        if (result.error.empty()) result.error = "Empty PostgREST response";
        result.success = false;
        return result;
    }
    
    Json::Value parsed = parse_response(response, &result.error);
    if (!append_db_rows(parsed, result.data, &result.error)) {
        result.success = false;
        return result;
    }
    
    result.success = true;
    
    return result;
}

SupabaseClient::DBResult SupabaseClient::update(const DBUpdateOptions& options) {
    DBResult result;
    
    std::string url = options.table;
    
    // Add filters
    std::vector<std::string> conditions;
    for (const auto& [key, value] : options.eq_filters) {
        conditions.push_back(key + "=eq." + url_encode(value));
    }
    
    if (!conditions.empty()) {
        url += "?" + std::accumulate(conditions.begin(), conditions.end(), std::string(),
                                       [](const std::string& a, const std::string& b) {
                                           return a.empty() ? b : a + "&" + b;
                                       });
    }
    
    Json::Value body = options.updates;
    
    std::map<std::string, std::string> headers;
    headers["apikey"] = config_.anon_key;
    headers["Authorization"] = get_bearer_header();
    headers["Prefer"] = "return=representation";
    
    std::string response = make_rest_request("PATCH", url, body, &result.error);
    if (response.empty()) {
        if (result.error.empty()) result.error = "Empty PostgREST response";
        result.success = false;
        return result;
    }
    
    Json::Value parsed = parse_response(response, &result.error);
    if (!append_db_rows(parsed, result.data, &result.error)) {
        result.success = false;
        return result;
    }
    
    result.success = true;
    
    return result;
}

SupabaseClient::DBResult SupabaseClient::remove(const DBDeleteOptions& options) {
    DBResult result;
    
    std::string url = options.table;
    
    // Add filters
    std::vector<std::string> conditions;
    for (const auto& [key, value] : options.eq_filters) {
        conditions.push_back(key + "=eq." + url_encode(value));
    }
    
    if (!conditions.empty()) {
        url += "?" + std::accumulate(conditions.begin(), conditions.end(), std::string(),
                                       [](const std::string& a, const std::string& b) {
                                           return a.empty() ? b : a + "&" + b;
                                       });
    }
    
    std::map<std::string, std::string> headers;
    headers["apikey"] = config_.anon_key;
    headers["Authorization"] = get_bearer_header();
    headers["Prefer"] = "return=representation";
    
    std::string response = make_rest_request("DELETE", url, Json::Value(), &result.error);
    if (response.empty()) {
        if (result.error.empty()) result.error = "Empty PostgREST response";
        result.success = false;
        return result;
    }
    
    Json::Value parsed = parse_response(response, &result.error);
    if (!append_db_rows(parsed, result.data, &result.error)) {
        result.success = false;
        return result;
    }
    
    result.success = true;
    
    return result;
}

// ---------------------------------------------------------------------------
// Realtime-compatible refresh
// ---------------------------------------------------------------------------
//
// The native launcher deliberately avoids shipping a second WebSocket stack.
// Supabase Realtime is therefore represented by a bounded HTTP refresh loop.
// It has the same subscription contract for the UI, emits only changed rows,
// and fails harmlessly when the backend is offline.

SupabaseClient::RealtimeChannel SupabaseClient::subscribe(
    const std::string& table,
    const std::string& filter,
    const std::function<void(const Json::Value&)>& callback) {
    RealtimeChannel channel;
    if (table.empty() || !callback) return channel;

    channel.id = generate_uuid();
    channel.topic = table;
    channel.callback = callback;

    auto subscription = std::make_unique<PollingSubscription>();
    subscription->stop = std::make_shared<std::atomic<bool>>(false);
    const auto stop = subscription->stop;
    const std::string channel_id = channel.id;

    subscription->worker = std::thread([this, table, filter, callback, stop, channel_id]() {
        std::map<std::string, std::string> snapshots;
        while (!stop->load()) {
            DBQueryOptions options;
            options.table = table;
            options.limit = 250;

            // Launcher subscriptions currently use the simple PostgREST form
            // key=eq.value. Reject complex filter text instead of interpolating
            // arbitrary query syntax into the polling request.
            const size_t eq = filter.find("=eq.");
            if (!filter.empty() && eq != std::string::npos && eq > 0) {
                const std::string key = filter.substr(0, eq);
                const std::string value = filter.substr(eq + 4);
                if (key.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") == std::string::npos) {
                    options.eq_filters[key] = value;
                }
            }

            const DBResult result = select(options);
            if (result.success) {
                std::set<std::string> current;
                for (const auto& row : result.data) {
                    const std::string row_id = row["id"].asString().empty()
                        ? row.toStyledString() : row["id"].asString();
                    const std::string serialized = row.toStyledString();
                    current.insert(row_id);
                    auto it = snapshots.find(row_id);
                    if (it == snapshots.end() || it->second != serialized) {
                        snapshots[row_id] = serialized;
                        callback(row);
                        for (const auto& observer : realtime_callbacks_) observer(channel_id, row);
                    }
                }
                for (auto it = snapshots.begin(); it != snapshots.end();) {
                    if (!current.count(it->first)) it = snapshots.erase(it);
                    else ++it;
                }
            }

            for (int i = 0; i < 100 && !stop->load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
    });

    {
        std::lock_guard<std::mutex> lock(mutex_);
        realtime_subscriptions_[channel.id] = std::move(subscription);
    }
    return channel;
}

bool SupabaseClient::unsubscribe(const std::string& channel_id) {
    std::unique_ptr<PollingSubscription> subscription;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = realtime_subscriptions_.find(channel_id);
        if (it == realtime_subscriptions_.end()) return false;
        subscription = std::move(it->second);
        realtime_subscriptions_.erase(it);
    }
    subscription->stop->store(true);
    if (subscription->worker.joinable()) subscription->worker.join();
    return true;
}

void SupabaseClient::unsubscribe_all() {
    std::map<std::string, std::unique_ptr<PollingSubscription>> subscriptions;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        subscriptions.swap(realtime_subscriptions_);
    }
    for (auto& [id, subscription] : subscriptions) {
        (void)id;
        subscription->stop->store(true);
        if (subscription->worker.joinable()) subscription->worker.join();
    }
}

bool SupabaseClient::send(const std::string& channel_id, const Json::Value& data) {
    (void)channel_id;
    (void)data;
    // Broadcast is not part of the HTTP polling contract. Callers must use a
    // persisted RPC/table mutation for state changes.
    return false;
}

// ---------------------------------------------------------------------------
// Edge Functions
// ---------------------------------------------------------------------------

SupabaseClient::EdgeFunctionResult SupabaseClient::call_edge_function(
    const std::string& function_name, const Json::Value& body) {
    EdgeFunctionResult result;

    // Essentials uses free PostgREST RPCs for its control plane. Minecraft
    // traffic never enters this path. Keep the Edge Function path for other
    // integrations until their functions are deployed.
    static const std::set<std::string> control_plane_rpcs = {
        "create_session", "start_session", "stop_session", "update_session",
        "join_session", "leave_session", "upsert_session_manifest",
        "get_session_manifest", "create_session_manifest"
    };
    if (control_plane_rpcs.find(function_name) != control_plane_rpcs.end()) {
        const auto rpc_result = rpc(function_name == "create_session_manifest"
                                        ? "upsert_session_manifest" : function_name, body);
        result.error = rpc_result.error;
        result.success = rpc_result.success;
        result.data = Json::arr();
        for (const auto& item : rpc_result.data) result.data.push(item);
        return result;
    }
    
    if (config_.project_url.empty() || config_.anon_key.empty()) {
        result.error = "Supabase is not configured";
        return result;
    }
    std::string url = config_.project_url + "/functions/v1/" + function_name;
    
    std::map<std::string, std::string> headers;
    headers["apikey"] = config_.anon_key;
    headers["Authorization"] = get_bearer_header();
    headers["Content-Type"] = "application/json";
    
    std::string body_str = json_to_string(body);
    std::string response = make_http_request("POST", url, body_str, headers, &result.error,
                                             config_.timeout_seconds * 1000);
    
    if (response.empty()) {
        result.success = false;
        return result;
    }
    
    result.data = parse_response(response, &result.error);
    result.success = !result.data.isNull();
    
    return result;
}

TurnCredentials SupabaseClient::request_turn_credentials(int ttl_seconds) {
    TurnCredentials result;
    Json::Value body;
    body["ttl"] = std::max(60, std::min(3600, ttl_seconds));
    const auto response = call_edge_function("get-turn-credentials", body);
    if (!response.success) {
        result.error = response.error.empty() ? "TURN credential request failed" : response.error;
        return result;
    }

    const Json::Value& root = response.data;
    const Json::Value& servers = root["iceServers"].isArray()
        ? root["iceServers"] : root["ice_servers"];
    for (const auto& item : servers) {
        TurnServer server;
        const Json::Value& urls = item["urls"];
        if (urls.is(Json::Type::Arr)) {
            for (const auto& url : urls) server.urls.push_back(url.asString());
        } else if (urls.is(Json::Type::Str)) {
            server.urls.push_back(urls.asString());
        }
        server.username = item["username"].asString();
        server.credential = item["credential"].asString();
        if (!server.urls.empty()) result.servers.push_back(std::move(server));
    }
    result.ttl = root["ttl"].asInt();
    if (result.servers.empty()) {
        result.error = "TURN service returned no ICE servers";
        return result;
    }
    result.success = true;
    return result;
}

SupabaseClient::DBResult SupabaseClient::rpc(const std::string& function_name,
                                             const Json::Value& args) {
    DBResult result;
    std::string error;
    const std::string response = make_rest_request(
        "POST", "rpc/" + url_encode(function_name), args, &error);
    if (response.empty()) {
        result.error = error.empty() ? "RPC request failed" : error;
        return result;
    }
    Json parsed = parse_response(response, &result.error);
    if (!result.error.empty()) return result;
    if (parsed.isArray()) {
        for (const auto& item : parsed.items()) result.data.push_back(item);
    } else if (!parsed.isNull()) {
        result.data.push_back(parsed);
    }
    result.count = static_cast<int>(result.data.size());
    result.success = true;
    return result;
}

bool SupabaseClient::is_current_user_staff() {
    if (!is_authenticated()) return false;
    const DBResult result = rpc("is_project_staff");
    return result.success && !result.data.empty() && result.data.front().as_bool();
}

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

bool SupabaseClient::is_authenticated() const {
    std::lock_guard<std::mutex> lock(auth_mutex_);
    return authenticated_.load(std::memory_order_acquire);
}

SupabaseClient::AuthUser SupabaseClient::current_user() const {
    std::lock_guard<std::mutex> lock(auth_mutex_);
    return current_user_;
}

std::string SupabaseClient::current_access_token() const {
    std::lock_guard<std::mutex> lock(auth_mutex_);
    return current_access_token_;
}

void SupabaseClient::set_access_token(const std::string& access_token) {
    std::lock_guard<std::mutex> lock(auth_mutex_);
    current_access_token_ = access_token;
}

void SupabaseClient::set_session(const AuthUser& user) {
    commit_session(user, false);
}

void SupabaseClient::clear_session() {
    std::vector<std::function<void(bool, const AuthUser&)>> callbacks;
    AuthUser cleared;
    {
        std::lock_guard<std::mutex> lock(auth_mutex_);
        current_access_token_.clear();
        current_refresh_token_.clear();
        current_user_ = cleared;
        authenticated_.store(false, std::memory_order_release);
        callbacks = auth_callbacks_;
    }
    for (auto& callback : callbacks) {
        callback(false, cleared);
    }
}

void SupabaseClient::on_auth_state_change(
    const std::function<void(bool, const AuthUser&)>& callback) {
    std::lock_guard<std::mutex> lock(auth_mutex_);
    auth_callbacks_.push_back(callback);
}

void SupabaseClient::commit_session(const AuthUser& user, bool notify_auth_observers) {
    std::vector<std::function<void(bool, const AuthUser&)>> callbacks;
    const bool authenticated = !user.id.empty() && !user.access_token.empty();
    {
        std::lock_guard<std::mutex> lock(auth_mutex_);
        current_user_ = user;
        current_access_token_ = user.access_token;
        current_refresh_token_ = user.refresh_token;
        authenticated_.store(authenticated, std::memory_order_release);
        if (authenticated && notify_auth_observers) callbacks = auth_callbacks_;
    }
    for (auto& callback : callbacks) {
        callback(true, user);
    }
}

void SupabaseClient::on_realtime_message(
    const std::function<void(const std::string&, const Json::Value&)>& callback) {
    realtime_callbacks_.push_back(callback);
}

// ---------------------------------------------------------------------------
// SupabaseManager Implementation
// ---------------------------------------------------------------------------

SupabaseManager::SupabaseManager() {}

SupabaseManager::~SupabaseManager() {
    shutdown();
}

SupabaseManager& SupabaseManager::instance() {
    static SupabaseManager instance;
    return instance;
}

bool SupabaseManager::initialize(const std::string& project_url,
                                 const std::string& anon_key) {
    if (initialized_) return true;
    
    SupabaseConfig config;
    if (!project_url.empty()) config.project_url = project_url;
    if (!anon_key.empty()) config.anon_key = anon_key;
    
    client_ = std::make_unique<SupabaseClient>(config);
    initialized_ = true;
    
    return true;
}

void SupabaseManager::shutdown() {
    // Any captured UI/session scope becomes invalid before the client is
    // released, preventing an in-flight worker from applying stale results at
    // teardown.
    session_generation_.fetch_add(1, std::memory_order_acq_rel);
    client_.reset();
    initialized_ = false;
}

SupabaseClient* SupabaseManager::client() {
    return client_.get();
}

SupabaseClient::AuthResponse SupabaseManager::sign_up(
    const std::string& email, const std::string& password,
    const std::map<std::string, std::string>& metadata) {
    if (!client_) return SupabaseClient::AuthResponse(false, kNotConfigured);
    auto response = client_->sign_up(email, password, metadata);
    if (response.success && !response.user.id.empty() && !response.access_token.empty()) {
        session_generation_.fetch_add(1, std::memory_order_acq_rel);
    }
    return response;
}

SupabaseClient::AuthResponse SupabaseManager::sign_in(
    const std::string& email, const std::string& password) {
    if (!client_) return SupabaseClient::AuthResponse(false, kNotConfigured);
    auto response = client_->sign_in(email, password);
    if (response.success && !response.user.id.empty() && !response.access_token.empty()) {
        session_generation_.fetch_add(1, std::memory_order_acq_rel);
    }
    return response;
}

SupabaseClient::AuthResponse SupabaseManager::resend_signup_confirmation(
    const std::string& email) {
    if (!client_) return SupabaseClient::AuthResponse(false, kNotConfigured);
    return client_->resend_signup_confirmation(email);
}

SupabaseClient::AuthResponse SupabaseManager::verify_otp(
    const std::string& email, const std::string& token, const std::string& type) {
    if (!client_) return SupabaseClient::AuthResponse(false, kNotConfigured);
    auto response = client_->verify_otp(email, token, type);
    if (response.success && !response.user.id.empty() && !response.access_token.empty()) {
        session_generation_.fetch_add(1, std::memory_order_acq_rel);
    }
    return response;
}

bool SupabaseManager::sign_out(SignOutScope scope) {
    // Invalidate captured UI scopes before the remote request. `sign_out`
    // always clears the local client credentials, including on a transport
    // failure, so there is no valid path that should retain the old epoch.
    session_generation_.fetch_add(1, std::memory_order_acq_rel);
    if (!client_) return false;
    return client_->sign_out(client_->current_access_token(), scope);
}

SupabaseUser SupabaseManager::get_current_user() {
    if (!client_ || !client_->is_authenticated()) return SupabaseUser();
    
    auto user = client_->current_user();
    SupabaseUser supa_user;
    supa_user.id = user.id;
    supa_user.email = user.email;
    supa_user.username = user.user_metadata.count("username") ? user.user_metadata.at("username") : "";
    supa_user.display_name = user.user_metadata.count("display_name") ? user.user_metadata.at("display_name") : "";
    supa_user.avatar_url = user.user_metadata.count("avatar_url") ? user.user_metadata.at("avatar_url") : "";
    supa_user.metadata = user.user_metadata;
    supa_user.created_at = user.created_at;
    supa_user.updated_at = user.updated_at;
    supa_user.last_login_at = user.last_login_at;
    return supa_user;
}

SupabaseClient::AuthResponse SupabaseManager::update_user(
    const std::map<std::string, std::string>& updates,
    const std::map<std::string, std::string>& metadata) {
    if (!client_ || !client_->is_authenticated()) {
        return {false, "Not authenticated"};
    }
    std::map<std::string, std::string> merged = updates;
    for (const auto& [key, value] : metadata) {
        merged["user_metadata." + key] = value;
    }
    const auto user = client_->update_user(client_->current_access_token(), merged);
    SupabaseClient::AuthResponse response;
    response.user = user;
    response.success = !user.id.empty();
    if (!response.success) response.error = "Could not update user";
    return response;
}

SupabaseClient::AuthResponse SupabaseManager::change_password(
    const std::string& current_password, const std::string& new_password) {
    if (!client_ || !client_->is_authenticated()) {
        return {false, "Not authenticated"};
    }
    return client_->change_password(current_password, new_password);
}

SupabaseClient::AuthResponse SupabaseManager::request_password_reset(const std::string& email) {
    if (!client_) return {false, kNotConfigured};
    return client_->request_password_reset(email);
}

SupabaseClient::AuthResponse SupabaseManager::refresh_token(const std::string& refresh_token) {
    if (!client_) return {false, kNotConfigured};
    auto response = client_->refresh_token(refresh_token);
    if (response.success && !response.user.id.empty() && !response.access_token.empty()) {
        session_generation_.fetch_add(1, std::memory_order_acq_rel);
    }
    return response;
}

SupabaseClient::AuthResponse SupabaseManager::enable_2fa(const std::string& code) {
    if (!client_ || !client_->is_authenticated()) {
        return {false, "Not authenticated"};
    }
    return client_->enable_2fa(code);
}

SupabaseClient::AuthResponse SupabaseManager::disable_2fa(const std::string& code) {
    if (!client_ || !client_->is_authenticated()) {
        return {false, "Not authenticated"};
    }
    return client_->disable_2fa(code);
}

SupabaseClient::AuthResponse SupabaseManager::verify_2fa_code(const std::string& code) {
    if (!client_ || !client_->is_authenticated()) {
        return {false, "Not authenticated"};
    }
    return client_->verify_2fa_code(code);
}

SupabaseClient::AuthResponse SupabaseManager::request_account_deletion() {
    if (!client_ || !client_->is_authenticated()) {
        return {false, "Not authenticated"};
    }
    return client_->request_account_deletion();
}

SupabaseClient::AuthResponse SupabaseManager::confirm_account_deletion(const std::string& confirmation_code) {
    if (!client_ || !client_->is_authenticated()) {
        return {false, "Not authenticated"};
    }
    return client_->confirm_account_deletion(confirmation_code);
}

SupabaseSecuritySettings SupabaseManager::get_security_settings() const {
    return get_security_settings(nullptr);
}

SupabaseSecuritySettings SupabaseManager::get_security_settings(std::string* error) const {
    if (!client_ || !client_->is_authenticated()) {
        if (error) *error = "Your Amalgam session is no longer available.";
        return SupabaseSecuritySettings();
    }
    return client_->get_security_settings(error);
}

std::vector<SupabaseServer> SupabaseManager::get_servers() {
    std::vector<SupabaseServer> servers;
    if (!client_ || !client_->is_authenticated()) return servers;
    
    SupabaseClient::DBQueryOptions options;
    options.table = "servers";
    options.eq_filters["user_id"] = client_->current_user().id;
    options.order_by = "created_at";
    options.order_asc = false;
    
    auto result = client_->select(options);
    if (!result.success) throw_read_error("servers", result.error);
    
    for (const auto& row : result.data) {
        SupabaseServer server;
        server.id = row.at("id").asString();
        server.user_id = row.at("user_id").asString();
        server.name = row.at("name").asString();
        server.alias = row.at("alias").asString();
        server.address = row.at("address").asString();
        server.host = row.at("host").asString();
        server.port = row.at("port").asInt();
        server.version = row.at("version").asString();
        server.type = row.at("type").asString();
        server.motd = row.at("motd").asString();
        server.max_players = row.at("max_players").asInt();
        server.online = row.at("online").asBool();
        server.ping_ms = row.at("ping_ms").asInt();
        server.created_at = row.at("created_at").asInt64();
        server.updated_at = row.at("updated_at").asInt64();
        
        servers.push_back(server);
    }
    
    return servers;
}

SupabaseServer SupabaseManager::create_server(const SupabaseServer& server) {
    if (!client_ || !client_->is_authenticated()) return SupabaseServer();
    
    SupabaseClient::DBInsertOptions options;
    options.table = "servers";
    
    Json::Value record;
    record["id"] = server.id.empty() ? generate_uuid() : server.id;
    record["user_id"] = client_->current_user().id;
    record["name"] = server.name;
    record["alias"] = server.alias;
    record["address"] = server.address;
    record["host"] = server.host;
    record["port"] = server.port;
    record["version"] = server.version;
    record["type"] = server.type;
    record["motd"] = server.motd;
    record["max_players"] = server.max_players;
    record["online"] = server.online;
    record["ping_ms"] = server.ping_ms;
    record["created_at"] = server.created_at;
    record["updated_at"] = server.updated_at;
    
    options.records.push_back(record);
    
    auto result = client_->insert(options);
    if (!result.success || result.data.empty()) return SupabaseServer();
    
    SupabaseServer created = server;
    created.id = result.data[0]["id"].asString();
    created.user_id = result.data[0]["user_id"].asString();
    created.created_at = result.data[0]["created_at"].asInt64();
    created.updated_at = result.data[0]["updated_at"].asInt64();
    
    return created;
}

bool SupabaseManager::update_server(const SupabaseServer& server) {
    if (!client_ || !client_->is_authenticated()) return false;
    
    SupabaseClient::DBUpdateOptions options;
    options.table = "servers";
    options.eq_filters["id"] = server.id;
    options.eq_filters["user_id"] = client_->current_user().id;
    
    Json::Value updates;
    if (!server.name.empty()) updates["name"] = server.name;
    if (!server.alias.empty()) updates["alias"] = server.alias;
    if (!server.address.empty()) updates["address"] = server.address;
    if (!server.host.empty()) updates["host"] = server.host;
    if (server.port > 0) updates["port"] = server.port;
    if (!server.version.empty()) updates["version"] = server.version;
    if (!server.type.empty()) updates["type"] = server.type;
    if (!server.motd.empty()) updates["motd"] = server.motd;
    if (server.max_players > 0) updates["max_players"] = server.max_players;
    updates["online"] = server.online;
    updates["ping_ms"] = server.ping_ms;
    updates["updated_at"] = server.updated_at;
    
    options.updates = updates;
    
    auto result = client_->update(options);
    return result.success;
}

bool SupabaseManager::delete_server(const std::string& server_id) {
    if (!client_ || !client_->is_authenticated()) return false;
    
    SupabaseClient::DBDeleteOptions options;
    options.table = "servers";
    options.eq_filters["id"] = server_id;
    options.eq_filters["user_id"] = client_->current_user().id;
    
    auto result = client_->remove(options);
    return result.success;
}

std::vector<SupabaseProfile> SupabaseManager::get_profiles() {
    std::vector<SupabaseProfile> profiles;
    if (!client_ || !client_->is_authenticated()) return profiles;
    
    SupabaseClient::DBQueryOptions options;
    options.table = "profiles";
    options.eq_filters["user_id"] = client_->current_user().id;
    options.order_by = "updated_at";
    options.order_asc = false;
    
    auto result = client_->select(options);
    if (!result.success) throw_read_error("profiles", result.error);
    
    for (const auto& row : result.data) {
        SupabaseProfile profile;
        profile.id = row.at("id").asString();
        profile.user_id = row.at("user_id").asString();
        profile.name = row.at("name").asString();
        profile.minecraft_version = row.at("minecraft_version").asString();
        profile.loader = row.at("loader").asString();
        profile.loader_version = row.at("loader_version").asString();
        profile.java_path = row.at("java_path").asString();
        profile.memory_mb = row.at("memory_mb").asInt();
        profile.created_at = row.at("created_at").asInt64();
        profile.updated_at = row.at("updated_at").asInt64();
        profile.last_played_at = row.at("last_played_at").asInt64();
        
        // Parse arrays
        if (row.isMember("mods") && row["mods"].isArray()) {
            for (const auto& mod : row["mods"]) {
                profile.mods.push_back(mod.asString());
            }
        }
        if (row.isMember("resource_packs") && row["resource_packs"].isArray()) {
            for (const auto& pack : row["resource_packs"]) {
                profile.resource_packs.push_back(pack.asString());
            }
        }
        if (row.isMember("data_packs") && row["data_packs"].isArray()) {
            for (const auto& pack : row["data_packs"]) {
                profile.data_packs.push_back(pack.asString());
            }
        }
        if (row.isMember("settings")) {
            for (const auto& key : row["settings"].getMemberNames()) {
                profile.settings[key] = row["settings"][key].asString();
            }
        }
        
        profiles.push_back(profile);
    }
    
    return profiles;
}

SupabaseProfile SupabaseManager::create_profile(const SupabaseProfile& profile) {
    if (!client_ || !client_->is_authenticated()) return SupabaseProfile();
    
    SupabaseClient::DBInsertOptions options;
    options.table = "profiles";
    
    Json::Value record;
    record["id"] = profile.id.empty() ? generate_uuid() : profile.id;
    record["user_id"] = client_->current_user().id;
    record["name"] = profile.name;
    record["minecraft_version"] = profile.minecraft_version;
    record["loader"] = profile.loader;
    record["loader_version"] = profile.loader_version;
    record["java_path"] = profile.java_path;
    record["memory_mb"] = profile.memory_mb;
    
    // Add arrays
    Json::Value mods(Json::arrayValue);
    for (const auto& mod : profile.mods) {
        mods.append(mod);
    }
    record["mods"] = mods;
    
    Json::Value resource_packs(Json::arrayValue);
    for (const auto& pack : profile.resource_packs) {
        resource_packs.append(pack);
    }
    record["resource_packs"] = resource_packs;
    
    Json::Value data_packs(Json::arrayValue);
    for (const auto& pack : profile.data_packs) {
        data_packs.append(pack);
    }
    record["data_packs"] = data_packs;
    
    // Add settings
    Json::Value settings;
    for (const auto& [key, value] : profile.settings) {
        settings[key] = value;
    }
    record["settings"] = settings;
    
    options.records.push_back(record);
    
    auto result = client_->insert(options);
    if (!result.success || result.data.empty()) return SupabaseProfile();
    
    SupabaseProfile created = profile;
    created.id = result.data[0]["id"].asString();
    created.user_id = result.data[0]["user_id"].asString();
    created.created_at = result.data[0]["created_at"].asInt64();
    created.updated_at = result.data[0]["updated_at"].asInt64();
    
    return created;
}

bool SupabaseManager::update_profile(const SupabaseProfile& profile) {
    if (!client_ || !client_->is_authenticated()) return false;
    
    SupabaseClient::DBUpdateOptions options;
    options.table = "profiles";
    options.eq_filters["id"] = profile.id;
    options.eq_filters["user_id"] = client_->current_user().id;
    
    Json::Value updates;
    if (!profile.name.empty()) updates["name"] = profile.name;
    if (!profile.minecraft_version.empty()) updates["minecraft_version"] = profile.minecraft_version;
    if (!profile.loader.empty()) updates["loader"] = profile.loader;
    if (!profile.loader_version.empty()) updates["loader_version"] = profile.loader_version;
    if (!profile.java_path.empty()) updates["java_path"] = profile.java_path;
    if (profile.memory_mb > 0) updates["memory_mb"] = profile.memory_mb;
    
    // Update arrays
    Json::Value mods(Json::arrayValue);
    for (const auto& mod : profile.mods) {
        mods.append(mod);
    }
    updates["mods"] = mods;
    
    Json::Value resource_packs(Json::arrayValue);
    for (const auto& pack : profile.resource_packs) {
        resource_packs.append(pack);
    }
    updates["resource_packs"] = resource_packs;
    
    Json::Value data_packs(Json::arrayValue);
    for (const auto& pack : profile.data_packs) {
        data_packs.append(pack);
    }
    updates["data_packs"] = data_packs;
    
    // Update settings
    Json::Value settings;
    for (const auto& [key, value] : profile.settings) {
        settings[key] = value;
    }
    updates["settings"] = settings;
    
    updates["updated_at"] = profile.updated_at;
    updates["last_played_at"] = profile.last_played_at;
    
    options.updates = updates;
    
    auto result = client_->update(options);
    return result.success;
}

bool SupabaseManager::delete_profile(const std::string& profile_id) {
    if (!client_ || !client_->is_authenticated()) return false;
    
    SupabaseClient::DBDeleteOptions options;
    options.table = "profiles";
    options.eq_filters["id"] = profile_id;
    options.eq_filters["user_id"] = client_->current_user().id;
    
    auto result = client_->remove(options);
    return result.success;
}

SupabaseClient::StorageUploadResult SupabaseManager::upload_to_storage(
    const std::string& bucket, const std::string& path,
    const std::vector<uint8_t>& data, const std::string& content_type) {
    if (!client_ || !client_->is_authenticated()) {
        return SupabaseClient::StorageUploadResult();
    }
    
    SupabaseClient::StorageUploadOptions options;
    options.bucket = bucket;
    options.path = path;
    options.data = data;
    options.content_type = content_type;
    options.upsert = true;
    
    return client_->upload_file(options);
}

SupabaseClient::StorageDownloadResult SupabaseManager::download_from_storage(
    const std::string& bucket, const std::string& path) {
    if (!client_ || !client_->is_authenticated()) {
        return SupabaseClient::StorageDownloadResult();
    }
    
    return client_->download_file(bucket, path);
}

std::vector<SupabaseStorageItem> SupabaseManager::list_storage_items(const std::string& type) {
    std::vector<SupabaseStorageItem> items;
    if (!client_ || !client_->is_authenticated()) return items;
    
    SupabaseClient::StorageListOptions options;
    options.bucket = "amalgam";
    if (!type.empty()) {
        options.prefix = type + "/";
    }
    options.limit = 1000;
    
    auto result = client_->list_files(options);
    if (!result.success) return items;
    
    for (const auto& item : result.items) {
        SupabaseStorageItem storage_item;
        storage_item.id = item.id;
        storage_item.name = item.name;
        storage_item.path = item.path;
        storage_item.size_bytes = item.size;
        storage_item.mime_type = item.mime_type;
        storage_item.created_at = item.created_at;
        storage_item.updated_at = item.updated_at;
        storage_item.metadata = item.metadata;
        
        // Extract type from path
        size_t slash = item.path.find('/');
        if (slash != std::string::npos) {
            storage_item.type = item.path.substr(0, slash);
        }
        
        items.push_back(storage_item);
    }
    
    return items;
}

std::vector<SupabaseNode> SupabaseManager::get_nodes() {
    std::vector<SupabaseNode> nodes;
    if (!client_ || !client_->is_authenticated()) return nodes;
    
    SupabaseClient::DBQueryOptions options;
    options.table = "nodes";
    options.eq_filters["user_id"] = client_->current_user().id;
    options.order_by = "created_at";
    options.order_asc = false;
    
    auto result = client_->select(options);
    if (!result.success) throw_read_error("nodes", result.error);
    
    for (const auto& row : result.data) {
        SupabaseNode node;
        node.id = row.at("id").asString();
        node.name = row.at("name").asString();
        node.host = row.at("host").asString();
        node.port = row.at("port").asInt();
        node.region = row.at("region").asString();
        node.zone = row.at("zone").asString();
        node.online = row.at("online").asBool();
        node.last_heartbeat = row.at("last_heartbeat").asInt64();
        node.storage_capacity = row.at("storage_capacity").asInt64();
        node.storage_used = row.at("storage_used").asInt64();
        node.storage_available = row.at("storage_available").asInt64();
        node.cpu_cores = row.at("cpu_cores").asInt();
        node.memory_total = row.at("memory_total").asInt64();
        node.memory_used = row.at("memory_used").asInt64();
        node.created_at = row.at("created_at").asInt64();
        node.updated_at = row.at("updated_at").asInt64();
        
        // Parse tags
        if (row.isMember("tags") && row["tags"].isArray()) {
            for (const auto& tag : row["tags"]) {
                node.tags.push_back(tag.asString());
            }
        }
        
        // Parse metadata
        if (row.isMember("metadata")) {
            for (const auto& key : row["metadata"].getMemberNames()) {
                node.metadata[key] = row["metadata"][key].asString();
            }
        }
        
        nodes.push_back(node);
    }
    
    return nodes;
}

SupabaseNode SupabaseManager::register_node(const SupabaseNode& node) {
    if (!client_ || !client_->is_authenticated()) return SupabaseNode();
    
    SupabaseClient::DBInsertOptions options;
    options.table = "nodes";
    
    Json::Value record;
    record["id"] = node.id.empty() ? generate_uuid() : node.id;
    record["user_id"] = client_->current_user().id;
    record["name"] = node.name;
    record["host"] = node.host;
    record["port"] = node.port;
    record["region"] = node.region;
    record["zone"] = node.zone;
    record["online"] = node.online;
    record["last_heartbeat"] = node.last_heartbeat;
    record["storage_capacity"] = static_cast<int64_t>(node.storage_capacity);
    record["storage_used"] = static_cast<int64_t>(node.storage_used);
    record["storage_available"] = static_cast<int64_t>(node.storage_available);
    record["cpu_cores"] = node.cpu_cores;
    record["memory_total"] = static_cast<int64_t>(node.memory_total);
    record["memory_used"] = static_cast<int64_t>(node.memory_used);
    
    // Add tags
    Json::Value tags(Json::arrayValue);
    for (const auto& tag : node.tags) {
        tags.append(tag);
    }
    record["tags"] = tags;
    
    // Add metadata
    Json::Value metadata;
    for (const auto& [key, value] : node.metadata) {
        metadata[key] = value;
    }
    record["metadata"] = metadata;
    
    options.records.push_back(record);
    
    auto result = client_->insert(options);
    if (!result.success || result.data.empty()) return SupabaseNode();
    
    SupabaseNode created = node;
    created.id = result.data[0]["id"].asString();
    created.user_id = result.data[0]["user_id"].asString();
    created.created_at = result.data[0]["created_at"].asInt64();
    created.updated_at = result.data[0]["updated_at"].asInt64();
    
    return created;
}

bool SupabaseManager::update_node(const SupabaseNode& node) {
    if (!client_ || !client_->is_authenticated()) return false;
    
    SupabaseClient::DBUpdateOptions options;
    options.table = "nodes";
    options.eq_filters["id"] = node.id;
    options.eq_filters["user_id"] = client_->current_user().id;
    
    Json::Value updates;
    if (!node.name.empty()) updates["name"] = node.name;
    if (!node.host.empty()) updates["host"] = node.host;
    if (node.port > 0) updates["port"] = node.port;
    if (!node.region.empty()) updates["region"] = node.region;
    if (!node.zone.empty()) updates["zone"] = node.zone;
    updates["online"] = node.online;
    updates["last_heartbeat"] = node.last_heartbeat;
    updates["storage_capacity"] = static_cast<int64_t>(node.storage_capacity);
    updates["storage_used"] = static_cast<int64_t>(node.storage_used);
    updates["storage_available"] = static_cast<int64_t>(node.storage_available);
    updates["cpu_cores"] = node.cpu_cores;
    updates["memory_total"] = static_cast<int64_t>(node.memory_total);
    updates["memory_used"] = static_cast<int64_t>(node.memory_used);
    
    // Update tags
    Json::Value tags(Json::arrayValue);
    for (const auto& tag : node.tags) {
        tags.append(tag);
    }
    updates["tags"] = tags;
    
    // Update metadata
    Json::Value metadata;
    for (const auto& [key, value] : node.metadata) {
        metadata[key] = value;
    }
    updates["metadata"] = metadata;
    
    updates["updated_at"] = node.updated_at;
    
    options.updates = updates;
    
    auto result = client_->update(options);
    return result.success;
}

bool SupabaseManager::deregister_node(const std::string& node_id) {
    if (!client_ || !client_->is_authenticated()) return false;
    
    SupabaseClient::DBDeleteOptions options;
    options.table = "nodes";
    options.eq_filters["id"] = node_id;
    options.eq_filters["user_id"] = client_->current_user().id;
    
    auto result = client_->remove(options);
    return result.success;
}

void SupabaseManager::subscribe_to_servers(
    const std::function<void(const SupabaseServer&)>& callback) {
    if (!client_ || !client_->is_authenticated()) return;
    
    client_->subscribe("servers", "user_id=eq." + client_->current_user().id,
                       [callback](const Json::Value& data) {
                           SupabaseServer server;
                           server.id = data["id"].asString();
                           server.user_id = data["user_id"].asString();
                           server.name = data["name"].asString();
                           server.host = data["host"].asString();
                           server.port = data["port"].asInt();
                           server.version = data["version"].asString();
                           server.type = data["type"].asString();
                           server.motd = data["motd"].asString();
                           server.max_players = data["max_players"].asInt();
                           server.online = data["online"].asBool();
                           server.ping_ms = data["ping_ms"].asInt();
                           server.created_at = data["created_at"].asInt64();
                           server.updated_at = data["updated_at"].asInt64();
                           
                           callback(server);
                       });
}

void SupabaseManager::subscribe_to_profiles(
    const std::function<void(const SupabaseProfile&)>& callback) {
    if (!client_ || !client_->is_authenticated()) return;
    
    client_->subscribe("profiles", "user_id=eq." + client_->current_user().id,
                       [callback](const Json::Value& data) {
                           SupabaseProfile profile;
                           profile.id = data["id"].asString();
                           profile.user_id = data["user_id"].asString();
                           profile.name = data["name"].asString();
                           profile.minecraft_version = data["minecraft_version"].asString();
                           profile.loader = data["loader"].asString();
                           profile.loader_version = data["loader_version"].asString();
                           profile.java_path = data["java_path"].asString();
                           profile.memory_mb = data["memory_mb"].asInt();
                           profile.created_at = data["created_at"].asInt64();
                           profile.updated_at = data["updated_at"].asInt64();
                           profile.last_played_at = data["last_played_at"].asInt64();
                           
                           // Parse arrays
                           if (data.isMember("mods") && data["mods"].isArray()) {
                               for (const auto& mod : data["mods"]) {
                                   profile.mods.push_back(mod.asString());
                               }
                           }
                           if (data.isMember("resource_packs") && data["resource_packs"].isArray()) {
                               for (const auto& pack : data["resource_packs"]) {
                                   profile.resource_packs.push_back(pack.asString());
                               }
                           }
                           if (data.isMember("data_packs") && data["data_packs"].isArray()) {
                               for (const auto& pack : data["data_packs"]) {
                                   profile.data_packs.push_back(pack.asString());
                               }
                           }
                           if (data.isMember("settings")) {
                               for (const auto& key : data["settings"].getMemberNames()) {
                                   profile.settings[key] = data["settings"][key].asString();
                               }
                           }
                           
                           callback(profile);
                       });
}

void SupabaseManager::subscribe_to_bedrock_profiles(
    const std::function<void(const aml::bedrock::BedrockProfile&)>& callback) {
    if (!client_ || !client_->is_authenticated()) return;
    
    client_->subscribe("bedrock_profiles", "user_id=eq." + client_->current_user().id,
                       [callback](const Json::Value& data) {
                           aml::bedrock::BedrockProfile profile;
                           profile.id = data["id"].asString();
                           profile.name = data["name"].asString();
                           profile.minecraft_version = data["minecraft_version"].asString();
                           profile.banner_path = data["banner_path"].asString();
                           profile.icon_path = data["icon_path"].asString();
                           profile.created = data["created"].asString();
                           profile.last_played = data["last_played"].asString();
                           profile.last_played_ts = data["last_played_ts"].asInt64();
                           profile.favorite = data["favorite"].asBool();
                           profile.group = data["group"].asString();
                           
                           // Parse packs if available
                           if (data["packs"].isArray()) {
                               for (const auto& pack : data["packs"]) {
                                   aml::bedrock::BedrockPackEntry pack_entry;
                                   pack_entry.uuid = pack["uuid"].asString();
                                   pack_entry.name = pack["name"].asString();
                                   pack_entry.version = pack["version"].asString();
                                   pack_entry.type = static_cast<aml::bedrock::BedrockPackType>(pack["type"].asInt());
                                   pack_entry.filename = pack["filename"].asString();
                                   pack_entry.enabled = pack["enabled"].asBool();
                                   pack_entry.load_order = pack["load_order"].asInt();
                                   pack_entry.source = pack["source"].asString();
                                   pack_entry.project_id = pack["project_id"].asString();
                                   pack_entry.author = pack["author"].asString();
                                   pack_entry.description = pack["description"].asString();
                                   pack_entry.size_bytes = pack["size_bytes"].asInt64();
                                   pack_entry.sha1 = pack["sha1"].asString();
                                   pack_entry.duplicate = pack["duplicate"].asBool();
                                   
                                   profile.packs.push_back(pack_entry);
                               }
                           }
                           
                           // Parse worlds if available
                           if (data["worlds"].isArray()) {
                               for (const auto& world : data["worlds"]) {
                                   aml::bedrock::BedrockWorldEntry world_entry;
                                   world_entry.name = world["name"].asString();
                                   world_entry.folder = world["folder"].asString();
                                   world_entry.dimension = world["dimension"].asString();
                                   world_entry.enabled = world["enabled"].asBool();
                                   world_entry.size_bytes = world["size_bytes"].asInt64();
                                   world_entry.last_played = world["last_played"].asString();
                                   world_entry.icon_path = world["icon_path"].asString();
                                   
                                   profile.worlds.push_back(world_entry);
                               }
                           }
                           
                           callback(profile);
                       });
}

void SupabaseManager::subscribe_to_account_activity(
    const std::function<void(const std::string&)>& callback) {
    if (!client_ || !client_->is_authenticated()) return;
    
    client_->subscribe("account_activity", "user_id=eq." + client_->current_user().id,
                       [callback](const Json::Value& data) {
                           // For simplicity, just pass the JSON string
                           // In a real implementation, this would parse the activity data
                           callback(data.toStyledString());
                       });
}

void SupabaseManager::unsubscribe(const std::string& channel_id) {
    if (!client_) return;
    client_->unsubscribe(channel_id);
}

void SupabaseManager::unsubscribe_all() {
    if (client_) client_->unsubscribe_all();
}

bool SupabaseManager::is_initialized() const {
    return initialized_;
}

bool SupabaseManager::is_authenticated() const {
    if (!client_) return false;
    return client_->is_authenticated();
}

uint64_t SupabaseManager::session_generation() const {
    return session_generation_.load(std::memory_order_acquire);
}

std::string SupabaseManager::get_project_url() const {
    return client_ ? client_->project_url() : std::string();
}

bool SupabaseManager::auto_login(const std::string& access_token, const std::string& refresh_token) {
    if (!client_) return false;
    
    // Validate the persisted access token before restoring the session.
    client_->set_access_token(access_token);
    auto user = client_->get_user(access_token);
    if (!user.id.empty()) {
        user.refresh_token = refresh_token;
        client_->set_session(user);
        session_generation_.fetch_add(1, std::memory_order_acq_rel);
        return true;
    }
    
    // If access token is expired, try to refresh
    if (!refresh_token.empty()) {
        auto response = client_->refresh_token(refresh_token);
        if (response.success && !response.user.id.empty()) {
            client_->set_session(response.user);
            session_generation_.fetch_add(1, std::memory_order_acq_rel);
            return true;
        }
    }
    // `set_access_token` above is provisional. A terminal validation failure
    // must not leave that token (or a prior user object) available to later
    // request construction.
    client_->clear_session();
    session_generation_.fetch_add(1, std::memory_order_acq_rel);
    return false;
}

bool SupabaseManager::publish_essentials_signal(const EssentialsSignal& signal) {
    if (!client_ || !client_->is_authenticated()) return false;
    Json row = Json::obj();
    row.set("session_id", Json::str(signal.session_id));
    row.set("sender_id", Json::str(signal.sender_id));
    row.set("recipient_id", Json::str(signal.recipient_id));
    row.set("kind", Json::str(signal.kind));
    row.set("payload", Json::str(signal.payload));
    SupabaseClient::DBInsertOptions options;
    options.table = "essentials_signals";
    options.records.push_back(std::move(row));
    return client_->insert(options).success;
}

std::vector<EssentialsSignal> SupabaseManager::poll_essentials_signals(
    const std::string& session_id, const std::string& recipient_id, int limit) {
    std::vector<EssentialsSignal> signals;
    if (!client_ || !client_->is_authenticated()) return signals;
    SupabaseClient::DBQueryOptions options;
    options.table = "essentials_signals";
    options.eq_filters["session_id"] = session_id;
    options.eq_filters["recipient_id"] = recipient_id;
    options.order_by = "created_at";
    options.order_asc = true;
    options.limit = std::max(1, std::min(200, limit));
    const auto result = client_->select(options);
    if (!result.success) return signals;
    for (const auto& row : result.data) {
        EssentialsSignal signal;
        signal.id = row.get("id").as_str();
        signal.session_id = row.get("session_id").as_str();
        signal.sender_id = row.get("sender_id").as_str();
        signal.recipient_id = row.get("recipient_id").as_str();
        signal.kind = row.get("kind").as_str();
        signal.payload = row.get("payload").as_str();
        signal.created_at = row.get("created_at").as_int();
        signals.push_back(std::move(signal));
    }
    return signals;
}

bool SupabaseManager::remove_essentials_signal(const std::string& signal_id) {
    if (!client_ || !client_->is_authenticated() || signal_id.empty()) return false;
    SupabaseClient::DBDeleteOptions options;
    options.table = "essentials_signals";
    options.eq_filters["id"] = signal_id;
    return client_->remove(options).success;
}

// ---------------------------------------------------------------------------
// Subscription Management (synced from Whop by backend)
// ---------------------------------------------------------------------------

SupabaseSubscription SupabaseManager::get_subscription() {
    std::lock_guard<std::mutex> lock(mutex_);
    SupabaseSubscription sub;
    if (!client_ || !client_->is_authenticated()) return sub;

    SupabaseClient::DBQueryOptions opts;
    opts.table = "subscriptions";
    opts.select = "*";
    // Filter to current user's active subscription
    auto user = client_->current_user();
    if (user.id.empty()) return sub;
    opts.eq_filters["user_id"] = user.id;
    opts.eq_filters["status"] = "active";
    opts.order_by = "created_at";
    opts.order_asc = false;
    opts.limit = 1;

    auto result = client_->select(opts);
    if (!result.success) return sub;
    sub.query_succeeded = true;
    if (result.data.empty()) return sub;

    const Json& row = result.data[0];
    sub.id = row.get("id").as_str();
    sub.user_id = row.get("user_id").as_str();
    sub.plan_id = row.get("plan_id").as_str();
    sub.plan_label = row.get("plan_label").as_str();
    sub.status = row.get("status").as_str();
    sub.amount = row.get("amount").as_num(0.0);
    sub.currency = row.get("currency").as_str("USD");
    sub.current_period_start = row.get("current_period_start").as_int(0);
    sub.current_period_end = row.get("current_period_end").as_int(0);
    sub.created_at = row.get("created_at").as_int(0);
    sub.cancelled_at = row.get("cancelled_at").as_int(0);
    sub.provider = row.get("provider").as_str("whop");
    sub.external_id = row.get("external_id").as_str();
    sub.valid = true;
    return sub;
}

std::vector<SupabaseSubscription> SupabaseManager::get_all_subscriptions() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SupabaseSubscription> out;
    if (!client_ || !client_->is_authenticated()) return out;

    SupabaseClient::DBQueryOptions opts;
    opts.table = "subscriptions";
    opts.select = "*";
    auto user = client_->current_user();
    if (user.id.empty()) return out;
    opts.eq_filters["user_id"] = user.id;
    opts.order_by = "created_at";
    opts.order_asc = false;

    auto result = client_->select(opts);
    if (!result.success) return out;

    for (const auto& row : result.data) {
        SupabaseSubscription sub;
        sub.id = row.get("id").as_str();
        sub.user_id = row.get("user_id").as_str();
        sub.plan_id = row.get("plan_id").as_str();
        sub.plan_label = row.get("plan_label").as_str();
        sub.status = row.get("status").as_str();
        sub.amount = row.get("amount").as_num(0.0);
        sub.currency = row.get("currency").as_str("USD");
        sub.current_period_start = row.get("current_period_start").as_int(0);
        sub.current_period_end = row.get("current_period_end").as_int(0);
        sub.created_at = row.get("created_at").as_int(0);
        sub.cancelled_at = row.get("cancelled_at").as_int(0);
        sub.provider = row.get("provider").as_str("whop");
        sub.external_id = row.get("external_id").as_str();
        sub.valid = true;
        out.push_back(std::move(sub));
    }
    return out;
}

SupabaseTurnUsage SupabaseManager::get_turn_usage() {
    std::lock_guard<std::mutex> lock(mutex_);
    SupabaseTurnUsage usage;
    if (!client_ || !client_->is_authenticated()) return usage;

    SupabaseClient::DBQueryOptions opts;
    opts.table = "turn_usage";
    opts.select = "*";
    auto user = client_->current_user();
    if (user.id.empty()) return usage;
    opts.eq_filters["user_id"] = user.id;
    opts.limit = 1;

    auto result = client_->select(opts);
    if (!result.success) return usage;
    usage.query_succeeded = true;
    if (result.data.empty()) return usage;

    const Json& row = result.data[0];
    usage.user_id = row.get("user_id").as_str();
    usage.used_bytes = static_cast<uint64_t>(row.get("used_bytes").as_int(0));
    usage.monthly_bytes = static_cast<uint64_t>(row.get("monthly_bytes").as_int(0));
    usage.period_start = row.get("period_start").as_int(0);
    usage.period_end = row.get("period_end").as_int(0);
    usage.valid = true;
    return usage;
}

// ---------------------------------------------------------------------------
// Bedrock Management
// ---------------------------------------------------------------------------

std::vector<aml::bedrock::BedrockProfile> SupabaseManager::get_bedrock_profiles() {
    std::vector<aml::bedrock::BedrockProfile> profiles;
    if (!client_ || !client_->is_authenticated()) return profiles;
    const auto& current_user = client_->current_user();
    if (current_user.id.empty()) return profiles;
    
    SupabaseClient::DBQueryOptions options;
    options.table = "bedrock_profiles";
    options.select = "*";
    options.eq_filters["user_id"] = current_user.id;
    options.order_by = "last_played_ts";
    options.order_asc = false;
    
    auto result = client_->select(options);
    if (!result.success || result.data.empty()) {
        return profiles;
    }
    
    for (const auto& row : result.data) {
        aml::bedrock::BedrockProfile profile;
        profile.id = row["id"].asString();
        profile.name = row["name"].asString();
        profile.minecraft_version = row["minecraft_version"].asString();
        profile.banner_path = row["banner_path"].asString();
        profile.icon_path = row["icon_path"].asString();
        profile.created = row["created"].asString();
        profile.last_played = row["last_played"].asString();
        profile.last_played_ts = row["last_played_ts"].asInt64();
        profile.favorite = row["favorite"].asBool();
        profile.group = row["group"].asString();
        
        // Parse packs if available
        if (row["packs"].isArray()) {
            for (const auto& pack : row["packs"]) {
                aml::bedrock::BedrockPackEntry pack_entry;
                pack_entry.profile_id = profile.id;
                pack_entry.profile_name = profile.name;
                pack_entry.uuid = pack["uuid"].asString();
                pack_entry.name = pack["name"].asString();
                pack_entry.version = pack["version"].asString();
                pack_entry.type = static_cast<aml::bedrock::BedrockPackType>(pack["type"].asInt());
                pack_entry.filename = pack["filename"].asString();
                pack_entry.enabled = pack["enabled"].asBool();
                pack_entry.load_order = pack["load_order"].asInt();
                pack_entry.source = pack["source"].asString();
                pack_entry.project_id = pack["project_id"].asString();
                pack_entry.author = pack["author"].asString();
                pack_entry.description = pack["description"].asString();
                pack_entry.size_bytes = pack["size_bytes"].asInt64();
                pack_entry.sha1 = pack["sha1"].asString();
                pack_entry.duplicate = pack["duplicate"].asBool();
                
                profile.packs.push_back(pack_entry);
            }
        }
        
        // Parse worlds if available
        if (row["worlds"].isArray()) {
            for (const auto& world : row["worlds"]) {
                aml::bedrock::BedrockWorldEntry world_entry;
                world_entry.profile_id = profile.id;
                world_entry.profile_name = profile.name;
                world_entry.name = world["name"].asString();
                world_entry.folder = world["folder"].asString();
                world_entry.dimension = world["dimension"].asString();
                world_entry.enabled = world["enabled"].asBool();
                world_entry.size_bytes = world["size_bytes"].asInt64();
                world_entry.last_played = world["last_played"].asString();
                world_entry.icon_path = world["icon_path"].asString();
                
                profile.worlds.push_back(world_entry);
            }
        }
        
        profiles.push_back(profile);
    }
    
    return profiles;
}

aml::bedrock::BedrockProfile SupabaseManager::create_bedrock_profile(const aml::bedrock::BedrockProfile& profile) {
    if (!client_ || !client_->is_authenticated()) {
        return aml::bedrock::BedrockProfile();
    }
    const auto& current_user = client_->current_user();
    if (current_user.id.empty()) return aml::bedrock::BedrockProfile();
    
    SupabaseClient::DBInsertOptions options;
    options.table = "bedrock_profiles";
    
    Json::Value row;
    row["id"] = profile.id;
    row["user_id"] = current_user.id;
    row["name"] = profile.name;
    row["minecraft_version"] = profile.minecraft_version;
    row["banner_path"] = profile.banner_path;
    row["icon_path"] = profile.icon_path;
    row["created"] = profile.created;
    row["last_played"] = profile.last_played;
    row["last_played_ts"] = profile.last_played_ts;
    row["favorite"] = profile.favorite;
    row["group"] = profile.group;
    
    // Serialize packs
    Json::Value packs_array;
    for (const auto& pack : profile.packs) {
        Json::Value pack_obj;
        pack_obj["uuid"] = pack.uuid;
        pack_obj["name"] = pack.name;
        pack_obj["version"] = pack.version;
        pack_obj["type"] = static_cast<int>(pack.type);
        pack_obj["filename"] = pack.filename;
        pack_obj["enabled"] = pack.enabled;
        pack_obj["load_order"] = pack.load_order;
        pack_obj["source"] = pack.source;
        pack_obj["project_id"] = pack.project_id;
        pack_obj["author"] = pack.author;
        pack_obj["description"] = pack.description;
        pack_obj["size_bytes"] = static_cast<int64_t>(pack.size_bytes);
        pack_obj["sha1"] = pack.sha1;
        pack_obj["duplicate"] = pack.duplicate;
        packs_array.append(pack_obj);
    }
    row["packs"] = packs_array;
    
    // Serialize worlds
    Json::Value worlds_array;
    for (const auto& world : profile.worlds) {
        Json::Value world_obj;
        world_obj["name"] = world.name;
        world_obj["folder"] = world.folder;
        world_obj["dimension"] = world.dimension;
        world_obj["enabled"] = world.enabled;
        world_obj["size_bytes"] = static_cast<int64_t>(world.size_bytes);
        world_obj["last_played"] = world.last_played;
        world_obj["icon_path"] = world.icon_path;
        worlds_array.append(world_obj);
    }
    row["worlds"] = worlds_array;
    
    options.records.push_back(row);
    
    auto result = client_->insert(options);
    if (!result.success || result.data.empty()) {
        return aml::bedrock::BedrockProfile();
    }
    
    return profile;
}

bool SupabaseManager::update_bedrock_profile(const aml::bedrock::BedrockProfile& profile) {
    if (!client_ || !client_->is_authenticated()) {
        return false;
    }
    const auto& current_user = client_->current_user();
    if (current_user.id.empty() || profile.id.empty()) return false;
    
    SupabaseClient::DBUpdateOptions options;
    options.table = "bedrock_profiles";
    options.eq_filters["id"] = profile.id;
    options.eq_filters["user_id"] = current_user.id;
    
    Json::Value row;
    row["name"] = profile.name;
    row["minecraft_version"] = profile.minecraft_version;
    row["banner_path"] = profile.banner_path;
    row["icon_path"] = profile.icon_path;
    row["last_played"] = profile.last_played;
    row["last_played_ts"] = profile.last_played_ts;
    row["favorite"] = profile.favorite;
    row["group"] = profile.group;
    
    // Serialize packs
    Json::Value packs_array;
    for (const auto& pack : profile.packs) {
        Json::Value pack_obj;
        pack_obj["uuid"] = pack.uuid;
        pack_obj["name"] = pack.name;
        pack_obj["version"] = pack.version;
        pack_obj["type"] = static_cast<int>(pack.type);
        pack_obj["filename"] = pack.filename;
        pack_obj["enabled"] = pack.enabled;
        pack_obj["load_order"] = pack.load_order;
        pack_obj["source"] = pack.source;
        pack_obj["project_id"] = pack.project_id;
        pack_obj["author"] = pack.author;
        pack_obj["description"] = pack.description;
        pack_obj["size_bytes"] = static_cast<int64_t>(pack.size_bytes);
        pack_obj["sha1"] = pack.sha1;
        pack_obj["duplicate"] = pack.duplicate;
        packs_array.append(pack_obj);
    }
    row["packs"] = packs_array;
    
    // Serialize worlds
    Json::Value worlds_array;
    for (const auto& world : profile.worlds) {
        Json::Value world_obj;
        world_obj["name"] = world.name;
        world_obj["folder"] = world.folder;
        world_obj["dimension"] = world.dimension;
        world_obj["enabled"] = world.enabled;
        world_obj["size_bytes"] = static_cast<int64_t>(world.size_bytes);
        world_obj["last_played"] = world.last_played;
        world_obj["icon_path"] = world.icon_path;
        worlds_array.append(world_obj);
    }
    row["worlds"] = worlds_array;
    
    options.updates = row;
    
    auto result = client_->update(options);
    return result.success;
}

bool SupabaseManager::delete_bedrock_profile(const std::string& profile_id) {
    if (!client_ || !client_->is_authenticated()) {
        return false;
    }
    const auto& current_user = client_->current_user();
    if (current_user.id.empty() || profile_id.empty()) return false;
    
    SupabaseClient::DBDeleteOptions options;
    options.table = "bedrock_profiles";
    options.eq_filters["id"] = profile_id;
    options.eq_filters["user_id"] = current_user.id;
    
    auto result = client_->remove(options);
    return result.success;
}

// ---------------------------------------------------------------------------
// Social: Friends
// ---------------------------------------------------------------------------

std::vector<SupabaseFriendship> SupabaseManager::get_friends() {
    std::vector<SupabaseFriendship> friends;
    if (!client_ || !client_->is_authenticated()) return friends;
    auto result = client_->call_edge_function("get_friends");
    if (result.success && result.data.isArray()) {
        for (const auto& item : result.data) {
            SupabaseFriendship f;
            f.id = item["id"].asString();
            f.user_id_a = item["user_id_a"].asString();
            f.user_id_b = item["user_id_b"].asString();
            f.status = item["status"].asString();
            // The backend has used both flattened and nested profile shapes
            // during rollout. Accept either without issuing N+1 requests.
            const Json* profile = nullptr;
            if (item.isMember("profile") && item["profile"].isObject())
                profile = &item["profile"];
            else if (item.isMember("friend") && item["friend"].isObject())
                profile = &item["friend"];
            const auto profile_value = [&item, profile](const char* key) {
                if (item.isMember(key)) return item[key].asString();
                return profile && profile->isMember(key) ? (*profile)[key].asString() : std::string{};
            };
            f.friend_display_name = profile_value("display_name");
            if (f.friend_display_name.empty()) f.friend_display_name = profile_value("username");
            f.friend_avatar_url = profile_value("avatar_url");
            f.friend_minecraft_username = profile_value("minecraft_username");
            f.created_at = item["created_at"].asInt64();
            f.updated_at = item["updated_at"].asInt64();
            friends.push_back(f);
        }
    }
    return friends;
}

std::vector<SupabaseFriendRequest> SupabaseManager::get_friend_requests() {
    std::vector<SupabaseFriendRequest> requests;
    if (!client_ || !client_->is_authenticated()) return requests;
    auto result = client_->call_edge_function("get_friend_requests");
    if (result.success && result.data.isArray()) {
        for (const auto& item : result.data) {
            SupabaseFriendRequest r;
            r.id = item["id"].asString();
            r.sender_id = item["sender_id"].asString();
            r.sender_username = item["sender_username"].asString();
            r.sender_avatar_url = item["sender_avatar_url"].asString();
            r.receiver_id = item["receiver_id"].asString();
            r.status = item["status"].asString();
            r.message = item["message"].asString();
            r.created_at = item["created_at"].asInt64();
            requests.push_back(r);
        }
    }
    return requests;
}

bool SupabaseManager::send_friend_request(const std::string& receiver_id, const std::string& message) {
    Json body;
    body.set("receiver_id", receiver_id);
    body.set("message", message);
    auto result = client_->call_edge_function("send_friend_request", body);
    return result.success;
}

bool SupabaseManager::accept_friend_request(const std::string& request_id) {
    Json body;
    body.set("request_id", request_id);
    auto result = client_->call_edge_function("accept_friend_request", body);
    return result.success;
}

bool SupabaseManager::reject_friend_request(const std::string& request_id) {
    Json body;
    body.set("request_id", request_id);
    auto result = client_->call_edge_function("reject_friend_request", body);
    return result.success;
}

bool SupabaseManager::remove_friend(const std::string& friend_id) {
    Json body;
    body.set("friend_id", friend_id);
    auto result = client_->call_edge_function("remove_friend", body);
    return result.success;
}

bool SupabaseManager::block_user(const std::string& user_id) {
    if (!client_ || !client_->is_authenticated()) return false;
    Json args = Json::obj();
    args.set("blocked_user_id", Json::str(user_id));
    return client_->rpc("block_user", args).success;
}

bool SupabaseManager::unblock_user(const std::string& user_id) {
    if (!client_ || !client_->is_authenticated()) return false;
    Json args = Json::obj();
    args.set("blocked_user_id", Json::str(user_id));
    return client_->rpc("unblock_user", args).success;
}

std::vector<std::string> SupabaseManager::get_blocked_users() {
    std::vector<std::string> blocked;
    if (!client_ || !client_->is_authenticated()) return blocked;
    auto result = client_->rpc("get_blocked_users", Json::obj());
    if (!result.success || result.data.empty() || !result.data.front().isArray()) return blocked;
    for (const auto& item : result.data.front()) {
        const auto user_id = item.asString();
        if (!user_id.empty()) blocked.push_back(user_id);
    }
    return blocked;
}

std::vector<SupabasePublicProfile> SupabaseManager::search_users(const std::string& query, int limit) {
    Json body;
    body.set("query", query);
    body.set("limit", limit);
    auto result = client_->call_edge_function("search_users", body);
    std::vector<SupabasePublicProfile> users;
    if (result.success && result.data.isArray()) {
        for (const auto& item : result.data) {
            SupabasePublicProfile p;
            p.user_id = item["user_id"].asString();
            p.display_name = item["display_name"].asString();
            p.avatar_url = item["avatar_url"].asString();
            p.bio = item["bio"].asString();
            p.minecraft_username = item["minecraft_username"].asString();
            p.is_public = item["is_public"].asBool();
            p.friend_count = static_cast<int>(item["friend_count"].asInt64());
            p.play_time_seconds = item["play_time_seconds"].asInt64();
            p.last_seen_at = item["last_seen_at"].asInt64();
            users.push_back(p);
        }
    }
    return users;
}

// ---------------------------------------------------------------------------
// Social: Messaging
// ---------------------------------------------------------------------------

std::vector<SupabaseConversation> SupabaseManager::get_conversations() {
    auto result = client_->call_edge_function("get_conversations");
    std::vector<SupabaseConversation> convos;
    if (result.success && result.data.isArray()) {
        for (const auto& item : result.data) {
            SupabaseConversation c;
            c.id = item["id"].asString();
            if (item.isMember("participant_ids") && item["participant_ids"].isArray()) {
                for (const auto& pid : item["participant_ids"]) {
                    c.participant_ids.push_back(pid.asString());
                }
            }
            c.last_message_content = item["last_message_content"].asString();
            c.last_message_sender_id = item["last_message_sender_id"].asString();
            c.last_message_at = item["last_message_at"].asInt64();
            c.created_at = item["created_at"].asInt64();
            c.unread_count = static_cast<int>(item["unread_count"].asInt64());
            convos.push_back(c);
        }
    }
    return convos;
}

std::vector<SupabaseMessage> SupabaseManager::get_messages(const std::string& conversation_id, int limit, int offset) {
    Json body;
    body.set("conversation_id", conversation_id);
    body.set("limit", limit);
    body.set("offset", offset);
    auto result = client_->call_edge_function("get_messages", body);
    std::vector<SupabaseMessage> messages;
    if (result.success && result.data.isArray()) {
        for (const auto& item : result.data) {
            SupabaseMessage m;
            m.id = item["id"].asString();
            m.conversation_id = item["conversation_id"].asString();
            m.sender_id = item["sender_id"].asString();
            m.sender_username = item["sender_username"].asString();
            m.content = item["content"].asString();
            m.is_read = item["is_read"].asBool();
            m.created_at = item["created_at"].asInt64();
            messages.push_back(m);
        }
    }
    return messages;
}

SupabaseMessage SupabaseManager::send_message(const std::string& conversation_id, const std::string& content) {
    Json body;
    body.set("conversation_id", conversation_id);
    body.set("content", content);
    auto result = client_->call_edge_function("send_message", body);
    SupabaseMessage msg;
    if (result.success) {
        msg.id = result.data["id"].asString();
        msg.conversation_id = conversation_id;
        msg.sender_id = client_->current_user().id;
        msg.content = content;
        msg.created_at = std::time(nullptr);
    }
    return msg;
}

bool SupabaseManager::mark_messages_read(const std::string& conversation_id) {
    Json body;
    body.set("conversation_id", conversation_id);
    auto result = client_->call_edge_function("mark_messages_read", body);
    return result.success;
}

SupabaseConversation SupabaseManager::get_or_create_conversation(const std::string& other_user_id) {
    Json body;
    body.set("other_user_id", other_user_id);
    auto result = client_->call_edge_function("get_or_create_conversation", body);
    SupabaseConversation conv;
    if (result.success) {
        conv.id = result.data["id"].asString();
        if (result.data.isMember("participant_ids") && result.data["participant_ids"].isArray()) {
            for (const auto& pid : result.data["participant_ids"]) {
                conv.participant_ids.push_back(pid.asString());
            }
        }
        conv.created_at = result.data["created_at"].asInt64();
    }
    return conv;
}

// ---------------------------------------------------------------------------
// Social: Parties
// ---------------------------------------------------------------------------

std::vector<SupabaseParty> SupabaseManager::get_parties() {
    auto result = client_->call_edge_function("get_parties");
    std::vector<SupabaseParty> parties;
    if (result.success && result.data.isArray()) {
        for (const auto& item : result.data) {
            SupabaseParty p;
            p.id = item["id"].asString();
            p.owner_id = item["owner_id"].asString();
            p.owner_username = item["owner_username"].asString();
            p.name = item["name"].asString();
            p.is_public = item["is_public"].asBool();
            p.invite_code = item["invite_code"].asString();
            p.max_members = static_cast<int>(item["max_members"].asInt64());
            p.member_count = static_cast<int>(item["member_count"].asInt64());
            p.created_at = item["created_at"].asInt64();
            parties.push_back(p);
        }
    }
    return parties;
}

SupabaseParty SupabaseManager::create_party(const std::string& name, bool is_public, int max_members) {
    Json body;
    body.set("name", name);
    body.set("is_public", is_public);
    body.set("max_members", max_members);
    auto result = client_->call_edge_function("create_party", body);
    SupabaseParty party;
    if (result.success) {
        party.id = result.data["id"].asString();
        party.name = name;
        party.is_public = is_public;
        party.max_members = max_members;
        party.invite_code = result.data["invite_code"].asString();
        party.owner_id = client_->current_user().id;
        party.member_count = 1;
        party.created_at = std::time(nullptr);
    }
    return party;
}

bool SupabaseManager::join_party(const std::string& invite_code) {
    Json body;
    body.set("invite_code", invite_code);
    auto result = client_->call_edge_function("join_party", body);
    return result.success;
}

bool SupabaseManager::leave_party(const std::string& party_id) {
    Json body;
    body.set("party_id", party_id);
    auto result = client_->call_edge_function("leave_party", body);
    return result.success;
}

bool SupabaseManager::disband_party(const std::string& party_id) {
    Json body;
    body.set("party_id", party_id);
    auto result = client_->call_edge_function("disband_party", body);
    return result.success;
}

std::vector<SupabasePartyMember> SupabaseManager::get_party_members(const std::string& party_id) {
    Json body;
    body.set("party_id", party_id);
    auto result = client_->call_edge_function("get_party_members", body);
    std::vector<SupabasePartyMember> members;
    if (result.success && result.data.isArray()) {
        for (const auto& item : result.data) {
            SupabasePartyMember m;
            m.party_id = party_id;
            m.user_id = item["user_id"].asString();
            m.username = item["username"].asString();
            m.role = item["role"].asString();
            m.joined_at = item["joined_at"].asInt64();
            members.push_back(m);
        }
    }
    return members;
}

// ---------------------------------------------------------------------------
// Social: Presence
// ---------------------------------------------------------------------------

bool SupabaseManager::update_presence(const std::string& status, const std::string& status_message, const std::string& server_id) {
    if (!client_ || !client_->is_authenticated()) return false;
    Json body;
    body.set("status", status);
    body.set("status_message", status_message);
    body.set("current_server_id", server_id);
    auto result = client_->call_edge_function("update_presence", body);
    return result.success;
}

SupabasePresence SupabaseManager::get_user_presence(const std::string& user_id) {
    Json body;
    body.set("user_id", user_id);
    auto result = client_->call_edge_function("get_user_presence", body);
    SupabasePresence p;
    if (result.success) {
        p.user_id = result.data["user_id"].asString();
        p.status = result.data["status"].asString();
        p.status_message = result.data["status_message"].asString();
        p.current_server_id = result.data["current_server_id"].asString();
        p.last_seen_at = result.data["last_seen_at"].asInt64();
        p.updated_at = result.data["updated_at"].asInt64();
    }
    return p;
}

std::vector<SupabasePresence> SupabaseManager::get_friends_presence() {
    std::vector<SupabasePresence> presences;
    if (!client_ || !client_->is_authenticated()) return presences;
    auto result = client_->call_edge_function("get_friends_presence");
    if (result.success && result.data.isArray()) {
        for (const auto& item : result.data) {
            SupabasePresence p;
            p.user_id = item["user_id"].asString();
            p.status = item["status"].asString();
            p.status_message = item["status_message"].asString();
            p.current_server_id = item["current_server_id"].asString();
            p.last_seen_at = item["last_seen_at"].asInt64();
            p.updated_at = item["updated_at"].asInt64();
            presences.push_back(p);
        }
    }
    return presences;
}

// ---------------------------------------------------------------------------
// Social: Realtime Subscriptions
// ---------------------------------------------------------------------------

void SupabaseManager::subscribe_to_messages(
    const std::function<void(const SupabaseMessage&)>& callback) {
    if (!client_ || !client_->is_authenticated()) return;
    client_->subscribe("messages", "",
        [callback](const Json::Value& data) {
            SupabaseMessage m;
            m.id = data["id"].asString();
            m.conversation_id = data["conversation_id"].asString();
            m.sender_id = data["sender_id"].asString();
            m.sender_username = data["sender_username"].asString();
            m.content = data["content"].asString();
            m.is_read = data["is_read"].asBool();
            m.created_at = data["created_at"].asInt64();
            callback(m);
        });
}

void SupabaseManager::subscribe_to_presence(
    const std::function<void(const SupabasePresence&)>& callback) {
    if (!client_ || !client_->is_authenticated()) return;
    client_->subscribe("user_presence", "",
        [callback](const Json::Value& data) {
            SupabasePresence p;
            p.user_id = data["user_id"].asString();
            p.status = data["status"].asString();
            p.status_message = data["status_message"].asString();
            p.current_server_id = data["current_server_id"].asString();
            p.last_seen_at = data["last_seen_at"].asInt64();
            p.updated_at = data["updated_at"].asInt64();
            callback(p);
        });
}

void SupabaseManager::subscribe_to_friend_requests(
    const std::function<void(const SupabaseFriendRequest&)>& callback) {
    if (!client_ || !client_->is_authenticated()) return;
    client_->subscribe("friend_requests", "",
        [callback](const Json::Value& data) {
            SupabaseFriendRequest r;
            r.id = data["id"].asString();
            r.sender_id = data["sender_id"].asString();
            r.sender_username = data["sender_username"].asString();
            r.sender_avatar_url = data["sender_avatar_url"].asString();
            r.receiver_id = data["receiver_id"].asString();
            r.status = data["status"].asString();
            r.message = data["message"].asString();
            r.created_at = data["created_at"].asInt64();
            callback(r);
        });
}

void SupabaseManager::subscribe_to_party_updates(
    const std::function<void(const std::string&, const std::string&)>& callback) {
    if (!client_ || !client_->is_authenticated()) return;
    client_->subscribe("party_members", "",
        [callback](const Json::Value& data) {
            std::string party_id = data["party_id"].asString();
            std::string event_type = data["event_type"].asString();
            callback(party_id, event_type);
        });
}

// ---------------------------------------------------------------------------
// Data Collection: batch insert RPCs
// ---------------------------------------------------------------------------

bool SupabaseManager::insert_events(const Json::Value& events_json) {
    if (!client_ || !client_->is_authenticated()) return false;
    auto result = client_->rpc("insert_events", events_json);
    return result.success;
}

bool SupabaseManager::insert_metrics(const Json::Value& metrics_json) {
    if (!client_ || !client_->is_authenticated()) return false;
    auto result = client_->rpc("insert_metrics", metrics_json);
    return result.success;
}

bool SupabaseManager::insert_errors(const Json::Value& errors_json) {
    if (!client_ || !client_->is_authenticated()) return false;
    auto result = client_->rpc("insert_errors", errors_json);
    return result.success;
}

bool SupabaseManager::insert_feature_usage(const Json::Value& usage_json) {
    if (!client_ || !client_->is_authenticated()) return false;
    auto result = client_->rpc("insert_feature_usage", usage_json);
    return result.success;
}

bool SupabaseManager::record_audit(const std::string& action, const std::string& resource_type,
                                  const std::string& resource_id, const Json::Value& changes,
                                  const Json::Value& metadata, const std::string& source) {
    if (!client_ || !client_->is_authenticated()) return false;
    Json args;
    args["p_action"] = action;
    args["p_resource_type"] = resource_type;
    args["p_resource_id"] = resource_id;
    args["p_changes"] = changes.isNull() ? Json::objectValue : changes;
    args["p_metadata"] = metadata.isNull() ? Json::objectValue : metadata;
    args["p_source"] = source;
    auto result = client_->rpc("record_audit", args);
    return result.success;
}

}  // namespace aml::supabase
