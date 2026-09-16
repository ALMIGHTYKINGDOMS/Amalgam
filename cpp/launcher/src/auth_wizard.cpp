#include "ui.h"
#include "ui_internal.h"
#include "supabase.h"
#include "account_manager.h"
#include "auth.h"
#include "net.h"
#include "config.h"
#include "online_config.h"
#include "official_launcher_bridge.h"

#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <cctype>
#include <chrono>

namespace aml::ui {

namespace {

ImVec4 alpha(ImVec4 color, float value) {
    color.w = value;
    return color;
}

ImVec2 wizard_modal_size(float preferred_width, float preferred_height) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float max_width = std::max(ui_px(420.0f), viewport->WorkSize.x - ui_px(40.0f));
    const float max_height = std::max(ui_px(360.0f), viewport->WorkSize.y - ui_px(40.0f));
    return ImVec2(std::min(ui_px(preferred_width), max_width),
                  std::min(ui_px(preferred_height), max_height));
}

void position_wizard_modal(float preferred_width, float preferred_height) {
    ImGui::SetNextWindowSize(wizard_modal_size(preferred_width, preferred_height),
                             ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
}

void draw_wizard_hero(UiState& st, const char* eyebrow, const char* title, const char* subtitle,
                      int current_step = -1, int total_steps = 0) {
    const float height = ui_px(112.0f);
    card_begin("##auth_wizard_hero", ImVec2(-1, height));
    const ImVec2 min = ImGui::GetWindowPos();
    const ImVec2 max = min + ImGui::GetWindowSize();
    ImDrawList* draw = ImGui::GetWindowDrawList();

    // Use the generated sanctuary as a restrained backdrop. The tint keeps
    // copy and controls legible while giving every account wizard a finished
    // branded surface instead of a flat gradient alone.
    draw_local_image(st, st.exe_dir + L"\\branding\\ai\\wizard-account-ai-v2.png",
                     min, ImGui::GetWindowSize(),
                     c32(alpha(k.text, 0.16f)), ui_model::ImageFit::Cover);

    // An intentionally restrained, game-inspired light field gives every
    // account flow an identity without competing with its form content.
    draw->AddRectFilledMultiColor(
        min + ImVec2(ui_px(1.0f), ui_px(1.0f)), max - ImVec2(ui_px(1.0f), ui_px(1.0f)),
        c32(alpha(k.brand_dk, 0.96f)), c32(alpha(k.surface2, 0.88f)),
        c32(alpha(k.surface, 0.88f)), c32(alpha(k.brand_dk, 0.96f)));
    const ImVec2 glow_center(max.x - ui_px(68.0f), min.y + ui_px(25.0f));
    for (int ring = 5; ring >= 1; --ring) {
        const float radius = ui_px(16.0f + ring * 9.0f);
        draw->AddCircleFilled(glow_center, radius,
                              c32(alpha(k.brand_hov, 0.008f * static_cast<float>(6 - ring))));
    }
    draw->AddLine(min + ImVec2(ui_px(14.0f), ui_px(1.0f)),
                  max - ImVec2(ui_px(14.0f), max.y - min.y - ui_px(1.0f)),
                  c32(alpha(k.brand_hov, 0.60f)), ui_px(1.0f));
    draw_brand_badge(draw, min + ImVec2(ui_px(43.0f), ui_px(55.0f)), ui_px(28.0f), ui_px(1.16f));

    // Every hero line is indented past the mark explicitly: ImGui returns the
    // cursor to the window's left edge on a new line, which put the title under
    // the mark instead of in the text column beside it.
    const float text_x = ui_px(88.0f);
    ImGui::SetCursorPos(ImVec2(text_x, ui_px(16.0f)));
    ImGui::PushFont(f_small);
    ImGui::TextColored(k.brand_hov, "%s", eyebrow);
    ImGui::PopFont();
    ImGui::SetCursorPosX(text_x);
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    ImGui::SetCursorPosX(text_x);
    ImGui::PushTextWrapPos(std::max(min.x + ui_px(210.0f), max.x - ui_px(128.0f)));
    ImGui::TextColored(k.muted, "%s", subtitle);
    ImGui::PopTextWrapPos();

    if (current_step >= 0 && total_steps > 0) {
        const std::string progress = "STEP " + std::to_string(current_step + 1) +
                                     " OF " + std::to_string(total_steps);
        ImGui::PushFont(f_small);
        const ImVec2 text_size = ImGui::CalcTextSize(progress.c_str());
        ImGui::PopFont();
        const ImVec2 badge_min(max.x - text_size.x - ui_px(28.0f), min.y + ui_px(16.0f));
        const ImVec2 badge_max(max.x - ui_px(16.0f), badge_min.y + ui_px(24.0f));
        draw->AddRectFilled(badge_min, badge_max, c32(alpha(k.bg, 0.76f)), ui_px(7.0f));
        draw->AddRect(badge_min, badge_max, c32(alpha(k.brand_hov, 0.60f)), ui_px(7.0f));
        draw->AddText(badge_min + ImVec2(ui_px(6.0f), ui_px(5.0f)),
                      c32(k.text), progress.c_str());
    }

    card_end();
}

void draw_wizard_feedback(const char* id, const std::string& message, bool is_error,
                          const char* error_title = "Something needs attention") {
    if (message.empty()) return;
    const ImVec4 tone = is_error ? k.red : k.green;
    card_begin(id, ImVec2(-1, 0));
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddCircleFilled(origin + ImVec2(ui_px(17.0f), ui_px(17.0f)), ui_px(14.0f),
                          c32(alpha(tone, 0.20f)));
    draw_icon(is_error ? IconId::Warning : IconId::Check,
              origin + ImVec2(ui_px(17.0f), ui_px(17.0f)), ui_px(8.0f), c32(tone));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ui_px(38.0f));
    ImGui::PushFont(f_bold);
    ImGui::TextColored(tone, "%s", is_error ? error_title : "You are all set");
    ImGui::PopFont();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ui_px(38.0f));
    ImGui::PushTextWrapPos();
    ImGui::TextColored(k.text, "%s", message.c_str());
    ImGui::PopTextWrapPos();
    card_end();
}

void draw_form_label(const char* label, const char* hint = nullptr) {
    ImGui::PushFont(f_bold);
    ImGui::TextUnformatted(label);
    ImGui::PopFont();
    if (hint && *hint) {
        ImGui::TextColored(k.muted, "%s", hint);
    }
}

void draw_security_note(const char* message) {
    card_begin("##auth_security_note", ImVec2(-1, 0));
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    draw_icon(IconId::Lock, origin + ImVec2(ui_px(12.0f), ui_px(12.0f)), ui_px(7.0f), c32(k.green));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ui_px(28.0f));
    ImGui::PushTextWrapPos();
    ImGui::TextColored(k.muted, "%s", message);
    ImGui::PopTextWrapPos();
    card_end();
}

