#include "essentials_ui.h"
#include "ui_internal.h"
#include "ui_model.h"
#include "loading_screen.h"
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
#include <sstream>
#include <thread>

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
    bool join_compat_shown = false;
    CompatCheck join_compat;
    SyncPlan join_sync_plan;
    bool join_syncing = false;
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
        if (primary_button("Set Status", ImVec2(ui_px(90.0f), ui_px(26.0f)))) {
            switch (ui.status_editor_selection) {
                case 0: ui.self_status = FriendStatus::Online; break;
                case 1: ui.self_status = FriendStatus::Away; break;
                case 2: ui.self_status = FriendStatus::InLauncher; break;
                case 3: ui.self_status = FriendStatus::Offline; break;
            }
            auto& pm = PresenceManager::instance();
            pm.set_status(ui.self_status, ui.self_status_message);
            ui.status_editor_open = false;
        }
        ImGui::SameLine();
        if (ghost_button("Cancel", ImVec2(ui_px(70.0f), ui_px(26.0f))))
            ui.status_editor_open = false;
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

        if (ghost_button("Remove Friend", ImVec2(bw, bh))) {
            if (FriendsManager::instance().remove_friend(fri_profile.user_id))
                push_notice(st, ui_model::NoticeLevel::Success, "Friend Removed",
                            fri_profile.display_name + " has been removed.");
        }
        ImGui::SameLine();
        if (ghost_button("Block", ImVec2(bw, bh))) {
            if (FriendsManager::instance().block_user(fri_profile.user_id))
                push_notice(st, ui_model::NoticeLevel::Success, "User Blocked",
                            fri_profile.display_name + " has been blocked.");
        }
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Empty state (no friends yet)
// ---------------------------------------------------------------------------

