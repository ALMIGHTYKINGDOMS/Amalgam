#include "ui.h"
#include "ui_internal.h"
#include "supabase.h"
#include "account_manager.h"
#include "essentials_manager.h"
#include "entitlements.h"
#include "net.h"
#include "config.h"
#include "online_config.h"

#include <windows.h>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace aml::ui {

// ---------------------------------------------------------------------------
// Social UI State
// ---------------------------------------------------------------------------

struct SocialUIState {
    int current_tab = 0; // 0=friends, 1=messages, 2=parties, 3=settings
    
    // Friends
    std::string friend_filter;
    int friend_sort = 0; // 0=username, 1=status, 2=last seen
    std::string friend_search;
    bool friend_requests_open = false;
    
    // Messages
    std::string selected_friend_id;
    std::string selected_conversation_id;
    std::string message_input;
    std::vector<std::string> message_history;
    std::string profile_friend_id;
    std::string profile_friend_name;
    bool profile_open = false;
    
    // Parties
    std::string party_name;
    std::string party_invite_code;
    bool party_creating = false;
    
    // Settings
    bool notifications_enabled = true;
    bool show_offline_friends = true;
    bool auto_accept_requests = false;
    bool is_public = true;
};

std::string format_time_ago(int64_t timestamp);

static SocialUIState& get_social_ui_state() {
    static SocialUIState state;
    return state;
}

static std::string bridge_field(std::string value) {
    for (char& c : value) {
        if (c == '\r' || c == '\n' || c == '|') c = ' ';
    }
    return value;
}

template <typename Writer>
static bool write_bridge_file(const std::filesystem::path& target, Writer writer,
                              std::string* error) {
    const std::filesystem::path temporary = target.wstring() + L".tmp-" +
        std::to_wstring(GetCurrentProcessId());
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        if (error) *error = "Could not create " + target.filename().string();
        return false;
    }
    writer(out);
    out.flush();
    const bool wrote = out.good();
    out.close();
    if (!wrote || !MoveFileExW(temporary.c_str(), target.c_str(),
                               MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        if (error) *error = "Could not publish " + target.filename().string();
        return false;
    }
    return true;
}

bool write_client_bridge(const config::Config& cfg, const std::wstring& target_dir,
                         std::string* error) {
    if (target_dir.empty()) {
        if (error) *error = "Client bridge directory is empty";
        return false;
    }
    std::error_code directory_error;
    const std::filesystem::path root(target_dir);
    std::filesystem::create_directories(root, directory_error);
    if (directory_error) {
        if (error) *error = "Could not create the client bridge directory";
        return false;
    }

    auto& friends_manager = aml::essentials::FriendsManager::instance();
    const auto friends_list = friends_manager.get_friends();
    if (!write_bridge_file(root / L"shared_friends.txt", [&](std::ofstream& out) {
        out << "# Shared friends list (launcher -> client)\n";
        for (const auto& friend_entry : friends_list) {
            const char* status = "Offline";
            if (friend_entry.status == aml::essentials::FriendStatus::Online) status = "Online";
            else if (friend_entry.status == aml::essentials::FriendStatus::Playing) status = "Playing";
            else if (friend_entry.status == aml::essentials::FriendStatus::InLauncher) status = "In Launcher";
            out << bridge_field(friend_entry.username) << "|" << status << "\n";
        }
    }, error)) return false;

    if (!write_bridge_file(root / L"shared_servers.txt", [&](std::ofstream& out) {
        out << "# Shared server list (launcher -> client)\n";
        // The official network is always joinable, ahead of the player's own
        // entries, so the client never lists an empty server tab.
        const auto& online = aml::online::config();
        if (!online.network_address.empty()) {
            out << bridge_field(online.network_name) << "|"
                << bridge_field(online.network_address) << "\n";
        }
        for (const auto& server : cfg.servers) {
            out << bridge_field(server.name) << "|"
                << bridge_field(aml::net::to_utf8(server.address)) << "\n";
        }
    }, error)) return false;

    const bool plus = aml::entitlements::EntitlementManager::instance().is_plus();
    struct CosmeticDef {
        const char* id;
        const char* name;
        const char* category;
        int rarity;
        bool premium;
        const char* description;
    };
    static const CosmeticDef kCosmetics[] = {
        {"amalgam_cape", "Amalgam Cape", "cape", 3, true, "The official Amalgam cape."},
        {"ender_dragon", "Ender Dragon Cape", "cape", 2, true, "Wings of the End."},
        {"nether_flame", "Nether Flame", "cape", 1, true, "Burns bright in the Nether."},
        {"void_walker", "Void Walker", "cape", 2, true, "Step between dimensions."},
        {"glacial", "Glacial", "cape", 0, false, "Cool as ice."},
        {"starlight", "Starlight", "cape", 1, true, "Born from the stars."},
        {"beta_tester", "Beta Tester", "badge", 3, false, "Original Amalgam tester."},
        {"content_creator", "Content Creator", "badge", 2, true, "For creators."},
        {"early_adopter", "Early Adopter", "badge", 1, false, "Here from the start."},
        {"purple_glow", "Purple Glow", "nameplate", 1, false, "Glowing purple name."},
        {"rainbow", "Rainbow", "nameplate", 2, true, "Colorful cycling name."},
        {"fire_trail", "Fire Trail", "nameplate", 3, true, "Fiery name effect."},
        {"wave", "Wave", "emote", 0, false, "Friendly wave."},
        {"dance", "Dance", "emote", 1, true, "Show your moves."},
        {"bow", "Bow", "emote", 0, false, "A respectful bow."},
        {"baby_dragon", "Baby Dragon", "pet", 3, true, "A loyal dragon companion."},
        {"cat", "Cat", "pet", 0, false, "A friendly feline."},
        {"parrot", "Parrot", "pet", 1, true, "A colorful feathered friend."},
    };
    return write_bridge_file(root / L"shared_cosmetics.txt", [&](std::ofstream& out) {
        out << "# id|name|category|rarity|owned|equipped|description\n";
        for (const auto& cosmetic : kCosmetics) {
            out << cosmetic.id << "|" << cosmetic.name << "|" << cosmetic.category << "|"
                << cosmetic.rarity << "|"
                << (cosmetic.premium ? (plus ? 1 : 0) : 1) << "|0|"
                << cosmetic.description << "\n";
        }
    }, error);
}