void draw_auth_progress(int current_step) {
    const char* steps[] = {"Welcome", "Account", "Verify", "Minecraft", "Ready"};
    constexpr int count = 5;
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float width = std::max(ui_px(340.0f), ImGui::GetContentRegionAvail().x);
    const float circle_radius = ui_px(12.0f);
    const float segment = (width - circle_radius * 2.0f) / static_cast<float>(count - 1);
    const float y = start.y + circle_radius;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    for (int i = 0; i < count; ++i) {
        const float x = start.x + circle_radius + segment * static_cast<float>(i);
        const bool complete = i < current_step;
        const bool active = i == current_step;
        const ImVec4 tone = complete ? k.green : active ? k.brand_hov : alpha(k.muted, 0.70f);
        if (i > 0) {
            const float previous_x = start.x + circle_radius + segment * static_cast<float>(i - 1);
            draw->AddLine(ImVec2(previous_x + circle_radius + ui_px(4.0f), y),
                          ImVec2(x - circle_radius - ui_px(4.0f), y),
                          c32(i <= current_step ? alpha(k.brand_hov, 0.92f) : alpha(k.border, 0.95f)),
                          ui_px(2.0f));
        }
        draw->AddCircleFilled(ImVec2(x, y), circle_radius, c32(tone));
        draw->AddCircle(ImVec2(x, y), circle_radius, c32(alpha(k.text, 0.25f)), 0, ui_px(1.0f));
        const std::string mark = complete ? "✓" : std::to_string(i + 1);
        const ImVec2 mark_size = ImGui::CalcTextSize(mark.c_str());
        draw->AddText(ImVec2(x - mark_size.x * 0.5f, y - mark_size.y * 0.5f), c32(k.text), mark.c_str());
        ImGui::PushFont(f_small);
        const ImVec2 label_size = ImGui::CalcTextSize(steps[i]);
        ImGui::PopFont();
        draw->AddText(ImVec2(x - label_size.x * 0.5f, y + circle_radius + ui_px(7.0f)),
                      c32(i <= current_step ? k.text : k.muted), steps[i]);
    }
    ImGui::Dummy(ImVec2(0.0f, circle_radius * 2.0f + ui_px(34.0f)));
}

void dismiss_auth_setup(UiState& st) {
    // Do not leave passwords, verification codes, or a stale step in memory
    // after the user closes the wizard.  The next explicit open starts clean.
    st.auth_wizard_state = AuthWizardState();
    st.wizard_open = false;
    st.register_popup_open = false;
    st.auth_prompt_dismissed = true;
}

} // namespace

void draw_wizard_progress(int current_step);
void draw_wizard_welcome(UiState& st);
void draw_wizard_create_account(UiState& st);
void draw_wizard_verify_email(UiState& st);
void draw_wizard_connect_microsoft(UiState& st);
void draw_wizard_complete(UiState& st);

// ---------------------------------------------------------------------------
// Auth Wizard State
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Auth Wizard
// ---------------------------------------------------------------------------

void draw_auth_wizard(UiState& st) {
    auto& wizard_state = st.auth_wizard_state;
    
    // Check if already authenticated
    auto& supabase = aml::supabase::SupabaseManager::instance();
    if (supabase.is_initialized() && supabase.is_authenticated()) {
        // Amalgam account setup is complete. Microsoft sign-in has its own
        // dialog and must never reopen or advance the profile/setup wizard.
        bool popup_open = true;
        if (ImGui::BeginPopupModal("Amalgam Account Setup", &popup_open,
                                   ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse)) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        if (!popup_open) dismiss_auth_setup(st);
        return;
    }
    
    // Set up modal.  The previous compact modal made a first-time account
    // setup feel like a prototype form; this intentionally follows the same
    // layered, spacious desktop language as the rest of the launcher.
    position_wizard_modal(760.0f, 670.0f);
    
    bool popup_open = true;
    if (!ImGui::BeginPopupModal("Amalgam Account Setup", &popup_open,
                                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse)) {
        if (!popup_open) dismiss_auth_setup(st);
        return;
    }
    ImGui::PushID("amalgam_account_setup");
    
    draw_wizard_hero(st, "AMALGAM ACCOUNT", "Welcome to your world",
                     "Create an Amalgam account, then connect Minecraft when you are ready.",
                     wizard_state.current_step, 5);
    ImGui::Spacing();
    draw_wizard_progress(wizard_state.current_step);
    ImGui::Separator();
    ImGui::Spacing();
    
    // Step content
    switch (wizard_state.current_step) {
        case 0:
            draw_wizard_welcome(st);
            break;
        case 1:
            draw_wizard_create_account(st);
            break;
        case 2:
            draw_wizard_verify_email(st);
            break;
        case 3:
            draw_wizard_connect_microsoft(st);
            break;
        case 4:
            draw_wizard_complete(st);
            break;
    }
    
    // Safe, actionable feedback sits next to the action area instead of
    // leaking transport bodies at the bottom of the modal.
    draw_wizard_feedback("##auth_wizard_error", wizard_state.error_message, true,
                         "Account setup needs attention");
    draw_wizard_feedback("##auth_wizard_success", wizard_state.success_message, false);
    if (!wizard_state.error_message.empty() || !wizard_state.success_message.empty()) ImGui::Spacing();

    ImGui::Separator();
    ImGui::Spacing();
    if (wizard_state.current_step > 0 && wizard_state.current_step < 4) {
        if (ghost_button("Back", ImVec2(ui_px(100.0f), ui_px(32.0f)))) {
            if (wizard_state.current_step > 0) {
                wizard_state.current_step--;
                wizard_state.error_message.clear();
                wizard_state.success_message.clear();
            }
        }
    }
    ImGui::SameLine(ImGui::GetWindowWidth() - ui_px(116.0f));
    if (ghost_button("Close", ImVec2(ui_px(96.0f), ui_px(32.0f)))) {
        popup_open = false;
        ImGui::CloseCurrentPopup();
    }
    
    ImGui::PopID();
    ImGui::EndPopup();
    if (!popup_open) dismiss_auth_setup(st);
}

