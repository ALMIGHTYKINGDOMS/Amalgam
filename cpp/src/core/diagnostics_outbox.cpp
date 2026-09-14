#include "core/diagnostics_outbox.h"

#include "core/game_mode_telemetry.h"

#include <windows.h>

#include <chrono>
#include <fstream>
#include <mutex>

#if defined(_WIN32) && __has_include(<winsqlite/winsqlite3.h>)
#include <winsqlite/winsqlite3.h>
#define AML_HAS_WINSQLITE 1
#else
#define AML_HAS_WINSQLITE 0
#endif

namespace aml::diagnostics {

namespace {

constexpr int kMaxRows = 256;

std::wstring default_path() {
    wchar_t buffer[MAX_PATH]{};
    DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return L"";
    std::wstring root(buffer, length);
    CreateDirectoryW((root + L"\\Amalgam").c_str(), nullptr);
    return root + L"\\Amalgam\\diagnostics.sqlite3";
}

int64_t wall_clock_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

}  // namespace

Outbox::Outbox() = default;

Outbox::~Outbox() {
    std::lock_guard<std::mutex> lock(mutex_);
    close_locked();
}

bool Outbox::set_enabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled) {
        enabled_ = false;
        close_locked();
        return true;
    }
    if (path_.empty()) path_ = default_path();
    enabled_ = open_locked();
    return enabled_;
}

bool Outbox::enabled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return enabled_ && db_ != nullptr;
}

void Outbox::set_path(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    close_locked();
    path_ = path;
    if (enabled_) enabled_ = open_locked();
}

bool Outbox::open_locked() {
#if AML_HAS_WINSQLITE
    if (path_.empty()) return false;
    std::wstring parent = path_.substr(0, path_.find_last_of(L"\\/"));
    if (!parent.empty()) {
        size_t slash = parent.find_last_of(L"\\/");
        if (slash != std::wstring::npos) CreateDirectoryW(parent.substr(0, slash).c_str(), nullptr);
        CreateDirectoryW(parent.c_str(), nullptr);
    }
    sqlite3* db = nullptr;
    if (sqlite3_open16(path_.c_str(), &db) != SQLITE_OK || !db) {
        if (db) sqlite3_close(db);
        return false;
    }
    db_ = db;
    sqlite3_busy_timeout(db, 1000);
    const char* schema =
        "CREATE TABLE IF NOT EXISTS diagnostics ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, created_ms INTEGER NOT NULL,"
        "match_id INTEGER NOT NULL, mode TEXT NOT NULL, confidence INTEGER NOT NULL,"
        "elapsed_ms INTEGER NOT NULL, players_alive INTEGER, teams_alive INTEGER,"
        "kills INTEGER, final_kills INTEGER, beds_alive INTEGER, episode INTEGER,"
        "timer_seconds INTEGER, refill_seconds INTEGER, generator_seconds INTEGER,"
        "chest_seconds INTEGER, border_seconds INTEGER, teleport_seconds INTEGER,"
        "crystals INTEGER, explosions INTEGER);";
    if (!exec_locked(schema)) {
        close_locked();
        return false;
    }
    return true;
#else
    return false;
#endif
}

void Outbox::close_locked() {
#if AML_HAS_WINSQLITE
    if (db_) sqlite3_close(static_cast<sqlite3*>(db_));
#endif
    db_ = nullptr;
}

bool Outbox::exec_locked(const char* sql) const {
#if AML_HAS_WINSQLITE
    if (!db_) return false;
    char* error = nullptr;
    int rc = sqlite3_exec(static_cast<sqlite3*>(db_), sql, nullptr, nullptr, &error);
    if (error) sqlite3_free(error);
    return rc == SQLITE_OK;
#else
    (void)sql;
    return false;
#endif
}

