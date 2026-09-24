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
#include <atomic>
#include <chrono>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

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
    bool close_add_friend_popup = false;
    
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
    bool close_create_party_popup = false;
    
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

static std::vector<Party> load_social_parties() {
    std::vector<Party> parties;
    auto& supabase = aml::supabase::SupabaseManager::instance();
    for (const auto& source : supabase.get_parties()) {
        Party party;
        party.id = source.id;
        party.name = source.name;
        party.owner_id = source.owner_id;
        party.owner_name = source.owner_username;
        party.max_members = source.max_members;
        party.is_public = source.is_public;
        party.invite_code = source.invite_code;
        party.created_at = source.created_at;
        for (const auto& member : supabase.get_party_members(source.id)) {
            party.member_ids.push_back(member.user_id);
            party.member_names.push_back(member.username);
        }
        parties.push_back(std::move(party));
    }
    return parties;
}

// The legacy social surface used Supabase getters while ImGui was drawing,
// including a party-members request for every visible party.  Keep a bounded
// read snapshot instead.  It lives for the process so a joined launcher worker
// can never race static destruction during shutdown.
struct SocialRemoteSnapshot {
    std::vector<Friend> friends;
    std::vector<aml::supabase::SupabaseFriendRequest> requests;
    std::vector<Party> parties;
    bool loaded = false;
    bool refreshing = false;
    std::string error;
};

struct SocialRemoteCache {
    std::mutex mu;
    std::atomic_bool refreshing{false};
    uint64_t generation = 0;
    uint64_t refreshed_at_ms = 0;
    bool authenticated = false;
    SocialRemoteSnapshot snapshot;
};

static SocialRemoteCache& get_social_remote_cache() {
    static SocialRemoteCache* cache = new SocialRemoteCache();
    return *cache;
}

static uint64_t social_now_ms() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

static SocialRemoteSnapshot snapshot_social_remote_cache() {
    auto& cache = get_social_remote_cache();
    std::lock_guard<std::mutex> lock(cache.mu);
    SocialRemoteSnapshot snapshot = cache.snapshot;
    snapshot.refreshing = cache.refreshing.load();
    return snapshot;
}

static void mark_social_remote_cache_dirty() {
    auto& cache = get_social_remote_cache();
    std::lock_guard<std::mutex> lock(cache.mu);
    // Invalidate a refresh that began before an account mutation.  Without
    // this generation bump, that stale read can win the race after a successful
    // mutation and suppress the correcting refresh for the normal interval.
    ++cache.generation;
    cache.refreshed_at_ms = 0;
}

static void request_social_remote_cache_refresh(UiState& st, bool force = false) {
    if (st.fixture_mode || st.shutting_down.load()) return;
    auto& cache = get_social_remote_cache();
    const uint64_t now = social_now_ms();
    // Authentication state is local, not a network request.  Clear a prior
    // account's snapshot before rendering if the player signed out, rather
    // than leaving friends or messages visible for the refresh interval.
    const bool authenticated = aml::supabase::SupabaseManager::instance().is_authenticated();
    {
        std::lock_guard<std::mutex> lock(cache.mu);
        if (cache.authenticated != authenticated) {
            cache.authenticated = authenticated;
            ++cache.generation;
            cache.refreshed_at_ms = 0;
            cache.snapshot = {};
        }
        constexpr uint64_t kRefreshIntervalMs = 15000;
        if (!force && cache.refreshed_at_ms != 0 &&
            now - cache.refreshed_at_ms < kRefreshIntervalMs) {
            return;
        }
    }

    bool expected = false;
    if (!cache.refreshing.compare_exchange_strong(expected, true)) return;
    uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(cache.mu);
        generation = ++cache.generation;
    }

    spawn_worker(st, std::thread([&cache, generation] {
        SocialRemoteSnapshot fetched;
        auto& supabase = aml::supabase::SupabaseManager::instance();
        if (!supabase.is_authenticated()) {
            fetched.error = "Sign in to load friends, messages, and parties.";
        } else {
            try {
                fetched.friends = load_social_friends();
                fetched.requests = supabase.get_friend_requests();
                fetched.parties = load_social_parties();
                fetched.loaded = true;
            } catch (const std::exception&) {
                fetched.error = "Social data could not be refreshed. Please retry.";
            } catch (...) {
                fetched.error = "Social data could not be refreshed. Please retry.";
            }
        }
        {
            std::lock_guard<std::mutex> lock(cache.mu);
            if (cache.generation == generation) {
                cache.snapshot = std::move(fetched);
                cache.refreshed_at_ms = social_now_ms();
            }
        }
        cache.refreshing = false;
    }));
}

