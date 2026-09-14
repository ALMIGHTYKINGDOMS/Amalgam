#include "ui.h"
#include "ui_internal.h"
#include "supabase.h"
#include "account_manager.h"
#include "server_manager.h"
#include "storage_manager.h"
#include "sync_manager.h"
#include "admin_auth.h"
#include "net.h"
#include "project_publishing.h"

#include <windows.h>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <ctime>

namespace aml::ui {

// ---------------------------------------------------------------------------
// Admin UI State
// ---------------------------------------------------------------------------

struct AdminUIState {
    int current_tab = 0; // 0=dashboard, 1=users, 2=servers, 3=nodes, 4=storage, 5=settings, 6=moderation
    
    // Dashboard
    int64_t last_refresh = 0;
    
    // Users
    std::string user_filter;
    int user_sort = 0; // 0=username, 1=email, 2=created, 3=last login
    std::string selected_user_id;
    bool user_editing = false;
    
    // Servers
    std::string server_filter;
    int server_sort = 0; // 0=name, 1=status, 2=created, 3=players
    std::string selected_server_id;
    
    // Nodes
    std::string node_filter;
    int node_sort = 0; // 0=name, 1=status, 2=cpu, 3=memory
    std::string selected_node_id;
    
    // Storage
    std::string storage_bucket_filter;
    
    // Settings
    bool maintenance_mode = false;
    std::string maintenance_message;
    int max_servers_per_user = 10;
    int max_nodes_per_user = 5;
    uint64_t max_storage_per_user = 1024 * 1024 * 1024; // 1GB
    std::string moderation_reason;
};

static AdminUIState& get_admin_ui_state() {
    static AdminUIState state;
    return state;
}

static int64_t activity_timestamp(const Json& row) {
    for (const char* key : {"timestamp", "occurred_at", "created_at"}) {
        const int64_t value = row.get(key).as_int();
        if (value > 0) return value;
    }
    return 0;
}

static std::string activity_text(const Json& row) {
    std::string text = row.get("description").as_str();
    if (text.empty()) text = row.get("message").as_str();
    if (text.empty()) text = row.get("action").as_str();
    if (text.empty()) text = row.get("type").as_str();
    return text;
}

static bool has_authoritative_metrics(const aml::servers::Node& node) {
    const auto it = node.metadata.find("metrics_status");
    return it != node.metadata.end() && it->second == "available";
}

static void draw_admin_moderation(UiState& st) {
    auto& admin = get_admin_ui_state();
    auto* client = aml::supabase::SupabaseManager::instance().client();
    page_title("Project Moderation", "Review creator projects before their first public release.");
    if (!client || !client->is_authenticated()) {
        empty_state("Sign in required", "Staff authentication is required to load the review queue.", "@");
        return;
    }
    card_begin("##moderation_header");
    ImGui::TextUnformatted("Safety Review Queue");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - ui_px(100.0f));
    if (ghost_button("Refresh", ImVec2(ui_px(90.0f), ui_px(30.0f)))) {}
    ImGui::InputTextMultiline("Review reason", &admin.moderation_reason, ImVec2(-1, ui_px(55.0f)));
    card_end();
    ImGui::Spacing();

    std::vector<aml::publishing::Project> queue;
    std::string error;
    if (!aml::publishing::list_moderation_queue(*client, queue, &error)) {
        ImGui::TextColored(k.red, "%s", error.c_str());
        return;
    }
    if (queue.empty()) {
        empty_state("Queue is clear", "No projects are waiting for first publication review.", "✓");
        return;
    }
    for (const auto& project : queue) {
        ImGui::PushID(project.id.c_str());
        card_begin("##moderation_project", ImVec2(-1, ui_px(145.0f)));
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted(project.name.c_str());
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::TextColored(k.muted, "(%s)", project.type.c_str());
        ImGui::TextColored(k.muted, "Owner: %s", project.owner_id.c_str());
        ImGui::TextWrapped("%s", project.description.c_str());
        if (primary_button("Approve", ImVec2(ui_px(100.0f), ui_px(30.0f)))) {
            std::string reason = admin.moderation_reason;
            if (aml::publishing::review_project(*client, project.id, "approve", reason, &error))
                push_notice(st, ui_model::NoticeLevel::Success, "Project Approved", "Future versions may publish without repeat review");
            else push_notice(st, ui_model::NoticeLevel::Error, "Approval Failed", error);
        }
        ImGui::SameLine();
        if (ghost_button("Reject", ImVec2(ui_px(100.0f), ui_px(30.0f)))) {
            std::string reason = admin.moderation_reason;
            if (reason.empty()) reason = "Safety review requires changes";
            if (aml::publishing::review_project(*client, project.id, "reject", reason, &error))
                push_notice(st, ui_model::NoticeLevel::Info, "Project Rejected", "The creator can revise and resubmit");
            else push_notice(st, ui_model::NoticeLevel::Error, "Rejection Failed", error);
        }
        ImGui::SameLine();
        if (ghost_button("Take Down", ImVec2(ui_px(100.0f), ui_px(30.0f)))) {
            std::string reason = admin.moderation_reason;
            if (reason.empty()) reason = "Removed by staff for safety reasons";
            if (aml::publishing::review_project(*client, project.id, "takedown", reason, &error))
                push_notice(st, ui_model::NoticeLevel::Warning, "Project Unpublished", "The project is no longer publicly listed");
            else push_notice(st, ui_model::NoticeLevel::Error, "Takedown Failed", error);
        }
        card_end();
        ImGui::PopID();
        ImGui::Spacing();
    }
}