void draw_microsoft_login_dialog(UiState& st) {
    if (st.microsoft_login_popup_open) {
        ImGui::OpenPopup("Microsoft Sign In");
    }
    bool popup_open = st.microsoft_login_popup_open;
    position_wizard_modal(700.0f, 540.0f);
    if (!ImGui::BeginPopupModal("Microsoft Sign In", &popup_open,
                                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse)) {
        return;
    }

    auto close_login = [&]() {
        popup_open = false;
        st.microsoft_login_popup_open = false;
        st.login_wizard_open = false;
        ImGui::CloseCurrentPopup();
    };

    draw_wizard_hero(st, "MINECRAFT ACCESS", "Connect Microsoft",
                     "Sign in here to play directly, or hand your prepared profile to the Minecraft Launcher instead.");
    ImGui::Spacing();

    int state = 0;
    std::string uri;
    std::string code;
    std::string error;
    std::string status;
    std::string account_username;
    {
        std::lock_guard<std::mutex> lock(st.auth_mu);
        state = st.login_wizard_state;
        uri = st.login_verification_uri;
        code = st.login_user_code;
        error = st.login_error;
        status = st.auth_status;
        account_username = st.account.username;
    }

    if (state == 1 && !code.empty()) {
        card_begin("##microsoft_device_code", ImVec2(-1, 0));
        const ImVec2 card_origin = ImGui::GetCursorScreenPos();
        draw_icon(IconId::Globe, card_origin + ImVec2(ui_px(16.0f), ui_px(16.0f)),
                  ui_px(9.0f), c32(k.brand_hov));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ui_px(36.0f));
        ImGui::PushFont(f_bold);
        ImGui::TextUnformatted("1. Enter this one-time code in Microsoft’s browser page");
        ImGui::PopFont();
        ImGui::Spacing();
        const ImVec2 code_min = ImGui::GetCursorScreenPos();
        const ImVec2 code_max(code_min.x + ImGui::GetContentRegionAvail().x, code_min.y + ui_px(64.0f));
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(code_min, code_max, c32(alpha(k.bg, 0.72f)), ui_px(10.0f));
        draw->AddRect(code_min, code_max, c32(alpha(k.brand_hov, 0.72f)), ui_px(10.0f), 0, ui_px(1.0f));
        ImGui::SetCursorScreenPos(code_min + ImVec2(ui_px(20.0f), ui_px(17.0f)));
        ImGui::PushFont(f_title);
        ImGui::TextColored(k.brand_hov, "%s", code.c_str());
        ImGui::PopFont();
        ImGui::SetCursorScreenPos(code_max + ImVec2(0.0f, ui_px(8.0f)));
        ImGui::TextColored(k.muted, "2. Finish sign-in in the browser. This window will continue automatically.");
        card_end();
        ImGui::Spacing();
        if (primary_button("Open secure Microsoft sign-in", ImVec2(ui_px(238.0f), ui_px(38.0f)))) {
            ShellExecuteA(nullptr, "open", uri.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        ImGui::SameLine();
        if (ghost_button("Copy code", ImVec2(ui_px(120.0f), ui_px(38.0f))))
            ImGui::SetClipboardText(code.c_str());
        ImGui::Spacing();
        draw_security_note("Waiting for confirmation from Microsoft. If Microsoft/Xbox reports that the app is pending approval, close this window and try again after approval is complete.");
    } else if (state == 2 && !account_username.empty()) {
        draw_wizard_feedback("##microsoft_connected", "Minecraft is connected and ready to use with your Amalgam profiles.", false);
        ImGui::Spacing();
        card_begin("##microsoft_profile", ImVec2(-1, 0));
        ImGui::TextColored(k.muted, "MINECRAFT PROFILE");
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted(account_username.c_str());
        ImGui::PopFont();
        if (!status.empty()) ImGui::TextColored(k.muted, "%s", status.c_str());
        card_end();
        ImGui::Spacing();
        if (primary_button("Continue to Amalgam", ImVec2(ui_px(205.0f), ui_px(38.0f)))) {
            close_login();
        }
    } else if (state == 3) {
        draw_wizard_feedback("##microsoft_error",
                             error.empty() ? "Microsoft sign-in could not be completed. Try again when you are ready."
                                           : humanize_error(error),
                             true, "Microsoft sign-in needs attention");
        ImGui::Spacing();
        std::string lower_error = error;
        std::transform(lower_error.begin(), lower_error.end(), lower_error.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const bool official_fallback =
            lower_error.find("official minecraft launcher") != std::string::npos ||
            lower_error.find("microsoft approval") != std::string::npos ||
            lower_error.find("unauthorized_client") != std::string::npos ||
            lower_error.find("application with identifier") != std::string::npos ||
            lower_error.find("aadsts700016") != std::string::npos ||
            lower_error.find("app not approved") != std::string::npos;
        if (official_fallback) {
            draw_security_note("Direct sign-in is still waiting on Minecraft approval, so this step can fail until it is granted. Playing does not: switch Play to the Minecraft Launcher mode and Amalgam hands it your prepared profile.");
            ImGui::Spacing();
            if (primary_button("Use Minecraft Launcher mode", ImVec2(ui_px(238.0f), ui_px(38.0f)))) {
                if (st.cfg) {
                    st.cfg->launch_mode = "official_launcher";
                    st.settings_dirty = true;
                }
                push_notice(st, ui_model::NoticeLevel::Info, "Minecraft Launcher mode is on",
                            "Play prepares your profile and hands it to the Minecraft Launcher.");
                close_login();
            }
            ImGui::SameLine();
            if (ghost_button("Open it now", ImVec2(ui_px(110.0f), ui_px(38.0f)))) {
                if (!aml::official_launcher::OpenOfficialLauncher()) {
                    push_notice(st, ui_model::NoticeLevel::Error,
                                "Minecraft Launcher not found",
                                "Install the official Minecraft Launcher, then press Play on a profile again.");
                }
            }
            ImGui::SameLine();
            if (ghost_button("Close", ImVec2(ui_px(84.0f), ui_px(38.0f)))) close_login();
        } else {
            if (primary_button("Try again", ImVec2(ui_px(120.0f), ui_px(38.0f)))) {
                close_login();
                start_microsoft_login(st);
            }
            ImGui::SameLine();
            if (ghost_button("Cancel", ImVec2(ui_px(100.0f), ui_px(38.0f)))) close_login();
        }
    } else {
        card_begin("##microsoft_starting", ImVec2(-1, 0));
        draw_loading_spinner(ui_px(20.0f));
        ImGui::SameLine(0, ui_px(12.0f));
        ImGui::PushFont(f_bold);
        ImGui::TextUnformatted("Preparing secure Microsoft sign-in");
        ImGui::PopFont();
        ImGui::TextColored(k.muted, "We are requesting a one-time browser code. This normally takes a few seconds.");
        card_end();
        if (!st.auth_working) {
            ImGui::Spacing();
            if (ghost_button("Cancel", ImVec2(ui_px(100.0f), ui_px(38.0f)))) {
                close_login();
            }
        }
    }

    ImGui::EndPopup();
    if (!popup_open) {
        st.microsoft_login_popup_open = false;
        st.login_wizard_open = false;
    }
}

// ---------------------------------------------------------------------------
// Progress Indicator
// ---------------------------------------------------------------------------

void draw_wizard_progress(int current_step) {
    draw_auth_progress(current_step);
}

// ---------------------------------------------------------------------------
// Step 0: Welcome
// ---------------------------------------------------------------------------

void draw_wizard_welcome(UiState& st) {
    auto& wizard_state = st.auth_wizard_state;
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted("Set up the essentials");
    ImGui::PopFont();
    ImGui::TextColored(k.muted, "A free Amalgam account keeps launcher features connected. Microsoft stays separate and is only needed when you play Minecraft.");
    ImGui::Spacing();

    struct Benefit { IconId icon; const char* title; const char* detail; };
    const Benefit benefits[] = {
        {IconId::Cube, "One launcher", "Manage Java, Bedrock, loaders, and isolated profiles in one place."},
        {IconId::Cloud, "Your setup, synced", "Keep supported launcher preferences and social features connected."},
        {IconId::Lock, "Private by design", "Passwords stay with their provider; Amalgam uses protected local session data."},
    };
    for (int i = 0; i < 3; ++i) {
        const std::string id = "##welcome_benefit_" + std::to_string(i);
        card_begin(id.c_str(), ImVec2(-1, 0));
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        draw_icon(benefits[i].icon, origin + ImVec2(ui_px(16.0f), ui_px(16.0f)),
                  ui_px(9.0f), c32(i == 2 ? k.green : k.brand_hov));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ui_px(38.0f));
        ImGui::PushFont(f_bold);
        ImGui::TextUnformatted(benefits[i].title);
        ImGui::PopFont();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ui_px(38.0f));
        ImGui::PushTextWrapPos();
        ImGui::TextColored(k.muted, "%s", benefits[i].detail);
        ImGui::PopTextWrapPos();
        card_end();
        if (i < 2) ImGui::Spacing();
    }

    ImGui::Spacing();
    if (primary_button("Create free Amalgam account", ImVec2(-1, ui_px(40.0f)))) {
        // Account onboarding must never share state with the profile creator.
        st.wizard_open = false;
        wizard_state.current_step = 1;
        wizard_state.error_message.clear();
        wizard_state.success_message.clear();
    }
    ImGui::Spacing();
    ImGui::TextColored(k.muted, "Already using Amalgam?");
    ImGui::SameLine();
    if (ghost_button("Sign in instead", ImVec2(ui_px(142.0f), ui_px(30.0f)))) {
        // Open sign in
        st.wizard_open = false;
        st.login_popup_open = true;
        ImGui::CloseCurrentPopup();
    }
}