static void draw_essentials_empty_state(UiState& st) {
    auto& ui = get_essentials_ui_state();

    // Left: add friend
    ImGui::PushFont(f_h2);
    ImGui::TextColored(k.text, "Friends");
    ImGui::PopFont();
    ImGui::Spacing();

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
        if (ghost_button("Send Request", ImVec2(ui_px(140.0f), ui_px(26.0f)))) {
            if (!ui.add_friend_input.empty()) {
                if (FriendsManager::instance().send_request(ui.add_friend_input))
                    push_notice(st, ui_model::NoticeLevel::Success,
                                "Friend Request Sent",
                                "Waiting for the other user to accept.");
                else
                    push_notice(st, ui_model::NoticeLevel::Error,
                                "Request Failed",
                                "Could not send friend request.");
                ui.add_friend_input.clear();
                ui.show_add_friend = false;
            }
        }
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
        ui.join_dialog_open = true;
        ui.hero_join_code = true;
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
        ui.join_dialog_open = true;
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
    if (ghost_button("Stop Hosting", ImVec2(ui_px(120.0f), bh))) {
        wh.stop_hosting();
        push_notice(st, ui_model::NoticeLevel::Info,
                    "Session Ended", "World hosting has been stopped.");
    }

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
            if (primary_button("JOIN",
                               ImVec2(ui_px(90.0f), ui_px(28.0f)))) {
                EssentialsInvite accepted;
                if (InviteManager::instance().accept_invite(invite.id, &accepted)) {
                    ui.join_session_id = accepted.session_id;
                    ui.join_address = accepted.session_address;
                    ui.join_token = accepted.join_token;
                    ui.join_dialog_open = !ui.join_address.empty() && !ui.join_token.empty();
                    push_notice(st, ui_model::NoticeLevel::Success,
                                 "Invite Accepted",
                                 "Joining " + invite.world_name + "...");
                } else {
                    push_notice(st, ui_model::NoticeLevel::Error,
                                 "Accept Failed",
                                 "Could not accept invite. Please try again.");
                }
            }
            ImGui::SameLine();
            if (ghost_button("DECLINE",
                             ImVec2(ui_px(90.0f), ui_px(28.0f)))) {
                if (!InviteManager::instance().decline_invite(invite.id))
                    push_notice(st, ui_model::NoticeLevel::Error,
                                "Decline Failed", "Could not decline invite.");
            }
        } else {
            if (ghost_button("CANCEL",
                             ImVec2(ui_px(90.0f), ui_px(28.0f)))) {
                if (!InviteManager::instance().cancel_invite(invite.id))
                    push_notice(st, ui_model::NoticeLevel::Error,
                                "Cancel Failed", "Could not cancel invite.");
            }
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
        if (ImGui::MenuItem("Remove Friend")) {
            if (FriendsManager::instance().remove_friend(fri.user_id))
                push_notice(st, ui_model::NoticeLevel::Success, "Friend Removed",
                            fri.display_name + " has been removed.");
        }
        if (ImGui::MenuItem("Block User")) {
            if (FriendsManager::instance().block_user(fri.user_id))
                push_notice(st, ui_model::NoticeLevel::Success, "User Blocked",
                            fri.display_name + " has been blocked.");
        }
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
            if (ghost_button("Send Request", ImVec2(-1, ui_px(26.0f)))) {
                if (!ui.add_friend_input.empty()) {
                    if (fm.send_request(ui.add_friend_input))
                        push_notice(st, ui_model::NoticeLevel::Success,
                                    "Friend Request Sent",
                                    "Waiting for the other user to accept.");
                    else
                        push_notice(st, ui_model::NoticeLevel::Error,
                                    "Request Failed",
                                    "Could not send friend request.");
                    ui.add_friend_input.clear();
                    ui.show_add_friend = false;
                }
            }
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
                if (ghost_button("Accept", ImVec2(ui_px(70.0f), ui_px(24.0f)))) {
                    if (fm.accept_request(req.id))
                        push_notice(st, ui_model::NoticeLevel::Success,
                                    "Request Accepted", req.sender_username + " is now your friend.");
                    else
                        push_notice(st, ui_model::NoticeLevel::Error,
                                    "Accept Failed", "Could not accept request.");
                }
                ImGui::SameLine();
                if (ghost_button("Decline", ImVec2(ui_px(70.0f), ui_px(24.0f)))) {
                    if (!fm.decline_request(req.id))
                        push_notice(st, ui_model::NoticeLevel::Error,
                                    "Decline Failed", "Could not decline request.");
                }
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
    if (primary_button("JOIN", ImVec2(ui_px(100.0f), ui_px(32.0f))) &&
        !ui.join_address.empty() && !ui.join_token.empty()) {
        ui.join_dialog_open = true;
        ui.join_compat_shown = false;
        ui.join_step = 0;
        ui.join_progress = 0.0f;
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
                               ImVec2(ui_px(100.0f), ui_px(30.0f)))) {
                ui.join_dialog_open = true;
                ui.join_session_id = sess.id;
                ui.join_address = sess.address;
                ui.join_token.clear();
                ui.join_compat_shown = false;
                ui.join_step = 0;
                ui.join_progress = 0.0f;
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

static void draw_notifications_tab(UiState& /*st*/) {
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
            if (primary_button("Accept",
                               ImVec2(ui_px(80.0f), ui_px(26.0f)))) {
                notif.on_accept();
                sm.clear_notification(notif.id);
            }
            ImGui::SameLine();
        }
        if (notif.on_decline) {
            if (ghost_button("Decline",
                             ImVec2(ui_px(80.0f), ui_px(26.0f)))) {
                notif.on_decline();
                sm.clear_notification(notif.id);
            }
            ImGui::SameLine();
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

        if (primary_button("START HOSTING", ImVec2(bw, bh))) {
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

                auto& wh = WorldHost::instance();
                EssentialsAddressResolver resolver;
                if (!EssentialsAddressResolver::IsValidAlias(opts.alias)) {
                    push_notice(st, ui_model::NoticeLevel::Error,
                                "Invalid Address", "Choose a valid non-reserved Essentials address.");
                } else if (!resolver.IsAvailable(opts.alias)) {
                    const auto suggestions = EssentialsAddressResolver::Suggestions(opts.alias);
                    push_notice(st, ui_model::NoticeLevel::Error,
                                "Address Unavailable",
                                "That address is already active. Try " + suggestions[0] +
                                " or " + suggestions[1] + ".");
                } else if (wh.start_hosting(opts, [](float, const std::string&) {})) {
                    ui.host_dialog_open = false;
                    push_notice(st, ui_model::NoticeLevel::Success,
                                "Hosting Started",
                                "Your world is online at " + EssentialsAddressResolver::ToAddress(opts.alias));
                } else {
                    push_notice(st, ui_model::NoticeLevel::Error,
                                "Hosting Failed",
                                "The local world or Essentials transport could not be started.");
                }
            }
        }

        ImGui::SameLine();

        if (ghost_button("CANCEL", ImVec2(bw, bh))) {
            ui.host_dialog_open = false;
        }

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

        if (primary_button(send_label.c_str(), ImVec2(bw, bh))) {
            if (ui.selected_invitees.empty()) {
                push_notice(st, ui_model::NoticeLevel::Warning,
                            "No Friends Selected",
                            "Select at least one friend to invite.");
            } else {
                auto& im = InviteManager::instance();
                auto& sm = SessionManager::instance();
                auto session = sm.get_session(ui.invite_session_id);
                int sent = 0, failed = 0;
                for (auto& uid : ui.selected_invitees) {
                    if (im.send_invite(uid, session))
                        ++sent;
                    else
                        ++failed;
                }
                if (failed > 0)
                    push_notice(st, ui_model::NoticeLevel::Warning,
                                "Partial Failure",
                                std::to_string(sent) + " invite(s) sent, " +
                                std::to_string(failed) + " failed.");
                else
                    push_notice(st, ui_model::NoticeLevel::Success,
                                "Invites Sent",
                                "Invitations sent to " + std::to_string(sent) +
                                    " friend(s).");
                ui.selected_invitees.clear();
                ui.invite_dialog_open = false;
            }
        }

        ImGui::SameLine();

        if (ghost_button("CANCEL", ImVec2(bw, bh))) {
            ui.selected_invitees.clear();
            ui.invite_dialog_open = false;
        }

        ImGui::EndPopup();
    }
    if (ui.invite_dialog_open) {
        ImGui::OpenPopup("Invite Friends");
    }
}

