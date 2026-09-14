#include "auth.h"

#include "json.h"
#include "net.h"

#include <windows.h>
#include <wincrypt.h>
#include <shellapi.h>

#include <chrono>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <thread>
#include <vector>

#pragma comment(lib, "crypt32.lib")

namespace aml::auth {

namespace {

// Minecraft accounts are consumer Microsoft accounts. Using the consumers
// tenant avoids common-tenant scope negotiation returning AADSTS70011 after
// the device-code approval page has already reported success.
constexpr char kDeviceUrl[] = "https://login.microsoftonline.com/consumers/oauth2/v2.0/devicecode";
constexpr char kTokenUrl[] = "https://login.microsoftonline.com/consumers/oauth2/v2.0/token";

int64_t now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string client_id_problem(const std::string& client_id) {
    if (client_id.empty()) {
        return "Microsoft sign-in is not configured for this Amalgam build. Add this launcher's Application (client) ID in Settings > Advanced before connecting an account.";
    }
    return "The Microsoft application ID is not valid. Use the GUID named Application (client) ID from this launcher's Microsoft Entra app registration.";
}

std::string friendly_oauth_error(const Json& response, const char* fallback) {
    const std::string code = response.get("error").as_str();
    const std::string description = response.get("error_description").as_str();
    if (code == "unauthorized_client" || description.find("AADSTS700016") != std::string::npos) {
        return "Microsoft rejected this launcher registration. Confirm the Application (client) ID and configure the app for personal Microsoft accounts, then try again.";
    }
    if (code == "invalid_client") {
        return "Microsoft could not validate this launcher registration. Check the Application (client) ID in Settings > Advanced.";
    }
    if (code == "access_denied") return "Microsoft sign-in was cancelled or access was not granted.";
    if (code == "expired_token") return "The Microsoft sign-in code expired. Start a new sign-in and use the new code.";
    return description.empty() ? std::string(fallback) : description;
}

std::wstring account_path() {
    wchar_t buffer[MAX_PATH]{};
    DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return L"";
    std::wstring dir = std::wstring(buffer, length) + L"\\Amalgam";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\account.json";
}

std::string url_encode(const std::string& value) {
    std::string out;
    static const char* hex = "0123456789ABCDEF";
    for (unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

std::string base64(const BYTE* data, DWORD len) {
    DWORD required = 0;
    if (!CryptBinaryToStringA(data, len, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &required))
        return {};
    std::string out(required, '\0');
    if (!CryptBinaryToStringA(data, len, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                              out.data(), &required)) return {};
    if (!out.empty() && out.back() == '\0') out.pop_back();
    return out;
}

bool from_base64(const std::string& value, std::vector<BYTE>& out) {
    DWORD required = 0;
    if (!CryptStringToBinaryA(value.c_str(), static_cast<DWORD>(value.size()),
                              CRYPT_STRING_BASE64, nullptr, &required, nullptr, nullptr)) return false;
    out.resize(required);
    return CryptStringToBinaryA(value.c_str(), static_cast<DWORD>(value.size()), CRYPT_STRING_BASE64,
                                out.data(), &required, nullptr, nullptr) == TRUE;
}

std::string protect(const std::string& value) {
    DATA_BLOB input{static_cast<DWORD>(value.size()),
                    reinterpret_cast<BYTE*>(const_cast<char*>(value.data()))};
    DATA_BLOB output{};
    if (!CryptProtectData(&input, L"Amalgam account", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &output)) return {};
    std::string encoded = base64(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return encoded;
}

std::string unprotect(const std::string& value) {
    std::vector<BYTE> encoded;
    if (!from_base64(value, encoded)) return {};
    DATA_BLOB input{static_cast<DWORD>(encoded.size()), encoded.data()};
    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &output)) return {};
    std::string result(reinterpret_cast<char*>(output.pbData), output.cbData);
    LocalFree(output.pbData);
    return result;
}

bool post_form(const std::string& url, const std::string& form, Json& result, std::string* err) {
    std::vector<uint8_t> body(form.begin(), form.end());
    std::vector<uint8_t> response;
    std::string network_error;
    if (!net::post(net::to_wide(url), {L"Content-Type: application/x-www-form-urlencoded"}, body,
                   response, &network_error)) {
        // Microsoft uses HTTP 400 for authorization_pending and slow_down.
        // net::post preserves its full bounded response body even for non-200
        // statuses, so parse it before considering the transport error text.
        if (!response.empty()) {
            std::string parse_error;
            result = Json::parse(std::string(response.begin(), response.end()), &parse_error);
            if (parse_error.empty()) {
                if (err) err->clear();
                return true;
            }
        }
        size_t json_start = network_error.find("{" );
        if (json_start != std::string::npos) {
            std::string parse_error;
            result = Json::parse(network_error.substr(json_start), &parse_error);
            if (parse_error.empty()) {
                if (err) err->clear();
                return true;
            }
        }
        if (err) *err = network_error;
        return false;
    }
    result = Json::parse(std::string(response.begin(), response.end()), err);
    return err == nullptr || err->empty();
}

bool post_json(const std::string& url, const Json& body, Json& result, std::string* err) {
    std::string text = body.dump();
    std::vector<uint8_t> bytes(text.begin(), text.end());
    std::vector<uint8_t> response;
    if (!net::post(net::to_wide(url), {L"Content-Type: application/json"}, bytes, response, err))
        return false;
    result = Json::parse(std::string(response.begin(), response.end()), err);
    return err == nullptr || err->empty();
}

bool finish_login(const std::string& microsoft_token, const std::string& refresh_token,
                  Account& out, std::string* err) {
    Json xbox_request = Json::obj();
    Json properties = Json::obj();
    properties.set("AuthMethod", Json::str("RPS"));
    properties.set("SiteName", Json::str("user.auth.xboxlive.com"));
    properties.set("RpsTicket", Json::str("d=" + microsoft_token));
    xbox_request.set("Properties", properties);
    xbox_request.set("RelyingParty", Json::str("http://auth.xboxlive.com"));
    xbox_request.set("TokenType", Json::str("JWT"));
    Json xbox_response;
    if (!post_json("https://user.auth.xboxlive.com/user/authenticate", xbox_request, xbox_response, err))
        return false;
    std::string xbox_token = xbox_response.get("Token").as_str();
    const Json& xbox_xui = xbox_response.get("DisplayClaims").get("xui");
    std::string uhs = xbox_xui.is(Json::Type::Arr) && xbox_xui.size() > 0
        ? xbox_xui.at(0).get("uhs").as_str() : std::string();
    if (xbox_token.empty() || uhs.empty()) {
        if (err) *err = "Xbox Live authentication returned no token";
        return false;
    }

    Json xsts_request = Json::obj();
    Json xsts_properties = Json::obj();
    xsts_properties.set("SandboxId", Json::str("RETAIL"));
    Json tokens = Json::arr();
    tokens.push(Json::str(xbox_token));
    xsts_properties.set("UserTokens", tokens);
    xsts_request.set("Properties", xsts_properties);
    xsts_request.set("RelyingParty", Json::str("rp://api.minecraftservices.com/"));
    xsts_request.set("TokenType", Json::str("JWT"));
    Json xsts_response;
    if (!post_json("https://xsts.auth.xboxlive.com/xsts/authorize", xsts_request, xsts_response, err))
        return false;
    std::string xsts_token = xsts_response.get("Token").as_str();
    const Json& xsts_xui = xsts_response.get("DisplayClaims").get("xui");
    std::string xsts_uhs = xsts_xui.is(Json::Type::Arr) && xsts_xui.size() > 0
        ? xsts_xui.at(0).get("uhs").as_str() : std::string();
    if (xsts_token.empty() || xsts_uhs.empty()) {
        if (err) *err = "XSTS authentication returned no token";
        return false;
    }

    Json mc_request = Json::obj();
    mc_request.set("identityToken", Json::str("XBL3.0 x=" + xsts_uhs + ";" + xsts_token));
    Json mc_response;
    if (!post_json("https://api.minecraftservices.com/authentication/login_with_xbox", mc_request,
                   mc_response, err)) {
        if (err && err->find("Invalid app registration") != std::string::npos) {
            *err = "Minecraft rejected this Microsoft app registration. Configure the Entra app for personal Microsoft accounts, enable public client/device-code authentication, and use its Application (client) ID in Amalgam Settings > Advanced.";
        }
        return false;
    }
    out.access_token = mc_response.get("access_token").as_str();
    int64_t expires = mc_response.get("expires_in").as_int(86400);
    out.expires_at = now_seconds() + expires;
    out.refresh_token = refresh_token;
    if (out.access_token.empty()) {
        if (err) *err = "Minecraft Services returned no access token";
        return false;
    }

    std::vector<uint8_t> profile_bytes;
    if (!net::get_with_headers(L"https://api.minecraftservices.com/minecraft/profile",
                               {L"Authorization: Bearer " + net::to_wide(out.access_token)},
                               profile_bytes, err)) return false;
    Json profile = Json::parse(std::string(profile_bytes.begin(), profile_bytes.end()), err);
    if (err && !err->empty()) return false;
    out.username = profile.get("name").as_str();
    out.uuid = profile.get("id").as_str();
    if (out.username.empty() || out.uuid.empty()) {
        if (err) *err = "Minecraft profile is unavailable (does this account own Java Edition?)";
        return false;
    }
    return true;
}

bool token_exchange(const std::string& form, Account& out, std::string* err) {
    Json token;
    if (!post_form(kTokenUrl, form, token, err)) return false;
    std::string access = token.get("access_token").as_str();
    std::string refresh = token.get("refresh_token").as_str();
    if (access.empty()) {
        if (err) *err = friendly_oauth_error(token, "Microsoft token exchange failed");
        return false;
    }
    return finish_login(access, refresh, out, err);
}

}  // namespace

bool valid_client_id(const std::string& client_id) {
    if (client_id.size() != 36) return false;
    for (size_t i = 0; i < client_id.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (client_id[i] != '-') return false;
        } else if (!std::isxdigit(static_cast<unsigned char>(client_id[i]))) {
            return false;
        }
    }
    return true;
}

bool login_device(Account& out, const std::string& client_id, const Log& log, std::string* err,
                  const DevicePrompt& prompt, bool open_browser) {
    if (!valid_client_id(client_id)) {
        if (err) *err = client_id_problem(client_id);
        return false;
    }
    out = {};
    out.microsoft_client_id = client_id;
    std::string form = "client_id=" + url_encode(client_id) +
                       "&scope=" + url_encode("XboxLive.signin offline_access");
    Json device;
    if (!post_form(kDeviceUrl, form, device, err)) return false;
    std::string device_code = device.get("device_code").as_str();
    std::string user_code = device.get("user_code").as_str();
    std::string uri = device.get("verification_uri").as_str("https://microsoft.com/devicelogin");
    int interval = static_cast<int>(device.get("interval").as_int(5));
    int expires = static_cast<int>(device.get("expires_in").as_int(900));
    if (device_code.empty() || user_code.empty()) {
        if (err) *err = friendly_oauth_error(device, "Microsoft device login failed");
        return false;
    }
    if (prompt) prompt(DeviceLoginPrompt{uri, user_code, expires});
    if (log) log(L"Open " + net::to_wide(uri) + L" and enter code " + net::to_wide(user_code));
    if (open_browser)
        ShellExecuteW(nullptr, L"open", net::to_wide(uri).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    for (int elapsed = 0; elapsed < expires; elapsed += interval) {
        std::this_thread::sleep_for(std::chrono::seconds(interval));
        Json token;
        std::string poll_form = "grant_type=urn:ietf:params:oauth:grant-type:device_code&client_id=" +
                                url_encode(client_id) + "&device_code=" + url_encode(device_code);
        if (!post_form(kTokenUrl, poll_form, token, err)) return false;
        std::string token_error = token.get("error").as_str();
        if (token_error == "authorization_pending") continue;
        if (token_error == "slow_down") {
            interval += 5;
            continue;
        }
        std::string access = token.get("access_token").as_str();
        if (access.empty()) {
            if (err) *err = friendly_oauth_error(token, "Microsoft login failed");
            return false;
        }
        std::string refresh = token.get("refresh_token").as_str();
        return finish_login(access, refresh, out, err);
    }
    if (err) *err = "Microsoft device login timed out";
    return false;
}

bool load(Account& out, std::string* err) {
    out = {};
    std::wstring path = account_path();
    if (path.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    Json j;
    std::string parse_error;
    if (!json_parse_file(path, j, &parse_error)) {
        if (err) *err = parse_error;
        return false;
    }
    out.username = j.get("username").as_str();
    out.uuid = j.get("uuid").as_str();
    out.expires_at = j.get("expires_at").as_int(0);
    out.access_token = unprotect(j.get("access_token").as_str());
    out.refresh_token = unprotect(j.get("refresh_token").as_str());
    out.microsoft_client_id = j.get("microsoft_client_id").as_str();
    if (out.username.empty() || out.uuid.empty() || out.access_token.empty()) {
        if (err) *err = "stored account is invalid or unavailable";
        return false;
    }
    return true;
}

bool save(const Account& account, std::string* err) {
    std::wstring path = account_path();
    if (path.empty()) {
        if (err) *err = "LOCALAPPDATA is unavailable";
        return false;
    }
    std::string access = protect(account.access_token);
    std::string refresh = protect(account.refresh_token);
    if (access.empty() || refresh.empty()) {
        if (err) *err = "Windows account protection failed";
        return false;
    }
    Json j = Json::obj();
    j.set("username", Json::str(account.username));
    j.set("uuid", Json::str(account.uuid));
    j.set("expires_at", Json::num(static_cast<double>(account.expires_at)));
    j.set("access_token", Json::str(access));
    j.set("refresh_token", Json::str(refresh));
    j.set("microsoft_client_id", Json::str(account.microsoft_client_id));
    std::string write_error;
    if (!json_write_file(path, j, &write_error)) {
        if (err) *err = write_error;
        return false;
    }
    return true;
}

bool logout(std::string* err) {
    std::wstring path = account_path();
    if (path.empty() || DeleteFileW(path.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND) return true;
    if (err) *err = "cannot remove stored account";
    return false;
}

bool ensure_valid(Account& account, const Log& log, std::string* err) {
    if (!load(account, err)) return false;
    if (account.expires_at > now_seconds() + 300) return true;
    if (account.refresh_token.empty()) {
        if (err) *err = "stored account has expired and no refresh token";
        return false;
    }
    if (!valid_client_id(account.microsoft_client_id)) {
        if (err) *err = "This saved Minecraft account needs to be connected again because its Microsoft launcher registration is no longer available.";
        return false;
    }
    std::string form = "grant_type=refresh_token&client_id=" + url_encode(account.microsoft_client_id) +
                       "&refresh_token=" + url_encode(account.refresh_token) +
                       "&scope=" + url_encode("XboxLive.signin offline_access");
    Account refreshed;
    if (!token_exchange(form, refreshed, err)) return false;
    refreshed.microsoft_client_id = account.microsoft_client_id;
    account = refreshed;
    if (log) log(L"Minecraft account token refreshed for " + net::to_wide(account.username));
    return save(account, err);
}

}  // namespace aml::auth