// ---------------------------------------------------------------------------
// Step 1: Create Account
// ---------------------------------------------------------------------------

void draw_wizard_create_account(UiState& st) {
    auto& wizard_state = st.auth_wizard_state;
    ImGui::PushID("create_amalgam_account_form");
    
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted("Create your Amalgam account");
    ImGui::PopFont();
    ImGui::TextColored(k.muted, "Use an email you can access. You will verify it before we connect Minecraft.");
    ImGui::Spacing();

    draw_form_label("Email address", "We only use this for account access and confirmation.");
    ImGui::SetNextItemWidth(-1);
    input_text_hint("##wizard_email", "you@example.com", &wizard_state.email);
    
    ImGui::Spacing();
    
    draw_form_label("Username", "This is your unique Amalgam name.");
    ImGui::SetNextItemWidth(-1);
    input_text_hint("##wizard_username", "Choose a username", &wizard_state.username);
    
    ImGui::Spacing();
    
    draw_form_label("Display name", "Optional — how you appear to other Amalgam users.");
    ImGui::SetNextItemWidth(-1);
    input_text_hint("##wizard_display_name", "Your display name", &wizard_state.display_name);
    
    ImGui::Spacing();
    
    draw_form_label("Password", "At least 8 characters, including an uppercase letter and number.");
    ImGui::SetNextItemWidth(-1);
    input_secret("##wizard_password", &wizard_state.password);
    
    ImGui::Spacing();
    
    draw_form_label("Confirm password");
    ImGui::SetNextItemWidth(-1);
    input_secret("##wizard_confirm_password", &wizard_state.confirm_password);
    
    // Live password requirements (updates as the user types)
    if (!wizard_state.password.empty() || !wizard_state.confirm_password.empty()) {
        ImGui::Spacing();
        const std::string& pw = wizard_state.password;
        const std::string& cp = wizard_state.confirm_password;
        const bool has_len = pw.size() >= 8;
        const bool has_upper = std::any_of(pw.begin(), pw.end(),
            [](unsigned char c) { return std::isupper(c) != 0; });
        const bool has_digit = std::any_of(pw.begin(), pw.end(),
            [](unsigned char c) { return std::isdigit(c) != 0; });
        const bool matches = !cp.empty() && pw == cp;
        card_begin("##password_requirements", ImVec2(-1, 0));
        ImGui::TextColored(k.muted, "PASSWORD CHECKLIST");
        auto req = [&](bool ok, const char* text) {
            ImGui::TextColored(ok ? k.green : k.muted, "%s %s", ok ? "\xe2\x9c\x93" : "\xe2\x97\x8b", text);
        };
        req(has_len, "At least 8 characters");
        req(has_upper, "Contains an uppercase letter");
        req(has_digit, "Contains a number");
        if (!cp.empty()) req(matches, "Passwords match");
        card_end();
    }
    
    ImGui::Spacing();
    card_begin("##account_consent", ImVec2(-1, 0));
    ImGui::TextColored(k.muted, "ACCOUNT CONSENT");
    ImGui::Checkbox("I accept the Terms of Service", &wizard_state.accept_terms);
    ImGui::SameLine();
    if (ghost_button("View terms", ImVec2(ui_px(96.0f), ui_px(26.0f)))) {
        ShellExecuteW(nullptr, L"open",
                      aml::net::to_wide(aml::online::config().terms_url()).c_str(),
                      nullptr, nullptr, SW_SHOWNORMAL);
    }
    
    ImGui::Checkbox("I accept the Privacy Policy", &wizard_state.accept_privacy);
    ImGui::SameLine();
    if (ghost_button("View privacy", ImVec2(ui_px(96.0f), ui_px(26.0f)))) {
        ShellExecuteW(nullptr, L"open",
                      aml::net::to_wide(aml::online::config().privacy_url()).c_str(),
                      nullptr, nullptr, SW_SHOWNORMAL);
    }
    
    ImGui::Checkbox("Send me optional product news and updates", &wizard_state.receive_newsletter);
    card_end();
    
    ImGui::Spacing();
    // Validate
    bool can_continue = !wizard_state.email.empty() &&
                       !wizard_state.username.empty() &&
                       wizard_state.password.size() >= 8 &&
                       wizard_state.password == wizard_state.confirm_password &&
                       wizard_state.accept_terms &&
                       wizard_state.accept_privacy;
    
    if (wizard_state.working) {
        ghost_button("Creating account...", ImVec2(-1, ui_px(40.0f)), true);
    } else if (can_continue) {
        if (primary_button("Create account", ImVec2(-1, ui_px(40.0f)))) {
            wizard_state.working = true;
            wizard_state.error_message.clear();
            
            // Create account via Supabase
            auto& supabase = aml::supabase::SupabaseManager::instance();
            
            // Prepare metadata
            std::map<std::string, std::string> metadata;
            metadata["username"] = wizard_state.username;
            metadata["display_name"] = wizard_state.display_name;
            metadata["newsletter"] = wizard_state.receive_newsletter ? "true" : "false";
            
            auto response = supabase.sign_up(
                wizard_state.email, 
                wizard_state.password,
                metadata);
            
            wizard_state.working = false;
            
            if (response.success) {
                if (!response.user.access_token.empty()) {
                    aml::account::AccountManager::instance().create_session(
                        response.user.email, response.user.access_token,
                        response.user.refresh_token, response.user.expires_at);
                    wizard_state.success_message =
                        "Account created. You can install mods and create profiles now. Minecraft sign-in is handled by the official launcher when you press Play.";
                    wizard_state.current_step = 4;
                    ImGui::CloseCurrentPopup();
                } else {
                    // Email confirmation is a supported production path. Keep
                    // the account form intact and move to the real verification
                    // step instead of leaving the user at a dead-end error.
                    wizard_state.current_step = 2;
                    wizard_state.code_sent = false;
                    wizard_state.verification_code.clear();
                    wizard_state.success_message =
                        "Account created. Check your email to finish verification.";
                }
            } else {
                wizard_state.error_message = response.error.empty() ? 
                    "Failed to create account" : humanize_error(response.error);
            }
        }
    } else {
        if (ghost_button("Create account", ImVec2(-1, ui_px(40.0f)))) {
            if (wizard_state.email.empty()) {
                wizard_state.error_message = "Email is required";
            } else if (wizard_state.username.empty()) {
                wizard_state.error_message = "Username is required";
            } else if (wizard_state.password.empty()) {
                wizard_state.error_message = "Password is required";
            } else if (wizard_state.password.size() < 8) {
                wizard_state.error_message = "Password must be at least 8 characters";
            } else if (wizard_state.password != wizard_state.confirm_password) {
                wizard_state.error_message = "Passwords do not match";
            } else if (!wizard_state.accept_terms) {
                wizard_state.error_message = "You must accept the Terms of Service";
            } else if (!wizard_state.accept_privacy) {
                wizard_state.error_message = "You must accept the Privacy Policy";
            }
        }
    }
    ImGui::Spacing();
    draw_security_note("Your password is sent only to the account provider over a secure connection. Amalgam never displays or stores it in the launcher interface.");
    ImGui::PopID();
}

