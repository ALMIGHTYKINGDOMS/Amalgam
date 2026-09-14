#include "ui.h"
#include "ui_internal.h"
#include "ui_model.h"
#include "account_manager.h"
#include "supabase.h"
#include "auth.h"
#include "net.h"
#include "entitlements.h"
#include "online_config.h"

#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <cctype>

namespace aml::ui {

// ---------------------------------------------------------------------------
// Account UI State
// ---------------------------------------------------------------------------

struct AccountUIState {
    int current_tab = 0; // 0=profile, 1=settings, 2=security, 3=sessions, 4=activity
    
    // Profile editing
    account::AccountProfile editing_profile;
    bool profile_editing = false;
    std::string profile_error;
    std::string profile_success;
    
    // Settings
    std::string theme;
    std::string language;
    bool receive_newsletter = true;
    bool enable_beta_features = false;
    bool social_notifications = true;
    bool social_show_offline = true;
    bool social_auto_accept = false;
    bool settings_initialized = false;
    std::string settings_profile_identity;
    
    // Security
    std::string current_password;
    std::string new_password;
    std::string confirm_password;
    std::string security_error;
    std::string security_success;
    bool changing_password = false;
    bool enabling_2fa = false;
    bool disabling_2fa = false;
    std::string two_factor_code;
    std::string two_factor_error;
    
    // Sessions
    std::string session_to_end;
    bool confirming_session_end = false;
    
