#pragma once

#include <string>
#include <vector>
#include <map>
#include <ctime>
#include <random>

namespace aml::supabase {
struct SupabaseSecuritySettings;
}

namespace aml::account {

// ---------------------------------------------------------------------------
// Account Session
// ---------------------------------------------------------------------------

struct AccountSession {
    std::string id;
    std::string email;
    std::string access_token;
    std::string refresh_token;
    int64_t expires_at = 0;
    int64_t created_at = 0;
    int64_t last_used = 0;
    
    bool is_expired() const {
        return std::time(nullptr) > expires_at;
    }
    
    int64_t time_until_expiry() const {
        return expires_at - std::time(nullptr);
    }
};

// ---------------------------------------------------------------------------
// Account Profile
// ---------------------------------------------------------------------------

struct AccountProfile {
    std::string email;
    std::string username;
    std::string display_name;
    std::string avatar_url;
    std::string bio;
    std::string theme = "system"; // system, light, dark
    std::string language = "en";
    bool receive_newsletter = true;
    bool enable_beta_features = false;
    int64_t created_at = 0;
    int64_t last_login_at = 0;
};

// ---------------------------------------------------------------------------
// Account Statistics
// ---------------------------------------------------------------------------

struct AccountStats {
    int total_sessions = 0;
    std::string current_session_id;
    int64_t account_created_at = 0;
    int64_t last_login_at = 0;
    int total_instances = 0;
    int total_modpacks = 0;
    int total_servers = 0;
    int64_t total_playtime = 0;
};

// ---------------------------------------------------------------------------
// Account Activity
// ---------------------------------------------------------------------------

struct AccountActivity {
    std::string id;
    std::string type; // login, logout, launch, install, etc.
    std::string description;
    int64_t timestamp = 0;
    std::string ip_address;
    std::string user_agent;
};

// ---------------------------------------------------------------------------
// Security Settings
// ---------------------------------------------------------------------------

struct SecuritySettings {
    bool two_factor_enabled = false;
    std::string two_factor_method; // email, auth_app, sms
    std::vector<std::string> trusted_devices;
    std::vector<std::string> recent_logins;
    int64_t last_password_change = 0;
};

// ---------------------------------------------------------------------------
// Account Manager
// ---------------------------------------------------------------------------

class AccountManager {
public:
    static AccountManager& instance();
    
    // Session Management
    bool create_session(const std::string& email, const std::string& access_token,
                       const std::string& refresh_token, int64_t expires_at);
    bool set_current_session(const std::string& session_id);
    bool end_session(const std::string& session_id);
    bool end_current_session();
    bool end_all_sessions();
    AccountSession get_current_session() const;
    std::vector<AccountSession> get_all_sessions() const;
    bool has_valid_session() const;
    bool refresh_current_session();
    
    // Profile Management
    bool update_profile(const AccountProfile& profile);
    AccountProfile get_profile() const;
    
    // Security Management
    bool change_password(const std::string& current_password, const std::string& new_password);
    bool request_password_reset(const std::string& email);
    bool enable_2fa(const std::string& code);
    bool disable_2fa(const std::string& code);
    bool verify_2fa_code(const std::string& code);
    aml::supabase::SupabaseSecuritySettings get_security_settings() const;
    aml::supabase::SupabaseSecuritySettings get_security_settings(std::string* error) const;
    
    // Linked Accounts
    bool link_microsoft_account(const std::string& microsoft_token);
    bool unlink_microsoft_account();
    bool is_microsoft_linked() const;
    std::string get_microsoft_token() const;
    
    // Statistics and Activity
    AccountStats get_account_stats() const;
    std::vector<AccountActivity> get_recent_activity() const;
    // The passive Account screen snapshots session and identity inputs on the
    // UI thread, then supplies those copies here from a joined worker.  That
    // avoids a worker reading mutable local-session state while a user signs
    // out or switches accounts. refresh_error reports an unavailable remote
    // source or an incomplete local fallback scan without fabricating an
    // empty activity history.
    std::vector<AccountActivity> get_recent_activity(
        const std::string& expected_user_id,
        const std::vector<AccountSession>& local_sessions,
        const std::wstring& instances_dir,
        std::string* refresh_error,
        bool* used_local_fallback) const;
    
    // Account Deletion
    bool request_account_deletion();
    bool confirm_account_deletion(const std::string& confirmation_code);
    
    // State
    bool is_authenticated() const { return has_valid_session(); }
    std::string get_current_email() const;
    std::string get_current_username() const;

private:
    AccountManager();
    ~AccountManager();
    
    // Prevent copying
    AccountManager(const AccountManager&) = delete;
    AccountManager& operator=(const AccountManager&) = delete;
    
    // Session Management
    std::string generate_session_id() const;
    bool save_sessions() const;
    bool load_sessions();
    
    // Data Protection
    std::string protect_data(const std::string& data) const;
    std::string unprotect_data(const std::string& data) const;
    
    // State
    std::vector<AccountSession> sessions_;
    std::string current_session_id_;
    std::string microsoft_token_;
};

}  // namespace aml::account
