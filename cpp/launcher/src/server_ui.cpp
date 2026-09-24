#include "ui.h"
#include "ui_internal.h"
#include "services.h"
#include "server_types.h"
#include "server_providers.h"
#include "server_provision.h"
#include "version_catalog.h"
#include "essentials_address.h"
#include "config.h"
#include "json.h"
#include "net.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commdlg.h>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <exception>
#include <mutex>
#include <thread>

namespace aml::ui {

// ---------------------------------------------------------------------------
// Local server persistence
// ---------------------------------------------------------------------------

static std::wstring servers_json_path() {
    return aml::net::get_local_app_data_path() + L"\\amalgam\\servers.json";
}

static bool save_local_servers(const std::vector<server::ServerConfig>& servers,
                               std::string* error = nullptr) {
    Json arr = Json::arr();
    for (const auto& s : servers) {
        Json obj = Json::obj();
        obj["name"] = s.name;
        obj["software"] = static_cast<int>(s.software);
        obj["minecraft_version"] = s.minecraft_version;
        obj["allocated_ram_mb"] = s.allocated_ram_mb;
        obj["max_players"] = s.max_players;
        obj["port"] = s.port;
        obj["server_directory"] = s.server_directory;
        obj["stage"] = static_cast<int>(s.stage);
        arr.push(obj);
    }
    std::string err;
    const bool saved = aml::json_write_file(servers_json_path(), arr, &err);
    if (!saved && error) {
        *error = err.empty()
            ? "The launcher could not save the local server list."
            : err;
    }
    return saved;
}

static void load_local_servers(std::vector<server::ServerConfig>& servers) {
    Json root;
    std::string err;
    if (!aml::json_parse_file(servers_json_path(), root, &err) || !root.isArray())
        return;
    servers.clear();
    for (size_t i = 0; i < root.size(); ++i) {
        const Json& j = root[i];
        server::ServerConfig s;
        s.name = j.get("name").as_str();
        s.software = static_cast<server::ServerSoftware>(j.get("software").as_int());
        s.minecraft_version = j.get("minecraft_version").as_str("1.20.1");
        s.allocated_ram_mb = static_cast<int>(j.get("allocated_ram_mb").as_int(4096));
        s.max_players = static_cast<int>(j.get("max_players").as_int(20));
        s.port = static_cast<int>(j.get("port").as_int(25565));
        s.server_directory = j.get("server_directory").as_str();
        s.stage = static_cast<server::ServerStage>(j.get("stage").as_int());
        servers.push_back(std::move(s));
    }
}

// Local run state belongs to the process the service is supervising; the saved
// list only records a hint. Every read of "is this server running" asks here.
static bool local_server_supervised(const server::ServerConfig& sv) {
    return aml::services::local_server_manager()->is_local_server_running(sv.name);
}

// Pull only telemetry the local supervisor can prove.  CPU and working-set
// memory come from the supervised Windows process; Minecraft-internal TPS and
// player counts remain unavailable until a server-side health bridge reports
// them, so the UI never turns zero defaults into fake live values.
static void refresh_local_server_metrics(UiState& st,
                                         const server::ServerConfig& server_config) {
    if (st.fixture_mode || !local_server_supervised(server_config)) {
        st.server_metrics = server::ServerMetrics();
        return;
    }

    std::string error;
    const auto sample = aml::services::local_server_manager()->get_local_server_metrics(
        server_config.name, &error);
    if (!sample.valid) {
        st.server_metrics = server::ServerMetrics();
        return;
    }

    auto& metrics = st.server_metrics;
    metrics = server::ServerMetrics();
    metrics.valid = true;
    metrics.cpu_valid = sample.cpu_valid;
    metrics.ram_valid = sample.memory_valid;
    metrics.tps_valid = false;
    metrics.players_valid = false;
    metrics.cpu_percent = sample.cpu_percent;
    if (sample.memory_valid) {
        constexpr double kMegabyte = 1024.0 * 1024.0;
        metrics.ram_mb = static_cast<int>(std::max<uint64_t>(1,
            static_cast<uint64_t>(sample.working_set_bytes / kMegabyte)));
        metrics.ram_percent = server_config.allocated_ram_mb > 0
            ? std::clamp(static_cast<float>(sample.working_set_bytes /
                (kMegabyte * static_cast<double>(server_config.allocated_ram_mb)) * 100.0),
                         0.0f, 100.0f)
            : 0.0f;
    }
}

static server::ServerStage effective_stage(const UiState& st,
                                           const server::ServerConfig& sv) {
    // Visual-review fixtures seed a running server with no process on purpose.
    if (st.fixture_mode) return sv.stage;
    return server::reconciled_stage(sv, local_server_supervised(sv));
}

// Correct a run stage no supervised process backs, so the saved list stops
// claiming a server is up after a restart.
static void reconcile_local_server_stages(std::vector<server::ServerConfig>& servers) {
    bool changed = false;
    for (auto& sv : servers) {
        server::ServerStage stage =
            server::reconciled_stage(sv, local_server_supervised(sv));
        if (stage == server::ServerStage::Installing) {
            // A persisted install means the launcher closed while the files
            // were being fetched: nothing finished them.
            stage = server::ServerStage::NotInstalled;
            sv.status_message = "The server files were not finished in the last session.";
        } else if (stage == server::ServerStage::Ready &&
                   !server::runtime_files_present(sv)) {
            stage = server::ServerStage::NotInstalled;
            sv.status_message = std::string(server::server_software_name(sv.software)) +
                                " is not installed in this server folder yet.";
            if (server::server_software_kind(sv.software) == server::RuntimeKind::Manual) {
                // Say what a manual runtime needs instead of pointing at a
                // download that does not exist.
                aml::server_providers::RuntimeArtifact artifact;
                std::string reason;
                aml::server_providers::resolve_runtime(sv.software, sv.minecraft_version,
                                                       artifact, &reason);
                if (!artifact.manual_hint.empty()) sv.status_message = artifact.manual_hint;
            }
        }
        if (stage != sv.stage) {
            sv.stage = stage;
            changed = true;
        }
    }
    if (changed) save_local_servers(servers);
}

// The runtime download the page is running. It lives outside ServerUIState
// because the render loop, the page and the worker thread all have to see it:
// atomics for the flag and progress, a mutex for the text.
struct RuntimeDownload {
    std::atomic<bool> active{false};
    std::atomic<bool> cancel{false};
    // Only active network transfers observe cancel through net::download's
    // progress callback. Resolution, unpacking, installer execution, and
    // final config writing cannot be stopped safely by this job.
    std::atomic<bool> cancellable{false};
    std::atomic<bool> cancellation_observed{false};
    std::atomic<float> progress{0.0f};
    // The worker publishes its outcome here and never touches the server list:
    // the page owns that list, so it applies the result on its own thread.
    std::atomic<bool> finished{false};
    std::mutex mu;
    std::string server;
    std::string phase;
    std::string outcome;
    bool success = false;
    bool cancelled = false;
};

static RuntimeDownload& runtime_download() {
    static RuntimeDownload job;
    return job;
}

// Whether any server on this page is supervised and up, or a runtime download
// is running. The console, the metrics and the download's progress all keep
// moving, so the render loop keeps drawing for this page.
bool server_ui_has_live_server(UiState& st) {
    // A visual fixture has no live server by definition.  This helper is
    // queried by the shared render loop, so protect the boundary here as well
    // as inside the page renderer.
    if (st.fixture_mode) return false;
    if (runtime_download().active.load()) return true;
    for (const auto& sv : st.servers)
        if (local_server_supervised(sv)) return true;
    return false;
}

// ---------------------------------------------------------------------------
// UI state singleton (codebase pattern)
// ---------------------------------------------------------------------------

enum class LocalServerAction {
    Start,
    Stop,
    Restart,
};

// Console, player, and world actions all write to the same supervised-process
// pipe. Keep their UI intent explicit so the result can be reconciled with the
// correct field or console without letting a late worker update a different
// server surface.
enum class LocalServerCommandOrigin : int64_t {
    PopupConsole,
    DetailConsole,
    Kick,
    Ban,
    WhitelistAdd,
    WhitelistRemove,
    OpAdd,
    OpRemove,
    SaveAll,
};

// A server name is the service-layer key, but it is not enough to safely
// reconcile a completed background operation into a mutable launcher list.
// Keep the saved-server identity and the action inputs together so a rename,
// re-import, or removal while an operation is in flight cannot update a newer
// entry by accident.
struct LocalServerActionTarget {
    std::string name;
    std::string server_directory;
    std::string minecraft_version;
    server::ServerSoftware software = server::ServerSoftware::Vanilla;
    int port = 0;
    int allocated_ram_mb = 0;
};

struct ServerUIState {
    bool loaded = false;
    bool create_open = false;
    int selected = -1;
    int action_pending = -1;
    int mode = 0;  // 0 = Host, 1 = Connect

    // Create dialog
    std::string create_name;
    std::string create_version;
    int create_software_idx = 0;
    int create_ram = 4096;
    int create_max_players = 20;
    int create_port = 25565;
    bool create_eula_accepted = false;
    std::string create_error;

    // The live version list for the software the create dialog is showing.
    // `version_software` is the software the list belongs to, so a reply from
    // an earlier selection cannot be painted as the current one.
    std::mutex version_mu;
    int version_software = -1;
    std::vector<std::string> version_list;
    bool version_loading = false;
    bool version_live = false;
    std::string version_error;


    // Filters
    int filter_status = 0;
    int filter_software = 0;
    int sort_mode = 0;

    // Console (popup legacy)
    bool console_open = false;
    int console_server_idx = -1;
    std::string console_filter;
    std::vector<server::ServerConsoleEntry> console_log;
    std::string console_input;
    bool console_auto_scroll = true;

    // Properties (popup legacy)
    bool properties_open = false;
    int properties_server_idx = -1;
    std::vector<std::pair<std::string, std::string>> properties;
    bool properties_dirty = false;

    // ── Server detail view ────────────────────────────────────────
    // When detail_server_idx >= 0 we show the full-panel detail page
    // for the Host server at that index.  detail_connect_idx handles
    // the Connect address-book row instead.
    int detail_server_idx = -1;   // index into UiState::servers
    int detail_connect_idx = -1;  // index into UiState::cfg->servers
    int detail_tab = 0;           // 0=Overview 1=Console 2=Files 3=Players 4=Plugins 5=Properties 6=World

    // Detail – file browser
    std::string file_browse_path;          // current directory (empty = server root)
    std::vector<std::string> file_entries; // filenames in current dir
    std::vector<bool> file_is_dir;
    bool file_list_dirty = true;
    std::string file_preview_name;
    std::string file_preview_content;
    bool file_preview_open = false;
    // Snapshot-only presentation of the file-row action menu.  This stays
    // separate from the live menu so a visual fixture cannot inherit a real
    // path, a filesystem read, or an Explorer launch target.
    bool fixture_file_context_menu_open = false;
    std::string fixture_file_context_menu_name;
    bool file_delete_confirm = false;
    std::string file_delete_target;
    std::string file_delete_error;

    // Detail – players
    std::vector<server::ServerPlayer> detail_players;
    std::vector<std::string> whitelist_entries;
    std::vector<std::string> ops_entries;
    bool players_dirty = true;
    std::string player_op_input;
    std::string player_whitelist_input;

    // Detail – world
    uint64_t world_size_bytes = 0;
    std::string world_name;
    bool world_dirty = true;
    bool restore_backup_confirm = false;
    std::string restore_backup_id;
    std::string restore_backup_name;
    std::string restore_backup_error;

    // Detail – console (embedded)
    std::string detail_console_filter;
    std::string detail_console_input;
    bool detail_console_auto_scroll = true;
    double detail_console_last_refresh = 0.0;

    // The last action this page asked the service to take and the reason the
    // service refused. Kept here so the cause is rendered on this page; the
    // floating Alerts window can fall outside a small launcher window.
    std::string action_failure_action;
    std::string action_failure_server;
    std::string action_failure_cause;
    std::string action_result_action;
    std::string action_result_server;
    std::string action_result_detail;

    // Process control can wait for a graceful stop, Java resolution, or pipe
    // setup. Keep it in one launcher-owned lane so the render path never waits
    // and no two controls can issue contradictory commands at once.
    AsyncUiRequestState local_action_request;
    bool local_action_target_active = false;
    LocalServerActionTarget local_action_target;

    // A command write is normally quick, but a blocked process pipe must
    // never freeze a launcher frame. This is deliberately a separate lane
    // from lifecycle work so a result can retain its console/form provenance.
    AsyncUiRequestState local_command_request;
    bool local_command_target_active = false;
    LocalServerActionTarget local_command_target;