    // Activity
    int activity_page = 0;
    const int activity_per_page = 20;
};

static AccountUIState& get_account_ui_state(UiState& /*st*/) {
    static AccountUIState state;
    return state;
}

struct SignOutResult {
    bool remote = false;
    bool local = false;
};

void clear_account_identity(UiState& st) {
    st.account.username.clear();
    st.account.email.clear();
    st.account.display_name.clear();
}

SignOutResult sign_out_current_account(UiState& st,
                                       aml::account::AccountManager& account_manager) {
    SignOutResult result;
    result.local = account_manager.end_current_session();
    result.remote = aml::supabase::SupabaseManager::instance().sign_out();
    aml::entitlements::EntitlementManager::instance().invalidate();
    clear_account_identity(st);
    return result;
}

SignOutResult sign_out_all_local_accounts(UiState& st,
                                          aml::account::AccountManager& account_manager) {
    SignOutResult result;
    result.local = account_manager.end_all_sessions();
    result.remote = aml::supabase::SupabaseManager::instance().sign_out();
    aml::entitlements::EntitlementManager::instance().invalidate();
    clear_account_identity(st);
    return result;
}

std::string sign_out_failure_message(const SignOutResult& result) {
    if (result.local && !result.remote) {
        return "Local session was cleared, but remote sign-out could not be confirmed";
    }
    if (!result.local && result.remote) {
        return "Remote sign-out completed, but local session cleanup could not be confirmed";
    }
    return "Remote sign-out and local session cleanup could not be confirmed";
}

// ---------------------------------------------------------------------------
// Account Overview Page
// ---------------------------------------------------------------------------

void draw_account_overview(UiState& st) {
    auto& account_state = get_account_ui_state(st);
    auto& account_manager = aml::account::AccountManager::instance();
    
    page_title("Account Overview", "Manage your Amalgam account and preferences");
    
    // Account card
    card_begin("##account_card");
    const bool compact_account_header =
        ImGui::GetContentRegionAvail().x < ui_px(760.0f);
    
    auto profile = account_manager.get_profile();
    auto stats = account_manager.get_account_stats();
    
    // Avatar and info
    ImGui::BeginGroup();
    
    // Avatar
    if (!profile.avatar_url.empty()) {
        ImVec2 avatar_pos = ImGui::GetCursorScreenPos();
        draw_project_image(st, profile.avatar_url,
                           avatar_pos, ImVec2(ui_px(80.0f), ui_px(80.0f)), c32(k.brand));
        ImGui::Dummy(ImVec2(ui_px(80.0f), ui_px(80.0f)));
    } else {
        // Default avatar
        ImVec2 avatar_pos = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddCircleFilled(avatar_pos + ImVec2(ui_px(40.0f), ui_px(40.0f)), ui_px(40.0f), c32(k.brand));
        draw_brand_mark(dl, avatar_pos + ImVec2(ui_px(40.0f), ui_px(40.0f)), ui_px(0.5f));
        ImGui::Dummy(ImVec2(ui_px(80.0f), ui_px(80.0f)));
    }
    
    ImGui::EndGroup();
    if (!compact_account_header) ImGui::SameLine();
    else ImGui::Spacing();
    
    // Account info
    ImGui::BeginGroup();
    ImGui::PushFont(f_title);
    ImGui::Text("%s", profile.display_name.empty() ? profile.username.c_str() : profile.display_name.c_str());
    ImGui::PopFont();
    ImGui::TextColored(k.muted, "%s", profile.email.c_str());
    
    // Keep Amalgam account state separate from Minecraft authentication.
    ImGui::Spacing();
    ImGui::TextColored(k.green, "✓ Amalgam Account Connected");
    ImGui::TextColored(k.muted,
                       "Minecraft sign-in is handled by the official Minecraft Launcher when you play.");
    
    ImGui::EndGroup();
    
    if (!compact_account_header) ImGui::SameLine();
    else ImGui::Spacing();
    
    // Quick actions
    ImGui::BeginGroup();
    if (ghost_button("Edit Profile", ImVec2(ui_px(120.0f), ui_px(32.0f)))) {
        account_state.current_tab = 0;
        account_state.profile_editing = true;
        account_state.editing_profile = profile;
    }
    if (ghost_button("Account Settings", ImVec2(ui_px(120.0f), ui_px(32.0f)))) {
        account_state.current_tab = 1;
    }
    if (ghost_button("Security", ImVec2(ui_px(120.0f), ui_px(32.0f)))) {
        account_state.current_tab = 2;
    }
    ImGui::EndGroup();
    
    card_end();
    
    ImGui::Spacing();
    
    // Statistics
    card_begin("##account_stats");
    ImGui::TextUnformatted("Account Statistics");
    ImGui::Separator();
    ImGui::Spacing();
    const float available_stats = ImGui::GetContentRegionAvail().x;
    const float stat_gap = ui_px(10.0f);
    const int stat_columns = ui_model::stat_card_columns(available_stats, g_ui_scale);
    const float stat_w = std::max(ui_px(120.0f),
        (available_stats - stat_gap * static_cast<float>(stat_columns - 1)) /
            static_cast<float>(stat_columns));
    const std::string sessions_value = std::to_string(stats.total_sessions);
    const std::string created_value = format_date(stats.account_created_at);
    const std::string login_value = format_date(stats.last_login_at);
    const char* current_value = stats.current_session_id.empty() ? "None" : "Active";
    struct AccountStat { const char* label; const char* value; ImVec4 accent; };
    const AccountStat account_stats[] = {
        {"Sessions", sessions_value.c_str(), k.brand},
        {"Created", created_value.c_str(), k.blue},
        {"Last Login", login_value.c_str(), k.green},
        {"Current", current_value,
         stats.current_session_id.empty() ? k.muted : k.green},
    };
    for (int i = 0; i < 4; ++i) {
        if (i > 0 && i % stat_columns != 0) ImGui::SameLine(0, stat_gap);
        draw_stat_card(account_stats[i].label, account_stats[i].value,
                       -1.0f, account_stats[i].accent, stat_w);
    }
    card_end();
    
    ImGui::Spacing();
    
    // ── Membership & entitlements ─────────────────────────────────
    {
        auto& ents = aml::entitlements::EntitlementManager::instance();
        const auto e = ents.snapshot();
        const bool plus = e.valid && ents.is_plus();
        const bool entitlement_known = e.valid;

        // Request a refresh from Supabase (Whop-synced subscription data)
        // if we have no cached data yet. The manager throttles by interval.
        if (!e.valid && !ents.refreshing()) {
            if (aml::supabase::SupabaseManager::instance().is_authenticated()) {
                ents.request_refresh_from_supabase();
            } else {
                const std::string token = account_manager.get_current_session().access_token;
                if (!token.empty()) ents.request_refresh(token);
            }
        }

        card_begin("##account_membership");
        ImVec2 mem_card_min = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // Top accent strip
        ImVec4 accent = plus ? k.brand : k.surface2;
        dl->AddRectFilled(mem_card_min,
            ImVec2(mem_card_min.x + ImGui::GetContentRegionAvail().x, mem_card_min.y + ui_px(4.0f)),
            c32(accent), ui_px(2.0f));
        ImGui::Dummy(ImVec2(0, ui_px(8.0f)));

        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("MEMBERSHIP");
        ImGui::PopFont();
        ImGui::Spacing();

        // ── Plan header ─────────────────────────────────────────────
        const char* plan_label = !entitlement_known
            ? "CHECKING MEMBERSHIP"
            : (e.plan_label.empty() ? (plus ? "AMALGAM+" : "FREE") : e.plan_label.c_str());
        ImGui::PushFont(f_title);
        ImGui::TextColored(plus ? k.brand : k.text, "%s", plan_label);
        ImGui::PopFont();
        ImGui::SameLine(0, ui_px(12.0f));

        if (plus) {
            dl->AddRectFilled(ImGui::GetCursorScreenPos(),
                ImVec2(ImGui::GetCursorScreenPos().x + ui_px(60.0f), ImGui::GetCursorScreenPos().y + ui_px(20.0f)),
                c32(k.green), ui_px(4.0f));
            ImGui::PushFont(f_small);
            dl->AddText(ImVec2(ImGui::GetCursorScreenPos().x + ui_px(8.0f), ImGui::GetCursorScreenPos().y + ui_px(3.0f)),
                IM_COL32(255,255,255,255), "ACTIVE");
            ImGui::PopFont();
            ImGui::Dummy(ImVec2(ui_px(60.0f), ui_px(20.0f)));
        }

        if (entitlement_known) {
            ImGui::TextColored(k.muted, "$%.2f/month", plus ? 19.99 : 0.0);
        } else {
            ImGui::TextColored(k.muted, "Membership status is loading");
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // ── ESSENTIALS RELAY section ─────────────────────────────────
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("ESSENTIALS RELAY");
        ImGui::PopFont();
        ImGui::Spacing();

        if (e.turn_monthly_bytes > 0) {
            const uint64_t used = e.turn_used_bytes;
            const uint64_t total = e.turn_monthly_bytes;
            const uint64_t remaining = total > used ? total - used : 0;
            const float frac = total > 0
                ? static_cast<float>(static_cast<double>(used) / total) : 0.0f;
            const float gb = 1024.0f * 1024.0f * 1024.0f;

            // Used / Total with progress bar
            ImGui::TextColored(k.text, "%.1f / %.1f GB", used / gb, total / gb);
            ImGui::Spacing();

            ImVec4 meter_color = frac >= 1.0f ? k.red :
                                 frac >= 0.9f ? k.red :
                                 frac >= 0.75f ? k.yellow : k.green;
            progress_bar(frac, ImVec2(ui_px(340.0f), ui_px(14.0f)),
                         nullptr, &meter_color);
            ImGui::Spacing();

            ImGui::TextColored(k.muted, "%.1f GB remaining", remaining / gb);

            // Reset date
            if (e.turn_reset_at > 0) {
                std::time_t reset_time = static_cast<std::time_t>(e.turn_reset_at);
                std::tm reset_tm{};
                localtime_s(&reset_tm, &reset_time);
                char reset_buf[64]{};
                std::strftime(reset_buf, sizeof(reset_buf), "%B %d", &reset_tm);
                ImGui::SameLine(ui_px(200.0f));
                ImGui::TextColored(k.muted, "Resets %s", reset_buf);
            }

            // Warnings
            ImGui::Spacing();
            if (frac >= 1.0f) {
                ImGui::TextColored(k.red, "Relay limit reached — direct P2P still works.");
            } else if (frac >= 0.9f) {
                ImGui::TextColored(k.red, "Almost at monthly relay limit.");
            } else if (frac >= 0.75f) {
                ImGui::TextColored(k.yellow, "Relay usage high.");
            }
        } else {
            ImGui::TextColored(k.muted, "No relay data available.");
            if (plus)
                ImGui::TextColored(k.muted, "Relay entitlement will appear after your first Essentials connection.");
        }

        // ── CLOUD SERVERS section ────────────────────────────────────
        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("CLOUD SERVERS");
        ImGui::PopFont();
        ImGui::Spacing();

        if (!entitlement_known) {
            ImGui::TextColored(k.muted, "Hosted-server details are loading from your account.");
        } else if (e.cloud_servers_used > 0) {
            ImGui::TextColored(k.text, "%d hosted server%s linked to this account",
                               e.cloud_servers_used, e.cloud_servers_used == 1 ? "" : "s");
            ImGui::TextColored(k.muted,
                               "Plan names, billing, and server controls are managed on the website.");
        } else {
            ImGui::TextColored(k.muted, "No hosted servers linked to this account.");
            ImGui::TextColored(k.muted,
                               "Create and manage Amalgam Cloud servers on the Amalgam website.");
        }

        ImGui::Spacing();

        // ── Action buttons ───────────────────────────────────────────
        if (plus) {
            if (ghost_button("Manage Membership", ImVec2(ui_px(170.0f), ui_px(32.0f)))) {
                ShellExecuteW(st.hwnd, L"open",
                    aml::net::to_wide(aml::online::config().manage_membership_url()).c_str(),
                    nullptr, nullptr, SW_SHOWNORMAL);
            }
        } else {
            if (primary_button(entitlement_known ? "View Amalgam+ Plans" : "View Membership Plans", ImVec2(ui_px(190.0f), ui_px(32.0f)))) {
                ShellExecuteW(st.hwnd, L"open",
                    aml::net::to_wide(aml::online::config().plans_url()).c_str(),
                    nullptr, nullptr, SW_SHOWNORMAL);
            }
        }
        if (entitlement_known) {
            ImGui::SameLine(0, ui_px(6.0f));
            if (ghost_button("Manage Cloud on Website", ImVec2(ui_px(190.0f), ui_px(32.0f)))) {
                ShellExecuteW(st.hwnd, L"open",
                    aml::net::to_wide(aml::online::config().cloud_account_url()).c_str(),
                    nullptr, nullptr, SW_SHOWNORMAL);
            }
        }
        if (plus && e.turn_monthly_bytes > 0 &&
            static_cast<float>(static_cast<double>(e.turn_used_bytes) / e.turn_monthly_bytes) >= 0.75f) {
            ImGui::SameLine(0, ui_px(6.0f));
            if (ghost_button("Review Relay Options", ImVec2(ui_px(170.0f), ui_px(32.0f)))) {
                ShellExecuteW(st.hwnd, L"open",
                    aml::net::to_wide(aml::online::config().plans_url()).c_str(),
                    nullptr, nullptr, SW_SHOWNORMAL);
            }
        }
        card_end();
    }
    
    ImGui::Spacing();
    
    // ── Plan comparison ──────────────────────────────────────────
    {
        auto& ents2 = aml::entitlements::EntitlementManager::instance();
        const auto e2 = ents2.snapshot();
        const bool plus2 = ents2.is_plus();

        card_begin("##plan_comparison", ImVec2(-1, 0));
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("PLAN COMPARISON");
        ImGui::PopFont();
        ImGui::Spacing();

        // Feature comparison table
        struct Feature { const char* name; bool free_val; bool plus_val; };
        const Feature features[] = {
            {"Launcher",                          true,  true},
            {"Accounts",                          true,  true},
            {"Java Management",                   true,  true},
            {"Mods & Modpacks",                   true,  true},
            {"Local Servers",                     true,  true},
            {"Basic Essentials",                  true,  true},
            {"Direct P2P",                        true,  true},
            {"Basic TURN Allowance",              true,  true},
            {"No Ads",                            false, true},
            {"80 GB Monthly TURN",                false, true},
            {"Premium Essentials",                false, true},
            {"Cloud Profile Sync",                false, true},
            {"Cloud Settings Sync",               false, true},
            {"Premium Themes",                    false, true},
            {"Advanced Profile Tools",            false, true},
            {"Early Access",                      false, true},
            {"Amalgam+ Badge",                    false, true},
            {"Priority Support",                  false, true},
        };

        const float avail = ImGui::GetContentRegionAvail().x;
        const float col1 = ui_px(240.0f);  // Feature name
        const float col2 = avail * 0.35f;  // Free
        // col3 = rest                        // Amalgam+

        // Header
        ImGui::PushFont(f_bold);
        ImGui::TextColored(k.muted, "Feature");
        ImGui::SameLine(col1);
        ImGui::TextColored(k.muted, "FREE ($0)");
        ImGui::SameLine(col1 + col2);
        ImGui::TextColored(k.brand, "AMALGAM+ ($19.99/mo)");
        ImGui::PopFont();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::PushFont(f_small);
        for (const auto& feat : features) {
            // Highlight current plan's column
            ImVec4 free_color = feat.free_val ? k.green : k.muted;
            ImVec4 plus_color = feat.plus_val ? k.green : k.muted;
            const char* free_text = feat.free_val ? "\xe2\x9c\x93" : "\xe2\x80\x94";
            const char* plus_text = feat.plus_val ? "\xe2\x9c\x93" : "\xe2\x80\x94";

            ImGui::TextColored(k.text, "%s", feat.name);
            ImGui::SameLine(col1);
            ImGui::TextColored(free_color, "%s", free_text);
            ImGui::SameLine(col1 + col2);
            ImGui::TextColored(plus_color, "%s", plus_text);
        }
        ImGui::PopFont();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (!e2.valid || !plus2) {
            if (primary_button(e2.valid ? "Upgrade to Amalgam+" : "View Membership Plans", ImVec2(ui_px(190.0f), ui_px(32.0f)))) {
                ShellExecuteW(st.hwnd, L"open",
                    aml::net::to_wide(aml::online::config().plans_url()).c_str(),
                    nullptr, nullptr, SW_SHOWNORMAL);
            }
        } else {
            ImGui::TextColored(k.green, "You're on Amalgam+ — thank you!");
        }
        card_end();
    }
    
    ImGui::Spacing();

    // ── TURN Add-ons ──────────────────────────────────────────────
    {
        auto& ents3 = aml::entitlements::EntitlementManager::instance();
        const auto e3 = ents3.snapshot();
        if (ents3.is_plus() && e3.turn_monthly_bytes > 0) {
            card_begin("##turn_addons", ImVec2(-1, 0));
            ImGui::PushFont(f_h2);
            ImGui::TextUnformatted("RELAY DATA ADD-ONS");
            ImGui::PopFont();
            ImGui::TextColored(k.muted,
                               "Additional relay capacity is not activated in the launcher yet.");
            ImGui::TextColored(k.muted,
                               "When metering and billing are available, options will be managed on the website.");
            ImGui::Spacing();
            if (ghost_button("Review Website Plans", ImVec2(ui_px(160.0f), ui_px(30.0f)))) {
                ShellExecuteW(st.hwnd, L"open",
                    aml::net::to_wide(aml::online::config().plans_url()).c_str(),
                    nullptr, nullptr, SW_SHOWNORMAL);
            }
            card_end();
            ImGui::Spacing();
        }
    }

    // Quick actions
    card_begin("##account_actions");
    ImGui::TextUnformatted("Quick Actions");
    ImGui::Separator();
    
    if (st.account.username.empty() && !account_manager.is_microsoft_linked()) {
        if (ghost_button("Connect Minecraft Account", ImVec2(ui_px(220.0f), ui_px(36.0f)))) {
            start_microsoft_login(st);
        }
        ImGui::SameLine();
        ImGui::TextColored(k.muted, "Optional here; the official launcher handles sign-in at Play.");
    }
    
    ImGui::Spacing();
    
    if (ghost_button("Sign Out", ImVec2(ui_px(120.0f), ui_px(32.0f)))) {
        const auto result = sign_out_current_account(st, account_manager);
        if (result.local && result.remote) {
            push_notice(st, ui_model::NoticeLevel::Success, "Signed Out",
                        "You have been signed out");
        } else {
            push_notice(st, ui_model::NoticeLevel::Error, "Sign Out Incomplete",
                        sign_out_failure_message(result));
        }
    }
    
    ImGui::SameLine();
    // Supabase cannot invalidate sessions on other devices from this client.
    // Keep that limitation visible without presenting a dead button; the
    // account website is the authoritative place for remote session control.
    ImGui::TextColored(k.muted, "Other-device sessions are managed on the website.");
    ImGui::SameLine(0, ui_px(8.0f));
    if (ghost_button("Manage Sessions", ImVec2(ui_px(135.0f), ui_px(32.0f)))) {
        ShellExecuteW(st.hwnd, L"open",
            aml::net::to_wide(aml::online::config().page_url("account/sessions")).c_str(),
            nullptr, nullptr, SW_SHOWNORMAL);
    }
    
    card_end();
}

// ---------------------------------------------------------------------------
// Profile Management
// ---------------------------------------------------------------------------

void draw_account_profile(UiState& st) {
    auto& account_state = get_account_ui_state(st);
    auto& account_manager = aml::account::AccountManager::instance();
    
    page_title("Profile Settings", "Update your personal information");
    
    if (!account_state.profile_editing) {
        // Display profile
        auto profile = account_manager.get_profile();
        
        card_begin("##profile_view");
        
        ImGui::TextUnformatted("Your Profile");
        ImGui::Separator();
        ImGui::Spacing();
        
        // Avatar
        ImGui::TextUnformatted("Avatar");
        ImGui::TextColored(k.muted, "Upload or change your profile picture");
        ImGui::Spacing();
        
        // Basic info
        ImGui::TextUnformatted("Email");
        ImGui::Text("%s", profile.email.c_str());
        ImGui::TextColored(k.muted, "Primary email address - cannot be changed");
        ImGui::Spacing();
        
        ImGui::TextUnformatted("Username");
        ImGui::Text("%s", profile.username.c_str());
        ImGui::TextColored(k.muted, "Your unique Amalgam username");
        ImGui::Spacing();
        
        ImGui::TextUnformatted("Display Name");
        ImGui::Text("%s", profile.display_name.empty() ? "Not set" : profile.display_name.c_str());
        ImGui::TextColored(k.muted, "How you appear to other users");
        ImGui::Spacing();
        
        ImGui::TextUnformatted("Bio");
        ImGui::Text("%s", profile.bio.empty() ? "Not set" : profile.bio.c_str());
        ImGui::Spacing();
        
        if (ghost_button("Edit Profile", ImVec2(ui_px(120.0f), ui_px(32.0f)))) {
            account_state.editing_profile = profile;
            account_state.profile_editing = true;
        }
        
        card_end();
        
        ImGui::Spacing();
        
        // Account creation info
        card_begin("##account_info");
        ImGui::TextUnformatted("Account Information");
        ImGui::Separator();
        
        ImGui::TextColored(k.muted, "Account Created");
        ImGui::Text("%s", format_date(profile.created_at).c_str());
        ImGui::Spacing();
        
        ImGui::TextColored(k.muted, "Last Login");
        ImGui::Text("%s", format_date(profile.last_login_at).c_str());
        
        card_end();
    } else {
        // Edit profile
        card_begin("##profile_edit");
        
        ImGui::TextUnformatted("Edit Profile");
        ImGui::Separator();
        ImGui::Spacing();
        
        // Display Name
        ImGui::TextUnformatted("Display Name");
        ImGui::SetNextItemWidth(ui_px(300.0f));
        input_text("##edit_display_name", &account_state.editing_profile.display_name);
        ImGui::TextColored(k.muted, "How you'll appear to other Amalgam users");
        ImGui::Spacing();
        
        // Bio
        ImGui::TextUnformatted("Bio");
        ImGui::SetNextItemWidth(ui_px(300.0f));
        ImGui::InputTextMultiline("##edit_bio", &account_state.editing_profile.bio,
                                  ImVec2(ui_px(400.0f), ui_px(100.0f)));
        ImGui::TextColored(k.muted, "Tell other users about yourself");
        ImGui::Spacing();
        
        // Avatar URL
        ImGui::TextUnformatted("Avatar URL");
        ImGui::SetNextItemWidth(ui_px(300.0f));
        input_text("##edit_avatar_url", &account_state.editing_profile.avatar_url);
        ImGui::TextColored(k.muted, "URL to your profile picture");
        ImGui::Spacing();
        
        // Error and success messages
        if (!account_state.profile_error.empty()) {
            ImGui::TextColored(k.red, "%s", account_state.profile_error.c_str());
            ImGui::Spacing();
        }
        
        if (!account_state.profile_success.empty()) {
            ImGui::TextColored(k.green, "%s", account_state.profile_success.c_str());
            ImGui::Spacing();
        }
        
        ImGui::Spacing();
        ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - ui_px(200.0f));
        
        if (ghost_button("Cancel", ImVec2(ui_px(80.0f), ui_px(32.0f)))) {
            account_state.profile_editing = false;
            account_state.profile_error.clear();
            account_state.profile_success.clear();
        }
        
        ImGui::SameLine();
        if (primary_button("Save Changes", ImVec2(ui_px(120.0f), ui_px(32.0f)))) {
            if (account_manager.update_profile(account_state.editing_profile)) {
                account_state.profile_success = "Profile updated successfully!";
                account_state.profile_error.clear();
                account_state.profile_editing = false;
            } else {
                account_state.profile_error = "Failed to update profile";
                account_state.profile_success.clear();
            }
        }
        
        card_end();
    }
}

// ---------------------------------------------------------------------------
// Account Settings
// ---------------------------------------------------------------------------

static std::string normalize_theme_code(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (value == "light" || value == "dark" || value == "system") return value;
    return "system";
}

static std::string normalize_language_code(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (value == "en" || value == "es" || value == "fr" || value == "de" ||
        value == "zh" || value == "ja") return value;
    return "en";
}

void draw_account_settings(UiState& st) {
    auto& account_state = get_account_ui_state(st);
    auto& account_manager = aml::account::AccountManager::instance();
    
    page_title("Account Settings", "Configure your preferences");
    
    const auto profile = account_manager.get_profile();
    const std::string profile_identity = profile.email.empty() ? profile.username : profile.email;
    if (!account_state.settings_initialized ||
        account_state.settings_profile_identity != profile_identity) {
        account_state.theme = normalize_theme_code(profile.theme);
        account_state.language = normalize_language_code(profile.language);
        account_state.receive_newsletter = profile.receive_newsletter;
        account_state.enable_beta_features = profile.enable_beta_features;
        if (st.cfg) {
            account_state.social_notifications = st.cfg->social_notifications;
            account_state.social_show_offline = st.cfg->social_show_offline;
            account_state.social_auto_accept = st.cfg->social_auto_accept;
        }
        account_state.settings_profile_identity = profile_identity;
        account_state.settings_initialized = true;
    }
    
    card_begin("##settings_general");
    ImGui::TextUnformatted("General Settings");
    ImGui::Separator();
    ImGui::Spacing();
    
    // Theme
    ImGui::TextUnformatted("Theme");
    const char* themes[] = { "System", "Light", "Dark" };
    const char* theme_codes[] = { "system", "light", "dark" };
    int current_theme = 0;
    for (int i = 0; i < 3; ++i) {
        if (account_state.theme == theme_codes[i]) {
            current_theme = i;
            break;
        }
    }
    
    if (ImGui::BeginCombo("##theme_combo", themes[current_theme])) {
        for (int i = 0; i < 3; ++i) {
            if (ImGui::Selectable(themes[i], current_theme == i)) {
                current_theme = i;
                account_state.theme = theme_codes[i];
            }
        }
        ImGui::EndCombo();
    }
    ImGui::Spacing();
    
    // Language
    ImGui::TextUnformatted("Language");
    const char* languages[] = { "English", "Spanish", "French", "German", "Chinese", "Japanese" };
    const char* language_codes[] = { "en", "es", "fr", "de", "zh", "ja" };
    int current_language = 0;
    for (int i = 0; i < 6; ++i) {
        if (account_state.language == language_codes[i]) {
            current_language = i;
            break;
        }
    }
    
    if (ImGui::BeginCombo("##language_combo", languages[current_language])) {
        for (int i = 0; i < 6; ++i) {
            if (ImGui::Selectable(languages[i], current_language == i)) {
                current_language = i;
                account_state.language = language_codes[i];
            }
        }
        ImGui::EndCombo();
    }
    ImGui::Spacing();
    
    // Newsletter
    ImGui::Checkbox("Receive Newsletter", &account_state.receive_newsletter);
    ImGui::TextColored(k.muted, "Get updates about new features and improvements");
    ImGui::Spacing();
    
    // Beta features
    ImGui::Checkbox("Enable Beta Features", &account_state.enable_beta_features);
    ImGui::TextColored(k.muted, "Get early access to experimental features");
    
    card_end();
    
    ImGui::Spacing();
    
    card_begin("##settings_notifications");
    ImGui::TextUnformatted("Social Notifications");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Checkbox("Enable friend and party notifications", &account_state.social_notifications);
    ImGui::TextColored(k.muted, "Show friend requests, messages, invites, and party activity.");
    ImGui::Spacing();
    ImGui::Checkbox("Show offline friends", &account_state.social_show_offline);
    ImGui::Spacing();
    ImGui::Checkbox("Automatically accept trusted invites", &account_state.social_auto_accept);
    ImGui::TextColored(k.muted, "Only use this when you understand the invite source.");

    card_end();
    
    ImGui::Spacing();
    ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - ui_px(120.0f));
    