static void draw_admin_feedback(UiState& st) {
    auto* client = aml::supabase::SupabaseManager::instance().client();
    page_title("Beta Feedback", "Review feedback submitted by beta testers.");
    if (!client || !client->is_authenticated()) {
        empty_state("Sign in required", "Staff authentication is required to review feedback.", "@");
        return;
    }
    card_begin("##admin_feedback_header");
    ImGui::TextUnformatted("Tester feedback");
    ImGui::TextColored(k.muted, "Feedback is collected without passwords, tokens, or raw logs.");
    card_end();
    ImGui::Spacing();

    aml::supabase::SupabaseClient::DBQueryOptions query;
    query.table = "beta_feedback";
    query.select = "id,user_id,category,rating,message,page,app_version,status,created_at";
    query.order_by = "created_at";
    query.order_asc = false;
    query.limit = 100;
    const auto result = client->select(query);
    if (!result.success) {
        ImGui::TextColored(k.red, "Feedback could not be loaded: %s", result.error.c_str());
        return;
    }
    if (result.data.empty()) {
        empty_state("No feedback yet", "Submitted beta feedback will appear here.", "✓");
        return;
    }
    if (ImGui::BeginTable("##admin_feedback_table", 6,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY,
                          ImVec2(0, ui_px(520.0f)))) {
        ImGui::TableSetupColumn("Category", 0, 0.12f);
        ImGui::TableSetupColumn("Rating", 0, 0.08f);
        ImGui::TableSetupColumn("Message", 0, 0.42f);
        ImGui::TableSetupColumn("Page", 0, 0.14f);
        ImGui::TableSetupColumn("Version", 0, 0.12f);
        ImGui::TableSetupColumn("Status", 0, 0.12f);
        ImGui::TableHeadersRow();
        for (const auto& row : result.data) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(row.get("category").as_str().c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%d/5", static_cast<int>(row.get("rating").as_int()));
            ImGui::TableSetColumnIndex(2);
            ImGui::TextWrapped("%s", row.get("message").as_str().c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::TextColored(k.muted, "%s", row.get("page").as_str().c_str());
            ImGui::TableSetColumnIndex(4);
            ImGui::TextColored(k.muted, "%s", row.get("app_version").as_str().c_str());
            ImGui::TableSetColumnIndex(5);
            ImGui::TextColored(k.yellow, "%s", row.get("status").as_str("open").c_str());
        }
        ImGui::EndTable();
    }
    (void)st;
}

// ---------------------------------------------------------------------------
// Admin Dashboard
// ---------------------------------------------------------------------------

void draw_admin_dashboard(UiState&) {
    auto& admin_ui = get_admin_ui_state();
    auto& supabase = aml::supabase::SupabaseManager::instance();
    auto& server_manager = aml::servers::ServerManager::instance();
    
    page_title("Admin Dashboard", "Manage Amalgam launcher users, servers, and resources");
    
    // Stats cards
    ImGui::BeginGroup();
    
    // Users card
    card_begin("##admin_stat_users");
    ImGui::TextUnformatted("Users");
    ImGui::PushFont(f_title);
    
    int user_count = 0;
    if (auto* client = supabase.client(); client && client->is_authenticated()) {
        const auto result = client->rpc("admin_list_users");
        if (result.success) user_count = result.count > 0 ? result.count : static_cast<int>(result.data.size());
    }
    
    ImGui::Text("%d", user_count);
    ImGui::PopFont();
    ImGui::TextColored(k.muted, "Total registered users");
    card_end();
    
    ImGui::EndGroup();
    ImGui::SameLine();
    
    ImGui::BeginGroup();
    
    // Servers card
    card_begin("##admin_stat_servers");
    ImGui::TextUnformatted("Servers");
    ImGui::PushFont(f_title);
    
    auto server_stats = server_manager.get_stats();
    ImGui::Text("%d", server_stats.total_servers);
    ImGui::PopFont();
    ImGui::TextColored(k.muted, "%d running", server_stats.running);
    card_end();
    
    ImGui::EndGroup();
    ImGui::SameLine();
    
    ImGui::BeginGroup();
    
    // Nodes card
    card_begin("##admin_stat_nodes");
    ImGui::TextUnformatted("Nodes");
    ImGui::PushFont(f_title);
    
    auto node_stats = server_manager.get_node_stats();
    ImGui::Text("%d", node_stats.total_nodes);
    ImGui::PopFont();
    ImGui::TextColored(k.muted, "%d online", node_stats.online);
    card_end();
    
    ImGui::EndGroup();
    ImGui::SameLine();
    
    ImGui::BeginGroup();
    
    // Storage card
    card_begin("##admin_stat_storage");
    ImGui::TextUnformatted("Storage");
    ImGui::PushFont(f_title);
    
    auto storage_stats = aml::storage::StorageManager::instance().get_stats();
    ImGui::Text("%s", format_bytes(storage_stats.total_size_bytes).c_str());
    ImGui::PopFont();
    ImGui::TextColored(k.muted, "%d files", storage_stats.total_files);
    card_end();
    
    ImGui::EndGroup();
    
    ImGui::Spacing();
    
    // System status
    card_begin("##admin_system_status");
    ImGui::TextUnformatted("System Status");
    ImGui::Separator();
    ImGui::Spacing();
    
    // Check system health
    auto sync_stats = aml::sync::SyncManager::instance().get_stats();
    
    ImGui::TextUnformatted("Sync Status");
    if (sync_stats.is_online) {
        ImGui::TextColored(k.green, "✓ Online");
    } else {
        ImGui::TextColored(k.red, "✗ Offline");
    }
    ImGui::SameLine();
    ImGui::TextColored(k.muted, " - Last sync: %s", 
                     sync_stats.last_sync_time > 0 ? 
                     format_date(sync_stats.last_sync_time).c_str() : "Never");
    
    ImGui::Spacing();
    
    ImGui::TextUnformatted("Database Connection");
    if (supabase.is_authenticated()) {
        ImGui::TextColored(k.green, "✓ Connected");
    } else {
        ImGui::TextColored(k.red, "✗ Disconnected");
    }
    
    ImGui::Spacing();
    
    if (ghost_button("Refresh Stats", ImVec2(ui_px(120.0f), ui_px(32.0f)))) {
        admin_ui.last_refresh = std::time(nullptr);
    }
    
    card_end();
    
    ImGui::Spacing();
    
    // Recent activity
    card_begin("##admin_recent_activity");
    ImGui::TextUnformatted("Recent Activity");
    ImGui::Separator();
    ImGui::Spacing();
    
    std::vector<Json> activity_rows;
    std::string activity_error;
    if (auto* client = supabase.client(); client && client->is_authenticated()) {
        aml::supabase::SupabaseClient::DBQueryOptions query;
        query.table = "account_activity";
        query.order_by = "created_at";
        query.order_asc = false;
        query.limit = 10;
        const auto result = client->select(query);
        if (result.success) activity_rows = result.data;
        else activity_error = result.error;
    } else {
        activity_error = "Staff authentication is required to load activity.";
    }

    int shown_activity = 0;
    if (!activity_rows.empty()) {
        for (const auto& row : activity_rows) {
            const int64_t timestamp = activity_timestamp(row);
            const std::string text = activity_text(row);
            if (timestamp <= 0 || text.empty()) continue;
            std::string actor = row.get("username").as_str();
            if (actor.empty()) actor = row.get("email").as_str();
            if (actor.empty()) actor = row.get("user_id").as_str();
            ImGui::TextColored(k.muted, "%s", format_date(timestamp).c_str());
            ImGui::SameLine();
            ImGui::TextWrapped("%s%s%s", actor.empty() ? "" : actor.c_str(),
                               actor.empty() ? "" : ": ", text.c_str());
            ++shown_activity;
        }
    }
    if (shown_activity == 0) {
        ImGui::TextColored(k.muted, "%s", activity_error.empty()
                           ? "No activity has been recorded."
                           : "Activity is unavailable from the configured Supabase project.");
    }
    
    card_end();
}

// ---------------------------------------------------------------------------
// Admin Users Tab
// ---------------------------------------------------------------------------

void draw_admin_users(UiState&) {
    auto& admin_ui = get_admin_ui_state();
    auto& supabase = aml::supabase::SupabaseManager::instance();
    
    page_title("User Management", "Manage Amalgam launcher users");
    
    card_begin("##admin_users_header");
    
    ImGui::TextUnformatted("Users");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - ui_px(200.0f));
    
    ImGui::TextColored(k.muted, "User invitations are managed by Supabase Auth.");
    
    ImGui::Spacing();
    
    // Filter and sort
    ImGui::SetNextItemWidth(ui_px(240.0f));
    input_text_hint("##admin_user_filter", "Filter users...", &admin_ui.user_filter);
    ImGui::SameLine();
    
    ImGui::SetNextItemWidth(ui_px(160.0f));
    const char* sort_options[] = {"Username (A-Z)", "Email (A-Z)", "Recently Created", "Recently Active"};
    if (ImGui::BeginCombo("##admin_user_sort", sort_options[admin_ui.user_sort])) {
        for (int i = 0; i < 4; ++i) {
            if (ImGui::Selectable(sort_options[i], admin_ui.user_sort == i)) {
                admin_ui.user_sort = i;
            }
        }
        ImGui::EndCombo();
    }
    
    card_end();
    
    ImGui::Spacing();
    
    // User list
    std::vector<aml::supabase::SupabaseUser> users;
    if (auto* client = supabase.client(); client && client->is_authenticated()) {
        const auto result = client->rpc("admin_list_users");
        if (result.success) {
            for (const auto& row : result.data) {
                aml::supabase::SupabaseUser user;
                user.id = row.get("id").as_str();
                user.email = row.get("email").as_str();
                user.username = row.get("username").as_str();
                user.display_name = row.get("display_name").as_str();
                user.avatar_url = row.get("avatar_url").as_str();
                user.created_at = row.get("created_at").as_int();
                user.updated_at = row.get("updated_at").as_int();
                user.last_login_at = row.get("last_login_at").as_int();
                users.push_back(std::move(user));
            }
        }
    }
    if (users.empty()) {
        empty_state("No Users", "No users have registered yet.", "U");
        return;
    }
    
    // Filter users
    auto filtered_users = users;
    if (!admin_ui.user_filter.empty()) {
        std::string filter = admin_ui.user_filter;
        std::transform(filter.begin(), filter.end(), filter.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        
        filtered_users.erase(std::remove_if(filtered_users.begin(), filtered_users.end(),
            [&filter](const auto& user) {
                std::string username = user.username;
                std::string email = user.email;
                std::transform(username.begin(), username.end(), username.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                std::transform(email.begin(), email.end(), email.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                return username.find(filter) == std::string::npos && 
                       email.find(filter) == std::string::npos;
            }), filtered_users.end());
    }
    
    // Sort users
    std::sort(filtered_users.begin(), filtered_users.end(),
        [&admin_ui](const auto& a, const auto& b) {
            switch (admin_ui.user_sort) {
                case 1: return a.email < b.email; // Email (A-Z)
                case 2: return a.created_at > b.created_at; // Recently Created
                case 3: return a.last_login_at > b.last_login_at; // Recently Active
                default: return a.username < b.username; // Username (A-Z)
            }
        });
    
    // Display users
    for (auto& user : filtered_users) {
        ImGui::PushID(user.id.c_str());
        
        card_begin(("##admin_user_" + user.id).c_str(), ImVec2(-1, ui_px(80.0f)));
        
        ImGui::BeginGroup();
        
        // User info
        ImGui::Text("%s", user.username.c_str());
        ImGui::TextColored(k.muted, "%s", user.email.c_str());
        
        // User stats
        ImGui::TextColored(k.muted, "Created: %s", format_date(user.created_at).c_str());
        ImGui::TextColored(k.muted, "Last Login: %s", format_date(user.last_login_at).c_str());
        
        ImGui::EndGroup();
        
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - ui_px(200.0f));
        
        ImGui::BeginGroup();
        
        ImGui::TextColored(k.muted, "Profile edits are self-service");
        
        ImGui::SameLine();
        
        ImGui::TextColored(k.muted, "Account deletion requires an audited Auth operation");
        
        ImGui::EndGroup();
        
        card_end();
        ImGui::PopID();
    }
}

// ---------------------------------------------------------------------------
// Admin Servers Tab
// ---------------------------------------------------------------------------

void draw_admin_servers(UiState& st) {
    auto& admin_ui = get_admin_ui_state();
    auto& server_manager = aml::servers::ServerManager::instance();
    
    page_title("Server Management", "Manage all user servers");
    
    card_begin("##admin_servers_header");
    
    ImGui::TextUnformatted("Servers");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - ui_px(200.0f));
    
    ImGui::TextColored(k.muted, "Server creation is temporarily unavailable.");
    
    ImGui::Spacing();
    
    // Filter and sort
    ImGui::SetNextItemWidth(ui_px(240.0f));
    input_text_hint("##admin_server_filter", "Filter servers...", &admin_ui.server_filter);
    ImGui::SameLine();
    
    ImGui::SetNextItemWidth(ui_px(160.0f));
    const char* sort_options[] = {"Name (A-Z)", "Status", "Recently Created", "Most Players"};
    if (ImGui::BeginCombo("##admin_server_sort", sort_options[admin_ui.server_sort])) {
        for (int i = 0; i < 4; ++i) {
            if (ImGui::Selectable(sort_options[i], admin_ui.server_sort == i)) {
                admin_ui.server_sort = i;
            }
        }
        ImGui::EndCombo();
    }
    
    card_end();
    
    ImGui::Spacing();
    
    // Get all servers
    auto servers = server_manager.get_servers();
    
    if (servers.empty()) {
        empty_state("No Servers", "No servers have been created yet.", "S");
        return;
    }
    
    // Filter servers
    auto filtered_servers = servers;
    if (!admin_ui.server_filter.empty()) {
        std::string filter = admin_ui.server_filter;
        std::transform(filter.begin(), filter.end(), filter.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        
        filtered_servers.erase(std::remove_if(filtered_servers.begin(), filtered_servers.end(),
            [&filter](const auto& server) {
                std::string name = server.name;
                std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                return name.find(filter) == std::string::npos;
            }), filtered_servers.end());
    }
    
    // Sort servers
    std::sort(filtered_servers.begin(), filtered_servers.end(),
        [&admin_ui](const auto& a, const auto& b) {
            switch (admin_ui.server_sort) {
                case 1: return a.status < b.status; // Status
                case 2: return a.created_at > b.created_at; // Recently Created
                case 3: return a.current_players > b.current_players; // Most Players
                default: return a.name < b.name; // Name (A-Z)
            }
        });
    
    // Display servers
    for (auto& server : filtered_servers) {
        ImGui::PushID(server.id.c_str());
        
        card_begin(("##admin_server_" + server.id).c_str(), ImVec2(-1, ui_px(90.0f)));
        
        ImGui::BeginGroup();
        
        // Server info
        ImGui::Text("%s", server.name.c_str());
        ImGui::TextColored(k.muted, "Type: %s", server.type.c_str());
        ImGui::TextColored(k.muted, "Version: %s", server.version.c_str());
        
        // Server status
        ImGui::TextColored(k.muted, "Status:");
        ImGui::SameLine();
        switch (server.status) {
            case aml::servers::ServerStatus::Running:
                ImGui::TextColored(k.green, "Running");
                break;
            case aml::servers::ServerStatus::Stopped:
                ImGui::TextColored(k.muted, "Stopped");
                break;
            case aml::servers::ServerStatus::Starting:
                ImGui::TextColored(k.yellow, "Starting");
                break;
            case aml::servers::ServerStatus::Stopping:
                ImGui::TextColored(k.yellow, "Stopping");
                break;
            case aml::servers::ServerStatus::Error:
                ImGui::TextColored(k.red, "Error");
                break;
            default:
                ImGui::TextColored(k.muted, "Unknown");
                break;
        }
        
        ImGui::TextColored(k.muted, "Players: %d/%d", server.current_players, server.max_players);
        
        ImGui::EndGroup();
        
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - ui_px(200.0f));
        
        ImGui::BeginGroup();
        
        // Start/Stop button
        if (server.status == aml::servers::ServerStatus::Running) {
            if (ghost_button("Stop", ImVec2(ui_px(80.0f), ui_px(28.0f)))) {
                server_manager.stop_server(server.id);
            }
        } else {
            if (ghost_button("Start", ImVec2(ui_px(80.0f), ui_px(28.0f)))) {
                server_manager.start_server(server.id);
            }
        }
        
        ImGui::SameLine();
        
        // More button
        if (icon_button(IconId::More, ImVec2(ui_px(30.0f), ui_px(28.0f)),
                        "More server actions")) {
            ImGui::OpenPopup(("##admin_server_menu_" + server.id).c_str());
        }
        
        // Server menu
        if (ImGui::BeginPopup(("##admin_server_menu_" + server.id).c_str())) {
            if (ImGui::MenuItem("Restart")) {
                server_manager.restart_server(server.id);
            }
            
            if (ImGui::MenuItem("Delete", nullptr, false, true)) {
                // Delete server
                if (server_manager.delete_server(server.id)) {
                    push_notice(st, ui_model::NoticeLevel::Success, "Server Deleted", 
                                "Server deleted successfully");
                }
            }
            
            ImGui::EndPopup();
        }
        
        ImGui::EndGroup();
        
        card_end();
        ImGui::PopID();
    }
}

// ---------------------------------------------------------------------------
// Admin Nodes Tab
// ---------------------------------------------------------------------------

void draw_admin_nodes(UiState& st) {
    auto& admin_ui = get_admin_ui_state();
    auto& server_manager = aml::servers::ServerManager::instance();
    
    page_title("Node Management", "Manage server nodes and clusters");
    
    card_begin("##admin_nodes_header");
    
    ImGui::TextUnformatted("Nodes");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - ui_px(200.0f));
    
    ImGui::TextColored(k.muted, "Node registration is temporarily unavailable.");
    
    ImGui::Spacing();
    
    // Filter and sort
    ImGui::SetNextItemWidth(ui_px(240.0f));
    input_text_hint("##admin_node_filter", "Filter nodes...", &admin_ui.node_filter);
    ImGui::SameLine();
    
    ImGui::SetNextItemWidth(ui_px(160.0f));
    const char* sort_options[] = {"Name (A-Z)", "Status", "CPU Usage", "Memory Usage"};
    if (ImGui::BeginCombo("##admin_node_sort", sort_options[admin_ui.node_sort])) {
        for (int i = 0; i < 4; ++i) {
            if (ImGui::Selectable(sort_options[i], admin_ui.node_sort == i)) {
                admin_ui.node_sort = i;
            }
        }
        ImGui::EndCombo();
    }
    
    card_end();
    
    ImGui::Spacing();
    
    // Get all nodes
    auto nodes = server_manager.get_nodes();
    
    if (nodes.empty()) {
        empty_state("No Nodes", "No server nodes have been registered yet.", "N");
        return;
    }
    
    // Filter nodes
    auto filtered_nodes = nodes;
    if (!admin_ui.node_filter.empty()) {
        std::string filter = admin_ui.node_filter;
        std::transform(filter.begin(), filter.end(), filter.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        
        filtered_nodes.erase(std::remove_if(filtered_nodes.begin(), filtered_nodes.end(),
            [&filter](const auto& node) {
                std::string name = node.name;
                std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                return name.find(filter) == std::string::npos;
            }), filtered_nodes.end());
    }
    
    // Sort nodes
    std::sort(filtered_nodes.begin(), filtered_nodes.end(),
        [&admin_ui](const auto& a, const auto& b) {
            switch (admin_ui.node_sort) {
                case 1: return a.status < b.status; // Status
                case 2: {
                    const bool a_available = has_authoritative_metrics(a);
                    const bool b_available = has_authoritative_metrics(b);
                    if (a_available != b_available) return a_available > b_available;
                    return a.cpu_usage > b.cpu_usage; // CPU Usage (descending)
                }
                case 3: {
                    const bool a_available = has_authoritative_metrics(a);
                    const bool b_available = has_authoritative_metrics(b);
                    if (a_available != b_available) return a_available > b_available;
                    return a.memory_usage > b.memory_usage; // Memory Usage (descending)
                }
                default: return a.name < b.name; // Name (A-Z)
            }
        });
    
    // Display nodes
    for (auto& node : filtered_nodes) {
        ImGui::PushID(node.id.c_str());
        
        card_begin(("##admin_node_" + node.id).c_str(), ImVec2(-1, ui_px(100.0f)));
        
        ImGui::BeginGroup();
        
        // Node info
        ImGui::Text("%s", node.name.c_str());
        ImGui::TextColored(k.muted, "Host: %s:%d", node.host.c_str(), node.port);
        
        // Node status
        ImGui::TextColored(k.muted, "Status:");
        ImGui::SameLine();
        switch (node.status) {
            case aml::servers::NodeStatus::Online:
                ImGui::TextColored(k.green, "Online");
                break;
            case aml::servers::NodeStatus::Offline:
                ImGui::TextColored(k.red, "Offline");
                break;
            case aml::servers::NodeStatus::Maintenance:
                ImGui::TextColored(k.yellow, "Maintenance");
                break;
            case aml::servers::NodeStatus::Overloaded:
                ImGui::TextColored(k.red, "Overloaded");
                break;
            default:
                ImGui::TextColored(k.muted, "Unknown");
                break;
        }
        
        // Resource usage
        if (has_authoritative_metrics(node)) {
            ImGui::TextColored(k.muted, "CPU: %.1f%%", node.cpu_usage * 100);
            ImGui::TextColored(k.muted, "Memory: %.1f%%", node.memory_usage * 100);
            ImGui::TextColored(k.muted, "Storage: %.1f%%", node.storage_usage * 100);
        } else {
            ImGui::TextColored(k.muted, "CPU: Unavailable");
            ImGui::TextColored(k.muted, "Memory: Unavailable");
            ImGui::TextColored(k.muted, "Storage: Unavailable");
        }
        
        // Hardware specs
        ImGui::TextColored(k.muted, "%d CPU cores | %d MB RAM | %d GB Storage",
                         node.cpu_cores, node.memory_mb, node.storage_gb);
        
        ImGui::EndGroup();
        
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - ui_px(150.0f));
        
        ImGui::BeginGroup();
        
        // Health check button
        if (ghost_button("Check Health", ImVec2(ui_px(120.0f), ui_px(28.0f)))) {
            server_manager.check_node_health();
            push_notice(st, ui_model::NoticeLevel::Info, "Health Check Requested",
                        "Node health data will refresh when the control plane responds.");
        }
        
        ImGui::SameLine();
        
        // More button
        if (icon_button(IconId::More, ImVec2(ui_px(30.0f), ui_px(28.0f)),
                        "More node actions")) {
            ImGui::OpenPopup(("##admin_node_menu_" + node.id).c_str());
        }
        
        // Node menu
        if (ImGui::BeginPopup(("##admin_node_menu_" + node.id).c_str())) {
            if (ImGui::MenuItem("Deregister", nullptr, false, true)) {
                // Deregister node
                if (server_manager.deregister_node(node.id)) {
                    push_notice(st, ui_model::NoticeLevel::Success, "Node Deregistered", 
                                "Node deregistered successfully");
                }
            }
            
            ImGui::EndPopup();
        }
        
        ImGui::EndGroup();
        
        card_end();
        ImGui::PopID();
    }
}

