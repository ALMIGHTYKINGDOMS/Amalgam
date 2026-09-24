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
#include <exception>
#include <functional>
#include <map>
#include <thread>
#include <utility>

namespace aml::ui {

namespace {

static void cancel_wizard_auth_request(UiState& st);
static void clear_wizard_secrets(AuthWizardState& wizard_state);

// Browser account actions always target the configured official-site origin.
// No credentials, Supabase tokens, or user-provided redirect destinations are
// ever included in these URLs.
void open_website_auth_page(const std::string& url) {
    const std::wstring wide_url = aml::net::to_wide(url);
    if (!wide_url.empty()) {
        ShellExecuteW(nullptr, L"open", wide_url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

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

// A multi-field form needs a stable viewport before its child body can reserve
// space for a persistent footer.  Unlike the short account steps, do not let
// the parent auto-fit this surface from a pre-layout child size.
void position_bounded_wizard_form_modal(float preferred_width) {
    ImGui::SetNextWindowSize(wizard_modal_size(preferred_width, 600.0f),
                             ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
}

// Account setup and recovery have short states as well as long validation and
// working states.  Let those modals fit short content instead of reserving a
// large empty panel, while still capping a long form inside the visible work
// area.  The tighter edge allowance is intentional: at 960x600 it preserves
// an obvious outer margin without taking action or feedback space away.
void position_content_aware_wizard_modal(float preferred_width, float minimum_height) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float max_width = std::max(ui_px(420.0f), viewport->WorkSize.x - ui_px(24.0f));
    const float max_height = std::max(ui_px(360.0f), viewport->WorkSize.y - ui_px(24.0f));
    const ImVec2 minimum(std::min(ui_px(preferred_width), max_width),
                         std::min(ui_px(minimum_height), max_height));
    ImGui::SetNextWindowSizeConstraints(minimum, ImVec2(max_width, max_height));
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
}

bool compact_wizard_viewport() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    return viewport && viewport->WorkSize.y <= ui_px(640.0f);
}

void draw_wizard_hero(UiState& st, const char* eyebrow, const char* title, const char* subtitle,
                      int current_step = -1, int total_steps = 0) {
    const float height = ui_px(compact_wizard_viewport() ? 96.0f : 112.0f);
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

// Keep the request-in-progress treatment distinct from a disabled button.  A
// real account request can take a moment, and the same component also gives
// the deterministic fixture a truthful, provider-free representation of that
// state.
void draw_account_creation_working_notice(const char* id, const char* detail) {
    card_begin(id, ImVec2(-1, 0));
    draw_loading_spinner(ui_px(18.0f));
    ImGui::SameLine(0, ui_px(12.0f));
    ImGui::PushFont(f_bold);
    ImGui::TextUnformatted("Creating your account securely");
    ImGui::PopFont();
    ImGui::TextColored(k.muted, "%s", detail);
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
    const float label_gap = ui_px(compact_wizard_viewport() ? 4.0f : 7.0f);
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
        draw->AddText(ImVec2(x - label_size.x * 0.5f, y + circle_radius + label_gap),
                      c32(i <= current_step ? k.text : k.muted), steps[i]);
    }
    ImGui::Dummy(ImVec2(0.0f, circle_radius * 2.0f +
                                  ui_px(compact_wizard_viewport() ? 24.0f : 34.0f)));
}

void dismiss_auth_setup(UiState& st) {
    // Do not leave passwords, verification codes, or a stale step in memory
    // after the user closes the wizard.  The next explicit open starts clean.
    cancel_wizard_auth_request(st);
    clear_wizard_secrets(st.auth_wizard_state);
    st.auth_wizard_state = AuthWizardState();
    st.wizard_open = false;
    st.register_popup_open = false;
    st.auth_prompt_dismissed = true;
}

// Account-dialog fixture surfaces need to remain deterministic if a future
// review harness reuses a process that previously authenticated.  Render a
// local, inert representation instead of resolving Supabase, retained device
// codes, or function-static password fields.
static bool begin_fixture_auth_popup(UiState& st, const char* popup,
                                     const char* eyebrow, const char* title,
                                     const char* subtitle, bool content_aware = false) {
    if (content_aware) position_content_aware_wizard_modal(640.0f, 430.0f);
    else position_wizard_modal(660.0f, 520.0f);
    bool popup_open = true;
    if (!ImGui::BeginPopupModal(popup, &popup_open,
                                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                    (content_aware ? ImGuiWindowFlags_AlwaysAutoResize
                                                   : ImGuiWindowFlags_None)))
        return false;
    draw_wizard_hero(st, eyebrow, title, subtitle);
    ImGui::Spacing();
    ImGui::TextColored(k.brand_hov,
                       "Visual fixture: local form preview only; no account, code, or provider state is used.");
    ImGui::Spacing();
    return true;
}

static void draw_fixture_register_dialog(UiState& st) {
    if (!begin_fixture_auth_popup(st, "Amalgam Account Setup", "AMALGAM ACCOUNT",
                                  "Create your account", "A safe preview of the registration journey."))
        return;
    std::string field;
    card_begin("##fixture_register_form");
    ImGui::BeginDisabled();
    draw_form_label("Email address");
    ImGui::InputTextWithHint("##fixture_register_email", "you@example.com", &field);
    draw_form_label("Username");
    ImGui::InputTextWithHint("##fixture_register_username", "Choose a username", &field);
    draw_form_label("Password");
    ImGui::InputTextWithHint("##fixture_register_password", "At least 8 characters", &field,
                             ImGuiInputTextFlags_Password);
    primary_button("Create free account", ImVec2(-1, ui_px(40.0f)), false, true);
    ImGui::EndDisabled();
    card_end();
    ImGui::EndPopup();
}

static void draw_fixture_login_dialog(UiState& st) {
    if (!begin_fixture_auth_popup(st, "Sign In to Amalgam", "AMALGAM ACCOUNT",
                                  "Welcome back", "A safe preview of the sign-in journey."))
        return;
    std::string field;
    card_begin("##fixture_login_form");
    ImGui::BeginDisabled();
    draw_form_label("Email address");
    ImGui::InputTextWithHint("##fixture_login_email", "you@example.com", &field);
    draw_form_label("Password");
    ImGui::InputTextWithHint("##fixture_login_password", "Your password", &field,
                             ImGuiInputTextFlags_Password);
    bool remember = true;
    ImGui::Checkbox("Keep me signed in on this Windows device", &remember);
    primary_button("Sign in", ImVec2(-1, ui_px(40.0f)), false, true);
    ImGui::EndDisabled();
    card_end();
    ImGui::EndPopup();
}

static void draw_fixture_password_reset_dialog(UiState& st) {
    if (!begin_fixture_auth_popup(st, "Reset Password", "ACCOUNT RECOVERY",
                                  "Reset your password", "A safe preview of the recovery journey.", true))
        return;
    std::string field;
    card_begin("##fixture_reset_form");
    ImGui::BeginDisabled();
    draw_form_label("Email address");
    ImGui::InputTextWithHint("##fixture_reset_email", "you@example.com", &field);
    primary_button("Send secure reset code", ImVec2(-1, ui_px(40.0f)), false, true);
    ImGui::EndDisabled();
    card_end();
    ImGui::TextColored(k.muted, "Reset codes and recovery requests are disabled during visual review.");
    ImGui::EndPopup();
}

static void draw_fixture_microsoft_login_dialog(UiState& st) {
    if (st.microsoft_login_popup_open) ImGui::OpenPopup("Microsoft Sign In");
    if (!begin_fixture_auth_popup(st, "Microsoft Sign In", "MINECRAFT ACCESS",
                                  "Connect Microsoft", "A safe preview of the one-time browser handoff."))
        return;
    card_begin("##fixture_microsoft_signin");
    ImGui::TextColored(k.muted, "ONE-TIME BROWSER CODE");
    ImGui::PushFont(f_title);
    ImGui::TextColored(k.brand_hov, "REVIEW-ONLY");
    ImGui::PopFont();
    ImGui::TextColored(k.muted, "The real code and browser destination are never requested for a visual fixture.");
    ImGui::Spacing();
    ImGui::BeginDisabled();
    primary_button("Open secure Microsoft sign-in", ImVec2(ui_px(238.0f), ui_px(38.0f)), false, true);
    ImGui::SameLine();
    ghost_button("Copy code", ImVec2(ui_px(120.0f), ui_px(38.0f)));
    ImGui::EndDisabled();
    card_end();
    ImGui::EndPopup();
}

// The original account-dialog fixtures deliberately kept a very small generic
// preview.  That made a smoke test possible, but it did not let visual review
// distinguish a validation failure from an email-verification, recovery, or
// Microsoft-device-code state.  The helpers below render the same information
// hierarchy as the live flows with inert, clearly synthetic data.  They never
// construct account services, request a device code, retain a password, or
// perform a browser handoff.
static bool is_account_setup_fixture(const std::string& fixture_case) {
    return fixture_case == "wizard-account-welcome" ||
           fixture_case == "wizard-account-create" ||
           fixture_case == "wizard-account-create-validation" ||
           fixture_case == "wizard-account-verify-email" ||
           fixture_case == "wizard-account-connect-minecraft" ||
           fixture_case == "wizard-account-complete" ||
           fixture_case == "wizard-account-error" ||
           fixture_case == "wizard-account-working";
}

static bool is_login_fixture(const std::string& fixture_case) {
    return fixture_case == "dialog-sign-in-validation" ||
           fixture_case == "dialog-sign-in-working" ||
           fixture_case == "dialog-sign-in-error";
}

static bool is_password_reset_fixture(const std::string& fixture_case) {
    return fixture_case == "wizard-password-reset-email" ||
           fixture_case == "wizard-password-reset-code" ||
           fixture_case == "wizard-password-reset-new-password" ||
           fixture_case == "wizard-password-reset-working" ||
           fixture_case == "wizard-password-reset-error" ||
           fixture_case == "wizard-password-reset-success";
}

static bool is_microsoft_login_fixture(const std::string& fixture_case) {
    return fixture_case == "dialog-microsoft-sign-in-starting" ||
           fixture_case == "dialog-microsoft-sign-in-code" ||
           fixture_case == "dialog-microsoft-sign-in-waiting" ||
           fixture_case == "dialog-microsoft-sign-in-connected" ||
           fixture_case == "dialog-microsoft-sign-in-error" ||
           fixture_case == "dialog-microsoft-sign-in-official-launcher-fallback";
}

static bool begin_staged_fixture_popup(const char* popup, float width, float height,
                                       bool content_aware = false,
                                       bool bounded_form_body = false) {
    if (bounded_form_body) position_bounded_wizard_form_modal(width);
    else if (content_aware) position_content_aware_wizard_modal(width, height);
    else position_wizard_modal(width, height);
    bool popup_open = true;
    return ImGui::BeginPopupModal(popup, &popup_open,
                                  ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                      (content_aware && !bounded_form_body
                                           ? ImGuiWindowFlags_AlwaysAutoResize
                                           : ImGuiWindowFlags_None));
}

static void draw_fixture_text_input(const char* id, const char* label, const char* hint,
                                    const char* value = "", bool secret = false,
                                    const char* detail = nullptr) {
    draw_form_label(label, detail);
    std::string sample = value;
    ImGui::SetNextItemWidth(-1);
    ImGui::BeginDisabled();
    if (secret) input_secret(id, &sample);
    else input_text_hint(id, hint, &sample);
    ImGui::EndDisabled();
}

static void draw_fixture_primary_action(const char* label) {
    ImGui::BeginDisabled();
    primary_button(label, ImVec2(-1, ui_px(40.0f)), false, true);
    ImGui::EndDisabled();
}

static void draw_fixture_wizard_footer(int current_step, int total_steps) {
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::BeginDisabled();
    if (current_step > 0 && current_step < total_steps - 1) {
        ghost_button("Back", ImVec2(ui_px(100.0f), ui_px(32.0f)));
        ImGui::SameLine(ImGui::GetWindowWidth() - ui_px(116.0f));
    } else {
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - ui_px(116.0f));
    }
    ghost_button("Close", ImVec2(ui_px(96.0f), ui_px(32.0f)));
    ImGui::EndDisabled();
}

static void draw_fixture_account_wizard(UiState& st) {
    const std::string& fixture_case = st.fixture_case;
    const bool validation = fixture_case == "wizard-account-create-validation";
    const bool error = fixture_case == "wizard-account-error";
    const bool working = fixture_case == "wizard-account-working";
    int step = 0;
    if (fixture_case == "wizard-account-create" || validation || error || working) step = 1;
    else if (fixture_case == "wizard-account-verify-email") step = 2;
    else if (fixture_case == "wizard-account-connect-minecraft") step = 3;
    else if (fixture_case == "wizard-account-complete") step = 4;

    if (!begin_staged_fixture_popup("Amalgam Account Setup", 760.0f, 480.0f, true,
                                    step == 1)) return;
    draw_wizard_hero(st, "AMALGAM ACCOUNT", "Welcome to your world",
                     "A safe, local representation of the account setup journey.", step, 5);
    ImGui::Spacing();
    draw_auth_progress(step);
    ImGui::Separator();
    ImGui::Spacing();

    // The production form deliberately keeps validation and provider feedback
    // adjacent to the submit action at the end of the form.  Fixtures capture
    // the top of a bounded form body, so stage the same feedback component in
    // the fixed lead area as well.  This remains inert: no account state,
    // request, or successful identity is synthesized for visual evidence.
    if (step == 1) {
        if (validation) {
            draw_wizard_feedback("##fixture_account_validation",
                                 "Add a valid email, matching password, and accept both required policies before continuing.",
                                 true, "A few details need attention");
            ImGui::Spacing();
        } else if (error) {
            draw_wizard_feedback("##fixture_account_error",
                                 "We could not create this account. Check the details and try again; no fixture request was sent.",
                                 true, "Account setup needs attention");
            ImGui::Spacing();
        } else if (working) {
            draw_account_creation_working_notice(
                "##fixture_account_working",
                "Fixture preview only — no account request is running.");
            ImGui::Spacing();
        }
        if (compact_wizard_viewport()) {
            ImGui::TextColored(k.muted,
                               "Scroll within the form below to review consent and continue.");
            ImGui::Spacing();
        }
    }

    const ImGuiWindowFlags content_flags = step == 1
        ? ImGuiWindowFlags_AlwaysVerticalScrollbar : ImGuiWindowFlags_None;
    const bool content_visible = ImGui::BeginChild(
        "##fixture_account_wizard_content", ImVec2(0.0f, -ui_px(56.0f)),
        ImGuiChildFlags_Borders, content_flags);

    if (content_visible && step == 0) {
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("Set up the essentials");
        ImGui::PopFont();
        ImGui::TextColored(k.muted,
                           "A free Amalgam account keeps launcher features connected. Minecraft stays separate.");
        ImGui::Spacing();
        struct Benefit { IconId icon; const char* title; const char* detail; };
        const Benefit benefits[] = {
            {IconId::Cube, "One launcher", "Manage Java, Bedrock, loaders, and isolated profiles in one place."},
            {IconId::Cloud, "Your setup, synced", "Keep supported preferences and social features connected."},
            {IconId::Lock, "Private by design", "Passwords stay with the account provider and are never shown here."},
        };
        for (int i = 0; i < 3; ++i) {
            const std::string id = "##fixture_account_welcome_" + std::to_string(i);
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
        draw_fixture_primary_action("Create free Amalgam account");
    } else if (content_visible && step == 1) {
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("Create your Amalgam account");
        ImGui::PopFont();
        ImGui::TextColored(k.muted, "Use an address you can access. Verification comes before Minecraft connection.");
        ImGui::Spacing();
        draw_fixture_text_input("##fixture_account_email", "Email address", "you@example.com",
                                validation ? "" : "qa-player@example.invalid",
                                false, "Used only for account access and confirmation.");
        ImGui::Spacing();
        draw_fixture_text_input("##fixture_account_username", "Username", "Choose a username",
                                validation ? "" : "qa_player", false, "Your unique Amalgam name.");
        ImGui::Spacing();
        draw_fixture_text_input("##fixture_account_display", "Display name", "Your display name",
                                "QA Player", false, "Optional — how you appear to other Amalgam users.");
        ImGui::Spacing();
        draw_fixture_text_input("##fixture_account_password", "Password", "At least 8 characters",
                                "FixturePass7", true, "At least 8 characters, with an uppercase letter and number.");
        ImGui::Spacing();
        draw_fixture_text_input("##fixture_account_confirm", "Confirm password", "Confirm your password",
                                validation ? "FixturePass6" : "FixturePass7", true);
        ImGui::Spacing();
        card_begin("##fixture_account_consent", ImVec2(-1, 0));
        ImGui::TextColored(k.muted, "ACCOUNT CONSENT");
        bool accepted = !validation;
        ImGui::BeginDisabled();
        ImGui::Checkbox("I accept the Terms of Service", &accepted);
        ImGui::Checkbox("I accept the Privacy Policy", &accepted);
        bool updates = false;
        ImGui::Checkbox("Send me optional product news and updates", &updates);
        ImGui::EndDisabled();
        card_end();
        ImGui::Spacing();
        if (working) {
            draw_fixture_primary_action("Creating account...");
        } else {
            draw_fixture_primary_action("Create account");
        }
        ImGui::Spacing();
        draw_security_note("Fixture data is local and inert. In the real flow, the password is sent only to the account provider over a secure connection.");
    } else if (content_visible && step == 2) {
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("Verify your email");
        ImGui::PopFont();
        ImGui::TextColored(k.muted, "Open the confirmation message, then return here to continue.");
        ImGui::Spacing();
        card_begin("##fixture_account_verify_inbox", ImVec2(-1, 0));
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        draw_icon(IconId::Bell, origin + ImVec2(ui_px(16.0f), ui_px(16.0f)), ui_px(9.0f), c32(k.brand_hov));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ui_px(38.0f));
        ImGui::PushFont(f_bold);
        ImGui::TextUnformatted("Check your inbox");
        ImGui::PopFont();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ui_px(38.0f));
        ImGui::TextColored(k.muted, "qa-player@example.invalid");
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ui_px(38.0f));
        ImGui::TextColored(k.muted, "Use the confirmation link, or the six-digit code if one was included.");
        card_end();
        ImGui::Spacing();
        draw_fixture_text_input("##fixture_account_verify_code", "Verification code", "6-digit code",
                                "123456", false, "Optional — only use a code included in the email.");
        ImGui::Spacing();
        draw_fixture_primary_action("Verify account");
        ImGui::Spacing();
        draw_security_note("Confirmation links and codes expire quickly. This review fixture never sends or verifies a code.");
    } else if (content_visible && step == 3) {
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("Connect Minecraft");
        ImGui::PopFont();
        ImGui::TextColored(k.muted, "Minecraft connection is optional. You can connect now or rely on the official Minecraft Launcher when you play.");
        ImGui::Spacing();
        card_begin("##fixture_account_connect_minecraft", ImVec2(-1, 0));
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        draw_icon(IconId::Globe, origin + ImVec2(ui_px(16.0f), ui_px(16.0f)), ui_px(9.0f), c32(k.brand_hov));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ui_px(38.0f));
        ImGui::PushFont(f_bold);
        ImGui::TextUnformatted("Microsoft browser sign-in");
        ImGui::PopFont();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ui_px(38.0f));
        ImGui::PushTextWrapPos();
        ImGui::TextColored(k.muted, "The real flow opens Microsoft’s secure browser page. Amalgam never sees your Microsoft password.");
        ImGui::PopTextWrapPos();
        card_end();
        ImGui::Spacing();
        draw_fixture_primary_action("Connect Microsoft account");
        ImGui::Spacing();
        draw_security_note("You can connect later. The official Minecraft Launcher remains the fallback for Minecraft authentication.");
    } else if (content_visible) {
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("You are ready to play");
        ImGui::PopFont();
        ImGui::TextColored(k.muted, "Your Amalgam account is ready. The official launcher handles Minecraft sign-in when you play.");
        ImGui::Spacing();
        card_begin("##fixture_account_complete", ImVec2(-1, 0));
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        draw_icon(IconId::Check, origin + ImVec2(ui_px(17.0f), ui_px(17.0f)), ui_px(10.0f), c32(k.green));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ui_px(38.0f));
        ImGui::PushFont(f_bold);
        ImGui::TextColored(k.green, "Account setup complete");
        ImGui::PopFont();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ui_px(38.0f));
        ImGui::TextColored(k.muted, "AMALGAM ACCOUNT  qa-player@example.invalid");
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ui_px(38.0f));
        ImGui::TextColored(k.muted, "MINECRAFT AUTHENTICATION  Official launcher");
        card_end();
        ImGui::Spacing();
        draw_form_label("What you can do next");
        ImGui::BulletText("Create and manage isolated Minecraft profiles");
        ImGui::BulletText("Install modpacks and content from supported sources");
        ImGui::BulletText("Launch Java and Bedrock from one launcher");
        ImGui::Spacing();
        draw_fixture_primary_action("Start using Amalgam");
    }

    // `@middle` and `@bottom` normally target the page host.  Account setup
    // intentionally owns one bounded form body, so honor the same fixture
    // request here on every fixture frame. Repeating it after the child has
    // established its range avoids a live wheel event and keeps the eventual
    // capture deterministic.
    if (content_visible && st.fixture_scroll_position > 0) {
        const float max_scroll = ImGui::GetScrollMaxY();
        if (max_scroll > 0.0f) {
            const float requested_scroll = st.fixture_scroll_position == 1
                ? max_scroll * 0.5f : max_scroll;
            ImGui::SetScrollY(requested_scroll);
        }
    }
    ImGui::EndChild();

    draw_fixture_wizard_footer(step, 5);
    ImGui::EndPopup();
}