void load_social_settings(const config::Config& cfg) {
    auto& s = get_social_ui_state();
    s.notifications_enabled = cfg.social_notifications;
    s.show_offline_friends = cfg.social_show_offline;
    s.auto_accept_requests = cfg.social_auto_accept;
}

void save_social_settings(config::Config& cfg) {
    const auto& s = get_social_ui_state();
    cfg.social_notifications = s.notifications_enabled;
    cfg.social_show_offline = s.show_offline_friends;
    cfg.social_auto_accept = s.auto_accept_requests;

    // Keep a global compatibility copy for older builds. New launches also
    // write these files directly into the selected profile.
    write_client_bridge(cfg, cfg.base_dir, nullptr);
}

// ---------------------------------------------------------------------------
// Social Data Structures
// ---------------------------------------------------------------------------

struct Friend {
    std::string id;
    std::string username;
    std::string avatar_url;
    std::string status;
    std::string status_message;
    int64_t last_seen;
    bool is_online;
    bool is_favorite;
    bool has_unread;
};

struct Message {
    std::string id;
    std::string sender_id;
    std::string sender_name;
    std::string content;
    int64_t timestamp;
    bool is_read;
};

struct Party {
    std::string id;
    std::string name;
    std::string owner_id;
    std::string owner_name;
    std::vector<std::string> member_ids;
    std::vector<std::string> member_names;
    int max_members;
    bool is_public;
    std::string invite_code;
    int64_t created_at;
};

static std::vector<Friend> load_social_friends() {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    const auto friendships = supabase.get_friends();
    const auto presence = supabase.get_friends_presence();
    const auto blocked = supabase.get_blocked_users();
    const auto current = supabase.get_current_user();
    std::vector<Friend> result;
    for (const auto& friendship : friendships) {
        if (friendship.status != "accepted") continue;
        Friend f;
        f.id = friendship.user_id_a == current.id ? friendship.user_id_b : friendship.user_id_a;
        if (std::find(blocked.begin(), blocked.end(), f.id) != blocked.end()) continue;
        f.username = friendship.friend_minecraft_username.empty()
            ? friendship.friend_display_name
            : friendship.friend_minecraft_username;
        if (f.username.empty()) f.username = f.id;
        f.status = "offline";
        f.status_message.clear();
        f.last_seen = friendship.updated_at;
        f.is_online = false;
        for (const auto& p : presence) {
            if (p.user_id != f.id) continue;
            f.status = p.status;
            f.status_message = p.status_message;
            f.last_seen = p.last_seen_at;
            f.is_online = p.status == "online" || p.status == "in_game";
            break;
        }
        f.is_favorite = false;
        f.has_unread = false;
        result.push_back(std::move(f));
    }
    return result;
}

// ---------------------------------------------------------------------------
// Friends Tab
// ---------------------------------------------------------------------------