// ---------------------------------------------------------------------------
// Step 2: Verify Email
// ---------------------------------------------------------------------------

void draw_wizard_verify_email(UiState& st) {
    auto& wizard_state = st.auth_wizard_state;
    
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted("Verify your email");
    ImGui::PopFont();
    ImGui::TextColored(k.muted, "We sent a confirmation message to the address you entered.");
    ImGui::Spacing();

    card_begin("##verification_inbox", ImVec2(-1, 0));
    const ImVec2 inbox_origin = ImGui::GetCursorScreenPos();
    draw_icon(IconId::Bell, inbox_origin + ImVec2(ui_px(16.0f), ui_px(16.0f)), ui_px(9.0f), c32(k.brand_hov));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ui_px(38.0f));
    ImGui::PushFont(f_bold);
    ImGui::TextUnformatted("Check your inbox");
    ImGui::PopFont();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ui_px(38.0f));
    ImGui::TextColored(k.muted, "%s", wizard_state.email.c_str());
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ui_px(38.0f));
    ImGui::TextColored(k.muted, "Open the confirmation link, then return here to continue.");
    card_end();
    
    ImGui::Spacing();
    
    draw_form_label("Verification code", "Optional — use this only if your email includes a six-digit code.");
    ImGui::SetNextItemWidth(-1);
    input_text_hint("##wizard_code", "6-digit code", &wizard_state.verification_code);
    
    ImGui::Spacing();
    
    // Resend confirmation email/code
    if (wizard_state.code_resend_seconds > 0) {
        ImGui::TextColored(k.muted, "Resend email in %d seconds", wizard_state.code_resend_seconds);
    } else {
        if (ghost_button("Resend verification email", ImVec2(ui_px(220.0f), ui_px(30.0f)))) {
            auto& supabase = aml::supabase::SupabaseManager::instance();
            auto response = supabase.resend_signup_confirmation(wizard_state.email);
            if (response.success) {
                wizard_state.code_sent = true;
                wizard_state.code_resend_seconds = 60;
                wizard_state.success_message = "Verification email resent.";
            } else {
                wizard_state.error_message = response.error.empty()
                    ? "Failed to resend verification email" : humanize_error(response.error);
            }
        }
    }
    
    ImGui::Spacing();
    if (primary_button("I confirmed my email", ImVec2(ui_px(206.0f), ui_px(38.0f)))) {
        wizard_state.working = true;
        wizard_state.error_message.clear();
        auto response = aml::supabase::SupabaseManager::instance().sign_in(
            wizard_state.email, wizard_state.password);
        wizard_state.working = false;
        if (response.success) {
            aml::account::AccountManager::instance().create_session(
                response.user.email, response.user.access_token,
                response.user.refresh_token, response.user.expires_at);
            wizard_state.current_step = 4;
            wizard_state.success_message =
                "Email verified. You can install mods and create profiles now. Minecraft sign-in is handled by the official launcher when you press Play.";
            ImGui::CloseCurrentPopup();
        } else {
            wizard_state.error_message = response.error.empty()
                ? "Open the confirmation link first, then try again."
                : humanize_error(response.error);
        }
    }
    ImGui::TextColored(k.muted, "The browser may return to a local page. Keep this window open, then use the button above after confirmation.");
    ImGui::Spacing();
    ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - ui_px(150.0f));
    
    // Check if code was sent
    if (!wizard_state.code_sent) {
        if (ghost_button("Send a new verification email", ImVec2(ui_px(246.0f), ui_px(36.0f)))) {
            auto& supabase = aml::supabase::SupabaseManager::instance();
            auto response = supabase.resend_signup_confirmation(wizard_state.email);
            if (response.success) {
                wizard_state.code_sent = true;
                wizard_state.code_resend_seconds = 60;
                wizard_state.success_message = "Verification email sent.";
            } else {
                wizard_state.error_message = response.error.empty()
                    ? "Failed to send verification email" : humanize_error(response.error);
            }
        }
    } else {
        if (wizard_state.working) {
            if (ghost_button("Verifying...", ImVec2(ui_px(150.0f), ui_px(36.0f)))) {}
        } else {
            if (primary_button("Verify code", ImVec2(ui_px(150.0f), ui_px(36.0f)))) {
                const bool valid_code = wizard_state.verification_code.size() == 6 &&
                    std::all_of(wizard_state.verification_code.begin(),
                                wizard_state.verification_code.end(),
                                [](unsigned char c) { return std::isdigit(c) != 0; });
                if (valid_code) {
                    wizard_state.working = true;
                    wizard_state.error_message.clear();
                    
                    auto& supabase = aml::supabase::SupabaseManager::instance();
                    auto result = supabase.client()->verify_otp(wizard_state.email, wizard_state.verification_code);
                    
                    wizard_state.working = false;
                    
                    if (result.success) {
                        aml::account::AccountManager::instance().create_session(
                            result.user.email, result.user.access_token,
                            result.user.refresh_token, result.user.expires_at);
                        wizard_state.current_step = 4;
                        wizard_state.success_message =
                            "Email verified. You can install mods and create profiles now. Minecraft sign-in is handled by the official launcher when you press Play.";
                        ImGui::CloseCurrentPopup();
                    } else {
                        wizard_state.error_message = result.error.empty() ? 
                            "Verification failed" : humanize_error(result.error);
                    }
                } else {
                    wizard_state.error_message = "Please enter the verification code from the email";
                }
            }
        }
    }
    
    // Update resend timer
    if (wizard_state.code_resend_seconds > 0 && !wizard_state.working) {
        static auto last_time = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_time).count();
        
        if (elapsed >= 1) {
            wizard_state.code_resend_seconds -= static_cast<int>(std::min<long long>(elapsed, 2147483647LL));
            if (wizard_state.code_resend_seconds < 0) {
                wizard_state.code_resend_seconds = 0;
            }
            last_time = now;
        }
    }
}

// ---------------------------------------------------------------------------
// Step 3: Connect Microsoft Account
// ---------------------------------------------------------------------------

