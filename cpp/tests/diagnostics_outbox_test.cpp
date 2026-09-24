#include "core/diagnostics_outbox.h"
#include "core/game_mode_telemetry.h"
#include "diagnostics.h"

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

bool test_local_diagnostics_state() {
    bool success = true;
    const auto expect = [&](bool condition, const char* message) {
        if (condition) return;
        std::cerr << "FAILED: diagnostics local state — " << message << "\n";
        success = false;
    };

    aml::config::Config config;
    auto backend = aml::diagnostics::local_state::backend(config, false);
    expect(backend.status == aml::diagnostics::CheckStatus::WARN,
           "missing backend configuration must be a local setup warning");
    expect(backend.check == "Local configuration",
           "backend result must not claim network connectivity");
    expect(backend.detail.find("Modrinth") == std::string::npos,
           "backend result must not use a provider as backend evidence");

    config.supabase_url = "http://project.supabase.co";
    config.supabase_anon_key = "public-client-key";
    const bool http_backend_is_eligible =
        aml::diagnostics::local_state::has_complete_backend_config(config) &&
        aml::diagnostics::local_state::is_https_endpoint(config.supabase_url);
    expect(!http_backend_is_eligible,
           "a non-HTTPS endpoint must not make the in-process backend eligible");
    backend = aml::diagnostics::local_state::backend(config, false);
    expect(backend.status == aml::diagnostics::CheckStatus::FAIL,
           "a non-HTTPS backend endpoint must fail closed");

    config.supabase_url = "https://project.supabase.co";
    backend = aml::diagnostics::local_state::backend(config, false);
    expect(backend.status == aml::diagnostics::CheckStatus::WARN,
           "an uninitialized backend client must not claim remote availability");
    expect(backend.detail.find("not contacted") != std::string::npos,
           "local backend result must say no remote check occurred");

    backend = aml::diagnostics::local_state::backend(config, true);
    expect(backend.status == aml::diagnostics::CheckStatus::PASS,
           "an initialized local backend client must pass the local check");

    auto essentials = aml::diagnostics::local_state::essentials(config, true, false);
    expect(essentials.status == aml::diagnostics::CheckStatus::WARN,
           "Essentials must not pass without an Amalgam session");
    expect(essentials.detail.find("Sign in") != std::string::npos,
           "the unsigned Essentials state must offer an actionable next step");
    essentials = aml::diagnostics::local_state::essentials(config, true, true);
    expect(essentials.status == aml::diagnostics::CheckStatus::PASS,
           "Essentials may pass only with configured, initialized, authenticated local state");
    expect(essentials.detail.find("not probed") != std::string::npos,
           "Essentials must not claim a remote service was probed");

    auto updater = aml::diagnostics::local_state::updater(false);
    expect(updater.status == aml::diagnostics::CheckStatus::WARN,
           "an updater without an embedded signing key must warn");
    expect(updater.detail.find("fail closed") != std::string::npos,
           "missing updater key must explicitly fail closed");
    updater = aml::diagnostics::local_state::updater(true);
    expect(updater.status == aml::diagnostics::CheckStatus::PASS,
           "a configured signed-manifest updater must pass its local policy check");
    expect(updater.detail.find("In-process") != std::string::npos &&
               updater.detail.find("updater.exe") == std::string::npos,
           "updater result must describe the in-process updater only");

    const auto providers = aml::diagnostics::local_state::providers();
    expect(providers.status == aml::diagnostics::CheckStatus::WARN &&
               providers.detail.find("not contacted") != std::string::npos,
           "provider diagnostics must explicitly avoid external requests");

    const std::string version = aml::diagnostics::local_state::version_detail("9.8.7-test");
    expect(version.find("9.8.7-test") != std::string::npos &&
               version.find("1.0.0") == std::string::npos,
           "diagnostic version wording must use the supplied release source");
    return success;
}

}  // namespace

int main() {
    bool success = true;
    if (!test_outbox()) {
        std::cerr << "FAILED: outbox\n";
        success = false;
    }
    if (!test_local_diagnostics_state()) success = false;
    return success ? 0 : 1;
}