// ---------------------------------------------------------------------------
// Join Dialog
// ---------------------------------------------------------------------------

static void draw_join_dialog(UiState&) {
    auto& ui = get_essentials_ui_state();

    ImGui::SetNextWindowSize(ImVec2(ui_px(500.0f), ui_px(480.0f)),
                             ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Join Session", &ui.join_dialog_open,
                               ImGuiWindowFlags_NoResize)) {

        if (!ui.join_compat_shown) {
            ImGui::TextUnformatted("Essentials Address");
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputText("##join_address", &ui.join_address);
            ImGui::TextUnformatted("Join code");
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputText("##join_token", &ui.join_token,
                             ImGuiInputTextFlags_Password);
            if (ui.join_address.empty() || ui.join_token.empty()) {
                ImGui::TextColored(k.muted, "Enter the address and host join code to continue.");
                ImGui::Spacing();
                if (ghost_button("Close", ImVec2(ui_px(100.0f), ui_px(30.0f))))
                    ui.join_dialog_open = false;
                ImGui::EndPopup();
                return;
            }
            if (ui.join_session_id.empty()) {
                EssentialsAddressResolver resolver;
                const auto resolved = resolver.Resolve(ui.join_address);
                if (!resolved.success) {
                    ImGui::TextColored(k.red, "%s", resolved.error.c_str());
                    ImGui::Spacing();
                    if (ghost_button("Close", ImVec2(ui_px(100.0f), ui_px(30.0f))))
                        ui.join_dialog_open = false;
                    ImGui::EndPopup();
                    return;
                }
                ui.join_session_id = resolved.session_id;
                ui.join_address = resolved.address;
            }
            ImGui::TextColored(k.muted, "Checking compatibility...");
            ImGui::Spacing();

            auto& jm = JoinManager::instance();
            auto result = jm.analyze_and_prepare(ui.join_session_id, ui.join_token);
            ui.join_compat = jm.get_current_compat();
            ui.join_sync_plan = jm.get_current_sync_plan();
            ui.join_compat_shown = true;

            if (!result.success) {
                ImGui::TextColored(k.red, "Error: %s",
                                   result.error.c_str());
                ImGui::Spacing();
                if (ghost_button("Close",
                                 ImVec2(ui_px(100.0f), ui_px(30.0f)))) {
                    ui.join_dialog_open = false;
                }
                ImGui::EndPopup();
                return;
            }
        }

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

        bool can_join = ui.join_compat.overall !=
                            CompatibilityLevel::Incompatible &&
                        !ui.join_syncing;

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

        ImGui::Spacing();

        float bw = ui_px(160.0f);
        float bh = ui_px(34.0f);

        if (can_join) {
            if (primary_button(ui.join_syncing ? "Syncing..." : "Sync & Join",
                               ImVec2(bw, bh))) {
                ui.join_syncing = true;
                ui.join_step = 1;
                ui.join_progress = 0.1f;
                auto& loading = aml::ui::LoadingScreen::instance();
                loading.show_fullscreen("Joining Session",
                                        "Preparing to connect...");
                loading.begin_steps({"Check Compatibility", "Sync Profile",
                                     "Download Mods", "Launch Game",
                                     "Connect to Host"});

                std::string session_id = ui.join_session_id;
                ui.join_dialog_open = false;

                std::thread([session_id]() {
                    auto& jm = JoinManager::instance();
                    auto& loading = aml::ui::LoadingScreen::instance();
                    loading.advance_step("Checking compatibility...");
                    jm.execute_join([&loading](float prog, const std::string& status) {
                        if (!status.empty()) {
                            if (prog < 0.3f)
                                loading.advance_step("Checking compatibility...");
                            else if (prog < 0.6f)
                                loading.advance_step("Syncing profile...");
                            else if (prog < 0.8f)
                                loading.advance_step("Setting up relay...");
                            else
                                loading.advance_step("Launching game...");
                        }
                    });

                    if (jm.is_joining()) {
                        loading.advance_step("Connected!");
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(600));
                    }
                    loading.hide();
                }).detach();

                ui.join_syncing = false;
            }
        } else if (ui.join_compat.overall ==
                   CompatibilityLevel::Incompatible) {
            ImGui::TextColored(k.red,
                               "Session is incompatible. Cannot join.");
        }

        ImGui::SameLine();

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
                if (ImGui::Selectable(privacy_short_name(p), selected)) {
                    wh.update_privacy(p);
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
        if (ImGui::SliderInt("##mgr_limit", &limit, 2, 32)) {
            wh.update_player_limit(limit);
        }
        ImGui::Columns(1);

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

                if (ghost_button("Kick",
                                 ImVec2(ui_px(60.0f), ui_px(22.0f)))) {
                    wh.kick_player(pid);
                    push_notice(st, ui_model::NoticeLevel::Info,
                                "Player Kicked",
                                pid + " has been kicked.");
                }
                ImGui::SameLine();
                if (ghost_button("Ban",
                                 ImVec2(ui_px(60.0f), ui_px(22.0f)))) {
                    wh.ban_player(pid);
                    push_notice(st, ui_model::NoticeLevel::Warning,
                                "Player Banned",
                                pid + " has been banned.");
                }

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
                if (ghost_button("Unban",
                                 ImVec2(ui_px(60.0f), ui_px(22.0f)))) {
                    wh.unban_player(ban.user_id);
                }
            }
            ImGui::EndChild();
        }

        ImGui::Spacing();

        float bw = ui_px(120.0f);
        float bh = ui_px(30.0f);

        if (ghost_button("Stop Hosting", ImVec2(bw, bh))) {
            wh.stop_hosting();
            ui.session_manager_open = false;
            push_notice(st, ui_model::NoticeLevel::Info,
                        "Session Ended",
                        "World hosting has been stopped.");
        }

        ImGui::SameLine();

        if (ghost_button("Close", ImVec2(bw, bh))) {
            ui.session_manager_open = false;
        }

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