    if (primary_button("Save Settings", ImVec2(ui_px(120.0f), ui_px(36.0f)))) {
        account::AccountProfile updated_profile = profile;
        updated_profile.theme = account_state.theme.empty()
            ? normalize_theme_code(profile.theme) : normalize_theme_code(account_state.theme);
        updated_profile.language = account_state.language.empty()
            ? normalize_language_code(profile.language) : normalize_language_code(account_state.language);
        updated_profile.receive_newsletter = account_state.receive_newsletter;
        updated_profile.enable_beta_features = account_state.enable_beta_features;
        if (st.cfg) {
            st.cfg->social_notifications = account_state.social_notifications;
            st.cfg->social_show_offline = account_state.social_show_offline;
            st.cfg->social_auto_accept = account_state.social_auto_accept;
        }
        
        if (account_manager.update_profile(updated_profile) &&
            (!st.cfg || config::save(st.exe_dir + L"\\launcher.json", *st.cfg))) {
            push_notice(st, ui_model::NoticeLevel::Success, "Settings Saved", "Your preferences have been updated");
        } else {
            push_notice(st, ui_model::NoticeLevel::Error, "Save Failed", "Could not save your preferences");
        }
    }
}

// ---------------------------------------------------------------------------
// Security Settings
// ---------------------------------------------------------------------------

