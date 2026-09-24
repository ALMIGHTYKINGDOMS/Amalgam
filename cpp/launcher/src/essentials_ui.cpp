#include "essentials_ui.h"
#include "ui_internal.h"
#include "ui_model.h"
#include "essentials.h"
#include "essentials_manager.h"
#include "essentials_session.h"
#include "essentials_address.h"
#include "essentials_sync.h"
#include "social_ui.h"
#include "entitlements.h"

#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <functional>
#include <sstream>
#include <thread>
#include <utility>

namespace aml::essentials {
using namespace aml::ui;

// ---------------------------------------------------------------------------
// UI State
// ---------------------------------------------------------------------------

struct EssentialsUIState {
    int current_tab = 0;

    std::string friend_search;
    std::string add_friend_input;
    bool show_add_friend = false;
    std::string selected_friend_id;

    bool host_dialog_open = false;
    std::string host_world_name;
    std::string host_alias;
    SessionPrivacy host_privacy = SessionPrivacy::FriendsOnly;
    int host_player_limit = 8;
    std::string host_profile_id;
    std::string host_description;

    bool invite_dialog_open = false;
    std::string invite_session_id;
    std::vector<std::string> selected_invitees;
    std::string invite_search;
    bool invite_show_online_only = false;

    bool join_dialog_open = false;
    uint64_t join_dialog_generation = 0;
    uint64_t join_intent_generation = 0;
    bool join_compat_shown = false;
    CompatCheck join_compat;
    SyncPlan join_sync_plan;
    bool join_syncing = false;
    uint64_t join_execution_generation = 0;
    std::string join_status_text;
    std::string join_session_id;
    std::string join_address;
    std::string join_token;
    int join_step = 0;
    float join_progress = 0.0f;

    bool session_manager_open = false;
    std::string manage_session_id;

    std::vector<EssentialsNotification> notifications;

    // Notification toast state
    std::string toast_message;
    std::string toast_title;
    float toast_timer = 0.0f;
    ImVec4 toast_color = ImVec4(0,0,0,0);
    int unread_notification_count = 0;

    // ── AAA layout state ──────────────────────────────────────────
    // Your Status card
    FriendStatus self_status = FriendStatus::Online;
    bool status_editor_open = false;
    int status_editor_selection = 0;
    std::string self_status_message;

    // Friend profile side panel (selected_friend_id drives it)
    bool friend_profile_panel_open = false;

    // Activity feed cache
    struct ActivityEntry {
        IconId icon = IconId::Info;   // vector icon
        ImVec4 color;
        std::string text;
        std::string time;
        int64_t timestamp = 0;
    };
    std::vector<ActivityEntry> activity_feed;
    int64_t activity_last_built = 0;