// ---------------------------------------------------------------------------
// Admin Storage Tab
// ---------------------------------------------------------------------------

void draw_admin_storage(UiState& st) {
    auto& admin_ui = get_admin_ui_state();
    auto& storage = aml::storage::StorageManager::instance();
    
    page_title("Storage Management", "Manage storage buckets and files");
    
    card_begin("##admin_storage_header");
    
    ImGui::TextUnformatted("Storage");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - ui_px(200.0f));
    
    ImGui::TextColored(k.muted, "Storage buckets are managed by the deployment environment.");
    
    ImGui::Spacing();
    
    // Filter
    ImGui::SetNextItemWidth(ui_px(240.0f));
    input_text_hint("##admin_storage_filter", "Filter buckets...", &admin_ui.storage_bucket_filter);
    
    card_end();
    
    ImGui::Spacing();
    
    // Get buckets
    auto buckets = storage.get_buckets();
    
    if (buckets.empty()) {
        empty_state("No Buckets", "No storage buckets have been created yet.", "B");
        return;
    }
    
    // Filter buckets
    auto filtered_buckets = buckets;
    if (!admin_ui.storage_bucket_filter.empty()) {
        std::string filter = admin_ui.storage_bucket_filter;
        std::transform(filter.begin(), filter.end(), filter.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        
        filtered_buckets.erase(std::remove_if(filtered_buckets.begin(), filtered_buckets.end(),
            [&filter](const auto& bucket) {
                std::string name = bucket.name;
                std::string description = bucket.description;
                std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                std::transform(description.begin(), description.end(), description.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                return name.find(filter) == std::string::npos && 
                       description.find(filter) == std::string::npos;
            }), filtered_buckets.end());
    }
    
    // Display buckets
    for (auto& bucket : filtered_buckets) {
        ImGui::PushID(bucket.name.c_str());
        
        card_begin(("##admin_bucket_" + bucket.name).c_str(), ImVec2(-1, ui_px(80.0f)));
        
        ImGui::BeginGroup();
        
        // Bucket info
        ImGui::Text("%s", bucket.name.c_str());
        ImGui::TextColored(k.muted, "%s", bucket.description.c_str());
        
        // Bucket stats
        ImGui::TextColored(k.muted, "Created: %s", format_date(bucket.created_at).c_str());
        if (bucket.public_access) {
            ImGui::TextColored(k.green, "Public Access");
        } else {
            ImGui::TextColored(k.muted, "Private Access");
        }
        
        ImGui::EndGroup();
        
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - ui_px(150.0f));
        
        ImGui::BeginGroup();
        
        // File browsing is intentionally withheld until storage permissions
        // and pagination are exposed through the deployment control plane.
        ImGui::TextColored(k.muted, "File browser unavailable");
        
        ImGui::SameLine();
        
        // More button
        if (icon_button(IconId::More, ImVec2(ui_px(30.0f), ui_px(28.0f)),
                        "More storage actions")) {
            ImGui::OpenPopup(("##admin_bucket_menu_" + bucket.name).c_str());
        }
        
        // Bucket menu
        if (ImGui::BeginPopup(("##admin_bucket_menu_" + bucket.name).c_str())) {
            if (ImGui::MenuItem("Delete", nullptr, false, true)) {
                // Delete bucket
                if (storage.delete_bucket(bucket.name)) {
                    push_notice(st, ui_model::NoticeLevel::Success, "Bucket Deleted", 
                                "Storage bucket deleted successfully");
                }
            }
            
            ImGui::EndPopup();
        }
        
        ImGui::EndGroup();
        
        card_end();
        ImGui::PopID();
    }
}

