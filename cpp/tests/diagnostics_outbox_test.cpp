#include "core/diagnostics_outbox.h"
#include "core/game_mode_telemetry.h"

#include <windows.h>

#include <fstream>
#include <iostream>
#include <string>

namespace {

bool test_outbox() {
    auto& outbox = aml::diagnostics::outbox();
    const std::wstring db = L"amalgam_diagnostics_test.sqlite3";
    const std::wstring csv = L"amalgam_diagnostics_test.csv";
    DeleteFileW(db.c_str());
    DeleteFileW(csv.c_str());
    outbox.set_enabled(false);
    outbox.set_path(db);
    if (outbox.pending() != 0) return false;

    if (!outbox.set_enabled(true)) {
        // Windows 10's optional SQLite component is allowed to be absent; fail closed.
        outbox.set_enabled(false);
        return true;
    }
    aml::telemetry::State state;
    state.active = true;
    state.match_id = 7;
    state.mode = aml::telemetry::Mode::Practice;
    state.confidence = 90;
    state.match_elapsed_ms = 12000;
    state.kills = 3;
    state.final_kills = 1;
    state.explosions = 4;
    if (!outbox.enqueue(state) || outbox.pending() != 1) return false;

    outbox.set_enabled(false);
    if (!outbox.set_enabled(true) || outbox.pending() != 1) return false;
    if (!outbox.export_csv(csv)) return false;
    std::ifstream file(csv);
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();
    if (content.find("match_id") == std::string::npos || content.find("Practice") == std::string::npos)
        return false;
    if (!outbox.delete_all() || outbox.pending() != 0) return false;
    outbox.set_enabled(false);
    DeleteFileW(db.c_str());
    DeleteFileW(csv.c_str());
    return true;
}

}  // namespace

int main() {
    if (!test_outbox()) {
        std::cerr << "FAILED: outbox\n";
        return 1;
    }
    return 0;
}