static bool social_request_is_working(const UiState& st, const std::string& action) {
    const auto snapshot = snapshot_async_ui_request(st.social_async_request);
    return snapshot.working && snapshot.action == action;
}

static bool social_request_lane_busy(const UiState& st) {
    return snapshot_async_ui_request(st.social_async_request).working;
}

static bool social_request_failed(const UiState& st, const std::string& action) {
    const auto snapshot = snapshot_async_ui_request(st.social_async_request);
    return !snapshot.working && snapshot.has_result &&
           snapshot.action == action && !snapshot.result.success;
}

static bool social_action_starts_with(const std::string& action, const char* prefix) {
    const size_t length = std::strlen(prefix);
    return action.size() >= length && action.compare(0, length, prefix) == 0;
}

static void draw_social_request_feedback(const UiState& st, const std::string& action,
                                         const char* working_label) {
    const auto snapshot = snapshot_async_ui_request(st.social_async_request);
    if (snapshot.action != action) return;
    if (snapshot.working) {
        ImGui::TextColored(k.muted, "%s", working_label);
    } else if (snapshot.has_result && !snapshot.result.success &&
               !snapshot.result.detail.empty()) {
        ImGui::TextColored(k.red, "%s", snapshot.result.detail.c_str());
    }
}

static bool start_social_request(UiState& st, const std::string& action,
                                 std::function<AsyncUiRequestResult()> work) {
    uint64_t generation = 0;
    if (!begin_async_ui_request(st.social_async_request, action, &generation)) {
        return false;
    }
    spawn_worker(st, std::thread([&st, action, generation,
                                  work = std::move(work)]() mutable {
        AsyncUiRequestResult result;
        try {
            result = work();
        } catch (const std::exception&) {
            result.success = false;
            result.title = "Social request failed";
            result.detail = "The request ended unexpectedly. Please try again.";
        } catch (...) {
            result.success = false;
            result.title = "Social request failed";
            result.detail = "The request ended unexpectedly. Please try again.";
        }
        if (!st.shutting_down.load()) {
            complete_async_ui_request(st.social_async_request, action,
                                      generation, std::move(result));
        }
    }));
    return true;
}

static void consume_social_request_result(UiState& st) {
    AsyncUiRequestSnapshot completed;
    if (!take_async_ui_request_result(st.social_async_request, &completed)) return;

    auto& social_ui = get_social_ui_state();
    const auto& result = completed.result;
    if (result.success) {
        if (completed.action == "social-conversation-load") {
            // A user may have selected somebody else while this request was
            // in flight.  Never replace their newer conversation with stale
            // messages from the prior selection.
            if (social_ui.selected_friend_id == result.payload_a) {
                social_ui.selected_conversation_id = result.payload_b;
                social_ui.message_history = result.items;
            }
        } else if (completed.action == "social-message-send") {
            if (social_ui.selected_friend_id == result.payload_a &&
                social_ui.selected_conversation_id == result.payload_b) {
                social_ui.message_history.push_back("You\n" + result.payload_c);
                social_ui.message_input.clear();
            }
        } else if (completed.action == "social-add-friend") {
            social_ui.friend_search.clear();
            social_ui.close_add_friend_popup = true;
        } else if (completed.action == "social-party-join-code") {
            if (social_ui.party_invite_code == result.payload_a) {
                social_ui.party_invite_code.clear();
            }
        } else if (completed.action == "social-party-create") {
            social_ui.party_name.clear();
            social_ui.party_creating = false;
            social_ui.close_create_party_popup = true;
        }

        if (social_action_starts_with(completed.action, "social-add-friend") ||
            social_action_starts_with(completed.action, "social-friend-") ||
            social_action_starts_with(completed.action, "social-party-")) {
            mark_social_remote_cache_dirty();
        }
        if (!result.title.empty()) {
            push_notice(st, result.warning ? ui_model::NoticeLevel::Warning
                                            : ui_model::NoticeLevel::Success,
                        result.title, result.detail);
        }
    } else if (!result.title.empty()) {
        push_notice(st, ui_model::NoticeLevel::Error, result.title,
                    result.detail.empty() ? "Please try again." : result.detail);
    }
}