void draw_account_security(UiState& st) {
    auto& account_state = get_account_ui_state(st);
    auto& account_manager = aml::account::AccountManager::instance();
    
    page_title("Security Settings", "Manage your account security");
    
    auto security_settings = account_manager.get_security_settings();
    
    card_begin("##security_password");
    ImGui::TextUnformatted("Password");
    ImGui::Separator();
    ImGui::Spacing();
    
    if (!account_state.changing_password) {
        ImGui::TextWrapped("Change your account password to keep your account secure.");
        ImGui::Spacing();
        
        if (ghost_button("Change Password", ImVec2(ui_px(150.0f), ui_px(32.0f)))) {
            account_state.changing_password = true;
        }
    } else {
        // Current password
        ImGui::TextUnformatted("Current Password");
        ImGui::SetNextItemWidth(ui_px(300.0f));
        ImGui::InputText("##current_password", &account_state.current_password, ImGuiInputTextFlags_Password);
        ImGui::Spacing();
        
        // New password
        ImGui::TextUnformatted("New Password");
        ImGui::SetNextItemWidth(ui_px(300.0f));
        ImGui::InputText("##new_password", &account_state.new_password, ImGuiInputTextFlags_Password);
        ImGui::Spacing();
        
        // Confirm password
        ImGui::TextUnformatted("Confirm New Password");
        ImGui::SetNextItemWidth(ui_px(300.0f));
        ImGui::InputText("##confirm_password", &account_state.confirm_password, ImGuiInputTextFlags_Password);
        ImGui::Spacing();
        
        // Error message
        if (!account_state.security_error.empty()) {
            ImGui::TextColored(k.red, "%s", account_state.security_error.c_str());
            ImGui::Spacing();
        }
        
        // Success message
        if (!account_state.security_success.empty()) {
            ImGui::TextColored(k.green, "%s", account_state.security_success.c_str());
            ImGui::Spacing();
        }
        
        ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - ui_px(200.0f));
        
        if (ghost_button("Cancel", ImVec2(ui_px(80.0f), ui_px(32.0f)))) {
            account_state.changing_password = false;
            account_state.current_password.clear();
            account_state.new_password.clear();
            account_state.confirm_password.clear();
            account_state.security_error.clear();
            account_state.security_success.clear();
        }
        
        ImGui::SameLine();
        if (primary_button("Change Password", ImVec2(ui_px(120.0f), ui_px(32.0f)))) {
            if (account_state.new_password.empty() || account_state.confirm_password.empty()) {
                account_state.security_error = "Please enter and confirm your new password";
            } else if (account_state.new_password != account_state.confirm_password) {
                account_state.security_error = "Passwords do not match";
            } else if (account_state.current_password.empty()) {
                account_state.security_error = "Please enter your current password";
            } else {
                if (account_manager.change_password(
                    account_state.current_password, 
                    account_state.new_password)) {
                    account_state.security_success = "Password changed successfully!";
                    account_state.security_error.clear();
                    account_state.changing_password = false;
                    account_state.current_password.clear();
                    account_state.new_password.clear();
                    account_state.confirm_password.clear();
                } else {
                    account_state.security_error = "Failed to change password. Please check your current password.";
                }
            }
        }
    }
    
    card_end();
    
    ImGui::Spacing();
    
    card_begin("##security_2fa");
    ImGui::TextUnformatted("Two-Factor Authentication");
    ImGui::Separator();
    ImGui::Spacing();
    
    if (security_settings.two_factor_enabled) {
        ImGui::TextColored(k.green, "✓ Two-Factor Authentication is enabled");
        ImGui::TextColored(k.muted, "Method: %s", security_settings.two_factor_method.c_str());
        ImGui::Spacing();
        
        if (ghost_button("Disable 2FA", ImVec2(ui_px(120.0f), ui_px(32.0f)))) {
            account_state.disabling_2fa = true;
        }
    } else {
        ImGui::TextWrapped("Add an extra layer of security to your account with two-factor authentication.");
        ImGui::Spacing();
        
        if (ghost_button("Enable 2FA", ImVec2(ui_px(120.0f), ui_px(32.0f)))) {
            account_state.enabling_2fa = true;
        }
    }
    
    card_end();
    
    ImGui::Spacing();
    
    card_begin("##security_sessions");
    ImGui::TextUnformatted("Active Sessions");
    ImGui::Separator();
    ImGui::Spacing();
    
    auto sessions = account_manager.get_all_sessions();
    
    if (sessions.empty()) {
        ImGui::TextColored(k.muted, "No active sessions");
    } else {
        for (const auto& session : sessions) {
            ImGui::Text("%s", session.email.c_str());
            ImGui::SameLine();
            ImGui::TextColored(k.muted, " - %s", format_date(session.created_at).c_str());
            ImGui::SameLine();
            
            if (session.id != account_manager.get_current_session().id) {
                if (ghost_button("Sign Out", ImVec2(ui_px(80.0f), ui_px(24.0f)))) {
                    account_state.session_to_end = session.id;
                    account_state.confirming_session_end = true;
                }
            } else {
                ImGui::TextColored(k.muted, "(Current)");
            }
        }
    }
    
    card_end();
}