// ---------------------------------------------------------------------------
// Admin Settings Tab
// ---------------------------------------------------------------------------

void draw_admin_settings_tab(UiState& st) {
    auto& admin_ui = get_admin_ui_state();
    (void)st;
    
    page_title("Admin Settings", "Configure Amalgam launcher administration");
    
    card_begin("##admin_settings_general");
    ImGui::TextUnformatted("General Settings");
    ImGui::Separator();
    ImGui::Spacing();
    
    // Maintenance mode
    ImGui::Checkbox("Maintenance Mode", &admin_ui.maintenance_mode);
    ImGui::TextColored(k.muted, "Enable maintenance mode to prevent new logins and show a maintenance message");
    
    if (admin_ui.maintenance_mode) {
        ImGui::Spacing();
        ImGui::TextUnformatted("Maintenance Message");
        ImGui::SetNextItemWidth(ui_px(400.0f));
        ImGui::InputTextMultiline("##admin_maintenance_message", &admin_ui.maintenance_message,
                                 ImVec2(ui_px(400.0f), ui_px(100.0f)));
        ImGui::TextColored(k.muted, "Message to display to users during maintenance");
    }
    
    card_end();
    
    ImGui::Spacing();
    
    card_begin("##admin_settings_limits");
    ImGui::TextUnformatted("Resource Limits");
    ImGui::Separator();
    ImGui::Spacing();
    
    // Max servers per user
    ImGui::TextUnformatted("Max Servers per User");
    ImGui::SetNextItemWidth(ui_px(100.0f));
    ImGui::InputInt("##admin_max_servers", &admin_ui.max_servers_per_user);
    ImGui::TextColored(k.muted, "Maximum number of servers each user can create");
    
    ImGui::Spacing();
    
    // Max nodes per user
    ImGui::TextUnformatted("Max Nodes per User");
    ImGui::SetNextItemWidth(ui_px(100.0f));
    ImGui::InputInt("##admin_max_nodes", &admin_ui.max_nodes_per_user);
    ImGui::TextColored(k.muted, "Maximum number of nodes each user can register");
    
    ImGui::Spacing();
    
    // Max storage per user
    ImGui::TextUnformatted("Max Storage per User (MB)");
    ImGui::SetNextItemWidth(ui_px(100.0f));
    int max_storage_mb = static_cast<int>(admin_ui.max_storage_per_user / (1024 * 1024));
    if (ImGui::InputInt("##admin_max_storage", &max_storage_mb)) {
        admin_ui.max_storage_per_user = static_cast<uint64_t>(max_storage_mb) * 1024 * 1024;
    }
    ImGui::TextColored(k.muted, "Maximum storage space each user can use (%s)",
                     format_bytes(admin_ui.max_storage_per_user).c_str());
    
    card_end();
    
    ImGui::Spacing();
    
    card_begin("##admin_settings_actions");
    ImGui::TextUnformatted("Admin Actions");
    ImGui::Separator();
    ImGui::Spacing();
    
    ImGui::TextColored(k.muted,
                       "Admin settings persistence is unavailable; changes apply only to this launcher session.");
    
    if (ghost_button("Reset to Defaults", ImVec2(ui_px(150.0f), ui_px(36.0f)))) {
        // Reset to defaults
        admin_ui.maintenance_mode = false;
        admin_ui.maintenance_message.clear();
        admin_ui.max_servers_per_user = 10;
        admin_ui.max_nodes_per_user = 5;
        admin_ui.max_storage_per_user = 1024 * 1024 * 1024;
    }
    
    card_end();
}