static void draw_fixture_login_state(UiState& st) {
    const std::string& fixture_case = st.fixture_case;
    const bool validation = fixture_case == "dialog-sign-in-validation";
    const bool working = fixture_case == "dialog-sign-in-working";
    const bool error = fixture_case == "dialog-sign-in-error";
    if (!begin_staged_fixture_popup("Sign In to Amalgam", 660.0f, 650.0f)) return;
    draw_wizard_hero(st, "AMALGAM ACCOUNT", "Welcome back",
                     "Sign in to keep your launcher preferences and supported profiles connected.");
    ImGui::Spacing();
    card_begin("##fixture_login_state_form", ImVec2(-1, 0));
    draw_fixture_text_input("##fixture_login_state_email", "Email address", "you@example.com",
                            validation ? "" : "qa-player@example.invalid");
    ImGui::Spacing();
    draw_fixture_text_input("##fixture_login_state_password", "Password", "Your password",
                            validation ? "" : "FixturePass7", true);
    ImGui::Spacing();
    bool remember = true;
    ImGui::BeginDisabled();
    ImGui::Checkbox("Keep me signed in on this Windows device", &remember);
    ImGui::EndDisabled();
    card_end();
    ImGui::Spacing();
    if (validation) {
        draw_wizard_feedback("##fixture_login_validation",
                             "Enter the email address and password for your Amalgam account to continue.",
                             true, "Sign-in needs attention");
        ImGui::Spacing();
    } else if (error) {
        draw_wizard_feedback("##fixture_login_error",
                             "We could not sign you in. Check your email and password, then try again.",
                             true, "Sign-in needs attention");
        ImGui::Spacing();
    }
    if (working) {
        card_begin("##fixture_login_working", ImVec2(-1, 0));
        draw_loading_spinner(ui_px(18.0f));
        ImGui::SameLine(0, ui_px(12.0f));
        ImGui::PushFont(f_bold);
        ImGui::TextUnformatted("Signing in securely");
        ImGui::PopFont();
        ImGui::TextColored(k.muted, "Please keep this window open while we confirm the account.");
        card_end();
        ImGui::Spacing();
        draw_fixture_primary_action("Signing in securely...");
    } else {
        draw_fixture_primary_action("Sign in");
    }
    ImGui::Spacing();
    draw_security_note("Fixture data is local and inert. Microsoft sign-in happens separately in Microsoft’s browser page.");
    ImGui::Spacing();
    ImGui::TextColored(k.muted, "Forgot your password?  Reset it");
    ImGui::TextColored(k.muted, "New to Amalgam?  Create a free account");
    draw_fixture_wizard_footer(0, 1);
    ImGui::EndPopup();
}