// ---------------------------------------------------------------------------
// Session Management
// ---------------------------------------------------------------------------

void draw_account_sessions(UiState& st) {
    auto& account_state = get_account_ui_state(st);
    auto& account_manager = aml::account::AccountManager::instance();
    
    page_title("Session Management", "View and manage your active sessions");
    
    auto sessions = account_manager.get_all_sessions();
    auto current_session = account_manager.get_current_session();
    
    card_begin("##sessions_list");
    ImGui::TextUnformatted("Active Sessions");
    ImGui::Separator();
    ImGui::Spacing();
    
    if (sessions.empty()) {
        empty_state("No Active Sessions", "You are not currently signed in on any device.", "S");
    } else {
        for (const auto& session : sessions) {
            ImGui::PushID(session.id.c_str());
            
            ImGui::BeginGroup();
            
            // Session info
            ImGui::Text("%s", session.email.c_str());
            ImGui::TextColored(k.muted, "Session ID: %s", session.id.substr(0, 8).c_str());
            ImGui::TextColored(k.muted, "Created: %s", format_date(session.created_at).c_str());
            ImGui::TextColored(k.muted, "Last Used: %s", format_date(session.last_used).c_str());
            
            // Session status
            if (session.id == current_session.id) {
                ImGui::TextColored(k.green, "✓ Current Session");
            } else if (session.is_expired()) {
                ImGui::TextColored(k.red, "✗ Expired");
            } else {
                ImGui::TextColored(k.muted, "Active");
            }
            
            ImGui::EndGroup();
            ImGui::SameLine();
            
            // Actions
            ImGui::BeginGroup();
            if (session.id != current_session.id) {
                if (ghost_button("Sign Out", ImVec2(ui_px(100.0f), ui_px(28.0f)))) {
                    account_state.session_to_end = session.id;
                    account_state.confirming_session_end = true;
                }
            }
            ImGui::EndGroup();
            
            ImGui::Separator();
            ImGui::PopID();
        }
    }
    
    card_end();
    
    // Confirm session end dialog
    if (account_state.confirming_session_end) {
        ImGui::OpenPopup("Confirm Sign Out");
        account_state.confirming_session_end = false;
    }
    
    if (ImGui::BeginPopupModal("Confirm Sign Out", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Are you sure you want to sign out from this session?");
        ImGui::Spacing();
        
        ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - ui_px(150.0f));
        
        if (ghost_button("Cancel", ImVec2(ui_px(70.0f), ui_px(32.0f)))) {
            ImGui::CloseCurrentPopup();
        }
        
        ImGui::SameLine();
        if (primary_button("Sign Out", ImVec2(ui_px(70.0f), ui_px(32.0f)))) {
            const bool ended = account_manager.end_session(account_state.session_to_end);
            account_state.session_to_end.clear();
            ImGui::CloseCurrentPopup();
            push_notice(st, ended ? ui_model::NoticeLevel::Success : ui_model::NoticeLevel::Error,
                        ended ? "Session Ended" : "Sign Out Failed",
                        ended ? "The session has been signed out"
                              : "The session could not be removed from local storage");
        }
        
        ImGui::EndPopup();
    }
}