void draw_wizard_connect_microsoft(UiState& st) {
    auto& wizard_state = st.auth_wizard_state;
    
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted("Connect Minecraft");
    ImGui::PopFont();
    ImGui::TextColored(k.muted, "You can install mods and manage profiles without Microsoft sign-in. Connect Minecraft only if you want to use this optional in-app shortcut; Play will otherwise open the official launcher.");
    ImGui::Spacing();
    card_begin("##microsoft_how_it_works", ImVec2(-1, 0));
    ImGui::TextColored(k.muted, "HOW IT WORKS");
    ImGui::Spacing();
    ImGui::BulletText("Amalgam opens Microsoft’s secure verification page.");
    ImGui::BulletText("You enter a one-time code and sign in there.");
    ImGui::BulletText("Microsoft returns a protected play token to this Windows user.");
    card_end();
    
    ImGui::Spacing();
    
    // Check if Microsoft login is in progress
    if (st.login_wizard_open) {
        ImGui::TextColored(k.brand_hov, "Microsoft sign-in is in progress in a separate secure window.");
        ImGui::Spacing();
        
        // Show Microsoft login status
        std::string verification_uri;
        std::string user_code;
        {
            std::lock_guard<std::mutex> lock(st.auth_mu);
            verification_uri = st.login_verification_uri;
            user_code = st.login_user_code;
        }
        if (!verification_uri.empty()) {
            card_begin("##microsoft_inline_code", ImVec2(-1, 0));
            ImGui::TextColored(k.muted, "ONE-TIME CODE");
            ImGui::PushFont(f_h2);
            ImGui::TextColored(k.brand_hov, "%s", user_code.c_str());
            ImGui::PopFont();
            ImGui::TextColored(k.muted, "Enter it at Microsoft’s verification page.");
            card_end();
            ImGui::Spacing();
            if (primary_button("Open secure sign-in", ImVec2(ui_px(190.0f), ui_px(36.0f)))) {
                ShellExecuteA(nullptr, "open", verification_uri.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            ImGui::SameLine();
            if (ghost_button("Copy code", ImVec2(ui_px(110.0f), ui_px(36.0f)))) {
                if (OpenClipboard(nullptr)) {
                    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, user_code.size() + 1);
                    if (hMem) {
                        char* pMem = static_cast<char*>(GlobalLock(hMem));
                        if (pMem) {
                            memcpy(pMem, user_code.c_str(), user_code.size() + 1);
                            GlobalUnlock(hMem);
                            SetClipboardData(CF_TEXT, hMem);
                        }
                    }
                    CloseClipboard();
                }
            }
        } else if (st.auth_working) {
            ImGui::TextColored(k.muted, "Waiting for Microsoft confirmation...");
        } else if (!st.auth_status.empty()) {
            ImGui::TextColored(k.muted, "%s", st.auth_status.c_str());
        }
    } else {
        ImGui::Spacing();
        if (primary_button("Connect Microsoft account", ImVec2(-1, ui_px(40.0f)))) {
            start_microsoft_login(st);
        }
    }
    
    ImGui::Spacing();
    draw_security_note("Amalgam never sees your Microsoft password. You can connect later; the official Minecraft Launcher handles Minecraft authentication when you play Java Edition.");
    ImGui::Spacing();
    ImGui::TextColored(k.muted, "Want to learn more?");
    ImGui::SameLine();
    if (ghost_button("Microsoft account help", ImVec2(ui_px(188.0f), ui_px(28.0f)))) {
        ShellExecuteW(nullptr, L"open",
                      aml::net::to_wide(aml::online::config().microsoft_url()).c_str(),
                      nullptr, nullptr, SW_SHOWNORMAL);
    }
    
    // Check if Microsoft is connected
    auto& supabase = aml::supabase::SupabaseManager::instance();
    if (supabase.is_authenticated() && !st.account.username.empty()) {
        // Microsoft is connected, move to the final review step.
        wizard_state.current_step = 4;
    }
}

// ---------------------------------------------------------------------------
// Step 4: Complete
// ---------------------------------------------------------------------------

void draw_wizard_complete(UiState& st) {
    auto& wizard_state = st.auth_wizard_state;

    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted("You are ready to play");
    ImGui::PopFont();
    ImGui::TextColored(k.muted, "Your Amalgam account is ready. You can create isolated profiles and install content now; the official launcher handles Minecraft sign-in when you play.");
    ImGui::Spacing();

    card_begin("##account_setup_complete", ImVec2(-1, 0));
    const ImVec2 complete_origin = ImGui::GetCursorScreenPos();
    draw_icon(IconId::Check, complete_origin + ImVec2(ui_px(17.0f), ui_px(17.0f)), ui_px(10.0f), c32(k.green));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ui_px(38.0f));
    ImGui::PushFont(f_bold);
    ImGui::TextColored(k.green, "Account setup complete");
    ImGui::PopFont();

    // Show account identities in the completion card rather than scattered
    // labels, making the final confirmation easy to scan.
    auto& supabase = aml::supabase::SupabaseManager::instance();
    auto user = supabase.get_current_user();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ui_px(38.0f));
    if (!user.email.empty()) {
        ImGui::TextColored(k.muted, "AMALGAM ACCOUNT  %s", user.email.c_str());
    }
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ui_px(38.0f));
    if (!st.account.username.empty()) {
        ImGui::TextColored(k.muted, "MINECRAFT PROFILE  %s", st.account.username.c_str());
    } else {
        ImGui::TextColored(k.muted, "MINECRAFT AUTHENTICATION  Official launcher");
    }
    card_end();
    
    ImGui::Spacing();
    draw_form_label("What you can do next");
    ImGui::BulletText("Create and manage isolated Minecraft profiles");
    ImGui::BulletText("Install modpacks and content from Modrinth and CurseForge");
    ImGui::BulletText("Launch Minecraft Java and Bedrock from one launcher");
    ImGui::BulletText("Save servers and keep your launcher preferences connected");
    ImGui::Spacing();
    if (primary_button("Start using Amalgam", ImVec2(-1, ui_px(40.0f)))) {
        // Close wizard and reset state
        wizard_state = AuthWizardState();
        ImGui::CloseCurrentPopup();
        
        // Show home page
        st.sidebar_item = 0;
        st.active_tab = 0;
    }
}

// ---------------------------------------------------------------------------
// Microsoft Login Integration
// ---------------------------------------------------------------------------

void start_microsoft_login_from_wizard(UiState& st) {
    // Minecraft authentication is optional in Amalgam. The explicit connect
    // action opens the official launcher fallback; it must not require an
    // Amalgam-side client ID merely to finish Amalgam account setup.
    start_microsoft_login(st);
}

// ---------------------------------------------------------------------------
// Password Reset State
// ---------------------------------------------------------------------------

struct PasswordResetState {
    int current_step = 0; // 0=email, 1=code, 2=new_password
    std::string email;
    std::string verification_code;
    std::string new_password;
    std::string confirm_password;
    std::string error_message;
    std::string success_message;
    bool working = false;
    bool code_sent = false;
    int code_resend_seconds = 0;
};

static PasswordResetState& get_password_reset_state() {
    static PasswordResetState state;
    return state;
}

// ---------------------------------------------------------------------------
// Password Reset Dialog
// ---------------------------------------------------------------------------