static void draw_fixture_password_reset_state(UiState& st) {
    const std::string& fixture_case = st.fixture_case;
    const bool working = fixture_case == "wizard-password-reset-working";
    const bool error = fixture_case == "wizard-password-reset-error";
    const bool success = fixture_case == "wizard-password-reset-success";
    int step = 0;
    if (fixture_case == "wizard-password-reset-code") step = 1;
    if (fixture_case == "wizard-password-reset-new-password") step = 2;
    if (!begin_staged_fixture_popup("Reset Password", 640.0f, 430.0f, true)) return;
    draw_wizard_hero(st, "ACCOUNT RECOVERY", "Reset your password",
                     "We will send a secure, time-limited confirmation to your email.", step, 3);
    ImGui::Spacing();
    if (step == 0) {
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("Find your account");
        ImGui::PopFont();
        ImGui::TextColored(k.muted, "Enter the email linked to your Amalgam account.");
        ImGui::Spacing();
        draw_fixture_text_input("##fixture_reset_state_email", "Email address", "you@example.com",
                                "qa-player@example.invalid");
        ImGui::Spacing();
        if (working) {
            card_begin("##fixture_reset_working", ImVec2(-1, 0));
            draw_loading_spinner(ui_px(18.0f));
            ImGui::SameLine(0, ui_px(12.0f));
            ImGui::PushFont(f_bold);
            ImGui::TextUnformatted("Sending secure reset code");
            ImGui::PopFont();
            ImGui::TextColored(k.muted, "Please keep this window open while the request completes.");
            card_end();
            ImGui::Spacing();
            draw_fixture_primary_action("Sending secure code...");
        } else {
            draw_fixture_primary_action("Send secure reset code");
        }
    } else if (step == 1) {
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("Confirm it is you");
        ImGui::PopFont();
        ImGui::TextColored(k.muted, "Enter the six-digit code sent to qa-player@example.invalid.");
        ImGui::Spacing();
        draw_fixture_text_input("##fixture_reset_state_code", "Verification code", "6-digit code", "123456");
        ImGui::Spacing();
        ImGui::TextColored(k.muted, "Resend code in 60 seconds");
        ImGui::Spacing();
        draw_fixture_primary_action("Verify code");
    } else {
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("Choose a new password");
        ImGui::PopFont();
        ImGui::TextColored(k.muted, "Use at least eight characters. A longer, unique password is best.");
        ImGui::Spacing();
        draw_fixture_text_input("##fixture_reset_state_new_password", "New password", "New password", "FixturePass8", true);
        ImGui::Spacing();
        draw_fixture_text_input("##fixture_reset_state_confirm_password", "Confirm new password", "Confirm new password", "FixturePass8", true);
        ImGui::Spacing();
        draw_fixture_primary_action("Reset password");
    }
    if (error) {
        ImGui::Spacing();
        draw_wizard_feedback("##fixture_reset_error",
                             "We could not complete that recovery request. Check the code and try again.",
                             true, "Password reset needs attention");
    } else if (success) {
        ImGui::Spacing();
        draw_wizard_feedback("##fixture_reset_success",
                             "Your password was reset. You can now sign in with the new password.", false);
    }
    ImGui::Spacing();
    draw_security_note("Reset codes expire quickly and are used only for this recovery request. This fixture never sends a code.");
    draw_fixture_wizard_footer(step, 3);
    ImGui::EndPopup();
}