// ---------------------------------------------------------------------------
// Admin Main Tab
// ---------------------------------------------------------------------------

void draw_admin_page(UiState& st) {
    auto& admin_ui = get_admin_ui_state();
    
    // Check if admin is unlocked
    if (!st.admin_unlocked) {
        // Show admin login
        draw_admin_login(st);
        return;
    }
    
    // Admin tabs
    ImGui::BeginTabBar("##admin_tabs");
    
    if (ImGui::BeginTabItem("Dashboard")) {
        admin_ui.current_tab = 0;
        ImGui::EndTabItem();
    }
    
    if (ImGui::BeginTabItem("Users")) {
        admin_ui.current_tab = 1;
        ImGui::EndTabItem();
    }
    
    if (ImGui::BeginTabItem("Servers")) {
        admin_ui.current_tab = 2;
        ImGui::EndTabItem();
    }
    
    if (ImGui::BeginTabItem("Nodes")) {
        admin_ui.current_tab = 3;
        ImGui::EndTabItem();
    }
    
    if (ImGui::BeginTabItem("Storage")) {
        admin_ui.current_tab = 4;
        ImGui::EndTabItem();
    }
    
    if (ImGui::BeginTabItem("Settings")) {
        admin_ui.current_tab = 5;
        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Project Review")) {
        admin_ui.current_tab = 6;
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Beta Feedback")) {
        admin_ui.current_tab = 7;
        ImGui::EndTabItem();
    }
    
    ImGui::EndTabBar();
    
    ImGui::Spacing();
    
    // Draw current tab
    switch (admin_ui.current_tab) {
        case 0:
        default:
            draw_admin_dashboard(st);
            break;
        case 1:
            draw_admin_users(st);
            break;
        case 2:
            draw_admin_servers(st);
            break;
        case 3:
            draw_admin_nodes(st);
            break;
        case 4:
            draw_admin_storage(st);
            break;
        case 5:
            draw_admin_settings_tab(st);
            break;
        case 6:
            draw_admin_moderation(st);
            break;
        case 7:
            draw_admin_feedback(st);
            break;
    }
}