// ---------------------------------------------------------------------------
// Account Activity
// ---------------------------------------------------------------------------

void draw_account_activity(UiState& /*st*/) {
    auto& account_manager = aml::account::AccountManager::instance();
    
    page_title("Account Activity", "View your recent account activity");
    
    auto activities = account_manager.get_recent_activity();
    
    card_begin("##activity_list");
    ImGui::TextUnformatted("Recent Activity");
    ImGui::Separator();
    ImGui::Spacing();
    
    if (activities.empty()) {
        empty_state("No Recent Activity", "Your account activity will appear here.", "A");
    } else {
        for (const auto& activity : activities) {
            ImGui::PushID(activity.id.c_str());
            
            ImGui::BeginGroup();
            ImGui::Text("%s", activity.type.c_str());
            ImGui::TextColored(k.muted, "%s", activity.description.c_str());
            ImGui::TextColored(k.muted, "%s", format_date(activity.timestamp).c_str());
            ImGui::EndGroup();
            
            ImGui::Separator();
            ImGui::PopID();
        }
    }
    
    card_end();
}

// ---------------------------------------------------------------------------
// Main Account Page
// ---------------------------------------------------------------------------

void draw_account_page(UiState& st) {
    auto& account_state = get_account_ui_state(st);
    auto& account_manager = aml::account::AccountManager::instance();
    auto& supabase = aml::supabase::SupabaseManager::instance();
    
    // Check if authenticated
    if (!account_manager.is_authenticated() && !supabase.is_authenticated()) {
        draw_breadcrumbs({"Home", "Account"});
        page_title("Your Amalgam account",
                   "One identity for launcher preferences, friends, profiles, and supported sync.");

        const float hero_height = std::clamp(ImGui::GetContentRegionAvail().x * 0.27f,
                                             ui_px(250.0f), ui_px(340.0f));
        card_begin("##account_signed_out", ImVec2(-1, hero_height));
        const ImVec2 hero_pos = ImGui::GetCursorScreenPos();
        const ImVec2 hero_size(ImGui::GetContentRegionAvail().x,
                               std::max(ui_px(210.0f), ImGui::GetContentRegionAvail().y));
        draw_local_image(st, st.exe_dir + L"\\branding\\ai\\account-portal-ai-v2.png",
                         hero_pos, hero_size, c32(k.brand_dk));
        ImDrawList* hero_draw = ImGui::GetWindowDrawList();
        hero_draw->AddRectFilledMultiColor(
            hero_pos, hero_pos + hero_size,
            c32(ImVec4(k.bg.x, k.bg.y, k.bg.z, 0.98f)),
            c32(ImVec4(k.bg.x, k.bg.y, k.bg.z, 0.46f)),
            c32(ImVec4(k.bg.x, k.bg.y, k.bg.z, 0.46f)),
            c32(ImVec4(k.bg.x, k.bg.y, k.bg.z, 0.98f)));
        hero_draw->AddRect(hero_pos, hero_pos + hero_size, c32(k.border), ui_px(10.0f));

        const float copy_width = std::min(hero_size.x * 0.56f, ui_px(640.0f));
        ImGui::SetCursorScreenPos(hero_pos + ImVec2(ui_px(28.0f), ui_px(28.0f)));
        ImGui::PushFont(f_title);
        ImGui::TextUnformatted("Play together. Keep your setup yours.");
        ImGui::PopFont();
        ImGui::PushTextWrapPos(hero_pos.x + ui_px(28.0f) + copy_width);
        ImGui::TextColored(k.muted,
            "Create a free Amalgam account for social features and launcher sync, then connect Microsoft separately when you are ready to play Minecraft.");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        ImGui::TextColored(k.green, "Secure browser sign-in");
        ImGui::SameLine(0, ui_px(18.0f));
        ImGui::TextColored(k.blue, "Windows-protected local credentials");
        ImGui::Spacing();
        if (primary_button("Sign In", ImVec2(ui_px(140.0f), ui_px(38.0f)))) {
            st.auth_prompt_dismissed = false;
            st.login_popup_open = true;
        }
        ImGui::SameLine(0, ui_px(8.0f));
        if (ghost_button("Create Free Account", ImVec2(ui_px(190.0f), ui_px(38.0f)))) {
            st.auth_prompt_dismissed = false;
            st.auth_wizard_state = AuthWizardState();
            st.register_popup_open = true;
        }
        ImGui::SetCursorScreenPos(hero_pos);
        ImGui::Dummy(hero_size);
        card_end();

        ImGui::Spacing();
        const bool three_columns = ImGui::GetContentRegionAvail().x >= ui_px(860.0f);
        const float gap = ui_px(12.0f);
        const float feature_width = three_columns
            ? (ImGui::GetContentRegionAvail().x - gap * 2.0f) / 3.0f
            : ImGui::GetContentRegionAvail().x;
        struct AccountFeature {
            const char* title;
            const char* detail;
            const ImVec4* color;
        };
        const AccountFeature features[] = {
            {"Private by design", "Passwords stay on the provider page; protected tokens stay with this Windows user.", &k.green},
            {"Your profiles, together", "Keep launcher preferences, friends, and supported profile settings connected.", &k.brand_hov},
            {"Microsoft stays separate", "Link the Minecraft account that owns the game without sharing its password with Amalgam.", &k.blue},
        };
        for (int i = 0; i < 3; ++i) {
            if (three_columns && i) ImGui::SameLine(0, gap);
            card_begin((std::string("##account_feature_") + std::to_string(i)).c_str(),
                       ImVec2(feature_width, ui_px(118.0f)));
            ImGui::TextColored(*features[i].color, "%s", features[i].title);
            ImGui::Spacing();
            ImGui::PushTextWrapPos();
            ImGui::TextColored(k.muted, "%s", features[i].detail);
            ImGui::PopTextWrapPos();
            card_end();
            if (!three_columns) ImGui::Spacing();
        }
        return;
    }
    
    // Account tabs — use the shared premium pill language instead of the
    // default ImGui tab strip so Account feels like the rest of the launcher.
    const char* account_tabs[] = {"Overview", "Profile", "Settings", "Security", "Sessions", "Activity"};
    const float tab_gap = ui_px(6.0f);
    for (int i = 0; i < 6; ++i) {
        if (i) ImGui::SameLine(0, tab_gap);
        const bool active = account_state.current_tab == i;
        const ImVec2 label_size = ImGui::CalcTextSize(account_tabs[i]);
        const float pad_x = ui_px(14.0f);
        const float tab_h = ui_px(34.0f);
        const ImVec2 tab_size(label_size.x + pad_x * 2.0f, tab_h);
        const ImVec2 tab_min = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(tab_min, tab_min + tab_size,
                          c32(active ? k.surface2 : ImVec4(0, 0, 0, 0)), ui_px(8.0f));
        if (active)
            dl->AddRect(tab_min, tab_min + tab_size, c32(k.brand), ui_px(8.0f),
                        0, ui_px(1.5f));
        ImGui::InvisibleButton((std::string("##account_tab_") + std::to_string(i)).c_str(), tab_size);
        if (ImGui::IsItemHovered() && !active)
            dl->AddRect(tab_min, tab_min + tab_size, c32(k.border), ui_px(8.0f),
                        0, ui_px(1.0f));
        if (ImGui::IsItemClicked()) account_state.current_tab = i;
        ImGui::PushFont(active ? f_bold : f_body);
        dl->AddText(tab_min + ImVec2(pad_x, (tab_h - ImGui::GetTextLineHeight()) * 0.5f),
                    c32(active ? k.text : k.muted), account_tabs[i]);
        ImGui::PopFont();
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }

    if (account_state.current_tab != 2) {
        account_state.settings_initialized = false;
        account_state.settings_profile_identity.clear();
    }
    
    ImGui::Spacing();
    
    // Draw current tab
    switch (account_state.current_tab) {
        case 0:
        default:
            draw_account_overview(st);
            break;
        case 1:
            draw_account_profile(st);
            break;
        case 2:
            draw_account_settings(st);
            break;
        case 3:
            draw_account_security(st);
            break;
        case 4:
            draw_account_sessions(st);
            break;
        case 5:
            draw_account_activity(st);
            break;
    }
}