static void draw_fixture_microsoft_login_state(UiState& st) {
    const std::string& fixture_case = st.fixture_case;
    const bool code = fixture_case == "dialog-microsoft-sign-in-code" ||
                      fixture_case == "dialog-microsoft-sign-in-waiting";
    const bool waiting = fixture_case == "dialog-microsoft-sign-in-waiting";
    const bool connected = fixture_case == "dialog-microsoft-sign-in-connected";
    const bool fallback = fixture_case == "dialog-microsoft-sign-in-official-launcher-fallback";
    const bool error = fixture_case == "dialog-microsoft-sign-in-error";
    if (st.microsoft_login_popup_open) ImGui::OpenPopup("Microsoft Sign In");
    if (!begin_staged_fixture_popup("Microsoft Sign In", 700.0f, 540.0f)) return;
    draw_wizard_hero(st, "MINECRAFT ACCESS", "Connect Microsoft",
                     "Use Microsoft’s secure browser page, or choose the official Minecraft Launcher when you play.");
    ImGui::Spacing();
    if (code) {
        card_begin("##fixture_microsoft_device_code", ImVec2(-1, 0));
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        draw_icon(IconId::Globe, origin + ImVec2(ui_px(16.0f), ui_px(16.0f)), ui_px(9.0f), c32(k.brand_hov));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ui_px(38.0f));
        ImGui::PushFont(f_bold);
        ImGui::TextUnformatted("1. Enter this one-time code in Microsoft’s browser page");
        ImGui::PopFont();
        ImGui::Spacing();
        ImGui::PushFont(f_title);
        ImGui::TextColored(k.brand_hov, "QA-CODE-42");
        ImGui::PopFont();
        ImGui::TextColored(k.muted, "Sample fixture code only — it cannot sign in or identify an account.");
        ImGui::Spacing();
        ImGui::TextColored(k.muted, "2. Finish sign-in in the browser. This window continues automatically.");
        card_end();
        ImGui::Spacing();
        draw_fixture_primary_action("Open secure Microsoft sign-in");
        if (waiting) {
            ImGui::Spacing();
            draw_security_note("Waiting for confirmation from Microsoft. You may close this window and try again later if approval is still pending.");
        }
    } else if (connected) {
        draw_wizard_feedback("##fixture_microsoft_connected",
                             "Minecraft is connected and ready to use with your Amalgam profiles.", false);
        ImGui::Spacing();
        card_begin("##fixture_microsoft_profile", ImVec2(-1, 0));
        ImGui::TextColored(k.muted, "MINECRAFT PROFILE");
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("QA Player");
        ImGui::PopFont();
        ImGui::TextColored(k.muted, "Fixture identity only — no Microsoft session is present.");
        card_end();
        ImGui::Spacing();
        draw_fixture_primary_action("Continue to Amalgam");
    } else if (fallback) {
        draw_wizard_feedback("##fixture_microsoft_fallback",
                             "Direct sign-in is still waiting on Minecraft approval. Playing can continue through the official Minecraft Launcher.",
                             true, "Microsoft sign-in needs attention");
        ImGui::Spacing();
        draw_security_note("Choose Minecraft Launcher mode to hand a prepared profile to the official launcher, where you sign in with your own account.");
        ImGui::Spacing();
        draw_fixture_primary_action("Use Minecraft Launcher mode");
    } else if (error) {
        draw_wizard_feedback("##fixture_microsoft_error",
                             "Microsoft sign-in could not be completed. Try again when you are ready.",
                             true, "Microsoft sign-in needs attention");
        ImGui::Spacing();
        draw_fixture_primary_action("Try again");
    } else {
        card_begin("##fixture_microsoft_starting", ImVec2(-1, 0));
        draw_loading_spinner(ui_px(20.0f));
        ImGui::SameLine(0, ui_px(12.0f));
        ImGui::PushFont(f_bold);
        ImGui::TextUnformatted("Preparing secure Microsoft sign-in");
        ImGui::PopFont();
        ImGui::TextColored(k.muted, "We are requesting a one-time browser code. This normally takes a few seconds.");
        card_end();
    }
    draw_fixture_wizard_footer(0, 1);
    ImGui::EndPopup();
}

} // namespace

bool auth_async_request_is_working(const UiState& st, const std::string& action) {
    const auto snapshot = snapshot_async_ui_request(st.auth_async_request);
    return snapshot.working && snapshot.action == action;
}

bool auth_async_request_lane_busy(const UiState& st) {
    return async_ui_request_is_reserved(st.auth_async_request);
}

bool start_auth_async_request(UiState& st, const std::string& action,
                               std::function<AsyncUiRequestResult()> work) {
    // A completed result is still owned by its originating surface until it
    // consumes it on the UI thread.  Do not replace that result with a new
    // account mutation before the surface can validate its identity scope.
    if (async_ui_request_is_reserved(st.auth_async_request)) return false;
    uint64_t generation = 0;
    if (!begin_async_ui_request(st.auth_async_request, action, &generation)) {
        return false;
    }

    // UiState owns and joins this worker before it is destroyed. The worker
    // captures form snapshots plus only safe synchronization handles; it
    // never touches ImGui or mutable form state, and a late result cannot
    // replace a newer generation.
    spawn_worker(st, std::thread([&st, action, generation,
                                  work = std::move(work)]() mutable {
        AsyncUiRequestResult result;
        try {
            result = work();
        } catch (const std::exception&) {
            result.success = false;
            result.title = "Account request failed";
            result.detail = "The account service ended the request unexpectedly. Please try again.";
        } catch (...) {
            result.success = false;
            result.title = "Account request failed";
            result.detail = "The account service ended the request unexpectedly. Please try again.";
        }

        // During launcher shutdown there is no live surface to consume this
        // result.  join_workers() still waits for the request to finish.
        if (!st.shutting_down.load()) {
            complete_async_ui_request(st.auth_async_request, action, generation,
                                      std::move(result));
        }
    }));
    return true;
}

bool take_auth_async_request_result(UiState& st, const std::string& action,
                                    AsyncUiRequestSnapshot* completed) {
    const auto snapshot = snapshot_async_ui_request(st.auth_async_request);
    if (snapshot.action != action) return false;
    return take_async_ui_request_result(st.auth_async_request, completed);
}

void invalidate_auth_async_request(UiState& st, const std::string& action) {
    const auto snapshot = snapshot_async_ui_request(st.auth_async_request);
    if (snapshot.action == action && (snapshot.working || snapshot.has_result)) {
        invalidate_async_ui_request(st.auth_async_request);
    }
}