void draw_social_friends(UiState& st) {
    auto& social_ui = get_social_ui_state();
    
    page_title("Friends", "Manage your friends list and social connections");
    
    card_begin("##social_friends_header");
    
    ImGui::TextUnformatted("Friends");
    const bool compact_social_header = ImGui::GetContentRegionAvail().x < ui_px(560.0f);
    if (!compact_social_header)
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - ui_px(300.0f));
    else
        ImGui::Spacing();
    
    // Friend requests button
    if (ghost_button("Friend Requests", ImVec2(ui_px(120.0f), ui_px(32.0f)))) {
        social_ui.friend_requests_open = !social_ui.friend_requests_open;
    }
    
    if (!compact_social_header) ImGui::SameLine();
    else ImGui::Spacing();
    
    // Add friend button
    if (primary_button("+ Add Friend", ImVec2(ui_px(120.0f), ui_px(32.0f)))) {
        request_popup("##add_friend_popup");
    }
    
    ImGui::Spacing();
    
    // Filter and sort
    const float filter_available = ImGui::GetContentRegionAvail().x;
    const bool compact_filters = filter_available < ui_px(520.0f);
    ImGui::SetNextItemWidth(compact_filters
        ? -1.0f
        : std::min(ui_px(240.0f), std::max(ui_px(160.0f), filter_available - ui_px(176.0f))));
    input_text_hint("##social_friend_filter", "Filter friends...", &social_ui.friend_filter);
    if (!compact_filters) ImGui::SameLine();
    
    ImGui::SetNextItemWidth(compact_filters ? -1.0f : ui_px(160.0f));
    const char* sort_options[] = {"Username (A-Z)", "Status", "Last Seen"};
    if (ImGui::BeginCombo("##social_friend_sort", sort_options[social_ui.friend_sort])) {
        for (int i = 0; i < 3; ++i) {
            if (ImGui::Selectable(sort_options[i], social_ui.friend_sort == i)) {
                social_ui.friend_sort = i;
            }
        }
        ImGui::EndCombo();
    }
    
    card_end();
    
    ImGui::Spacing();
    
    // Add friend popup
    if (ImGui::BeginPopup("##add_friend_popup")) {
        ImGui::TextUnformatted("Add Friend");
        ImGui::Separator();
        ImGui::Spacing();
        
        ImGui::TextUnformatted("Username or Email");
        ImGui::SetNextItemWidth(ui_px(300.0f));
        ImGui::InputText("##add_friend_input", &social_ui.friend_search);
        
        ImGui::Spacing();
        
        if (primary_button("Send Request", ImVec2(ui_px(120.0f), ui_px(32.0f)))) {
            if (!social_ui.friend_search.empty()) {
                auto& supabase = aml::supabase::SupabaseManager::instance();
                const auto users = supabase.search_users(social_ui.friend_search, 1);
                if (users.empty()) {
                    push_notice(st, ui_model::NoticeLevel::Warning, "User Not Found", "No matching public account was found");
                } else if (supabase.send_friend_request(users.front().user_id)) {
                    push_notice(st, ui_model::NoticeLevel::Success, "Request Sent", "Friend request sent");
                    social_ui.friend_search.clear();
                    ImGui::CloseCurrentPopup();
                } else {
                    push_notice(st, ui_model::NoticeLevel::Error, "Request Failed", "The friend request could not be sent");
                }
            }
        }
        
        ImGui::SameLine();
        
        if (ghost_button("Cancel", ImVec2(ui_px(100.0f), ui_px(32.0f)))) {
            social_ui.friend_search.clear();
            ImGui::CloseCurrentPopup();
        }
        
        ImGui::EndPopup();
    }
    
    // Friend requests panel
    if (social_ui.friend_requests_open) {
        card_begin("##social_friend_requests");
        ImGui::TextUnformatted("Friend Requests");
        ImGui::Separator();
        ImGui::Spacing();
        
        auto requests = aml::supabase::SupabaseManager::instance().get_friend_requests();
        if (requests.empty()) {
            ImGui::TextColored(k.muted, "No pending friend requests");
        } else {
            for (const auto& request : requests) {
                ImGui::Text("%s", request.sender_username.c_str());
                ImGui::SameLine();
                if (primary_button(("Accept##" + request.id).c_str(), ImVec2(ui_px(75.0f), ui_px(25.0f)))) {
                    if (aml::supabase::SupabaseManager::instance().accept_friend_request(request.id))
                        push_notice(st, ui_model::NoticeLevel::Success, "Friend Added", request.sender_username);
                }
                ImGui::SameLine();
                if (ghost_button(("Reject##" + request.id).c_str(), ImVec2(ui_px(75.0f), ui_px(25.0f))))
                    aml::supabase::SupabaseManager::instance().reject_friend_request(request.id);
            }
        }
        
        card_end();
        ImGui::Spacing();
    }
    
    auto friends = load_social_friends();
    
    if (friends.empty()) {
        empty_state("No Friends", "Add friends to start chatting and playing together.", "F");
        return;
    }
    
    // Filter friends
    auto filtered_friends = friends;
    if (!social_ui.friend_filter.empty()) {
        std::string filter = social_ui.friend_filter;
        std::transform(filter.begin(), filter.end(), filter.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        
        filtered_friends.erase(std::remove_if(filtered_friends.begin(), filtered_friends.end(),
            [&filter](const auto& friend_) {
                std::string username = friend_.username;
                std::transform(username.begin(), username.end(), username.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                return username.find(filter) == std::string::npos;
            }), filtered_friends.end());
    }
    
    // Sort friends
    std::sort(filtered_friends.begin(), filtered_friends.end(),
        [&social_ui](const auto& a, const auto& b) {
            switch (social_ui.friend_sort) {
                case 1: return a.is_online > b.is_online; // Status (online first)
                case 2: return a.last_seen > b.last_seen; // Last Seen (recent first)
                default: return a.username < b.username; // Username (A-Z)
            }
        });
    
    // Display friends
    for (auto& friend_ : filtered_friends) {
        ImGui::PushID(friend_.id.c_str());
        
        card_begin(("##social_friend_" + friend_.id).c_str(), ImVec2(-1, ui_px(70.0f)));
        
        ImGui::BeginGroup();
        
        // Friend info
        ImGui::Text("%s", friend_.username.c_str());
        
        // Status
        if (friend_.is_online) {
            ImGui::TextColored(k.green, "● Online");
            ImGui::SameLine();
            ImGui::TextColored(k.muted, " - %s", friend_.status_message.c_str());
        } else {
            ImGui::TextColored(k.muted, "○ Offline");
            ImGui::SameLine();
            ImGui::TextColored(k.muted, " - Last seen: %s", format_time_ago(friend_.last_seen).c_str());
        }
        
        ImGui::EndGroup();
        
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - ui_px(200.0f));
        
        ImGui::BeginGroup();
        
        // Message button
        if (ghost_button("Message", ImVec2(ui_px(80.0f), ui_px(28.0f)))) {
            social_ui.selected_friend_id = friend_.id;
            social_ui.current_tab = 1; // Switch to messages tab
        }
        
        ImGui::SameLine();
        
        // More button
        if (icon_button(IconId::More, ImVec2(ui_px(30.0f), ui_px(28.0f)),
                        "More friend actions")) {
            ImGui::OpenPopup(("##social_friend_menu_" + friend_.id).c_str());
        }
        
        // Friend menu
        if (ImGui::BeginPopup(("##social_friend_menu_" + friend_.id).c_str())) {
            if (ImGui::MenuItem("View Profile")) {
                social_ui.profile_friend_id = friend_.id;
                social_ui.profile_friend_name = friend_.username;
                // Opened below, in the window that begins the modal.
                social_ui.profile_open = true;
            }
            
            if (ImGui::MenuItem("Remove Friend", nullptr, false, true)) {
                if (aml::supabase::SupabaseManager::instance().remove_friend(friend_.id))
                    push_notice(st, ui_model::NoticeLevel::Success, "Friend Removed", friend_.username);
            }

            if (ImGui::MenuItem("Block User", nullptr, false, true)) {
                if (aml::supabase::SupabaseManager::instance().block_user(friend_.id))
                    push_notice(st, ui_model::NoticeLevel::Success, "User Blocked", friend_.username);
                else
                    push_notice(st, ui_model::NoticeLevel::Error, "Block Failed", "The user could not be blocked");
            }
            
            ImGui::EndPopup();
        }
        
        ImGui::EndGroup();
        
        card_end();
        ImGui::PopID();
    }

    if (social_ui.profile_open) ImGui::OpenPopup("##social_friend_profile");
    if (ImGui::BeginPopupModal("##social_friend_profile", &social_ui.profile_open,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Friend Profile");
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextColored(k.brand, "%s", social_ui.profile_friend_name.c_str());
        ImGui::TextColored(k.muted, "User ID: %s", social_ui.profile_friend_id.c_str());
        ImGui::Spacing();
        if (ghost_button("Close", ImVec2(ui_px(90.0f), ui_px(30.0f)))) {
            social_ui.profile_open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// ---------------------------------------------------------------------------
// Messages Tab
// ---------------------------------------------------------------------------

void draw_social_messages(UiState& st) {
    auto& social_ui = get_social_ui_state();
    
    page_title("Messages", "Chat with your friends");
    
    // Friend list sidebar. At narrow widths, stack the list above the
    // conversation so message bubbles and the composer retain usable space.
    const bool narrow_messages = ImGui::GetContentRegionAvail().x < ui_px(760.0f);
    ImGui::BeginChild("##social_friend_list",
                      ImVec2(narrow_messages ? -1.0f : ui_px(250.0f),
                             narrow_messages ? ui_px(180.0f) : -1.0f), true);
    
    ImGui::PushFont(f_bold);
    ImGui::TextUnformatted("Friends");
    ImGui::PopFont();
    ImGui::Separator();
    ImGui::Spacing();
    
    auto friends = load_social_friends();
    int online_count = 0;
    for (auto& f : friends) if (f.is_online) ++online_count;
    ImGui::TextColored(k.muted, "%d online", online_count);
    ImGui::Spacing();
    
    for (auto& friend_ : friends) {
        ImGui::PushID(friend_.id.c_str());
        
        const bool selected = social_ui.selected_friend_id == friend_.id;
        const float row_h = ui_px(48.0f);
        const ImVec2 row_min = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        
        // Row background + selection
        dl->AddRectFilled(row_min, row_min + ImVec2(ImGui::GetContentRegionAvail().x, row_h),
                          c32(selected ? k.sel : ImVec4(0, 0, 0, 0)), ui_px(8.0f));
        if (selected)
            dl->AddRect(row_min, row_min + ImVec2(ImGui::GetContentRegionAvail().x, row_h),
                        c32(k.brand), ui_px(8.0f), 0, ui_px(1.5f));
        
        // Avatar
        const ImVec2 av_pos = row_min + ImVec2(ui_px(8.0f), (row_h - ui_px(28.0f)) * 0.5f);
        dl->AddCircleFilled(av_pos + ImVec2(ui_px(14.0f), ui_px(14.0f)), ui_px(14.0f),
                            c32(friend_.is_online ? k.green : k.surface2));
        const char initial = friend_.username.empty() ? '?' : friend_.username[0];
        char init_str[2] = { initial, '\0' };
        const ImVec2 ts = ImGui::CalcTextSize(init_str);
        dl->AddText(av_pos + ImVec2(ui_px(14.0f) - ts.x * 0.5f,
                                    ui_px(14.0f) - ts.y * 0.5f),
                    c32(friend_.is_online ? ImVec4(0.05f, 0.1f, 0.06f, 1.0f) : k.muted),
                    init_str);
        
        // Name + presence
        ImGui::PushFont(f_bold);
        dl->AddText(row_min + ImVec2(ui_px(44.0f), ui_px(6.0f)), c32(k.text),
                    friend_.username.c_str());
        ImGui::PopFont();
        dl->AddText(row_min + ImVec2(ui_px(44.0f), ui_px(26.0f)),
                    c32(friend_.is_online ? k.green : k.muted),
                    friend_.is_online ? "Online" : "Offline");
        
        // Unread dot
        if (friend_.has_unread) {
            dl->AddCircleFilled(row_min + ImVec2(ImGui::GetContentRegionAvail().x - ui_px(10.0f),
                                                 row_h * 0.5f),
                                ui_px(4.0f), c32(k.brand));
        }
        
        ImGui::InvisibleButton(("##social_message_friend_" + friend_.id).c_str(),
                               ImVec2(-1, row_h));
        if (ImGui::IsItemHovered())
            dl->AddRect(row_min, row_min + ImVec2(ImGui::GetContentRegionAvail().x, row_h),
                        c32(k.border), ui_px(8.0f), 0, ui_px(1.0f));
        if (ImGui::IsItemClicked()) {
            social_ui.selected_friend_id = friend_.id;
            social_ui.message_history.clear();
            auto& supabase = aml::supabase::SupabaseManager::instance();
            const auto conversation = supabase.get_or_create_conversation(friend_.id);
            social_ui.selected_conversation_id = conversation.id;
            for (const auto& message : supabase.get_messages(conversation.id))
                social_ui.message_history.push_back(message.sender_username + "\n" + message.content);
        }
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        
        ImGui::Spacing();
        ImGui::PopID();
    }
    
    ImGui::EndChild();
    
    if (!narrow_messages) ImGui::SameLine();
    else ImGui::Spacing();
    
    // Message area
    ImGui::BeginChild("##social_message_area",
                      ImVec2(-1, narrow_messages ? ui_px(360.0f) : -ui_px(100.0f)), true);
    
    if (social_ui.selected_friend_id.empty()) {
        empty_state("Select a Friend", "Choose a friend from the list to start chatting.");
    } else {
        // Display messages as bubbles
        ImGui::BeginChild("##social_message_history", ImVec2(-1, -1), false);
        
        const char* self_name = player_display_name(st).empty() ? "You" : player_display_name(st).c_str();
        for (auto& message : social_ui.message_history) {
            const size_t sep = message.find('\n');
            const std::string sender = sep == std::string::npos ? "" : message.substr(0, sep);
            const std::string content = sep == std::string::npos ? message : message.substr(sep + 1);
            const bool mine = sender.empty() || sender == self_name || sender == "You";
            const float avail = ImGui::GetContentRegionAvail().x;
            const float max_w = std::min(avail * 0.72f, ui_px(460.0f));
            const ImVec2 text_sz = ImGui::CalcTextSize(content.c_str(), nullptr, false, max_w);
            const float pad = ui_px(10.0f);
            const float bubble_w = std::min(max_w, text_sz.x) + pad * 2.0f;
            const float bubble_h = text_sz.y + pad * 1.6f;
            const float x = mine ? avail - bubble_w : 0.0f;
            const ImVec2 bmin = ImGui::GetCursorScreenPos() + ImVec2(x, 0);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(bmin, bmin + ImVec2(bubble_w, bubble_h),
                              c32(mine ? k.brand_dk : k.surface2), ui_px(9.0f),
                              mine ? (ImDrawFlags_RoundCornersTopLeft | ImDrawFlags_RoundCornersTopRight |
                                      ImDrawFlags_RoundCornersBottomLeft)
                                   : (ImDrawFlags_RoundCornersTopLeft | ImDrawFlags_RoundCornersTopRight |
                                      ImDrawFlags_RoundCornersBottomRight));
            if (!mine && !sender.empty()) {
                dl->AddText(bmin + ImVec2(pad, ui_px(3.0f)), c32(k.brand), sender.c_str());
            }
            dl->AddText(bmin + ImVec2(pad, pad * 0.6f + (mine ? 0.0f : ui_px(14.0f))),
                        c32(k.text), content.c_str());
            ImGui::Dummy(ImVec2(avail, bubble_h + ui_px(6.0f)));
        }
        
        if (social_ui.message_history.empty()) {
            ImGui::TextColored(k.muted, "No messages yet. Start a conversation!");
        }
        
        ImGui::EndChild();
    }
    
    ImGui::EndChild();
    
    // Message input
    if (!social_ui.selected_friend_id.empty()) {
        ImGui::Spacing();
        
        card_begin("##social_message_input");
        ImGui::SetNextItemWidth(-ui_px(100.0f));
        ImGui::InputText("##social_message_input_text", &social_ui.message_input,
                       ImGuiInputTextFlags_EnterReturnsTrue);
        
        ImGui::SameLine();
        
        if (primary_button("Send", ImVec2(ui_px(80.0f), ui_px(32.0f)))) {
            if (!social_ui.message_input.empty()) {
                auto& supabase = aml::supabase::SupabaseManager::instance();
                if (social_ui.selected_conversation_id.empty()) {
                    const auto conversation = supabase.get_or_create_conversation(social_ui.selected_friend_id);
                    social_ui.selected_conversation_id = conversation.id;
                }
                const auto sent = supabase.send_message(social_ui.selected_conversation_id,
                                                        social_ui.message_input);
                if (!sent.id.empty()) {
                    social_ui.message_history.push_back(std::string("You\n") + social_ui.message_input);
                    social_ui.message_input.clear();
                } else {
                    push_notice(st, ui_model::NoticeLevel::Error, "Message Failed", "The message could not be sent");
                }
            }
        }
        
        card_end();
    }
}

// ---------------------------------------------------------------------------
// Parties Tab
// ---------------------------------------------------------------------------

void draw_social_parties(UiState& st) {
    auto& social_ui = get_social_ui_state();
    auto& account_manager = aml::account::AccountManager::instance();
    
    page_title("Parties", "Create and join parties with friends");
    
    card_begin("##social_parties_header");
    
    ImGui::TextUnformatted("Parties");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - ui_px(200.0f));
    
    if (primary_button("+ Create Party", ImVec2(ui_px(150.0f), ui_px(32.0f)))) {
        social_ui.party_creating = true;
        request_popup("##create_party_popup");
    }
    
    ImGui::Spacing();
    
    // Join party input
    ImGui::SetNextItemWidth(ui_px(240.0f));
    input_text_hint("##social_party_join", "Enter invite code...", &social_ui.party_invite_code);
    ImGui::SameLine();
    
    if (ghost_button("Join", ImVec2(ui_px(80.0f), ui_px(32.0f)))) {
        if (!social_ui.party_invite_code.empty()) {
            if (aml::supabase::SupabaseManager::instance().join_party(social_ui.party_invite_code)) {
                push_notice(st, ui_model::NoticeLevel::Success, "Party Joined",
                            "Successfully joined party");
                social_ui.party_invite_code.clear();
            } else {
                push_notice(st, ui_model::NoticeLevel::Error, "Join Failed", "Invalid or expired invite code");
            }
        }
    }
    
    card_end();
    
    ImGui::Spacing();
    
    // Create party popup
    if (ImGui::BeginPopup("##create_party_popup")) {
        ImGui::TextUnformatted("Create Party");
        ImGui::Separator();
        ImGui::Spacing();
        
        ImGui::TextUnformatted("Party Name");
        ImGui::SetNextItemWidth(ui_px(300.0f));
        ImGui::InputText("##create_party_name", &social_ui.party_name);
        
        ImGui::Spacing();
        
        ImGui::TextUnformatted("Privacy");
        if (ImGui::RadioButton("Public (Anyone can join)", social_ui.is_public))
            social_ui.is_public = true;
        if (ImGui::RadioButton("Private (Invite only)", !social_ui.is_public))
            social_ui.is_public = false;
        
        ImGui::Spacing();
        
        if (primary_button("Create", ImVec2(ui_px(120.0f), ui_px(32.0f)))) {
            if (!social_ui.party_name.empty()) {
                auto party = aml::supabase::SupabaseManager::instance().create_party(
                    social_ui.party_name, social_ui.is_public);
                if (!party.id.empty()) {
                    push_notice(st, ui_model::NoticeLevel::Success, "Party Created",
                                "Party '" + social_ui.party_name + "' created successfully");
                    social_ui.party_name.clear();
                    social_ui.party_creating = false;
                    ImGui::CloseCurrentPopup();
                } else {
                    push_notice(st, ui_model::NoticeLevel::Error, "Party Failed", "The party could not be created");
                }
            }
        }
        
        ImGui::SameLine();
        
        if (ghost_button("Cancel", ImVec2(ui_px(100.0f), ui_px(32.0f)))) {
            social_ui.party_name.clear();
            social_ui.party_creating = false;
            ImGui::CloseCurrentPopup();
        }
        
        ImGui::EndPopup();
    }
    
    std::vector<Party> parties;
    for (const auto& source : aml::supabase::SupabaseManager::instance().get_parties()) {
        Party party;
        party.id = source.id;
        party.name = source.name;
        party.owner_id = source.owner_id;
        party.owner_name = source.owner_username;
        party.max_members = source.max_members;
        party.is_public = source.is_public;
        party.invite_code = source.invite_code;
        party.created_at = source.created_at;
        for (const auto& member : aml::supabase::SupabaseManager::instance().get_party_members(source.id)) {
            party.member_ids.push_back(member.user_id);
            party.member_names.push_back(member.username);
        }
        parties.push_back(std::move(party));
    }
    
    if (parties.empty()) {
        empty_state("No Parties", "Create or join a party to play with friends.", "P");
        return;
    }
    
    // Display parties
    for (auto& party : parties) {
        ImGui::PushID(party.id.c_str());
        
        card_begin(("##social_party_" + party.id).c_str(), ImVec2(-1, 0));
        
        // Party icon tile
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 icon_pos = ImGui::GetCursorScreenPos();
        const float icon_r = ui_px(15.0f);
        dl->AddCircleFilled(icon_pos + ImVec2(icon_r, icon_r), icon_r, c32(k.surface));
        dl->AddCircle(icon_pos + ImVec2(icon_r, icon_r), icon_r,
                      c32(party.is_public ? k.green : k.brand), 0, ui_px(1.0f));
        draw_icon(IconId::Users, icon_pos + ImVec2(icon_r, icon_r), ui_px(9.0f),
                  c32(party.is_public ? k.green : k.brand));
        ImGui::Dummy(ImVec2(icon_r * 2.0f, icon_r * 2.0f));
        ImGui::SameLine(0, ui_px(10.0f));
        
        // Party name + owner
        ImGui::BeginGroup();
        ImGui::PushFont(f_bold);
        ImGui::Text("%s", party.name.c_str());
        ImGui::PopFont();
        ImGui::TextColored(k.muted, "Owner: %s", party.owner_name.c_str());
        ImGui::EndGroup();
        
        ImGui::SameLine(0, ui_px(10.0f));
        // Meta chips
        auto chip = [&](const char* text, const ImVec4& accent) {
            const ImVec2 bp = ImGui::GetCursorScreenPos();
            const ImVec2 sz = ImGui::CalcTextSize(text) + ImVec2(ui_px(12.0f), ui_px(6.0f));
            ImVec4 bg = accent; bg.w = 0.12f;
            dl->AddRectFilled(bp, bp + sz, c32(bg), ui_px(5.0f));
            dl->AddText(bp + ImVec2(ui_px(6.0f), ui_px(3.0f)), c32(accent), text);
            ImGui::Dummy(sz + ImVec2(0, ui_px(4.0f)));
        };
        std::string members_chip = std::to_string(party.member_ids.size()) + "/" +
                                   std::to_string(party.max_members) + " members";
        chip(members_chip.c_str(), k.blue);
        ImGui::SameLine(0, ui_px(6.0f));
        chip(party.is_public ? "Public" : "Private",
             party.is_public ? k.green : k.muted);
        
        ImGui::SameLine(ImGui::GetCursorPosX() +
                        std::max(0.0f, ImGui::GetContentRegionAvail().x - ui_px(200.0f)));
        
        ImGui::BeginGroup();
        
        // Join/Leave button
        bool is_member = std::find(party.member_ids.begin(), party.member_ids.end(), 
                                   account_manager.get_current_session().id) != party.member_ids.end();
        
        if (is_member) {
            if (ghost_button("Leave", ImVec2(ui_px(80.0f), ui_px(28.0f)))) {
                if (aml::supabase::SupabaseManager::instance().leave_party(party.id))
                    push_notice(st, ui_model::NoticeLevel::Success, "Party Left", party.name);
                else
                    push_notice(st, ui_model::NoticeLevel::Error, "Leave Failed", "Could not leave the party");
            }
        } else {
            if (primary_button("Join", ImVec2(ui_px(80.0f), ui_px(28.0f)))) {
                if (aml::supabase::SupabaseManager::instance().join_party(party.invite_code))
                    push_notice(st, ui_model::NoticeLevel::Success, "Party Joined", party.name);
                else
                    push_notice(st, ui_model::NoticeLevel::Error, "Join Failed", "Could not join the party");
            }
        }
        
        ImGui::SameLine();
        
        // More button
        if (icon_button(IconId::More, ImVec2(ui_px(30.0f), ui_px(28.0f)),
                        "More party actions")) {
            ImGui::OpenPopup(("##social_party_menu_" + party.id).c_str());
        }
        
        // Party menu
        if (ImGui::BeginPopup(("##social_party_menu_" + party.id).c_str())) {
            if (ImGui::MenuItem("Copy Invite Code")) {
                if (OpenClipboard(nullptr)) {
                    EmptyClipboard();
                    const SIZE_T bytes = (party.invite_code.size() + 1) * sizeof(char);
                    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
                    if (memory) {
                        void* target = GlobalLock(memory);
                        if (target) {
                            memcpy(target, party.invite_code.c_str(), bytes);
                            GlobalUnlock(memory);
                            SetClipboardData(CF_TEXT, memory);
                        } else {
                            GlobalFree(memory);
                        }
                    }
                    CloseClipboard();
                }
                push_notice(st, ui_model::NoticeLevel::Success, "Invite Code Copied", party.invite_code);
            }
            
            const bool is_owner = party.owner_id == account_manager.get_current_session().id;
            if (ImGui::MenuItem("Disband Party", nullptr, false, is_owner)) {
                if (aml::supabase::SupabaseManager::instance().disband_party(party.id))
                    push_notice(st, ui_model::NoticeLevel::Success, "Party Disbanded", party.name);
                else
                    push_notice(st, ui_model::NoticeLevel::Error, "Disband Failed", "Could not disband the party");
            }
            
            ImGui::EndPopup();
        }
        
        ImGui::EndGroup();
        
        card_end();
        ImGui::PopID();
    }
}

// ---------------------------------------------------------------------------
// Settings Tab
// ---------------------------------------------------------------------------

void draw_social_settings(UiState& st) {
    auto& social_ui = get_social_ui_state();
    
    page_title("Social Settings", "Configure your social preferences");
    
    card_begin("##social_settings_notifications");
    ImGui::TextUnformatted("Notifications");
    ImGui::Separator();
    ImGui::Spacing();
    
    ImGui::Checkbox("Enable Notifications", &social_ui.notifications_enabled);
    ImGui::TextColored(k.muted, "Receive notifications for friend requests, messages, and party invites");
    
    ImGui::Spacing();
    
    ImGui::Checkbox("Show Offline Friends", &social_ui.show_offline_friends);
    ImGui::TextColored(k.muted, "Show offline friends in your friends list");
    
    ImGui::Spacing();
    
    ImGui::Checkbox("Auto-accept Friend Requests", &social_ui.auto_accept_requests);
    ImGui::TextColored(k.muted, "Automatically accept incoming friend requests");
    
    card_end();
    
    ImGui::Spacing();
    
    card_begin("##social_settings_actions");
    ImGui::TextUnformatted("Actions");
    ImGui::Separator();
    ImGui::Spacing();
    
    if (ghost_button("Save Settings", ImVec2(ui_px(150.0f), ui_px(36.0f)))) {
        save_social_settings(*st.cfg);
        if (config::save(st.exe_dir + L"\\launcher.json", *st.cfg)) {
            push_notice(st, ui_model::NoticeLevel::Success, "Settings Saved",
                        "Social settings have been saved");
        } else {
            push_notice(st, ui_model::NoticeLevel::Error, "Save Failed",
                        "Could not write launcher.json");
        }
    }
    
    ImGui::SameLine();
    
    if (ghost_button("Reset to Defaults", ImVec2(ui_px(150.0f), ui_px(36.0f)))) {
        // Reset to defaults
        social_ui.notifications_enabled = true;
        social_ui.show_offline_friends = true;
        social_ui.auto_accept_requests = false;
    }
    
    card_end();
}

// ---------------------------------------------------------------------------
// Main Social Page
// ---------------------------------------------------------------------------

void draw_social_page(UiState& st) {
    auto& social_ui = get_social_ui_state();
    
    page_title("Social", "Connect with friends, chat, and play together");
    
    // Social tabs
    if (ImGui::BeginTabBar("##social_tabs")) {
    
    if (ImGui::BeginTabItem("Friends")) {
        social_ui.current_tab = 0;
        ImGui::EndTabItem();
    }
    
    if (ImGui::BeginTabItem("Messages")) {
        social_ui.current_tab = 1;
        ImGui::EndTabItem();
    }
    
    if (ImGui::BeginTabItem("Parties")) {
        social_ui.current_tab = 2;
        ImGui::EndTabItem();
    }
    
    if (ImGui::BeginTabItem("Settings")) {
        social_ui.current_tab = 3;
        ImGui::EndTabItem();
    }
    
        ImGui::EndTabBar();
    }
    
    ImGui::Spacing();
    
    // Draw current tab
    switch (social_ui.current_tab) {
        case 0:
        default:
            draw_social_friends(st);
            break;
        case 1:
            draw_social_messages(st);
            break;
        case 2:
            draw_social_parties(st);
            break;
        case 3:
            draw_social_settings(st);
            break;
    }
}

// ---------------------------------------------------------------------------
// Helper Functions
// ---------------------------------------------------------------------------

std::string format_time_ago(int64_t timestamp) {
    if (timestamp <= 0) {
        return "Never";
    }
    
    int64_t now = std::time(nullptr);
    int64_t diff = now - timestamp;
    
    if (diff < 60) {
        return "Just now";
    } else if (diff < 3600) {
        int minutes = static_cast<int>(diff / 60);
        return std::to_string(minutes) + " minute" + (minutes > 1 ? "s" : "") + " ago";
    } else if (diff < 86400) {
        int hours = static_cast<int>(diff / 3600);
        return std::to_string(hours) + " hour" + (hours > 1 ? "s" : "") + " ago";
    } else if (diff < 2592000) {
        int days = static_cast<int>(diff / 86400);
        return std::to_string(days) + " day" + (days > 1 ? "s" : "") + " ago";
    } else {
        return format_date(timestamp);
    }
}

}  // namespace aml::ui