void draw_password_reset_dialog(UiState& st) {
    auto& reset_state = get_password_reset_state();
    position_wizard_modal(640.0f, 560.0f);

    bool popup_open = true;
    if (!ImGui::BeginPopupModal("Reset Password", &popup_open,
                                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse)) {
        if (!popup_open) reset_state = PasswordResetState();
        return;
    }
    
    draw_wizard_hero(st, "ACCOUNT RECOVERY", "Reset your password",
                     "We will send a secure, time-limited confirmation to your email.",
                     reset_state.current_step, 3);
    ImGui::Spacing();
    
    switch (reset_state.current_step) {
        case 0: // Email step
            ImGui::PushFont(f_h2);
            ImGui::TextUnformatted("Find your account");
            ImGui::PopFont();
            ImGui::TextColored(k.muted, "Enter the email linked to your Amalgam account.");
            ImGui::Spacing();
            draw_form_label("Email address");
            ImGui::SetNextItemWidth(-1);
            input_text_hint("##reset_email", "you@example.com", &reset_state.email);
            
            ImGui::Spacing();
            
            if (reset_state.working) {
                ghost_button("Sending secure code...", ImVec2(-1, ui_px(40.0f)), true);
            } else {
                if (primary_button("Send secure reset code", ImVec2(-1, ui_px(40.0f)))) {
                    if (reset_state.email.empty()) {
                        reset_state.error_message = "Email is required";
                    } else {
                        reset_state.working = true;
                        reset_state.error_message.clear();
                        
                        auto& supabase = aml::supabase::SupabaseManager::instance();
                        auto response = supabase.request_password_reset(reset_state.email);
                        
                        reset_state.working = false;
                        
                        if (response.success) {
                            reset_state.current_step = 1;
                            reset_state.code_sent = true;
                            reset_state.code_resend_seconds = 60;
                            reset_state.success_message = "Reset code sent! Check your email.";
                        } else {
                            reset_state.error_message = response.error.empty() ? 
                                "Failed to send reset code" : humanize_error(response.error);
                        }
                    }
                }
            }
            break;
            
        case 1: // Verification code step
            ImGui::PushFont(f_h2);
            ImGui::TextUnformatted("Confirm it is you");
            ImGui::PopFont();
            ImGui::TextColored(k.muted, "Enter the six-digit code sent to %s.", reset_state.email.c_str());
            ImGui::Spacing();
            draw_form_label("Verification code");
            ImGui::SetNextItemWidth(-1);
            input_text_hint("##reset_code", "6-digit code", &reset_state.verification_code);
            
            ImGui::Spacing();
            
            // Resend code
            if (reset_state.code_resend_seconds > 0) {
                ImGui::TextColored(k.muted, "Resend code in %d seconds", reset_state.code_resend_seconds);
            } else {
                if (ghost_button("Resend code", ImVec2(ui_px(130.0f), ui_px(30.0f)))) {
                    auto& supabase = aml::supabase::SupabaseManager::instance();
                    auto response = supabase.request_password_reset(reset_state.email);
                    
                    if (response.success) {
                        reset_state.code_sent = true;
                        reset_state.code_resend_seconds = 60;
                        reset_state.success_message = "Reset code resent!";
                    } else {
                        reset_state.error_message = response.error.empty() ? 
                            "Failed to resend code" : humanize_error(response.error);
                    }
                }
            }
            
            ImGui::Spacing();
            if (primary_button("Verify code", ImVec2(-1, ui_px(40.0f)))) {
                if (reset_state.verification_code.length() == 6) {
                    reset_state.working = true;
                    reset_state.error_message.clear();
                    
                    auto& supabase = aml::supabase::SupabaseManager::instance();
                    auto result = supabase.client()->verify_otp(
                        reset_state.email, reset_state.verification_code, "recovery");
                    
                    reset_state.working = false;
                    
                    if (result.success) {
                        reset_state.current_step = 2;
                    } else {
                        reset_state.error_message = result.error.empty() ? 
                            "Verification failed" : humanize_error(result.error);
                    }
                } else {
                    reset_state.error_message = "Please enter a valid 6-digit code";
                }
            }
            break;
            
        case 2: // New password step
            ImGui::PushFont(f_h2);
            ImGui::TextUnformatted("Choose a new password");
            ImGui::PopFont();
            ImGui::TextColored(k.muted, "Use at least eight characters. A longer, unique password is best.");
            ImGui::Spacing();
            draw_form_label("New password");
            ImGui::SetNextItemWidth(-1);
            input_secret("##reset_new_password", &reset_state.new_password);
            
            ImGui::Spacing();
            
            draw_form_label("Confirm new password");
            ImGui::SetNextItemWidth(-1);
            input_secret("##reset_confirm_password", &reset_state.confirm_password);
            
            ImGui::Spacing();
            if (primary_button("Reset password", ImVec2(-1, ui_px(40.0f)))) {
                if (reset_state.new_password.empty() || reset_state.confirm_password.empty()) {
                    reset_state.error_message = "Please enter and confirm your new password";
                } else if (reset_state.new_password != reset_state.confirm_password) {
                    reset_state.error_message = "Passwords do not match";
                } else {
                    reset_state.working = true;
                    reset_state.error_message.clear();
                    
                    auto& supabase = aml::supabase::SupabaseManager::instance();
                    auto result = supabase.client()->change_password("", reset_state.new_password);
                    
                    reset_state.working = false;
                    
                    if (result.success) {
                        reset_state.success_message = "Password reset successfully! You can now sign in.";
                        reset_state.current_step = 0;
                        reset_state.email.clear();
                        reset_state.verification_code.clear();
                        reset_state.new_password.clear();
                        reset_state.confirm_password.clear();
                        
                        ImGui::CloseCurrentPopup();
                    } else {
                        reset_state.error_message = result.error.empty() ? 
                            "Failed to reset password" : humanize_error(result.error);
                    }
                }
            }
            break;
    }
    
    ImGui::Spacing();
    draw_wizard_feedback("##reset_error", reset_state.error_message, true, "Password reset needs attention");
    draw_wizard_feedback("##reset_success", reset_state.success_message, false);
    if (!reset_state.error_message.empty() || !reset_state.success_message.empty()) ImGui::Spacing();
    draw_security_note("Reset codes expire quickly and can only be used for this password recovery request.");
    
    // Update resend timer
    if (reset_state.code_resend_seconds > 0 && !reset_state.working) {
        static auto last_time = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_time).count();
        
        if (elapsed >= 1) {
            reset_state.code_resend_seconds -= static_cast<int>(std::min<long long>(elapsed, 2147483647LL));
            if (reset_state.code_resend_seconds < 0) {
                reset_state.code_resend_seconds = 0;
            }
            last_time = now;
        }
    }

    ImGui::Spacing();
    if (ghost_button("Close", ImVec2(ui_px(96.0f), ui_px(32.0f)))) {
        popup_open = false;
        ImGui::CloseCurrentPopup();
    }
    
    ImGui::EndPopup();
    if (!popup_open) {
        reset_state = PasswordResetState();
        st.password_reset_popup_open = false;
    }
}

// ---------------------------------------------------------------------------
// Login Wizard (for existing users)
// ---------------------------------------------------------------------------

