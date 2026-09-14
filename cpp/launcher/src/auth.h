#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace aml::auth {

struct Account {
    std::string email;
    std::string display_name;
    std::string username;
    std::string uuid;
    std::string access_token;
    std::string refresh_token;
    // Refresh tokens are bound to the public application that requested them.
    // Persist the ID used at sign-in so a later launcher setting change does
    // not silently break an already-connected account.
    std::string microsoft_client_id;
    int64_t expires_at = 0;
};

using Log = std::function<void(const std::wstring&)>;

// Data shown by a caller-owned sign-in surface.  Keeping the device code
// separate from the log stream lets the launcher present a clear, guided
// Microsoft sign-in flow without parsing a human-readable diagnostic string.
struct DeviceLoginPrompt {
    std::string verification_uri;
    std::string user_code;
    int expires_in = 0;
};

using DevicePrompt = std::function<void(const DeviceLoginPrompt&)>;

// Microsoft assigns each registered application a GUID-shaped Application
// (client) ID.  A launcher must use its own registration; borrowed client IDs
// are rejected or can be revoked without notice.
bool valid_client_id(const std::string& client_id);

// Device-code sign-in. The caller-owned prompt surface receives the
// verification URI and one-time code; the browser is only opened when the
// caller asks for it (open_browser), so a GUI flow can let the user copy the
// code before the sign-in page appears.
bool login_device(Account& out, const std::string& client_id, const Log& log, std::string* err,
                  const DevicePrompt& prompt = {}, bool open_browser = true);
bool load(Account& out, std::string* err = nullptr);
bool save(const Account& account, std::string* err = nullptr);
bool logout(std::string* err = nullptr);
bool ensure_valid(Account& account, const Log& log, std::string* err);

}  // namespace aml::auth