// ---------------------------------------------------------------------------
// Account Switcher
// ---------------------------------------------------------------------------

void draw_account_switcher(UiState& st) {
    auto& account_manager = aml::account::AccountManager::instance();
    auto sessions = account_manager.get_all_sessions();
    auto current_session = account_manager.get_current_session();
    
    ImGui::SetNextWindowSize(ImVec2(ui_px(400.0f), ui_px(500.0f)));
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    
    if (!ImGui::BeginPopupModal("Switch Account", nullptr, 
                                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse)) {
        return;
    }
    
    page_title("Switch Account", "Choose which Amalgam account to use");
    
    if (sessions.empty()) {
        empty_state("No Accounts", "You need to sign in with an Amalgam account first.", "A");
    } else {
        for (const auto& session : sessions) {
            ImGui::PushID(session.id.c_str());
            
            // Session card
            card_begin("##session_card", ImVec2(-1, ui_px(80.0f)));
            
            ImGui::BeginGroup();
            
            // Session info
            auto profile = account_manager.get_profile();
            ImGui::Text("%s", session.email.c_str());
            
            if (session.id == current_session.id) {
                ImGui::TextColored(k.green, "✓ Current Session");
            } else if (session.is_expired()) {
                ImGui::TextColored(k.red, "✗ Expired");
            } else {
                ImGui::TextColored(k.muted, "Active");
            }
            
            ImGui::TextColored(k.muted, "Last used: %s", format_date(session.last_used).c_str());
            
            ImGui::EndGroup();
            
            ImGui::SameLine();
            ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - ui_px(200.0f));
            
            ImGui::BeginGroup();
            if (session.id != current_session.id) {
                if (ghost_button("Switch", ImVec2(ui_px(80.0f), ui_px(28.0f)))) {
                    if (account_manager.set_current_session(session.id)) {
                        // Refresh the session if expired
                        if (session.is_expired()) {
                            account_manager.refresh_current_session();
                        }
                        
                        // Update Supabase client
                        auto& supabase = aml::supabase::SupabaseManager::instance();
                        auto switched_session = account_manager.get_current_session();
                        supabase.auto_login(switched_session.access_token, switched_session.refresh_token);
                        
                        ImGui::CloseCurrentPopup();
                        push_notice(st, ui_model::NoticeLevel::Success, "Account Switched", 
                                    "Switched to account: " + session.email);
                    } else {
                        push_notice(st, ui_model::NoticeLevel::Error, "Switch Failed", 
                                    "Could not switch to the selected account");
                    }
                }
                
                ImGui::SameLine();
                if (ghost_button("Sign Out", ImVec2(ui_px(80.0f), ui_px(28.0f)))) {
                    if (account_manager.end_session(session.id)) {
                        push_notice(st, ui_model::NoticeLevel::Success, "Signed Out", 
                                    "Account signed out: " + session.email);
                    } else {
                        push_notice(st, ui_model::NoticeLevel::Error, "Sign Out Failed", 
                                    "Could not sign out the selected account");
                    }
                }
            } else {
                if (ghost_button("Sign Out", ImVec2(ui_px(160.0f), ui_px(28.0f)))) {
                    const auto result = sign_out_current_account(st, account_manager);
                    if (result.local && result.remote) {
                        ImGui::CloseCurrentPopup();
                        push_notice(st, ui_model::NoticeLevel::Success, "Signed Out", 
                                    "Current account signed out");
                    } else {
                        ImGui::CloseCurrentPopup();
                        push_notice(st, ui_model::NoticeLevel::Error, "Sign Out Failed", 
                                    sign_out_failure_message(result));
                    }
                }
            }
            
            ImGui::EndGroup();
            
            card_end();
            ImGui::PopID();
        }
    }
    
    ImGui::Spacing();
    ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - ui_px(120.0f));
    
    if (ghost_button("Close", ImVec2(ui_px(120.0f), ui_px(32.0f)))) {
        ImGui::CloseCurrentPopup();
    }
    
    ImGui::EndPopup();
}