namespace {

constexpr const char* kAuthActionWizardSignUp = "auth-wizard-sign-up";
constexpr const char* kAuthActionWizardResendSignup = "auth-wizard-resend-signup";
constexpr const char* kAuthActionWizardConfirmEmail = "auth-wizard-confirm-email";
constexpr const char* kAuthActionWizardVerifySignupOtp = "auth-wizard-verify-signup-otp";
constexpr const char* kAuthActionLogin = "auth-login";
constexpr const char* kAuthActionResetRequest = "auth-reset-request";
constexpr const char* kAuthActionResetResend = "auth-reset-resend";
constexpr const char* kAuthActionResetVerify = "auth-reset-verify";
constexpr const char* kAuthActionResetPassword = "auth-reset-password";

static bool is_wizard_auth_action(const std::string& action) {
    return action == kAuthActionWizardSignUp ||
           action == kAuthActionWizardResendSignup ||
           action == kAuthActionWizardConfirmEmail ||
           action == kAuthActionWizardVerifySignupOtp;
}

static bool is_password_reset_action(const std::string& action) {
    return action == kAuthActionResetRequest || action == kAuthActionResetResend ||
           action == kAuthActionResetVerify || action == kAuthActionResetPassword;
}

static void wipe_auth_secret(std::string& value) {
    std::fill(value.begin(), value.end(), '\0');
    value.clear();
}

static void clear_wizard_secrets(AuthWizardState& wizard_state) {
    wipe_auth_secret(wizard_state.password);
    wipe_auth_secret(wizard_state.confirm_password);
    wipe_auth_secret(wizard_state.verification_code);
}

static AsyncUiRequestResult auth_response_result(
    const aml::supabase::SupabaseClient::AuthResponse& response,
    const char* success_title, const char* failure_title,
    const char* fallback_error) {
    AsyncUiRequestResult result;
    result.success = response.success;
    result.title = response.success ? success_title : failure_title;
    if (!response.success) {
        result.detail = response.error.empty() ? fallback_error : response.error;
        return result;
    }

    // These fields stay in the guarded, short-lived hand-off only until the
    // render thread can commit the remembered local session.  They are never
    // rendered or logged.
    result.payload_a = response.user.email;
    result.payload_b = response.user.access_token;
    result.payload_c = response.user.refresh_token;
    result.number_a = response.user.expires_at;
    return result;
}

static std::string auth_failure_detail(const AsyncUiRequestResult& result,
                                       const char* fallback) {
    return result.detail.empty() ? std::string(fallback) : humanize_error(result.detail);
}

static bool persist_wizard_session(AuthWizardState& wizard_state,
                                   const AsyncUiRequestResult& result) {
    if (result.payload_b.empty()) {
        wizard_state.error_message =
            "The account service completed the request but did not return a usable sign-in session. Please try again.";
        return false;
    }
    const std::string& email = result.payload_a.empty() ? wizard_state.email : result.payload_a;
    if (email.empty() || !aml::account::AccountManager::instance().create_session(
                             email, result.payload_b, result.payload_c, result.number_a)) {
        wizard_state.error_message =
            "You were signed in, but Amalgam could not safely save the protected local session. Please try again.";
        return false;
    }
    clear_wizard_secrets(wizard_state);
    return true;
}

static void cancel_wizard_auth_request(UiState& st) {
    const auto snapshot = snapshot_async_ui_request(st.auth_async_request);
    if ((snapshot.working || snapshot.has_result) && is_wizard_auth_action(snapshot.action)) {
        invalidate_async_ui_request(st.auth_async_request);
    }
}

static void cancel_password_reset_request(UiState& st) {
    const auto snapshot = snapshot_async_ui_request(st.auth_async_request);
    if ((snapshot.working || snapshot.has_result) && is_password_reset_action(snapshot.action)) {
        invalidate_async_ui_request(st.auth_async_request);
    }
}

static void cancel_login_request(UiState& st) {
    invalidate_auth_async_request(st, kAuthActionLogin);
}

static void consume_wizard_auth_result(UiState& st) {
    const auto snapshot = snapshot_async_ui_request(st.auth_async_request);
    if (!is_wizard_auth_action(snapshot.action)) return;

    AsyncUiRequestSnapshot completed;
    if (!take_auth_async_request_result(st, snapshot.action, &completed)) return;

    auto& wizard_state = st.auth_wizard_state;
    wizard_state.working = false;
    const auto& result = completed.result;
    if (!result.success) {
        wizard_state.error_message = auth_failure_detail(
            result, "The account request could not be completed. Please try again.");
        return;
    }

    if (completed.action == kAuthActionWizardSignUp) {
        if (!result.payload_b.empty()) {
            if (!persist_wizard_session(wizard_state, result)) return;
            wizard_state.current_step = 4;
            wizard_state.success_message =
                "Account created. You can install mods and create profiles now. Minecraft sign-in is handled by the official launcher when you press Play.";
        } else {
            wizard_state.current_step = 2;
            wizard_state.code_sent = false;
            wipe_auth_secret(wizard_state.verification_code);
            wizard_state.success_message =
                "Account created. Check your email to finish verification.";
        }
        return;
    }

    if (completed.action == kAuthActionWizardResendSignup) {
        wizard_state.code_sent = true;
        wizard_state.code_resend_seconds = 60;
        wizard_state.success_message = "Verification email sent.";
        return;
    }

    if (!persist_wizard_session(wizard_state, result)) return;
    wizard_state.current_step = 4;
    wizard_state.success_message =
        "Email verified. You can install mods and create profiles now. Minecraft sign-in is handled by the official launcher when you press Play.";
}

static void draw_auth_lane_busy_hint(const UiState& st) {
    const auto snapshot = snapshot_async_ui_request(st.auth_async_request);
    if (snapshot.working && snapshot.action.empty()) {
        ImGui::TextColored(k.muted,
                           "A cancelled account request is finishing safely. You can retry in a moment.");
    } else if (snapshot.working) {
        ImGui::TextColored(k.muted,
                           "Another account request is in progress. Please wait for it to finish.");
    }
}

}  // namespace

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
    if (st.fixture_mode) {
        if (st.fixture_case == "dialog-register") draw_fixture_register_dialog(st);
        else if (is_account_setup_fixture(st.fixture_case)) draw_fixture_account_wizard(st);
        return;
    }
    // The provider can update its in-memory session in the worker before the
    // next frame. Consume the guarded result first so a successful sign-up or
    // verification has a chance to commit its protected local session before
    // the authenticated early-exit closes this wizard.
    consume_wizard_auth_result(st);
    auto& wizard_state = st.auth_wizard_state;
    const bool account_creation_step = wizard_state.current_step == 1;
    const auto account_request = snapshot_async_ui_request(st.auth_async_request);
    const bool wizard_request_pending = account_request.working &&
        is_wizard_auth_action(account_request.action);
    
    // Check if already authenticated
    auto& supabase = aml::supabase::SupabaseManager::instance();
    if (!wizard_request_pending && supabase.is_initialized() && supabase.is_authenticated()) {
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
    
    // Short account steps fit their content. The long create-account step
    // instead receives a viewport-clamped surface so its single bounded form
    // body has stable space for scrolling and the outer footer stays visible.
    if (account_creation_step) position_bounded_wizard_form_modal(760.0f);
    else position_content_aware_wizard_modal(760.0f, 480.0f);
    
    bool popup_open = true;
    if (!ImGui::BeginPopupModal("Amalgam Account Setup", &popup_open,
                                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                    (account_creation_step ? ImGuiWindowFlags_None
                                                           : ImGuiWindowFlags_AlwaysAutoResize))) {
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

    // This step has five fields and mandatory consent.  Its one bounded body
    // owns overflow so the surrounding wizard chrome, progress, and exit path
    // remain visible on a compact desktop.  The permanent rail makes the
    // continuation discoverable before a player reaches the lower fields.
    if (account_creation_step && compact_wizard_viewport()) {
        ImGui::TextColored(k.muted,
                           "Scroll within the form below to review consent and continue.");
        ImGui::Spacing();
    }
    const ImGuiWindowFlags content_flags = account_creation_step
        ? ImGuiWindowFlags_AlwaysVerticalScrollbar : ImGuiWindowFlags_None;
    const bool content_visible = ImGui::BeginChild(
        "##auth_wizard_step_content", ImVec2(0.0f, -ui_px(56.0f)),
        ImGuiChildFlags_Borders, content_flags);
    if (content_visible) {
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

        // Safe, actionable feedback stays adjacent to the action in the
        // scrollable form. A player who triggers validation at the bottom
        // therefore sees the exact recovery guidance without a jump to the
        // top of the wizard.
        draw_wizard_feedback("##auth_wizard_error", wizard_state.error_message, true,
                             "Account setup needs attention");
        draw_wizard_feedback("##auth_wizard_success", wizard_state.success_message, false);
        if (!wizard_state.error_message.empty() || !wizard_state.success_message.empty()) ImGui::Spacing();
    }
    ImGui::EndChild();

    ImGui::Separator();
    ImGui::Spacing();
    if (wizard_state.current_step > 0 && wizard_state.current_step < 4) {
        if (ghost_button("Back", ImVec2(ui_px(100.0f), ui_px(32.0f)))) {
            // Results from an abandoned step must not advance a now-different
            // form when their in-flight provider request eventually returns.
            cancel_wizard_auth_request(st);
            wizard_state.working = false;
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
    if (st.fixture_mode) {
        if (st.fixture_case == "dialog-microsoft-sign-in") draw_fixture_microsoft_login_dialog(st);
        else if (is_microsoft_login_fixture(st.fixture_case)) draw_fixture_microsoft_login_state(st);
        return;
    }
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
        // Treat a dialog hand-off as a close: no account-form secret may
        // survive behind the next popup.
        cancel_wizard_auth_request(st);
        clear_wizard_secrets(wizard_state);
        st.auth_wizard_state = AuthWizardState();
        st.wizard_open = false;
        st.login_popup_open = true;
        ImGui::CloseCurrentPopup();
    }
    ImGui::Spacing();
    if (ghost_button("Open official website", ImVec2(ui_px(190.0f), ui_px(30.0f)))) {
        open_website_auth_page(aml::online::config().login_url());
    }
    ImGui::TextColored(k.muted, "The website and launcher use the same Amalgam account.");
}

// ---------------------------------------------------------------------------
// Step 1: Create Account
// ---------------------------------------------------------------------------

void draw_wizard_create_account(UiState& st) {
    auto& wizard_state = st.auth_wizard_state;
    wizard_state.working = auth_async_request_is_working(st, kAuthActionWizardSignUp);
    const bool account_lane_busy = auth_async_request_lane_busy(st);
    const bool form_locked = wizard_state.working || account_lane_busy;
    ImGui::PushID("create_amalgam_account_form");
    
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted("Create your Amalgam account");
    ImGui::PopFont();
    ImGui::TextColored(k.muted, "Use an email you can access. You will verify it before we connect Minecraft.");
    ImGui::Spacing();
    ImGui::TextColored(k.muted, "Prefer the official website for registration?");
    ImGui::SameLine();
    if (ghost_button("Open website", ImVec2(ui_px(122.0f), ui_px(30.0f)))) {
        open_website_auth_page(aml::online::config().register_url());
    }
    ImGui::Spacing();

    if (form_locked) ImGui::BeginDisabled();
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
    if (form_locked) ImGui::EndDisabled();
    
    ImGui::Spacing();
    // Validate
    bool can_continue = !wizard_state.email.empty() &&
                       !wizard_state.username.empty() &&
                       wizard_state.password.size() >= 8 &&
                       wizard_state.password == wizard_state.confirm_password &&
                       wizard_state.accept_terms &&
                       wizard_state.accept_privacy;
    
    if (wizard_state.working) {
        draw_account_creation_working_notice(
            "##account_creation_working",
            "Please keep this window open while the secure account request completes.");
        ImGui::Spacing();
        primary_button("Creating account...", ImVec2(-1, ui_px(40.0f)), true, true);
    } else if (account_lane_busy) {
        ghost_button("Another account request is finishing...", ImVec2(-1, ui_px(40.0f)), true);
    } else if (can_continue) {
        if (primary_button("Create account", ImVec2(-1, ui_px(40.0f)))) {
            // Prepare metadata
            std::map<std::string, std::string> metadata;
            metadata["username"] = wizard_state.username;
            metadata["display_name"] = wizard_state.display_name;
            metadata["newsletter"] = wizard_state.receive_newsletter ? "true" : "false";
            const std::string email = wizard_state.email;
            std::string password = wizard_state.password;
            if (start_auth_async_request(
                    st, kAuthActionWizardSignUp,
                    [email, password, metadata = std::move(metadata)]() mutable {
                        try {
                            const auto response = aml::supabase::SupabaseManager::instance().sign_up(
                                email, password, metadata);
                            wipe_auth_secret(password);
                            return auth_response_result(response, "Account created",
                                                        "Account creation failed",
                                                        "Failed to create account");
                        } catch (...) {
                            wipe_auth_secret(password);
                            throw;
                        }
                    })) {
                wizard_state.working = true;
                wizard_state.error_message.clear();
                wizard_state.success_message.clear();
            } else {
                wizard_state.error_message =
                    "Another account request is still finishing. Please try again in a moment.";
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
    if (account_lane_busy && !wizard_state.working) draw_auth_lane_busy_hint(st);
    if (account_lane_busy && !wizard_state.working) ImGui::Spacing();
    draw_security_note("Your password is sent only to the account provider over a secure connection. Amalgam never displays or stores it in the launcher interface.");
    ImGui::PopID();
}

// ---------------------------------------------------------------------------
// Step 2: Verify Email
// ---------------------------------------------------------------------------

void draw_wizard_verify_email(UiState& st) {
    auto& wizard_state = st.auth_wizard_state;
    wizard_state.working =
        auth_async_request_is_working(st, kAuthActionWizardResendSignup) ||
        auth_async_request_is_working(st, kAuthActionWizardConfirmEmail) ||
        auth_async_request_is_working(st, kAuthActionWizardVerifySignupOtp);
    const bool account_lane_busy = auth_async_request_lane_busy(st);
    
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
    if (account_lane_busy) ImGui::BeginDisabled();
    input_text_hint("##wizard_code", "6-digit code", &wizard_state.verification_code);
    if (account_lane_busy) ImGui::EndDisabled();
    
    ImGui::Spacing();
    
    // Resend confirmation email/code
    if (wizard_state.code_resend_seconds > 0) {
        ImGui::TextColored(k.muted, "Resend email in %d seconds", wizard_state.code_resend_seconds);
    } else if (auth_async_request_is_working(st, kAuthActionWizardResendSignup)) {
        ghost_button("Sending verification email...", ImVec2(ui_px(220.0f), ui_px(30.0f)), true);
    } else if (account_lane_busy) {
        ghost_button("Resend verification email", ImVec2(ui_px(220.0f), ui_px(30.0f)), true);
    } else {
        if (ghost_button("Resend verification email", ImVec2(ui_px(220.0f), ui_px(30.0f)))) {
            const std::string email = wizard_state.email;
            if (start_auth_async_request(
                    st, kAuthActionWizardResendSignup, [email]() {
                        const auto response = aml::supabase::SupabaseManager::instance()
                                                  .resend_signup_confirmation(email);
                        return auth_response_result(response, "Verification email sent",
                                                    "Verification email failed",
                                                    "Failed to resend verification email");
                    })) {
                wizard_state.working = true;
                wizard_state.error_message.clear();
                wizard_state.success_message.clear();
            } else {
                wizard_state.error_message =
                    "Another account request is still finishing. Please try again in a moment.";
            }
        }
    }
    
    ImGui::Spacing();
    if (auth_async_request_is_working(st, kAuthActionWizardConfirmEmail)) {
        primary_button("Confirming email...", ImVec2(ui_px(206.0f), ui_px(38.0f)), true, true);
    } else if (account_lane_busy) {
        primary_button("I confirmed my email", ImVec2(ui_px(206.0f), ui_px(38.0f)), false, true);
    } else if (primary_button("I confirmed my email", ImVec2(ui_px(206.0f), ui_px(38.0f)))) {
        const std::string email = wizard_state.email;
        std::string password = wizard_state.password;
        if (start_auth_async_request(
                st, kAuthActionWizardConfirmEmail, [email, password]() mutable {
                    try {
                        const auto response = aml::supabase::SupabaseManager::instance().sign_in(
                            email, password);
                        wipe_auth_secret(password);
                        return auth_response_result(response, "Email confirmed", "Email confirmation failed",
                                                    "Open the confirmation link first, then try again.");
                    } catch (...) {
                        wipe_auth_secret(password);
                        throw;
                    }
                })) {
            wizard_state.working = true;
            wizard_state.error_message.clear();
            wizard_state.success_message.clear();
        } else {
            wizard_state.error_message =
                "Another account request is still finishing. Please try again in a moment.";
        }
    }
    ImGui::TextColored(k.muted, "The browser may return to a local page. Keep this window open, then use the button above after confirmation.");
    ImGui::Spacing();
    ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - ui_px(150.0f));
    
    // Check if code was sent
    if (!wizard_state.code_sent) {
        if (auth_async_request_is_working(st, kAuthActionWizardResendSignup)) {
            ghost_button("Sending verification email...", ImVec2(ui_px(246.0f), ui_px(36.0f)), true);
        } else if (account_lane_busy) {
            ghost_button("Send a new verification email", ImVec2(ui_px(246.0f), ui_px(36.0f)), true);
        } else if (ghost_button("Send a new verification email", ImVec2(ui_px(246.0f), ui_px(36.0f)))) {
            const std::string email = wizard_state.email;
            if (start_auth_async_request(
                    st, kAuthActionWizardResendSignup, [email]() {
                        const auto response = aml::supabase::SupabaseManager::instance()
                                                  .resend_signup_confirmation(email);
                        return auth_response_result(response, "Verification email sent",
                                                    "Verification email failed",
                                                    "Failed to send verification email");
                    })) {
                wizard_state.working = true;
                wizard_state.error_message.clear();
                wizard_state.success_message.clear();
            } else {
                wizard_state.error_message =
                    "Another account request is still finishing. Please try again in a moment.";
            }
        }
    } else {
        if (auth_async_request_is_working(st, kAuthActionWizardVerifySignupOtp)) {
            primary_button("Verifying...", ImVec2(ui_px(150.0f), ui_px(36.0f)), true, true);
        } else if (account_lane_busy) {
            primary_button("Verify code", ImVec2(ui_px(150.0f), ui_px(36.0f)), false, true);
        } else {
            if (primary_button("Verify code", ImVec2(ui_px(150.0f), ui_px(36.0f)))) {
                const bool valid_code = wizard_state.verification_code.size() == 6 &&
                    std::all_of(wizard_state.verification_code.begin(),
                                wizard_state.verification_code.end(),
                                [](unsigned char c) { return std::isdigit(c) != 0; });
                if (valid_code) {
                    const std::string email = wizard_state.email;
                    std::string code = wizard_state.verification_code;
                    if (start_auth_async_request(
                            st, kAuthActionWizardVerifySignupOtp,
                            [email, code]() mutable {
                                try {
                                    const auto response = aml::supabase::SupabaseManager::instance()
                                                              .verify_otp(email, code);
                                    wipe_auth_secret(code);
                                    return auth_response_result(response, "Email verified",
                                                                "Verification failed",
                                                                "Verification failed");
                                } catch (...) {
                                    wipe_auth_secret(code);
                                    throw;
                                }
                            })) {
                        wizard_state.working = true;
                        wizard_state.error_message.clear();
                        wizard_state.success_message.clear();
                    } else {
                        wizard_state.error_message =
                            "Another account request is still finishing. Please try again in a moment.";
                    }
                } else {
                    wizard_state.error_message = "Please enter the verification code from the email";
                }
            }
        }
    }

    if (account_lane_busy && !wizard_state.working) {
        ImGui::Spacing();
        draw_auth_lane_busy_hint(st);
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
        clear_wizard_secrets(wizard_state);
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
    bool close_after_success = false;
};

static PasswordResetState& get_password_reset_state() {
    static PasswordResetState state;
    return state;
}

static void clear_password_reset_secrets(PasswordResetState& state) {
    wipe_auth_secret(state.verification_code);
    wipe_auth_secret(state.new_password);
    wipe_auth_secret(state.confirm_password);
}

static void consume_password_reset_auth_result(UiState& st,
                                               PasswordResetState& reset_state) {
    const auto snapshot = snapshot_async_ui_request(st.auth_async_request);
    if (!is_password_reset_action(snapshot.action)) return;

    AsyncUiRequestSnapshot completed;
    if (!take_auth_async_request_result(st, snapshot.action, &completed)) return;

    reset_state.working = false;
    const auto& result = completed.result;
    if (!result.success) {
        reset_state.error_message = auth_failure_detail(
            result, "The password recovery request could not be completed. Please try again.");
        return;
    }

    if (completed.action == kAuthActionResetRequest) {
        reset_state.current_step = 1;
        reset_state.code_sent = true;
        reset_state.code_resend_seconds = 60;
        reset_state.success_message = "Reset code sent. Check your email.";
    } else if (completed.action == kAuthActionResetResend) {
        reset_state.code_sent = true;
        reset_state.code_resend_seconds = 60;
        reset_state.success_message = "Reset code resent. Check your email.";
    } else if (completed.action == kAuthActionResetVerify) {
        wipe_auth_secret(reset_state.verification_code);
        reset_state.current_step = 2;
        reset_state.success_message = "Code verified. Choose a new password.";
    } else if (completed.action == kAuthActionResetPassword) {
        clear_password_reset_secrets(reset_state);
        reset_state.success_message = "Password reset successfully. You can now sign in.";
        reset_state.close_after_success = true;
        push_notice(st, ui_model::NoticeLevel::Success, "Password reset",
                    "Your password was changed. You can now sign in to Amalgam.");
    }
}

// ---------------------------------------------------------------------------
// Password Reset Dialog
// ---------------------------------------------------------------------------

void draw_password_reset_dialog(UiState& st) {
    if (st.fixture_mode) {
        if (st.fixture_case == "dialog-password-reset") draw_fixture_password_reset_dialog(st);
        else if (is_password_reset_fixture(st.fixture_case)) draw_fixture_password_reset_state(st);
        return;
    }
    auto& reset_state = get_password_reset_state();
    reset_state.working =
        auth_async_request_is_working(st, kAuthActionResetRequest) ||
        auth_async_request_is_working(st, kAuthActionResetResend) ||
        auth_async_request_is_working(st, kAuthActionResetVerify) ||
        auth_async_request_is_working(st, kAuthActionResetPassword);
    // Recovery can be one field or include a visible error/success card.
    // Fit the short states closely, then grow up to the safe viewport cap so
    // feedback and the primary action never extend beyond the desktop.
    position_content_aware_wizard_modal(640.0f, 430.0f);

    bool popup_open = true;
    if (!ImGui::BeginPopupModal("Reset Password", &popup_open,
                                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                    ImGuiWindowFlags_AlwaysAutoResize)) {
        if (!popup_open) {
            cancel_password_reset_request(st);
            clear_password_reset_secrets(reset_state);
            reset_state = PasswordResetState();
        }
        return;
    }
    consume_password_reset_auth_result(st, reset_state);
    const bool account_lane_busy = auth_async_request_lane_busy(st);
    
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
            if (account_lane_busy) ImGui::BeginDisabled();
            input_text_hint("##reset_email", "you@example.com", &reset_state.email);
            if (account_lane_busy) ImGui::EndDisabled();
            
            ImGui::Spacing();
            
            if (auth_async_request_is_working(st, kAuthActionResetRequest)) {
                primary_button("Sending secure code...", ImVec2(-1, ui_px(40.0f)), true, true);
            } else if (account_lane_busy) {
                primary_button("Send secure reset code", ImVec2(-1, ui_px(40.0f)), false, true);
            } else {
                if (primary_button("Send secure reset code", ImVec2(-1, ui_px(40.0f)))) {
                    if (reset_state.email.empty()) {
                        reset_state.error_message = "Email is required";
                    } else {
                        const std::string email = reset_state.email;
                        if (start_auth_async_request(
                                st, kAuthActionResetRequest, [email]() {
                                    const auto response = aml::supabase::SupabaseManager::instance()
                                                              .request_password_reset(email);
                                    return auth_response_result(response, "Reset code sent",
                                                                "Reset code request failed",
                                                                "Failed to send reset code");
                                })) {
                            reset_state.working = true;
                            reset_state.error_message.clear();
                            reset_state.success_message.clear();
                        } else {
                            reset_state.error_message =
                                "Another account request is still finishing. Please try again in a moment.";
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
            if (account_lane_busy) ImGui::BeginDisabled();
            input_text_hint("##reset_code", "6-digit code", &reset_state.verification_code);
            if (account_lane_busy) ImGui::EndDisabled();
            
            ImGui::Spacing();
            
            // Resend code
            if (reset_state.code_resend_seconds > 0) {
                ImGui::TextColored(k.muted, "Resend code in %d seconds", reset_state.code_resend_seconds);
            } else if (auth_async_request_is_working(st, kAuthActionResetResend)) {
                ghost_button("Sending reset code...", ImVec2(ui_px(130.0f), ui_px(30.0f)), true);
            } else if (account_lane_busy) {
                ghost_button("Resend code", ImVec2(ui_px(130.0f), ui_px(30.0f)), true);
            } else {
                if (ghost_button("Resend code", ImVec2(ui_px(130.0f), ui_px(30.0f)))) {
                    const std::string email = reset_state.email;
                    if (start_auth_async_request(
                            st, kAuthActionResetResend, [email]() {
                                const auto response = aml::supabase::SupabaseManager::instance()
                                                          .request_password_reset(email);
                                return auth_response_result(response, "Reset code resent",
                                                            "Reset code request failed",
                                                            "Failed to resend code");
                            })) {
                        reset_state.working = true;
                        reset_state.error_message.clear();
                        reset_state.success_message.clear();
                    } else {
                        reset_state.error_message =
                            "Another account request is still finishing. Please try again in a moment.";
                    }
                }
            }
            
            ImGui::Spacing();
            if (auth_async_request_is_working(st, kAuthActionResetVerify)) {
                primary_button("Verifying code...", ImVec2(-1, ui_px(40.0f)), true, true);
            } else if (account_lane_busy) {
                primary_button("Verify code", ImVec2(-1, ui_px(40.0f)), false, true);
            } else if (primary_button("Verify code", ImVec2(-1, ui_px(40.0f)))) {
                const bool valid_code = reset_state.verification_code.length() == 6 &&
                    std::all_of(reset_state.verification_code.begin(),
                                reset_state.verification_code.end(),
                                [](unsigned char c) { return std::isdigit(c) != 0; });
                if (valid_code) {
                    const std::string email = reset_state.email;
                    std::string code = reset_state.verification_code;
                    if (start_auth_async_request(
                            st, kAuthActionResetVerify, [email, code]() mutable {
                                try {
                                    const auto response = aml::supabase::SupabaseManager::instance()
                                                              .verify_otp(email, code, "recovery");
                                    wipe_auth_secret(code);
                                    return auth_response_result(response, "Reset code verified",
                                                                "Verification failed",
                                                                "Verification failed");
                                } catch (...) {
                                    wipe_auth_secret(code);
                                    throw;
                                }
                            })) {
                        reset_state.working = true;
                        reset_state.error_message.clear();
                        reset_state.success_message.clear();
                    } else {
                        reset_state.error_message =
                            "Another account request is still finishing. Please try again in a moment.";
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
            if (account_lane_busy) ImGui::BeginDisabled();
            input_secret("##reset_new_password", &reset_state.new_password);
            
            ImGui::Spacing();
            
            draw_form_label("Confirm new password");
            ImGui::SetNextItemWidth(-1);
            input_secret("##reset_confirm_password", &reset_state.confirm_password);
            if (account_lane_busy) ImGui::EndDisabled();
            
            ImGui::Spacing();
            if (auth_async_request_is_working(st, kAuthActionResetPassword)) {
                primary_button("Resetting password...", ImVec2(-1, ui_px(40.0f)), true, true);
            } else if (account_lane_busy) {
                primary_button("Reset password", ImVec2(-1, ui_px(40.0f)), false, true);
            } else if (primary_button("Reset password", ImVec2(-1, ui_px(40.0f)))) {
                if (reset_state.new_password.empty() || reset_state.confirm_password.empty()) {
                    reset_state.error_message = "Please enter and confirm your new password";
                } else if (reset_state.new_password != reset_state.confirm_password) {
                    reset_state.error_message = "Passwords do not match";
                } else {
                    std::string password = reset_state.new_password;
                    if (start_auth_async_request(
                            st, kAuthActionResetPassword, [password]() mutable {
                                AsyncUiRequestResult unavailable;
                                try {
                                    auto* client = aml::supabase::SupabaseManager::instance().client();
                                    if (!client) {
                                        unavailable.success = false;
                                        unavailable.title = "Password reset unavailable";
                                        unavailable.detail = "The account service is not configured.";
                                        wipe_auth_secret(password);
                                        return unavailable;
                                    }
                                    const auto response = client->change_password("", password);
                                    wipe_auth_secret(password);
                                    return auth_response_result(response, "Password reset",
                                                                "Password reset failed",
                                                                "Failed to reset password");
                                } catch (...) {
                                    wipe_auth_secret(password);
                                    throw;
                                }
                            })) {
                        reset_state.working = true;
                        reset_state.error_message.clear();
                        reset_state.success_message.clear();
                    } else {
                        reset_state.error_message =
                            "Another account request is still finishing. Please try again in a moment.";
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
    ImGui::Spacing();
    ImGui::TextColored(k.muted, "Prefer the official website for recovery?");
    ImGui::SameLine();
    if (ghost_button("Open website", ImVec2(ui_px(122.0f), ui_px(30.0f)))) {
        open_website_auth_page(aml::online::config().password_reset_url());
    }
    if (account_lane_busy && !reset_state.working) {
        ImGui::Spacing();
        draw_auth_lane_busy_hint(st);
    }
    
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
    if (reset_state.close_after_success) {
        popup_open = false;
        ImGui::CloseCurrentPopup();
    } else if (ghost_button("Close", ImVec2(ui_px(96.0f), ui_px(32.0f)))) {
        cancel_password_reset_request(st);
        popup_open = false;
        ImGui::CloseCurrentPopup();
    }
    
    ImGui::EndPopup();
    if (!popup_open) {
        cancel_password_reset_request(st);
        clear_password_reset_secrets(reset_state);
        reset_state = PasswordResetState();
        st.password_reset_popup_open = false;
    }
}

// ---------------------------------------------------------------------------
// Login Wizard (for existing users)
// ---------------------------------------------------------------------------

void draw_amalgam_login_wizard(UiState& st) {
    if (st.fixture_mode) {
        if (st.fixture_case == "dialog-sign-in") draw_fixture_login_dialog(st);
        else if (is_login_fixture(st.fixture_case)) draw_fixture_login_state(st);
        return;
    }
    static std::string login_email;
    static std::string login_password;
    static bool login_remember = true;
    static bool show_password = false;
    static std::string login_error;
    static bool login_working = false;

    login_working = auth_async_request_is_working(st, kAuthActionLogin);

    position_wizard_modal(660.0f, 650.0f);
    bool popup_open = true;
    if (!ImGui::BeginPopupModal("Sign In to Amalgam", &popup_open,
                                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse)) {
        if (!popup_open) {
            cancel_login_request(st);
            wipe_auth_secret(login_password);
            login_error.clear();
            login_working = false;
        }
        return;
    }

    auto close_login = [&]() {
        cancel_login_request(st);
        popup_open = false;
        wipe_auth_secret(login_password);
        login_error.clear();
        login_working = false;
        ImGui::CloseCurrentPopup();
    };

    AsyncUiRequestSnapshot completed;
    if (take_auth_async_request_result(st, kAuthActionLogin, &completed)) {
        login_working = false;
        if (completed.result.success) {
            bool session_saved = true;
            if (login_remember && !completed.result.payload_b.empty()) {
                const std::string& email = completed.result.payload_a.empty()
                    ? login_email : completed.result.payload_a;
                session_saved = !email.empty() &&
                    aml::account::AccountManager::instance().create_session(
                        email, completed.result.payload_b, completed.result.payload_c,
                        completed.result.number_a);
            }
            if (!session_saved) {
                login_error =
                    "You were signed in, but Amalgam could not safely save the protected local session. Please try again.";
            } else {
                wipe_auth_secret(login_password);
                ImGui::CloseCurrentPopup();
                st.auth_prompt_dismissed = true;
            }
        } else {
            login_error = auth_failure_detail(
                completed.result,
                "We could not sign you in. Check your email and password, then try again.");
        }
    }
    const bool account_lane_busy = auth_async_request_lane_busy(st);

    draw_wizard_hero(st, "AMALGAM ACCOUNT", "Welcome back",
                     "Sign in to keep your launcher preferences, social features, and supported profiles connected.");
    ImGui::Spacing();
    card_begin("##amalgam_login_form", ImVec2(-1, 0));
    if (account_lane_busy) ImGui::BeginDisabled();
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
    if (account_lane_busy) ImGui::EndDisabled();
    card_end();

    ImGui::Spacing();
    draw_wizard_feedback("##login_error", login_error, true, "Sign-in needs attention");
    if (!login_error.empty()) ImGui::Spacing();
    if (login_working) {
        primary_button("Signing in securely...", ImVec2(-1, ui_px(40.0f)), true, true);
    } else if (account_lane_busy) {
        primary_button("Sign in", ImVec2(-1, ui_px(40.0f)), false, true);
    } else {
        if (primary_button("Sign in", ImVec2(-1, ui_px(40.0f)))) {
            if (login_email.empty()) {
                login_error = "Enter the email address for your Amalgam account.";
            } else if (login_password.empty()) {
                login_error = "Enter your password to continue.";
            } else {
                const std::string email = login_email;
                std::string password = login_password;
                if (start_auth_async_request(
                        st, kAuthActionLogin, [email, password]() mutable {
                            try {
                                const auto response = aml::supabase::SupabaseManager::instance().sign_in(
                                    email, password);
                                wipe_auth_secret(password);
                                return auth_response_result(response, "Signed in", "Sign-in failed",
                                                            "We could not sign you in. Check your email and password, then try again.");
                            } catch (...) {
                                wipe_auth_secret(password);
                                throw;
                            }
                        })) {
                    login_working = true;
                    login_error.clear();
                } else {
                    login_error =
                        "Another account request is still finishing. Please try again in a moment.";
                }
            }
        }
    }
    ImGui::Spacing();
    draw_security_note("Your password stays with the Amalgam account service. Microsoft sign-in happens separately in Microsoft’s browser page.");
    ImGui::Spacing();
    ImGui::TextColored(k.muted, "Forgot your password?");
    ImGui::SameLine();
    if (ghost_button("Reset it", ImVec2(ui_px(92.0f), ui_px(30.0f)), account_lane_busy)) {
        close_login();
        // Queue the next modal for the following frame. Opening a sibling
        // popup while closing this one is timing-sensitive in Dear ImGui.
        st.password_reset_popup_open = true;
    }
    ImGui::Spacing();
    ImGui::TextColored(k.muted, "New to Amalgam?");
    ImGui::SameLine();
    if (ghost_button("Create a free account", ImVec2(ui_px(180.0f), ui_px(30.0f)), account_lane_busy)) {
        close_login();
        st.wizard_open = false;
        clear_wizard_secrets(st.auth_wizard_state);
        st.auth_wizard_state = AuthWizardState();
        // Use the shared modal hand-off queue so sign-in -> registration
        // always lands in the next dialog on the next render frame.
        st.register_popup_open = true;
    }

    ImGui::Spacing();
    if (ghost_button("Open official website", ImVec2(ui_px(190.0f), ui_px(30.0f)))) {
        open_website_auth_page(aml::online::config().login_url());
    }
    ImGui::TextColored(k.muted, "The website and launcher use the same Amalgam account.");

    ImGui::Spacing();
    if (account_lane_busy && !login_working) draw_auth_lane_busy_hint(st);
    if (account_lane_busy && !login_working) ImGui::Spacing();
    if (ghost_button("Close", ImVec2(ui_px(96.0f), ui_px(32.0f)))) close_login();
    
    ImGui::EndPopup();
    if (!popup_open) {
        cancel_login_request(st);
        wipe_auth_secret(login_password);
        login_error.clear();
        login_working = false;
    }
}

// ---------------------------------------------------------------------------
// Account Button with Wizard
// ---------------------------------------------------------------------------

void draw_account_button_with_wizard(UiState& st) {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    // Auth workers mutate the provider session only in their own lane. Avoid
    // peeking at that mutable client mid-request from the render thread.
    const bool account_request_pending = auth_async_request_lane_busy(st);
    
    if (!account_request_pending && supabase.is_authenticated()) {
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
                request_amalgam_sign_out(st);
                ImGui::CloseCurrentPopup();
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

    // The individual dialogs continue rendering their local loading state;
    // defer provider-session reads until the account worker publishes its
    // guarded completion.
    if (auth_async_request_lane_busy(st)) {
        if (ImGui::IsPopupOpen("Amalgam Account Setup")) {
            draw_auth_wizard(st);
            draw_amalgam_login_wizard(st);
        }
        return;
    }
    
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