    std::string remove_server_error;
};

static ServerUIState& state() {
    static ServerUIState s;
    return s;
}

// Server actions report their failure here, keeping the cause the service
// reported (missing jar, Java it cannot resolve, port already in use, wrong
// state) instead of replacing it with a generic line.
static void report_server_failure(ServerUIState& s, UiState& st, const char* action,
                                 const std::string& server, const std::string& cause) {
    s.action_result_action.clear();
    s.action_result_server.clear();
    s.action_result_detail.clear();
    s.action_failure_action = action;
    s.action_failure_server = server;
    s.action_failure_cause = cause.empty()
        ? "The server service refused the request without reporting a cause."
        : cause;
    push_notice(st, ui_model::NoticeLevel::Error, std::string(action) + " failed",
                server.empty() ? s.action_failure_cause
                               : server + ": " + s.action_failure_cause);
}

static void clear_server_failure(ServerUIState& s) {
    s.action_failure_action.clear();
    s.action_failure_server.clear();
    s.action_failure_cause.clear();
}

static void clear_server_action_result(ServerUIState& s) {
    s.action_result_action.clear();
    s.action_result_server.clear();
    s.action_result_detail.clear();
}

static const char* local_server_action_key(LocalServerAction action) {
    switch (action) {
        case LocalServerAction::Start:   return "local-server-start";
        case LocalServerAction::Stop:    return "local-server-stop";
        case LocalServerAction::Restart: return "local-server-restart";
    }
    return "local-server-action";
}

static const char* local_server_action_label(LocalServerAction action) {
    switch (action) {
        case LocalServerAction::Start:   return "Start";
        case LocalServerAction::Stop:    return "Stop";
        case LocalServerAction::Restart: return "Restart";
    }
    return "Server action";
}

static const char* local_server_action_label_from_key(const std::string& action) {
    if (action == "local-server-start") return "Start";
    if (action == "local-server-stop") return "Stop";
    if (action == "local-server-restart") return "Restart";
    return "Server action";
}

static const char* local_server_action_working_label_from_key(const std::string& action) {
    if (action == "local-server-start") return "Starting";
    if (action == "local-server-stop") return "Stopping";
    if (action == "local-server-restart") return "Restarting";
    return "Working on";
}

static LocalServerActionTarget make_local_server_action_target(
    const server::ServerConfig& server) {
    LocalServerActionTarget target;
    target.name = server.name;
    target.server_directory = server.server_directory;
    target.minecraft_version = server.minecraft_version;
    target.software = server.software;
    target.port = server.port;
    target.allocated_ram_mb = server.allocated_ram_mb;
    return target;
}

static bool local_server_action_target_matches(const server::ServerConfig& server,
                                               const LocalServerActionTarget& target) {
    return server.name == target.name &&
           server.server_directory == target.server_directory &&
           server.minecraft_version == target.minecraft_version &&
           server.software == target.software &&
           server.port == target.port &&
           server.allocated_ram_mb == target.allocated_ram_mb;
}

// Return a target only when it resolves to exactly one saved entry. The local
// service currently uses the name as its key, so accepting an ambiguous saved
// identity would make a correct worker result unsafe to apply in the UI.
static server::ServerConfig* find_unique_local_server_action_target(
    UiState& st, const LocalServerActionTarget& target) {
    server::ServerConfig* match = nullptr;
    for (auto& server : st.servers) {
        if (!local_server_action_target_matches(server, target)) continue;
        if (match) return nullptr;
        match = &server;
    }
    return match;
}

static void set_local_server_action_result_target(AsyncUiRequestResult& result,
                                                   const LocalServerActionTarget& target) {
    result.payload_a = target.name;
    result.payload_b = target.server_directory;
    result.payload_c = target.minecraft_version;
    result.numbers = {
        static_cast<int64_t>(target.port),
        static_cast<int64_t>(target.allocated_ram_mb),
        static_cast<int64_t>(target.software),
    };
}

static bool read_local_server_action_result_target(const AsyncUiRequestResult& result,
                                                   LocalServerActionTarget& target) {
    if (result.payload_a.empty() || result.numbers.size() != 3) return false;
    target.name = result.payload_a;
    target.server_directory = result.payload_b;
    target.minecraft_version = result.payload_c;
    target.port = static_cast<int>(result.numbers[0]);
    target.allocated_ram_mb = static_cast<int>(result.numbers[1]);
    target.software = static_cast<server::ServerSoftware>(result.numbers[2]);
    return true;
}

static AsyncUiRequestResult run_local_server_action(LocalServerAction action,
                                                     const LocalServerActionTarget& target) {
    AsyncUiRequestResult result;
    result.number_a = -1;  // No stage reconciliation is safe for this result.
    set_local_server_action_result_target(result, target);

    auto* service = aml::services::local_server_manager();
    const char* label = local_server_action_label(action);
    if (!service) {
        result.title = std::string(label) + " unavailable";
        result.detail = "The local server service is unavailable. Please restart Amalgam and try again.";
        return result;
    }

    std::string error;
    bool completed = false;
    switch (action) {
        case LocalServerAction::Start:
            completed = service->start_local_server(target.name, "", target.allocated_ram_mb, &error);
            if (completed) result.number_a = static_cast<int64_t>(server::ServerStage::Running);
            break;
        case LocalServerAction::Stop:
            completed = service->stop_local_server(target.name, &error);
            if (completed) result.number_a = static_cast<int64_t>(server::ServerStage::Stopped);
            break;
        case LocalServerAction::Restart: {
            const bool stopped = service->stop_local_server(target.name, &error);
            if (!stopped) break;
            completed = service->start_local_server(target.name, "", target.allocated_ram_mb, &error);
            // A restart that successfully stopped the process but could not
            // start it again still has a truthful local state: stopped.
            result.number_a = static_cast<int64_t>(completed
                ? server::ServerStage::Running
                : server::ServerStage::Stopped);
            break;
        }
    }

    result.success = completed;
    result.title = completed ? std::string(label) + " complete"
                             : std::string(label) + " failed";
    if (completed) {
        result.detail = std::string("Amalgam completed the local-only ") +
            label + " action for \"" + target.name + "\".";
    } else {
        result.detail = error.empty()
            ? "The local server service did not report why the request could not complete."
            : error;
    }
    return result;
}

static bool local_server_action_lane_busy(const ServerUIState& s) {
    return snapshot_async_ui_request(s.local_action_request).working;
}

static bool local_server_action_is_working(const ServerUIState& s,
                                           LocalServerAction action) {
    const auto snapshot = snapshot_async_ui_request(s.local_action_request);
    return snapshot.working && snapshot.action == local_server_action_key(action);
}

static const char* local_server_command_key(LocalServerCommandOrigin origin) {
    switch (origin) {
        case LocalServerCommandOrigin::PopupConsole:   return "local-server-command-popup-console";
        case LocalServerCommandOrigin::DetailConsole:  return "local-server-command-detail-console";
        case LocalServerCommandOrigin::Kick:           return "local-server-command-kick";
        case LocalServerCommandOrigin::Ban:            return "local-server-command-ban";
        case LocalServerCommandOrigin::WhitelistAdd:   return "local-server-command-whitelist-add";
        case LocalServerCommandOrigin::WhitelistRemove:return "local-server-command-whitelist-remove";
        case LocalServerCommandOrigin::OpAdd:          return "local-server-command-op-add";
        case LocalServerCommandOrigin::OpRemove:       return "local-server-command-op-remove";
        case LocalServerCommandOrigin::SaveAll:        return "local-server-command-save-all";
    }
    return "local-server-command";
}

static const char* local_server_command_label(LocalServerCommandOrigin origin) {
    switch (origin) {
        case LocalServerCommandOrigin::PopupConsole:
        case LocalServerCommandOrigin::DetailConsole:  return "Console command";
        case LocalServerCommandOrigin::Kick:           return "Kick";
        case LocalServerCommandOrigin::Ban:            return "Ban";
        case LocalServerCommandOrigin::WhitelistAdd:   return "Add to Whitelist";
        case LocalServerCommandOrigin::WhitelistRemove:return "Remove from Whitelist";
        case LocalServerCommandOrigin::OpAdd:          return "Make OP";
        case LocalServerCommandOrigin::OpRemove:       return "De-op";
        case LocalServerCommandOrigin::SaveAll:        return "Save-All";
    }
    return "Server command";
}

static bool local_server_command_origin_from_result(int64_t value,
                                                    LocalServerCommandOrigin& origin) {
    if (value < static_cast<int64_t>(LocalServerCommandOrigin::PopupConsole) ||
        value > static_cast<int64_t>(LocalServerCommandOrigin::SaveAll)) {
        return false;
    }
    origin = static_cast<LocalServerCommandOrigin>(value);
    return true;
}

static bool local_server_command_lane_busy(const ServerUIState& s) {
    return async_ui_request_is_reserved(s.local_command_request);
}

static bool local_server_command_is_working(const ServerUIState& s,
                                            LocalServerCommandOrigin origin) {
    const auto snapshot = snapshot_async_ui_request(s.local_command_request);
    return snapshot.working && snapshot.action == local_server_command_key(origin);
}

static AsyncUiRequestResult run_local_server_command(
    const LocalServerActionTarget& target, LocalServerCommandOrigin origin,
    const std::string& command) {
    AsyncUiRequestResult result;
    set_local_server_action_result_target(result, target);
    result.number_a = static_cast<int64_t>(origin);
    result.title = local_server_command_label(origin);

    auto* service = aml::services::local_server_manager();
    if (!service) {
        result.detail = "The local server service is unavailable. Please restart Amalgam and try again.";
        return result;
    }

    std::string response;
    std::string error;
    result.success = service->send_command(target.name, command, &response, &error);
    if (result.success) {
        result.detail = response.empty() ? "Command sent to local server" : response;
    } else {
        result.detail = error.empty()
            ? "The local server service did not report why the command could not be sent."
            : error;
    }
    return result;
}

static bool start_local_server_command(UiState& st, const server::ServerConfig& server,
                                       LocalServerCommandOrigin origin,
                                       const std::string& command) {
    auto& s = state();
    // Fixtures are an inert visual boundary even if a future presentation
    // route accidentally exposes a command control.
    if (st.fixture_mode || command.empty()) return false;
    if (local_server_action_lane_busy(s) || local_server_command_lane_busy(s)) return false;

    const LocalServerActionTarget target = make_local_server_action_target(server);
    if (!find_unique_local_server_action_target(st, target)) {
        report_server_failure(s, st, local_server_command_label(origin), target.name,
                              "This saved server changed or is ambiguous. Refresh the server list before trying again.");
        return false;
    }

    const std::string request_action = local_server_command_key(origin);
    uint64_t request_generation = 0;
    if (!begin_async_ui_request(s.local_command_request, request_action, &request_generation)) {
        return false;
    }

    clear_server_failure(s);
    clear_server_action_result(s);
    s.local_command_target_active = true;
    s.local_command_target = target;
    spawn_worker(st, std::thread([&st, request_action, request_generation, origin, target, command]() {
        AsyncUiRequestResult result;
        try {
            result = run_local_server_command(target, origin, command);
        } catch (const std::exception&) {
            set_local_server_action_result_target(result, target);
            result.number_a = static_cast<int64_t>(origin);
            result.title = local_server_command_label(origin);
            result.detail = "The local server command ended unexpectedly. Please try again.";
        } catch (...) {
            set_local_server_action_result_target(result, target);
            result.number_a = static_cast<int64_t>(origin);
            result.title = local_server_command_label(origin);
            result.detail = "The local server command ended unexpectedly. Please try again.";
        }
        if (!st.shutting_down.load()) {
            complete_async_ui_request(state().local_command_request, request_action,
                                      request_generation, std::move(result));
        }
    }));
    return true;
}

static void append_local_server_command_console_result(ServerUIState& s,
                                                       bool success,
                                                       const std::string& detail) {
    server::ServerConsoleEntry entry;
    entry.timestamp = success ? "<" : "!";
    entry.message = detail.empty()
        ? (success ? "Command sent" : "Command could not be sent")
        : detail;
    s.console_log.push_back(std::move(entry));
}

static void consume_local_server_command_result(UiState& st) {
    auto& s = state();
    AsyncUiRequestSnapshot completed;
    if (!take_async_ui_request_result(s.local_command_request, &completed)) return;

    s.local_command_target_active = false;
    LocalServerActionTarget target;
    LocalServerCommandOrigin origin = LocalServerCommandOrigin::PopupConsole;
    if (!read_local_server_action_result_target(completed.result, target) ||
        !local_server_command_origin_from_result(completed.result.number_a, origin)) {
        report_server_failure(s, st, "Server command", {},
                              "The background command result was incomplete, so Amalgam did not apply it.");
        return;
    }

    if (!find_unique_local_server_action_target(st, target)) {
        report_server_failure(s, st, local_server_command_label(origin), target.name,
                              "The saved server changed while this command was running. Amalgam left the newer entry unchanged; check the actual process state before retrying.");
        return;
    }

    const char* label = local_server_command_label(origin);
    const bool console_origin = origin == LocalServerCommandOrigin::PopupConsole ||
                                origin == LocalServerCommandOrigin::DetailConsole;
    if (console_origin) {
        append_local_server_command_console_result(s, completed.result.success,
                                                   completed.result.detail);
    }

    if (!completed.result.success) {
        report_server_failure(s, st, label, target.name, completed.result.detail);
        return;
    }

    switch (origin) {
        case LocalServerCommandOrigin::WhitelistAdd:
            s.player_whitelist_input.clear();
            s.players_dirty = true;
            break;
        case LocalServerCommandOrigin::WhitelistRemove:
        case LocalServerCommandOrigin::OpAdd:
        case LocalServerCommandOrigin::OpRemove:
        case LocalServerCommandOrigin::Kick:
        case LocalServerCommandOrigin::Ban:
            s.players_dirty = true;
            if (origin == LocalServerCommandOrigin::OpAdd) s.player_op_input.clear();
            break;
        case LocalServerCommandOrigin::PopupConsole:
        case LocalServerCommandOrigin::DetailConsole:
        case LocalServerCommandOrigin::SaveAll:
            break;
    }

    clear_server_failure(s);
    s.action_result_action = label;
    s.action_result_server = target.name;
    s.action_result_detail = completed.result.detail;
    push_notice(st, ui_model::NoticeLevel::Success, std::string(label) + " complete",
                target.name + ": " + completed.result.detail);
}

static bool start_local_server_action(UiState& st, int server_index,
                                      LocalServerAction action) {
    auto& s = state();
    // Keep the fixture isolation hard at the action boundary rather than
    // relying solely on the fixture renderer's disabled controls.
    if (st.fixture_mode) return false;
    if (server_index < 0 || server_index >= static_cast<int>(st.servers.size())) {
        report_server_failure(s, st, local_server_action_label(action), {},
                              "The selected server is no longer available. Refresh the page and try again.");
        return false;
    }
    if (local_server_action_lane_busy(s) || local_server_command_lane_busy(s)) return false;

    const LocalServerActionTarget target =
        make_local_server_action_target(st.servers[static_cast<size_t>(server_index)]);
    if (!find_unique_local_server_action_target(st, target)) {
        report_server_failure(s, st, local_server_action_label(action), target.name,
                              "This saved server changed or is ambiguous. Refresh the server list before trying again.");
        return false;
    }

    const std::string request_action = local_server_action_key(action);
    uint64_t request_generation = 0;
    if (!begin_async_ui_request(s.local_action_request, request_action, &request_generation)) {
        return false;
    }

    clear_server_failure(s);
    clear_server_action_result(s);
    s.local_action_target_active = true;
    s.local_action_target = target;
    spawn_worker(st, std::thread([&st, request_action, request_generation, action, target]() {
        AsyncUiRequestResult result;
        try {
            result = run_local_server_action(action, target);
        } catch (const std::exception&) {
            set_local_server_action_result_target(result, target);
            result.number_a = -1;
            result.title = std::string(local_server_action_label(action)) + " failed";
            result.detail = "The local server action ended unexpectedly. Please try again.";
        } catch (...) {
            set_local_server_action_result_target(result, target);
            result.number_a = -1;
            result.title = std::string(local_server_action_label(action)) + " failed";
            result.detail = "The local server action ended unexpectedly. Please try again.";
        }
        if (!st.shutting_down.load()) {
            complete_async_ui_request(state().local_action_request, request_action,
                                      request_generation, std::move(result));
        }
    }));
    return true;
}

static void consume_local_server_action_result(UiState& st) {
    auto& s = state();
    AsyncUiRequestSnapshot completed;
    if (!take_async_ui_request_result(s.local_action_request, &completed)) return;

    s.local_action_target_active = false;
    LocalServerActionTarget target;
    const char* label = local_server_action_label_from_key(completed.action);
    if (!read_local_server_action_result_target(completed.result, target)) {
        report_server_failure(s, st, label, {},
                              "The background result was incomplete, so Amalgam did not update any saved server state.");
        return;
    }

    server::ServerConfig* current = find_unique_local_server_action_target(st, target);
    if (!current) {
        report_server_failure(s, st, label, target.name,
                              "The saved server changed while this action was running. Amalgam left the newer entry unchanged; check the actual process state before retrying.");
        return;
    }

    const int64_t saved_stage = completed.result.number_a;
    if (saved_stage >= static_cast<int64_t>(server::ServerStage::NotInstalled) &&
        saved_stage <= static_cast<int64_t>(server::ServerStage::Crashed)) {
        current->stage = static_cast<server::ServerStage>(saved_stage);
        std::string save_error;
        if (!save_local_servers(st.servers, &save_error)) {
            report_server_failure(s, st, label, target.name,
                                  "The process action completed, but Amalgam could not save its new status: " +
                                  (save_error.empty() ? std::string("unknown save error") : save_error));
            return;
        }
    }

    if (!completed.result.success) {
        report_server_failure(s, st, label, target.name, completed.result.detail);
        return;
    }

    clear_server_failure(s);
    s.action_result_action = label;
    s.action_result_server = target.name;
    s.action_result_detail = completed.result.detail;
    push_notice(st, ui_model::NoticeLevel::Success, std::string(label) + " complete",
                completed.result.detail);
}

// This confirmation is reachable both from a server card and the local-server
// detail view.  Keep it outside either layout so the detail view cannot set an
// action_pending flag and then return before a user ever sees the confirmation.
static void draw_remove_server_dialog(UiState& st) {
    auto& s = state();
    if (s.action_pending >= 0 && s.action_pending < static_cast<int>(st.servers.size())) {
        ImGui::OpenPopup("Confirm Remove Server");
    }
    if (!ImGui::BeginPopupModal("Confirm Remove Server", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted("Remove saved server entry?");
    ImGui::PopFont();
    ImGui::Text("Remove \"%s\" from Amalgam?",
                s.action_pending >= 0 && s.action_pending < static_cast<int>(st.servers.size())
                    ? st.servers[static_cast<size_t>(s.action_pending)].name.c_str() : "");
    ImGui::TextColored(k.muted,
                       "This removes only Amalgam's saved server entry. The server folder and all server files stay on this PC.");
    if (!s.remove_server_error.empty()) {
        ImGui::Spacing();
        ImGui::PushFont(f_bold);
        ImGui::TextColored(k.red, "Remove failed");
        ImGui::PopFont();
        ImGui::PushStyleColor(ImGuiCol_Text, k.red);
        ImGui::TextWrapped("%s", s.remove_server_error.c_str());
        ImGui::PopStyleColor();
    }
    const bool fixture_preview = st.fixture_mode;
    const bool local_action_busy = local_server_action_lane_busy(s) ||
                                   local_server_command_lane_busy(s);
    if (fixture_preview) {
        ImGui::Spacing();
        ImGui::TextColored(k.brand_hov,
                           "Visual fixture — removing this saved entry is disabled; no server list is changed.");
    }
    if (local_action_busy && !fixture_preview) {
        ImGui::Spacing();
        ImGui::TextColored(k.muted,
                           "A local server action is still finishing. This saved entry cannot be removed yet.");
    }
    ImGui::Spacing();
    const auto attempt_remove_entry = [&] {
        // Do not rely on a disabled fixture button alone. This is the action
        // boundary that protects the persisted server list from any fixture
        // invocation path.
        if (st.fixture_mode || local_server_action_lane_busy(s)) return;
        if (s.action_pending < 0 || s.action_pending >= static_cast<int>(st.servers.size())) return;
        const int pending_index = s.action_pending;
        std::vector<server::ServerConfig> revised_servers = st.servers;
        revised_servers.erase(revised_servers.begin() + pending_index);
        std::string save_error;
        if (save_local_servers(revised_servers, &save_error)) {
            st.servers = std::move(revised_servers);
            s.action_pending = -1;
            s.detail_server_idx = -1;
            s.selected = -1;
            s.remove_server_error.clear();
            clear_server_failure(s);
            push_notice(st, ui_model::NoticeLevel::Success, "Server entry removed",
                        "The saved entry was removed from Amalgam. Its server folder and files were left unchanged.");
            ImGui::CloseCurrentPopup();
        } else {
            s.remove_server_error = save_error.empty()
                ? "The launcher could not save this change. The server remains in Amalgam."
                : "The launcher could not save this change. The server remains in Amalgam: " + save_error;
            report_server_failure(s, st, "Remove server entry",
                                  st.servers[static_cast<size_t>(pending_index)].name,
                                  s.remove_server_error);
        }
    };
    if (danger_button("Remove entry", ImVec2(ui_px(120.0f), ui_px(32.0f)),
                      fixture_preview || local_action_busy)) {
        attempt_remove_entry();
    }
    ImGui::SameLine();
    if (ghost_button(fixture_preview ? "Close preview" : "Cancel",
                     ImVec2(fixture_preview ? ui_px(116.0f) : ui_px(80.0f), ui_px(32.0f)))) {
        s.action_pending = -1;
        s.remove_server_error.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// Rendered inside the page content, above everything else the Servers page
// draws, so the reason is readable without leaving the page or opening Alerts.
static void draw_server_action_failure(UiState&) {
    auto& s = state();

    const auto request = snapshot_async_ui_request(s.local_action_request);
    if (request.working) {
        const char* action = local_server_action_working_label_from_key(request.action);
        ImGui::PushStyleColor(ImGuiCol_ChildBg,
                              ImVec4(k.brand.x, k.brand.y, k.brand.z, 0.09f));
        ImGui::PushStyleColor(ImGuiCol_Border,
                              ImVec4(k.brand.x, k.brand.y, k.brand.z, 0.36f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, ui_px(8.0f));
        if (ImGui::BeginChild("##server_action_working", ImVec2(-1, 0),
                              ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY)) {
            ImGui::PushFont(f_bold);
            ImGui::TextColored(k.brand_hov, "%s local server", action);
            ImGui::PopFont();
            if (s.local_action_target_active && !s.local_action_target.name.empty()) {
                ImGui::SameLine(0, ui_px(6.0f));
                ImGui::TextColored(k.muted, "%s", s.local_action_target.name.c_str());
            }
            ImGui::PushFont(f_small);
            ImGui::TextColored(k.muted,
                               "Amalgam is keeping the interface responsive while the local process operation finishes.");
            ImGui::PopFont();
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
        ImGui::Spacing();
    }

    const auto command_request = snapshot_async_ui_request(s.local_command_request);
    if (command_request.working) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg,
                              ImVec4(k.brand.x, k.brand.y, k.brand.z, 0.09f));
        ImGui::PushStyleColor(ImGuiCol_Border,
                              ImVec4(k.brand.x, k.brand.y, k.brand.z, 0.36f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, ui_px(8.0f));
        if (ImGui::BeginChild("##server_command_working", ImVec2(-1, 0),
                              ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY)) {
            ImGui::PushFont(f_bold);
            ImGui::TextColored(k.brand_hov, "Sending local server command");
            ImGui::PopFont();
            if (s.local_command_target_active && !s.local_command_target.name.empty()) {
                ImGui::SameLine(0, ui_px(6.0f));
                ImGui::TextColored(k.muted, "%s", s.local_command_target.name.c_str());
            }
            ImGui::PushFont(f_small);
            ImGui::TextColored(k.muted,
                               "Amalgam is keeping the interface responsive while the command is delivered.");
            ImGui::PopFont();
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
        ImGui::Spacing();
    }

    if (s.action_failure_action.empty() && s.action_result_action.empty()) return;

    const bool failed = !s.action_failure_action.empty();
    const ImVec4 accent = failed ? k.red : k.green;
    const std::string& action = failed ? s.action_failure_action : s.action_result_action;
    const std::string& server = failed ? s.action_failure_server : s.action_result_server;
    const std::string& detail = failed ? s.action_failure_cause : s.action_result_detail;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(accent.x, accent.y, accent.z, 0.08f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(accent.x, accent.y, accent.z, 0.30f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, ui_px(8.0f));
    if (ImGui::BeginChild("##server_action_feedback", ImVec2(-1, 0),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY)) {
        ImGui::PushFont(f_bold);
        ImGui::TextColored(accent, "%s %s", action.c_str(), failed ? "failed" : "complete");
        ImGui::PopFont();
        if (!server.empty()) {
            ImGui::SameLine(0, ui_px(6.0f));
            ImGui::TextColored(k.muted, "%s", server.c_str());
        }
        ImGui::PushFont(f_small);
        ImGui::TextWrapped("%s", detail.c_str());
        ImGui::PopFont();
        ImGui::Spacing();
        if (ghost_button("Dismiss", ImVec2(ui_px(86.0f), ui_px(24.0f)))) {
            if (failed) clear_server_failure(s);
            else clear_server_action_result(s);
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
    ImGui::Spacing();
}

// ---------------------------------------------------------------------------
// Software constants
// ---------------------------------------------------------------------------

static int software_index(server::ServerSoftware software) {
    const auto& catalog = server::software_catalog();
    for (size_t i = 0; i < catalog.size(); ++i) {
        if (catalog[i].software == software) return static_cast<int>(i);
    }
    return 0;
}

static server::ServerSoftware software_at(int index) {
    const auto& catalog = server::software_catalog();
    if (index < 0 || index >= static_cast<int>(catalog.size())) return catalog.front().software;
    return catalog[static_cast<size_t>(index)].software;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static const char* stage_label(server::ServerStage stage) {
    switch (stage) {
        case server::ServerStage::NotInstalled: return "Not Installed";
        case server::ServerStage::Installing:   return "Installing";
        case server::ServerStage::Ready:        return "Ready";
        case server::ServerStage::Running:      return "Running";
        case server::ServerStage::Stopped:      return "Stopped";
        case server::ServerStage::Error:        return "Error";
        case server::ServerStage::Crashed:      return "Crashed";
    }
    return "Unknown";
}

static ImVec4 stage_color(server::ServerStage stage) {
    switch (stage) {
        case server::ServerStage::Running:      return k.green;
        case server::ServerStage::Ready:        return k.blue;
        case server::ServerStage::Installing:   return k.yellow;
        case server::ServerStage::Stopped:      return k.muted;
        case server::ServerStage::Error:        return k.red;
        case server::ServerStage::Crashed:      return {1.0f, 0.2f, 0.2f, 1.0f};
        case server::ServerStage::NotInstalled: return k.muted;
    }
    return k.muted;
}

static ImVec4 stage_bg_color(server::ServerStage stage) {
    ImVec4 c = stage_color(stage);
    c.w = 0.15f;
    return c;
}

// Keep every server-memory reading in the same human scale. The persisted
// value is MB, but a player scanning a running server benefits more from
// "3.2 GB" than from a four-digit implementation detail.
static std::string format_server_ram_mb(int megabytes) {
    const uint64_t safe_megabytes = static_cast<uint64_t>(std::max(0, megabytes));
    return format_bytes(safe_megabytes * 1024ull * 1024ull);
}

// Snapshot fixtures and a few providers report runtime duration as
// "HH:MM:SS uptime". Render that compact machine-oriented form as natural
// language, while leaving every other provider/status message untouched.
static std::string humanize_server_status_message(const std::string& message) {
    static const std::string suffix = " uptime";
    if (message.size() <= suffix.size() ||
        message.compare(message.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return message;
    }

    const std::string clock = message.substr(0, message.size() - suffix.size());
    int hours = 0;
    int minutes = 0;
    int seconds = 0;
    char trailing = '\0';
    if (std::sscanf(clock.c_str(), "%d:%d:%d%c", &hours, &minutes, &seconds, &trailing) != 3 ||
        hours < 0 || minutes < 0 || minutes >= 60 || seconds < 0 || seconds >= 60) {
        return message;
    }

    const int days = hours / 24;
    hours %= 24;
    std::string result = "Online for ";
    bool has_part = false;
    auto append_part = [&](int value, const char* singular, const char* plural) {
        if (value <= 0) return;
        if (has_part) result += " ";
        result += std::to_string(value);
        result += " ";
        result += value == 1 ? singular : plural;
        has_part = true;
    };
    append_part(days, "day", "days");
    append_part(hours, "hour", "hours");
    append_part(minutes, "minute", "minutes");
    // Seconds add useful detail for a just-started server, but are visual
    // noise once the player already has an hours/minutes answer to scan.
    if (!has_part) {
        result += std::to_string(seconds);
        result += seconds == 1 ? " second" : " seconds";
    }
    return result;
}

static const char* software_filter_name(int idx) {
    if (idx <= 0) return "All Software";
    return server::server_software_name(software_at(idx - 1));
}

static const char* status_filter_name(int idx) {
    switch (idx) {
        case 0: return "All Status";
        case 1: return "Running";
        case 2: return "Ready";
        case 3: return "Stopped";
        case 4: return "Error";
    }
    return "All";
}

static bool matches_filters(const UiState& st, const server::ServerConfig& sv,
                            int filter_status, int filter_software) {
    if (filter_status > 0) {
        const server::ServerStage stage = effective_stage(st, sv);
        server::ServerStage target;
        switch (filter_status) {
            case 1: target = server::ServerStage::Running; break;
            case 2: target = server::ServerStage::Ready; break;
            case 3: target = server::ServerStage::Stopped; break;
            case 4: target = server::ServerStage::Error; break;
            default: target = stage;
        }
        if (stage != target) return false;
    }
    if (filter_software > 0) {
        if (sv.software != software_at(filter_software - 1)) return false;
    }
    return true;
}

static const char* sort_label(int mode) {
    switch (mode) {
        case 0: return "Name";
        case 1: return "Status";
        case 2: return "Version";
        case 3: return "Port";
    }
    return "Name";
}

static std::string server_properties_path(const std::string& dir) {
    return dir + "\\server.properties";
}

static void load_server_properties(const std::string& dir,
                                   std::vector<std::pair<std::string, std::string>>& props) {
    props.clear();
    std::string path = server_properties_path(dir);
    FILE* f = nullptr;
    fopen_s(&f, path.c_str(), "r");
    if (!f) return;
    char buf[1024];
    while (fgets(buf, sizeof(buf), f)) {
        std::string line(buf);
        if (!line.empty() && line.back() == '\n') line.pop_back();
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq != std::string::npos) {
            props.emplace_back(line.substr(0, eq), line.substr(eq + 1));
        }
    }
    fclose(f);
}

static void save_server_properties(const std::string& dir,
                                   const std::vector<std::pair<std::string, std::string>>& props) {
    std::string path = server_properties_path(dir);
    FILE* f = nullptr;
    fopen_s(&f, path.c_str(), "w");
    if (!f) return;
    fprintf(f, "# Server properties generated by Amalgam\n");
    for (const auto& kv : props) {
        fprintf(f, "%s=%s\n", kv.first.c_str(), kv.second.c_str());
    }
    fclose(f);
}

// ---------------------------------------------------------------------------
// Downloading a server's files (the page's own job)
// ---------------------------------------------------------------------------

// One runtime download at a time. The launcher installs the runtime the page
// asks for, and the outcome is written onto the server's own record so the
// badge, the buttons and the next Start all read the same truth.
static void start_provision(UiState& st, int server_index) {
    auto& s = state();
    auto& job = runtime_download();
    if (job.active.load()) return;
    if (server_index < 0 || server_index >= static_cast<int>(st.servers.size())) return;

    const server::ServerConfig cfg = st.servers[server_index];

    // A manual runtime has nothing to download, so the page states what to add
    // instead of starting a job that can only fail.
    if (server::server_software_kind(cfg.software) == server::RuntimeKind::Manual) {
        aml::server_providers::RuntimeArtifact artifact;
        std::string hint;
        aml::server_providers::resolve_runtime(cfg.software, cfg.minecraft_version, artifact,
                                               &hint);
        st.servers[server_index].stage = server::ServerStage::NotInstalled;
        st.servers[server_index].status_message =
            artifact.manual_hint.empty() ? hint : artifact.manual_hint;
        save_local_servers(st.servers);
        clear_server_failure(s);
        return;
    }

    job.cancel.store(false);
    job.cancellable.store(false);
    job.cancellation_observed.store(false);
    job.progress.store(-1.0f);  // unknown until the transfer reports a size
    {
        std::lock_guard<std::mutex> lock(job.mu);
        job.server = cfg.name;
        job.phase = "Preparing";
    }
    job.active.store(true);
    st.servers[server_index].stage = server::ServerStage::Installing;
    st.servers[server_index].status_message.clear();
    save_local_servers(st.servers);
    clear_server_failure(s);

    spawn_worker(st, std::thread([&job, cfg]() {
        // Existing/imported Java servers can only be prepared if their local
        // eula.txt already records the acknowledgement. New servers receive
        // that file from the explicit create-dialog confirmation before this
        // worker starts. Check before even resolving an installer Java runtime;
        // the provisioning API validates the same fact again before providers.
        const bool eula_accepted = aml::server_provision::has_accepted_eula(cfg);
        // A loader installer has to run with the same Java the server itself
        // will start with, so the version is resolved through the supervisor's
        // single Java root rather than a second path policy.
        std::wstring java_path;
        std::string java_error;
        if (eula_accepted &&
            server::server_software_kind(cfg.software) == server::RuntimeKind::Installer) {
            java_path = aml::services::local_server_manager()->resolve_local_server_java(
                cfg.minecraft_version, &java_error);
        }

        auto phase_sink = [&job](const std::string& text) {
            // The provisioning contract emits "Downloading ..." only around
            // net::download, the one phase whose progress callback can honour
            // a cancel request. All other phases are truthfully non-cancellable.
            job.cancellable.store(text.rfind("Downloading ", 0) == 0);
            std::lock_guard<std::mutex> lock(job.mu);
            job.phase = text;
        };
        auto progress_sink = [&job](uint64_t done, uint64_t total) {
            if (job.cancel.load() && job.cancellable.load()) {
                job.cancellation_observed.store(true);
                return false;
            }
            if (total > 0)
                job.progress.store(static_cast<float>(static_cast<double>(done) /
                                                       static_cast<double>(total)));
            return true;
        };

        std::string error;
        bool ok = aml::server_provision::provision_runtime(cfg, java_path, eula_accepted,
                                                           progress_sink, phase_sink, &error);
        job.cancellable.store(false);

        if (ok) {
            // eula.txt and server.properties belong to a prepared server, so
            // they are written (or rewritten) once the runtime is in place.
            std::string config_error;
            if (!aml::server_provision::write_server_config_files(cfg, eula_accepted,
                                                                    &config_error)) {
                error = config_error;
                ok = false;
            }
        }
        if (ok && !aml::server_provision::verify_runnable_runtime(cfg, &error)) {
            // Runtime delivery and config writes may succeed while an installer
            // still leaves no startable target. Do not let that become Ready.
            ok = false;
        }
        const bool cancelled = !ok && job.cancellation_observed.load();

        {
            std::lock_guard<std::mutex> lock(job.mu);
            job.success = ok && error.empty();
            job.cancelled = cancelled;
            job.outcome = job.success ? std::string()
                                      : (cancelled ? std::string() : error);
        }
        job.finished.store(true);
        job.active.store(false);
    }));
}

// Applies a finished download to the server the page owns. Runs on the UI
// thread, so the server list is only ever written from one place.
static void apply_finished_download(UiState& st) {
    auto& s = state();
    auto& job = runtime_download();
    if (job.active.load() || !job.finished.exchange(false)) return;

    std::string server_name;
    std::string outcome;
    bool success = false;
    bool cancelled = false;
    {
        std::lock_guard<std::mutex> lock(job.mu);
        server_name = job.server;
        outcome = job.outcome;
        success = job.success;
        cancelled = job.cancelled;
        job.phase.clear();
    }

    for (auto& sv : st.servers) {
        if (sv.name != server_name) continue;
        if (success) {
            // Final UI-thread guard for a runtime deleted or left incomplete
            // after the worker's verification but before the Ready badge.
            std::string runtime_error;
            if (!aml::server_provision::verify_runnable_runtime(sv, &runtime_error)) {
                success = false;
                outcome = runtime_error;
            }
        }
        if (success) {
            sv.stage = server::ServerStage::Ready;
            sv.status_message.clear();
        } else if (cancelled) {
            sv.stage = server::ServerStage::NotInstalled;
            sv.status_message = "Download cancelled before the server files were complete.";
        } else {
            sv.stage = server::ServerStage::Error;
            sv.status_message = outcome.empty() ? "The server files could not be installed."
                                                : outcome;
        }
        break;
    }
    save_local_servers(st.servers);

    if (!success && !cancelled) {
        report_server_failure(s, st, "Prepare server files", server_name,
                              outcome.empty() ? "download failed" : outcome);
    }
}

static std::string download_phase() {
    auto& job = runtime_download();
    std::lock_guard<std::mutex> lock(job.mu);
    return job.phase;
}

static bool download_can_cancel() {
    return runtime_download().cancellable.load();
}

// Whether the job in flight is this server's, so the progress card and the
// Cancel button only appear on the server the user started.
static bool downloading_runtime_for(const std::string& server_name) {
    auto& job = runtime_download();
    if (!job.active.load()) return false;
    std::lock_guard<std::mutex> lock(job.mu);
    return job.server == server_name;
}

// ---------------------------------------------------------------------------
// Live version lists
// ---------------------------------------------------------------------------

// Fetches this software's real version list in the background and keeps the
// reply only if it still describes what the dialog is showing. A provider that
// cannot be reached is reported, not silently replaced with a made-up list.
static void request_software_versions(UiState& st, int software_index) {
    auto& s = state();
    const server::ServerSoftware software = software_at(software_index);
    {
        std::lock_guard<std::mutex> lock(s.version_mu);
        if (s.version_software == software_index &&
            (s.version_loading || s.version_live || !s.version_error.empty()))
            return;
        s.version_software = software_index;
        s.version_list.clear();
        s.version_live = false;
        s.version_loading = true;
        s.version_error.clear();
    }

    spawn_worker(st, std::thread([&s, software, software_index]() {
        std::vector<std::string> versions;
        std::string error;
        const bool ok = aml::server_providers::software_versions(software, versions, &error);
        std::lock_guard<std::mutex> lock(s.version_mu);
        if (s.version_software != software_index) return;
        s.version_loading = false;
        if (ok && !versions.empty()) {
            s.version_list = std::move(versions);
            s.version_live = true;
        } else {
            s.version_error = error.empty()
                ? std::string(server::server_software_name(software)) +
                      " did not report any versions."
                : error;
        }
    }));
}

// ---------------------------------------------------------------------------
// Draw helpers
// ---------------------------------------------------------------------------

static void draw_status_badge(server::ServerStage stage) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec4 sc = stage_color(stage);
    ImVec4 bg = stage_bg_color(stage);
    draw_badge(dl, pos, stage_label(stage), sc, bg);
    ImGui::Dummy(ImVec2(
        ImGui::CalcTextSize(stage_label(stage)).x + ui_px(14.0f),
        ui_px(20.0f)));
}

static void draw_server_card(server::ServerConfig& sv, int index, UiState& st) {
    auto& s = state();
    ImGui::PushID(index);

    const server::ServerStage stage = effective_stage(st, sv);
    bool is_running = (stage == server::ServerStage::Running);
    bool is_ready = (stage == server::ServerStage::Ready ||
                     stage == server::ServerStage::Stopped ||
                     stage == server::ServerStage::NotInstalled);
    refresh_local_server_metrics(st, sv);

    // ── Card with left status strip ──────────────────────────────────
    card_begin(("##srv_" + std::to_string(index)).c_str(), ImVec2(-1, 0));

    // Left status strip
    ImVec2 card_min = ImGui::GetCursorScreenPos();
    ImVec2 card_content_max = ImGui::GetContentRegionMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec4 strip_color = stage_color(stage);
    dl->AddRectFilled(
        card_min,
        ImVec2(card_min.x + ui_px(4.0f), card_min.y + ui_px(76.0f)),
        c32(strip_color), ui_px(2.0f));

    // A real visual anchor makes the server list scan like the launcher board
    // instead of a settings form. Per-server artwork can replace this shared
    // launcher scene later without changing the layout contract.
    const float thumbnail_size = ui_px(72.0f);
    const ImVec2 thumbnail_pos = card_min + ImVec2(ui_px(12.0f), 0.0f);
    draw_local_image(st, st.exe_dir + L"\\branding\\ai\\server-card-ai-v2.png",
                     thumbnail_pos, ImVec2(thumbnail_size, thumbnail_size),
                     c32(k.brand_dk));
    dl->AddRect(thumbnail_pos, thumbnail_pos + ImVec2(thumbnail_size, thumbnail_size),
                c32(k.border), ui_px(8.0f));
    ImGui::SetCursorScreenPos(
        ImVec2(thumbnail_pos.x + thumbnail_size + ui_px(14.0f), thumbnail_pos.y));

    // ── Header row: name + badges (clickable to open detail) ────────
    ImGui::PushFont(f_bold);
    ImGui::TextColored(k.text, "%s", sv.name.c_str());
    ImGui::PopFont();
    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    if (ImGui::IsItemClicked()) {
        s.detail_server_idx = index;
        s.detail_tab = 0;
        s.file_list_dirty = true;
        s.players_dirty = true;
        s.world_dirty = true;
    }

    ImGui::SameLine(0, ui_px(8.0f));
    draw_status_badge(stage);

    ImGui::SameLine(0, ui_px(6.0f));
    {
        ImDrawList* dl2 = ImGui::GetWindowDrawList();
        ImVec2 bp = ImGui::GetCursorScreenPos();
        ImVec4 sw_bg = k.brand;
        sw_bg.w = 0.15f;
        draw_badge(dl2, bp, server::server_software_name(sv.software), k.brand, sw_bg);
        ImGui::Dummy(ImVec2(
            ImGui::CalcTextSize(server::server_software_name(sv.software)).x + ui_px(14.0f),
            ui_px(20.0f)));
    }

    ImGui::SameLine(0, ui_px(6.0f));
    {
        ImDrawList* dl3 = ImGui::GetWindowDrawList();
        ImVec2 bp = ImGui::GetCursorScreenPos();
        ImVec4 ver_bg = k.blue;
        ver_bg.w = 0.12f;
        draw_badge(dl3, bp, sv.minecraft_version.c_str(), k.blue, ver_bg);
        ImGui::Dummy(ImVec2(
            ImGui::CalcTextSize(sv.minecraft_version.c_str()).x + ui_px(14.0f),
            ui_px(20.0f)));
    }

    // ── Info row (stat chips) ───────────────────────────────────────
    // Pin metadata to the free space beside the thumbnail. Previously this
    // depended on Dear ImGui's SameLine wrapping and could jump beneath the
    // image at compact sizes, making the server cards look broken.
    ImGui::SetCursorScreenPos(ImVec2(thumbnail_pos.x + thumbnail_size + ui_px(14.0f),
                                     thumbnail_pos.y + ui_px(34.0f)));
    auto meta_chip = [&](const char* text, const ImVec4& accent) {
        const ImVec2 bp = ImGui::GetCursorScreenPos();
        const ImVec2 sz = ImGui::CalcTextSize(text) + ImVec2(ui_px(14.0f), ui_px(8.0f));
        ImVec4 bg = accent; bg.w = 0.12f;
        dl->AddRectFilled(bp, bp + sz, c32(bg), ui_px(6.0f));
        dl->AddText(bp + ImVec2(ui_px(7.0f), ui_px(4.0f)), c32(accent), text);
        ImGui::Dummy(sz + ImVec2(0, ui_px(4.0f)));
    };
    std::string port_chip = "Port " + std::to_string(sv.port);
    meta_chip(port_chip.c_str(), k.muted);
    ImGui::SameLine(0, ui_px(6.0f));
    std::string ram_chip = "RAM " + std::to_string(sv.allocated_ram_mb) + " MB";
    meta_chip(ram_chip.c_str(), k.blue);
    ImGui::SameLine(0, ui_px(6.0f));
    std::string max_chip = "Max " + std::to_string(sv.max_players) + " players";
    meta_chip(max_chip.c_str(), k.green);

    if (!is_running && !sv.status_message.empty()) {
        ImGui::Spacing();
        const ImVec4 sc = (stage == server::ServerStage::Error) ? k.red : k.muted;
        ImGui::TextColored(sc, "%s", sv.status_message.c_str());
    }

    // Return subsequent full-width rows to the content origin and keep them
    // below the thumbnail, even when metadata wraps at a compact width.
    {
        const float content_bottom = ImGui::GetCursorScreenPos().y;
        ImGui::SetCursorScreenPos(ImVec2(card_min.x,
            std::max(content_bottom, thumbnail_pos.y + thumbnail_size + ui_px(4.0f))));
    }

    // ── Running metrics ─────────────────────────────────────────────
    if (is_running) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextColored(k.green, "\xe2\x97\x8f");
        ImGui::SameLine();
        ImGui::TextColored(k.green, "Online");
        if (!sv.status_message.empty()) {
            ImGui::SameLine(0, ui_px(12.0f));
            ImGui::TextColored(k.muted, "%s", sv.status_message.c_str());
        }

        // Inline metric bars
        auto& metrics = st.server_metrics;
        if (metrics.valid) {
            ImGui::Spacing();
            ImVec2 bar_pos = ImGui::GetCursorScreenPos();
            float bar_w = ImGui::GetContentRegionAvail().x;
            float bar_h = ui_px(5.0f);
            float bar_y = bar_pos.y;
            ImDrawList* metrics_dl = ImGui::GetWindowDrawList();

            // RAM bar
            float ram_pct = std::clamp(metrics.ram_percent / 100.0f, 0.0f, 1.0f);
            ImVec4 ram_col = ram_pct > 0.9f ? k.red : ram_pct > 0.7f ? k.orange : k.blue;
            metrics_dl->AddRectFilled(ImVec2(bar_pos.x, bar_y), ImVec2(bar_pos.x + bar_w, bar_y + bar_h), c32(k.surface2), bar_h * 0.5f);
            if (metrics.ram_valid)
                metrics_dl->AddRectFilled(ImVec2(bar_pos.x, bar_y), ImVec2(bar_pos.x + bar_w * ram_pct, bar_y + bar_h), c32(ram_col), bar_h * 0.5f);
            ImGui::PushFont(f_small);
            const std::string ram_label = metrics.ram_valid
                ? "RAM " + std::to_string(static_cast<int>(metrics.ram_percent)) + "%"
                : "RAM unavailable";
            metrics_dl->AddText(ImVec2(bar_pos.x, bar_y + bar_h + ui_px(2.0f)), c32(k.muted), ram_label.c_str());
            ImGui::PopFont();

            // CPU bar
            float cpu_pct = std::clamp(metrics.cpu_percent / 100.0f, 0.0f, 1.0f);
            ImVec4 cpu_col = cpu_pct > 0.9f ? k.red : cpu_pct > 0.7f ? k.orange : k.green;
            float cpu_y = bar_y + bar_h + ui_px(16.0f);
            metrics_dl->AddRectFilled(ImVec2(bar_pos.x, cpu_y), ImVec2(bar_pos.x + bar_w, cpu_y + bar_h), c32(k.surface2), bar_h * 0.5f);
            if (metrics.cpu_valid)
                metrics_dl->AddRectFilled(ImVec2(bar_pos.x, cpu_y), ImVec2(bar_pos.x + bar_w * cpu_pct, cpu_y + bar_h), c32(cpu_col), bar_h * 0.5f);
            ImGui::PushFont(f_small);
            const std::string cpu_label = metrics.cpu_valid
                ? "CPU " + std::to_string(static_cast<int>(metrics.cpu_percent)) + "%"
                : "CPU unavailable";
            metrics_dl->AddText(ImVec2(bar_pos.x, cpu_y + bar_h + ui_px(2.0f)), c32(k.muted), cpu_label.c_str());
            ImGui::PopFont();

            // TPS + Players row
            float row3_y = cpu_y + bar_h + ui_px(16.0f);
            ImGui::PushFont(f_small);
            const std::string tps_label = metrics.tps_valid
                ? "TPS " + std::to_string(metrics.tps) : "TPS unavailable";
            const std::string players_label = metrics.players_valid
                ? std::to_string(metrics.players_online) + "/" + std::to_string(sv.max_players) + " online"
                : "Players unavailable";
            metrics_dl->AddText(ImVec2(bar_pos.x, row3_y), c32(metrics.tps_valid ? k.green : k.muted), tps_label.c_str());
            metrics_dl->AddText(ImVec2(bar_pos.x + bar_w * 0.5f, row3_y), c32(k.muted), players_label.c_str());
            ImGui::PopFont();
            ImGui::Dummy(ImVec2(0, row3_y - bar_pos.y + ui_px(20.0f)));
        }
    }

    // ── Action buttons ──────────────────────────────────────────────
    // Keep the command row visually attached to the server identity. Full
    // default Spacing calls left stopped servers looking like unfinished,
    // stretched rows at desktop widths.
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ui_px(3.0f));
    ImGui::Separator();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ui_px(3.0f));

    float bw = ui_px(80.0f);
    float bh = ui_px(28.0f);

    const bool local_action_busy = local_server_action_lane_busy(s) ||
                                   local_server_command_lane_busy(s);
    const bool start_working = local_server_action_is_working(s, LocalServerAction::Start);
    const bool stop_working = local_server_action_is_working(s, LocalServerAction::Stop);
    const bool restart_working = local_server_action_is_working(s, LocalServerAction::Restart);
    if (is_running) {
        if (ghost_button(stop_working ? "Stopping..." : "Stop", ImVec2(bw, bh),
                         local_action_busy)) {
            start_local_server_action(st, index, LocalServerAction::Stop);
        }
        ImGui::SameLine(0, ui_px(4.0f));
        if (ghost_button(restart_working ? "Restarting..." : "Restart",
                         ImVec2(bw + ui_px(10.0f), bh), local_action_busy)) {
            start_local_server_action(st, index, LocalServerAction::Restart);
        }
    } else if (downloading_runtime_for(sv.name)) {
        if (download_can_cancel()) {
            if (ghost_button("Cancel download", ImVec2(bw + ui_px(20.0f), bh)))
                runtime_download().cancel.store(true);
        } else {
            ImGui::BeginDisabled();
            ghost_button("Working", ImVec2(bw + ui_px(20.0f), bh));
            ImGui::EndDisabled();
        }
    } else if (!runtime_files_present(sv) &&
               server::server_software_kind(sv.software) != server::RuntimeKind::Manual) {
        // Nothing to start yet: this is the download the server needs, offered
        // where the Start button would be.
        if (primary_button("Prepare", ImVec2(bw + ui_px(10.0f), bh))) start_provision(st, index);
    } else if (is_ready) {
        if (primary_button(start_working ? "Starting..." : "Start", ImVec2(bw, bh),
                           start_working, local_action_busy)) {
            start_local_server_action(st, index, LocalServerAction::Start);
        }
    }

    // Manage button always visible
    ImGui::SameLine(0, ui_px(4.0f));
    if (ghost_button("Manage", ImVec2(bw + ui_px(20.0f), bh))) {
        s.detail_server_idx = index;
        s.detail_tab = 0;
        s.file_list_dirty = true;
        s.players_dirty = true;
        s.world_dirty = true;
    }

    if (!sv.server_directory.empty()) {
        ImGui::SameLine(0, ui_px(4.0f));
        if (ghost_button("Folder", ImVec2(bw, bh))) {
            ShellExecuteW(nullptr, L"open",
                aml::net::to_wide(sv.server_directory).c_str(),
                nullptr, nullptr, SW_SHOWNORMAL);
        }
    }

    ImGui::SameLine(0, ui_px(4.0f));
    if (ghost_button("Remove", ImVec2(bw, bh), local_action_busy)) {
        s.action_pending = index;
        s.remove_server_error.clear();
    }

    card_end();
    ImGui::PopID();
}

// ---------------------------------------------------------------------------
// Console viewer
// ---------------------------------------------------------------------------

static void draw_console_panel(UiState& st) {
    auto& s = state();
    if (!s.console_open || s.console_server_idx < 0 ||
        s.console_server_idx >= static_cast<int>(st.servers.size())) {
        return;
    }

    auto& sv = st.servers[s.console_server_idx];

    // This legacy floating console is not the normal snapshot route, but keep
    // the same hard boundary if a fixture ever reaches it through retained UI
    // state.  In particular, do not let a fixture fall through to send_command.
    if (st.fixture_mode) {
        ImGui::SetNextWindowSize(ImVec2(ui_px(600), ui_px(400)), ImGuiCond_Appearing);
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                                ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (ImGui::Begin("Server Console", &s.console_open,
                         ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::PushFont(f_h2);
            ImGui::TextColored(k.text, "Console: %s", sv.name.c_str());
            ImGui::PopFont();
            ImGui::SameLine(0, ui_px(12.0f));
            draw_status_badge(sv.stage);
            ImGui::TextColored(k.brand_hov,
                               "Visual fixture — static local sample; console reading and commands are disabled.");
            ImGui::Spacing();
            ImGui::BeginDisabled();
            std::string fixture_filter;
            ImGui::SetNextItemWidth(ui_px(200.0f));
            input_text_hint("##fixture_console_filter", "Filter logs...", &fixture_filter);
            ImGui::SameLine(0, ui_px(8.0f));
            ghost_button("Copy", ImVec2(ui_px(60.0f), ui_px(24.0f)));
            ImGui::SameLine(0, ui_px(4.0f));
            ghost_button("Clear", ImVec2(ui_px(60.0f), ui_px(24.0f)));
            ImGui::EndDisabled();
            ImGui::Spacing();
            if (ImGui::BeginChild("##fixture_console_log", ImVec2(-1, -ui_px(48.0f)),
                                  ImGuiChildFlags_Borders)) {
                ImGui::TextColored(k.muted, "[10:14:08]");
                ImGui::SameLine();
                ImGui::TextColored(k.green, "INFO  Representative fixture console ready");
                ImGui::TextColored(k.muted, "[10:16:28]");
                ImGui::SameLine();
                ImGui::TextColored(k.green, "INFO  Saved the representative game state");
            }
            ImGui::EndChild();
            std::string fixture_command = "say Welcome";
            ImGui::BeginDisabled();
            ImGui::SetNextItemWidth(-ui_px(80.0f) - ui_px(4.0f));
            ImGui::InputText("##fixture_console_cmd", &fixture_command);
            ImGui::SameLine();
            primary_button("Send", ImVec2(ui_px(76.0f), ImGui::GetFrameHeight()));
            ImGui::EndDisabled();
        }
        ImGui::End();
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(ui_px(600), ui_px(400)), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::Begin("Server Console", &s.console_open,
                     ImGuiWindowFlags_NoSavedSettings)) {
        // Header
        ImGui::PushFont(f_h2);
        ImGui::TextColored(k.text, "Console: %s", sv.name.c_str());
        ImGui::PopFont();
        ImGui::SameLine(0, ui_px(12.0f));
        draw_status_badge(effective_stage(st, sv));

        ImGui::Spacing();

        // Filter
        ImGui::SetNextItemWidth(ui_px(200.0f));
        input_text_hint("##console_filter", "Filter logs...", &s.console_filter);

        ImGui::SameLine(0, ui_px(8.0f));
        if (ghost_button("Copy", ImVec2(ui_px(60.0f), ui_px(24.0f)))) {
            std::string copy_text;
            for (const auto& entry : s.console_log)
                copy_text += "[" + entry.timestamp + "] " + entry.message + "\n";
            if (!copy_text.empty()) ImGui::SetClipboardText(copy_text.c_str());
        }
        ImGui::SameLine(0, ui_px(4.0f));
        if (ghost_button("Clear", ImVec2(ui_px(60.0f), ui_px(24.0f)))) {
            s.console_log.clear();
        }
        ImGui::SameLine(0, ui_px(4.0f));
        if (ghost_button(s.console_auto_scroll ? "Auto-scroll ON" : "Auto-scroll OFF",
                         ImVec2(ui_px(110.0f), ui_px(24.0f)))) {
            s.console_auto_scroll = !s.console_auto_scroll;
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Log area
        float footer_h = ImGui::GetFrameHeightWithSpacing() + ui_px(4.0f);
        if (ImGui::BeginChild("##console_log",
                ImVec2(-1, -(footer_h)), ImGuiChildFlags_Borders)) {
            for (const auto& entry : s.console_log) {
                if (!s.console_filter.empty() &&
                    entry.message.find(s.console_filter) == std::string::npos)
                    continue;

                // Color code by level
                ImVec4 log_color = k.muted;
                if (entry.message.find("ERROR") != std::string::npos ||
                    entry.message.find("SEVERE") != std::string::npos)
                    log_color = k.red;
                else if (entry.message.find("WARN") != std::string::npos)
                    log_color = k.yellow;
                else if (entry.message.find("INFO") != std::string::npos)
                    log_color = k.green;

                ImGui::TextColored(k.muted, "[%s]", entry.timestamp.c_str());
                ImGui::SameLine();
                ImGui::TextColored(log_color, "%s", entry.message.c_str());
            }

            if (s.console_auto_scroll &&
                ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
                ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();

        ImGui::Spacing();

        const auto submit_console_command = [&] {
            if (s.console_input.empty()) return;
            const std::string command = s.console_input;
            if (!start_local_server_command(st, sv, LocalServerCommandOrigin::PopupConsole,
                                            command)) {
                return;
            }
            server::ServerConsoleEntry cmd_entry;
            cmd_entry.timestamp = ">";
            cmd_entry.message = command;
            s.console_log.push_back(std::move(cmd_entry));
            s.console_input.clear();
        };
        const bool command_busy = local_server_action_lane_busy(s) ||
                                  local_server_command_lane_busy(s);

        // Command input
        if (command_busy) ImGui::BeginDisabled();
        ImGui::SetNextItemWidth(-ui_px(80.0f) - ui_px(4.0f));
        if (ImGui::InputText("##console_cmd", &s.console_input,
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            submit_console_command();
            ImGui::SetKeyboardFocusHere(-1);
        }
        ImGui::SameLine();
        const bool sending = local_server_command_is_working(
            s, LocalServerCommandOrigin::PopupConsole);
        if (primary_button(sending ? "Sending..." : "Send",
                           ImVec2(ui_px(76.0f), ImGui::GetFrameHeight()),
                           false, command_busy)) {
            submit_console_command();
        }
        if (command_busy) ImGui::EndDisabled();
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Properties editor
// ---------------------------------------------------------------------------

static void draw_properties_panel(UiState& st) {
    auto& s = state();
    if (!s.properties_open || s.properties_server_idx < 0 ||
        s.properties_server_idx >= static_cast<int>(st.servers.size())) {
        return;
    }

    auto& sv = st.servers[s.properties_server_idx];

    ImGui::SetNextWindowSize(ImVec2(ui_px(500), ui_px(450)), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::Begin("Server Properties", &s.properties_open,
                     ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::PushFont(f_h2);
        ImGui::TextColored(k.text, "Properties: %s", sv.name.c_str());
        ImGui::PopFont();

        ImGui::Spacing();

        if (s.properties.empty()) {
            empty_state("No server.properties found",
                        "Start the server once to generate the properties file.");
        } else {
            ImGui::TextColored(k.muted, "%d properties", (int)s.properties.size());
            ImGui::Spacing();

            float footer_h = ImGui::GetFrameHeightWithSpacing() + ui_px(8.0f);
            if (ImGui::BeginChild("##props_list",
                    ImVec2(-1, -(footer_h)), ImGuiChildFlags_Borders)) {
                for (size_t i = 0; i < s.properties.size(); ++i) {
                    auto& [key, val] = s.properties[i];
                    ImGui::PushID(static_cast<int>(i));

                    ImGui::SetNextItemWidth(ui_px(180.0f));
                    ImGui::TextColored(k.brand, "%s", key.c_str());
                    ImGui::SameLine(ui_px(184.0f));
                    ImGui::SetNextItemWidth(-1);

                    // Special handling for known boolean properties
                    bool is_bool = (key == "online-mode" || key == "pvp" ||
                                    key == "enable-command-block" || key == "spawn-monsters" ||
                                    key == "spawn-animals" || key == "spawn-npcs" ||
                                    key == "white-list" || key == "spawn-protection" &&
                                    val == "0" || val == "1" ||
                                    key == "enforce-whitelist");
                    if (is_bool) {
                        bool bval = (val == "true");
                        if (ImGui::Checkbox("##val", &bval)) {
                            val = bval ? "true" : "false";
                            s.properties_dirty = true;
                        }
                    } else {
                        char buf[512];
                        strncpy(buf, val.c_str(), sizeof(buf) - 1);
                        buf[sizeof(buf) - 1] = '\0';
                        if (ImGui::InputText("##val", buf, sizeof(buf))) {
                            val = buf;
                            s.properties_dirty = true;
                        }
                    }

                    ImGui::PopID();
                }
            }
            ImGui::EndChild();

            ImGui::Spacing();
            if (s.properties_dirty) {
                if (primary_button("Save Properties",
                                   ImVec2(ui_px(130.0f), ui_px(30.0f)))) {
                    save_server_properties(sv.server_directory, s.properties);
                    s.properties_dirty = false;
                }
                ImGui::SameLine();
            }
            if (ghost_button("Reload", ImVec2(ui_px(80.0f), ui_px(30.0f)))) {
                load_server_properties(sv.server_directory, s.properties);
                s.properties_dirty = false;
            }
        }
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Server file browser helpers
// ---------------------------------------------------------------------------

static void scan_server_directory(ServerUIState& s, const std::string& dir) {
    s.file_entries.clear();
    s.file_is_dir.clear();
    namespace fs = std::filesystem;
    std::error_code ec;
    std::string full = dir;
    if (!s.file_browse_path.empty()) {
        full += "\\" + s.file_browse_path;
    }
    for (auto& entry : fs::directory_iterator(aml::net::to_wide(full), ec)) {
        const auto name_u8 = entry.path().filename().u8string();
        s.file_entries.emplace_back(reinterpret_cast<const char*>(name_u8.c_str()));
        std::error_code tec;
        s.file_is_dir.push_back(entry.is_directory(tec));
    }
    // Sort: directories first, then alphabetical
    for (size_t i = 0; i < s.file_entries.size(); ++i) {
        for (size_t j = i + 1; j < s.file_entries.size(); ++j) {
            bool swap_needed = false;
            if (s.file_is_dir[i] && !s.file_is_dir[j]) continue;
            if (!s.file_is_dir[i] && s.file_is_dir[j]) swap_needed = true;
            else if (s.file_entries[i] > s.file_entries[j]) swap_needed = true;
            if (swap_needed) {
                std::string tmp_name = s.file_entries[i];
                s.file_entries[i] = s.file_entries[j];
                s.file_entries[j] = tmp_name;
                bool tmp_dir = s.file_is_dir[i];
                s.file_is_dir[i] = s.file_is_dir[j];
                s.file_is_dir[j] = tmp_dir;
            }
        }
    }
}

static std::string file_full_path(ServerUIState& s, const std::string& base_dir,
                                   const std::string& filename) {
    std::string path = base_dir;
    if (!s.file_browse_path.empty()) path += "\\" + s.file_browse_path;
    if (!filename.empty()) path += "\\" + filename;
    return path;
}

// Read a text file into a string (truncated to 64 KB for display).
static std::string read_text_file(const std::string& path, size_t max_bytes = 65536) {
    FILE* f = nullptr;
    fopen_s(&f, path.c_str(), "rb");
    if (!f) return "(unable to open file)";
    std::string out;
    out.resize(max_bytes);
    size_t n = fread(out.data(), 1, max_bytes, f);
    fclose(f);
    out.resize(n);
    return out;
}

// Format file size nicely.
static std::string file_size_label(uint64_t bytes) {
    if (bytes < 1024) return std::to_string(bytes) + " B";
    if (bytes < 1024 * 1024)
        return std::to_string(bytes / 1024) + "." +
               std::to_string((bytes % 1024) / 102) + " KB";
    if (bytes < 1024ULL * 1024 * 1024)
        return std::to_string(bytes / (1024 * 1024)) + "." +
               std::to_string((bytes % (1024 * 1024)) / (1024 * 102)) + " MB";
    return std::to_string(bytes / (1024ULL * 1024 * 1024)) + "." +
           std::to_string((bytes % (1024ULL * 1024 * 1024)) / (1024ULL * 1024 * 102)) + " GB";
}

static const char* fixture_server_properties_preview_text() {
    return R"(# Amalgam visual fixture — static sample; no server file was read.
# This representative text exists only for layout and scroll review.
accepts-transfers=false
allow-flight=false
allow-nether=true
broadcast-console-to-ops=true
broadcast-rcon-to-ops=true
bug-report-link=
difficulty=normal
enable-command-block=false
enable-jmx-monitoring=false
enable-query=false
enable-rcon=false
enable-status=true
enforce-secure-profile=true
enforce-whitelist=false
entity-broadcast-range-percentage=100
force-gamemode=false
function-permission-level=2
gamemode=survival
generate-structures=true
generator-settings={}
hardcore=false
hide-online-players=false
level-name=world
level-seed=
level-type=minecraft:normal
log-ips=true
max-chained-neighbor-updates=1000000
max-players=12
max-tick-time=60000
max-world-size=29999984
motd=Forsaken World SMP
network-compression-threshold=256
online-mode=true
op-permission-level=4
player-idle-timeout=0
prevent-proxy-connections=false
pvp=true
query.port=25565
rate-limit=0
rcon.password=
rcon.port=25575
region-file-compression=deflate
require-resource-pack=false
resource-pack=
resource-pack-prompt=
server-ip=
server-port=25565
simulation-distance=10
spawn-animals=true
spawn-monsters=true
spawn-npcs=true
spawn-protection=16
sync-chunk-writes=true
text-filtering-config=
use-native-transport=true
view-distance=10
white-list=false
)";
}

// The regular preview window is fed by a real file and can offer Explorer.
// Fixture preview deliberately uses neither path: its content is a local
// literal and every externally meaningful action stays disabled.
static void draw_fixture_server_file_preview(ServerUIState& s, UiState& st) {
    if (!s.file_preview_open) return;

    const std::string display_name = s.file_preview_name.empty()
        ? "server.properties" : s.file_preview_name;
    const std::string window_name =
        "File: " + display_name + "###fixture_server_file_preview";
    ImGui::SetNextWindowSize(ImVec2(
        std::min(ui_px(620.0f), ImGui::GetMainViewport()->WorkSize.x - ui_px(40.0f)),
        std::min(ui_px(470.0f), ImGui::GetMainViewport()->WorkSize.y - ui_px(40.0f))),
        ImGuiCond_Appearing);
    if (ImGui::Begin(window_name.c_str(), &s.file_preview_open,
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse)) {
        ImGui::PushFont(f_h2);
        ImGui::TextColored(k.text, "%s", display_name.c_str());
        ImGui::PopFont();
        ImGui::TextColored(k.brand_hov,
                           "Visual fixture — static server.properties text; no file or Explorer action is used.");
        ImGui::Spacing();

        // Use a fixed, component-owned viewport rather than a remaining-space
        // height: fixtures need a reliable range for @middle and @bottom even
        // while their containing page has a separate scroll host.
        const float preview_body_height = std::min(
            ui_px(286.0f), std::max(ui_px(170.0f),
                                    ImGui::GetContentRegionAvail().y - ui_px(66.0f)));
        ImGui::PushFont(f_mono);
        if (ImGui::BeginChild("##fixture_server_file_preview_body",
                              ImVec2(-1, preview_body_height), ImGuiChildFlags_Borders)) {
            ImGui::TextUnformatted(fixture_server_properties_preview_text());
            // Repeat after this child has laid out its static text. That keeps
            // capture scrolling deterministic without synthesizing input.
            if (st.fixture_scroll_position > 0) {
                const float max_scroll = ImGui::GetScrollMaxY();
                if (max_scroll > 0.0f) {
                    const float requested_scroll = st.fixture_scroll_position == 1
                        ? max_scroll * 0.5f : max_scroll;
                    ImGui::SetScrollY(requested_scroll);
                }
            }
        }
        ImGui::EndChild();
        ImGui::PopFont();

        ImGui::Spacing();
        ghost_button("Open in Explorer", ImVec2(ui_px(130.0f), ui_px(28.0f)), true);
        ImGui::SameLine(0, ui_px(8.0f));
        if (ghost_button("Close preview", ImVec2(ui_px(116.0f), ui_px(28.0f)))) {
            s.file_preview_open = false;
        }
    }
    ImGui::End();
}

// Render the file-row context menu as a fully inert, deterministic fixture.
// It deliberately does not share the live file-list popup below: that popup
// resolves real paths for preview and Explorer actions.  Keeping this branch
// self-contained makes the visual evidence truthful even when a review runs
// on a machine that has a same-named local server.
static void draw_fixture_server_file_context_menu(ServerUIState& s) {
    if (!s.fixture_file_context_menu_open) return;

    const std::string display_name = s.fixture_file_context_menu_name.empty()
        ? "server.properties" : s.fixture_file_context_menu_name;
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    ImGui::OpenPopup("##fixture_server_file_context_menu");
    if (ImGui::BeginPopup("##fixture_server_file_context_menu",
                          ImGuiWindowFlags_AlwaysAutoResize |
                          ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::PushFont(f_bold);
        ImGui::TextUnformatted(display_name.c_str());
        ImGui::PopFont();
        ImGui::TextColored(k.brand_hov,
                           "Visual fixture — static local sample; no file was read, opened, or changed.");
        ImGui::Separator();
        ImGui::BeginDisabled();
        ImGui::MenuItem("Preview");
        ImGui::MenuItem("Open in Explorer");
        ImGui::Separator();
        ImGui::MenuItem("Delete");
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }
}

static void draw_fixture_server_files_tab(ServerUIState& s,
                                          const server::ServerConfig& sv,
                                          UiState& st) {
    ImGui::PushFont(f_small);
    ImGui::TextColored(k.muted, "Directory:");
    ImGui::SameLine();
    ImGui::TextColored(k.text, "C:\\Amalgam\\Servers\\%s", sv.name.c_str());
    ImGui::PopFont();
    ImGui::TextColored(k.brand_hov,
                       "Visual fixture: representative files only; browsing, refresh, preview, and file actions are disabled.");
    ImGui::Spacing();
    ghost_button("< Back", ImVec2(ui_px(80.0f), ui_px(26.0f)), true);
    ImGui::SameLine(0, ui_px(8.0f));
    ghost_button("Refresh", ImVec2(ui_px(80.0f), ui_px(26.0f)), true);
    ImGui::Spacing();

    struct FixtureFile {
        const char* name;
        const char* detail;
        bool directory;
    };
    const FixtureFile files[] = {
        {"world", "Folder", true},
        {"logs", "Folder", true},
        {"plugins", "Folder", true},
        {"eula.txt", "1 KB", false},
        {"server.properties", "2 KB", false},
        {"server.jar", "45 MB", false},
    };
    if (ImGui::BeginChild("##fixture_server_files_list", ImVec2(-1, ui_px(184.0f)),
                          ImGuiChildFlags_Borders)) {
        for (const auto& file : files) {
            ImGui::TextColored(file.directory ? k.brand : k.text, "%s", file.name);
            ImGui::SameLine(ui_px(250.0f));
            ImGui::TextColored(k.muted, "%s", file.detail);
        }
    }
    ImGui::EndChild();
    ImGui::TextColored(k.muted, "%d representative items", static_cast<int>(sizeof(files) / sizeof(files[0])));

    draw_fixture_server_file_context_menu(s);
    draw_fixture_server_file_preview(s, st);
}

// Kept separate from the live directory listing so fixture routes can show the
// same confirmation and recovery composition without ever enumerating files.
static void draw_server_file_delete_dialog(ServerUIState& s,
                                           const server::ServerConfig& sv,
                                           UiState& st) {
    if (s.file_delete_confirm) {
        ImGui::OpenPopup("##confirm_file_delete");
        s.file_delete_confirm = false;  // open once
    }
    if (ImGui::BeginPopupModal("##confirm_file_delete", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Delete \"%s\"?", s.file_delete_target.c_str());
        ImGui::TextColored(k.muted, "This cannot be undone.");
        if (!s.file_delete_error.empty()) {
            ImGui::Spacing();
            ImGui::PushFont(f_bold);
            ImGui::TextColored(k.red, "Delete failed");
            ImGui::PopFont();
            ImGui::PushStyleColor(ImGuiCol_Text, k.red);
            ImGui::TextWrapped("%s", s.file_delete_error.c_str());
            ImGui::PopStyleColor();
        }
        const bool fixture_preview = st.fixture_mode;
        if (fixture_preview) {
            ImGui::Spacing();
            ImGui::TextColored(k.brand_hov,
                               "Visual fixture — deletion is disabled; no fixture file is inspected or changed.");
        }
        ImGui::Spacing();
        const char* delete_label = s.file_delete_error.empty() ? "Delete" : "Retry delete";
        const auto attempt_file_delete = [&] {
            // The fixture button is disabled, but retain this guard at the
            // operation entry so no path or filesystem call can occur if this
            // renderer is invoked through another UI path.
            if (st.fixture_mode) return;
            namespace fs = std::filesystem;
            std::string path = file_full_path(s, sv.server_directory, s.file_delete_target);
            const std::wstring native_path = aml::net::to_wide(path);
            std::error_code ec;
            const bool directory = fs::is_directory(native_path, ec);
            bool removed = false;
            if (!ec) {
                if (directory) removed = fs::remove_all(native_path, ec) > 0;
                else removed = fs::remove(native_path, ec);
            }
            if (ec) {
                s.file_list_dirty = true;
                s.file_delete_error = directory
                    ? "The folder could not be fully deleted. Some contents may already be removed; refresh before trying again."
                    : "The file could not be deleted. Check that it is not in use, then try again.";
                report_server_failure(s, st, "Delete file", sv.name,
                                      s.file_delete_error + " (" + ec.message() + ")");
            } else if (!removed) {
                s.file_list_dirty = true;
                s.file_delete_error = "This item no longer exists. Refresh the file list before trying again.";
                report_server_failure(s, st, "Delete file", sv.name, s.file_delete_error);
            } else {
                s.file_list_dirty = true;
                s.file_delete_target.clear();
                s.file_delete_error.clear();
                clear_server_failure(s);
                ImGui::CloseCurrentPopup();
            }
        };
        if (danger_button(delete_label, ImVec2(ui_px(104.0f), ui_px(28.0f)), fixture_preview)) {
            attempt_file_delete();
        }
        ImGui::SameLine();
        if (ghost_button(fixture_preview ? "Close preview" : "Cancel",
                         ImVec2(fixture_preview ? ui_px(116.0f) : ui_px(70.0f), ui_px(28.0f)))) {
            s.file_delete_target.clear();
            s.file_delete_error.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// ---------------------------------------------------------------------------
// Server detail – file browser tab
// ---------------------------------------------------------------------------

static void draw_server_files_tab(ServerUIState& s, const server::ServerConfig& sv,
                                  UiState& st) {
    if (st.fixture_mode) {
        draw_fixture_server_files_tab(s, sv, st);
        draw_server_file_delete_dialog(s, sv, st);
        return;
    }
    if (s.file_list_dirty) {
        scan_server_directory(s, sv.server_directory);
        s.file_list_dirty = false;
    }

    // Breadcrumb path
    {
        ImGui::PushFont(f_small);
        ImGui::TextColored(k.muted, "Directory:");
        ImGui::SameLine();
        // A fixture directory is intentionally a disposable artifact path.
        // Never place that machine-specific path in visual evidence; use the
        // representative launcher location users recognize instead.
        std::string display_path = st.fixture_mode
            ? "C:\\Amalgam\\Servers\\" + sv.name
            : sv.server_directory;
        if (!s.file_browse_path.empty()) display_path += "\\" + s.file_browse_path;
        ImGui::TextColored(k.text, "%s", display_path.c_str());
        ImGui::PopFont();
    }

    // Navigation controls
    ImGui::Spacing();
    if (!s.file_browse_path.empty()) {
        if (ghost_button("< Back", ImVec2(ui_px(80.0f), ui_px(26.0f)))) {
            auto pos = s.file_browse_path.rfind('\\');
            if (pos != std::string::npos)
                s.file_browse_path = s.file_browse_path.substr(0, pos);
            else
                s.file_browse_path.clear();
            s.file_list_dirty = true;
        }
        ImGui::SameLine(0, ui_px(8.0f));
    }
    if (ghost_button("Refresh", ImVec2(ui_px(80.0f), ui_px(26.0f)))) {
        s.file_list_dirty = true;
    }

    ImGui::Spacing();

    // File list
    float footer_h = ImGui::GetFrameHeightWithSpacing() + ui_px(4.0f);
    if (ImGui::BeginChild("##server_files_list",
            ImVec2(-1, -(footer_h + ui_px(28.0f))), ImGuiChildFlags_Borders)) {
        if (s.file_entries.empty()) {
            empty_state("Empty directory", "No files found in this folder.");
        } else {
            for (size_t i = 0; i < s.file_entries.size(); ++i) {
                const auto& name = s.file_entries[i];
                bool is_dir = s.file_is_dir[i];
                ImGui::PushID(static_cast<int>(i));

                // Icon (vector)
                const ImVec2 icon_center = ImGui::GetCursorScreenPos() +
                                           ImVec2(ui_px(8.0f), ui_px(9.0f));
                if (is_dir) {
                    draw_icon(IconId::Folder, icon_center, ui_px(7.0f), c32(k.brand));
                } else {
                    // Color by extension
                    auto dot = name.rfind('.');
                    ImVec4 ext_color = k.text;
                    if (dot != std::string::npos) {
                        auto ext = name.substr(dot + 1);
                        if (ext == "jar" || ext == "zip") ext_color = k.brand;
                        else if (ext == "properties" || ext == "yml" || ext == "yaml" || ext == "toml") ext_color = k.green;
                        else if (ext == "log" || ext == "txt") ext_color = k.muted;
                        else if (ext == "json") ext_color = k.yellow;
                    }
                    draw_icon(IconId::File, icon_center, ui_px(7.0f), c32(ext_color));
                }
                ImGui::Dummy(ImVec2(ui_px(16.0f), 0));
                ImGui::SameLine();

                // Clickable name
                ImGui::TextColored(is_dir ? k.brand : k.text, "%s", name.c_str());

                // Right-click context
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                    ImGui::OpenPopup("##file_ctx");
                }

                // Double-click to open
                if (ImGui::IsItemHovered()) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    if (ImGui::IsMouseDoubleClicked(0)) {
                        if (is_dir) {
                            if (!s.file_browse_path.empty())
                                s.file_browse_path += "\\" + name;
                            else
                                s.file_browse_path = name;
                            s.file_list_dirty = true;
                        } else {
                            s.file_preview_name = name;
                            s.file_preview_content = read_text_file(
                                file_full_path(s, sv.server_directory, name));
                            s.file_preview_open = true;
                        }
                    }
                }

                // Context menu
                if (ImGui::BeginPopup("##file_ctx")) {
                    if (is_dir) {
                        if (ImGui::MenuItem("Open")) {
                            if (!s.file_browse_path.empty())
                                s.file_browse_path += "\\" + name;
                            else
                                s.file_browse_path = name;
                            s.file_list_dirty = true;
                        }
                    } else {
                        if (ImGui::MenuItem("Preview")) {
                            s.file_preview_name = name;
                            s.file_preview_content = read_text_file(
                                file_full_path(s, sv.server_directory, name));
                            s.file_preview_open = true;
                        }
                        if (ImGui::MenuItem("Open in Explorer")) {
                            ShellExecuteW(nullptr, L"open",
                                aml::net::to_wide(file_full_path(s, sv.server_directory, name)).c_str(),
                                nullptr, nullptr, SW_SHOWNORMAL);
                        }
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Delete")) {
                        s.file_delete_target = name;
                        s.file_delete_error.clear();
                        s.file_delete_confirm = true;
                    }
                    ImGui::EndPopup();
                }

                ImGui::PopID();
            }
        }
    }
    ImGui::EndChild();

    ImGui::Spacing();
    ImGui::TextColored(k.muted, "%d items", (int)s.file_entries.size());

    draw_server_file_delete_dialog(s, sv, st);

    // File preview window
    if (s.file_preview_open) {
        ImGui::SetNextWindowSize(ImVec2(ui_px(560), ui_px(420)), ImGuiCond_Appearing);
        if (ImGui::Begin(("File: " + s.file_preview_name).c_str(), &s.file_preview_open,
                         ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::PushFont(f_mono);
            float footer_h2 = ImGui::GetFrameHeightWithSpacing() + ui_px(4.0f);
            if (ImGui::BeginChild("##file_preview_content",
                    ImVec2(-1, -(footer_h2)), ImGuiChildFlags_Borders)) {
                ImGui::TextWrapped("%s", s.file_preview_content.c_str());
            }
            ImGui::EndChild();
            ImGui::PopFont();

            ImGui::Spacing();
            if (ghost_button("Open in Explorer", ImVec2(ui_px(130.0f), ui_px(26.0f)))) {
                ShellExecuteW(nullptr, L"open",
                    aml::net::to_wide(file_full_path(s, sv.server_directory, s.file_preview_name)).c_str(),
                    nullptr, nullptr, SW_SHOWNORMAL);
            }
        }
        ImGui::End();
    }
}

// ---------------------------------------------------------------------------
// Server detail – players tab
// ---------------------------------------------------------------------------

static void draw_server_players_tab(ServerUIState& s, const server::ServerConfig& sv, UiState& st) {
    ImGui::PushFont(f_h2);
    ImGui::TextColored(k.text, "Online Players");
    ImGui::PopFont();
    ImGui::Spacing();

    // A visual-review fixture must never query a same-named local server or
    // offer controls which can issue vanilla server commands.  The four rows
    // deliberately match the overview metric seeded in ui.cpp.
    if (st.fixture_mode) {
        ImGui::TextColored(k.brand_hov,
                           "Visual fixture: representative local player data; management is disabled.");
        ImGui::Spacing();
        struct FixturePlayer { const char* name; const char* ping; ImVec4 color; };
        const FixturePlayer players[] = {
            {"Alex", "42 ms", k.green},
            {"MiraBuilds", "57 ms", k.green},
            {"AveryStone", "86 ms", k.green},
            {"NovaCraft", "118 ms", k.yellow},
        };
        card_begin("##fixture_players_list", ImVec2(-1, 0));
        for (size_t i = 0; i < sizeof(players) / sizeof(players[0]); ++i) {
            const auto& player = players[i];
            ImGui::PushID(static_cast<int>(i));
            const float row_h = ui_px(48.0f);
            const ImVec2 row_min = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(row_min, row_min + ImVec2(ImGui::GetContentRegionAvail().x, row_h),
                              c32(k.surface), ui_px(8.0f));
            dl->AddCircleFilled(row_min + ImVec2(ui_px(18.0f), row_h * 0.5f), ui_px(10.0f),
                                c32(k.brand_dk));
            char initial[2] = {player.name[0], '\0'};
            const ImVec2 initial_size = ImGui::CalcTextSize(initial);
            dl->AddText(row_min + ImVec2(ui_px(18.0f) - initial_size.x * 0.5f,
                                         row_h * 0.5f - initial_size.y * 0.5f),
                        c32(k.text), initial);
            ImGui::PushFont(f_bold);
            dl->AddText(row_min + ImVec2(ui_px(38.0f),
                                         (row_h - ImGui::GetTextLineHeight()) * 0.5f),
                        c32(k.text), player.name);
            ImGui::PopFont();
            const ImVec2 ping_pos = row_min + ImVec2(ui_px(210.0f),
                                                      (row_h - ui_px(20.0f)) * 0.5f);
            const ImVec2 ping_size = ImGui::CalcTextSize(player.ping) +
                                    ImVec2(ui_px(12.0f), ui_px(4.0f));
            ImVec4 ping_bg = player.color;
            ping_bg.w = 0.12f;
            dl->AddRectFilled(ping_pos, ping_pos + ping_size, c32(ping_bg), ui_px(5.0f));
            dl->AddText(ping_pos + ImVec2(ui_px(6.0f), ui_px(2.0f)), c32(player.color),
                        player.ping);
            // Advance through normal layout instead of moving the cursor past
            // the card boundary, which Dear ImGui correctly flags as an
            // invalid parent-window extension in a capture.
            ImGui::Dummy(ImVec2(0, row_h));
            ImGui::PopID();
        }
        card_end();
        ImGui::TextColored(k.muted, "4 / %d connected", sv.max_players);
        ImGui::Spacing();
        ImGui::PushFont(f_h2);
        ImGui::TextColored(k.text, "Access controls");
        ImGui::PopFont();
        ImGui::TextColored(k.muted,
                           "Whitelist and operator management are unavailable in visual fixtures.");
        return;
    }

    const bool command_busy = local_server_action_lane_busy(s) ||
                              local_server_command_lane_busy(s);

    if (effective_stage(st, sv) != server::ServerStage::Running) {
        empty_state("Server not running", "Start the server to see online players.");
    } else if (s.detail_players.empty()) {
        empty_state("No players online", "Waiting for players to join.");
    } else {
        card_begin("##players_list", ImVec2(-1, 0));
        for (size_t i = 0; i < s.detail_players.size(); ++i) {
            auto& p = s.detail_players[i];
            ImGui::PushID(static_cast<int>(i));
            const float row_h = ui_px(52.0f);
            const ImVec2 row_min = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(row_min, row_min + ImVec2(ImGui::GetContentRegionAvail().x, row_h),
                              c32(k.surface), ui_px(8.0f));

            // Player avatar dot + name
            dl->AddCircleFilled(row_min + ImVec2(ui_px(18.0f), row_h * 0.5f), ui_px(10.0f),
                                c32(k.brand_dk));
            const char initial = p.name.empty() ? '?' : p.name[0];
            char init_str[2] = { initial, '\0' };
            const ImVec2 its = ImGui::CalcTextSize(init_str);
            dl->AddText(row_min + ImVec2(ui_px(18.0f) - its.x * 0.5f,
                                         row_h * 0.5f - its.y * 0.5f),
                        c32(k.text), init_str);
            ImGui::PushFont(f_bold);
            dl->AddText(row_min + ImVec2(ui_px(38.0f), (row_h - ImGui::GetTextLineHeight()) * 0.5f),
                        c32(k.text), p.name.c_str());
            ImGui::PopFont();

            // Ping badge
            ImVec4 ping_col = p.ping_ms < 100 ? k.green : p.ping_ms < 200 ? k.yellow : k.red;
            const ImVec2 ping_pos = row_min + ImVec2(ui_px(220.0f), (row_h - ui_px(20.0f)) * 0.5f);
            const ImVec2 ping_sz = ImGui::CalcTextSize(p.ping_str.c_str()) + ImVec2(ui_px(12.0f), ui_px(4.0f));
            ImVec4 ping_bg = ping_col; ping_bg.w = 0.12f;
            dl->AddRectFilled(ping_pos, ping_pos + ping_sz, c32(ping_bg), ui_px(5.0f));
            dl->AddText(ping_pos + ImVec2(ui_px(6.0f), ui_px(2.0f)), c32(ping_col),
                        p.ping_str.c_str());

            // Actions
            ImGui::SameLine(ImGui::GetCursorPosX() +
                            std::max(0.0f, ImGui::GetContentRegionAvail().x - ui_px(140.0f)));
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (row_h - ui_px(26.0f)) * 0.5f);
            if (ghost_button("Kick", ImVec2(ui_px(62.0f), ui_px(26.0f)), command_busy)) {
                start_local_server_command(st, sv, LocalServerCommandOrigin::Kick,
                                           "kick " + p.name);
            }
            ImGui::SameLine(0, ui_px(4.0f));
            if (ghost_button("Ban", ImVec2(ui_px(62.0f), ui_px(26.0f)), command_busy)) {
                start_local_server_command(st, sv, LocalServerCommandOrigin::Ban,
                                           "ban " + p.name);
            }
            ImGui::Dummy(ImVec2(0, row_h));
            ImGui::PopID();
        }
        card_end();
    }

    ImGui::Spacing();
    ImGui::Spacing();

    // Whitelist / OP management
    ImGui::PushFont(f_h2);
    ImGui::TextColored(k.text, "Whitelist");
    ImGui::PopFont();
    ImGui::Spacing();

    if (command_busy) ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(ui_px(220.0f));
    ImGui::InputText("##wl_input", &s.player_whitelist_input);
    ImGui::SameLine();
    if (primary_button("Add to Whitelist", ImVec2(ui_px(130.0f), ui_px(26.0f))) &&
        !s.player_whitelist_input.empty()) {
        start_local_server_command(st, sv, LocalServerCommandOrigin::WhitelistAdd,
                                   "whitelist add " + s.player_whitelist_input);
    }
    ImGui::Spacing();

    if (!s.whitelist_entries.empty()) {
        card_begin("##whitelist", ImVec2(-1, 0));
        for (size_t i = 0; i < s.whitelist_entries.size(); ++i) {
            ImGui::PushID(static_cast<int>(i + 1000));
            ImGui::TextColored(k.text, "%s", s.whitelist_entries[i].c_str());
            ImGui::SameLine(0, ui_px(12.0f));
            if (ghost_button("Remove", ImVec2(ui_px(70.0f), ui_px(22.0f)))) {
                start_local_server_command(st, sv, LocalServerCommandOrigin::WhitelistRemove,
                                           "whitelist remove " + s.whitelist_entries[i]);
            }
            ImGui::PopID();
        }
        card_end();
    }

    ImGui::Spacing();
    ImGui::Spacing();

    // Operators
    ImGui::PushFont(f_h2);
    ImGui::TextColored(k.text, "Operators (OPs)");
    ImGui::PopFont();
    ImGui::Spacing();

    ImGui::SetNextItemWidth(ui_px(220.0f));
    ImGui::InputText("##op_input", &s.player_op_input);
    ImGui::SameLine();
    if (primary_button("Make OP", ImVec2(ui_px(100.0f), ui_px(26.0f))) &&
        !s.player_op_input.empty()) {
        start_local_server_command(st, sv, LocalServerCommandOrigin::OpAdd,
                                   "op " + s.player_op_input);
    }
    ImGui::Spacing();

    if (!s.ops_entries.empty()) {
        card_begin("##ops_list", ImVec2(-1, 0));
        for (size_t i = 0; i < s.ops_entries.size(); ++i) {
            ImGui::PushID(static_cast<int>(i + 2000));
            ImGui::TextColored(k.brand, "%s", s.ops_entries[i].c_str());
            ImGui::SameLine(0, ui_px(12.0f));
            if (ghost_button("De-op", ImVec2(ui_px(70.0f), ui_px(22.0f)))) {
                start_local_server_command(st, sv, LocalServerCommandOrigin::OpRemove,
                                           "deop " + s.ops_entries[i]);
            }
            ImGui::PopID();
        }
        card_end();
    }
    if (command_busy) ImGui::EndDisabled();
}

// ---------------------------------------------------------------------------
// Server detail – world tab
// ---------------------------------------------------------------------------

// Shared by the live World tab and its inert fixture equivalent.  Keeping the
// confirmation outside the live-backup list is important: the fixture must
// show the exact same decision boundary without enumerating any reviewer
// files or letting the fixture issue a restore request.
static void draw_server_backup_restore_dialog(ServerUIState& s,
                                              const server::ServerConfig& sv,
                                              UiState& st) {
    if (s.restore_backup_confirm) ImGui::OpenPopup("Confirm Backup Restore##server_backup");
    if (!ImGui::BeginPopupModal("Confirm Backup Restore##server_backup", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    ImGui::PushFont(f_h2);
    ImGui::TextColored(k.text, "Restore backup?");
    ImGui::PopFont();
    ImGui::TextWrapped("Restore \"%s\" into server \"%s\"?",
                       s.restore_backup_name.c_str(), sv.name.c_str());
    ImGui::Spacing();
    ImGui::TextColored(k.yellow,
                       "The server must be stopped. When current files exist, Amalgam first creates and retains a pre-restore backup.");
    ImGui::TextColored(k.muted,
                       "The selected backup replaces the current server files only after it has been staged successfully.");
    if (!s.restore_backup_error.empty()) {
        ImGui::Spacing();
        ImGui::PushFont(f_bold);
        ImGui::TextColored(k.red, "Restore failed");
        ImGui::PopFont();
        ImGui::PushStyleColor(ImGuiCol_Text, k.red);
        ImGui::TextWrapped("%s", s.restore_backup_error.c_str());
        ImGui::PopStyleColor();
    }
    const bool fixture_preview = st.fixture_mode;
    if (fixture_preview) {
        ImGui::Spacing();
        ImGui::TextColored(k.brand_hov,
                           "Visual fixture — restoration is disabled; no server files are staged or changed.");
    }
    ImGui::Spacing();
    const auto attempt_backup_restore = [&] {
        // Keep the hard fixture boundary at the action entry rather than
        // relying solely on disabled visual controls.
        if (st.fixture_mode) return;
        if (local_server_supervised(sv)) {
            s.restore_backup_error = "Stop this local server before restoring a backup.";
        } else {
            std::string restore_error;
            if (aml::services::local_server_manager()->restore_backup(
                    sv.name, s.restore_backup_id, &restore_error)) {
                s.world_dirty = true;
                s.restore_backup_confirm = false;
                s.restore_backup_id.clear();
                s.restore_backup_name.clear();
                s.restore_backup_error.clear();
                clear_server_failure(s);
                push_notice(st, ui_model::NoticeLevel::Success, "Backup Restored",
                            "The previous server files were preserved as a pre-restore backup.");
                ImGui::CloseCurrentPopup();
            } else {
                s.restore_backup_error = restore_error.empty()
                    ? "The backup could not be restored; current server files were not changed."
                    : restore_error;
                report_server_failure(s, st, "Restore Backup", sv.name, s.restore_backup_error);
            }
        }
    };
    if (danger_button("Restore backup", ImVec2(ui_px(140.0f), ui_px(32.0f)), fixture_preview)) {
        attempt_backup_restore();
    }
    ImGui::SameLine();
    if (ghost_button(fixture_preview ? "Close preview" : "Cancel",
                     ImVec2(fixture_preview ? ui_px(116.0f) : ui_px(80.0f), ui_px(32.0f)))) {
        s.restore_backup_confirm = false;
        s.restore_backup_id.clear();
        s.restore_backup_name.clear();
        s.restore_backup_error.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

static void draw_server_world_tab(ServerUIState& s, const server::ServerConfig& sv, UiState& st) {
    ImGui::PushFont(f_h2);
    ImGui::TextColored(k.text, "World");
    ImGui::PopFont();
    ImGui::Spacing();

    // Do not enumerate a reviewer-owned world directory from a fixture.  This
    // keeps the visual evidence representative, deterministic, and inert.
    if (st.fixture_mode) {
        ImGui::TextColored(k.brand_hov,
                           "Visual fixture: representative local world data; file actions are disabled.");
        ImGui::Spacing();
        card_begin("##fixture_world_info", ImVec2(-1, 0));
        ImGui::TextColored(k.text, "Forsaken World SMP");
        ImGui::TextColored(k.muted, "world  •  42 MB  •  Survival");
        ImGui::Spacing();
        ghost_button("Save-All", ImVec2(ui_px(100.0f), ui_px(28.0f)), true);
        ImGui::SameLine(0, ui_px(4.0f));
        ghost_button("Open Folder", ImVec2(ui_px(120.0f), ui_px(28.0f)), true);
        card_end();
        ImGui::Spacing();
        ImGui::PushFont(f_h2);
        ImGui::TextColored(k.text, "Backups");
        ImGui::PopFont();
        ImGui::Spacing();
        card_begin("##fixture_world_backup", ImVec2(-1, 0));
        ImGui::TextColored(k.text, "Pre-release checkpoint");
        ImGui::SameLine(0, ui_px(10.0f));
        ImGui::TextColored(k.muted, "42 MB  •  Local fixture");
        ImGui::SameLine(0, ui_px(12.0f));
        ghost_button("Restore", ImVec2(ui_px(70.0f), ui_px(22.0f)), true);
        card_end();
        draw_server_backup_restore_dialog(s, sv, st);
        return;
    }

    if (sv.server_directory.empty()) {
        empty_state("No server directory", "Set a server directory to manage the world.");
        return;
    }

    // World info card
    card_begin("##world_info", ImVec2(-1, 0));
    {
        // Scan world folder
        namespace fs = std::filesystem;
        std::string world_dir = sv.server_directory + "\\world";
        std::error_code ec;
        bool exists = fs::exists(aml::net::to_wide(world_dir), ec);

        if (!exists) {
            // Also check level-name from server.properties
            std::string level_name = "world";
            {
                FILE* f = nullptr;
                std::string prop_path = sv.server_directory + "\\server.properties";
                fopen_s(&f, prop_path.c_str(), "r");
                if (f) {
                    char buf[256];
                    while (fgets(buf, sizeof(buf), f)) {
                        std::string line(buf);
                        if (line.find("level-name=") == 0) {
                            level_name = line.substr(11);
                            if (!level_name.empty() && level_name.back() == '\n')
                                level_name.pop_back();
                        }
                    }
                    fclose(f);
                }
                world_dir = sv.server_directory + "\\" + level_name;
                exists = fs::exists(aml::net::to_wide(world_dir), ec);
            }
        }

        if (!exists) {
            ImGui::TextColored(k.muted, "World folder not found.");
            ImGui::TextColored(k.muted, "Start the server once to generate the world.");
        } else {
            // Calculate world size
            if (s.world_dirty || s.world_size_bytes == 0) {
                s.world_size_bytes = 0;
                std::error_code size_ec;
                for (auto& entry : fs::recursive_directory_iterator(
                        aml::net::to_wide(world_dir), size_ec)) {
                    if (entry.is_regular_file(size_ec))
                        s.world_size_bytes += entry.file_size(size_ec);
                }
                s.world_dirty = false;
            }

            ImGui::TextColored(k.text, "World Size:");
            ImGui::SameLine();
            ImGui::TextColored(k.muted, "%s", file_size_label(s.world_size_bytes).c_str());

            ImGui::Spacing();

            // Quick actions
            float bw = ui_px(100.0f);
            float bh = ui_px(28.0f);

            if (effective_stage(st, sv) == server::ServerStage::Running) {
                const bool command_busy = local_server_action_lane_busy(s) ||
                                          local_server_command_lane_busy(s);
                const bool saving = local_server_command_is_working(
                    s, LocalServerCommandOrigin::SaveAll);
                if (ghost_button(saving ? "Saving..." : "Save-All", ImVec2(bw, bh),
                                 command_busy)) {
                    start_local_server_command(st, sv, LocalServerCommandOrigin::SaveAll,
                                               "save-all");
                }
                ImGui::SameLine(0, ui_px(4.0f));
            }

            if (ghost_button("Open Folder", ImVec2(bw + ui_px(20.0f), bh))) {
                ShellExecuteW(nullptr, L"open",
                    aml::net::to_wide(world_dir).c_str(),
                    nullptr, nullptr, SW_SHOWNORMAL);
            }
        }
    }
    card_end();

    ImGui::Spacing();
    ImGui::Spacing();

    // World backup section
    ImGui::PushFont(f_h2);
    ImGui::TextColored(k.text, "Backups");
    ImGui::PopFont();
    ImGui::Spacing();

    if (!st.fixture_mode && ghost_button("Create Backup", ImVec2(ui_px(120.0f), ui_px(30.0f)))) {
        std::string backup_id;
        std::string backup_err;
        if (aml::services::local_server_manager()->create_backup(
                sv.name, sv.name + "_manual", &backup_id, &backup_err)) {
            clear_server_failure(s);
        } else {
            report_server_failure(s, st, "Create Backup", sv.name, backup_err);
        }
    }

    ImGui::Spacing();

    // A fixture must not enumerate the reviewer's global local-backup root.
    // Provide a static representative record for layout evidence instead.
    if (st.fixture_mode) {
        card_begin("##fixture_backups_list", ImVec2(-1, 0));
        ImGui::TextColored(k.text, "Forsaken World SMP - pre-release checkpoint");
        ImGui::SameLine(0, ui_px(10.0f));
        ImGui::TextColored(k.muted, "(Local fixture - 42 MB)");
        ImGui::SameLine(0, ui_px(12.0f));
        ImGui::BeginDisabled();
        ghost_button("Restore", ImVec2(ui_px(70.0f), ui_px(22.0f)));
        ImGui::EndDisabled();
        card_end();
    } else {
        // List backups from the services layer
        auto backups = aml::services::local_server_manager()->list_backups(sv.name);
        if (backups.empty()) {
            empty_state("No backups yet", "Create a backup above to protect your world.");
        } else {
            card_begin("##backups_list", ImVec2(-1, 0));
            for (size_t i = 0; i < backups.size(); ++i) {
                ImGui::PushID(static_cast<int>(i + 3000));
                auto& b = backups[i];
                auto name_it = b.find("name");
                auto id_it = b.find("id");
                auto time_it = b.find("created_at");
                std::string bname = name_it != b.end() ? name_it->second : "Backup";
                std::string bid = id_it != b.end() ? id_it->second : "";
                std::string btime = time_it != b.end() ? time_it->second : "";

                ImGui::TextColored(k.text, "%s", bname.c_str());
                if (!btime.empty()) {
                    ImGui::SameLine();
                    ImGui::TextColored(k.muted, "(%s)", btime.c_str());
                }
                ImGui::SameLine(0, ui_px(12.0f));
                if (ghost_button("Restore", ImVec2(ui_px(70.0f), ui_px(22.0f)))) {
                    if (!bid.empty()) {
                        s.restore_backup_id = bid;
                        s.restore_backup_name = bname;
                        s.restore_backup_error.clear();
                        s.restore_backup_confirm = true;
                    }
                }
                ImGui::PopID();
            }
            card_end();
        }
    }

    draw_server_backup_restore_dialog(s, sv, st);
}

// ---------------------------------------------------------------------------
// Server detail – console tab (embedded)
// ---------------------------------------------------------------------------

// Keep the console visual-review route entirely self-contained.  The live
// console below reads a supervised server and can send commands; even a
// disabled-looking live widget would not be an acceptable fixture boundary.
static void draw_fixture_server_detail_console(const server::ServerConfig& sv) {
    ImGui::PushFont(f_h2);
    ImGui::TextColored(k.text, "Console: %s", sv.name.c_str());
    ImGui::PopFont();
    ImGui::SameLine(0, ui_px(12.0f));
    draw_status_badge(sv.stage);
    ImGui::Spacing();
    ImGui::TextColored(k.brand_hov,
                       "Visual fixture — representative local log only. Reading, copying, clearing, and sending commands are disabled.");
    ImGui::Spacing();

    std::string fixture_filter;
    ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(ui_px(200.0f));
    input_text_hint("##fixture_dconsole_filter", "Filter logs...", &fixture_filter);
    ImGui::SameLine(0, ui_px(8.0f));
    ghost_button("Copy", ImVec2(ui_px(60.0f), ui_px(24.0f)));
    ImGui::SameLine(0, ui_px(4.0f));
    ghost_button("Clear", ImVec2(ui_px(60.0f), ui_px(24.0f)));
    ImGui::SameLine(0, ui_px(4.0f));
    ghost_button("Auto-scroll ON", ImVec2(ui_px(110.0f), ui_px(24.0f)));
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // The real console intentionally owns a bounded, auto-scrolling log
    // viewer.  A review fixture must instead participate in the launcher's
    // page-level scroll host: the generic @top/@middle/@bottom capture cases
    // target that host, not an arbitrary nested child.  This content-sized
    // card makes each capture position a truthful portion of one stable,
    // explicitly local representative transcript while the live console below
    // remains completely unchanged.
    struct FixtureConsoleEntry {
        int section;
        const char* timestamp;
        const char* message;
        ImVec4 color;
    };
    const char* section_titles[] = {
        "STARTUP",
        "PLAYER ACTIVITY",
        "WORLD SAVES",
        "SERVER MAINTENANCE",
        "SESSION TAIL",
    };
    const FixtureConsoleEntry entries[] = {
        {0, "10:14:02", "INFO  Starting Minecraft server version 1.21.1", k.green},
        {0, "10:14:03", "INFO  Loading server properties", k.green},
        {0, "10:14:04", "INFO  Loading 3 representative mods", k.green},
        {0, "10:14:05", "INFO  Preparing spawn area: 0%", k.muted},
        {0, "10:14:06", "INFO  Preparing spawn area: 54%", k.muted},
        {0, "10:14:07", "INFO  Preparing spawn area: 100%", k.muted},
        {0, "10:14:08", "INFO  Done (6.214s)! For help, type \"help\"", k.green},
        {0, "10:14:10", "INFO  Listening on 0.0.0.0:25565", k.green},

        {1, "10:15:17", "INFO  Alex joined the game", k.green},
        {1, "10:15:18", "INFO  Alex joined the lobby", k.muted},
        {1, "10:15:31", "INFO  Sam joined the game", k.green},
        {1, "10:15:44", "INFO  Sent welcome message to Sam", k.muted},
        {1, "10:16:02", "INFO  Alex completed advancement [Stone Age]", k.muted},
        {1, "10:16:11", "WARN  Can't keep up! Is the server overloaded?", k.yellow},
        {1, "10:16:15", "INFO  Recovered 38 ms behind the expected tick rate", k.green},
        {1, "10:16:22", "INFO  Sam moved to world_nether", k.muted},

        {2, "10:16:28", "INFO  Saved the game", k.green},
        {2, "10:16:29", "INFO  Saved chunks for level 'world'/minecraft:overworld", k.muted},
        {2, "10:16:29", "INFO  Saved chunks for level 'world_nether'/minecraft:the_nether", k.muted},
        {2, "10:18:04", "INFO  Autosave checkpoint started", k.muted},
        {2, "10:18:05", "INFO  Flushed 12 modified chunks", k.green},
        {2, "10:18:05", "INFO  Autosave checkpoint complete", k.green},
        {2, "10:20:09", "INFO  Saved the game", k.green},
        {2, "10:20:10", "INFO  World save completed in 41 ms", k.muted},

        {3, "10:22:00", "INFO  Scheduled maintenance check started", k.muted},
        {3, "10:22:01", "INFO  Runtime health check passed", k.green},
        {3, "10:22:02", "INFO  Storage space is available", k.green},
        {3, "10:23:18", "INFO  Player list synchronized", k.muted},
        {3, "10:24:40", "WARN  Slow tick observed; continuing to monitor", k.yellow},
        {3, "10:24:41", "INFO  Tick time returned to normal", k.green},
        {3, "10:26:00", "INFO  Scheduled maintenance check complete", k.green},
        {3, "10:27:34", "INFO  Saved the game", k.green},

        {4, "10:29:12", "INFO  Alex left the game", k.muted},
        {4, "10:29:13", "INFO  Sam returned to the overworld", k.muted},
        {4, "10:30:00", "INFO  Autosave checkpoint started", k.muted},
        {4, "10:30:01", "INFO  Autosave checkpoint complete", k.green},
        {4, "10:31:25", "INFO  Player list synchronized", k.muted},
        {4, "10:32:44", "INFO  Saved the game", k.green},
        {4, "10:33:00", "INFO  Representative fixture capture is ready", k.green},
        {4, "10:33:01", "INFO  Commands remain disabled in this review route", k.muted},
    };
    constexpr int fixture_entry_count =
        static_cast<int>(sizeof(entries) / sizeof(entries[0]));

    card_begin("##fixture_dconsole_log_document", ImVec2(-1, 0));
    ImGui::TextColored(k.muted,
                       "Representative local transcript — %d static records",
                       fixture_entry_count);
    ImGui::Spacing();
    int previous_section = -1;
    for (const auto& entry : entries) {
        if (entry.section != previous_section) {
            if (previous_section >= 0) {
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
            }
            ImGui::TextColored(k.brand_hov, "%s", section_titles[entry.section]);
            previous_section = entry.section;
        }
        ImGui::TextColored(k.muted, "[%s]", entry.timestamp);
        ImGui::SameLine(ui_px(78.0f));
        ImGui::PushTextWrapPos();
        ImGui::TextColored(entry.color, "%s", entry.message);
        ImGui::PopTextWrapPos();
    }
    card_end();

    ImGui::Spacing();
    std::string fixture_command = "say Welcome to Forsaken World SMP";
    const float send_width = ui_px(76.0f);
    const float send_gap = ImGui::GetStyle().ItemSpacing.x;
    const float command_width = std::max(ui_px(120.0f),
        ImGui::GetContentRegionAvail().x - send_width - send_gap);
    ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(command_width);
    ImGui::InputText("##fixture_dconsole_cmd", &fixture_command);
    ImGui::SameLine(0, send_gap);
    primary_button("Send", ImVec2(send_width, ImGui::GetFrameHeight()));
    ImGui::EndDisabled();
}

static void draw_server_detail_console(ServerUIState& s, const server::ServerConfig& sv, UiState& st) {
    if (st.fixture_mode) {
        draw_fixture_server_detail_console(sv);
        return;
    }

    // The visual fixture seeds `s.console_log` from UiState.  Do not ask the
    // service layer for a same-named real server's logs: that lookup reads the
    // reviewer's local server registry and can expose a real latest.log.
    if (!st.fixture_mode && ImGui::GetTime() - s.detail_console_last_refresh >= 1.0) {
        s.detail_console_last_refresh = ImGui::GetTime();
        const auto live_entries =
            aml::services::local_server_manager()->get_console_logs(sv.name, 500, nullptr);
        if (!live_entries.empty()) {
            s.console_log.clear();
            s.console_log.reserve(live_entries.size());
            for (const auto& live : live_entries) {
                server::ServerConsoleEntry entry;
                const std::time_t raw_time = static_cast<std::time_t>(live.timestamp);
                std::tm local_time{};
                char time_text[16]{};
                if (localtime_s(&local_time, &raw_time) == 0)
                    std::strftime(time_text, sizeof(time_text), "%H:%M:%S", &local_time);
                entry.timestamp = time_text[0] ? time_text : "live";
                entry.message = live.message;
                s.console_log.push_back(std::move(entry));
            }
        }
    }

    // Header
    ImGui::PushFont(f_h2);
    ImGui::TextColored(k.text, "Console: %s", sv.name.c_str());
    ImGui::PopFont();
    ImGui::SameLine(0, ui_px(12.0f));
    draw_status_badge(effective_stage(st, sv));

    ImGui::Spacing();

    // Controls row
    ImGui::SetNextItemWidth(ui_px(200.0f));
    input_text_hint("##dconsole_filter", "Filter logs...", &s.detail_console_filter);

    ImGui::SameLine(0, ui_px(8.0f));
    if (ghost_button("Copy", ImVec2(ui_px(60.0f), ui_px(24.0f)))) {
        std::string copy_text;
        for (const auto& entry : s.console_log)
            copy_text += "[" + entry.timestamp + "] " + entry.message + "\n";
        if (!copy_text.empty()) ImGui::SetClipboardText(copy_text.c_str());
    }
    ImGui::SameLine(0, ui_px(4.0f));
    if (ghost_button("Clear", ImVec2(ui_px(60.0f), ui_px(24.0f)))) {
        s.console_log.clear();
    }
    ImGui::SameLine(0, ui_px(4.0f));
    if (ghost_button(s.detail_console_auto_scroll ? "Auto-scroll ON" : "Auto-scroll OFF",
                     ImVec2(ui_px(110.0f), ui_px(24.0f)))) {
        s.detail_console_auto_scroll = !s.detail_console_auto_scroll;
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Log area
    float footer_h = ImGui::GetFrameHeightWithSpacing() + ui_px(4.0f);
    if (ImGui::BeginChild("##dconsole_log",
            ImVec2(-1, -(footer_h)), ImGuiChildFlags_Borders)) {
        for (const auto& entry : s.console_log) {
            if (!s.detail_console_filter.empty() &&
                entry.message.find(s.detail_console_filter) == std::string::npos)
                continue;

            ImVec4 log_color = k.muted;
            if (entry.message.find("ERROR") != std::string::npos ||
                entry.message.find("SEVERE") != std::string::npos)
                log_color = k.red;
            else if (entry.message.find("WARN") != std::string::npos)
                log_color = k.yellow;
            else if (entry.message.find("INFO") != std::string::npos)
                log_color = k.green;

            ImGui::TextColored(k.muted, "[%s]", entry.timestamp.c_str());
            ImGui::SameLine();
            ImGui::TextColored(log_color, "%s", entry.message.c_str());
        }

        if (s.detail_console_auto_scroll &&
            ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
            ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();

    ImGui::Spacing();

    auto submit_command = [&]() {
        if (s.detail_console_input.empty()) return;
        const std::string command = s.detail_console_input;
        if (!start_local_server_command(st, sv, LocalServerCommandOrigin::DetailConsole,
                                        command)) {
            return;
        }
        server::ServerConsoleEntry cmd_entry;
        cmd_entry.timestamp = ">";
        cmd_entry.message = command;
        s.console_log.push_back(std::move(cmd_entry));
        s.detail_console_input.clear();
    };

    const bool command_busy = local_server_action_lane_busy(s) ||
                              local_server_command_lane_busy(s);

    // Command input. Reserve the exact button width and current style gap so
    // the Send action remains inside the content region at every DPI scale.
    const float send_width = ui_px(76.0f);
    const float send_gap = ImGui::GetStyle().ItemSpacing.x;
    const float command_width = std::max(ui_px(120.0f),
        ImGui::GetContentRegionAvail().x - send_width - send_gap);
    if (command_busy) ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(command_width);
    if (ImGui::InputText("##dconsole_cmd", &s.detail_console_input,
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
        submit_command();
        ImGui::SetKeyboardFocusHere(-1);
    }
    ImGui::SameLine(0, send_gap);
    const bool sending = local_server_command_is_working(
        s, LocalServerCommandOrigin::DetailConsole);
    if (primary_button(sending ? "Sending..." : "Send",
                       ImVec2(send_width, ImGui::GetFrameHeight()),
                       false, command_busy)) {
        submit_command();
    }
    if (command_busy) ImGui::EndDisabled();
}

// ---------------------------------------------------------------------------
// Server detail – overview tab
// ---------------------------------------------------------------------------

// This presentation reads only the fixture-seeded configuration and snapshot
// metrics. The normal overview reaches the local supervisor, runtime
// downloader, filesystem and persisted server list through its action row; a
// visual fixture must not inherit any of those paths merely to show the same
// composition.
static void draw_fixture_server_overview_tab(const server::ServerConfig& sv, const UiState& st) {
    const server::ServerStage stage = sv.stage;

    card_begin("##fixture_detail_hero", ImVec2(-1, 0));
    {
        const ImVec2 hero_origin = ImGui::GetCursorScreenPos();
        const float hero_height = ui_px(126.0f);
        const ImVec2 hero_size(ImGui::GetContentRegionAvail().x, hero_height);
        ImDrawList* hero_draw = ImGui::GetWindowDrawList();
        hero_draw->AddRectFilled(hero_origin, hero_origin + hero_size, c32(k.surface2),
                                 ui_px(10.0f));
        hero_draw->AddRect(hero_origin, hero_origin + hero_size, c32(k.border),
                           ui_px(10.0f));
        hero_draw->AddRectFilled(hero_origin,
                                 hero_origin + ImVec2(ui_px(5.0f), hero_height),
                                 c32(stage_color(stage)), ui_px(2.0f));
        hero_draw->AddCircleFilled(hero_origin + ImVec2(ui_px(34.0f), ui_px(34.0f)),
                                   ui_px(22.0f), c32(k.brand_dk));
        draw_icon(IconId::Server, hero_origin + ImVec2(ui_px(34.0f), ui_px(34.0f)),
                  ui_px(12.0f), c32(k.brand_hov));

        ImGui::SetCursorScreenPos(hero_origin + ImVec2(ui_px(68.0f), ui_px(14.0f)));
        ImGui::PushFont(f_title);
        ImGui::TextUnformatted(sv.name.c_str());
        ImGui::PopFont();
        ImGui::SameLine(0, ui_px(10.0f));
        draw_status_badge(stage);
        ImGui::TextColored(k.muted, "%s  •  Minecraft %s",
                           server::server_software_name(sv.software),
                           sv.minecraft_version.c_str());
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, k.brand_hov);
        ImGui::TextWrapped(
            "Visual fixture — representative local server state; no server, file, process, or launcher configuration is read or changed.");
        ImGui::PopStyleColor();

        ImGui::SetCursorScreenPos(ImVec2(hero_origin.x, hero_origin.y + hero_height));
        ImGui::Dummy(ImVec2(0, 0));
    }
    card_end();

    ImGui::Spacing();
    card_begin("##fixture_detail_metrics", ImVec2(-1, 0));
    ImGui::PushFont(f_bold);
    ImGui::TextColored(k.text, "Representative activity");
    ImGui::PopFont();
    ImGui::TextColored(k.muted, "Snapshot-only values for visual layout review.");
    ImGui::Spacing();
    const float column_width = std::max(ui_px(112.0f),
        (ImGui::GetContentRegionAvail().x - ui_px(30.0f)) / 4.0f);
    const auto& fixture_metrics = st.server_metrics;
    const bool metrics_available = fixture_metrics.valid;
    const ImVec4 unavailable_color = k.muted;
    const float cpu_pct = std::clamp(fixture_metrics.cpu_percent / 100.0f, 0.0f, 1.0f);
    const float ram_pct = std::clamp(fixture_metrics.ram_percent / 100.0f, 0.0f, 1.0f);
    const ImVec4 cpu_color = cpu_pct > 0.9f ? k.red : cpu_pct > 0.7f ? k.orange : k.green;
    const ImVec4 ram_color = ram_pct > 0.9f ? k.red : ram_pct > 0.7f ? k.orange : k.blue;
    const ImVec4 tps_color = fixture_metrics.tps >= 19.0f ? k.green
        : fixture_metrics.tps >= 15.0f ? k.yellow : k.red;
    char tps_text[32]{};
    if (metrics_available)
        std::snprintf(tps_text, sizeof(tps_text), "%.1f", fixture_metrics.tps);

    const struct {
        const char* label;
        std::string value;
        ImVec4 color;
    } metrics[] = {
        {"CPU", metrics_available
            ? std::to_string(static_cast<int>(fixture_metrics.cpu_percent)) + "%"
            : "Unavailable", metrics_available ? cpu_color : unavailable_color},
        {"RAM", metrics_available
            ? format_server_ram_mb(fixture_metrics.ram_mb) + " / " +
                format_server_ram_mb(sv.allocated_ram_mb)
            : "Unavailable", metrics_available ? ram_color : unavailable_color},
        {"TPS", metrics_available ? tps_text : "Unavailable",
            metrics_available ? tps_color : unavailable_color},
        {"PLAYERS", metrics_available
            ? std::to_string(fixture_metrics.players_online) + " / " +
                std::to_string(sv.max_players)
            : "Unavailable", metrics_available ? k.brand : unavailable_color},
    };
    for (size_t i = 0; i < sizeof(metrics) / sizeof(metrics[0]); ++i) {
        if (i) ImGui::SameLine(0, ui_px(10.0f));
        const ImVec2 metric_min = ImGui::GetCursorScreenPos();
        const ImVec2 metric_size(column_width, ui_px(52.0f));
        ImDrawList* metric_draw = ImGui::GetWindowDrawList();
        ImVec4 metric_bg = metrics[i].color;
        metric_bg.w = 0.10f;
        metric_draw->AddRectFilled(metric_min, metric_min + metric_size, c32(metric_bg),
                                   ui_px(7.0f));
        metric_draw->AddRect(metric_min, metric_min + metric_size,
                             c32(ImVec4(metrics[i].color.x, metrics[i].color.y,
                                         metrics[i].color.z, 0.45f)),
                             ui_px(7.0f));
        metric_draw->AddText(metric_min + ImVec2(ui_px(10.0f), ui_px(7.0f)),
                             c32(k.muted), metrics[i].label);
        metric_draw->AddText(f_bold, f_bold->LegacySize,
                             metric_min + ImVec2(ui_px(10.0f), ui_px(25.0f)),
                             c32(metrics[i].color), metrics[i].value.c_str());
        ImGui::Dummy(metric_size);
    }
    card_end();

    ImGui::Spacing();
    ImGui::TextColored(k.muted,
                       "Operational controls are shown for layout only and cannot act in this visual fixture.");
    ImGui::BeginDisabled();
    ghost_button("Stop", ImVec2(ui_px(108.0f), ui_px(32.0f)));
    ImGui::SameLine(0, ui_px(6.0f));
    ghost_button("Restart", ImVec2(ui_px(108.0f), ui_px(32.0f)));
    ImGui::SameLine(0, ui_px(6.0f));
    ghost_button("Open Folder", ImVec2(ui_px(132.0f), ui_px(32.0f)));
    ImGui::SameLine(0, ui_px(16.0f));
    danger_button("Remove from Amalgam", ImVec2(ui_px(178.0f), ui_px(32.0f)));
    ImGui::EndDisabled();
}

static void draw_server_overview_tab(ServerUIState& s, server::ServerConfig& sv, UiState& st) {
    if (st.fixture_mode) {
        draw_fixture_server_overview_tab(sv, st);
        return;
    }

    const server::ServerStage stage = effective_stage(st, sv);
    bool is_running = (stage == server::ServerStage::Running);
    bool is_ready = (stage == server::ServerStage::Ready ||
                     stage == server::ServerStage::Stopped ||
                     stage == server::ServerStage::NotInstalled);

    // ── Status hero card ────────────────────────────────────────────
    card_begin("##detail_hero", ImVec2(-1, 0));
    {
        const ImVec2 hero_art_pos = ImGui::GetCursorScreenPos();
        const float hero_art_height = ui_px(132.0f);
        const ImVec2 hero_art_size(ImGui::GetContentRegionAvail().x, hero_art_height);
        draw_local_image(st, st.exe_dir + L"\\branding\\ai\\server-card-ai-v2.png",
                         hero_art_pos, hero_art_size, c32(k.brand_dk));
        ImGui::GetWindowDrawList()->AddRectFilledMultiColor(
            hero_art_pos, hero_art_pos + hero_art_size,
            c32(ImVec4(k.bg.x, k.bg.y, k.bg.z, 0.94f)),
            c32(ImVec4(k.bg.x, k.bg.y, k.bg.z, 0.42f)),
            c32(ImVec4(k.bg.x, k.bg.y, k.bg.z, 0.52f)),
            c32(ImVec4(k.bg.x, k.bg.y, k.bg.z, 0.94f)));
        ImGui::GetWindowDrawList()->AddRect(hero_art_pos, hero_art_pos + hero_art_size,
                                            c32(k.border), ui_px(10.0f));

        // Wide screens otherwise leave a large, visually unanchored patch of
        // artwork beside the server identity. A compact, read-only limits
        // panel balances that space without hiding the scene or duplicating a
        // primary control. It deliberately disappears before it could crowd
        // the identity and status badges on compact widths.
        if (hero_art_size.x >= ui_px(860.0f)) {
            const float limits_w = std::min(ui_px(224.0f), hero_art_size.x * 0.26f);
            const float limits_h = ui_px(88.0f);
            const ImVec2 limits_min(hero_art_pos.x + hero_art_size.x - limits_w - ui_px(18.0f),
                                    hero_art_pos.y + (hero_art_height - limits_h) * 0.5f);
            const ImVec2 limits_max = limits_min + ImVec2(limits_w, limits_h);
            ImDrawList* hero_dl = ImGui::GetWindowDrawList();
            hero_dl->AddRectFilled(limits_min, limits_max,
                                   c32(ImVec4(k.bg.x, k.bg.y, k.bg.z, 0.80f)), ui_px(8.0f));
            hero_dl->AddRect(limits_min, limits_max,
                             c32(ImVec4(k.border.x, k.border.y, k.border.z, 0.92f)),
                             ui_px(8.0f));

            const ImVec2 text_pos = limits_min + ImVec2(ui_px(12.0f), ui_px(10.0f));
            const std::string memory_limit = format_server_ram_mb(sv.allocated_ram_mb);
            const std::string player_capacity = std::to_string(sv.max_players) +
                (sv.max_players == 1 ? " player slot" : " player slots");
            hero_dl->AddText(f_small, f_small->LegacySize, text_pos, c32(k.muted),
                             "MEMORY LIMIT");
            hero_dl->AddText(f_bold, f_bold->LegacySize,
                             text_pos + ImVec2(0, ui_px(16.0f)), c32(k.blue),
                             memory_limit.c_str());
            hero_dl->AddText(f_small, f_small->LegacySize,
                             text_pos + ImVec2(0, ui_px(47.0f)), c32(k.muted),
                             "PLAYER CAPACITY");
            hero_dl->AddText(f_bold, f_bold->LegacySize,
                             text_pos + ImVec2(0, ui_px(63.0f)), c32(k.green),
                             player_capacity.c_str());
        }

        // Left status strip
        ImVec2 card_min = ImGui::GetCursorScreenPos();
        ImVec4 strip_color = stage_color(stage);
        const float strip_inset = ui_px(8.0f);
        ImGui::GetWindowDrawList()->AddRectFilled(
            card_min + ImVec2(0, strip_inset),
            ImVec2(card_min.x + ui_px(5.0f), card_min.y + hero_art_height - strip_inset),
            c32(strip_color), ui_px(2.0f));

        // Indent the full text column, not just the first line. The previous
        // one-line spacer reset after a newline, letting status copy collide
        // with the green status rail.
        ImGui::Indent(ui_px(14.0f));

        // Name + badges
        ImGui::PushFont(f_title);
        ImGui::TextUnformatted(sv.name.c_str());
        ImGui::PopFont();
        ImGui::SameLine(0, ui_px(12.0f));
        draw_status_badge(stage);
        ImGui::SameLine(0, ui_px(8.0f));
        {
            ImDrawList* dl2 = ImGui::GetWindowDrawList();
            ImVec2 bp = ImGui::GetCursorScreenPos();
            ImVec4 sw_bg = k.brand;
            sw_bg.w = 0.15f;
            draw_badge(dl2, bp, server::server_software_name(sv.software), k.brand, sw_bg);
            ImGui::Dummy(ImVec2(
                ImGui::CalcTextSize(server::server_software_name(sv.software)).x + ui_px(14.0f),
                ui_px(20.0f)));
        }
        ImGui::SameLine(0, ui_px(8.0f));
        {
            ImDrawList* dl3 = ImGui::GetWindowDrawList();
            ImVec2 bp = ImGui::GetCursorScreenPos();
            ImVec4 ver_bg = k.blue;
            ver_bg.w = 0.12f;
            draw_badge(dl3, bp, sv.minecraft_version.c_str(), k.blue, ver_bg);
            ImGui::Dummy(ImVec2(
                ImGui::CalcTextSize(sv.minecraft_version.c_str()).x + ui_px(14.0f),
                ui_px(20.0f)));
        }

        ImGui::Spacing();

        // Config summary (stat chips)
        ImDrawList* dl = ImGui::GetWindowDrawList();
        auto meta_chip = [&](const char* text, const ImVec4& accent) {
            const ImVec2 bp = ImGui::GetCursorScreenPos();
            const ImVec2 sz = ImGui::CalcTextSize(text) + ImVec2(ui_px(14.0f), ui_px(8.0f));
            ImVec4 bg = accent; bg.w = 0.12f;
            dl->AddRectFilled(bp, bp + sz, c32(bg), ui_px(6.0f));
            dl->AddText(bp + ImVec2(ui_px(7.0f), ui_px(4.0f)), c32(accent), text);
            ImGui::Dummy(sz + ImVec2(0, ui_px(4.0f)));
        };
        std::string port_chip = "Port " + std::to_string(sv.port);
        meta_chip(port_chip.c_str(), k.muted);
        ImGui::SameLine(0, ui_px(6.0f));
        std::string ram_chip = "RAM " + format_server_ram_mb(sv.allocated_ram_mb);
        meta_chip(ram_chip.c_str(), k.blue);
        ImGui::SameLine(0, ui_px(6.0f));
        std::string max_chip = "Max " + std::to_string(sv.max_players) + " players";
        meta_chip(max_chip.c_str(), k.green);

        if (!sv.status_message.empty()) {
            ImGui::Spacing();
            const ImVec4 sc = (stage == server::ServerStage::Error) ? k.red : k.muted;
            const std::string readable_status = humanize_server_status_message(sv.status_message);
            ImGui::PushFont(f_small);
            ImGui::TextColored(sc, "%s", readable_status.c_str());
            ImGui::PopFont();
        }

        const float content_bottom = ImGui::GetCursorScreenPos().y;
        ImGui::Unindent(ui_px(14.0f));
        if (content_bottom < hero_art_pos.y + hero_art_height) {
            ImGui::SetCursorScreenPos(ImVec2(hero_art_pos.x,
                                             hero_art_pos.y + hero_art_height));
            ImGui::Dummy(ImVec2(0, 0));
        }
    }
    card_end();

    ImGui::Spacing();

    // ── Runtime download progress ───────────────────────────────────
    if (downloading_runtime_for(sv.name)) {
        const float transfer = runtime_download().progress.load();
        card_begin("##detail_download", ImVec2(-1, 0));
        ImGui::PushFont(f_bold);
        ImGui::TextUnformatted(download_phase().c_str());
        ImGui::PopFont();
        ImGui::Spacing();
        char overlay[64]{};
        if (transfer >= 0.0f)
            std::snprintf(overlay, sizeof(overlay), "%d%%", static_cast<int>(transfer * 100.0f));
        progress_bar(transfer, ImVec2(-1, ui_px(8.0f)), overlay);
        ImGui::PushFont(f_small);
        if (download_can_cancel()) {
            ImGui::TextColored(k.muted,
                               "You can cancel while this file downloads; this page updates as it goes.");
        } else {
            ImGui::TextColored(k.muted,
                               "This step cannot be cancelled safely. It will finish or report an error.");
        }
        ImGui::PopFont();
        card_end();
        ImGui::Spacing();
    }

    // ── Quick actions row ───────────────────────────────────────────
    {
        float bw = ui_px(100.0f);
        float bh = ui_px(32.0f);

        const bool local_action_busy = local_server_action_lane_busy(s) ||
                                       local_server_command_lane_busy(s);
        const bool start_working = local_server_action_is_working(s, LocalServerAction::Start);
        const bool stop_working = local_server_action_is_working(s, LocalServerAction::Stop);
        const bool restart_working = local_server_action_is_working(s, LocalServerAction::Restart);
        if (is_running) {
            if (ghost_button(stop_working ? "Stopping..." : "Stop",
                             ImVec2(bw + ui_px(10.0f), bh), local_action_busy)) {
                start_local_server_action(st, s.detail_server_idx, LocalServerAction::Stop);
            }
            ImGui::SameLine(0, ui_px(6.0f));
            if (ghost_button(restart_working ? "Restarting..." : "Restart",
                             ImVec2(bw + ui_px(10.0f), bh), local_action_busy)) {
                start_local_server_action(st, s.detail_server_idx, LocalServerAction::Restart);
            }
        } else if (downloading_runtime_for(sv.name)) {
            if (download_can_cancel()) {
                if (ghost_button("Cancel download", ImVec2(bw + ui_px(30.0f), bh))) {
                    runtime_download().cancel.store(true);
                }
            } else {
                ImGui::BeginDisabled();
                ghost_button("Working", ImVec2(bw + ui_px(30.0f), bh));
                ImGui::EndDisabled();
            }
        } else if (!runtime_files_present(sv)) {
            // The files the server starts from are not on disk. Offer the
            // download here instead of a Start that can only fail. A manual
            // runtime has nothing to fetch, so that case opens the folder the
            // jar has to be placed in.
            const bool manual =
                server::server_software_kind(sv.software) == server::RuntimeKind::Manual;
            if (primary_button(manual ? "Add server file" : "Prepare server files",
                               ImVec2(bw + ui_px(50.0f), bh))) {
                if (manual) {
                    ShellExecuteW(nullptr, L"open",
                        aml::net::to_wide(sv.server_directory).c_str(),
                        nullptr, nullptr, SW_SHOWNORMAL);
                } else {
                    start_provision(st, s.detail_server_idx);
                }
            }
            ImGui::SameLine(0, ui_px(6.0f));
            if (ghost_button("Open Folder", ImVec2(bw + ui_px(20.0f), bh))) {
                ShellExecuteW(nullptr, L"open",
                    aml::net::to_wide(sv.server_directory).c_str(),
                    nullptr, nullptr, SW_SHOWNORMAL);
            }
        } else if (is_ready) {
            if (primary_button(start_working ? "Starting..." : "Start",
                               ImVec2(bw + ui_px(10.0f), bh), start_working,
                               local_action_busy)) {
                start_local_server_action(st, s.detail_server_idx, LocalServerAction::Start);
            }
            ImGui::SameLine(0, ui_px(6.0f));
            if (ghost_button("Open Folder", ImVec2(bw + ui_px(20.0f), bh))) {
                ShellExecuteW(nullptr, L"open",
                    aml::net::to_wide(sv.server_directory).c_str(),
                    nullptr, nullptr, SW_SHOWNORMAL);
            }
        }

        // Split irreversible removal away from the operating controls. Stop
        // and Restart stay neutral; a red outline gives Delete Server its own
        // unmistakable risk tier before the confirmation step appears.
        ImGui::SameLine(0, ui_px(16.0f));
        const ImVec2 divider_min = ImGui::GetCursorScreenPos();
        ImVec4 divider_color = k.border;
        divider_color.w = 0.9f;
        ImGui::GetWindowDrawList()->AddLine(
            divider_min + ImVec2(0, ui_px(5.0f)),
            divider_min + ImVec2(0, bh - ui_px(5.0f)),
            c32(divider_color), ui_px(1.0f));
        ImGui::Dummy(ImVec2(ui_px(1.0f), bh));
        ImGui::SameLine(0, ui_px(16.0f));
        if (danger_button("Remove from Amalgam", ImVec2(bw + ui_px(52.0f), bh),
                          local_action_busy)) {
            s.action_pending = s.detail_server_idx;
            s.remove_server_error.clear();
        }
        if (ImGui::IsItemHovered()) {
            draw_tooltip("Removes this server only from Amalgam. Its server folder and files stay where they are.");
        }
    }

    ImGui::Spacing();

    // ── Performance metrics (if running) ────────────────────────────
    if (is_running) {
        card_begin("##detail_metrics", ImVec2(-1, 0));
        ImGui::PushFont(f_h2);
        ImGui::TextColored(k.text, "Live Performance");
        ImGui::PopFont();
        ImGui::Spacing();

        auto& metrics = st.server_metrics;
        if (!metrics.valid) {
            // A missing telemetry provider is a normal, honest state for a
            // local server. Keep it compact and actionable instead of turning
            // the overview into a large empty panel.
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            draw_icon(IconId::Info, origin + ImVec2(ui_px(12.0f), ui_px(15.0f)),
                      ui_px(9.0f), c32(k.muted));
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ui_px(34.0f));
            ImGui::PushFont(f_bold);
            ImGui::TextUnformatted("Telemetry not reported yet");
            ImGui::PopFont();
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ui_px(34.0f));
            ImGui::TextColored(k.muted,
                               "Lifecycle state is authoritative; CPU, RAM, TPS, and player counts will appear when the server exposes a health snapshot.");
            ImGui::Spacing();
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ui_px(34.0f));
            if (ghost_button("Open Console", ImVec2(ui_px(122.0f), ui_px(28.0f))))
                s.detail_tab = 1;
        } else {

        // Stat cards row
        float card_w = (ImGui::GetContentRegionAvail().x - ui_px(36.0f)) / 4.0f;

        float max_h = 0;
        float cpu_pct = metrics.cpu_percent / 100.0f;
        ImVec4 cpu_color = cpu_pct > 0.9f ? k.red : cpu_pct > 0.7f ? k.orange : k.green;
        const std::string cpu_value = metrics.cpu_valid
            ? std::to_string(static_cast<int>(metrics.cpu_percent)) + "%"
            : "Unavailable";
        max_h = std::max(max_h, draw_stat_card(
            "CPU", cpu_value.c_str(), metrics.cpu_valid ? cpu_pct : -1.0f,
            metrics.cpu_valid ? cpu_color : k.muted, card_w));
        ImGui::SameLine(0, ui_px(12.0f));

        float ram_pct = metrics.ram_percent / 100.0f;
        ImVec4 ram_color = ram_pct > 0.9f ? k.red : ram_pct > 0.7f ? k.orange : k.blue;
        const std::string ram_value = metrics.ram_valid
            ? format_server_ram_mb(metrics.ram_mb) : "Unavailable";
        max_h = std::max(max_h, draw_stat_card(
            "RAM", ram_value.c_str(), metrics.ram_valid ? ram_pct : -1.0f,
            metrics.ram_valid ? ram_color : k.muted, card_w));
        ImGui::SameLine(0, ui_px(12.0f));

        ImVec4 tps_col = metrics.tps >= 19.0f ? k.green : metrics.tps >= 15.0f ? k.yellow : k.red;
        float tps_pct = metrics.tps / 20.0f;
        char tps_value[32]{};
        if (metrics.tps_valid)
            std::snprintf(tps_value, sizeof(tps_value), "%.1f", metrics.tps);
        else
            std::snprintf(tps_value, sizeof(tps_value), "Unavailable");
        // TPS is a health metric, not capacity: 19.8/20 is explicitly green.
        // Its semantic status color also drives the bar so the card cannot
        // contradict its own healthy reading through generic high-is-danger
        // capacity thresholds.
        max_h = std::max(max_h, draw_stat_card(
            "TPS", tps_value, metrics.tps_valid ? tps_pct : -1.0f,
            metrics.tps_valid ? tps_col : k.muted, card_w,
            ui_model::StatCardProgressPolarity::HigherIsBetter, &tps_col));
        ImGui::SameLine(0, ui_px(12.0f));

        float pl_pct = (float)metrics.players_online / (float)std::max(1, sv.max_players);
        const std::string players_value = metrics.players_valid
            ? std::to_string(metrics.players_online) + " / " + std::to_string(sv.max_players)
            : "Unavailable";
        max_h = std::max(max_h, draw_stat_card(
            "Players", players_value.c_str(), metrics.players_valid ? pl_pct : -1.0f,
            metrics.players_valid ? k.brand : k.muted, card_w));
        }

        card_end();
    }
}

// ---------------------------------------------------------------------------
// Server detail – plugins tab
// ---------------------------------------------------------------------------

static void draw_server_plugins_tab(ServerUIState&, const server::ServerConfig& sv, UiState& st) {
    ImGui::PushFont(f_h2);
    ImGui::TextColored(k.text, "Plugins & Mods");
    ImGui::PopFont();
    ImGui::Spacing();

    // Directory enumeration is intentionally replaced with a static fixture
    // list so a screenshot cannot disclose local jar names or paths.
    if (st.fixture_mode) {
        ImGui::TextColored(k.brand_hov,
                           "Visual fixture: representative local mod inventory; file actions are disabled.");
        ImGui::Spacing();
        ImGui::PushFont(f_h2);
        ImGui::TextColored(k.text, "Mods");
        ImGui::PopFont();
        ImGui::Spacing();
        const char* mods[] = {"Fabric API 0.92.2", "Lithium 0.12.7", "Sodium 0.5.11"};
        card_begin("##fixture_mods_list", ImVec2(-1, 0));
        for (size_t i = 0; i < sizeof(mods) / sizeof(mods[0]); ++i) {
            ImGui::PushID(static_cast<int>(i));
            draw_icon(IconId::Cube, ImGui::GetCursorScreenPos() +
                      ImVec2(ui_px(8.0f), ui_px(8.0f)), ui_px(6.0f), c32(k.brand));
            ImGui::Dummy(ImVec2(ui_px(16.0f), ui_px(16.0f)));
            ImGui::SameLine(0, ui_px(4.0f));
            ImGui::TextColored(k.text, "%s", mods[i]);
            ImGui::PopID();
        }
        card_end();
        ImGui::TextColored(k.muted, "%d mods in this representative server",
                           (int)(sizeof(mods) / sizeof(mods[0])));
        return;
    }

    namespace fs = std::filesystem;
    bool has_plugins = false;
    bool has_mods = false;
    std::string plugins_dir = sv.server_directory + "\\plugins";
    std::string mods_dir = sv.server_directory + "\\mods";

    std::error_code ec;
    has_plugins = fs::exists(aml::net::to_wide(plugins_dir), ec);
    has_mods = fs::exists(aml::net::to_wide(mods_dir), ec);

    if (!has_plugins && !has_mods) {
        empty_state("No plugins or mods directory",
                    "Create a 'plugins' or 'mods' folder in your server directory to add them.");
        ImGui::Spacing();
        if (ghost_button("Open Server Folder", ImVec2(ui_px(140.0f), ui_px(28.0f)))) {
            ShellExecuteW(nullptr, L"open",
                aml::net::to_wide(sv.server_directory).c_str(),
                nullptr, nullptr, SW_SHOWNORMAL);
        }
        return;
    }

    if (has_plugins) {
        ImGui::PushFont(f_h2);
        ImGui::TextColored(k.text, "Plugins");
        ImGui::PopFont();
        ImGui::Spacing();

        std::vector<std::string> jars;
        for (auto& entry : fs::directory_iterator(aml::net::to_wide(plugins_dir), ec)) {
            if (entry.is_regular_file(ec)) {
                auto ext = entry.path().extension().u8string();
                std::string ext_s(reinterpret_cast<const char*>(ext.c_str()));
                if (ext_s == ".jar") {
                    auto fname = entry.path().filename().u8string();
                    jars.emplace_back(reinterpret_cast<const char*>(fname.c_str()));
                }
            }
        }
        std::sort(jars.begin(), jars.end());

        if (jars.empty()) {
            empty_state("No plugins installed", "Drop .jar files into the plugins folder.");
        } else {
            card_begin("##plugins_list", ImVec2(-1, 0));
            for (size_t i = 0; i < jars.size(); ++i) {
                ImGui::PushID(static_cast<int>(i + 4000));
                const ImVec2 icon_pos = ImGui::GetCursorScreenPos();
                draw_icon(IconId::Cube,
                          icon_pos + ImVec2(ui_px(8.0f), ui_px(8.0f)),
                          ui_px(6.0f), c32(k.brand));
                ImGui::Dummy(ImVec2(ui_px(16.0f), ui_px(16.0f)));
                ImGui::SameLine(0, ui_px(4.0f));
                ImGui::TextColored(k.text, "%s", jars[i].c_str());
                ImGui::PopID();
            }
            card_end();
            ImGui::TextColored(k.muted, "%d plugins", (int)jars.size());
        }
        ImGui::Spacing();
    }

    if (has_mods) {
        ImGui::PushFont(f_h2);
        ImGui::TextColored(k.text, "Mods");
        ImGui::PopFont();
        ImGui::Spacing();

        std::vector<std::string> jars;
        for (auto& entry : fs::directory_iterator(aml::net::to_wide(mods_dir), ec)) {
            if (entry.is_regular_file(ec)) {
                auto ext = entry.path().extension().u8string();
                std::string ext_s(reinterpret_cast<const char*>(ext.c_str()));
                if (ext_s == ".jar") {
                    auto fname = entry.path().filename().u8string();
                    jars.emplace_back(reinterpret_cast<const char*>(fname.c_str()));
                }
            }
        }
        std::sort(jars.begin(), jars.end());

        if (jars.empty()) {
            empty_state("No mods installed", "Drop .jar files into the mods folder.");
        } else {
            card_begin("##mods_list", ImVec2(-1, 0));
            for (size_t i = 0; i < jars.size(); ++i) {
                ImGui::PushID(static_cast<int>(i + 5000));
                const ImVec2 icon_pos = ImGui::GetCursorScreenPos();
                draw_icon(IconId::Cube,
                          icon_pos + ImVec2(ui_px(8.0f), ui_px(8.0f)),
                          ui_px(6.0f), c32(k.brand));
                ImGui::Dummy(ImVec2(ui_px(16.0f), ui_px(16.0f)));
                ImGui::SameLine(0, ui_px(4.0f));
                ImGui::TextColored(k.text, "%s", jars[i].c_str());
                ImGui::PopID();
            }
            card_end();
            ImGui::TextColored(k.muted, "%d mods", (int)jars.size());
        }
    }
}

// ---------------------------------------------------------------------------
// Server detail – properties tab (embedded)
// ---------------------------------------------------------------------------

static void draw_server_properties_tab(ServerUIState& s, const server::ServerConfig& sv, UiState& st) {
    ImGui::PushFont(f_h2);
    ImGui::TextColored(k.text, "server.properties");
    ImGui::PopFont();
    ImGui::Spacing();

    // Render an explicit static sample before any local server file is read.
    if (st.fixture_mode) {
        ImGui::TextColored(k.brand_hov,
                           "Visual fixture: representative settings; editing and disk access are disabled.");
        ImGui::Spacing();
        struct FixtureProperty { const char* key; const char* value; };
        const FixtureProperty properties[] = {
            {"motd", "Forsaken World SMP"},
            {"gamemode", "survival"},
            {"difficulty", "normal"},
            {"max-players", "12"},
            {"online-mode", "true"},
            {"pvp", "true"},
        };
        card_begin("##fixture_server_properties", ImVec2(-1, 0));
        for (const auto& property : properties) {
            ImGui::TextColored(k.brand, "%s", property.key);
            ImGui::SameLine(ui_px(220.0f));
            ImGui::TextColored(k.text, "%s", property.value);
        }
        card_end();
        return;
    }

    // Load properties if needed
    if (s.properties.empty() || s.properties_server_idx != s.detail_server_idx) {
        load_server_properties(sv.server_directory, s.properties);
        s.properties_server_idx = s.detail_server_idx;
        s.properties_dirty = false;
    }

    if (s.properties.empty()) {
        empty_state("No server.properties found",
                    "Start the server once to generate the properties file.");
        return;
    }

    ImGui::TextColored(k.muted, "%d properties", (int)s.properties.size());
    ImGui::Spacing();

    float footer_h = ImGui::GetFrameHeightWithSpacing() + ui_px(8.0f);
    if (ImGui::BeginChild("##detail_props_list",
            ImVec2(-1, -(footer_h)), ImGuiChildFlags_Borders)) {
        for (size_t i = 0; i < s.properties.size(); ++i) {
            auto& [key, val] = s.properties[i];
            ImGui::PushID(static_cast<int>(i + 6000));

            ImGui::TextColored(k.brand, "%s", key.c_str());
            ImGui::SameLine(ui_px(220.0f));
            ImGui::SetNextItemWidth(-1);

            bool is_bool = (key == "online-mode" || key == "pvp" ||
                            key == "enable-command-block" || key == "spawn-monsters" ||
                            key == "spawn-animals" || key == "spawn-npcs" ||
                            key == "white-list" || key == "enforce-whitelist" ||
                            key == "allow-flight" || key == "allow-nether" ||
                            key == "spawn-protection");
            if (is_bool) {
                bool bval = (val == "true");
                if (ImGui::Checkbox("##val", &bval)) {
                    val = bval ? "true" : "false";
                    s.properties_dirty = true;
                }
            } else {
                char buf[512];
                strncpy(buf, val.c_str(), sizeof(buf) - 1);
                buf[sizeof(buf) - 1] = '\0';
                if (ImGui::InputText("##val", buf, sizeof(buf))) {
                    val = buf;
                    s.properties_dirty = true;
                }
            }

            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    ImGui::Spacing();
    if (s.properties_dirty) {
        if (primary_button("Save Properties",
                           ImVec2(ui_px(130.0f), ui_px(30.0f)))) {
            save_server_properties(sv.server_directory, s.properties);
            s.properties_dirty = false;
        }
        ImGui::SameLine();
    }
    if (ghost_button("Reload", ImVec2(ui_px(80.0f), ui_px(30.0f)))) {
        load_server_properties(sv.server_directory, s.properties);
        s.properties_dirty = false;
    }
}

// ---------------------------------------------------------------------------
// Server detail – Connect server version (for external servers)
// ---------------------------------------------------------------------------

static void draw_server_detail_connect(UiState& st) {
    auto& s = state();
    if (s.detail_connect_idx < 0 || s.detail_connect_idx >= static_cast<int>(st.cfg->servers.size()))
        return;

    const auto& server = st.cfg->servers[s.detail_connect_idx];
    static const char* kTypeLabels[] = {"Java", "Bedrock", "Modded (Java)", "Bedrock Modded"};
    int type_idx = (server.type >= 0 && server.type < 4) ? server.type : 0;

    // Breadcrumb
    if (ghost_button("< Servers", ImVec2(ui_px(120.0f), ui_px(28.0f)))) {
        s.detail_connect_idx = -1;
    }
    ImGui::Spacing();

    // Hero card
    card_begin("##connect_detail_hero", ImVec2(-1, 0));
    ImGui::PushFont(f_title);
    ImGui::TextUnformatted(server.name.c_str());
    ImGui::PopFont();
    ImGui::Spacing();

    ImGui::TextColored(k.muted, "Address:");
    ImGui::SameLine();
    ImGui::TextColored(k.text, "%s", net::to_utf8(server.address).c_str());

    ImGui::TextColored(k.muted, "Type:");
    ImGui::SameLine();
    ImGui::TextColored(k.brand, "%s", kTypeLabels[type_idx]);

    if (!server.profile.empty()) {
        ImGui::TextColored(k.muted, "Profile:");
        ImGui::SameLine();
        ImGui::TextColored(k.brand, "%s", server.profile.c_str());
    }

    ImGui::Spacing();
    ImGui::Spacing();

    // Actions
    float bw = ui_px(100.0f);
    float bh = ui_px(32.0f);

    if (primary_button("Play", ImVec2(bw + ui_px(10.0f), bh)) && !st.running) {
        if (!server.profile.empty()) {
            st.selected = server.profile;
            st.active_instance_dir.clear();
            for (auto& inst : st.instance_list) {
                if (inst.id == server.profile) {
                    st.active_instance_dir = inst.directory;
                    break;
                }
            }
        }
        if (!st.active_instance_dir.empty() && !st.selected.empty()) {
            st.pending_instance_dir = st.active_instance_dir;
            st.pending_launch = true;
            st.pending_id = st.selected;
            st.pending_server = net::to_utf8(server.address);
        }
    }
    ImGui::SameLine(0, ui_px(6.0f));
    if (ghost_button("Edit", ImVec2(bw, bh))) {
        s.detail_connect_idx = -1;
        st.selected_server = s.detail_connect_idx;
        st.server_name = server.name;
        st.ui_server = net::to_utf8(server.address);
        st.server_edit_type = server.type;
        st.server_edit_profile = server.profile;
    }
    ImGui::SameLine(0, ui_px(6.0f));
    if (ghost_button("Copy Address", ImVec2(bw + ui_px(10.0f), bh))) {
        ImGui::SetClipboardText(net::to_utf8(server.address).c_str());
    }

    card_end();
}

// ---------------------------------------------------------------------------
// Main server detail panel
// ---------------------------------------------------------------------------

static void draw_server_detail(UiState& st) {
    auto& s = state();

    // ── Connect mode detail ───────────────────────────────────────
    if (s.detail_connect_idx >= 0) {
        draw_server_detail_connect(st);
        return;
    }

    // ── Host mode detail ──────────────────────────────────────────
    if (s.detail_server_idx < 0 || s.detail_server_idx >= static_cast<int>(st.servers.size()))
        return;

    server::ServerConfig& sv = st.servers[s.detail_server_idx];
    s.detail_tab = std::clamp(s.detail_tab, 0, 6);

    // Seed the console log from the UiState-level log so both the fixture and
    // any live console feed share one source of truth.
    if (st.fixture_mode) {
        s.console_log = st.server_console_log;
    } else if (s.console_log.empty() && !st.server_console_log.empty()) {
        s.console_log = st.server_console_log;
    }

    // Breadcrumb. A seeded visual fixture is intentionally a static route:
    // tab and back navigation would otherwise expose arbitrary operational
    // detail views during a capture.
    if (!st.fixture_mode &&
        ghost_button("< Servers", ImVec2(ui_px(120.0f), ui_px(28.0f)))) {
        s.detail_server_idx = -1;
        return;
    }
    if (st.fixture_mode) {
        ghost_button("< Servers", ImVec2(ui_px(120.0f), ui_px(28.0f)), true);
        ImGui::SameLine(0, ui_px(8.0f));
        ImGui::TextColored(k.brand_hov,
                           "Visual fixture — navigation and server actions are disabled.");
    }
    ImGui::Spacing();

    // ── Tab bar ───────────────────────────────────────────────────
    static const char* kTabs[] = {
        "Overview", "Console", "Files", "Players",
        "Plugins", "Properties", "World"
    };
    static const int kTabCount = 7;

    // Premium pill tabs (matching the design-system tab language)
    const float tab_gap = ui_px(6.0f);
    const float tab_h = ui_px(32.0f);
    for (int i = 0; i < kTabCount; ++i) {
        const bool active = (s.detail_tab == i);
        const char* label = kTabs[i];
        const ImVec2 label_sz = ImGui::CalcTextSize(label);
        const float pad_x = ui_px(14.0f);
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
        dl->AddRectFilled(tab_min, tab_min + tab_size,
                          c32(active ? k.surface2 : ImVec4(0, 0, 0, 0)),
                          ui_px(8.0f));
        if (active)
            dl->AddRect(tab_min, tab_min + tab_size, c32(k.brand), ui_px(8.0f),
                        0, ui_px(1.5f));
        if (st.fixture_mode) ImGui::BeginDisabled();
        ImGui::InvisibleButton((std::string("##srv_tab_") + std::to_string(i)).c_str(),
                               tab_size);
        if (st.fixture_mode) ImGui::EndDisabled();
        if (ImGui::IsItemHovered() && !active)
            dl->AddRect(tab_min, tab_min + tab_size, c32(k.border), ui_px(8.0f),
                        0, ui_px(1.0f));
        if (!st.fixture_mode && ImGui::IsItemClicked()) s.detail_tab = i;
        ImGui::PushFont(active ? f_bold : f_body);
        dl->AddText(tab_min + ImVec2(pad_x, (tab_h - ImGui::GetTextLineHeight()) * 0.5f),
                    c32(active ? k.text : k.muted), label);
        ImGui::PopFont();
        if (!st.fixture_mode && ImGui::IsItemHovered())
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    ImGui::Spacing();

    // ── Tab content ───────────────────────────────────────────────
    switch (s.detail_tab) {
        case 0: draw_server_overview_tab(s, sv, st); break;
        case 1: draw_server_detail_console(s, sv, st); break;
        case 2: draw_server_files_tab(s, sv, st); break;
        case 3: draw_server_players_tab(s, sv, st); break;
        case 4: draw_server_plugins_tab(s, sv, st); break;
        case 5: draw_server_properties_tab(s, sv, st); break;
        case 6: draw_server_world_tab(s, sv, st); break;
    }
}

// ---------------------------------------------------------------------------
// Create dialog
// ---------------------------------------------------------------------------

// The versions the selected software actually publishes, plus why they are
// missing when a provider cannot be reached. The bundled catalogue is the
// first paint so the dialog is never empty, and it is labelled as such.
static std::vector<std::string> create_versions(ServerUIState& s, bool* live,
                                                std::string* error) {
    std::lock_guard<std::mutex> lock(s.version_mu);
    *live = s.version_live;
    *error = s.version_error;
    if (s.version_live && !s.version_list.empty()) return s.version_list;
    return version_catalog::server_versions();
}

static void draw_create_dialog(UiState& st) {
    auto& s = state();
    if (!s.create_open) return;

    // A visual fixture must never make the dialog's eager provider request.
    // The bundled catalog is intentionally sufficient to render the complete
    // selector and is identified as such below.
    if (st.fixture_mode) {
        std::lock_guard<std::mutex> lock(s.version_mu);
        s.version_software = s.create_software_idx;
        s.version_list.clear();
        s.version_live = false;
        s.version_loading = false;
        s.version_error.clear();
    } else {
        request_software_versions(st, s.create_software_idx);
    }
    bool versions_live = false;
    std::string versions_error;
    const std::vector<std::string> supported_versions =
        create_versions(s, &versions_live, &versions_error);
    bool versions_loading = false;
    {
        std::lock_guard<std::mutex> lock(s.version_mu);
        versions_loading = s.version_loading;
    }
    if (s.create_version.empty() && !supported_versions.empty())
        s.create_version = supported_versions.front();

    ImGui::OpenPopup("Create Server");
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(
        std::min(ui_px(660.0f), ImGui::GetMainViewport()->WorkSize.x - ui_px(40.0f)),
        std::min(ui_px(690.0f), ImGui::GetMainViewport()->WorkSize.y - ui_px(40.0f))),
        ImGuiCond_Appearing);

    if (ImGui::BeginPopupModal("Create Server", &s.create_open,
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {

        // Keep the decision and recovery copy reachable at compact heights.
        // The body owns its own scroll range while this footer never moves
        // below the viewport, so Create/Cancel remain usable after the
        // validation text grows or the desktop window is short. A file-write
        // failure also reserves room for its exact recovery detail here, so a
        // compact viewport never makes the user rely on scroll position.
        const float create_footer_height = s.create_error.empty()
            ? ui_px(78.0f) : ui_px(128.0f);
        bool valid = false;
        std::string footer_status;
        ImVec4 footer_status_color = k.muted;
        if (ImGui::BeginChild("##create_server_body",
                              ImVec2(0.0f, -create_footer_height),
                              ImGuiChildFlags_None)) {
        card_begin("##create_server_hero", ImVec2(-1, ui_px(88.0f)));
        const ImVec2 hero_origin = ImGui::GetCursorScreenPos();
        ImDrawList* hero_draw = ImGui::GetWindowDrawList();
        hero_draw->AddCircleFilled(hero_origin + ImVec2(ui_px(29.0f), ui_px(29.0f)), ui_px(24.0f),
                                   c32(ImVec4(k.brand_dk.x, k.brand_dk.y, k.brand_dk.z, 0.92f)));
        draw_icon(IconId::Server, hero_origin + ImVec2(ui_px(29.0f), ui_px(29.0f)), ui_px(13.0f), c32(k.brand_hov));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ui_px(64.0f));
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("Create a local server");
        ImGui::PopFont();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ui_px(64.0f));
        ImGui::TextColored(k.muted, "Choose a runtime, capacity, and protected server settings. You can edit all of this later.");
        card_end();
        ImGui::Spacing();

        // Fixture state is a staged visual sample, not an editable draft.
        // Disable the operational fields as well as the Create footer so no
        // review interaction can alter even transient creation choices.
        if (st.fixture_mode) ImGui::BeginDisabled();
        card_begin("##create_server_settings", ImVec2(-1, 0));
        ImGui::TextColored(k.muted, "SERVER BASICS");
        ImGui::PushFont(f_bold);
        ImGui::TextUnformatted("Server name");
        ImGui::PopFont();
        ImGui::SetNextItemWidth(-1);
        if (input_text_hint("##cs_name", "For example: Survival Realm", &s.create_name)) {
            s.create_error.clear();
        }

        ImGui::Spacing();
        ImGui::PushFont(f_bold);
        ImGui::TextUnformatted("Server software");
        ImGui::PopFont();
        ImGui::SetNextItemWidth(-1);
        const auto& catalog = server::software_catalog();
        if (ImGui::BeginCombo("##cs_sw", catalog[s.create_software_idx].label)) {
            for (size_t i = 0; i < catalog.size(); ++i) {
                const bool selected = s.create_software_idx == static_cast<int>(i);
                if (ImGui::Selectable(catalog[i].label, selected) && !selected) {
                    s.create_software_idx = static_cast<int>(i);
                    // A version from the previous software means nothing to
                    // this one, so the selector waits for the new list.
                    s.create_version.clear();
                    if (server::is_native_software(catalog[i].software)) s.create_port = 19132;
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::TextColored(k.muted, "%s", catalog[s.create_software_idx].note);

        ImGui::Spacing();
        ImGui::PushFont(f_bold);
        ImGui::TextUnformatted("Minecraft version");
        ImGui::PopFont();
        if (server::is_native_software(software_at(s.create_software_idx)))
            ImGui::TextColored(k.muted,
                               "Bedrock publishes one current build; the launcher installs that one.");
        else if (versions_loading)
            ImGui::TextColored(k.muted, "Loading versions from the provider…");
        else if (!versions_error.empty())
            ImGui::TextColored(k.yellow,
                               "Provider version list unavailable (%s). Showing bundled targets.",
                               versions_error.c_str());
        else
            ImGui::TextColored(k.muted, "Every version this software publishes; %s.",
                                versions_live ? "fetched from the provider just now"
                                              : "bundled with this launcher");
        if (st.fixture_mode)
            ImGui::TextColored(k.brand_hov,
                                "Visual fixture: bundled targets only; no provider was contacted.");
        ImGui::SetNextItemWidth(-1);
        const char* version_label = s.create_version.empty()
            ? (versions_loading ? "Loading versions…" : "No versions available")
            : s.create_version.c_str();
        if (ImGui::BeginCombo("##cs_ver", version_label)) {
            for (const std::string& version : supported_versions) {
                const bool selected = s.create_version == version;
                if (ImGui::Selectable(version.c_str(), selected)) {
                    s.create_version = version;
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        ImGui::Spacing();
        ImGui::PushFont(f_bold);
        ImGui::Text("Memory allocation: %d MB", s.create_ram);
        ImGui::PopFont();
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderInt("##cs_ram", &s.create_ram, 512, 16384, "%d MB");

        ImGui::Spacing();

        // Two-column layout for port and players
        ImGui::Columns(2, nullptr, false);
        ImGui::SetColumnWidth(0, ui_px(200.0f));
        ImGui::Text("Max Players");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputInt("##cs_mp", &s.create_max_players, 1, 10);
        s.create_max_players = std::clamp(s.create_max_players, 1, 200);

        ImGui::NextColumn();
        ImGui::Text("Port");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputInt("##cs_port", &s.create_port, 1, 100);
        s.create_port = std::clamp(s.create_port, 1, 65535);
        ImGui::Columns(1);
        card_end();
        if (st.fixture_mode) ImGui::EndDisabled();

        ImGui::Spacing();
        card_begin("##create_server_eula", ImVec2(-1, 0));
        ImGui::TextColored(k.muted, "MINECRAFT EULA");
        if (st.fixture_mode) ImGui::BeginDisabled();
        ImGui::Checkbox("I have read and agree to the Minecraft EULA",
                        &s.create_eula_accepted);
        ImGui::SameLine();
        if (st.fixture_mode) {
            ImGui::SmallButton("Read EULA");
        } else if (ImGui::SmallButton("Read EULA")) {
            ShellExecuteW(nullptr, L"open", L"https://aka.ms/MinecraftEULA",
                          nullptr, nullptr, SW_SHOWNORMAL);
        }
        if (st.fixture_mode) ImGui::EndDisabled();
        ImGui::TextColored(k.muted,
            "Amalgam writes eula=true only after this explicit confirmation.");
        card_end();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        std::string name_error;
        const bool safe_name = server::validate_server_name(s.create_name, &name_error);
        bool duplicate_name = false;
        for (const auto& existing : st.servers) {
            std::string left = existing.name;
            std::string right = s.create_name;
            std::transform(left.begin(), left.end(), left.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            std::transform(right.begin(), right.end(), right.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (!right.empty() && left == right) { duplicate_name = true; break; }
        }
        if (!safe_name)
            ImGui::TextColored(k.red, "%s", name_error.c_str());
        else if (duplicate_name)
            ImGui::TextColored(k.red, "A server with this name already exists.");
        else if (!s.create_eula_accepted)
            ImGui::TextColored(k.yellow, "EULA acceptance is required before creation.");
        else if (s.create_version.empty() && !versions_loading)
            ImGui::TextColored(k.red,
                               "No version is available for this software right now. Pick another "
                               "software, or try again when the provider responds.");
        if (!s.create_error.empty()) {
            ImGui::TextColored(k.red, "SERVER FILES WERE NOT SAVED");
            ImGui::TextWrapped("%s", s.create_error.c_str());
        }
        if (st.fixture_mode)
            ImGui::TextColored(k.muted,
                                "Visual fixture: server creation is disabled for this capture.");
        valid = !st.fixture_mode && safe_name && !duplicate_name && s.create_eula_accepted &&
            !s.create_version.empty();
        if (!s.create_error.empty()) {
            footer_status = "Server files were not saved. Review the detail below, then retry.";
            footer_status_color = k.red;
        } else if (!safe_name) {
            footer_status = name_error;
            footer_status_color = k.red;
        } else if (duplicate_name) {
            footer_status = "Choose a different server name.";
            footer_status_color = k.red;
        } else if (!s.create_eula_accepted) {
            footer_status = "Accept the Minecraft EULA to enable creation.";
            footer_status_color = k.yellow;
        } else if (s.create_version.empty() && !versions_loading) {
            footer_status = "Choose a compatible Minecraft version.";
            footer_status_color = k.red;
        } else if (st.fixture_mode) {
            footer_status = "Visual fixture: creation is disabled.";
            footer_status_color = k.brand_hov;
        }
        // The fixture harness scrolls page-level hosts by default. This modal
        // body owns a separate range, so apply its requested @middle/@bottom
        // position after layout without changing normal player interaction.
        if (st.fixture_mode && st.fixture_scroll_position > 0) {
            const float max_scroll = ImGui::GetScrollMaxY();
            if (max_scroll > 0.0f) {
                const float requested_scroll = st.fixture_scroll_position == 1
                    ? max_scroll * 0.5f : max_scroll;
                ImGui::SetScrollY(requested_scroll);
            }
        }
        }
        ImGui::EndChild();

        if (!footer_status.empty()) {
            ImGui::TextColored(footer_status_color, "%s", footer_status.c_str());
            if (!s.create_error.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, footer_status_color);
                ImGui::TextWrapped("%s", s.create_error.c_str());
                ImGui::PopStyleColor();
            }
        }
        ImGui::Separator();
        ImGui::Spacing();
        if (!valid) ImGui::BeginDisabled();
        if (primary_button("Create", ImVec2(ui_px(100.0f), ui_px(32.0f)))) {
            server::ServerConfig cfg;
            cfg.name = s.create_name;
            cfg.software = software_at(s.create_software_idx);
            cfg.minecraft_version = s.create_version;
            cfg.allocated_ram_mb = s.create_ram;
            cfg.max_players = s.create_max_players;
            cfg.port = s.create_port;

            const std::filesystem::path dir =
                std::filesystem::path(aml::net::get_local_app_data_path()) / L"amalgam" /
                L"servers" / aml::net::to_wide(s.create_name);
            cfg.server_directory = aml::net::to_utf8(dir.wstring());

            std::string config_error;
            const bool config_written = aml::server_provision::write_server_config_files(
                cfg, s.create_eula_accepted, &config_error);
            if (!config_written) {
                s.create_error = config_error.empty()
                    ? "Amalgam could not create this server's files."
                    : config_error;
                report_server_failure(s, st, "Create server", cfg.name, s.create_error);
            } else {
                cfg.stage = server::ServerStage::NotInstalled;
                st.servers.push_back(cfg);
                save_local_servers(st.servers);
                clear_server_failure(s);

                s.create_open = false;
                s.create_name.clear();
                s.create_ram = 4096;
                s.create_max_players = 20;
                s.create_port = 25565;
                s.create_eula_accepted = false;
                s.create_error.clear();
                ImGui::CloseCurrentPopup();

                // The download starts on a worker and the new server opens on its
                // own page, so the user watches the files arrive instead of a
                // frozen dialog. A manual runtime has nothing to fetch.
                const int index = static_cast<int>(st.servers.size()) - 1;
                s.detail_server_idx = index;
                s.detail_connect_idx = -1;
                s.detail_tab = 0;
                start_provision(st, index);
            }
        }
        if (!valid) ImGui::EndDisabled();

        ImGui::SameLine();
        if (ghost_button("Cancel", ImVec2(ui_px(90.0f), ui_px(32.0f)))) {
            s.create_open = false;
            s.create_name.clear();
            s.create_eula_accepted = false;
            s.create_error.clear();
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    if (!s.create_open) {
        s.create_name.clear();
        s.create_eula_accepted = false;
        s.create_error.clear();
    }
}

// ---------------------------------------------------------------------------
// Fixture helper: lets the snapshot runner open the cloud tab directly.
// ---------------------------------------------------------------------------

void reset_fixture_server_state() {
    auto& s = state();
    // In an in-process review run, do not carry a prior live server page's
    // state, console, property edit, form value, or action failure forward
    // into a named deterministic fixture.
    s.loaded = true;
    s.create_open = false;
    s.selected = -1;
    s.action_pending = -1;
    s.mode = 0;
    s.create_name.clear();
    s.create_version.clear();
    s.create_software_idx = 0;
    s.create_ram = 4096;
    s.create_max_players = 20;
    s.create_port = 25565;
    s.create_eula_accepted = false;
    s.create_error.clear();
    {
        std::lock_guard<std::mutex> lock(s.version_mu);
        s.version_software = -1;
        s.version_list.clear();
        s.version_loading = false;
        s.version_live = false;
        s.version_error.clear();
    }
    s.filter_status = 0;
    s.filter_software = 0;
    s.sort_mode = 0;
    s.console_open = false;
    s.console_server_idx = -1;
    s.console_filter.clear();
    s.console_log.clear();
    s.console_input.clear();
    s.console_auto_scroll = true;
    s.properties_open = false;
    s.properties_server_idx = -1;
    s.properties.clear();
    s.properties_dirty = false;
    s.detail_server_idx = -1;
    s.detail_connect_idx = -1;
    s.detail_tab = 0;
    s.file_browse_path.clear();
    s.file_entries.clear();
    s.file_is_dir.clear();
    s.file_list_dirty = true;
    s.file_preview_name.clear();
    s.file_preview_content.clear();
    s.file_preview_open = false;
    s.fixture_file_context_menu_open = false;
    s.fixture_file_context_menu_name.clear();
    s.file_delete_confirm = false;
    s.file_delete_target.clear();
    s.file_delete_error.clear();
    s.detail_players.clear();
    s.whitelist_entries.clear();
    s.ops_entries.clear();
    s.players_dirty = true;
    s.player_op_input.clear();
    s.player_whitelist_input.clear();
    s.world_size_bytes = 0;
    s.world_name.clear();
    s.world_dirty = true;
    s.restore_backup_confirm = false;
    s.restore_backup_id.clear();
    s.restore_backup_name.clear();
    s.restore_backup_error.clear();
    s.detail_console_filter.clear();
    s.detail_console_input.clear();
    s.detail_console_auto_scroll = true;
    s.detail_console_last_refresh = 0.0;
    s.remove_server_error.clear();
    clear_server_failure(s);
}

void set_fixture_server_mode(int mode) {
    state().mode = mode;
}

void set_fixture_server_detail(int server_index, int tab) {
    auto& s = state();
    s.mode = 0;
    s.detail_server_idx = std::max(0, server_index);
    s.detail_connect_idx = -1;
    s.detail_tab = std::clamp(tab, 0, 6);
    s.file_list_dirty = true;
    s.players_dirty = true;
    s.world_dirty = true;
}

// Deliberately open production confirmations with synthetic, non-actionable
// data.  The snapshot harness disables the underlying launcher actions, but
// these states still exercise the real modal composition and destructive-copy
// clarity that a reviewer needs to inspect.
void set_fixture_server_destructive_overlay(const std::string& fixture_case) {
    auto& s = state();
    if (fixture_case == "server-file-context-menu") {
        set_fixture_server_detail(0, 2);
        s.fixture_file_context_menu_name = "server.properties";
        s.fixture_file_context_menu_open = true;
    } else if (fixture_case == "server-create-validation" ||
               fixture_case == "server-create-error") {
        // Stage the same create dialog used in production, but never make its
        // action valid in fixture mode.  The dialog's fixture branch uses the
        // bundled catalog and prevents both provider requests and file writes.
        set_fixture_server_create_open(true);
        s.create_name = fixture_case == "server-create-validation"
            ? "Survival/Realm" : "Survival Realm";
        s.create_eula_accepted = true;
        s.create_error = fixture_case == "server-create-error"
            ? "Amalgam could not prepare the server files. The staged configuration was not saved; check the destination and retry."
            : "";
    } else if (fixture_case == "server-file-delete-confirm" ||
        fixture_case == "server-file-delete-error") {
        set_fixture_server_detail(0, 2);
        s.file_delete_target = "server.properties";
        s.file_delete_error = fixture_case == "server-file-delete-error"
            ? "The file could not be deleted. Check that it is not in use, then try again."
            : "";
        s.file_delete_confirm = true;
    } else if (fixture_case == "server-file-preview") {
        set_fixture_server_detail(0, 2);
        s.file_preview_name = "server.properties";
        s.file_preview_content = fixture_server_properties_preview_text();
        s.file_preview_open = true;
    } else if (fixture_case == "server-backup-restore-confirm" ||
               fixture_case == "server-backup-restore-error") {
        set_fixture_server_detail(0, 6);
        s.restore_backup_id = "fixture-pre-release";
        s.restore_backup_name = "Forsaken World SMP - pre-release checkpoint";
        s.restore_backup_error = fixture_case == "server-backup-restore-error"
            ? "The backup could not be restored. Current server files were not changed. You can retry safely."
            : "";
        s.restore_backup_confirm = true;
    } else if (fixture_case == "server-remove-confirm" ||
               fixture_case == "server-remove-error") {
        set_fixture_server_detail(0, 0);
        s.action_pending = 0;
        s.remove_server_error = fixture_case == "server-remove-error"
            ? "The launcher could not save this change. The server remains in Amalgam. You can retry safely."
            : "";
    }
}

void set_fixture_server_create_open(bool open) {
    auto& s = state();
    s.mode = 0;
    s.detail_server_idx = -1;
    s.detail_connect_idx = -1;
    s.create_open = open;
}

// The production overview combines server controls, import browsing, saved
// address mutation, and launch hand-off in one page.  Keep fixture captures
// on this separately-owned facade so no seeded route can accidentally invoke
// a local manager, browse a folder, save configuration, or begin a launch.
static void draw_fixture_server_overview(UiState& st) {
    const server::ServerConfig* fixture_server =
        st.servers.empty() ? nullptr : &st.servers.front();
    const std::string name = fixture_server ? fixture_server->name : "Fixture server unavailable";
    const server::ServerStage stage = fixture_server
        ? fixture_server->stage : server::ServerStage::NotInstalled;
    std::string metadata = "No representative server configuration is available.";
    std::string activity = "No representative server activity is available.";
    if (fixture_server) {
        metadata = std::string(server::server_software_name(fixture_server->software)) +
            "  •  Minecraft " + fixture_server->minecraft_version +
            "  •  Port " + std::to_string(fixture_server->port);
        if (st.server_metrics.valid) {
            char tps_text[32]{};
            std::snprintf(tps_text, sizeof(tps_text), "%.1f", st.server_metrics.tps);
            activity = std::to_string(st.server_metrics.players_online) + " / " +
                std::to_string(fixture_server->max_players) + " players  •  " +
                format_server_ram_mb(st.server_metrics.ram_mb) + " / " +
                format_server_ram_mb(fixture_server->allocated_ram_mb) + "  •  " +
                std::string(tps_text) + " TPS";
        } else {
            activity = std::string("Capacity ") +
                std::to_string(fixture_server->max_players) +
                " players  •  Runtime metrics unavailable";
        }
    }

    page_title("Servers", "Run local servers here — hosted servers are managed on the website.");
    draw_breadcrumbs({"Home", "Servers"});
    ImGui::PushStyleColor(ImGuiCol_Text, k.brand_hov);
    ImGui::TextWrapped(
        "Visual fixture — static representative server inventory. No local service, folder, saved address, launch, or configuration is accessed.");
    ImGui::PopStyleColor();
    ImGui::Spacing();

    ImGui::PushFont(f_h2);
    ImGui::TextColored(k.text, "LOCAL SERVERS");
    ImGui::PopFont();
    ImGui::TextColored(k.muted, "The controls below are intentionally inactive for this review capture.");
    ImGui::Spacing();

    ImGui::BeginDisabled();
    primary_button("+ Create Local Server", ImVec2(ui_px(170.0f), ui_px(32.0f)));
    ImGui::SameLine(0, ui_px(8.0f));
    ghost_button("Import Server", ImVec2(ui_px(120.0f), ui_px(32.0f)));
    ImGui::SameLine(0, ui_px(12.0f));
    ghost_button("All statuses", ImVec2(ui_px(112.0f), ui_px(32.0f)));
    ImGui::SameLine(0, ui_px(4.0f));
    ghost_button("All software", ImVec2(ui_px(122.0f), ui_px(32.0f)));
    ImGui::EndDisabled();

    ImGui::Spacing();
    card_begin("##fixture_server_overview_card", ImVec2(-1, 0));
    const ImVec2 card_origin = ImGui::GetCursorScreenPos();
    ImDrawList* card_draw = ImGui::GetWindowDrawList();
    card_draw->AddRectFilled(card_origin,
                             card_origin + ImVec2(ui_px(5.0f), ui_px(100.0f)),
                             c32(stage_color(stage)), ui_px(2.0f));
    card_draw->AddCircleFilled(card_origin + ImVec2(ui_px(34.0f), ui_px(35.0f)),
                               ui_px(22.0f), c32(k.brand_dk));
    draw_icon(IconId::Server, card_origin + ImVec2(ui_px(34.0f), ui_px(35.0f)),
              ui_px(12.0f), c32(k.brand_hov));
    ImGui::SetCursorScreenPos(card_origin + ImVec2(ui_px(70.0f), ui_px(12.0f)));
    ImGui::PushFont(f_bold);
    ImGui::TextUnformatted(name.c_str());
    ImGui::PopFont();
    ImGui::SameLine(0, ui_px(8.0f));
    draw_status_badge(stage);
    ImGui::TextColored(k.muted, "%s", metadata.c_str());
    ImGui::TextColored(k.muted, "%s", activity.c_str());
    ImGui::Spacing();
    ImGui::BeginDisabled();
    ghost_button("Stop", ImVec2(ui_px(80.0f), ui_px(28.0f)));
    ImGui::SameLine(0, ui_px(4.0f));
    ghost_button("Restart", ImVec2(ui_px(92.0f), ui_px(28.0f)));
    ImGui::SameLine(0, ui_px(4.0f));
    ghost_button("Manage", ImVec2(ui_px(92.0f), ui_px(28.0f)));
    ImGui::SameLine(0, ui_px(4.0f));
    ghost_button("Folder", ImVec2(ui_px(80.0f), ui_px(28.0f)));
    ImGui::SameLine(0, ui_px(4.0f));
    danger_button("Remove", ImVec2(ui_px(84.0f), ui_px(28.0f)));
    ImGui::EndDisabled();
    card_end();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::PushFont(f_h2);
    ImGui::TextColored(k.text, "SAVED SERVERS");
    ImGui::PopFont();
    ImGui::TextColored(k.muted, "Representative quick-connect entry; it is neither read from nor written to your launcher settings.");
    ImGui::Spacing();
    card_begin("##fixture_saved_server", ImVec2(-1, 0));
    ImGui::PushFont(f_bold);
    ImGui::TextColored(k.text, "Amalgam Network");
    ImGui::PopFont();
    ImGui::TextColored(k.muted, "play.amalgam-network.com  •  Java");
    ImGui::SameLine(0, ui_px(18.0f));
    ImGui::BeginDisabled();
    ghost_button("Connect", ImVec2(ui_px(76.0f), ui_px(26.0f)));
    ImGui::SameLine(0, ui_px(4.0f));
    ghost_button("Copy", ImVec2(ui_px(60.0f), ui_px(26.0f)));
    ImGui::SameLine(0, ui_px(4.0f));
    ghost_button("Remove", ImVec2(ui_px(70.0f), ui_px(26.0f)));
    ImGui::EndDisabled();
    card_end();
}

// ---------------------------------------------------------------------------
// Main entry point
// ---------------------------------------------------------------------------

// Render the shared overview shell before touching any local-server state. In
// particular, this lets the Cloud handoff route return before a local server
// download, reconcile, or persistence check is even considered.
static bool draw_server_overview_shell(UiState& st, ServerUIState& s) {
    draw_page_emblem(st, "server-emblem-ai.png");
    page_title("Servers", "Run local servers here — hosted servers are managed on the website.");
    draw_breadcrumbs({"Home", "Servers"});

    // Branded scene keeps this operational page visually connected to the
    // launcher while the controls remain real and readable on top.
    {
        const float art_h = ui_px(116.0f);
        const ImVec2 art_pos = ImGui::GetCursorScreenPos();
        const ImVec2 art_size(ImGui::GetContentRegionAvail().x, art_h);
        draw_local_image(st, st.exe_dir + L"\\branding\\ai\\launcher-servers-ai.png",
                         art_pos, art_size, c32(k.brand_dk));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(art_pos, art_pos + art_size,
                          c32(ImVec4(0.02f, 0.01f, 0.06f, 0.32f)), ui_px(12.0f));
        dl->AddRect(art_pos, art_pos + art_size,
                    c32(ImVec4(k.brand.x, k.brand.y, k.brand.z, 0.35f)), ui_px(12.0f),
                    0, ui_px(1.0f));
        ImGui::SetCursorScreenPos(art_pos + ImVec2(ui_px(18.0f), ui_px(16.0f)));
        ImGui::PushFont(f_title);
        ImGui::TextColored(k.text, "Build your world");
        ImGui::PopFont();
        ImGui::TextColored(k.muted, "Host locally, connect external servers, and keep every world organized.");
        ImGui::SetCursorScreenPos(ImVec2(art_pos.x, art_pos.y + art_h + ui_px(12.0f)));
    }

    // ── Mode switch: LOCAL / AMALGAM CLOUD (premium pill tabs) ────
    const char* modes[] = {"LOCAL", "AMALGAM CLOUD"};
    const float tab_gap = ui_px(6.0f);
    const float tab_h = ui_px(34.0f);
    for (int i = 0; i < 2; ++i) {
        if (i) ImGui::SameLine(0, tab_gap);
        const bool active = s.mode == i;
        const char* label = modes[i];
        const ImVec2 label_sz = ImGui::CalcTextSize(label);
        const float pad_x = ui_px(16.0f);
        const ImVec2 tab_size(label_sz.x + pad_x * 2.0f, tab_h);
        const ImVec2 tab_min = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(tab_min, tab_min + tab_size,
                          c32(active ? k.surface2 : ImVec4(0, 0, 0, 0)),
                          ui_px(8.0f));
        if (active)
            dl->AddRect(tab_min, tab_min + tab_size, c32(k.brand), ui_px(8.0f),
                        0, ui_px(1.5f));
        ImGui::InvisibleButton((std::string("##mode_") + std::to_string(i)).c_str(),
                               tab_size);
        if (ImGui::IsItemHovered() && !active)
            dl->AddRect(tab_min, tab_min + tab_size, c32(k.border), ui_px(8.0f),
                        0, ui_px(1.0f));
        if (ImGui::IsItemClicked()) s.mode = i;
        ImGui::PushFont(active ? f_bold : f_body);
        dl->AddText(tab_min + ImVec2(pad_x, (tab_h - ImGui::GetTextLineHeight()) * 0.5f),
                    c32(active ? k.text : k.muted), label);
        ImGui::PopFont();
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    ImGui::Separator();
    ImGui::Spacing();
    return s.mode == 1;
}

void draw_server_manager(UiState& st) {
    auto& s = state();

    // Consume completed process-control work before route selection. A user
    // can switch to the Cloud handoff or a detail view while a local command
    // runs; its result still belongs to the launcher-owned local list and must
    // not remain reserved until they navigate back.
    if (!st.fixture_mode) {
        consume_local_server_action_result(st);
        consume_local_server_command_result(st);
    }

    // A Cloud fixture has its own inert renderer. Route it before the local
    // overview facade so the review captures the official-site handoff rather
    // than a representative local-server inventory.
    if (st.fixture_mode && s.detail_server_idx < 0 && s.mode == 1) {
        draw_cloud_page(st);
        return;
    }

    // The non-detail local-server fixture needs its own renderer before any of
    // the live overview's folder-picker, manager, persistence, or launch
    // paths. Create-dialog fixtures still receive the genuine (but inert)
    // modal overlay below, so their validation/recovery composition remains
    // covered.
    if (st.fixture_mode && s.detail_server_idx < 0) {
        draw_fixture_server_overview(st);
        draw_create_dialog(st);
        return;
    }

    // The common shell contains the switcher required to return from Cloud to
    // Local. When Cloud is selected it exits before local-server initialization
    // or reconciliation, keeping the website handoff side-effect free.
    if (s.detail_server_idx < 0 && draw_server_overview_shell(st, s)) {
        draw_cloud_page(st);
        return;
    }

    if (!st.fixture_mode) {
        apply_finished_download(st);
        // This page is the only consumer of the server service layer. Keep the
        // supervisor pointed at the Java directory the rest of the launcher uses —
        // the settings page can change it while the launcher runs.
        aml::services::local_server_manager()->set_local_java_root(
            st.cfg ? st.cfg->java_cache_dir : std::wstring());
    }
    if (!s.loaded) {
        // Fixtures never fall back to the reviewer's saved server list.  A
        // missing seed is rendered as the facade's representative empty data,
        // rather than becoming permission to read local configuration.
        if (!st.fixture_mode) {
            load_local_servers(st.servers);
            reconcile_local_server_stages(st.servers);
        }
        s.loaded = true;
    }

    // ── Detail view (local server) ──────────────────────────────────
    if (s.detail_server_idx >= 0) {
        page_title("Servers", nullptr);
        draw_breadcrumbs({"Home", "Servers", "Local"});
        draw_server_action_failure(st);
        draw_server_detail(st);
        draw_remove_server_dialog(st);
        // Overlays
        draw_create_dialog(st);
        draw_console_panel(st);
        draw_properties_panel(st);
        return;
    }

    // The overview shell has already been rendered above. Local-only error
    // feedback belongs below its switcher so a Cloud handoff never inherits a
    // stale local-server operation message.
    draw_server_action_failure(st);

    // ══════════════════════════════════════════════════════════════
    // LOCAL SERVERS
    // ══════════════════════════════════════════════════════════════

    // Header
    ImGui::PushFont(f_h2);
    ImGui::TextColored(k.text, "LOCAL SERVERS");
    ImGui::PopFont();
    ImGui::TextColored(k.muted, "Run and manage Minecraft servers directly from this PC.");
    ImGui::Spacing();

    // Toolbar
    if (primary_button("+ Create Local Server", ImVec2(ui_px(170.0f), ui_px(32.0f)))) {
        s.create_open = true;
    }
    ImGui::SameLine(0, ui_px(8.0f));
    if (ghost_button("Import Server", ImVec2(ui_px(120.0f), ui_px(32.0f)))) {
        wchar_t path_buf[MAX_PATH] = {0};
        BROWSEINFOW bi{};
        bi.hwndOwner = st.hwnd;
        bi.lpszTitle = L"Select an existing Minecraft server folder to import.";
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
        if (pidl) {
            if (SHGetPathFromIDListW(pidl, path_buf)) {
                std::wstring folder(path_buf);
                std::string dir_utf8 = aml::net::to_utf8(folder);
                // Read server.properties to detect version, port, software.
                server::ServerConfig cfg;
                cfg.server_directory = dir_utf8;
                cfg.stage = server::ServerStage::Ready;
                cfg.allocated_ram_mb = 4096;
                cfg.max_players = 20;
                cfg.port = 25565;
                cfg.minecraft_version = "1.20.1";
                cfg.software = server::ServerSoftware::Vanilla;
                // Parse server.properties if present.
                std::wstring props_path = folder + L"\\server.properties";
                std::ifstream props(props_path);
                if (props.is_open()) {
                    std::string line;
                    while (std::getline(props, line)) {
                        auto eq = line.find('=');
                        if (eq == std::string::npos) continue;
                        std::string key = line.substr(0, eq);
                        std::string val = line.substr(eq + 1);
                        // Trim whitespace.
                        auto trim = [](std::string& s) {
                            while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
                            size_t start = s.find_first_not_of(" \t");
                            if (start != std::string::npos) s = s.substr(start);
                        };
                        trim(key); trim(val);
                        if (key == "server-port") cfg.port = std::clamp(std::stoi(val.empty() ? "25565" : val), 1, 65535);
                        else if (key == "max-players") cfg.max_players = std::clamp(std::stoi(val.empty() ? "20" : val), 1, 200);
                        else if (key == "server-name" || key == "motd") {
                            if (!val.empty() && cfg.name.empty()) cfg.name = val;
                        }
                    }
                }
                // Detect software from jar files in the directory.
                for (const auto& entry : std::filesystem::directory_iterator(folder)) {
                    if (!entry.is_regular_file()) continue;
                    const auto& p = entry.path();
                    const auto ext = p.extension().wstring();
                    const auto stem = p.stem().wstring();
                    auto to_lower = [](std::wstring s) {
                        for (auto& c : s) c = (wchar_t)towlower((wint_t)c);
                        return s;
                    };
                    std::wstring le = to_lower(stem);
                    std::wstring le_ext = to_lower(ext);
                    if (le_ext != L".jar") continue;
                    if (le.find(L"paper") != std::wstring::npos) cfg.software = server::ServerSoftware::Paper;
                    else if (le.find(L"purpur") != std::wstring::npos) cfg.software = server::ServerSoftware::Paper;
                    else if (le.find(L"forge") != std::wstring::npos || le.find(L"neoforge") != std::wstring::npos) cfg.software = server::ServerSoftware::Vanilla;
                    else if (le.find(L"fabric") != std::wstring::npos) cfg.software = server::ServerSoftware::Vanilla;
                    else if (le.find(L"velocity") != std::wstring::npos) cfg.software = server::ServerSoftware::Vanilla;
                    else if (le.find(L"waterfall") != std::wstring::npos) cfg.software = server::ServerSoftware::Vanilla;
                }
                // Use folder name if no name was found.
                if (cfg.name.empty()) {
                    auto folder_name = std::filesystem::path(folder).filename().wstring();
                    cfg.name = aml::net::to_utf8(folder_name);
                }
                // Check for duplicate.
                bool duplicate = false;
                for (const auto& existing : st.servers) {
                    if (existing.server_directory == dir_utf8) { duplicate = true; break; }
                }
                if (!duplicate) {
                    st.servers.push_back(cfg);
                    save_local_servers(st.servers);
                }
            }
            CoTaskMemFree(pidl);
        }
    }

    // Keep the filters on a dedicated row once the content column becomes
    // narrow. This prevents the create/import actions and combo boxes from
    // competing for the same horizontal space on smaller windows.
    if (ImGui::GetContentRegionAvail().x < ui_px(760.0f)) {
        ImGui::NewLine();
        ImGui::Spacing();
    } else {
        ImGui::SameLine(0, ui_px(12.0f));
    }

    // Filter bar
    {
        const char* status_name = status_filter_name(s.filter_status);
        ImGui::SetNextItemWidth(ui_px(110.0f));
        if (ImGui::BeginCombo("##srv_status_f", status_name)) {
            for (int i = 0; i < 5; ++i) {
                if (ImGui::Selectable(status_filter_name(i), s.filter_status == i))
                    s.filter_status = i;
            }
            ImGui::EndCombo();
        }
    }
    ImGui::SameLine(0, ui_px(4.0f));
    {
        const char* sw_name = software_filter_name(s.filter_software);
        ImGui::SetNextItemWidth(ui_px(110.0f));
        if (ImGui::BeginCombo("##srv_sw_f", sw_name)) {
            for (int i = 0; i <= static_cast<int>(server::software_catalog().size()); ++i) {
                if (ImGui::Selectable(software_filter_name(i), s.filter_software == i))
                    s.filter_software = i;
            }
            ImGui::EndCombo();
        }
    }
    ImGui::SameLine(0, ui_px(4.0f));
    {
        ImGui::SetNextItemWidth(ui_px(130.0f));
        if (ImGui::BeginCombo("##srv_sort", ("Sort: " + std::string(sort_label(s.sort_mode))).c_str())) {
            for (int i = 0; i < 4; ++i) {
                if (ImGui::Selectable(sort_label(i), s.sort_mode == i))
                    s.sort_mode = i;
            }
            ImGui::EndCombo();
        }
    }

    ImGui::SameLine(0, ui_px(12.0f));
    int shown = 0;
    for (const auto& sv : st.servers) {
        if (matches_filters(st, sv, s.filter_status, s.filter_software)) ++shown;
    }
    ImGui::TextColored(k.muted, "%d / %d servers", shown, (int)st.servers.size());

    ImGui::Spacing();

    // ── Server list ────────────────────────────────────────────────
    if (st.servers.empty()) {
        ImGui::Spacing();
        card_begin("##srv_empty", ImVec2(-1, ui_px(200.0f)));
        illustrated_empty_state(IconId::Server, "NO SERVERS YET",
                                "Create a local server to host Minecraft on this PC, or deploy a 24/7 server on the website.");
        ImGui::SetCursorPosX(
            ImGui::GetCursorPosX() +
            (ImGui::GetContentRegionAvail().x - ui_px(360.0f)) * 0.5f);
        if (primary_button("Create Your First Server",
                           ImVec2(ui_px(200.0f), ui_px(36.0f)))) {
            s.create_open = true;
        }
        ImGui::SameLine(0, ui_px(8.0f));
        if (ghost_button("Cloud Hosting",
                         ImVec2(ui_px(130.0f), ui_px(36.0f)))) {
            s.mode = 1;
        }
        card_end();
    } else {
        // Collect filtered indices for sorting
        std::vector<int> indices;
        for (int i = 0; i < static_cast<int>(st.servers.size()); ++i) {
            if (matches_filters(st, st.servers[i], s.filter_status, s.filter_software))
                indices.push_back(i);
        }

        // Sort
        std::sort(indices.begin(), indices.end(), [&](int a, int b) {
            const auto& sa = st.servers[a];
            const auto& sb = st.servers[b];
            switch (s.sort_mode) {
                case 1: return static_cast<int>(effective_stage(st, sa)) <
                               static_cast<int>(effective_stage(st, sb));
                case 2: return sa.minecraft_version < sb.minecraft_version;
                case 3: return sa.port < sb.port;
                default: return sa.name < sb.name;
            }
        });

        for (int idx : indices) {
            draw_server_card(st.servers[idx], idx, st);
            ImGui::Spacing();
        }

        if (shown == 0) {
            card_begin("##srv_no_match", ImVec2(-1, ui_px(80.0f)));
            empty_state("No matching servers", "Adjust your filters to see more servers.");
            card_end();
        }
    }

    draw_remove_server_dialog(st);

    // ── Saved external servers (moved from old Connect tab) ────────
    if (!st.cfg->servers.empty()) {
        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::PushFont(f_h2);
        ImGui::TextColored(k.text, "SAVED SERVERS");
        ImGui::PopFont();
        ImGui::TextColored(k.muted, "External Minecraft server addresses for quick-connect.");
        ImGui::Spacing();

        for (size_t i = 0; i < st.cfg->servers.size(); ++i) {
            const auto& server = st.cfg->servers[i];
            ImGui::PushID(static_cast<int>(i + 9000));
            card_begin("##saved_srv", ImVec2(-1, 0));

            static const char* kTypeLabels[] = {"Java", "Bedrock", "Modded", "Bedrock Modded"};
            ImVec4 kTypeBadgeColors[] = {k.blue, k.green, k.brand, ImVec4(0.4f, 0.6f, 0.8f, 1.0f)};
            int type_idx = (server.type >= 0 && server.type < 4) ? server.type : 0;

            // Server icon tile
            const ImVec2 icon_pos = ImGui::GetCursorScreenPos();
            const float icon_r = ui_px(14.0f);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(icon_pos, icon_pos + ImVec2(icon_r * 2.0f, icon_r * 2.0f),
                              c32(k.surface), ui_px(7.0f));
            dl->AddRect(icon_pos, icon_pos + ImVec2(icon_r * 2.0f, icon_r * 2.0f),
                         c32(kTypeBadgeColors[type_idx]), ui_px(7.0f), 0, ui_px(1.0f));
            draw_icon(IconId::Server, icon_pos + ImVec2(icon_r, icon_r), ui_px(8.0f),
                      c32(kTypeBadgeColors[type_idx]));
            ImGui::Dummy(ImVec2(icon_r * 2.0f, icon_r * 2.0f));
            ImGui::SameLine(0, ui_px(10.0f));

            // Name + address
            ImGui::BeginGroup();
            ImGui::PushFont(f_bold);
            ImGui::TextColored(k.text, "%s", server.name.c_str());
            ImGui::PopFont();
            ImGui::TextColored(k.muted, "%s", net::to_utf8(server.address).c_str());
            ImGui::EndGroup();

            // Type badge + profile chip
            ImGui::SameLine(0, ui_px(10.0f));
            {
                const ImVec2 bp = ImGui::GetCursorScreenPos();
                const ImVec2 badge_sz = ImGui::CalcTextSize(kTypeLabels[type_idx]) +
                                        ImVec2(ui_px(12.0f), ui_px(6.0f));
                ImVec4 bg = kTypeBadgeColors[type_idx];
                bg.w = 0.15f;
                dl->AddRectFilled(bp, bp + badge_sz, c32(bg), ui_px(5.0f));
                dl->AddText(bp + ImVec2(ui_px(6.0f), ui_px(3.0f)),
                            c32(kTypeBadgeColors[type_idx]), kTypeLabels[type_idx]);
                ImGui::Dummy(badge_sz + ImVec2(0, ui_px(6.0f)));
            }
            if (!server.profile.empty()) {
                ImGui::SameLine(0, ui_px(6.0f));
                const ImVec2 bp = ImGui::GetCursorScreenPos();
                const ImVec2 badge_sz = ImGui::CalcTextSize("Profile") +
                                        ImVec2(ui_px(12.0f), ui_px(6.0f));
                dl->AddRectFilled(bp, bp + badge_sz, c32(ImVec4(k.brand.x, k.brand.y, k.brand.z, 0.15f)),
                                  ui_px(5.0f));
                dl->AddText(bp + ImVec2(ui_px(6.0f), ui_px(3.0f)), c32(k.brand), "Profile");
                ImGui::Dummy(badge_sz + ImVec2(0, ui_px(6.0f)));
            }

            // Actions
            ImGui::SameLine(ImGui::GetCursorPosX() +
                            std::max(0.0f, ImGui::GetContentRegionAvail().x - ui_px(230.0f)));
            if (ghost_button("Connect", ImVec2(ui_px(70.0f), ui_px(26.0f)))) {
                if (!server.profile.empty()) {
                    st.selected = server.profile;
                    st.active_instance_dir.clear();
                    for (auto& inst : st.instance_list) {
                        if (inst.id == server.profile) {
                            st.active_instance_dir = inst.directory;
                            break;
                        }
                    }
                }
                if (!st.active_instance_dir.empty() && !st.selected.empty()) {
                    st.pending_instance_dir = st.active_instance_dir;
                    st.pending_launch = true;
                    st.pending_id = st.selected;
                    st.pending_server = net::to_utf8(server.address);
                }
            }
            ImGui::SameLine(0, ui_px(4.0f));
            if (ghost_button("Copy", ImVec2(ui_px(60.0f), ui_px(24.0f)))) {
                ImGui::SetClipboardText(net::to_utf8(server.address).c_str());
            }
            ImGui::SameLine(0, ui_px(4.0f));
            if (ghost_button("Remove", ImVec2(ui_px(70.0f), ui_px(24.0f)))) {
                st.cfg->servers.erase(st.cfg->servers.begin() + i);
                config::save(st.exe_dir + L"\\launcher.json", *st.cfg);
                ImGui::PopID();
                break;  // index invalidated
            }

            card_end();
            ImGui::PopID();
        }
    }

    // ── Overlays ───────────────────────────────────────────────────
    draw_create_dialog(st);
    draw_console_panel(st);
    draw_properties_panel(st);
}

}  // namespace aml::ui