// ---------------------------------------------------------------------------
// Account Button for Top Bar
// ---------------------------------------------------------------------------

void draw_account_button_enhanced(UiState& st) {
    auto& account_manager = aml::account::AccountManager::instance();
    
    if (account_manager.is_authenticated()) {
        auto profile = account_manager.get_profile();
        
        ImGui::PushFont(f_bold);
    ImGui::Text("%s", profile.display_name.empty() ? profile.username.c_str() : profile.display_name.c_str());
        ImGui::PopFont();
        ImGui::SameLine();
        
        if (ghost_button("v", ImVec2(ui_px(24.0f), ui_px(24.0f)))) {
            ImGui::OpenPopup("Account Menu");
        }
        
        // Account menu popup
        if (ImGui::BeginPopup("Account Menu")) {
            ImGui::TextUnformatted("Account");
            ImGui::Separator();
            
            if (ImGui::MenuItem("Account Settings")) {
                navigate_to(st, 15, 0); // Navigate to account page
            }
            
            if (ImGui::MenuItem("Profile")) {
                navigate_to(st, 15, 1); // Navigate to profile tab
            }
            
            if (ImGui::MenuItem("Security")) {
                navigate_to(st, 15, 2); // Navigate to security tab
            }
            
            if (ImGui::MenuItem("Sessions")) {
                navigate_to(st, 15, 3); // Navigate to sessions tab
            }
            
            ImGui::Separator();
            
            // Show all sessions for switching
            auto sessions = account_manager.get_all_sessions();
            if (sessions.size() > 1) {
                ImGui::TextUnformatted("Switch Account");
                ImGui::Separator();
                
                for (const auto& session : sessions) {
                    if (session.id != account_manager.get_current_session().id) {
                        if (ImGui::MenuItem(session.email.c_str())) {
                            if (account_manager.set_current_session(session.id)) {
                                // Refresh the session if expired
                                if (session.is_expired()) {
                                    account_manager.refresh_current_session();
                                }
                                
                                // Update Supabase client
                                auto& supabase = aml::supabase::SupabaseManager::instance();
                                auto current_session = account_manager.get_current_session();
                                supabase.auto_login(current_session.access_token, current_session.refresh_token);
                                
                                push_notice(st, ui_model::NoticeLevel::Success, "Account Switched", 
                                            "Switched to account: " + session.email);
                            } else {
                                push_notice(st, ui_model::NoticeLevel::Error, "Switch Failed", 
                                            "Could not switch to the selected account");
                            }
                        }
                    }
                }
                
                ImGui::Separator();
            }
            
            if (ImGui::MenuItem("Sign Out")) {
                const auto result = sign_out_current_account(st, account_manager);
                if (result.local && result.remote) {
                    push_notice(st, ui_model::NoticeLevel::Success, "Signed Out",
                                "You have been signed out");
                } else {
                    push_notice(st, ui_model::NoticeLevel::Error, "Sign Out Incomplete",
                                sign_out_failure_message(result));
                }
            }
            
            if (ImGui::MenuItem("Sign Out All Accounts")) {
                const auto result = sign_out_all_local_accounts(st, account_manager);
                if (result.local && result.remote) {
                    push_notice(st, ui_model::NoticeLevel::Success, "Signed Out Locally",
                                "All local account sessions were cleared and the current remote session was signed out");
                } else {
                    push_notice(st, ui_model::NoticeLevel::Error, "Sign Out Incomplete",
                                sign_out_failure_message(result));
                }
            }
            
            ImGui::EndPopup();
        }
    } else {
        // Show sign in button
        if (ghost_button("Sign In / Create Account", ImVec2(ui_px(180.0f), ui_px(32.0f)))) {
            st.auth_prompt_dismissed = false;
            ImGui::OpenPopup("Amalgam Account Setup");
        }
    }
}

// ---------------------------------------------------------------------------
// Helper Functions
}  // namespace aml::ui