bool Outbox::enqueue(const telemetry::State& state) {
    std::lock_guard<std::mutex> lock(mutex_);
#if AML_HAS_WINSQLITE
    if (!enabled_ || !db_) return false;
    const char* sql =
        "INSERT INTO diagnostics (created_ms,match_id,mode,confidence,elapsed_ms,players_alive,"
        "teams_alive,kills,final_kills,beds_alive,episode,timer_seconds,refill_seconds,"
        "generator_seconds,chest_seconds,border_seconds,teleport_seconds,crystals,explosions)"
        " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(static_cast<sqlite3*>(db_), sql, -1, &statement, nullptr) != SQLITE_OK)
        return false;
    int index = 1;
    sqlite3_bind_int64(statement, index++, wall_clock_ms());
    sqlite3_bind_int64(statement, index++, static_cast<sqlite3_int64>(state.match_id));
    sqlite3_bind_text(statement, index++, telemetry::mode_name(state.mode), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, index++, state.confidence);
    sqlite3_bind_int64(statement, index++, static_cast<sqlite3_int64>(state.match_elapsed_ms));
    sqlite3_bind_int(statement, index++, state.players_alive);
    sqlite3_bind_int(statement, index++, state.teams_alive);
    sqlite3_bind_int(statement, index++, state.kills);
    sqlite3_bind_int(statement, index++, state.final_kills);
    sqlite3_bind_int(statement, index++, state.beds_alive);
    sqlite3_bind_int(statement, index++, state.episode);
    sqlite3_bind_int(statement, index++, state.timer_seconds);
    sqlite3_bind_int(statement, index++, state.refill_seconds);
    sqlite3_bind_int(statement, index++, state.generator_seconds);
    sqlite3_bind_int(statement, index++, state.chest_seconds);
    sqlite3_bind_int(statement, index++, state.border_seconds);
    sqlite3_bind_int(statement, index++, state.teleport_seconds);
    sqlite3_bind_int(statement, index++, state.crystals);
    sqlite3_bind_int(statement, index++, state.explosions);
    bool ok = sqlite3_step(statement) == SQLITE_DONE;
    sqlite3_finalize(statement);
    if (!ok) return false;
    return exec_locked("DELETE FROM diagnostics WHERE id NOT IN "
                       "(SELECT id FROM diagnostics ORDER BY id DESC LIMIT 256);");
#else
    (void)state;
    return false;
#endif
}

size_t Outbox::pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
#if AML_HAS_WINSQLITE
    if (!enabled_ || !db_) return 0;
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(static_cast<sqlite3*>(db_), "SELECT COUNT(*) FROM diagnostics;", -1,
                           &statement, nullptr) != SQLITE_OK)
        return 0;
    size_t result = sqlite3_step(statement) == SQLITE_ROW
        ? static_cast<size_t>(sqlite3_column_int64(statement, 0)) : 0;
    sqlite3_finalize(statement);
    return result;
#else
    return 0;
#endif
}

bool Outbox::export_csv(const std::wstring& path) const {
    std::lock_guard<std::mutex> lock(mutex_);
#if AML_HAS_WINSQLITE
    if (!enabled_ || !db_ || path.empty()) return false;
    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open()) return false;
    file << "created_ms,match_id,mode,confidence,elapsed_ms,players_alive,teams_alive,kills,"
            "final_kills,beds_alive,episode,timer_seconds,refill_seconds,generator_seconds,"
            "chest_seconds,border_seconds,teleport_seconds,crystals,explosions\n";
    const char* sql = "SELECT created_ms,match_id,mode,confidence,elapsed_ms,players_alive,"
                      "teams_alive,kills,final_kills,beds_alive,episode,timer_seconds,"
                      "refill_seconds,generator_seconds,chest_seconds,border_seconds,"
                      "teleport_seconds,crystals,explosions FROM diagnostics ORDER BY id;";
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(static_cast<sqlite3*>(db_), sql, -1, &statement, nullptr) != SQLITE_OK)
        return false;
    while (sqlite3_step(statement) == SQLITE_ROW) {
        for (int i = 0; i < 19; ++i) {
            if (i) file << ',';
            if (i == 2) {
                const unsigned char* text = sqlite3_column_text(statement, i);
                file << (text ? reinterpret_cast<const char*>(text) : "");
            } else {
                file << sqlite3_column_int64(statement, i);
            }
        }
        file << '\n';
    }
    sqlite3_finalize(statement);
    return file.good();
#else
    (void)path;
    return false;
#endif
}

bool Outbox::delete_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    return exec_locked("DELETE FROM diagnostics;");
}

Outbox& outbox() {
    static Outbox instance;
    return instance;
}

}  // namespace aml::diagnostics