// ---------------------------------------------------------------------------
// Admin Login
// ---------------------------------------------------------------------------

void draw_admin_login(UiState& st) {
    config::Config& c = *st.cfg;
    const uint64_t now = GetTickCount64();

    // Use the authenticated Supabase staff role when available. The local
    // password remains an offline fallback; email strings are never trusted by
    // the client as an authorization mechanism.
    static uint64_t last_staff_check = 0;
    if (!st.admin_unlocked && now - last_staff_check > 5000) {
        last_staff_check = now;
        auto* client = aml::supabase::SupabaseManager::instance().client();
        if (client && client->is_current_user_staff()) {
            st.admin_unlocked = true;
            st.admin_unlock_until_ms = now + 15ull * 60ull * 1000ull;
            st.admin_status = "Staff access enabled for this account.";
            return;
        }
    }
    
    if (st.admin_unlocked && now >= st.admin_unlock_until_ms) {
        st.admin_unlocked = false;
        st.admin_password.clear();
        st.admin_password_confirm.clear();
        st.admin_status = "Admin session expired after 15 minutes.";
    }
    
    card_begin("##adminaccess", ImVec2(-1, 0));
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted("Admin control center");
    ImGui::PopFont();
    ImGui::TextColored(k.muted,
                       "Publisher and credential controls are local to this Windows account. They are never included in exported profiles or release packages.");
    ImGui::Spacing();
    
    if (!admin_auth::configured(c) && !st.admin_unlocked) {
        auto* client = aml::supabase::SupabaseManager::instance().client();
        if (client && client->is_authenticated()) {
            ImGui::TextColored(k.yellow, "Staff role required.");
            ImGui::TextWrapped(
                "This beta does not create local Admin passwords. Admin access is granted server-side to approved staff accounts.");
            ImGui::TextColored(k.muted, "Signed in account: %s", client->current_user().email.c_str());
            ImGui::Spacing();
            ImGui::TextColored(k.muted,
                               "Ask the project owner to assign this account in public.staff_roles, then reload the page.");
            if (ghost_button("Refresh staff access", ImVec2(ui_px(170.0f), ui_px(32.0f))))
                last_staff_check = 0;
        } else {
            ImGui::TextColored(k.yellow, "Amalgam account sign-in required.");
            ImGui::TextWrapped("Sign into your Amalgam account before requesting staff access.");
        }
    } else {
        ImGui::SetNextItemWidth(auto_item_width(320.0f, 180.0f));
        input_secret("##admin_password", &st.admin_password);
        ImGui::SameLine();
        if (ghost_button("Unlock", ImVec2(ui_px(100.0f), ui_px(34.0f)))) {
            if (admin_auth::verify_password(c, st.admin_password)) {
                st.admin_unlocked = true;
                st.admin_unlock_until_ms = now + 15ull * 60ull * 1000ull;
                st.admin_password.clear();
                st.admin_status = "Admin access enabled for this session.";
            } else {
                st.admin_password.clear();
                st.admin_status = "Admin password is incorrect.";
            }
        }
    }
    
    if (!st.admin_status.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(k.muted, "%s", st.admin_status.c_str());
    }
    
    card_end();
}

// ---------------------------------------------------------------------------
// Helper Functions
}  // namespace aml::ui