    // Quick actions / hero button state (all real actions)
    bool quick_add_friend = false;      // opens inline add-friend row
    bool hero_host_world = false;       // opens host dialog
    bool hero_join_code = false;        // opens join dialog
};

static EssentialsUIState& get_essentials_ui_state() {
    static EssentialsUIState state;
    return state;
}

// Opening a fresh dialog invalidates an analysis result for any dialog the
// player previously closed.  Reopening an already-running join deliberately
// keeps its generation so it can display that task's guarded progress.
static void open_fresh_join_dialog(EssentialsUIState& ui) {
    ++ui.join_intent_generation;
    if (ui.join_intent_generation == 0) ++ui.join_intent_generation;
    ++ui.join_dialog_generation;
    if (ui.join_dialog_generation == 0) ++ui.join_dialog_generation;
    ui.join_dialog_open = true;
    ui.join_compat_shown = false;
    ui.join_step = 0;
    ui.join_progress = 0.0f;
    ui.join_status_text.clear();
}

// ---------------------------------------------------------------------------
// Account-backed UI work
// ---------------------------------------------------------------------------
//
// Every manager operation below can reach Supabase, a local profile, or the
// Essentials transport.  ImGui draw functions must only schedule that work;
// the result comes back through UiState's joined worker list.  A single
// serialized request lane also avoids issuing contradictory account mutations
// (for example remove and block) against the same social graph at once.

static bool essentials_request_is_working(const UiState& st,
                                          const std::string& action) {
    const auto snapshot = snapshot_async_ui_request(st.essentials_async_request);
    return snapshot.working && snapshot.action == action;
}

static bool essentials_request_lane_busy(const UiState& st) {
    return snapshot_async_ui_request(st.essentials_async_request).working;
}

static bool essentials_action_starts_with(const std::string& action,
                                          const char* prefix) {
    const size_t length = std::strlen(prefix);
    return action.size() >= length && action.compare(0, length, prefix) == 0;
}

static bool essentials_request_failed(const UiState& st,
                                      const std::string& action) {
    const auto snapshot = snapshot_async_ui_request(st.essentials_async_request);
    return !snapshot.working && snapshot.has_result &&
           snapshot.action == action && !snapshot.result.success;
}

static void draw_essentials_request_feedback(const UiState& st,
                                             const std::string& action,
                                             const char* working_label) {
    const auto snapshot = snapshot_async_ui_request(st.essentials_async_request);
    if (snapshot.action != action) return;
    if (snapshot.working) {
        ImGui::TextColored(k.muted, "%s", working_label);
    } else if (snapshot.has_result && !snapshot.result.success &&
               !snapshot.result.detail.empty()) {
        ImGui::TextColored(k.red, "%s", snapshot.result.detail.c_str());
    }
}

static bool start_essentials_request(
    UiState& st, const std::string& action,
    std::function<AsyncUiRequestResult()> work) {
    uint64_t generation = 0;
    if (!begin_async_ui_request(st.essentials_async_request, action, &generation)) {
        return false;
    }

    spawn_worker(st, std::thread([&st, action, generation,
                                  work = std::move(work)]() mutable {
        AsyncUiRequestResult result;
        try {
            result = work();
        } catch (const std::exception&) {
            result.success = false;
            result.title = "Essentials request failed";
            result.detail = "The request ended unexpectedly. Please try again.";
        } catch (...) {
            result.success = false;
            result.title = "Essentials request failed";
            result.detail = "The request ended unexpectedly. Please try again.";
        }
        // UiState remains alive until join_workers() returns.  During shutdown
        // there is no visible surface left to consume a result, so leave it
        // unpublished rather than resurrecting an obsolete UI state.
        if (!st.shutting_down.load()) {
            complete_async_ui_request(st.essentials_async_request, action,
                                      generation, std::move(result));
        }
    }));
    return true;
}

static void consume_essentials_request_result(UiState& st) {
    AsyncUiRequestSnapshot completed;
    if (!take_async_ui_request_result(st.essentials_async_request, &completed)) {
        return;
    }

    auto& ui = get_essentials_ui_state();
    const auto& result = completed.result;
    if (result.success) {
        if (completed.action == "friend-request") {
            ui.add_friend_input.clear();
            ui.show_add_friend = false;
            ui.quick_add_friend = false;
        } else if (essentials_action_starts_with(completed.action, "invite-accept:")) {
            const uint64_t join_intent_generation = result.number_a > 0
                ? static_cast<uint64_t>(result.number_a) : 0;
            if (!ui.join_syncing && join_intent_generation != 0 &&
                join_intent_generation == ui.join_intent_generation) {
                ui.join_session_id = result.payload_a;
                ui.join_address = result.payload_b;
                ui.join_token = result.payload_c;
                if (!ui.join_address.empty() && !ui.join_token.empty()) {
                    open_fresh_join_dialog(ui);
                }
            }
        } else if (completed.action == "host-start") {
            ui.host_dialog_open = false;
        } else if (completed.action == "invite-send") {
            ui.selected_invitees.clear();
            ui.invite_dialog_open = false;
        } else if (completed.action == "join-analyze") {
            const uint64_t dialog_generation = result.number_a > 0
                ? static_cast<uint64_t>(result.number_a) : 0;
            if (ui.join_dialog_open && dialog_generation != 0 &&
                dialog_generation == ui.join_dialog_generation) {
                ui.join_session_id = result.payload_a;
                ui.join_address = result.payload_b;
                auto& joins = JoinManager::instance();
                ui.join_compat = joins.get_current_compat();
                ui.join_sync_plan = joins.get_current_sync_plan();
                ui.join_compat_shown = true;
            }
        } else if (completed.action == "join-execute") {
            // The joined request only starts the intentionally long-lived
            // JoinManager task.  Its own snapshot carries the matching
            // generation and is the sole source of later progress updates.
            ui.join_execution_generation = result.number_a > 0
                ? static_cast<uint64_t>(result.number_a) : 0;
            ui.join_syncing = ui.join_execution_generation != 0;
            ui.join_status_text = ui.join_syncing
                ? "Starting the session join..." : "Could not start the session join.";
        } else if (completed.action == "status-set" && !result.payload_a.empty()) {
            const int status = std::clamp(std::atoi(result.payload_a.c_str()), 0, 6);
            ui.self_status = static_cast<FriendStatus>(status);
            ui.status_editor_open = false;
        } else if (completed.action == "host-stop") {
            ui.session_manager_open = false;
        }
        if (!result.title.empty()) {
            push_notice(st, result.warning ? ui_model::NoticeLevel::Warning
                                            : ui_model::NoticeLevel::Success,
                        result.title,
                        result.detail);
        }
    } else {
        if (completed.action == "join-execute") {
            ui.join_syncing = false;
            ui.join_execution_generation = 0;
            ui.join_status_text = "Could not start the session join.";
        }
        if (!result.title.empty()) {
        push_notice(st, ui_model::NoticeLevel::Error, result.title,
                    result.detail.empty() ? "Please try again." : result.detail);
        }
    }
}

static void refresh_join_progress(UiState& st) {
    auto& ui = get_essentials_ui_state();
    if (!ui.join_syncing || ui.join_execution_generation == 0) return;

    const JoinProgressSnapshot progress =
        JoinManager::instance().get_progress_snapshot();
    // A cancelled or superseded task is not allowed to alter a newer dialog's
    // UI.  The generation comes from the joined request that initiated this
    // exact task, rather than from the visual dialog itself.
    if (progress.generation != ui.join_execution_generation) {
        ui.join_syncing = false;
        return;
    }

    ui.join_progress = std::clamp(progress.progress, 0.0f, 1.0f);
    if (!progress.status.empty()) ui.join_status_text = progress.status;
    if (progress.active) {
        ui.join_step = std::clamp(static_cast<int>(ui.join_progress * 4.0f), 0, 3);
        return;
    }

    if (progress.completed) {
        ui.join_syncing = false;
        const std::string detail = progress.status.empty()
            ? (progress.success ? "The Minecraft session completed." :
                                "The session could not be completed.")
            : progress.status;
        push_notice(st, progress.success ? ui_model::NoticeLevel::Success
                                         : ui_model::NoticeLevel::Error,
                    progress.success ? "Minecraft session complete" : "Session join failed",
                    detail);
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static ImVec4 status_color(FriendStatus s) {
    switch (s) {
        case FriendStatus::Online:    return k.green;
        case FriendStatus::InLauncher:return k.blue;
        case FriendStatus::Playing:   return k.brand;
        case FriendStatus::Hosting:   return k.yellow;
        case FriendStatus::Joining:   return k.orange;
        case FriendStatus::Away:      return k.muted;
        default:                      return k.muted;
    }
}

static ImVec4 compat_color(CompatibilityLevel l) {
    switch (l) {
        case CompatibilityLevel::Match:          return k.green;
        case CompatibilityLevel::MinorMismatch:  return k.yellow;
        case CompatibilityLevel::MajorMismatch:  return k.orange;
        case CompatibilityLevel::Incompatible:   return k.red;
        default:                                 return k.muted;
    }
}

static const char* compat_icon(CompatibilityLevel l) {
    switch (l) {
        case CompatibilityLevel::Match:          return "\xe2\x9c\x93";
        case CompatibilityLevel::MinorMismatch:  return "\xe2\x9a\xa0";
        case CompatibilityLevel::MajorMismatch:  return "\xe2\x9a\xa0";
        case CompatibilityLevel::Incompatible:   return "\xe2\x9c\x97";
        default:                                 return "?";
    }
}

static const char* privacy_short_name(SessionPrivacy p) {
    switch (p) {
        case SessionPrivacy::InviteOnly:         return "Invite Only";
        case SessionPrivacy::FriendsOnly:        return "Friends Only";
        case SessionPrivacy::FriendsOfFriends:   return "Friends of Friends";
        case SessionPrivacy::Private:            return "Private";
        default:                                 return "Unknown";
    }
}

static const char* session_state_tag(SessionState s) {
    switch (s) {
        case SessionState::Starting:  return "Starting...";
        case SessionState::Online:    return "Online";
        case SessionState::Stopping:  return "Stopping...";
        case SessionState::Ended:     return "Ended";
        case SessionState::Crashed:   return "Crashed";
        default:                      return "Unknown";
    }
}

static ImVec4 session_state_color(SessionState s) {
    switch (s) {
        case SessionState::Starting:  return k.yellow;
        case SessionState::Online:    return k.green;
        case SessionState::Stopping:  return k.orange;
        case SessionState::Ended:     return k.muted;
        case SessionState::Crashed:   return k.red;
        default:                      return k.muted;
    }
}

static char name_initial(const std::string& name) {
    if (name.empty()) return '?';
    return static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
}

static ImVec4 avatar_color(const std::string& name) {
    size_t h = 0;
    for (char c : name) h = h * 131 + static_cast<unsigned char>(c);
    float hue = static_cast<float>(h % 360) / 360.0f;
    ImVec4 c;
    ImGui::ColorConvertHSVtoRGB(hue, 0.55f, 0.75f, c.x, c.y, c.z);
    c.w = 1.0f;
    return c;
}

static void draw_avatar_circle(const char* label, const ImVec4& color,
                               float radius) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float diameter = radius * 2.0f;

    dl->AddCircleFilled(ImVec2(p.x + radius, p.y + radius), radius,
                        c32(color));

    const char* text = label;
    ImVec2 ts = ImGui::CalcTextSize(text);
    dl->AddText(ImVec2(p.x + radius - ts.x * 0.5f,
                        p.y + radius - ts.y * 0.5f),
                c32(ImVec4(1,1,1,1)), text);

    ImGui::Dummy(ImVec2(diameter, diameter));
}

static std::string time_ago(int64_t timestamp) {
    if (timestamp <= 0) return "";
    int64_t now = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    int64_t diff = now - timestamp;
    if (diff < 60) return "just now";
    if (diff < 3600) return std::to_string(diff / 60) + "m ago";
    if (diff < 86400) return std::to_string(diff / 3600) + "h ago";
    return std::to_string(diff / 86400) + "d ago";
}

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------

static void draw_friends_tab(UiState& st);
static void draw_invites_tab(UiState& st);
static void draw_sessions_tab(UiState& st);
static void draw_notifications_tab(UiState& st);
static void draw_host_dialog(UiState& st);
static void draw_invite_dialog(UiState& st);
static void draw_join_dialog(UiState& st);
static void draw_session_manager(UiState& st);
static void draw_invite_card(const EssentialsInvite& invite, bool received,
                             UiState& st);
static void draw_session_card_hosting(UiState& st);
static void draw_toast_notification(UiState& st);
static void draw_essentials_hero(UiState& st);
static void build_activity_feed(UiState& st);
static void draw_activity_feed(UiState& st);
static void draw_quick_actions(UiState& st);
static void draw_your_status(UiState& st);
static void draw_connection_card(UiState& st);
static void draw_friend_profile_panel(UiState& st);
static void draw_essentials_empty_state(UiState& st);
static void draw_essentials_info_card(UiState& st);

// ---------------------------------------------------------------------------
// Visual-review fixture — fully local Essentials façade
// ---------------------------------------------------------------------------
//
// Essentials normally composes its views from friends, sessions, invites, and
// account managers.  Some of those managers refresh remote state as a side
// effect of a getter when a launcher process has an already-authenticated
// account.  A visual fixture must never depend on, request, or disclose that
// state.  Keep every Essentials fixture in this small, explicit façade rather
// than trying to guard individual getters spread across the live UI.

static int fixture_essentials_tab(const std::string& fixture_case) {
    if (fixture_case == "essentials-invites") return 1;
    if (fixture_case == "essentials-sessions" ||
        fixture_case == "essentials-session-manager" ||
        fixture_case == "essentials-session-manager-working" ||
        fixture_case == "essentials-session-manager-error") return 2;
    if (fixture_case == "essentials-notifications") return 3;
    if (fixture_case == "essentials-messages") return 4;
    if (fixture_case == "essentials-parties") return 5;
    return 0;
}

static void fixture_essentials_tab_pill(const char* label, bool active, bool hot) {
    const ImVec2 text_size = ImGui::CalcTextSize(label);
    const ImVec2 size(text_size.x + ui_px(28.0f), ui_px(34.0f));
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, pos + size, c32(active ? k.surface2 : ImVec4(0, 0, 0, 0)),
                      ui_px(8.0f));
    if (active)
        dl->AddRect(pos, pos + size, c32(k.brand), ui_px(8.0f), 0, ui_px(1.5f));
    if (hot)
        dl->AddCircleFilled(pos + ImVec2(size.x - ui_px(9.0f), ui_px(8.0f)),
                            ui_px(3.0f), c32(k.brand));
    ImGui::InvisibleButton((std::string("##fixture_ess_tab_") + label).c_str(), size);
    ImGui::PushFont(active ? f_bold : f_body);
    dl->AddText(pos + ImVec2(ui_px(14.0f),
                             (size.y - ImGui::GetTextLineHeight()) * 0.5f),
                c32(active ? k.text : k.muted), label);
    ImGui::PopFont();
}

enum class FixtureEssentialsDialogKind {
    Host,
    Invite,
    Join,
    SessionManager,
};

enum class FixtureEssentialsDialogState {
    Ready,
    Working,
    Error,
    Checking,
    Incompatible,
    Syncing,
};

// Fixture dialog routes intentionally use copied literals rather than any of
// the live Essentials types.  They must remain safe even if a launcher happens
// to start with an authenticated account and active local session.
struct FixtureEssentialsDialog {
    FixtureEssentialsDialogKind kind;
    FixtureEssentialsDialogState state;
    const char* title;
    const char* subtitle;
};

static const FixtureEssentialsDialog* fixture_essentials_dialog_for_case(
    const std::string& fixture_case) {
    static constexpr FixtureEssentialsDialog kHostReady{
        FixtureEssentialsDialogKind::Host, FixtureEssentialsDialogState::Ready,
        "Host a world", "Set up a private session for your friends."};
    static constexpr FixtureEssentialsDialog kHostWorking{
        FixtureEssentialsDialogKind::Host, FixtureEssentialsDialogState::Working,
        "Host a world", "Preparing the selected world for a hosted session."};
    static constexpr FixtureEssentialsDialog kHostError{
        FixtureEssentialsDialogKind::Host, FixtureEssentialsDialogState::Error,
        "Host a world", "Review the saved world and Essentials address before retrying."};
    static constexpr FixtureEssentialsDialog kInviteReady{
        FixtureEssentialsDialogKind::Invite, FixtureEssentialsDialogState::Ready,
        "Invite friends", "Choose people who can join this hosted world."};
    static constexpr FixtureEssentialsDialog kInviteWorking{
        FixtureEssentialsDialogKind::Invite, FixtureEssentialsDialogState::Working,
        "Invite friends", "Sending invitations to the selected friends."};
    static constexpr FixtureEssentialsDialog kInviteError{
        FixtureEssentialsDialogKind::Invite, FixtureEssentialsDialogState::Error,
        "Invite friends", "Review the session and selected friends before retrying."};
    static constexpr FixtureEssentialsDialog kJoinReady{
        FixtureEssentialsDialogKind::Join, FixtureEssentialsDialogState::Ready,
        "Join a session", "Use the address and code shared by the host."};
    static constexpr FixtureEssentialsDialog kJoinChecking{
        FixtureEssentialsDialogKind::Join, FixtureEssentialsDialogState::Checking,
        "Join a session", "Checking the session and local profile compatibility."};
    static constexpr FixtureEssentialsDialog kJoinError{
        FixtureEssentialsDialogKind::Join, FixtureEssentialsDialogState::Error,
        "Join a session", "The session could not be checked with the supplied address and code."};
    static constexpr FixtureEssentialsDialog kJoinIncompatible{
        FixtureEssentialsDialogKind::Join, FixtureEssentialsDialogState::Incompatible,
        "Join a session", "This session needs a compatible local profile before it can be joined."};
    static constexpr FixtureEssentialsDialog kJoinSyncing{
        FixtureEssentialsDialogKind::Join, FixtureEssentialsDialogState::Syncing,
        "Join a session", "Synchronizing the compatible local profile before launch."};
    static constexpr FixtureEssentialsDialog kSessionManagerReady{
        FixtureEssentialsDialogKind::SessionManager, FixtureEssentialsDialogState::Ready,
        "Session manager", "Review connection health and participant access."};
    static constexpr FixtureEssentialsDialog kSessionManagerWorking{
        FixtureEssentialsDialogKind::SessionManager, FixtureEssentialsDialogState::Working,
        "Session manager", "Applying the requested session setting."};
    static constexpr FixtureEssentialsDialog kSessionManagerError{
        FixtureEssentialsDialogKind::SessionManager, FixtureEssentialsDialogState::Error,
        "Session manager", "The requested session setting could not be applied."};

    if (fixture_case == "essentials-host-dialog") return &kHostReady;
    if (fixture_case == "essentials-host-dialog-working") return &kHostWorking;
    if (fixture_case == "essentials-host-dialog-error") return &kHostError;
    if (fixture_case == "essentials-invite-dialog") return &kInviteReady;
    if (fixture_case == "essentials-invite-dialog-working") return &kInviteWorking;
    if (fixture_case == "essentials-invite-dialog-error") return &kInviteError;
    if (fixture_case == "essentials-join-dialog") return &kJoinReady;
    if (fixture_case == "essentials-join-dialog-checking") return &kJoinChecking;
    if (fixture_case == "essentials-join-dialog-error") return &kJoinError;
    if (fixture_case == "essentials-join-dialog-incompatible") return &kJoinIncompatible;
    if (fixture_case == "essentials-join-dialog-syncing") return &kJoinSyncing;
    if (fixture_case == "essentials-session-manager") return &kSessionManagerReady;
    if (fixture_case == "essentials-session-manager-working") return &kSessionManagerWorking;
    if (fixture_case == "essentials-session-manager-error") return &kSessionManagerError;
    return nullptr;
}

static void draw_fixture_essentials_local_notice() {
    ImGui::TextColored(k.brand_hov,
                       "LOCAL VISUAL FIXTURE — account, session, profile, and network services are not used.");
    ImGui::TextColored(k.muted,
                       "All example fields and action controls are disabled; this preview cannot change anything.");
}

static void draw_fixture_join_compat_row(const char* label, const char* state,
                                         const char* detail, const ImVec4& color) {
    ImGui::TextColored(k.text, "%s", label);
    ImGui::SameLine(ui_px(166.0f));
    ImGui::TextColored(color, "%s", state);
    if (detail && detail[0] != '\0') {
        ImGui::SameLine(ui_px(300.0f));
        ImGui::TextColored(k.muted, "%s", detail);
    }
}

static void draw_fixture_essentials_dialog(const std::string& fixture_case) {
    const auto* dialog = fixture_essentials_dialog_for_case(fixture_case);

    // This mirrors the dedicated friend-profile fixture: Close preview must be
    // useful, but changing a local visual route must never read or change the
    // live EssentialsUIState.
    static std::string visible_fixture_case;
    static bool preview_open = false;
    if (visible_fixture_case != fixture_case) {
        visible_fixture_case = fixture_case;
        preview_open = dialog != nullptr;
    }
    if (!dialog || !preview_open) return;

    constexpr bool kFixtureActionDisabled = true;
    static std::string fixture_world = "Forsaken World SMP";
    static std::string fixture_alias = "forsaken-world";
    static std::string fixture_description = "A local sample session for visual review.";
    static std::string fixture_address = "forsaken-world.amalgam-essentials";
    static std::string fixture_code = "ORBIT-7Q";

    const char* popup_name = "Essentials dialog preview###essentials_fixture_dialog";
    float desired_height = 430.0f;
    if (dialog->kind == FixtureEssentialsDialogKind::Host) {
        desired_height = dialog->state == FixtureEssentialsDialogState::Ready
            ? 520.0f : 540.0f;
    } else if (dialog->kind == FixtureEssentialsDialogKind::Invite) {
        desired_height = dialog->state == FixtureEssentialsDialogState::Ready
            ? 410.0f : 480.0f;
    } else if (dialog->kind == FixtureEssentialsDialogKind::Join) {
        const bool expanded_join_detail =
            dialog->state == FixtureEssentialsDialogState::Incompatible ||
            dialog->state == FixtureEssentialsDialogState::Syncing;
        desired_height = expanded_join_detail ? 500.0f : 440.0f;
    }
    ImGui::OpenPopup(popup_name);
    set_next_adaptive_window(560.0f, desired_height, 360.0f, 280.0f,
                             ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(popup_name, &preview_open,
                                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                 ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoScrollbar |
                                 ImGuiWindowFlags_NoScrollWithMouse)) {
        return;
    }

    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted(dialog->title);
    ImGui::PopFont();
    ImGui::TextColored(k.muted, "%s", dialog->subtitle);
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // State-specific detail can outgrow a compact modal, especially at a
    // higher DPI.  Keep the disclosure and controls in the parent so the
    // only scroll owner is this intentional detail region and Close preview
    // never becomes unreachable below an outer popup scroll range.
    const ImGuiStyle& style = ImGui::GetStyle();
    const float footer_height = std::max(
        ui_px(112.0f),
        2.0f * ImGui::GetTextLineHeightWithSpacing() +
            std::max(ui_px(32.0f), ImGui::GetFrameHeight()) +
            5.0f * style.ItemSpacing.y + style.WindowPadding.y);
    const bool detail_visible = ImGui::BeginChild(
        "##fixture_essentials_dialog_detail", ImVec2(0.0f, -footer_height),
        ImGuiChildFlags_None);

    if (detail_visible && dialog->kind == FixtureEssentialsDialogKind::Host) {
        ImGui::TextUnformatted("World name");
        ImGui::BeginDisabled(kFixtureActionDisabled);
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##fixture_host_world", &fixture_world);
        ImGui::EndDisabled();
        ImGui::Spacing();
        ImGui::TextUnformatted("Essentials address");
        ImGui::BeginDisabled(kFixtureActionDisabled);
        ImGui::SetNextItemWidth(ui_px(260.0f));
        ImGui::InputText("##fixture_host_alias", &fixture_alias);
        ImGui::EndDisabled();
        ImGui::SameLine(0, ui_px(8.0f));
        ImGui::TextColored(k.muted, ".amalgam-essentials");
        ImGui::TextColored(k.muted, "3–32 lowercase letters, numbers, and hyphens");
        ImGui::Spacing();
        ImGui::TextColored(k.muted, "PRIVACY");
        ImGui::TextColored(k.text, "Friends only  •  8 player limit");
        ImGui::Spacing();
        ImGui::TextUnformatted("Description");
        ImGui::BeginDisabled(kFixtureActionDisabled);
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextMultiline("##fixture_host_description", &fixture_description,
                                  ImVec2(-1, ui_px(48.0f)));
        ImGui::EndDisabled();

        if (dialog->state == FixtureEssentialsDialogState::Working) {
            ImGui::Spacing();
            ImGui::TextColored(k.brand, "Starting hosted session…");
            ImGui::TextColored(k.muted,
                               "This static preview does not start a local world or transport.");
        } else if (dialog->state == FixtureEssentialsDialogState::Error) {
            ImGui::Spacing();
            card_begin("##fixture_host_error", ImVec2(-1, 0));
            ImGui::PushFont(f_bold);
            ImGui::TextColored(k.red, "Could not start hosting");
            ImGui::PopFont();
            ImGui::TextWrapped("The sample world was not shared. Verify the world and address, then retry.");
            card_end();
        }
    } else if (detail_visible && dialog->kind == FixtureEssentialsDialogKind::Invite) {
        ImGui::TextColored(k.muted, "HOSTED SESSION");
        ImGui::TextColored(k.text, "Forsaken World SMP");
        ImGui::Spacing();
        ImGui::TextColored(k.muted, "SELECTED FRIENDS (3)");
        ImGui::BeginDisabled(kFixtureActionDisabled);
        const char* selected_friends[] = {"Alex", "MiraBuilds", "AveryStone"};
        for (const char* friend_name : selected_friends) {
            bool selected = true;
            ImGui::Checkbox(friend_name, &selected);
            ImGui::SameLine();
            ImGui::TextColored(k.green, "● Online");
        }
        ImGui::EndDisabled();

        if (dialog->state == FixtureEssentialsDialogState::Working) {
            ImGui::Spacing();
            ImGui::TextColored(k.brand, "Sending 3 invitations…");
            ImGui::TextColored(k.muted,
                               "This local preview is not sending messages or session invitations.");
        } else if (dialog->state == FixtureEssentialsDialogState::Error) {
            ImGui::Spacing();
            card_begin("##fixture_invite_error", ImVec2(-1, 0));
            ImGui::PushFont(f_bold);
            ImGui::TextColored(k.red, "Could not send invites");
            ImGui::PopFont();
            ImGui::TextWrapped("No sample invitations were sent. Confirm the session is available, then retry.");
            card_end();
        }
    } else if (detail_visible && dialog->kind == FixtureEssentialsDialogKind::Join) {
        ImGui::TextUnformatted("Essentials address");
        ImGui::BeginDisabled(kFixtureActionDisabled);
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##fixture_join_address", &fixture_address);
        ImGui::TextUnformatted("Join code");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##fixture_join_code", &fixture_code, ImGuiInputTextFlags_Password);
        ImGui::EndDisabled();
        ImGui::Spacing();

        if (dialog->state == FixtureEssentialsDialogState::Checking) {
            ImGui::TextColored(k.brand, "Checking the session and local profile compatibility…");
            ImGui::TextColored(k.muted, "No address lookup, profile scan, or compatibility task is running.");
        } else if (dialog->state == FixtureEssentialsDialogState::Error) {
            card_begin("##fixture_join_error", ImVec2(-1, 0));
            ImGui::PushFont(f_bold);
            ImGui::TextColored(k.red, "Could not find session");
            ImGui::PopFont();
            ImGui::TextWrapped("The sample address could not be resolved. No join was started; you can retry safely.");
            card_end();
        } else if (dialog->state == FixtureEssentialsDialogState::Incompatible) {
            ImGui::TextColored(k.brand, "COMPATIBILITY CHECK");
            ImGui::Spacing();
            draw_fixture_join_compat_row("Minecraft version", "✓ Compatible",
                                         "1.20.1 vs 1.20.1", k.green);
            draw_fixture_join_compat_row("Loader", "✓ Compatible", "Fabric vs Fabric", k.green);
            draw_fixture_join_compat_row("Mods", "✕ Incompatible", "1 required mod missing", k.red);
            ImGui::Spacing();
            ImGui::TextColored(k.red, "Missing mod: Amalgam Essentials Bridge 2.4.0");
            ImGui::TextColored(k.red, "Session is incompatible. Cannot join.");
        } else if (dialog->state == FixtureEssentialsDialogState::Syncing) {
            ImGui::TextColored(k.brand, "COMPATIBILITY CHECK");
            draw_fixture_join_compat_row("Minecraft version", "✓ Compatible",
                                         "1.20.1 vs 1.20.1", k.green);
            draw_fixture_join_compat_row("Loader", "✓ Compatible", "Fabric vs Fabric", k.green);
            draw_fixture_join_compat_row("Mods", "✓ Compatible", "2 updates queued", k.green);
            ImGui::Spacing();
            ImGui::TextColored(k.brand, "Syncing required mods (2 of 4)…");
            progress_bar(0.58f, ImVec2(-1, ui_px(12.0f)));
            ImGui::TextColored(k.muted,
                               "Static progress only — no files are being downloaded or modified.");
        } else {
            ImGui::TextColored(k.muted,
                               "Check compatibility before syncing or launching Minecraft.");
        }
    } else if (detail_visible) {
        ImGui::TextColored(k.brand, "SESSION: Forsaken World SMP");
        ImGui::Spacing();
        ImGui::TextColored(k.text, "State");
        ImGui::SameLine(ui_px(170.0f));
        ImGui::TextColored(k.green, "● Online");
        ImGui::TextColored(k.text, "Players");
        ImGui::SameLine(ui_px(170.0f));
        ImGui::TextColored(k.text, "4 / 12");
        ImGui::TextColored(k.text, "Privacy");
        ImGui::SameLine(ui_px(170.0f));
        ImGui::TextColored(k.text, "Friends only");
        ImGui::Spacing();
        ImGui::TextColored(k.muted, "CONNECTED PLAYERS");
        ImGui::BeginDisabled(kFixtureActionDisabled);
        bool sample_player = true;
        ImGui::Checkbox("Alex  •  connected", &sample_player);
        ImGui::SameLine();
        ghost_button("Kick", ImVec2(ui_px(60.0f), ui_px(24.0f)), true);
        ImGui::EndDisabled();

        if (dialog->state == FixtureEssentialsDialogState::Working) {
            ImGui::Spacing();
            ImGui::TextColored(k.brand, "Applying session setting…");
            ImGui::TextColored(k.muted,
                               "This local preview is not changing privacy, players, or a hosted world.");
        } else if (dialog->state == FixtureEssentialsDialogState::Error) {
            ImGui::Spacing();
            card_begin("##fixture_session_manager_error", ImVec2(-1, 0));
            ImGui::PushFont(f_bold);
            ImGui::TextColored(k.red, "Could not update session settings");
            ImGui::PopFont();
            ImGui::TextWrapped("The sample session remains unchanged. You can retry safely.");
            card_end();
        }
    }
    ImGui::EndChild();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    draw_fixture_essentials_local_notice();
    ImGui::Spacing();
    if (ghost_button("Close preview", ImVec2(ui_px(124.0f), ui_px(32.0f)))) {
        preview_open = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine(0, ui_px(8.0f));

    const bool working = dialog->state == FixtureEssentialsDialogState::Working ||
                         dialog->state == FixtureEssentialsDialogState::Checking ||
                         dialog->state == FixtureEssentialsDialogState::Syncing;
    const bool error = dialog->state == FixtureEssentialsDialogState::Error;
    const char* action_label = "Apply";
    if (dialog->kind == FixtureEssentialsDialogKind::Host) {
        action_label = working ? "Starting…" : (error ? "Retry hosting" : "Start hosting");
    } else if (dialog->kind == FixtureEssentialsDialogKind::Invite) {
        action_label = working ? "Sending…" : (error ? "Retry invites" : "Send invites (3)");
    } else if (dialog->kind == FixtureEssentialsDialogKind::Join) {
        action_label = working ? (dialog->state == FixtureEssentialsDialogState::Checking
                                      ? "Checking…" : "Syncing…")
                    : (error ? "Retry check" : "Sync & join");
    } else {
        action_label = working ? "Saving…" : (error ? "Retry update" : "Apply changes");
    }
    primary_button(action_label, ImVec2(ui_px(152.0f), ui_px(32.0f)),
                   working, kFixtureActionDisabled);
    ImGui::EndPopup();
}

// The live friend-row menu performs profile navigation and relationship
// mutations.  This fixture reproduces its visual hierarchy with no callback
// or selected-friend state, so context-menu evidence stays local and inert.
static void draw_fixture_friend_context_menu(const std::string& fixture_case) {
    constexpr const char* kFixtureCase = "essentials-friend-context-menu";
    static std::string visible_fixture_case;
    static bool preview_open = false;
    if (visible_fixture_case != fixture_case) {
        visible_fixture_case = fixture_case;
        preview_open = fixture_case == kFixtureCase;
    }
    if (fixture_case != kFixtureCase || !preview_open) return;

    constexpr const char* popup_name = "Friend options###essentials_fixture_friend_context";
    const ImVec2 page_anchor = ImGui::GetWindowPos() + ImGui::GetWindowContentRegionMin();
    ImGui::SetNextWindowPos(page_anchor + ImVec2(ui_px(38.0f), ui_px(198.0f)),
                            ImGuiCond_Appearing);
    ImGui::OpenPopup(popup_name);
    if (!ImGui::BeginPopup(popup_name, ImGuiWindowFlags_NoSavedSettings)) return;

    ImGui::TextColored(k.muted, "MiraBuilds");
    ImGui::Separator();
    ImGui::BeginDisabled(true);
    ImGui::MenuItem("View Profile");
    ImGui::MenuItem("Message");
    ImGui::Separator();
    ImGui::MenuItem("Remove Friend");
    ImGui::MenuItem("Block User");
    ImGui::EndDisabled();
    ImGui::Separator();
    ImGui::TextColored(k.brand_hov, "LOCAL FIXTURE — actions disabled");
    if (ImGui::MenuItem("Close preview")) {
        preview_open = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// Friend-profile routes deliberately render beside the normal fixture façade
// rather than through the live account-backed profile panel. That panel
// resolves a selected account friend and can begin account-backed work; visual
// evidence needs a representative, entirely local composition instead.
static bool is_fixture_friend_profile_case(const std::string& fixture_case) {
    return fixture_case == "essentials-friend-profile" ||
           fixture_case == "essentials-friend-profile-remove-working" ||
           fixture_case == "essentials-friend-profile-remove-error" ||
           fixture_case == "essentials-friend-profile-block-working" ||
           fixture_case == "essentials-friend-profile-block-error";
}

static void draw_fixture_friend_profile(const std::string& fixture_case) {
    const bool friend_profile_route = is_fixture_friend_profile_case(fixture_case);

    // A capture process can traverse more than one fixture case. Reset local
    // visibility whenever its route changes, including through another
    // Essentials fixture, while allowing Close preview to work normally for
    // the active route without changing live Essentials state.
    static std::string visible_fixture_case;
    static bool preview_open = false;
    if (visible_fixture_case != fixture_case) {
        visible_fixture_case = fixture_case;
        preview_open = friend_profile_route;
    }
    if (!friend_profile_route || !preview_open) return;

    const bool remove_working = fixture_case == "essentials-friend-profile-remove-working";
    const bool remove_error = fixture_case == "essentials-friend-profile-remove-error";
    const bool block_working = fixture_case == "essentials-friend-profile-block-working";
    const bool block_error = fixture_case == "essentials-friend-profile-block-error";

    // These are intentionally plain constants, not an EssentialsFriend. That
    // keeps this visual-only presenter incapable of loading or disclosing an
    // account's actual social graph.
    struct FixtureFriendProfile {
        const char* display_name;
        const char* username;
        const char* presence;
        const char* current_profile;
        const char* game_version;
    };
    constexpr FixtureFriendProfile friend_profile{
        "MiraBuilds", "@mirabuilds", "Online — playing", "Forsaken World SMP", "MC 1.20.1 • Fabric"
    };

    constexpr const char* popup_name =
        "Friend Profile###essentials_fixture_friend_profile";
    ImGui::OpenPopup(popup_name);
    ImGui::SetNextWindowSize(ImVec2(
        std::min(ui_px(520.0f), ImGui::GetMainViewport()->WorkSize.x - ui_px(40.0f)),
        std::min(ui_px(420.0f), ImGui::GetMainViewport()->WorkSize.y - ui_px(40.0f))),
        ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(popup_name, &preview_open,
                                ImGuiWindowFlags_NoResize |
                                ImGuiWindowFlags_NoCollapse |
                                ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }

    ImGui::PushFont(f_h2);
    ImGui::TextColored(k.text, "%s", friend_profile.display_name);
    ImGui::PopFont();
    ImGui::SameLine(0, ui_px(8.0f));
    ImGui::TextColored(k.muted, "%s", friend_profile.username);
    ImGui::TextColored(k.green, "● %s", friend_profile.presence);
    ImGui::Spacing();

    ImGui::TextColored(k.muted, "PLAYING");
    ImGui::TextColored(k.text, "%s", friend_profile.current_profile);
    ImGui::TextColored(k.muted, "%s", friend_profile.game_version);
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextColored(k.brand_hov,
                       "LOCAL VISUAL FIXTURE — static sample data; no account, request, or network service is used.");
    ImGui::TextColored(k.muted,
                       "Invite, message, remove, and block are preview-only and cannot change a friend relationship.");

    if (remove_working || block_working) {
        ImGui::Spacing();
        ImGui::TextColored(k.brand, "%s", remove_working ? "Removing friend…" : "Blocking user…");
        ImGui::TextColored(k.muted,
                           "This local preview shows an in-progress state; no request is running.");
    } else if (remove_error || block_error) {
        ImGui::Spacing();
        card_begin("##fixture_friend_profile_error", ImVec2(-1, 0));
        ImGui::PushFont(f_bold);
        ImGui::TextColored(k.red, "%s",
                           remove_error ? "Could not remove friend" : "Could not block user");
        ImGui::PopFont();
        ImGui::TextWrapped("%s", remove_error
            ? "The friend relationship was not changed. You can retry safely."
            : "The user was not blocked. You can retry safely.");
        card_end();
    }

    ImGui::Spacing();
    const float button_width = ui_px(150.0f);
    const ImVec2 action_size(button_width, ui_px(30.0f));
    primary_button("Invite", action_size, false, true);
    ImGui::SameLine(0, ui_px(8.0f));
    ghost_button("Message", action_size, true);
    ImGui::Spacing();
    ghost_button(remove_working ? "Removing…" : "Remove Friend", action_size, true);
    ImGui::SameLine(0, ui_px(8.0f));
    ghost_button(block_working ? "Blocking…" : "Block", action_size, true);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (ghost_button("Close preview", ImVec2(ui_px(124.0f), ui_px(30.0f)))) {
        preview_open = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

static void draw_fixture_essentials_tab(UiState& st) {
    const int tab = fixture_essentials_tab(st.fixture_case);
    draw_page_emblem(st, "essentials-emblem-ai.png");
    page_title("Amalgam Essentials", "Connect, host, and play together — local visual representation");
    draw_breadcrumbs({"Home", "Essentials"});

    card_begin("##fixture_essentials_hero", ImVec2(-1, ui_px(116.0f)));
    ImGui::PushFont(f_title);
    ImGui::TextColored(k.text, "Your multiplayer hub");
    ImGui::PopFont();
    ImGui::TextColored(k.muted,
                       "Invite friends, host worlds, and keep every session in one place.");
    ImGui::Spacing();
    ImGui::TextColored(k.green, "● 3 friends online");
    ImGui::SameLine(0, ui_px(20.0f));
    ImGui::TextColored(k.text, "2 new invites");
    ImGui::SameLine(0, ui_px(20.0f));
    ImGui::TextColored(k.text, "1 active session");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - ui_px(365.0f));
    primary_button("Host a world", ImVec2(ui_px(116.0f), ui_px(30.0f)), false, true);
    ImGui::SameLine(0, ui_px(6.0f));
    ghost_button("Invite friends", ImVec2(ui_px(116.0f), ui_px(30.0f)), true);
    ImGui::SameLine(0, ui_px(6.0f));
    ghost_button("Join via code", ImVec2(ui_px(116.0f), ui_px(30.0f)), true);
    card_end();
    ImGui::TextColored(k.brand_hov,
                       "LOCAL VISUAL FIXTURE — no account, message, party, or session service is used.");
    ImGui::Spacing();

    const char* labels[] = {"Friends (3/5)", "Invites (2)", "Sessions (1)",
                            "Messages", "Parties", "Notifications (!2)"};
    const int ids[] = {0, 1, 2, 4, 5, 3};
    for (int i = 0; i < 6; ++i) {
        if (i > 0) ImGui::SameLine(0, ui_px(6.0f));
        fixture_essentials_tab_pill(labels[i], ids[i] == tab,
                                    ids[i] == 1 || ids[i] == 3);
    }
    ImGui::Spacing();
    ImGui::Spacing();

    if (tab == 0) {
        struct FixtureFriend { const char* name; const char* status; ImVec4 color; };
        const FixtureFriend friends[] = {
            {"Alex", "Playing Forge 1.20.1", k.brand},
            {"MiraBuilds", "In the launcher", k.blue},
            {"AveryStone", "Hosting a world", k.yellow},
            {"NovaCraft", "Away", k.muted},
            {"PixelRanger", "Offline", k.muted},
        };
        card_begin("##fixture_essentials_friends", ImVec2(-1, 0));
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("Friends");
        ImGui::PopFont();
        ImGui::TextColored(k.muted, "Representative presence indicators and session context.");
        ImGui::Spacing();
        for (const auto& friend_entry : friends) {
            ImGui::TextColored(friend_entry.color, "●");
            ImGui::SameLine(0, ui_px(7.0f));
            ImGui::PushFont(f_bold);
            ImGui::TextColored(k.text, "%s", friend_entry.name);
            ImGui::PopFont();
            ImGui::SameLine(0, ui_px(9.0f));
            ImGui::TextColored(k.muted, "%s", friend_entry.status);
        }
        card_end();
        ImGui::Spacing();
        card_begin("##fixture_essentials_activity", ImVec2(-1, 0));
        ImGui::PushFont(f_bold);
        ImGui::TextUnformatted("Active session");
        ImGui::PopFont();
        ImGui::TextColored(k.text, "Forsaken World SMP");
        ImGui::TextColored(k.green, "● Online  •  4 / 12 participants  •  Fabric 1.20.1");
        card_end();
    } else if (tab == 1) {
        card_begin("##fixture_essentials_invites", ImVec2(-1, 0));
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("Received invites");
        ImGui::PopFont();
        ImGui::Spacing();
        ImGui::TextColored(k.text, "AveryStone invited you to Redstone Builders");
        ImGui::TextColored(k.muted, "Fabric 1.20.1  •  3 / 8 players  •  just now");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextColored(k.text, "MiraBuilds invited you to Weekend Survival");
        ImGui::TextColored(k.muted, "Vanilla 1.21.1  •  2 / 6 players  •  6m ago");
        card_end();
    } else if (tab == 2) {
        card_begin("##fixture_essentials_sessions", ImVec2(-1, 0));
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("Your sessions");
        ImGui::PopFont();
        ImGui::TextColored(k.green, "● Forsaken World SMP is online");
        ImGui::TextColored(k.muted, "AveryStone hosting  •  Fabric 1.20.1  •  4 / 12 players");
        ImGui::Spacing();
        ghost_button("Manage session", ImVec2(ui_px(128.0f), ui_px(28.0f)), true);
        card_end();
    } else if (tab == 3) {
        card_begin("##fixture_essentials_notifications", ImVec2(-1, 0));
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("Notifications");
        ImGui::PopFont();
        ImGui::TextColored(k.text, "MiraBuilds accepted your invite");
        ImGui::TextColored(k.muted, "Ready to join Forsaken World SMP  •  just now");
        ImGui::Spacing();
        ImGui::TextColored(k.text, "NovaCraft is back in the launcher");
        ImGui::TextColored(k.muted, "Presence update  •  8m ago");
        card_end();
    } else if (tab == 4) {
        card_begin("##fixture_essentials_messages", ImVec2(-1, 0));
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("Messages");
        ImGui::PopFont();
        ImGui::TextColored(k.brand_hov, "MiraBuilds");
        ImGui::TextWrapped("I have the build ready. Want to join after this review pass?");
        ImGui::Spacing();
        ImGui::TextColored(k.muted, "You  •  Absolutely — I will join once the visual review is complete.");
        card_end();
    } else {
        card_begin("##fixture_essentials_parties", ImVec2(-1, 0));
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("Parties");
        ImGui::PopFont();
        ImGui::TextColored(k.text, "Redstone Builders");
        ImGui::TextColored(k.muted, "AveryStone  •  3 / 8 members  •  Public");
        ImGui::Spacing();
        ImGui::TextColored(k.text, "Weekend Survival");
        ImGui::TextColored(k.muted, "MiraBuilds  •  2 / 6 members  •  Invite only");
        card_end();
    }

    draw_fixture_essentials_dialog(st.fixture_case);
    draw_fixture_friend_profile(st.fixture_case);
    draw_fixture_friend_context_menu(st.fixture_case);
}

// ---------------------------------------------------------------------------
// Activity feed — built from real manager data (notifications + sessions)
// ---------------------------------------------------------------------------

static void build_activity_feed(UiState&) {
    auto& ui = get_essentials_ui_state();
    const int64_t now = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());

    // Rebuild at most once per second to avoid churn.
    if (now == ui.activity_last_built) return;
    ui.activity_last_built = now;
    ui.activity_feed.clear();

    auto push = [&](IconId icon, const ImVec4& color,
                    const std::string& text, int64_t ts) {
        if (ui.activity_feed.size() >= 6) return;
        EssentialsUIState::ActivityEntry e;
        e.icon = icon;
        e.color = color;
        e.text = text;
        e.time = time_ago(ts);
        e.timestamp = ts;
        ui.activity_feed.push_back(std::move(e));
    };

    // Real notifications (player joined / friend request / invite / session end).
    auto& sm = SessionManager::instance();
    auto notifs = sm.get_notifications();
    std::vector<EssentialsNotification> sorted = notifs;
    std::sort(sorted.begin(), sorted.end(), [](const EssentialsNotification& a,
                                                const EssentialsNotification& b) {
        return a.created_at > b.created_at;
    });
    for (auto& n : sorted) {
        const std::string lower_title = n.title;
        ImVec4 col = k.brand;
        IconId icon = IconId::Info;
        if (lower_title.find("join") != std::string::npos ||
            lower_title.find("Join") != std::string::npos) {
            col = k.green;
            icon = IconId::Play;
        } else if (lower_title.find("friend") != std::string::npos ||
                   lower_title.find("request") != std::string::npos) {
            col = k.brand;
            icon = IconId::User;
        } else if (lower_title.find("invite") != std::string::npos ||
                   lower_title.find("Invite") != std::string::npos) {
            col = k.yellow;
            icon = IconId::Bell;
        } else if (lower_title.find("left") != std::string::npos ||
                   lower_title.find("end") != std::string::npos) {
            col = k.red;
            icon = IconId::Close;
        }
        std::string text = n.title;
        if (!n.body.empty()) text += " — " + n.body;
        push(icon, col, text, n.created_at);
    }

    // Hosting status is real activity.
    auto& wh = WorldHost::instance();
    if (wh.is_hosting()) {
        auto session = wh.get_current_session();
        push(IconId::Globe, k.brand,
             "You are hosting " + session.world_name, session.created_at);
    }

    // Active friend sessions are real activity.
    auto sessions = sm.get_active_sessions();
    for (auto& s : sessions) {
        if (s.host_user_id.empty()) continue;
        push(IconId::Play, k.green,
             s.host_username + " is hosting " + s.world_name, s.created_at);
    }

    std::sort(ui.activity_feed.begin(), ui.activity_feed.end(),
              [](const EssentialsUIState::ActivityEntry& a,
                 const EssentialsUIState::ActivityEntry& b) {
                  return a.timestamp > b.timestamp;
              });
}

static void draw_activity_feed(UiState& st) {
    auto& ui = get_essentials_ui_state();
    build_activity_feed(st);

    card_begin("##activity_feed", ImVec2(-1, 0));
    ImGui::PushFont(f_h2);
    ImGui::TextColored(k.text, "Recent Activity");
    ImGui::PopFont();
    ImGui::SameLine(0, ui_px(12.0f));
    ImGui::TextColored(k.muted, "(%d)", (int)ui.activity_feed.size());
    ImGui::Spacing();

    if (ui.activity_feed.empty()) {
        ImGui::TextColored(k.muted, "No activity yet.");
        ImGui::TextColored(k.muted, "Invite a friend or host a world to get started.");
    } else {
        for (size_t i = 0; i < ui.activity_feed.size(); ++i) {
            const auto& e = ui.activity_feed[i];
            ImGui::PushID(static_cast<int>(i));
            const ImVec2 icon_center = ImGui::GetCursorScreenPos() + ImVec2(ui_px(7.0f), ui_px(9.0f));
            draw_icon(e.icon, icon_center, ui_px(6.5f), c32(e.color));
            ImGui::Dummy(ImVec2(ui_px(14.0f), 0));
            ImGui::SameLine(0, ui_px(6.0f));
            ImGui::TextWrapped("%s", e.text.c_str());
            if (!e.time.empty()) {
                ImGui::SameLine();
                ImGui::TextColored(k.muted, "\xe2\x80\xa2 %s", e.time.c_str());
            }
            ImGui::PopID();
            if (i + 1 < ui.activity_feed.size()) ImGui::Spacing();
        }
    }
    card_end();
}

// ---------------------------------------------------------------------------
// Quick actions grid
// ---------------------------------------------------------------------------

static void draw_quick_actions(UiState& st) {
    auto& ui = get_essentials_ui_state();

    card_begin("##quick_actions", ImVec2(-1, 0));
    ImGui::PushFont(f_h2);
    ImGui::TextColored(k.text, "Quick Actions");
    ImGui::PopFont();
    ImGui::Spacing();

    struct Action {
        IconId icon;
        const char* label;
        bool primary;
    };
    // Hosting and joining stay in the hero command bar. Keeping this rail
    // focused on social management avoids repeating the same three actions in
    // every column of the control center.
    const Action actions[] = {
        {IconId::User, "Add Friend", false},
        {IconId::Users, "Create Party", false},
        {IconId::Bell, "View Invites", false},
        {IconId::Settings, "Essentials Settings", false},
    };

    const float gap = ui_px(8.0f);
    const int per_row = 2;
    const float avail = ImGui::GetContentRegionAvail().x;
    const float tile_w = (avail - gap * (per_row - 1)) / per_row;
    const float tile_h = ui_px(56.0f);

    for (int i = 0; i < 4; ++i) {
        if (i > 0 && i % per_row != 0) ImGui::SameLine(0, gap);
        const auto& a = actions[i];
        const bool is_last_in_row = (i % per_row == per_row - 1);

        ImGui::PushID(i);
        ImVec4 bg = a.primary ? k.brand : k.surface;
        ImVec4 bg_hov = a.primary ? k.brand_hov : k.hover;
        ImGui::PushStyleColor(ImGuiCol_Button, bg);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bg_hov);
        if (ImGui::Button("##qa_tile", ImVec2(tile_w, tile_h))) {
            switch (i) {
                case 0: ui.quick_add_friend = true; ui.show_add_friend = true; break;
                case 1: ui.current_tab = 5; break;  // Parties
                case 2: ui.current_tab = 1; break;  // Invites
                case 3:
                    // Essentials settings live on the Settings page.
                    st.sidebar_item = 12;
                    st.active_tab = 4;
                    break;
                default: break;
            }
        }
        ImGui::PopStyleColor(2);

        // Draw icon + label centered over the button.
        ImVec2 btn_min = ImGui::GetItemRectMin();
        ImVec2 btn_max = ImGui::GetItemRectMax();
        ImVec2 center = ImVec2((btn_min.x + btn_max.x) * 0.5f,
                               (btn_min.y + btn_max.y) * 0.5f);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec4 icon_col = a.primary ? ImVec4(1,1,1,1) : k.brand;
        draw_icon(a.icon, ImVec2(center.x, btn_min.y + ui_px(18.0f)),
                  ui_px(9.0f), c32(icon_col));
        ImVec2 label_ts = ImGui::CalcTextSize(a.label);
        dl->AddText(ImVec2(center.x - label_ts.x * 0.5f,
                           btn_max.y - ui_px(8.0f) - label_ts.y),
                    c32(k.text), a.label);
        if (ImGui::IsItemHovered() && !a.primary) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
        // The tile intentionally uses a visual-only button label; expose the
        // action name as a tooltip so keyboard/mouse users never have to infer
        // what an icon tile does.
        draw_tooltip(a.label);
        ImGui::PopID();
        (void)is_last_in_row;
    }
    card_end();
}

// ---------------------------------------------------------------------------
// Your Status card
// ---------------------------------------------------------------------------

static const char* presence_label(FriendStatus s) {
    return friend_status_name(s);
}

static void draw_your_status(UiState& st) {
    auto& ui = get_essentials_ui_state();

    const std::string username = player_display_name(st);
    const char* name_ptr = username.empty() ? "Player" : username.c_str();

    card_begin("##your_status", ImVec2(-1, 0));
    ImGui::PushFont(f_h2);
    ImGui::TextColored(k.text, "Your Status");
    ImGui::PopFont();
    ImGui::Spacing();

    // Avatar + name + presence
    ImVec4 ac = avatar_color(username.empty() ? "Amalgam" : username);
    draw_avatar_circle(std::string(1, name_initial(username.empty() ? "Amalgam" : username)).c_str(),
                       ac, ui_px(16.0f));
    ImGui::SameLine(0, ui_px(10.0f));
    ImGui::BeginGroup();
    ImGui::PushFont(f_bold);
    ImGui::TextUnformatted(name_ptr);
    ImGui::PopFont();
    ImVec4 sc = status_color(ui.self_status);
    ImGui::TextColored(sc, "\xe2\x97\x8f %s", presence_label(ui.self_status));
    ImGui::EndGroup();

    ImGui::Spacing();

    // Current activity (hosting or playing)
    auto& wh = WorldHost::instance();
    if (wh.is_hosting()) {
        auto session = wh.get_current_session();
        ImGui::TextColored(k.muted, "Hosting %s", session.world_name.c_str());
        ImGui::TextColored(k.muted, "MC %s \xe2\x80\xa2 %s",
                           session.minecraft_version.c_str(),
                           session.loader.c_str());
    } else if (!st.selected.empty()) {
        ImGui::TextColored(k.muted, "In Launcher");
    } else {
        ImGui::TextColored(k.muted, "In Launcher");
    }

    ImGui::Spacing();

    // Status editor toggle
    if (!ui.status_editor_open) {
        if (ghost_button("Set Custom Status", ImVec2(ui_px(150.0f), ui_px(26.0f))))
            ui.status_editor_open = true;
    } else {
        static const char* kStatuses[] = {"Online", "Away", "Busy", "Invisible"};
        ImGui::TextColored(k.muted, "Presence");
        for (int i = 0; i < 4; ++i) {
            if (i > 0) ImGui::SameLine(0, ui_px(8.0f));
            const bool selected = (ui.status_editor_selection == i);
            if (selected) ImGui::PushStyleColor(ImGuiCol_Button, k.brand);
            else          ImGui::PushStyleColor(ImGuiCol_Button, k.surface);
            if (ImGui::Button(kStatuses[i]))
                ui.status_editor_selection = i;
            ImGui::PopStyleColor();
        }
        ImGui::Spacing();
        ImGui::SetNextItemWidth(-1);
        input_text_hint("##self_status_msg", "What are you up to?",
                        &ui.self_status_message);
        ImGui::Spacing();
        const bool setting_status = essentials_request_is_working(st, "status-set");
        const bool status_lane_busy = essentials_request_lane_busy(st);
        const bool retry_status = essentials_request_failed(st, "status-set");
        if (primary_button(setting_status ? "Saving..." :
                           (retry_status ? "Retry Status" : "Set Status"),
                           ImVec2(ui_px(90.0f), ui_px(26.0f)), setting_status,
                           status_lane_busy)) {
            FriendStatus requested = FriendStatus::Online;
            switch (ui.status_editor_selection) {
                case 0: requested = FriendStatus::Online; break;
                case 1: requested = FriendStatus::Away; break;
                case 2: requested = FriendStatus::InLauncher; break;
                case 3: requested = FriendStatus::Offline; break;
                default: break;
            }
            const std::string message = ui.self_status_message;
            start_essentials_request(st, "status-set", [requested, message] {
                AsyncUiRequestResult result;
                result.success = PresenceManager::instance().set_status(requested, message);
                result.title = result.success ? "Status updated" : "Could not update status";
                result.detail = result.success
                    ? "Your presence is now visible to friends."
                    : "Your previous presence remains active. You can retry.";
                result.payload_a = std::to_string(static_cast<int>(requested));
                return result;
            });
        }
        ImGui::SameLine();
        if (ghost_button("Cancel", ImVec2(ui_px(70.0f), ui_px(26.0f)), status_lane_busy))
            ui.status_editor_open = false;
        draw_essentials_request_feedback(st, "status-set", "Updating your status...");
    }
    card_end();
}

// ---------------------------------------------------------------------------
// Connection card
// ---------------------------------------------------------------------------

static void draw_connection_card(UiState& st) {
    card_begin("##connection_card", ImVec2(-1, 0));
    ImGui::PushFont(f_h2);
    ImGui::TextColored(k.text, "Connection");
    ImGui::PopFont();
    ImGui::Spacing();

    auto& wh = WorldHost::instance();
    const bool hosting = wh.is_hosting();
    const auto status = ConnectionManager::instance().get_status();

    // Guest connections report the transport that was actually negotiated.
    // A host is not itself connected to a remote peer, so show the honest
    // preference/fallback state instead of inventing a latency or relay value.
    if (ConnectionManager::instance().is_connected()) {
        ImVec4 color = k.green;
        const char* transport = "Direct P2P";
        if (status.type == ConnectionType::Relay) {
            color = k.yellow;
            transport = "TURN Relay";
        } else if (status.type == ConnectionType::Failed) {
            color = k.red;
            transport = "Connection failed";
        }
        ImGui::TextColored(color, "● %s", transport);
        if (status.ping_ms > 0)
            ImGui::SameLine(0, ui_px(10.0f));
        if (status.ping_ms > 0) ImGui::TextColored(k.muted, "%d ms", status.ping_ms);
        if (!status.remote_address.empty())
            ImGui::TextColored(k.muted, "%s", status.remote_address.c_str());
    } else if (hosting) {
        ImGui::TextColored(k.green, "● Host online");
        ImGui::TextColored(k.muted, "Direct P2P preferred; TURN Relay is the fallback.");
        const auto session = wh.get_current_session();
        if (!session.address.empty()) {
            ImGui::TextColored(k.muted, "Address");
            ImGui::TextColored(k.brand, "%s", session.address.c_str());
            if (ghost_button("Copy Address", ImVec2(ui_px(110.0f), ui_px(24.0f)))) {
                ImGui::SetClipboardText(session.address.c_str());
                push_notice(st, ui_model::NoticeLevel::Success, "Address Copied",
                            session.address);
            }
        }
    } else {
        ImGui::TextColored(k.blue, "● Ready");
        ImGui::TextColored(k.muted, "Direct P2P will be tried before TURN Relay.");
    }
    card_end();
}

// ---------------------------------------------------------------------------
// Essentials info / relay usage card
// ---------------------------------------------------------------------------

static void draw_essentials_info_card(UiState& /*st*/) {
    card_begin("##essentials_info", ImVec2(-1, 0));
    ImGui::PushFont(f_h2);
    ImGui::TextColored(k.text, "Essentials Info");
    ImGui::PopFont();
    ImGui::Spacing();
    ImGui::TextWrapped(
        "Essentials lets you host singleplayer worlds, invite friends, and "
        "play together. Relay usage is tracked separately from direct P2P.");
    ImGui::Spacing();

    // Relay usage from entitlements when available (real backend data).
    const auto& ents = aml::entitlements::EntitlementManager::instance().snapshot();
    if (ents.turn_monthly_bytes > 0) {
        const float gb = 1024.0f * 1024.0f * 1024.0f;
        const uint64_t used = ents.turn_used_bytes;
        const uint64_t total = ents.turn_monthly_bytes;
        const float frac = total > 0 ? static_cast<float>(static_cast<double>(used) / total) : 0.0f;
        ImGui::TextColored(k.muted, "Relay Usage");
        ImGui::SameLine(0, ui_px(24.0f));
        ImGui::TextColored(k.text, "%.1f / %.1f GB", used / gb, total / gb);
        progress_bar(frac, ImVec2(-1, ui_px(14.0f)));
        if (ents.turn_reset_at > 0) {
            char buf[64];
            std::tm tmv;
            const std::time_t ts = static_cast<std::time_t>(ents.turn_reset_at);
            localtime_s(&tmv, &ts);
            std::strftime(buf, sizeof(buf), "Resets %b %d, %Y", &tmv);
            ImGui::TextColored(k.muted, "%s", buf);
        }
    } else {
        ImGui::TextColored(k.muted, "Relay usage will appear once you sign in.");
    }
    card_end();
}

// ---------------------------------------------------------------------------
// Friend profile side panel
// ---------------------------------------------------------------------------

static void draw_friend_profile_panel(UiState& st) {
    auto& ui = get_essentials_ui_state();
    if (!ui.friend_profile_panel_open || ui.selected_friend_id.empty()) return;

    EssentialsFriend fri_profile;
    bool found = false;
    for (auto& f : FriendsManager::instance().get_friends()) {
        if (f.user_id == ui.selected_friend_id) {
            fri_profile = f;
            found = true;
            break;
        }
    }
    if (!found) {
        ui.friend_profile_panel_open = false;
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(ui_px(340.0f), ui_px(480.0f)), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::Begin("Friend Profile", &ui.friend_profile_panel_open,
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse)) {
        ImGui::PushFont(f_h2);
        ImGui::TextColored(k.text, "%s",
            fri_profile.display_name.empty() ? fri_profile.username.c_str() : fri_profile.display_name.c_str());
        ImGui::PopFont();

        ImVec4 sc = status_color(fri_profile.status);
        ImGui::TextColored(sc, "\xe2\x97\x8f %s",
            fri_profile.status_message.empty() ? friend_status_name(fri_profile.status)
                                               : fri_profile.status_message.c_str());
        ImGui::Spacing();

        // Current activity
        if (!fri_profile.current_profile_name.empty()) {
            ImGui::TextColored(k.muted, "Playing");
            ImGui::TextColored(k.text, "%s", fri_profile.current_profile_name.c_str());
            if (!fri_profile.current_game_version.empty()) {
                ImGui::TextColored(k.muted, "MC %s", fri_profile.current_game_version.c_str());
            }
        } else if (fri_profile.status == FriendStatus::InLauncher) {
            ImGui::TextColored(k.muted, "In Launcher");
        }

        if (fri_profile.last_seen > 0 && fri_profile.status == FriendStatus::Offline) {
            ImGui::TextColored(k.muted, "Last seen %s", time_ago(fri_profile.last_seen).c_str());
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Actions
        float bw = ui_px(130.0f);
        float bh = ui_px(30.0f);

        if (fri_profile.status != FriendStatus::Offline) {
            if (primary_button("Invite", ImVec2(bw, bh))) {
                auto& sm = SessionManager::instance();
                auto sid = sm.get_current_session_id();
                if (!sid.empty()) {
                    ui.invite_session_id = sid;
                    ui.invite_dialog_open = true;
                } else {
                    push_notice(st, ui_model::NoticeLevel::Warning,
                                "No Active Session",
                                "Host a world first before inviting friends.");
                }
            }
            ImGui::SameLine();
            if (ghost_button("Message", ImVec2(bw, bh))) {
                ui.current_tab = 4;
                ui.friend_profile_panel_open = false;
            }
            ImGui::Spacing();
        }

        const std::string remove_action = "friend-remove:" + fri_profile.user_id;
        const bool removing = essentials_request_is_working(st, remove_action);
        const bool action_lane_busy = essentials_request_lane_busy(st);
        if (ghost_button(removing ? "Removing..." : "Remove Friend", ImVec2(bw, bh),
                         action_lane_busy)) {
            const std::string user_id = fri_profile.user_id;
            const std::string display_name = fri_profile.display_name.empty()
                ? fri_profile.username : fri_profile.display_name;
            start_essentials_request(st, remove_action, [user_id, display_name] {
                AsyncUiRequestResult result;
                result.success = FriendsManager::instance().remove_friend(user_id);
                result.title = result.success ? "Friend removed" : "Could not remove friend";
                result.detail = result.success
                    ? display_name + " has been removed."
                    : "The friend relationship was not changed. You can retry safely.";
                return result;
            });
        }
        ImGui::SameLine();
        const std::string block_action = "friend-block:" + fri_profile.user_id;
        const bool blocking = essentials_request_is_working(st, block_action);
        if (ghost_button(blocking ? "Blocking..." : "Block", ImVec2(bw, bh), action_lane_busy)) {
            const std::string user_id = fri_profile.user_id;
            const std::string display_name = fri_profile.display_name.empty()
                ? fri_profile.username : fri_profile.display_name;
            start_essentials_request(st, block_action, [user_id, display_name] {
                AsyncUiRequestResult result;
                result.success = FriendsManager::instance().block_user(user_id);
                result.title = result.success ? "User blocked" : "Could not block user";
                result.detail = result.success
                    ? display_name + " has been blocked."
                    : "The user was not blocked. You can retry safely.";
                return result;
            });
        }
        draw_essentials_request_feedback(st, remove_action, "Removing friend...");
        draw_essentials_request_feedback(st, block_action, "Blocking user...");
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Empty state (no friends yet)
// ---------------------------------------------------------------------------

static void draw_essentials_empty_state(UiState& st) {
    auto& ui = get_essentials_ui_state();

    // Friends, sessions, and invites are all account-backed, so a signed-out
    // visitor gets a sign-in prompt instead of actions that cannot succeed.
    const bool signed_in = aml::supabase::SupabaseManager::instance().is_authenticated();

    // Left: add friend
    ImGui::PushFont(f_h2);
    ImGui::TextColored(k.text, "Friends");
    ImGui::PopFont();
    ImGui::Spacing();

    if (!signed_in) {
        card_begin("##empty_sign_in", ImVec2(-1, 0));
        ImGui::TextColored(k.muted, "Friends, worlds, and invites need an");
        ImGui::TextColored(k.muted, "Amalgam account. Sign in to get started.");
        ImGui::Spacing();
        if (primary_button("Sign In", ImVec2(ui_px(140.0f), ui_px(32.0f)))) {
            st.auth_prompt_dismissed = false;
            st.login_popup_open = true;
        }
        card_end();
        ImGui::Spacing();
        return;
    }

    card_begin("##empty_add_friend", ImVec2(-1, 0));
    ImGui::TextColored(k.muted, "Add friends to start hosting worlds,");
    ImGui::TextColored(k.muted, "joining sessions, and playing together.");
    ImGui::Spacing();
    if (primary_button("+ Add Your First Friend",
                       ImVec2(ui_px(180.0f), ui_px(32.0f)))) {
        ui.show_add_friend = true;
        ui.quick_add_friend = true;
    }
    ImGui::Spacing();
    if (ui.show_add_friend) {
        ImGui::TextColored(k.brand, "Friend code (AMG-XXXX-XXXX):");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##add_friend_input", &ui.add_friend_input);
        const bool sending_request = essentials_request_is_working(st, "friend-request");
        const bool request_lane_busy = essentials_request_lane_busy(st);
        const bool retry_request = essentials_request_failed(st, "friend-request");
        if (ghost_button(sending_request ? "Sending..." :
                         (retry_request ? "Retry Request" : "Send Request"),
                         ImVec2(ui_px(140.0f), ui_px(26.0f)), request_lane_busy)) {
            if (!ui.add_friend_input.empty()) {
                const std::string friend_code = ui.add_friend_input;
                start_essentials_request(st, "friend-request", [friend_code] {
                    AsyncUiRequestResult result;
                    result.success = FriendsManager::instance().send_request(friend_code);
                    result.title = result.success ? "Friend request sent" : "Request failed";
                    result.detail = result.success
                        ? "Waiting for the other player to accept."
                        : "The request was not sent. Check the code and retry.";
                    return result;
                });
            }
        }
        draw_essentials_request_feedback(st, "friend-request", "Sending friend request...");
    }
    card_end();

    ImGui::Spacing();

    // Feature cards (vector icons, consistent with the design system)
    const IconId feature_icons[] = {IconId::Globe, IconId::Users, IconId::Cube, IconId::Play};
    const char* feature_names[] = {"Host Worlds", "Invite Friends", "Sync Modpacks", "Join Sessions"};
    for (int i = 0; i < 4; ++i) {
        if (i > 0 && i % 2 != 0) ImGui::SameLine(0, ui_px(8.0f));
        float avail = ImGui::GetContentRegionAvail().x;
        float tile_w = (avail - ui_px(8.0f)) * 0.5f;
        card_begin(("##feat_" + std::to_string(i)).c_str(), ImVec2(tile_w, ui_px(64.0f)));
        const ImVec2 icon_center = ImGui::GetCursorScreenPos() +
            ImVec2(ui_px(20.0f), ui_px(20.0f));
        draw_icon(feature_icons[i], icon_center, ui_px(12.0f), c32(k.brand));
        ImGui::SetCursorPos(ImVec2(ui_px(48.0f), ui_px(20.0f)));
        ImGui::TextColored(k.text, "%s", feature_names[i]);
        card_end();
    }
    ImGui::Spacing();

    // Center: illustration + CTA
    ImGui::PushFont(f_h2);
    ImGui::TextColored(k.text, "No Friends Yet");
    ImGui::PopFont();
    ImGui::TextColored(k.muted,
        "Add friends to start hosting worlds, joining sessions, and playing together.");
    ImGui::Spacing();
    if (primary_button("Host a World", ImVec2(ui_px(140.0f), ui_px(32.0f)))) {
        ui.host_dialog_open = true;
    }
    ImGui::SameLine();
    if (ghost_button("Join via Code", ImVec2(ui_px(130.0f), ui_px(32.0f)))) {
        ui.hero_join_code = true;
        if (ui.join_syncing) ui.join_dialog_open = true;
        else open_fresh_join_dialog(ui);
    }
}

// ---------------------------------------------------------------------------
// Hero header
// ---------------------------------------------------------------------------

static void draw_essentials_hero(UiState& st) {
    auto& ui = get_essentials_ui_state();

    auto& fm = FriendsManager::instance();
    auto friends = fm.get_friends();
    int online_count = 0;
    for (auto& f : friends)
        if (f.status != FriendStatus::Offline) ++online_count;

    auto& im = InviteManager::instance();
    auto received = im.get_received_invites();
    int pending_invites = 0;
    for (auto& inv : received)
        if (inv.status == InviteStatus::Pending) ++pending_invites;

    auto& sm = SessionManager::instance();
    auto sessions = sm.get_active_sessions();
    auto& wh = WorldHost::instance();

    // ── Branded social-world background ───────────────────────────
    const bool compact_hero = ImGui::GetContentRegionAvail().x < ui_px(900.0f);
    const float hero_h = ui_px(compact_hero ? 156.0f : 96.0f);
    ImVec2 hero_min = ImGui::GetCursorScreenPos();
    ImVec2 hero_max = ImVec2(hero_min.x + ImGui::GetContentRegionAvail().x,
                             hero_min.y + hero_h);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    draw_local_image(st, st.exe_dir + L"\\branding\\ai\\launcher-essentials-ai.png",
                     hero_min, hero_max - hero_min, c32(k.brand_dk));
    dl->AddRectFilled(hero_min, hero_max,
                      c32(ImVec4(0.02f, 0.01f, 0.06f, 0.46f)), ui_px(12.0f));
    dl->AddRect(hero_min, hero_max, c32(ImVec4(k.brand.x, k.brand.y, k.brand.z, 0.25f)), ui_px(12.0f));

    ImGui::Dummy(ImVec2(ui_px(18.0f), hero_h));
    ImGui::SetCursorScreenPos(ImVec2(hero_min.x + ui_px(18.0f), hero_min.y + ui_px(14.0f)));

    // Title + subtitle.  The page header above already reads "Amalgam
    // Essentials"; repeat the product name here and the hero reads as a
    // duplicate.  The hero carries status + actions instead.
    ImGui::PushFont(f_title);
    ImGui::TextColored(k.text, "Essentials");
    ImGui::PopFont();
    ImGui::SameLine(0, ui_px(10.0f));
    ImVec4 online_col = k.green;
    ImGui::PushFont(f_small);
    ImGui::TextColored(online_col, "\xe2\x97\x8f Online");
    ImGui::PopFont();

    ImGui::TextColored(k.muted, "Connect, host, invite, sync, and play together.");

    // Action buttons at right of hero
    float btn_w = ui_px(120.0f);
    float btn_h = ui_px(30.0f);
    float btn_y = hero_min.y + ui_px(compact_hero ? 74.0f : 50.0f);
    float x = hero_max.x - ui_px(16.0f);

    auto place_right = [&](const char* label, bool primary) {
        x -= btn_w;
        ImGui::SetCursorScreenPos(ImVec2(x, btn_y));
        bool clicked;
        if (primary)
            clicked = primary_button(label, ImVec2(btn_w, btn_h));
        else
            clicked = ghost_button(label, ImVec2(btn_w, btn_h));
        x -= ui_px(8.0f);
        return clicked;
    };

    if (place_right("Join via Code", false)) {
        ui.hero_join_code = true;
        if (ui.join_syncing) ui.join_dialog_open = true;
        else open_fresh_join_dialog(ui);
    }
    if (place_right("Invite Friends", false)) {
        auto& sm2 = SessionManager::instance();
        auto sid = sm2.get_current_session_id();
        if (!sid.empty()) {
            ui.invite_session_id = sid;
            ui.invite_dialog_open = true;
        } else {
            push_notice(st, ui_model::NoticeLevel::Warning,
                        "No Active Session",
                        "Host a world first before inviting friends.");
        }
    }
    if (place_right("+ Host World", true)) {
        ui.host_dialog_open = true;
    }

    // Stats row below hero content
    ImGui::SetCursorScreenPos(ImVec2(hero_min.x + ui_px(18.0f), hero_min.y + hero_h - ui_px(26.0f)));
    ImGui::TextColored(k.text, "Friends %d", (int)friends.size());
    ImGui::SameLine(0, ui_px(6.0f));
    ImGui::TextColored(k.green, "(Online %d)", online_count);
    ImGui::SameLine(0, ui_px(24.0f));
    ImGui::TextColored(k.text, "Invites %d", pending_invites);
    ImGui::SameLine(0, ui_px(6.0f));
    ImGui::TextColored(k.yellow, "(New)");
    ImGui::SameLine(0, ui_px(24.0f));
    ImGui::TextColored(k.text, "Active Sessions %d", (int)sessions.size() + (wh.is_hosting() ? 1 : 0));

    // Advance cursor past the hero
    ImGui::SetCursorScreenPos(ImVec2(hero_min.x, hero_max.y + ui_px(12.0f)));
    ImGui::Spacing();
}

// ---------------------------------------------------------------------------
// Toast Notification Renderer
// ---------------------------------------------------------------------------

static void draw_toast_notification(UiState& /*st*/) {
    auto& ui = get_essentials_ui_state();
    if (ui.toast_timer <= 0.0f) return;

    ui.toast_timer -= ImGui::GetIO().DeltaTime;
    if (ui.toast_timer <= 0.0f) return;

    float alpha = std::min(ui.toast_timer, 1.0f);
    float toast_w = ui_px(320.0f);
    float toast_h = ui_px(60.0f);
    ImVec2 vp = ImGui::GetMainViewport()->Pos;
    ImVec2 vp_sz = ImGui::GetMainViewport()->Size;
    float x = vp.x + vp_sz.x - toast_w - ui_px(16.0f);
    float y = vp.y + ui_px(16.0f);

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImVec4 bg = ImVec4(0.12f, 0.12f, 0.15f, 0.95f * alpha);
    ImVec4 border_col = ui.toast_color;
    border_col.w = 0.6f * alpha;

    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + toast_w, y + toast_h),
                      c32(bg), ui_px(8.0f));
    dl->AddRect(ImVec2(x, y), ImVec2(x + toast_w, y + toast_h),
                c32(border_col), ui_px(8.0f), 0, ui_px(1.5f));

    float accent_w = ui_px(4.0f);
    ImVec4 accent = ui.toast_color;
    accent.w = alpha;
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + accent_w, y + toast_h),
                      c32(accent), ui_px(8.0f), ImDrawFlags_RoundCornersLeft);

    ImVec4 title_col = k.text;
    title_col.w = alpha;
    ImVec4 muted_col = k.muted;
    muted_col.w = alpha;

    ImGui::SetCursorScreenPos(ImVec2(x + accent_w + ui_px(10.0f), y + ui_px(8.0f)));
    ImGui::PushFont(f_bold);
    ImGui::TextColored(title_col, "%s", ui.toast_title.c_str());
    ImGui::PopFont();

    ImGui::SetCursorScreenPos(ImVec2(x + accent_w + ui_px(10.0f),
                                     y + ui_px(26.0f)));
    ImGui::TextColored(muted_col, "%s", ui.toast_message.c_str());

    ImGui::SetCursorScreenPos(ImVec2(x, y));
}

// ---------------------------------------------------------------------------
// Main Tab
// ---------------------------------------------------------------------------

void draw_essentials_tab(UiState& st) {
    if (st.fixture_mode) {
        draw_fixture_essentials_tab(st);
        return;
    }

    consume_essentials_request_result(st);
    refresh_join_progress(st);
    auto& ui = get_essentials_ui_state();

    draw_page_emblem(st, "essentials-emblem-ai.png");
    page_title("Amalgam Essentials",
               "Connect with friends and host multiplayer sessions");
    draw_breadcrumbs({"Home", "Essentials"});

    // Update unread notification count
    auto& sm = SessionManager::instance();
    auto notifs = sm.get_notifications();
    ui.unread_notification_count = 0;
    for (auto& n : notifs) {
        if (!n.read) ++ui.unread_notification_count;
    }

    // ── Hero header ───────────────────────────────────────────────
    draw_essentials_hero(st);
    ImGui::Spacing();

    // Toast overlay
    draw_toast_notification(st);

    // ── Tab bar with counts ───────────────────────────────────────
    {
        auto& fm = FriendsManager::instance();
        auto friends = fm.get_friends();
        int online_count = 0;
        for (auto& f : friends) {
            if (f.status != FriendStatus::Offline) ++online_count;
        }

        auto& im = InviteManager::instance();
        auto received = im.get_received_invites();
        int pending_invites = 0;
        for (auto& inv : received) {
            if (inv.status == InviteStatus::Pending) ++pending_invites;
        }

        auto& sm2 = SessionManager::instance();
        auto sessions = sm2.get_active_sessions();

        // Premium pill tabs (matching the Library/design-system tab language)
        struct EssTab { const char* label; int id; int count; bool hot; };
        std::vector<EssTab> ess_tabs;
        std::string friends_label = "Friends";
        if (!friends.empty())
            friends_label += " (" + std::to_string(online_count) + "/" +
                             std::to_string((int)friends.size()) + ")";
        ess_tabs.push_back({friends_label.c_str(), 0, 0, false});
        std::string invites_label = "Invites";
        if (pending_invites > 0)
            invites_label += " (" + std::to_string(pending_invites) + ")";
        ess_tabs.push_back({invites_label.c_str(), 1, 0, pending_invites > 0});
        std::string sessions_label = "Sessions";
        if (!sessions.empty())
            sessions_label += " (" + std::to_string((int)sessions.size()) + ")";
        ess_tabs.push_back({sessions_label.c_str(), 2, 0, false});
        ess_tabs.push_back({"Messages", 4, 0, false});
        ess_tabs.push_back({"Parties", 5, 0, false});
        std::string notif_label = "Notifications";
        if (ui.unread_notification_count > 0)
            notif_label += " (!" + std::to_string(ui.unread_notification_count) + ")";
        ess_tabs.push_back({notif_label.c_str(), 3, 0, ui.unread_notification_count > 0});

        const float tab_gap = ui_px(6.0f);
        for (size_t i = 0; i < ess_tabs.size(); ++i) {
            const bool active = ui.current_tab == ess_tabs[i].id;
            const char* label = ess_tabs[i].label;
            const ImVec2 label_sz = ImGui::CalcTextSize(label);
            const float pad_x = ui_px(14.0f);
            const float tab_h = ui_px(34.0f);
            const ImVec2 tab_size(label_sz.x + pad_x * 2.0f, tab_h);
            const float content_right = ImGui::GetCursorScreenPos().x +
                                        ImGui::GetContentRegionAvail().x;
            if (i > 0) {
                const float next_x = ImGui::GetCursorScreenPos().x + tab_gap + tab_size.x;
                if (next_x > content_right) {
                    ImGui::NewLine();
                    ImGui::Spacing();
                } else {
                    ImGui::SameLine(0, tab_gap);
                }
            }
            const ImVec2 tab_min = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            // Track pill
            dl->AddRectFilled(tab_min, tab_min + tab_size,
                              c32(active ? k.surface2 : ImVec4(0, 0, 0, 0)),
                              ui_px(8.0f));
            if (active)
                dl->AddRect(tab_min, tab_min + tab_size, c32(k.brand), ui_px(8.0f),
                            0, ui_px(1.5f));
            // Hot dot for unread invites / notifications
            if (ess_tabs[i].hot) {
                dl->AddCircleFilled(
                    tab_min + ImVec2(label_sz.x + pad_x - ui_px(3.0f), ui_px(6.0f)),
                    ui_px(3.0f), c32(k.brand));
            }
            ImGui::InvisibleButton((std::string("##ess_tab_") +
                                    std::to_string(ess_tabs[i].id)).c_str(), tab_size);
            const bool hovered = ImGui::IsItemHovered();
            if (hovered && !active)
                dl->AddRect(tab_min, tab_min + tab_size, c32(k.border), ui_px(8.0f),
                            0, ui_px(1.0f));
            if (ImGui::IsItemClicked()) ui.current_tab = ess_tabs[i].id;
            ImGui::PushFont(active ? f_bold : f_body);
            dl->AddText(tab_min + ImVec2(pad_x, (tab_h - ImGui::GetTextLineHeight()) * 0.5f),
                        c32(active ? k.text : k.muted), label);
            ImGui::PopFont();
            if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
    }

    ImGui::Spacing();

    // ── Main content ──────────────────────────────────────────────
    if (ui.current_tab == 0) {
        // Friends tab uses the premium 3-column control-center layout.
        draw_friends_tab(st);
    } else {
        switch (ui.current_tab) {
            case 1: draw_invites_tab(st);      break;
            case 2: draw_sessions_tab(st);     break;
            case 3: draw_notifications_tab(st); break;
            case 4: draw_social_messages(st);   break;
            case 5: draw_social_parties(st);    break;
            default: draw_friends_tab(st);      break;
        }
    }

    if (ui.host_dialog_open)     draw_host_dialog(st);
    if (ui.invite_dialog_open)   draw_invite_dialog(st);
    if (ui.join_dialog_open)     draw_join_dialog(st);
    if (ui.session_manager_open) draw_session_manager(st);
    draw_friend_profile_panel(st);
}

// ---------------------------------------------------------------------------
// Session Card — Centerpiece (Hosting)
// ---------------------------------------------------------------------------

static void draw_session_card_hosting(UiState& st) {
    auto& ui = get_essentials_ui_state();
    auto& wh = WorldHost::instance();
    auto session = wh.get_current_session();

    // The center card is reserved for a real host session.  In particular,
    // do not render an online-looking card when the shared session store says
    // there are no active sessions.
    if (!wh.is_hosting() || session.id.empty() || session.state == SessionState::Ended ||
        session.state == SessionState::Crashed) {
        card_begin("##essentials_no_active_session");
        ImGui::PushFont(f_h2);
        ImGui::TextColored(k.text, "No active session");
        ImGui::PopFont();
        ImGui::TextColored(k.muted,
                           "Host a world or join a friend's session to see live multiplayer activity here.");
        card_end();
        return;
    }

    card_begin("##essentials_hosting_status");

    // Status line
    ImGui::TextColored(k.green, "\xe2\x97\x8f");
    ImGui::SameLine();
    ImGui::TextColored(k.green, "WORLD ONLINE");

    ImGui::Spacing();

    // World name (large)
    ImGui::PushFont(f_title);
    ImGui::TextColored(k.text, "%s", session.world_name.c_str());
    ImGui::PopFont();

    // Game mode + version + loader
    ImGui::TextColored(k.muted, "%s  \xe2\x80\xa2  MC %s  \xe2\x80\xa2  %s",
                       privacy_short_name(session.privacy),
                       session.minecraft_version.c_str(),
                       session.loader.c_str());

    ImGui::Spacing();

    // Player avatars row
    float avatar_r = ui_px(12.0f);
    float avatar_spacing = ui_px(4.0f);

    ImGui::TextColored(k.muted, "Players:");
    ImGui::SameLine();

    int display_count = std::min((int)session.connected_players.size(), 6);
    for (int i = 0; i < display_count; ++i) {
        ImVec4 ac = avatar_color(session.connected_players[i]);
        draw_avatar_circle(
            std::string(1, name_initial(session.connected_players[i])).c_str(),
            ac, avatar_r);
        if (i < display_count - 1) ImGui::SameLine(0, avatar_spacing);
    }

    if ((int)session.connected_players.size() > 6) {
        ImGui::SameLine(0, avatar_spacing);
        ImGui::TextColored(k.muted, "+%d",
                           (int)session.connected_players.size() - 6);
    }

    // Player count badge
    ImGui::SameLine(ui_px(240.0f));
    ImVec4 badge_bg = k.green;
    badge_bg.w = 0.15f;
    ImVec2 badge_pos = ImGui::GetCursorScreenPos();
    std::string player_count_text = std::to_string(session.player_count) +
                                    " / " + std::to_string(session.player_limit) +
                                    " Players";
    ImVec2 badge_ts = ImGui::CalcTextSize(player_count_text.c_str());
    float badge_pad = ui_px(8.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(
        ImVec2(badge_pos.x - badge_pad, badge_pos.y - ui_px(2.0f)),
        ImVec2(badge_pos.x + badge_ts.x + badge_pad,
               badge_pos.y + badge_ts.y + ui_px(2.0f)),
        c32(badge_bg), ui_px(4.0f));
    dl->AddText(badge_pos, c32(k.green), player_count_text.c_str());
    ImGui::Dummy(ImVec2(badge_ts.x + badge_pad * 2, badge_ts.y + ui_px(4.0f)));

    ImGui::Spacing();

    // Session state
    ImGui::TextColored(session_state_color(session.state), "[%s]",
                       session_state_tag(session.state));

    ImGui::TextColored(k.muted, "Essentials Address");
    ImGui::SameLine();
    ImGui::TextColored(k.brand, "%s", session.address.c_str());
    if (ghost_button("COPY ADDRESS", ImVec2(ui_px(130.0f), ui_px(26.0f)))) {
        ImGui::SetClipboardText(session.address.c_str());
        push_notice(st, ui_model::NoticeLevel::Success, "Address Copied", session.address);
    }
    ImGui::TextColored(k.muted, "Share the address and join code with invited players:");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##essentials_join_code", &session.join_token,
                     ImGuiInputTextFlags_ReadOnly);

    ImGui::Spacing();

    // Action buttons
    float bw = ui_px(140.0f);
    float bh = ui_px(32.0f);

    if (primary_button("Invite Friends", ImVec2(bw, bh))) {
        ui.invite_dialog_open = true;
        ui.invite_session_id = session.id;
    }
    ImGui::SameLine();
    if (ghost_button("Manage Session", ImVec2(bw, bh))) {
        ui.session_manager_open = true;
        ui.manage_session_id = session.id;
    }
    ImGui::SameLine();
    const bool stopping_host = essentials_request_is_working(st, "host-stop");
    const bool host_action_busy = essentials_request_lane_busy(st);
    const bool retry_stop = essentials_request_failed(st, "host-stop");
    if (ghost_button(stopping_host ? "Stopping..." :
                     (retry_stop ? "Retry Stop" : "Stop Hosting"),
                     ImVec2(ui_px(120.0f), bh), host_action_busy)) {
        start_essentials_request(st, "host-stop", [] {
            AsyncUiRequestResult result;
            result.success = WorldHost::instance().stop_hosting();
            result.title = result.success ? "Session ended" : "Could not stop hosting";
            result.detail = result.success
                ? "World hosting has been stopped."
                : "The session remains active. You can retry safely.";
            return result;
        });
    }
    draw_essentials_request_feedback(st, "host-stop", "Stopping hosted world...");

    card_end();
}

// ---------------------------------------------------------------------------
// Friend Card
// ---------------------------------------------------------------------------

static void draw_invite_card(const EssentialsInvite& invite, bool received,
                             UiState& st) {
    auto& ui = get_essentials_ui_state();
    std::string card_id = "##invite_" + invite.id;
    card_begin(card_id.c_str());

    // Sender row with avatar
    ImVec4 ac = avatar_color(received ? invite.host_username
                                      : invite.target_user_id);
    float avatar_r = ui_px(14.0f);

    std::string sender_name = received ? invite.host_username
                                       : invite.target_user_id;
    draw_avatar_circle(
        std::string(1, name_initial(sender_name)).c_str(),
        ac, avatar_r);
    ImGui::SameLine(0, ui_px(10.0f));

    ImGui::BeginGroup();
    if (received) {
        if (f_bold) ImGui::PushFont(f_bold);
        ImGui::TextColored(k.brand, "%s", invite.host_username.c_str());
        if (f_bold) ImGui::PopFont();
    } else {
        if (f_bold) ImGui::PushFont(f_bold);
        ImGui::TextColored(k.brand, "%s", invite.target_user_id.c_str());
        if (f_bold) ImGui::PopFont();
    }
    ImGui::EndGroup();

    // World name (prominent)
    ImGui::Spacing();
    if (f_h2) ImGui::PushFont(f_h2);
    ImGui::TextColored(k.text, "%s", invite.world_name.c_str());
    if (f_h2) ImGui::PopFont();

    // Version + loader + mods as chips
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto meta_chip = [&](const char* text, const ImVec4& accent) {
        const ImVec2 bp = ImGui::GetCursorScreenPos();
        const ImVec2 sz = ImGui::CalcTextSize(text) + ImVec2(ui_px(12.0f), ui_px(6.0f));
        ImVec4 bg = accent; bg.w = 0.12f;
        dl->AddRectFilled(bp, bp + sz, c32(bg), ui_px(5.0f));
        dl->AddText(bp + ImVec2(ui_px(6.0f), ui_px(3.0f)), c32(accent), text);
        ImGui::Dummy(sz + ImVec2(0, ui_px(4.0f)));
    };
    std::string ver_chip = "MC " + invite.minecraft_version;
    meta_chip(ver_chip.c_str(), k.blue);
    if (!invite.loader.empty() && invite.loader != "vanilla") {
        ImGui::SameLine(0, ui_px(6.0f));
        meta_chip(invite.loader.c_str(), k.brand);
    }
    std::string mods_chip = std::to_string(invite.mod_count) + " mods";
    ImGui::SameLine(0, ui_px(6.0f));
    meta_chip(mods_chip.c_str(), k.green);

    // Time ago
    std::string elapsed = time_ago(invite.created_at);
    if (!elapsed.empty()) {
        ImGui::SameLine(0, ui_px(6.0f));
        meta_chip(elapsed.c_str(), k.muted);
    }

    ImGui::Spacing();

    // Action buttons
    if (invite.status == InviteStatus::Pending) {
        if (received) {
            const std::string accept_action = "invite-accept:" + invite.id;
            const std::string decline_action = "invite-decline:" + invite.id;
            const bool invite_lane_busy = essentials_request_lane_busy(st);
            const bool accepting = essentials_request_is_working(st, accept_action);
            const bool retry_accept = essentials_request_failed(st, accept_action);
            if (primary_button(accepting ? "JOINING..." :
                               (retry_accept ? "RETRY" : "JOIN"),
                               ImVec2(ui_px(90.0f), ui_px(28.0f)), accepting,
                               invite_lane_busy)) {
                const std::string invite_id = invite.id;
                const std::string world_name = invite.world_name;
                ++ui.join_intent_generation;
                if (ui.join_intent_generation == 0) ++ui.join_intent_generation;
                const uint64_t join_intent_generation = ui.join_intent_generation;
                start_essentials_request(st, accept_action,
                    [invite_id, world_name, join_intent_generation] {
                    EssentialsInvite accepted;
                    AsyncUiRequestResult result;
                    result.number_a = static_cast<int64_t>(join_intent_generation);
                    result.success = InviteManager::instance().accept_invite(invite_id, &accepted);
                    result.title = result.success ? "Invite accepted" : "Accept failed";
                    result.detail = result.success
                        ? "Preparing " + world_name + " for compatibility checks."
                        : "The invite is still pending. You can retry.";
                    if (result.success) {
                        result.payload_a = accepted.session_id;
                        result.payload_b = accepted.session_address;
                        result.payload_c = accepted.join_token;
                    }
                    return result;
                });
            }
            ImGui::SameLine();
            const bool declining = essentials_request_is_working(st, decline_action);
            const bool retry_decline = essentials_request_failed(st, decline_action);
            if (ghost_button(declining ? "DECLINING..." :
                             (retry_decline ? "RETRY DECLINE" : "DECLINE"),
                             ImVec2(ui_px(90.0f), ui_px(28.0f)), invite_lane_busy)) {
                const std::string invite_id = invite.id;
                start_essentials_request(st, decline_action, [invite_id] {
                    AsyncUiRequestResult result;
                    result.success = InviteManager::instance().decline_invite(invite_id);
                    result.title = result.success ? "Invite declined" : "Decline failed";
                    result.detail = result.success
                        ? "The host has been notified."
                        : "The invite is still pending. You can retry.";
                    return result;
                });
            }
            draw_essentials_request_feedback(st, accept_action, "Accepting invite...");
            draw_essentials_request_feedback(st, decline_action, "Declining invite...");
        } else {
            const std::string cancel_action = "invite-cancel:" + invite.id;
            const bool invite_lane_busy = essentials_request_lane_busy(st);
            const bool cancelling = essentials_request_is_working(st, cancel_action);
            const bool retry_cancel = essentials_request_failed(st, cancel_action);
            if (ghost_button(cancelling ? "CANCELLING..." :
                             (retry_cancel ? "RETRY" : "CANCEL"),
                             ImVec2(ui_px(90.0f), ui_px(28.0f)), invite_lane_busy)) {
                const std::string invite_id = invite.id;
                start_essentials_request(st, cancel_action, [invite_id] {
                    AsyncUiRequestResult result;
                    result.success = InviteManager::instance().cancel_invite(invite_id);
                    result.title = result.success ? "Invite cancelled" : "Cancel failed";
                    result.detail = result.success
                        ? "The invitation is no longer active."
                        : "The invitation was not cancelled. You can retry.";
                    return result;
                });
            }
            draw_essentials_request_feedback(st, cancel_action, "Cancelling invite...");
        }
    } else {
        const char* status_text = "Unknown";
        ImVec4 status_col = k.muted;
        switch (invite.status) {
            case InviteStatus::Accepted:
                status_text = "\xe2\x9c\x93 Accepted";
                status_col = k.green;
                break;
            case InviteStatus::Declined:
                status_text = "\xe2\x9c\x97 Declined";
                status_col = k.red;
                break;
            case InviteStatus::Expired:
                status_text = "\xe2\x8f\xb3 Expired";
                status_col = k.muted;
                break;
            case InviteStatus::Cancelled:
                status_text = "Cancelled";
                status_col = k.muted;
                break;
            default:
                break;
        }
        ImGui::TextColored(status_col, "%s", status_text);
    }

    card_end();
    ImGui::Spacing();
}

// ---------------------------------------------------------------------------
// Compact friend row (for the left column list)
// ---------------------------------------------------------------------------

static void draw_friend_row(const EssentialsFriend& fri, UiState& st) {
    auto& ui = get_essentials_ui_state();
    ImGui::PushID(fri.user_id.c_str());

    card_begin("##friend_row", ImVec2(-1, 0));

    // Avatar
    ImVec4 ac = avatar_color(fri.username);
    draw_avatar_circle(
        std::string(1, name_initial(fri.display_name.empty()
                                        ? fri.username
                                        : fri.display_name))
            .c_str(),
        ac, ui_px(13.0f));
    ImGui::SameLine(0, ui_px(8.0f));

    // Name + status column
    ImGui::BeginGroup();
    ImGui::PushFont(f_bold);
    ImGui::TextColored(k.text, "%s",
        fri.display_name.empty() ? fri.username.c_str() : fri.display_name.c_str());
    ImGui::PopFont();

    ImVec4 sc = status_color(fri.status);
    if (fri.status != FriendStatus::Offline) {
        if (!fri.current_profile_name.empty()) {
            ImGui::TextColored(k.muted, "%s", fri.current_profile_name.c_str());
            if (!fri.current_game_version.empty()) {
                ImGui::SameLine();
                ImGui::TextColored(k.muted, "\xe2\x80\xa2 %s", fri.current_game_version.c_str());
            }
        } else {
            ImGui::TextColored(sc, "\xe2\x97\x8f %s",
                fri.status_message.empty() ? friend_status_name(fri.status)
                                           : fri.status_message.c_str());
        }
    } else {
        std::string last = fri.last_seen > 0 ? time_ago(fri.last_seen) : "";
        ImGui::TextColored(k.muted, "Offline%s", last.empty() ? "" : (" \xe2\x80\xa2 " + last).c_str());
    }
    ImGui::EndGroup();

    // Quick action + context menu on the right
    float btn_w = ui_px(56.0f);
    float btn_h = ui_px(24.0f);
    float row_right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    ImGui::SameLine(row_right - btn_w * 2 - ui_px(6.0f));

    if (fri.status != FriendStatus::Offline) {
        if (primary_button("Join", ImVec2(btn_w, btn_h))) {
            auto& sm = SessionManager::instance();
            auto sid = sm.get_current_session_id();
            if (!sid.empty()) {
                ui.invite_session_id = sid;
                ui.invite_dialog_open = true;
            } else {
                push_notice(st, ui_model::NoticeLevel::Warning,
                            "No Active Session",
                            "Host a world first before inviting friends.");
            }
        }
        ImGui::SameLine(0, ui_px(6.0f));
    } else {
        btn_w = ui_px(112.0f);
        ImGui::SameLine(row_right - btn_w - ui_px(2.0f));
    }

    if (ghost_button("...", ImVec2(btn_w, btn_h))) {
        ImGui::OpenPopup("##friend_row_ctx");
    }

    // Context menu
    if (ImGui::BeginPopup("##friend_row_ctx")) {
        if (ImGui::MenuItem("View Profile")) {
            ui.selected_friend_id = fri.user_id;
            ui.friend_profile_panel_open = true;
        }
        if (fri.status != FriendStatus::Offline &&
            ImGui::MenuItem("Message")) {
            ui.current_tab = 4;
        }
        ImGui::Separator();
        const std::string remove_action = "friend-remove:" + fri.user_id;
        const std::string block_action = "friend-block:" + fri.user_id;
        const bool action_lane_busy = essentials_request_lane_busy(st);
        if (ImGui::MenuItem("Remove Friend", nullptr, false, !action_lane_busy)) {
            const std::string user_id = fri.user_id;
            const std::string display_name = fri.display_name.empty()
                ? fri.username : fri.display_name;
            start_essentials_request(st, remove_action, [user_id, display_name] {
                AsyncUiRequestResult result;
                result.success = FriendsManager::instance().remove_friend(user_id);
                result.title = result.success ? "Friend removed" : "Could not remove friend";
                result.detail = result.success
                    ? display_name + " has been removed."
                    : "The friend relationship was not changed. You can retry safely.";
                return result;
            });
        }
        if (ImGui::MenuItem("Block User", nullptr, false, !action_lane_busy)) {
            const std::string user_id = fri.user_id;
            const std::string display_name = fri.display_name.empty()
                ? fri.username : fri.display_name;
            start_essentials_request(st, block_action, [user_id, display_name] {
                AsyncUiRequestResult result;
                result.success = FriendsManager::instance().block_user(user_id);
                result.title = result.success ? "User blocked" : "Could not block user";
                result.detail = result.success
                    ? display_name + " has been blocked."
                    : "The user was not blocked. You can retry safely.";
                return result;
            });
        }
        draw_essentials_request_feedback(st, remove_action, "Removing friend...");
        draw_essentials_request_feedback(st, block_action, "Blocking user...");
        ImGui::EndPopup();
    }

    // Click on the row opens the profile panel
    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }

    card_end();
    ImGui::PopID();
}

// ---------------------------------------------------------------------------
// Friends Tab — premium 3-column control center
//
//   LEFT  28% : friends list (search, online / offline groups)
//   CENTER 44%: active session + recent activity + invites preview
//   RIGHT  28%: your status + quick actions + connection + relay info
// ---------------------------------------------------------------------------

static void draw_friends_tab(UiState& st) {
    auto& ui = get_essentials_ui_state();
    auto& fm = FriendsManager::instance();
    auto friends = fm.get_friends();

    const float full = ImGui::GetContentRegionAvail().x;
    const float gap = ui_px(12.0f);
    const bool stacked_columns = ui_model::essentials_uses_stacked_columns(full, g_ui_scale);
    const float left_w = stacked_columns ? full : full * 0.28f;
    const float center_w = stacked_columns ? full : full * 0.44f;
    const float right_w = stacked_columns ? full : full - left_w - center_w - gap * 2.0f;
    (void)right_w; // reserved for future column layout

    // ── Helper: draw the friends list content (search, groups, rows) ──
    auto draw_friends_list_content = [&]() {
        // Friends header + search
        ImGui::PushFont(f_h2);
        ImGui::TextColored(k.text, "Friends");
        ImGui::PopFont();
        ImGui::SameLine(0, ui_px(8.0f));
        ImGui::TextColored(k.muted, "%d", (int)friends.size());
        ImGui::Spacing();

        input_text_hint("##friend_search", "Search friends...", &ui.friend_search);
        ImGui::Spacing();

        if (primary_button("+ Add Friend", ImVec2(-1, ui_px(30.0f)))) {
            ui.show_add_friend = !ui.show_add_friend;
        }
        if (ui.show_add_friend) {
            ImGui::Spacing();
            ImGui::TextColored(k.muted, "Friend code (AMG-XXXX-XXXX):");
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##add_friend_input", &ui.add_friend_input);
            const bool sending_request = essentials_request_is_working(st, "friend-request");
            const bool request_lane_busy = essentials_request_lane_busy(st);
            const bool retry_request = essentials_request_failed(st, "friend-request");
            if (ghost_button(sending_request ? "Sending..." :
                             (retry_request ? "Retry Request" : "Send Request"),
                             ImVec2(-1, ui_px(26.0f)), request_lane_busy)) {
                if (!ui.add_friend_input.empty()) {
                    const std::string friend_code = ui.add_friend_input;
                    start_essentials_request(st, "friend-request", [friend_code] {
                        AsyncUiRequestResult result;
                        result.success = FriendsManager::instance().send_request(friend_code);
                        result.title = result.success ? "Friend request sent" : "Request failed";
                        result.detail = result.success
                            ? "Waiting for the other player to accept."
                            : "The request was not sent. Check the code and retry.";
                        return result;
                    });
                }
            }
            draw_essentials_request_feedback(st, "friend-request", "Sending friend request...");
        }

        // Pending requests
        auto requests = fm.get_pending_requests();
        if (!requests.empty()) {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::TextColored(k.yellow, "Pending Requests (%d)", (int)requests.size());
            ImGui::Spacing();
            for (auto& req : requests) {
                ImGui::PushID(req.id.c_str());
                card_begin("##req_row", ImVec2(-1, 0));
                ImGui::TextColored(k.text, "%s", req.sender_username.c_str());
                const std::string accept_action = "friend-accept:" + req.id;
                const std::string decline_action = "friend-decline:" + req.id;
                const bool request_action_busy = essentials_request_lane_busy(st);
                const bool accepting = essentials_request_is_working(st, accept_action);
                if (ghost_button(accepting ? "Accepting..." : "Accept",
                                 ImVec2(ui_px(70.0f), ui_px(24.0f)), request_action_busy)) {
                    const std::string request_id = req.id;
                    const std::string username = req.sender_username;
                    start_essentials_request(st, accept_action, [request_id, username] {
                        AsyncUiRequestResult result;
                        result.success = FriendsManager::instance().accept_request(request_id);
                        result.title = result.success ? "Request accepted" : "Accept failed";
                        result.detail = result.success
                            ? username + " is now your friend."
                            : "The friend request is still pending. You can retry.";
                        return result;
                    });
                }
                ImGui::SameLine();
                const bool declining = essentials_request_is_working(st, decline_action);
                if (ghost_button(declining ? "Declining..." : "Decline",
                                 ImVec2(ui_px(70.0f), ui_px(24.0f)), request_action_busy)) {
                    const std::string request_id = req.id;
                    start_essentials_request(st, decline_action, [request_id] {
                        AsyncUiRequestResult result;
                        result.success = FriendsManager::instance().decline_request(request_id);
                        result.title = result.success ? "Request declined" : "Decline failed";
                        result.detail = result.success
                            ? "The friend request was declined."
                            : "The friend request is still pending. You can retry.";
                        return result;
                    });
                }
                draw_essentials_request_feedback(st, accept_action, "Accepting friend request...");
                draw_essentials_request_feedback(st, decline_action, "Declining friend request...");
                card_end();
                ImGui::PopID();
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // ── Online group ─────────────────────────────────────────────
        std::string lower_search = ui.friend_search;
        auto to_lower = [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        };
        std::transform(lower_search.begin(), lower_search.end(),
                       lower_search.begin(), to_lower);

        auto matches = [&](const EssentialsFriend& f) {
            if (lower_search.empty()) return true;
            std::string n = f.username, d = f.display_name;
            std::transform(n.begin(), n.end(), n.begin(), to_lower);
            std::transform(d.begin(), d.end(), d.begin(), to_lower);
            return n.find(lower_search) != std::string::npos ||
                   d.find(lower_search) != std::string::npos;
        };

        int online = 0, offline = 0;
        for (auto& f : friends) {
            if (!matches(f)) continue;
            if (f.status != FriendStatus::Offline) ++online;
            else ++offline;
        }

        if (online > 0) {
            ImGui::PushFont(f_small);
            ImGui::TextColored(k.green, "ONLINE \xe2\x80\x94 %d", online);
            ImGui::PopFont();
            ImGui::Spacing();
            for (auto& fri : friends) {
                if (!matches(fri) || fri.status == FriendStatus::Offline) continue;
                draw_friend_row(fri, st);
                ImGui::Spacing();
            }
        }

        if (offline > 0) {
            ImGui::Spacing();
            ImGui::PushFont(f_small);
            ImGui::TextColored(k.muted, "OFFLINE \xe2\x80\x94 %d", offline);
            ImGui::PopFont();
            ImGui::Spacing();
            for (auto& fri : friends) {
                if (!matches(fri) || fri.status != FriendStatus::Offline) continue;
                draw_friend_row(fri, st);
                ImGui::Spacing();
            }
        }

        if (online == 0 && offline == 0) {
            empty_state("No Results", "No friends match your search.");
        }
    };

    // ── Helper: draw the right-rail content (status, actions, connection) ──
    auto draw_right_rail = [&]() {
        draw_your_status(st);
        ImGui::Spacing();
        draw_quick_actions(st);
        ImGui::Spacing();
        draw_connection_card(st);
        ImGui::Spacing();
        draw_essentials_info_card(st);
    };

    // ── Helper: draw the center content (session, activity, invites) ──
    auto draw_center_content = [&]() {
        draw_session_card_hosting(st);
        ImGui::Spacing();
        draw_activity_feed(st);
        ImGui::Spacing();

        // Invites preview
        auto& im = InviteManager::instance();
        auto received = im.get_received_invites();
        int pending_received = 0;
        for (auto& inv : received)
            if (inv.status == InviteStatus::Pending) ++pending_received;

        card_begin("##invites_preview", ImVec2(-1, 0));
        ImGui::PushFont(f_h2);
        ImGui::TextColored(k.text, "Invites");
        ImGui::PopFont();
        ImGui::SameLine(0, ui_px(12.0f));
        if (pending_received > 0) {
            ImGui::TextColored(k.yellow, "(%d new)", pending_received);
        }
        ImGui::Spacing();

        if (pending_received == 0) {
            ImGui::TextColored(k.muted, "No pending invites.");
        } else {
            int shown_invites = 0;
            for (auto& inv : received) {
                if (inv.status != InviteStatus::Pending || shown_invites >= 2) continue;
                draw_invite_card(inv, true, st);
                ++shown_invites;
            }
            if (pending_received > 2) {
                ImGui::TextColored(k.muted, "+%d more in the Invites tab.", pending_received - 2);
            }
        }
        card_end();
    };

    // ══════════════════════════════════════════════════════════════
    // Empty state — all three sections still render for consistency
    // ══════════════════════════════════════════════════════════════
    // Columns must be width-scoped children: the section bodies use
    // fill-width widgets, so without a child the left column consumes the
    // whole row and SameLine pushes the other columns off-window.
    auto draw_column = [&](float width, const char* id, auto&& body) {
        if (stacked_columns) {
            ImGui::Spacing();
            body();
            return;
        }
        ImGui::BeginChild(id, ImVec2(width, 0),
                          ImGuiChildFlags_None | ImGuiChildFlags_AutoResizeY,
                          ImGuiWindowFlags_NoScrollbar);
        body();
        ImGui::EndChild();
        ImGui::SameLine(0, gap);
    };

    if (friends.empty()) {
        draw_column(stacked_columns ? 0.0f : left_w, "##ess_col_left",
                    [&] { draw_essentials_empty_state(st); });
        draw_column(stacked_columns ? 0.0f : center_w, "##ess_col_center", draw_center_content);
        draw_column(stacked_columns ? 0.0f : right_w, "##ess_col_right", draw_right_rail);
        if (!stacked_columns) ImGui::NewLine();
        return;
    }

    // ══════════════════════════════════════════════════════════════
    // LEFT — Friends list
    // ══════════════════════════════════════════════════════════════
    draw_column(left_w, "##ess_col_left", draw_friends_list_content);

    // ══════════════════════════════════════════════════════════════
    // CENTER — Session / activity / invites
    // ══════════════════════════════════════════════════════════════
    draw_column(center_w, "##ess_col_center", draw_center_content);

    // ══════════════════════════════════════════════════════════════
    // RIGHT — Status / quick actions / connection / info
    // ══════════════════════════════════════════════════════════════
    draw_column(right_w, "##ess_col_right", draw_right_rail);
    if (!stacked_columns) ImGui::NewLine();
}

// ---------------------------------------------------------------------------
// Invites Tab
// ---------------------------------------------------------------------------

static void draw_invites_tab(UiState& st) {
    auto& im = InviteManager::instance();
    auto received = im.get_received_invites();
    auto sent = im.get_sent_invites();

    ImGui::PushFont(f_bold);
    ImGui::TextColored(k.brand, "RECEIVED INVITES");
    ImGui::PopFont();
    ImGui::SameLine();
    if (!received.empty()) {
        ImGui::TextColored(k.muted, "(%d)", (int)received.size());
    }
    ImGui::Spacing();

    if (received.empty()) {
        card_begin("##no_received", ImVec2(-1, ui_px(60.0f)));
        empty_state("No Invites",
                    "When someone invites you to their world, it will appear here.");
        card_end();
    } else {
        for (auto& inv : received) {
            draw_invite_card(inv, true, st);
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::PushFont(f_bold);
    ImGui::TextColored(k.brand, "SENT INVITES");
    ImGui::PopFont();
    ImGui::SameLine();
    if (!sent.empty()) {
        ImGui::TextColored(k.muted, "(%d)", (int)sent.size());
    }
    ImGui::Spacing();

    if (sent.empty()) {
        card_begin("##no_sent", ImVec2(-1, ui_px(60.0f)));
        empty_state("No Sent Invites",
                    "Invite friends to your world to see them here.");
        card_end();
    } else {
        for (auto& inv : sent) {
            draw_invite_card(inv, false, st);
        }
    }
}

// ---------------------------------------------------------------------------
// Sessions Tab
// ---------------------------------------------------------------------------

static void draw_sessions_tab(UiState&) {
    auto& ui = get_essentials_ui_state();
    auto& sm = SessionManager::instance();
    auto sessions = sm.get_active_sessions();

    // ── Join by code card ──────────────────────────────────────────
    card_begin("##join_by_code");
    ImGui::PushFont(f_bold);
    ImGui::TextColored(k.brand, "JOIN WITH ADDRESS");
    ImGui::PopFont();
    ImGui::TextColored(k.muted, "Use the Essentials address and join code shared by the host.");
    ImGui::Spacing();
    const float join_input_w = std::max(ui_px(220.0f),
                                        (ImGui::GetContentRegionAvail().x - ui_px(16.0f)) * 0.5f);
    ImGui::SetNextItemWidth(join_input_w);
    input_text_hint("##join_addr", "Address (e.g. myworld.amalgam-essentials)", &ui.join_address);
    ImGui::SameLine(0, ui_px(8.0f));
    ImGui::SetNextItemWidth(join_input_w);
    ImGui::InputText("##join_code_tok", &ui.join_token, ImGuiInputTextFlags_Password);
    ImGui::Spacing();
    if (primary_button("JOIN", ImVec2(ui_px(100.0f), ui_px(32.0f)), false,
                       ui.join_syncing) &&
        !ui.join_address.empty() && !ui.join_token.empty()) {
        open_fresh_join_dialog(ui);
    }
    card_end();
    ImGui::Spacing();

    // ── Active sessions section ────────────────────────────────────
    ImGui::PushFont(f_bold);
    ImGui::TextColored(k.brand, "ACTIVE SESSIONS");
    ImGui::PopFont();
    if (!sessions.empty()) {
        ImGui::SameLine(0, ui_px(8.0f));
        ImGui::TextColored(k.muted, "(%d)", (int)sessions.size());
    }
    ImGui::Spacing();

    if (sessions.empty()) {
        card_begin("##no_sessions", ImVec2(-1, ui_px(100.0f)));
        empty_state("No Active Sessions",
                    "No one in your friends list is currently hosting a session.");
        card_end();

        ImGui::Spacing();
        ImGui::Spacing();

        ImGui::SetCursorPosX(
            ImGui::GetCursorPosX() +
            (ImGui::GetContentRegionAvail().x - ui_px(200.0f)) * 0.5f);
        if (primary_button("HOST WORLD", ImVec2(ui_px(200.0f), ui_px(36.0f)))) {
            ui.host_dialog_open = true;
        }
        return;
    }

    for (auto& sess : sessions) {
        std::string card_id = "##session_" + sess.id;
        card_begin(card_id.c_str());

        // World name (large)
        if (f_h2) ImGui::PushFont(f_h2);
        ImGui::TextColored(k.text, "%s", sess.world_name.c_str());
        if (f_h2) ImGui::PopFont();

        // Address
        if (!sess.address.empty()) {
            ImGui::SameLine(0, ui_px(8.0f));
            ImGui::TextColored(k.brand, "%s", sess.address.c_str());
        }

        // State badge
        ImVec4 sc = session_state_color(sess.state);
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 bp = ImGui::GetCursorScreenPos();
            ImVec4 sbg = sc;
            sbg.w = 0.12f;
            draw_badge(dl, bp, session_state_tag(sess.state), sc, sbg);
            ImGui::Dummy(ImVec2(
                ImGui::CalcTextSize(session_state_tag(sess.state)).x + ui_px(14.0f),
                ui_px(20.0f)));
        }

        ImGui::Spacing();

        // Host + version + loader as stat chips
        ImDrawList* dl = ImGui::GetWindowDrawList();
        auto meta_chip = [&](const char* text, const ImVec4& accent) {
            const ImVec2 bp = ImGui::GetCursorScreenPos();
            const ImVec2 sz = ImGui::CalcTextSize(text) + ImVec2(ui_px(14.0f), ui_px(8.0f));
            ImVec4 bg = accent; bg.w = 0.12f;
            dl->AddRectFilled(bp, bp + sz, c32(bg), ui_px(6.0f));
            dl->AddText(bp + ImVec2(ui_px(7.0f), ui_px(4.0f)), c32(accent), text);
            ImGui::Dummy(sz + ImVec2(0, ui_px(4.0f)));
        };
        std::string host_chip = "Host: " + sess.host_username;
        meta_chip(host_chip.c_str(), k.text);
        ImGui::SameLine(0, ui_px(6.0f));
        std::string ver_chip = "MC " + sess.minecraft_version;
        meta_chip(ver_chip.c_str(), k.blue);
        if (!sess.loader.empty() && sess.loader != "vanilla") {
            ImGui::SameLine(0, ui_px(6.0f));
            meta_chip(sess.loader.c_str(), k.brand);
        }
        ImGui::SameLine(0, ui_px(6.0f));
        std::string players_chip = std::to_string(sess.player_count) + " / " +
                                   std::to_string(sess.player_limit) + " players";
        meta_chip(players_chip.c_str(), sess.player_count >= sess.player_limit ? k.yellow : k.green);
        ImGui::SameLine(0, ui_px(6.0f));
        meta_chip(privacy_short_name(sess.privacy), k.muted);

        ImGui::Spacing();

        // Player avatars
        if (!sess.connected_players.empty()) {
            float avatar_r = ui_px(11.0f);
            int display_count = std::min((int)sess.connected_players.size(), 10);
            for (int i = 0; i < display_count; ++i) {
                ImVec4 ac = avatar_color(sess.connected_players[i]);
                draw_avatar_circle(
                    std::string(1, name_initial(sess.connected_players[i])).c_str(),
                    ac, avatar_r);
                if (i < display_count - 1) ImGui::SameLine(0, ui_px(3.0f));
            }
            if ((int)sess.connected_players.size() > 10) {
                ImGui::SameLine(0, ui_px(3.0f));
                ImGui::TextColored(k.muted, "+%d",
                    (int)sess.connected_players.size() - 10);
            }
        }

        ImGui::Spacing();

        // Action
        if (sess.state == SessionState::Online &&
            sess.player_count < sess.player_limit) {
            if (primary_button("Join",
                               ImVec2(ui_px(100.0f), ui_px(30.0f)), false,
                               ui.join_syncing)) {
                ui.join_session_id = sess.id;
                ui.join_address = sess.address;
                ui.join_token.clear();
                open_fresh_join_dialog(ui);
            }
        } else if (sess.player_count >= sess.player_limit) {
            ImGui::TextColored(k.red, "Session Full");
        } else {
            ImGui::TextColored(k.muted, "Not Available");
        }

        card_end();
        ImGui::Spacing();
    }
}

// ---------------------------------------------------------------------------
// Notifications Tab
// ---------------------------------------------------------------------------

static void draw_notifications_tab(UiState& st) {
    auto& sm = SessionManager::instance();
    auto notifs = sm.get_notifications();

    if (notifs.empty()) {
        card_begin("##no_notifs", ImVec2(-1, ui_px(80.0f)));
        empty_state("No Notifications",
                    "Session activity, invites, and updates will appear here.");
        card_end();
        return;
    }

    ImGui::PushFont(f_bold);
    ImGui::TextColored(k.brand, "NOTIFICATIONS");
    ImGui::PopFont();
    ImGui::SameLine(0, ui_px(8.0f));
    ImGui::TextColored(k.muted, "(%d)", (int)notifs.size());
    ImGui::Spacing();

    for (auto& notif : notifs) {
        std::string card_id = "##notif_" + notif.id;
        card_begin(card_id.c_str());

        // Unread indicator
        if (!notif.read) {
            ImVec4 dot_color = k.brand;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 p = ImGui::GetCursorScreenPos();
            dl->AddCircleFilled(ImVec2(p.x + ui_px(4.0f),
                                        p.y + ImGui::GetTextLineHeight() * 0.5f),
                                ui_px(4.0f), c32(dot_color));
            ImGui::Dummy(ImVec2(ui_px(12.0f), 0));
            ImGui::SameLine();
        }

        // Title
        if (f_bold) ImGui::PushFont(f_bold);
        ImGui::TextUnformatted(notif.title.c_str());
        if (f_bold) ImGui::PopFont();

        if (!notif.body.empty()) {
            ImGui::TextColored(k.muted, "%s", notif.body.c_str());
        }

        if (!notif.from_username.empty()) {
            ImGui::TextColored(k.muted, "From: %s",
                               notif.from_username.c_str());
        }

        ImGui::Spacing();

        // Action buttons
        if (notif.on_accept) {
            const std::string accept_action = "notification-accept:" + notif.id;
            const bool accepting = essentials_request_is_working(st, accept_action);
            if (primary_button(accepting ? "Accepting..." : "Accept",
                               ImVec2(ui_px(80.0f), ui_px(26.0f)), accepting,
                               essentials_request_lane_busy(st))) {
                const auto callback = notif.on_accept;
                const std::string notification_id = notif.id;
                start_essentials_request(st, accept_action, [callback, notification_id] {
                    callback();
                    SessionManager::instance().clear_notification(notification_id);
                    AsyncUiRequestResult result;
                    result.success = true;
                    result.title = "Action accepted";
                    result.detail = "The notification was handled.";
                    return result;
                });
            }
            ImGui::SameLine();
            draw_essentials_request_feedback(st, accept_action, "Accepting notification action...");
        }
        if (notif.on_decline) {
            const std::string decline_action = "notification-decline:" + notif.id;
            const bool declining = essentials_request_is_working(st, decline_action);
            if (ghost_button(declining ? "Declining..." : "Decline",
                             ImVec2(ui_px(80.0f), ui_px(26.0f)),
                             essentials_request_lane_busy(st))) {
                const auto callback = notif.on_decline;
                const std::string notification_id = notif.id;
                start_essentials_request(st, decline_action, [callback, notification_id] {
                    callback();
                    SessionManager::instance().clear_notification(notification_id);
                    AsyncUiRequestResult result;
                    result.success = true;
                    result.title = "Action declined";
                    result.detail = "The notification was handled.";
                    return result;
                });
            }
            ImGui::SameLine();
            draw_essentials_request_feedback(st, decline_action, "Declining notification action...");
        }

        if (ghost_button("Dismiss",
                         ImVec2(ui_px(80.0f), ui_px(26.0f)))) {
            sm.clear_notification(notif.id);
        }

        card_end();
        ImGui::Spacing();
    }
}

// ---------------------------------------------------------------------------
// Host World Dialog
// ---------------------------------------------------------------------------

static void draw_host_dialog(UiState& st) {
    auto& ui = get_essentials_ui_state();

    ImGui::SetNextWindowSize(ImVec2(ui_px(460.0f), ui_px(420.0f)),
                             ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Host World", &ui.host_dialog_open,
                               ImGuiWindowFlags_NoResize)) {

        ImGui::TextColored(k.brand, "HOST A WORLD");
        ImGui::Separator();
        ImGui::Spacing();

        // World Name
        ImGui::Text("World Name");
        ImGui::SetNextItemWidth(ui_px(380.0f));
        ImGui::InputText("##host_world_name", &ui.host_world_name);

        ImGui::Spacing();

        ImGui::Text("Essentials Address");
        ImGui::SetNextItemWidth(ui_px(300.0f));
        ImGui::InputTextWithHint("##host_alias", "forsaken", &ui.host_alias);
        std::string preview_alias = ui.host_alias.empty()
            ? EssentialsAddressResolver::NormalizeAlias(ui.host_world_name)
            : EssentialsAddressResolver::NormalizeAlias(ui.host_alias);
        if (!EssentialsAddressResolver::IsValidAlias(preview_alias)) preview_alias = "world-xxxx";
        ImGui::SameLine();
        ImGui::TextColored(k.muted, "%s", (preview_alias + ".amalgam-essentials").c_str());
        ImGui::TextColored(k.muted, "3-32 lowercase letters, numbers, and hyphens");

        ImGui::Spacing();

        // Privacy selector
        ImGui::Text("Privacy");
        ImGui::SetNextItemWidth(ui_px(380.0f));
        const char* priv_current = privacy_short_name(ui.host_privacy);
        if (ImGui::BeginCombo("##host_privacy", priv_current)) {
            for (int i = 0; i < 4; ++i) {
                auto p = static_cast<SessionPrivacy>(i);
                bool selected = (ui.host_privacy == p);
                if (ImGui::Selectable(privacy_short_name(p), selected)) {
                    ui.host_privacy = p;
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        ImGui::Spacing();

        // Player limit slider with label
        ImGui::Text("Max Players: %d", ui.host_player_limit);
        ImGui::SetNextItemWidth(ui_px(380.0f));
        ImGui::SliderInt("##host_player_limit", &ui.host_player_limit,
                         2, 32);

        ImGui::Spacing();

        // Description textarea
        ImGui::Text("Description (optional)");
        ImGui::SetNextItemWidth(ui_px(380.0f));
        ImGui::InputTextMultiline("##host_description",
                                  &ui.host_description,
                                  ImVec2(ui_px(380.0f), ui_px(60.0f)));

        ImGui::Spacing();
        ImGui::Spacing();

        // Action buttons
        float bw = ui_px(180.0f);
        float bh = ui_px(34.0f);

        const bool starting_host = essentials_request_is_working(st, "host-start");
        const bool host_lane_busy = essentials_request_lane_busy(st);
        const bool retry_host = essentials_request_failed(st, "host-start");
        if (primary_button(starting_host ? "STARTING..." :
                           (retry_host ? "RETRY HOSTING" : "START HOSTING"),
                           ImVec2(bw, bh), starting_host, host_lane_busy)) {
            if (ui.host_world_name.empty()) {
                push_notice(st, ui_model::NoticeLevel::Error,
                            "World Name Required",
                            "Please enter a world name.");
            } else {
                HostOptions opts;
                opts.profile_id = ui.host_profile_id;
                opts.world_name = ui.host_world_name;
                opts.alias = ui.host_alias.empty()
                    ? EssentialsAddressResolver::Generate(ui.host_world_name)
                    : EssentialsAddressResolver::NormalizeAlias(ui.host_alias);
                opts.privacy = ui.host_privacy;
                opts.player_limit = ui.host_player_limit;

                if (!EssentialsAddressResolver::IsValidAlias(opts.alias)) {
                    push_notice(st, ui_model::NoticeLevel::Error,
                                "Invalid Address", "Choose a valid non-reserved Essentials address.");
                } else {
                    start_essentials_request(st, "host-start", [opts] {
                        AsyncUiRequestResult result;
                        EssentialsAddressResolver resolver;
                        if (!resolver.IsAvailable(opts.alias)) {
                            const auto suggestions = EssentialsAddressResolver::Suggestions(opts.alias);
                            result.title = "Address unavailable";
                            result.detail = "That address is already active. Try " +
                                suggestions[0] + " or " + suggestions[1] + ".";
                            return result;
                        }
                        result.success = WorldHost::instance().start_hosting(
                            opts, [](float, const std::string&) {});
                        result.title = result.success ? "Hosting started" : "Hosting failed";
                        result.detail = result.success
                            ? "Your world is online at " +
                                EssentialsAddressResolver::ToAddress(opts.alias)
                            : "The local world or Essentials transport could not be started. You can retry.";
                        return result;
                    });
                }
            }
        }

        ImGui::SameLine();

        if (ghost_button("CANCEL", ImVec2(bw, bh), host_lane_busy)) {
            ui.host_dialog_open = false;
        }
        draw_essentials_request_feedback(st, "host-start", "Creating session and starting host...");

        ImGui::EndPopup();
    }
    if (ui.host_dialog_open) {
        ImGui::OpenPopup("Host World");
    }
}

// ---------------------------------------------------------------------------
// Invite Dialog
// ---------------------------------------------------------------------------

static void draw_invite_dialog(UiState& st) {
    auto& ui = get_essentials_ui_state();

    ImGui::SetNextWindowSize(ImVec2(ui_px(400.0f), ui_px(460.0f)),
                             ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Invite Friends", &ui.invite_dialog_open,
                               ImGuiWindowFlags_NoResize)) {

        auto& fm = FriendsManager::instance();
        auto friends = fm.get_friends();

        // Search bar
        ImGui::TextColored(k.brand, "SELECT FRIENDS TO INVITE");
        ImGui::Spacing();

        input_text_hint("##invite_search", "Search friends...",
                        &ui.invite_search);

        // Online filter toggle
        ImGui::SameLine();
        if (ghost_button(ui.invite_show_online_only ? "All" : "Online",
                         ImVec2(ui_px(52.0f), ui_px(24.0f)))) {
            ui.invite_show_online_only = !ui.invite_show_online_only;
        }

        ImGui::Spacing();

        // Select all / Deselect all
        float small_btn = ui_px(90.0f);
        float small_h = ui_px(22.0f);
        if (ghost_button("Select All", ImVec2(small_btn, small_h))) {
            ui.selected_invitees.clear();
            for (auto& fri : friends) {
                if (fri.status != FriendStatus::Offline) {
                    ui.selected_invitees.push_back(fri.user_id);
                }
            }
        }
        ImGui::SameLine();
        if (ghost_button("Deselect All", ImVec2(small_btn, small_h))) {
            ui.selected_invitees.clear();
        }

        ImGui::Spacing();

        // Friend list
        ImGui::BeginChild("##invite_friend_list",
                          ImVec2(ui_px(360.0f), ui_px(250.0f)));

        for (auto& fri : friends) {
            // Filter by search
            std::string lower_search = ui.invite_search;
            auto to_lower_fn = [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            };
            std::transform(lower_search.begin(), lower_search.end(),
                           lower_search.begin(), to_lower_fn);

            if (!lower_search.empty()) {
                std::string name_lower = fri.username;
                std::transform(name_lower.begin(), name_lower.end(),
                               name_lower.begin(), to_lower_fn);
                std::string dn_lower = fri.display_name;
                std::transform(dn_lower.begin(), dn_lower.end(),
                               dn_lower.begin(), to_lower_fn);
                if (name_lower.find(lower_search) == std::string::npos &&
                    dn_lower.find(lower_search) == std::string::npos) {
                    continue;
                }
            }

            // Filter by online
            if (ui.invite_show_online_only &&
                fri.status == FriendStatus::Offline) {
                continue;
            }

            bool selected = false;
            for (auto& sid : ui.selected_invitees) {
                if (sid == fri.user_id) { selected = true; break; }
            }

            // Avatar + checkbox row
            ImVec4 ac = avatar_color(fri.username);
            draw_avatar_circle(
                std::string(1, name_initial(fri.display_name.empty()
                                                ? fri.username
                                                : fri.display_name))
                    .c_str(),
                ac, ui_px(10.0f));
            ImGui::SameLine(0, ui_px(6.0f));

            if (ImGui::Checkbox(
                    (fri.display_name.empty() ? fri.username
                                              : fri.display_name)
                        .c_str(),
                    &selected)) {
                if (selected) {
                    ui.selected_invitees.push_back(fri.user_id);
                } else {
                    ui.selected_invitees.erase(
                        std::remove(ui.selected_invitees.begin(),
                                    ui.selected_invitees.end(),
                                    fri.user_id),
                        ui.selected_invitees.end());
                }
            }

            ImGui::SameLine();
            ImGui::TextColored(status_color(fri.status), "\xe2\x97\x8f");
            ImGui::SameLine();
            ImGui::TextColored(k.muted, "%s",
                               friend_status_name(fri.status));
        }

        ImGui::EndChild();

        ImGui::Spacing();

        // Bottom bar with count badge + buttons
        float bw = ui_px(160.0f);
        float bh = ui_px(30.0f);

        // Send Invites button with count badge
        std::string send_label = "SEND INVITES";
        if (!ui.selected_invitees.empty()) {
            send_label += " (" + std::to_string(ui.selected_invitees.size()) + ")";
        }

        const bool sending_invites = essentials_request_is_working(st, "invite-send");
        const bool invite_lane_busy = essentials_request_lane_busy(st);
        const bool retry_invites = essentials_request_failed(st, "invite-send");
        if (primary_button(sending_invites ? "SENDING..." :
                           (retry_invites ? "RETRY INVITES" : send_label.c_str()),
                           ImVec2(bw, bh), sending_invites, invite_lane_busy)) {
            if (ui.selected_invitees.empty()) {
                push_notice(st, ui_model::NoticeLevel::Warning,
                            "No Friends Selected",
                            "Select at least one friend to invite.");
            } else {
                auto& sm = SessionManager::instance();
                auto session = sm.get_session(ui.invite_session_id);
                const auto invitees = ui.selected_invitees;
                start_essentials_request(st, "invite-send", [invitees, session] {
                    int sent = 0;
                    int failed = 0;
                    for (const auto& user_id : invitees) {
                        if (InviteManager::instance().send_invite(user_id, session)) ++sent;
                        else ++failed;
                    }
                    AsyncUiRequestResult result;
                    result.success = sent > 0;
                    result.warning = sent > 0 && failed > 0;
                    result.title = result.success
                        ? (result.warning ? "Some invites could not be sent" : "Invites sent")
                        : "Could not send invites";
                    result.detail = result.success
                        ? std::to_string(sent) + " invite(s) sent" +
                            (failed > 0 ? "; " + std::to_string(failed) + " failed." : ".")
                        : "No invitations were sent. Check the session and retry.";
                    return result;
                });
            }
        }

        ImGui::SameLine();

        if (ghost_button("CANCEL", ImVec2(bw, bh), invite_lane_busy)) {
            ui.selected_invitees.clear();
            ui.invite_dialog_open = false;
        }
        draw_essentials_request_feedback(st, "invite-send", "Sending invitations...");

        ImGui::EndPopup();
    }
    if (ui.invite_dialog_open) {
        ImGui::OpenPopup("Invite Friends");
    }
}

// ---------------------------------------------------------------------------
// Join Dialog
// ---------------------------------------------------------------------------

static void draw_join_dialog(UiState& st) {
    auto& ui = get_essentials_ui_state();

    set_next_adaptive_window(500.0f, 480.0f, 360.0f, 280.0f,
                             ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Join Session", &ui.join_dialog_open,
                               ImGuiWindowFlags_NoResize |
                               ImGuiWindowFlags_NoScrollbar |
                               ImGuiWindowFlags_NoScrollWithMouse)) {

        if (!ui.join_compat_shown) {
            const bool checking_join = essentials_request_is_working(st, "join-analyze");
            const bool join_lane_busy = essentials_request_lane_busy(st);
            const bool retry_check = essentials_request_failed(st, "join-analyze");
            ImGui::TextUnformatted("Essentials Address");
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::BeginDisabled(join_lane_busy);
            ImGui::InputText("##join_address", &ui.join_address);
            ImGui::TextUnformatted("Join code");
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputText("##join_token", &ui.join_token,
                             ImGuiInputTextFlags_Password);
            ImGui::EndDisabled();
            if (ui.join_address.empty() || ui.join_token.empty()) {
                ImGui::TextColored(k.muted, "Enter the address and host join code to continue.");
                ImGui::Spacing();
                if (ghost_button("Close", ImVec2(ui_px(100.0f), ui_px(30.0f))))
                    ui.join_dialog_open = false;
                ImGui::EndPopup();
                return;
            }
            ImGui::Spacing();
            if (checking_join) {
                ImGui::TextColored(k.muted,
                                   "Checking the session and local profile compatibility...");
                ImGui::Spacing();
                if (ghost_button("Close", ImVec2(ui_px(100.0f), ui_px(30.0f)))) {
                    // Closing only hides this dialog.  The result is generation
                    // guarded and will not reopen it after the user leaves.
                    ui.join_dialog_open = false;
                }
                ImGui::EndPopup();
                return;
            }
            if (retry_check) {
                draw_essentials_request_feedback(st, "join-analyze", "Checking compatibility...");
                ImGui::Spacing();
            } else {
                ImGui::TextColored(k.muted,
                                   "Check compatibility before syncing or launching Minecraft.");
            }
            if (primary_button(retry_check ? "Retry Check" : "Check Compatibility",
                               ImVec2(ui_px(180.0f), ui_px(32.0f)), false,
                               join_lane_busy)) {
                const std::string supplied_session_id = ui.join_session_id;
                const std::string supplied_address = ui.join_address;
                const std::string supplied_token = ui.join_token;
                const uint64_t dialog_generation = ui.join_dialog_generation;
                start_essentials_request(st, "join-analyze",
                    [supplied_session_id, supplied_address, supplied_token, dialog_generation] {
                        AsyncUiRequestResult result;
                        result.number_a = static_cast<int64_t>(dialog_generation);
                        std::string session_id = supplied_session_id;
                        std::string address = supplied_address;
                        if (session_id.empty()) {
                            EssentialsAddressResolver resolver;
                            const auto resolved = resolver.Resolve(address);
                            if (!resolved.success) {
                                result.title = "Could not find session";
                                result.detail = resolved.error.empty()
                                    ? "The address could not be resolved. You can retry."
                                    : resolved.error;
                                return result;
                            }
                            session_id = resolved.session_id;
                            address = resolved.address;
                        }
                        const auto analysis = JoinManager::instance().analyze_and_prepare(
                            session_id, supplied_token);
                        result.success = analysis.success;
                        result.title = analysis.success ? "" : "Compatibility check failed";
                        result.detail = analysis.success ? "" : analysis.error;
                        if (analysis.success) {
                            result.payload_a = session_id;
                            result.payload_b = address;
                        }
                        return result;
                    });
            }
            ImGui::SameLine();
            if (ghost_button("Close", ImVec2(ui_px(100.0f), ui_px(32.0f)))) {
                ui.join_dialog_open = false;
            }
            ImGui::EndPopup();
            return;
        }

        // The compatibility result can include several bounded lists and
        // progress detail.  Reserve a stable footer before laying it out so
        // Sync & Join / Cancel never follows an outer popup scroll range.
        const ImGuiStyle& style = ImGui::GetStyle();
        const float footer_height = std::max(
            ui_px(64.0f),
            std::max(ui_px(34.0f), ImGui::GetFrameHeight()) +
                3.0f * style.ItemSpacing.y + style.WindowPadding.y);
        const bool can_join = ui.join_compat.overall !=
                                  CompatibilityLevel::Incompatible &&
                              !ui.join_syncing;
        ImGui::BeginChild("##join_session_detail", ImVec2(0.0f, -footer_height),
                          ImGuiChildFlags_None);

        // Step indicators
        const char* step_labels[] = {"Check", "Sync", "Download", "Launch"};
        float step_w = ui_px(110.0f);
        float step_h = ui_px(28.0f);
        float total_steps_w = step_w * 4 + ui_px(6.0f) * 3;
        float start_x = ImGui::GetCursorPosX() +
                        (ImGui::GetContentRegionAvail().x - total_steps_w) * 0.5f;

        ImGui::SetCursorPosX(start_x);
        for (int i = 0; i < 4; ++i) {
            bool active = (i <= ui.join_step);
            bool current = (i == ui.join_step);

            ImVec4 step_bg = active ? k.brand : ImVec4(0.15f, 0.15f, 0.18f, 1.0f);
            ImVec4 step_text = active ? ImVec4(1,1,1,1) : k.muted;

            if (current) {
                step_bg = k.brand;
            }

            ImVec2 p = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p, ImVec2(p.x + step_w, p.y + step_h),
                              c32(step_bg), ui_px(6.0f));

            ImVec2 ts = ImGui::CalcTextSize(step_labels[i]);
            dl->AddText(
                ImVec2(p.x + (step_w - ts.x) * 0.5f,
                       p.y + (step_h - ts.y) * 0.5f),
                c32(step_text), step_labels[i]);

            ImGui::Dummy(ImVec2(step_w, step_h));
            if (i < 3) ImGui::SameLine(0, ui_px(6.0f));
        }

        ImGui::Spacing();
        ImGui::Spacing();

        // Progress bar
        if (ui.join_syncing) {
            float bar_w = ImGui::GetContentRegionAvail().x;
            float bar_h = ui_px(6.0f);
            ImVec2 bar_pos = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();

            dl->AddRectFilled(bar_pos,
                              ImVec2(bar_pos.x + bar_w, bar_pos.y + bar_h),
                              c32(k.surface), ui_px(3.0f));
            dl->AddRectFilled(bar_pos,
                              ImVec2(bar_pos.x + bar_w * ui.join_progress,
                                     bar_pos.y + bar_h),
                              c32(k.brand), ui_px(3.0f));

            ImGui::Dummy(ImVec2(bar_w, bar_h));
            ImGui::Spacing();
            ImGui::TextColored(k.muted, "%s", ui.join_status_text.empty()
                ? "Joining session..." : ui.join_status_text.c_str());
            ImGui::TextColored(k.muted,
                "You can close this window; the session task keeps running safely in the background.");
            ImGui::Spacing();
        }

        // Compatibility header
        ImGui::TextColored(k.brand, "COMPATIBILITY CHECK");
        ImGui::Spacing();

        // Compat results as rows
        auto draw_compat_row = [](const char* label,
                                  CompatibilityLevel level,
                                  const char* host_val,
                                  const char* local_val) {
            ImVec4 lc = compat_color(level);
            const char* icon = compat_icon(level);

            ImGui::Columns(3, nullptr, false);
            ImGui::SetColumnWidth(0, ui_px(120.0f));
            ImGui::SetColumnWidth(1, ui_px(200.0f));

            ImGui::TextUnformatted(label);
            ImGui::NextColumn();

            ImGui::TextColored(lc, "%s %s", icon,
                               compat_level_name(level));
            ImGui::NextColumn();

            if (host_val && local_val) {
                ImGui::TextColored(k.muted, "%s vs %s", host_val, local_val);
            }
            ImGui::Columns(1);
        };

        draw_compat_row("Minecraft Version",
                        ui.join_compat.version_level,
                        ui.join_compat.host_minecraft_version.c_str(),
                        ui.join_compat.local_minecraft_version.c_str());
        draw_compat_row("Loader",
                        ui.join_compat.loader_level,
                        ui.join_compat.host_loader.c_str(),
                        ui.join_compat.local_loader.c_str());
        draw_compat_row("Mods",
                        ui.join_compat.mods_level, nullptr, nullptr);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Missing / outdated / extra mods
        if (!ui.join_compat.missing_mods.empty()) {
            ImGui::TextColored(k.red, "Missing Mods (%d):",
                               (int)ui.join_compat.missing_mods.size());
            ImGui::BeginChild("##missing_mods",
                              ImVec2(ui_px(440.0f), ui_px(70.0f)));
            for (auto& m : ui.join_compat.missing_mods) {
                ImGui::TextColored(k.red, "  \xe2\x9c\x97 %s %s",
                                   m.name.c_str(), m.version.c_str());
            }
            ImGui::EndChild();
        }

        if (!ui.join_compat.outdated_mods.empty()) {
            ImGui::TextColored(k.yellow, "Outdated Mods (%d):",
                               (int)ui.join_compat.outdated_mods.size());
            ImGui::BeginChild("##outdated_mods",
                              ImVec2(ui_px(440.0f), ui_px(70.0f)));
            for (auto& m : ui.join_compat.outdated_mods) {
                ImGui::TextColored(k.yellow, "  \xe2\x9a\xa0 %s %s",
                                   m.name.c_str(), m.version.c_str());
            }
            ImGui::EndChild();
        }

        if (!ui.join_compat.extra_mods.empty()) {
            ImGui::TextColored(k.muted, "Extra Mods (%d):",
                               (int)ui.join_compat.extra_mods.size());
            ImGui::BeginChild("##extra_mods",
                              ImVec2(ui_px(440.0f), ui_px(60.0f)));
            for (auto& m : ui.join_compat.extra_mods) {
                ImGui::TextColored(k.muted, "  %s %s", m.name.c_str(),
                                   m.version.c_str());
            }
            ImGui::EndChild();
        }

        ImGui::Spacing();

        if (can_join) {
            if (!ui.join_sync_plan.mods_to_download.empty() ||
                !ui.join_sync_plan.mods_to_update.empty()) {
                ImGui::TextColored(k.muted,
                    "Sync: %d download(s), %d update(s), %s",
                    (int)ui.join_sync_plan.mods_to_download.size(),
                    (int)ui.join_sync_plan.mods_to_update.size(),
                    format_bytes(ui.join_sync_plan.total_download_size)
                        .c_str());
                ImGui::Spacing();
            }
        }

        draw_essentials_request_feedback(st, "join-execute", "Starting session join...");
        if (!can_join && ui.join_compat.overall ==
                             CompatibilityLevel::Incompatible) {
            ImGui::TextColored(k.red, "Session is incompatible. Cannot join.");
        }
        ImGui::EndChild();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        float bw = ui_px(160.0f);
        float bh = ui_px(34.0f);

        if (can_join) {
            const bool starting_join = essentials_request_is_working(st, "join-execute");
            const bool essentials_busy = essentials_request_lane_busy(st);
            if (primary_button(starting_join ? "Starting..." : "Sync & Join",
                               ImVec2(bw, bh), starting_join, essentials_busy)) {
                if (start_essentials_request(st, "join-execute", [] {
                        auto& joins = JoinManager::instance();
                        AsyncUiRequestResult result;
                        result.success = joins.execute_join({});
                        if (!result.success) {
                            result.title = "Could not start session join";
                            result.detail = "Another join is already active, or the join task could not start.";
                            return result;
                        }
                        const JoinProgressSnapshot progress = joins.get_progress_snapshot();
                        result.number_a = static_cast<int64_t>(progress.generation);
                        if (result.number_a <= 0) {
                            result.success = false;
                            result.title = "Could not start session join";
                            result.detail = "The join task did not publish a valid progress state.";
                        }
                        return result;
                    })) {
                    // The worker-owned handoff stays visible until its matching
                    // JoinProgressSnapshot reaches a terminal state.
                    ui.join_syncing = true;
                    ui.join_execution_generation = 0;
                    ui.join_step = 0;
                    ui.join_progress = 0.0f;
                    ui.join_status_text = "Starting the session join...";
                }
            }
            ImGui::SameLine();
        }

        if (ghost_button("CANCEL", ImVec2(bw, bh))) {
            ui.join_dialog_open = false;
        }

        ImGui::EndPopup();
    }
    if (ui.join_dialog_open) {
        ImGui::OpenPopup("Join Session");
    }
}

// ---------------------------------------------------------------------------
// Session Manager
// ---------------------------------------------------------------------------

static void draw_session_manager(UiState& st) {
    auto& ui = get_essentials_ui_state();

    ImGui::SetNextWindowSize(ImVec2(ui_px(500.0f), ui_px(440.0f)),
                             ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Session Manager", &ui.session_manager_open,
                               ImGuiWindowFlags_NoResize)) {

        auto& wh = WorldHost::instance();
        auto session = wh.get_current_session();

        ImGui::TextColored(k.brand, "SESSION: %s",
                           session.world_name.c_str());
        ImGui::Spacing();

        ImGui::Columns(2, nullptr, false);
        ImGui::SetColumnWidth(0, ui_px(240.0f));

        ImGui::Text("State:");
        ImGui::NextColumn();
        ImGui::TextColored(session_state_color(session.state), "%s",
                           session_state_tag(session.state));
        ImGui::Columns(1);

        ImGui::Columns(2, nullptr, false);
        ImGui::SetColumnWidth(0, ui_px(240.0f));

        ImGui::Text("Players:");
        ImGui::NextColumn();
        ImGui::Text("%d / %d", session.player_count, session.player_limit);
        ImGui::Columns(1);

        ImGui::Columns(2, nullptr, false);
        ImGui::SetColumnWidth(0, ui_px(240.0f));

        ImGui::Text("Privacy:");
        ImGui::NextColumn();

        ImGui::SetNextItemWidth(ui_px(160.0f));
        const char* priv_current = privacy_short_name(session.privacy);
        if (ImGui::BeginCombo("##mgr_privacy", priv_current)) {
            for (int i = 0; i < 4; ++i) {
                auto p = static_cast<SessionPrivacy>(i);
                bool selected = (session.privacy == p);
                const bool session_action_busy = essentials_request_lane_busy(st);
                if (ImGui::Selectable(privacy_short_name(p), selected,
                                      session_action_busy ? ImGuiSelectableFlags_Disabled
                                                          : ImGuiSelectableFlags_None)) {
                    start_essentials_request(st, "host-privacy", [p] {
                        AsyncUiRequestResult result;
                        result.success = WorldHost::instance().update_privacy(p);
                        result.title = result.success ? "Session privacy updated"
                                                     : "Could not update session privacy";
                        result.detail = result.success
                            ? std::string("Session is now ") + privacy_short_name(p) + "."
                            : "The previous privacy setting remains active. You can retry.";
                        return result;
                    });
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::Columns(1);

        ImGui::Columns(2, nullptr, false);
        ImGui::SetColumnWidth(0, ui_px(240.0f));

        ImGui::Text("Player Limit:");
        ImGui::NextColumn();

        int limit = session.player_limit;
        ImGui::SetNextItemWidth(ui_px(160.0f));
        const bool session_action_busy = essentials_request_lane_busy(st);
        ImGui::BeginDisabled(session_action_busy);
        ImGui::SliderInt("##mgr_limit", &limit, 2, 32);
        const bool limit_committed = ImGui::IsItemDeactivatedAfterEdit();
        ImGui::EndDisabled();
        if (!session_action_busy && limit_committed && limit != session.player_limit) {
            start_essentials_request(st, "host-player-limit", [limit] {
                AsyncUiRequestResult result;
                result.success = WorldHost::instance().update_player_limit(limit);
                result.title = result.success ? "Player limit updated"
                                             : "Could not update player limit";
                result.detail = result.success
                    ? "The session now allows " + std::to_string(limit) + " players."
                    : "The previous player limit remains active. You can retry.";
                return result;
            });
        }
        ImGui::Columns(1);
        draw_essentials_request_feedback(st, "host-privacy", "Updating session privacy...");
        draw_essentials_request_feedback(st, "host-player-limit", "Updating player limit...");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextColored(k.brand, "CONNECTED PLAYERS");
        ImGui::Spacing();

        ImGui::BeginChild("##mgr_players",
                          ImVec2(ui_px(440.0f), ui_px(120.0f)));

        if (session.connected_players.empty()) {
            ImGui::TextColored(k.muted, "No players connected.");
        } else {
            for (auto& pid : session.connected_players) {
                card_begin(("##mgr_pl_" + pid).c_str());

                ImVec4 ac = avatar_color(pid);
                draw_avatar_circle(
                    std::string(1, name_initial(pid)).c_str(),
                    ac, ui_px(10.0f));
                ImGui::SameLine(0, ui_px(8.0f));

                ImGui::TextUnformatted(pid.c_str());

                ImGui::SameLine(ui_px(300.0f));

                const std::string kick_action = "host-kick:" + pid;
                const std::string ban_action = "host-ban:" + pid;
                const bool player_action_busy = essentials_request_lane_busy(st);
                const bool kicking = essentials_request_is_working(st, kick_action);
                if (ghost_button(kicking ? "Kicking..." : "Kick",
                                 ImVec2(ui_px(60.0f), ui_px(22.0f)), player_action_busy)) {
                    const std::string player_id = pid;
                    start_essentials_request(st, kick_action, [player_id] {
                        AsyncUiRequestResult result;
                        result.success = WorldHost::instance().kick_player(player_id);
                        result.title = result.success ? "Player kicked" : "Could not kick player";
                        result.detail = result.success
                            ? player_id + " has been kicked."
                            : "The player remains connected. You can retry.";
                        return result;
                    });
                }
                ImGui::SameLine();
                const bool banning = essentials_request_is_working(st, ban_action);
                if (ghost_button(banning ? "Banning..." : "Ban",
                                 ImVec2(ui_px(60.0f), ui_px(22.0f)), player_action_busy)) {
                    const std::string player_id = pid;
                    start_essentials_request(st, ban_action, [player_id] {
                        AsyncUiRequestResult result;
                        result.success = WorldHost::instance().ban_player(player_id);
                        result.title = result.success ? "Player banned" : "Could not ban player";
                        result.detail = result.success
                            ? player_id + " has been banned."
                            : "The player remains connected. You can retry.";
                        result.warning = result.success;
                        return result;
                    });
                }
                draw_essentials_request_feedback(st, kick_action, "Kicking player...");
                draw_essentials_request_feedback(st, ban_action, "Banning player...");

                card_end();
            }
        }

        ImGui::EndChild();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        auto bans = SessionManager::instance().get_bans(ui.manage_session_id);
        if (!bans.empty()) {
            ImGui::TextColored(k.red, "BANNED PLAYERS");
            ImGui::Spacing();

            ImGui::BeginChild("##mgr_bans",
                              ImVec2(ui_px(440.0f), ui_px(80.0f)));
            for (auto& ban : bans) {
                ImGui::TextColored(k.red, "%s", ban.username.c_str());
                ImGui::SameLine();
                const std::string unban_action = "host-unban:" + ban.user_id;
                const bool unbanning = essentials_request_is_working(st, unban_action);
                if (ghost_button(unbanning ? "Unbanning..." : "Unban",
                                 ImVec2(ui_px(60.0f), ui_px(22.0f)),
                                 essentials_request_lane_busy(st))) {
                    const std::string user_id = ban.user_id;
                    start_essentials_request(st, unban_action, [user_id] {
                        AsyncUiRequestResult result;
                        result.success = WorldHost::instance().unban_player(user_id);
                        result.title = result.success ? "Player unbanned" : "Could not unban player";
                        result.detail = result.success
                            ? user_id + " can join again."
                            : "The ban remains in place. You can retry.";
                        return result;
                    });
                }
                draw_essentials_request_feedback(st, unban_action, "Removing ban...");
            }
            ImGui::EndChild();
        }

        ImGui::Spacing();

        float bw = ui_px(120.0f);
        float bh = ui_px(30.0f);

        const bool stopping_host = essentials_request_is_working(st, "host-stop");
        const bool session_lane_busy = essentials_request_lane_busy(st);
        if (ghost_button(stopping_host ? "Stopping..." : "Stop Hosting", ImVec2(bw, bh),
                         session_lane_busy)) {
            start_essentials_request(st, "host-stop", [] {
                AsyncUiRequestResult result;
                result.success = WorldHost::instance().stop_hosting();
                result.title = result.success ? "Session ended" : "Could not stop hosting";
                result.detail = result.success
                    ? "World hosting has been stopped."
                    : "The session remains active. You can retry safely.";
                return result;
            });
        }

        ImGui::SameLine();

        if (ghost_button("Close", ImVec2(bw, bh), session_lane_busy)) {
            ui.session_manager_open = false;
        }
        draw_essentials_request_feedback(st, "host-stop", "Stopping hosted world...");

        ImGui::EndPopup();
    }
    if (ui.session_manager_open) {
        ImGui::OpenPopup("Session Manager");
    }
}

// ---------------------------------------------------------------------------
// Profile Actions
// ---------------------------------------------------------------------------

void draw_profile_essentials_actions(UiState& /*st*/,
                                     const std::string& profile_id) {
    auto& ui = get_essentials_ui_state();

    if (primary_button("HOST WORLD",
                       ImVec2(ui_px(110.0f), ui_px(32.0f)))) {
        ui.host_dialog_open = true;
        ui.host_profile_id = profile_id;
    }

    ImGui::SameLine();

    if (ghost_button("INVITE",
                     ImVec2(ui_px(80.0f), ui_px(32.0f)))) {
        ui.invite_dialog_open = true;
    }
}

// ---------------------------------------------------------------------------
// Sidebar Indicator
// ---------------------------------------------------------------------------

void draw_essentials_sidebar_indicator(UiState& /*st*/) {
    auto& wh = WorldHost::instance();
    if (!wh.is_hosting()) return;

    auto session = wh.get_current_session();
    ImGui::TextColored(k.green, "\xe2\x97\x8f");
    ImGui::SameLine();
    ImGui::Text("Hosting");
    ImGui::SameLine();
    ImGui::TextColored(k.muted, "%d/%d", session.player_count,
                       session.player_limit);
}

}  // namespace aml::essentials