static void draw_social_cache_status(UiState& st, const SocialRemoteSnapshot& snapshot) {
    if (snapshot.refreshing) {
        ImGui::TextColored(k.muted, "Refreshing social data...");
    } else if (!snapshot.error.empty()) {
        ImGui::TextColored(k.red, "%s", snapshot.error.c_str());
        ImGui::SameLine();
        if (ghost_button("Retry", ImVec2(ui_px(72.0f), ui_px(24.0f)))) {
            request_social_remote_cache_refresh(st, true);
        }
    }
}

// ---------------------------------------------------------------------------
// Friends Tab
// ---------------------------------------------------------------------------

void draw_social_friends(UiState& st) {
    if (st.fixture_mode) {
        page_title("Friends", "Local visual fixture");
        ImGui::TextColored(k.brand_hov,
                           "Visual fixture: no account or social service is used.");
        card_begin("##fixture_social_friends");
        ImGui::TextUnformatted("Friends are available after sign-in in the live launcher.");
        card_end();
        return;
    }
    consume_social_request_result(st);
    request_social_remote_cache_refresh(st);
    const auto remote = snapshot_social_remote_cache();
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
    draw_social_cache_status(st, remote);
    
    ImGui::Spacing();
    
    // Add friend popup
    if (ImGui::BeginPopup("##add_friend_popup")) {
        if (social_ui.close_add_friend_popup) {
            social_ui.close_add_friend_popup = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::TextUnformatted("Add Friend");
        ImGui::Separator();
        ImGui::Spacing();
        
        ImGui::TextUnformatted("Username or Email");
        ImGui::SetNextItemWidth(ui_px(300.0f));
        const bool adding_friend = social_request_is_working(st, "social-add-friend");
        const bool social_busy = social_request_lane_busy(st);
        ImGui::BeginDisabled(social_busy);
        ImGui::InputText("##add_friend_input", &social_ui.friend_search);
        ImGui::EndDisabled();
        
        ImGui::Spacing();
        
        const bool retry_add = social_request_failed(st, "social-add-friend");
        if (primary_button(adding_friend ? "Sending..." :
                           (retry_add ? "Retry Request" : "Send Request"),
                           ImVec2(ui_px(120.0f), ui_px(32.0f)), adding_friend, social_busy)) {
            if (!social_ui.friend_search.empty()) {
                const std::string query = social_ui.friend_search;
                start_social_request(st, "social-add-friend", [query] {
                    auto& supabase = aml::supabase::SupabaseManager::instance();
                    const auto users = supabase.search_users(query, 1);
                    AsyncUiRequestResult result;
                    if (users.empty()) {
                        result.title = "User not found";
                        result.detail = "No matching public account was found. Check the name and retry.";
                    } else {
                        result.success = supabase.send_friend_request(users.front().user_id);
                        result.title = result.success ? "Friend request sent" : "Request failed";
                        result.detail = result.success
                            ? "Waiting for the other player to accept."
                            : "The friend request could not be sent. You can retry.";
                    }
                    return result;
                });
            }
        }
        
        ImGui::SameLine();
        
        if (ghost_button("Cancel", ImVec2(ui_px(100.0f), ui_px(32.0f)), social_busy)) {
            social_ui.friend_search.clear();
            ImGui::CloseCurrentPopup();
        }
        draw_social_request_feedback(st, "social-add-friend", "Sending friend request...");
        
        ImGui::EndPopup();
    }
    
    // Friend requests panel
    if (social_ui.friend_requests_open) {
        card_begin("##social_friend_requests");
        ImGui::TextUnformatted("Friend Requests");
        ImGui::Separator();
        ImGui::Spacing();
        
        const auto& requests = remote.requests;
        if (requests.empty()) {
            ImGui::TextColored(k.muted, "No pending friend requests");
        } else {
            for (const auto& request : requests) {
                ImGui::Text("%s", request.sender_username.c_str());
                ImGui::SameLine();
                const std::string accept_action = "social-friend-accept:" + request.id;
                const std::string reject_action = "social-friend-reject:" + request.id;
                const bool request_busy = social_request_lane_busy(st);
                const bool accepting = social_request_is_working(st, accept_action);
                if (primary_button(accepting ? "Accepting..." : "Accept",
                                   ImVec2(ui_px(75.0f), ui_px(25.0f)), accepting,
                                   request_busy)) {
                    const std::string request_id = request.id;
                    const std::string username = request.sender_username;
                    start_social_request(st, accept_action, [request_id, username] {
                        AsyncUiRequestResult result;
                        result.success = aml::supabase::SupabaseManager::instance()
                            .accept_friend_request(request_id);
                        result.title = result.success ? "Friend added" : "Could not accept request";
                        result.detail = result.success
                            ? username + " is now your friend."
                            : "The request remains pending. You can retry.";
                        return result;
                    });
                }
                ImGui::SameLine();
                const bool rejecting = social_request_is_working(st, reject_action);
                if (ghost_button(rejecting ? "Rejecting..." : "Reject",
                                 ImVec2(ui_px(75.0f), ui_px(25.0f)), request_busy)) {
                    const std::string request_id = request.id;
                    start_social_request(st, reject_action, [request_id] {
                        AsyncUiRequestResult result;
                        result.success = aml::supabase::SupabaseManager::instance()
                            .reject_friend_request(request_id);
                        result.title = result.success ? "Friend request rejected" : "Could not reject request";
                        result.detail = result.success
                            ? "The request was declined."
                            : "The request remains pending. You can retry.";
                        return result;
                    });
                }
                draw_social_request_feedback(st, accept_action, "Accepting friend request...");
                draw_social_request_feedback(st, reject_action, "Rejecting friend request...");
            }
        }
        
        card_end();
        ImGui::Spacing();
    }
    
    auto friends = remote.friends;
    
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
            
            const std::string remove_action = "social-friend-remove:" + friend_.id;
            const std::string block_action = "social-friend-block:" + friend_.id;
            const bool friend_action_busy = social_request_lane_busy(st);
            if (ImGui::MenuItem("Remove Friend", nullptr, false, !friend_action_busy)) {
                const std::string friend_id = friend_.id;
                const std::string username = friend_.username;
                start_social_request(st, remove_action, [friend_id, username] {
                    AsyncUiRequestResult result;
                    result.success = aml::supabase::SupabaseManager::instance()
                        .remove_friend(friend_id);
                    result.title = result.success ? "Friend removed" : "Could not remove friend";
                    result.detail = result.success
                        ? username + " has been removed."
                        : "The friend relationship was not changed. You can retry.";
                    return result;
                });
            }

            if (ImGui::MenuItem("Block User", nullptr, false, !friend_action_busy)) {
                const std::string friend_id = friend_.id;
                const std::string username = friend_.username;
                start_social_request(st, block_action, [friend_id, username] {
                    AsyncUiRequestResult result;
                    result.success = aml::supabase::SupabaseManager::instance()
                        .block_user(friend_id);
                    result.title = result.success ? "User blocked" : "Could not block user";
                    result.detail = result.success
                        ? username + " has been blocked."
                        : "The user was not blocked. You can retry.";
                    return result;
                });
            }
            draw_social_request_feedback(st, remove_action, "Removing friend...");
            draw_social_request_feedback(st, block_action, "Blocking user...");
            
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

// Visual-review captures must never hydrate a real account or poll the social
// service.  These compact, representative facades also keep the Messages and
// Parties routes useful in a fresh fixture process, where there is deliberately
// no signed-in user.
static void draw_fixture_social_messages(UiState&) {
    static std::string fixture_draft;
    page_title("Messages", "A representative local conversation layout");
    ImGui::TextColored(k.brand_hov,
                       "Visual fixture: local sample data only; no account or message service is used.");
    ImGui::Spacing();

    const bool narrow = ImGui::GetContentRegionAvail().x < ui_px(760.0f);
    ImGui::BeginChild("##fixture_social_friend_list",
                      ImVec2(narrow ? -1.0f : ui_px(250.0f),
                             narrow ? ui_px(168.0f) : ui_px(322.0f)), true);
    ImGui::PushFont(f_bold);
    ImGui::TextUnformatted("Friends");
    ImGui::PopFont();
    ImGui::TextColored(k.green, "2 online");
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::PushFont(f_bold);
    ImGui::TextUnformatted("AveryStone");
    ImGui::PopFont();
    ImGui::TextColored(k.green, "Online - Lobby");
    ImGui::Spacing();
    ImGui::PushFont(f_bold);
    ImGui::TextUnformatted("MiraBuilds");
    ImGui::PopFont();
    ImGui::TextColored(k.green, "Online - Survival");
    ImGui::Spacing();
    ImGui::TextColored(k.muted, "NoraRedstone - Offline");
    ImGui::EndChild();

    if (!narrow) ImGui::SameLine();
    else ImGui::Spacing();

    ImGui::BeginChild("##fixture_social_message_area",
                      ImVec2(-1, narrow ? ui_px(248.0f) : ui_px(322.0f)), true);
    ImGui::PushFont(f_bold);
    ImGui::TextUnformatted("AveryStone");
    ImGui::PopFont();
    ImGui::TextColored(k.green, "Online now");
    ImGui::Separator();
    ImGui::Spacing();
    card_begin("##fixture_social_message_inbound");
    ImGui::TextColored(k.brand, "AveryStone");
    ImGui::TextWrapped("The new profile is ready. Want to test the world seed together?");
    card_end();
    ImGui::Spacing();
    card_begin("##fixture_social_message_outbound");
    ImGui::TextColored(k.brand_hov, "You");
    ImGui::TextWrapped("Absolutely - I will join after this visual review pass.");
    card_end();
    ImGui::EndChild();

    ImGui::Spacing();
    card_begin("##fixture_social_message_input");
    ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(-ui_px(94.0f));
    ImGui::InputTextWithHint("##fixture_social_message_draft", "Messaging is disabled in visual fixtures",
                             &fixture_draft);
    ImGui::SameLine();
    primary_button("Send", ImVec2(ui_px(80.0f), ui_px(32.0f)), false, true);
    ImGui::EndDisabled();
    card_end();
}

void draw_social_messages(UiState& st) {
    if (st.fixture_mode) {
        draw_fixture_social_messages(st);
        return;
    }
    consume_social_request_result(st);
    request_social_remote_cache_refresh(st);
    const auto remote = snapshot_social_remote_cache();
    auto& social_ui = get_social_ui_state();
    
    page_title("Messages", "Chat with your friends");
    draw_social_cache_status(st, remote);
    draw_social_request_feedback(st, "social-conversation-load", "Loading conversation...");
    
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
    
    auto friends = remote.friends;
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
        const bool retry_selected_friend =
            social_request_failed(st, "social-conversation-load") &&
            social_ui.selected_friend_id == friend_.id;
        if (ImGui::IsItemClicked() && !social_request_lane_busy(st) &&
            (social_ui.selected_friend_id != friend_.id || retry_selected_friend)) {
            social_ui.selected_friend_id = friend_.id;
            social_ui.message_history.clear();
            social_ui.selected_conversation_id.clear();
            const std::string friend_id = friend_.id;
            start_social_request(st, "social-conversation-load", [friend_id] {
                auto& supabase = aml::supabase::SupabaseManager::instance();
                const auto conversation = supabase.get_or_create_conversation(friend_id);
                AsyncUiRequestResult result;
                if (conversation.id.empty()) {
                    result.title = "Could not open conversation";
                    result.detail = "The conversation is unavailable. You can select the friend again to retry.";
                    return result;
                }
                result.success = true;
                result.payload_a = friend_id;
                result.payload_b = conversation.id;
                for (const auto& message : supabase.get_messages(conversation.id)) {
                    result.items.push_back(message.sender_username + "\n" + message.content);
                }
                return result;
            });
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
        
        const std::string player_name = player_display_name(st);
        const char* self_name = player_name.empty() ? "You" : player_name.c_str();
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
        
        if (social_request_is_working(st, "social-conversation-load")) {
            ImGui::TextColored(k.muted, "Loading messages...");
        } else if (social_ui.message_history.empty()) {
            ImGui::TextColored(k.muted, "No messages yet. Start a conversation!");
        }
        
        ImGui::EndChild();
    }
    
    ImGui::EndChild();
    
    // Message input
    if (!social_ui.selected_friend_id.empty()) {
        ImGui::Spacing();
        
        card_begin("##social_message_input");
        const bool sending_message = social_request_is_working(st, "social-message-send");
        const bool messaging_busy = social_request_lane_busy(st);
        const bool retry_message = social_request_failed(st, "social-message-send");
        ImGui::SetNextItemWidth(-ui_px(100.0f));
        ImGui::BeginDisabled(messaging_busy || social_ui.selected_conversation_id.empty());
        ImGui::InputText("##social_message_input_text", &social_ui.message_input,
                       ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::EndDisabled();
        
        ImGui::SameLine();
        
        if (primary_button(sending_message ? "Sending..." :
                           (retry_message ? "Retry" : "Send"),
                           ImVec2(ui_px(80.0f), ui_px(32.0f)), sending_message,
                           messaging_busy || social_ui.selected_conversation_id.empty())) {
            if (!social_ui.message_input.empty()) {
                const std::string friend_id = social_ui.selected_friend_id;
                const std::string conversation_id = social_ui.selected_conversation_id;
                const std::string content = social_ui.message_input;
                start_social_request(st, "social-message-send",
                    [friend_id, conversation_id, content] {
                        AsyncUiRequestResult result;
                        const auto sent = aml::supabase::SupabaseManager::instance()
                            .send_message(conversation_id, content);
                        result.success = !sent.id.empty();
                        result.title = result.success ? "" : "Message failed";
                        result.detail = result.success ? ""
                            : "The message was not sent. You can retry.";
                        result.payload_a = friend_id;
                        result.payload_b = conversation_id;
                        result.payload_c = content;
                        return result;
                    });
            }
        }
        draw_social_request_feedback(st, "social-message-send", "Sending message...");
        
        card_end();
    }
}

// ---------------------------------------------------------------------------
// Parties Tab
// ---------------------------------------------------------------------------

static void draw_fixture_social_parties(UiState&) {
    static std::string fixture_invite_code;
    page_title("Parties", "Representative local party availability");
    ImGui::TextColored(k.brand_hov,
                       "Visual fixture: local sample data only; no account or party service is used.");
    ImGui::Spacing();

    card_begin("##fixture_social_parties_header");
    ImGui::PushFont(f_bold);
    ImGui::TextUnformatted("Parties");
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::TextColored(k.muted, "2 available");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - ui_px(150.0f));
    ImGui::BeginDisabled();
    primary_button("+ Create Party", ImVec2(ui_px(140.0f), ui_px(32.0f)), false, true);
    ImGui::EndDisabled();
    ImGui::Spacing();
    ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(ui_px(240.0f));
    ImGui::InputTextWithHint("##fixture_social_party_code", "Invite code", &fixture_invite_code);
    ImGui::SameLine();
    ghost_button("Join", ImVec2(ui_px(80.0f), ui_px(32.0f)));
    ImGui::EndDisabled();
    card_end();

    struct FixtureParty { const char* name; const char* owner; const char* members; const char* privacy; };
    static constexpr FixtureParty kParties[] = {
        {"Redstone Builders", "AveryStone", "3 / 8 members", "Public"},
        {"Weekend Survival", "MiraBuilds", "2 / 6 members", "Invite only"},
    };
    for (const auto& party : kParties) {
        ImGui::Spacing();
        card_begin((std::string("##fixture_social_party_") + party.name).c_str());
        ImGui::PushFont(f_bold);
        ImGui::TextUnformatted(party.name);
        ImGui::PopFont();
        ImGui::TextColored(k.muted, "Owner: %s", party.owner);
        ImGui::TextColored(k.blue, "%s", party.members);
        ImGui::SameLine();
        ImGui::TextColored(std::string(party.privacy) == "Public" ? k.green : k.muted,
                           "%s", party.privacy);
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - ui_px(82.0f));
        ImGui::BeginDisabled();
        primary_button("Join", ImVec2(ui_px(72.0f), ui_px(28.0f)), false, true);
        ImGui::EndDisabled();
        card_end();
    }
}

void draw_social_parties(UiState& st) {
    if (st.fixture_mode) {
        draw_fixture_social_parties(st);
        return;
    }
    consume_social_request_result(st);
    request_social_remote_cache_refresh(st);
    const auto remote = snapshot_social_remote_cache();
    auto& social_ui = get_social_ui_state();
    auto& account_manager = aml::account::AccountManager::instance();
    
    page_title("Parties", "Create and join parties with friends");
    draw_social_cache_status(st, remote);
    
    card_begin("##social_parties_header");
    
    ImGui::TextUnformatted("Parties");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - ui_px(200.0f));
    
    const bool social_action_busy = social_request_lane_busy(st);
    if (primary_button("+ Create Party", ImVec2(ui_px(150.0f), ui_px(32.0f)), false,
                       social_action_busy)) {
        social_ui.party_creating = true;
        request_popup("##create_party_popup");
    }
    
    ImGui::Spacing();
    
    // Join party input
    ImGui::SetNextItemWidth(ui_px(240.0f));
    ImGui::BeginDisabled(social_action_busy);
    input_text_hint("##social_party_join", "Enter invite code...", &social_ui.party_invite_code);
    ImGui::EndDisabled();
    ImGui::SameLine();
    
    const bool joining_by_code = social_request_is_working(st, "social-party-join-code");
    const bool retry_join_by_code = social_request_failed(st, "social-party-join-code");
    if (ghost_button(joining_by_code ? "Joining..." :
                     (retry_join_by_code ? "Retry" : "Join"),
                     ImVec2(ui_px(80.0f), ui_px(32.0f)), social_action_busy)) {
        if (!social_ui.party_invite_code.empty()) {
            const std::string invite_code = social_ui.party_invite_code;
            start_social_request(st, "social-party-join-code", [invite_code] {
                AsyncUiRequestResult result;
                result.success = aml::supabase::SupabaseManager::instance().join_party(invite_code);
                result.title = result.success ? "Party joined" : "Could not join party";
                result.detail = result.success
                    ? "You are now in the party."
                    : "The invite code is invalid or expired. You can retry.";
                result.payload_a = invite_code;
                return result;
            });
        }
    }
    draw_social_request_feedback(st, "social-party-join-code", "Joining party...");
    
    card_end();
    
    ImGui::Spacing();
    
    // Create party popup
    if (ImGui::BeginPopup("##create_party_popup")) {
        if (social_ui.close_create_party_popup) {
            social_ui.close_create_party_popup = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::TextUnformatted("Create Party");
        ImGui::Separator();
        ImGui::Spacing();
        
        ImGui::TextUnformatted("Party Name");
        ImGui::SetNextItemWidth(ui_px(300.0f));
        ImGui::BeginDisabled(social_action_busy);
        ImGui::InputText("##create_party_name", &social_ui.party_name);
        
        ImGui::Spacing();
        
        ImGui::TextUnformatted("Privacy");
        if (ImGui::RadioButton("Public (Anyone can join)", social_ui.is_public))
            social_ui.is_public = true;
        if (ImGui::RadioButton("Private (Invite only)", !social_ui.is_public))
            social_ui.is_public = false;
        ImGui::EndDisabled();
        
        ImGui::Spacing();
        
        const bool creating_party = social_request_is_working(st, "social-party-create");
        const bool retry_create = social_request_failed(st, "social-party-create");
        if (primary_button(creating_party ? "Creating..." :
                           (retry_create ? "Retry Create" : "Create"),
                           ImVec2(ui_px(120.0f), ui_px(32.0f)), creating_party,
                           social_action_busy)) {
            if (!social_ui.party_name.empty()) {
                const std::string party_name = social_ui.party_name;
                const bool is_public = social_ui.is_public;
                start_social_request(st, "social-party-create", [party_name, is_public] {
                    AsyncUiRequestResult result;
                    const auto party = aml::supabase::SupabaseManager::instance()
                        .create_party(party_name, is_public);
                    result.success = !party.id.empty();
                    result.title = result.success ? "Party created" : "Could not create party";
                    result.detail = result.success
                        ? "Party '" + party_name + "' is ready for friends."
                        : "The party was not created. You can retry.";
                    return result;
                });
            }
        }
        
        ImGui::SameLine();
        
        if (ghost_button("Cancel", ImVec2(ui_px(100.0f), ui_px(32.0f)), social_action_busy)) {
            social_ui.party_name.clear();
            social_ui.party_creating = false;
            ImGui::CloseCurrentPopup();
        }
        draw_social_request_feedback(st, "social-party-create", "Creating party...");
        
        ImGui::EndPopup();
    }
    
    const auto& parties = remote.parties;
    
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
        const std::string leave_action = "social-party-leave:" + party.id;
        const std::string join_action = "social-party-join:" + party.id;
        const std::string disband_action = "social-party-disband:" + party.id;
        const bool party_action_busy = social_request_lane_busy(st);
        
        if (is_member) {
            const bool leaving = social_request_is_working(st, leave_action);
            const bool retry_leave = social_request_failed(st, leave_action);
            if (ghost_button(leaving ? "Leaving..." : (retry_leave ? "Retry" : "Leave"),
                             ImVec2(ui_px(80.0f), ui_px(28.0f)), party_action_busy)) {
                const std::string party_id = party.id;
                const std::string party_name = party.name;
                start_social_request(st, leave_action, [party_id, party_name] {
                    AsyncUiRequestResult result;
                    result.success = aml::supabase::SupabaseManager::instance().leave_party(party_id);
                    result.title = result.success ? "Party left" : "Could not leave party";
                    result.detail = result.success
                        ? party_name + " is no longer in your party list."
                        : "You are still in the party. You can retry.";
                    return result;
                });
            }
        } else {
            const bool joining = social_request_is_working(st, join_action);
            const bool retry_join = social_request_failed(st, join_action);
            if (primary_button(joining ? "Joining..." : (retry_join ? "Retry" : "Join"),
                               ImVec2(ui_px(80.0f), ui_px(28.0f)), joining,
                               party_action_busy)) {
                const std::string invite_code = party.invite_code;
                const std::string party_name = party.name;
                start_social_request(st, join_action, [invite_code, party_name] {
                    AsyncUiRequestResult result;
                    result.success = aml::supabase::SupabaseManager::instance().join_party(invite_code);
                    result.title = result.success ? "Party joined" : "Could not join party";
                    result.detail = result.success
                        ? "You joined " + party_name + "."
                        : "The party could not be joined. You can retry.";
                    return result;
                });
            }
        }
        draw_social_request_feedback(st, leave_action, "Leaving party...");
        draw_social_request_feedback(st, join_action, "Joining party...");
        
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
            if (ImGui::MenuItem("Disband Party", nullptr, false,
                                is_owner && !party_action_busy)) {
                const std::string party_id = party.id;
                const std::string party_name = party.name;
                start_social_request(st, disband_action, [party_id, party_name] {
                    AsyncUiRequestResult result;
                    result.success = aml::supabase::SupabaseManager::instance().disband_party(party_id);
                    result.title = result.success ? "Party disbanded" : "Could not disband party";
                    result.detail = result.success
                        ? party_name + " has been disbanded."
                        : "The party is still active. You can retry.";
                    return result;
                });
            }
            draw_social_request_feedback(st, disband_action, "Disbanding party...");
            
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