void draw_amalgam_login_wizard(UiState& st) {
    static std::string login_email;
    static std::string login_password;
    static bool login_remember = true;
    static bool show_password = false;
    static std::string login_error;
    static bool login_working = false;

    position_wizard_modal(660.0f, 650.0f);
    bool popup_open = true;
    if (!ImGui::BeginPopupModal("Sign In to Amalgam", &popup_open,
                                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse)) {
        if (!popup_open) {
            login_password.clear();
            login_error.clear();
            login_working = false;
        }
        return;
    }

    auto close_login = [&]() {
        popup_open = false;
        login_password.clear();
        login_error.clear();
        login_working = false;
        ImGui::CloseCurrentPopup();
    };

    draw_wizard_hero(st, "AMALGAM ACCOUNT", "Welcome back",
                     "Sign in to keep your launcher preferences, social features, and supported profiles connected.");
    ImGui::Spacing();
    card_begin("##amalgam_login_form", ImVec2(-1, 0));
    draw_form_label("Email address");
    ImGui::SetNextItemWidth(-1);
    if (input_text_hint("##login_email", "you@example.com", &login_email)) login_error.clear();
    ImGui::Spacing();
    draw_form_label("Password");
    const float password_button_width = ui_px(84.0f);
    ImGui::SetNextItemWidth(std::max(ui_px(180.0f), ImGui::GetContentRegionAvail().x - password_button_width - ui_px(8.0f)));
    const ImGuiInputTextFlags password_flags = show_password ? ImGuiInputTextFlags_None : ImGuiInputTextFlags_Password;
    if (ImGui::InputText("##login_password", &login_password, password_flags)) login_error.clear();
    ImGui::SameLine(0, ui_px(8.0f));
    if (ghost_button(show_password ? "Hide" : "Show", ImVec2(password_button_width, ui_px(32.0f))))
        show_password = !show_password;
    ImGui::Spacing();
    ImGui::Checkbox("Keep me signed in on this Windows device", &login_remember);
    card_end();

    ImGui::Spacing();
    draw_wizard_feedback("##login_error", login_error, true, "Sign-in needs attention");
    if (!login_error.empty()) ImGui::Spacing();
    if (login_working) {
        ghost_button("Signing in securely...", ImVec2(-1, ui_px(40.0f)), true);
    } else {
        if (primary_button("Sign in", ImVec2(-1, ui_px(40.0f)))) {
            if (login_email.empty()) {
                login_error = "Enter the email address for your Amalgam account.";
            } else if (login_password.empty()) {
                login_error = "Enter your password to continue.";
            } else {
                login_working = true;
                login_error.clear();
                
                auto& supabase = aml::supabase::SupabaseManager::instance();
                auto response = supabase.sign_in(login_email, login_password);
                
                login_working = false;
                
                if (response.success) {
                    if (login_remember && !response.user.access_token.empty()) {
                        aml::account::AccountManager::instance().create_session(
                            response.user.email, response.user.access_token,
                            response.user.refresh_token, response.user.expires_at);
                    }
                    // Sign in successful. Microsoft authentication is separate
                    // and is handled by the official launcher when the user plays.
                    ImGui::CloseCurrentPopup();
                    st.auth_prompt_dismissed = true;
                } else {
                    login_error = response.error.empty()
                        ? "We could not sign you in. Check your email and password, then try again."
                        : humanize_error(response.error);
                }
            }
        }
    }
    ImGui::Spacing();
    draw_security_note("Your password stays with the Amalgam account service. Microsoft sign-in happens separately in Microsoft’s browser page.");
    ImGui::Spacing();
    ImGui::TextColored(k.muted, "Forgot your password?");
    ImGui::SameLine();
    if (ghost_button("Reset it", ImVec2(ui_px(92.0f), ui_px(30.0f)))) {
        ImGui::CloseCurrentPopup();
        // Queue the next modal for the following frame. Opening a sibling
        // popup while closing this one is timing-sensitive in Dear ImGui.
        st.password_reset_popup_open = true;
    }
    ImGui::Spacing();
    ImGui::TextColored(k.muted, "New to Amalgam?");
    ImGui::SameLine();
    if (ghost_button("Create a free account", ImVec2(ui_px(180.0f), ui_px(30.0f)))) {
        ImGui::CloseCurrentPopup();
        st.wizard_open = false;
        st.auth_wizard_state = AuthWizardState();
        // Use the shared modal hand-off queue so sign-in -> registration
        // always lands in the next dialog on the next render frame.
        st.register_popup_open = true;
    }

    ImGui::Spacing();
    if (ghost_button("Close", ImVec2(ui_px(96.0f), ui_px(32.0f)))) close_login();
    
    ImGui::EndPopup();
    if (!popup_open) {
        login_password.clear();
        login_error.clear();
        login_working = false;
    }
}

// ---------------------------------------------------------------------------
// Account Button with Wizard
// ---------------------------------------------------------------------------

void draw_account_button_with_wizard(UiState& st) {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    
    if (supabase.is_authenticated()) {
        // Show user info
        auto user = supabase.get_current_user();
        
        ImGui::PushFont(f_bold);
        if (!user.email.empty()) {
            ImGui::TextUnformatted(user.email.c_str());
        } else {
            ImGui::TextUnformatted("Amalgam User");
        }
        ImGui::PopFont();
        ImGui::SameLine();
        
        if (ghost_button("v", ImVec2(ui_px(24.0f), ui_px(24.0f)))) {
            // Open account menu
            ImGui::OpenPopup("Account Menu");
        }
        
        // Account menu popup
        if (ImGui::BeginPopup("Account Menu")) {
            ImGui::TextUnformatted("Account");
            ImGui::Separator();
            
            if (ImGui::MenuItem("Profile")) {
                navigate_to(st, 10, 10);
            }
            
            if (ImGui::MenuItem("Settings")) {
                // Open settings
            }
            
            if (ImGui::MenuItem("Sign Out")) {
                aml::account::AccountManager::instance().end_current_session();
                supabase.sign_out();
                st.account.username.clear();
                
                push_notice(st, ui_model::NoticeLevel::Info, "Signed out", 
                            "You have been signed out of Amalgam");
            }
            
            ImGui::EndPopup();
        }
    } else {
        // Show sign in/create account button
        if (ghost_button("Sign In / Create Account", ImVec2(ui_px(180.0f), ui_px(32.0f)))) {
            st.auth_prompt_dismissed = false;
            ImGui::OpenPopup("Amalgam Account Setup");
        }
        
        // Open the auth wizard
        draw_auth_wizard(st);
        draw_amalgam_login_wizard(st);
    }
}

// ---------------------------------------------------------------------------
// Main Auth Wizard Entry Point
// ---------------------------------------------------------------------------

void show_auth_wizard_if_needed(UiState& st) {
    // Check if we need to show the auth wizard
    // This is called from the main UI loop
    
    auto& supabase = aml::supabase::SupabaseManager::instance();
    
    // If Supabase is not initialized, don't show wizard yet
    if (!supabase.is_initialized()) {
        return;
    }
    
    // Check if user is authenticated with Amalgam
    if (!supabase.is_authenticated() && !st.auth_prompt_dismissed) {
        // Not authenticated - show auth wizard
        ImGui::OpenPopup("Amalgam Account Setup");
        draw_auth_wizard(st);
        draw_amalgam_login_wizard(st);
        return;
    }
    
    // Microsoft authentication is deliberately not part of Amalgam account
    // onboarding. Local profiles, mods, Java, AI, and servers remain usable
    // while the official launcher owns Minecraft sign-in at Play time.
    
    // Amalgam authenticated - no wizard needed
    // But check if wizard is already open
    if (ImGui::IsPopupOpen("Amalgam Account Setup")) {
        draw_auth_wizard(st);
        draw_amalgam_login_wizard(st);
    }
}

}  // namespace aml::ui
