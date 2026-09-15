#pragma once

#include "config.h"
#include "instances.h"

#include <string>

namespace aml::ui {

struct UiState;

// Used by the release visual-review tooling. Fixture mode is deliberately
// opt-in and never changes the normal player-facing launcher state.
struct RunOptions {
    bool fixture_mode = false;
    bool safe_mode = false;  // started with --safe-mode: no auto network calls
    std::wstring capture_path;
    int capture_after_frames = 0;
    int initial_width = 0;
    int initial_height = 0;
    std::string initial_page;
    int fixture_tab = 0;
    int fixture_sidebar = 0;
    int fixture_settings_section = 0;
    bool fixture_profile_detail = false;
    bool fixture_project_detail = false;
    bool fixture_cloud = false;
    int fixture_server_detail_tab = -1;
};

bool run_window(config::Config* cfg, const RunOptions& options = {});

// Canonical release identity, shared by the About page, the updater policy and
// the command-line help so the version is reported from one place.
const char* launcher_version();

// Synchronize launcher-owned metadata and shared services into the selected
// profile's in-game client directory. Exposed for the launch path and its
// integration probe; normal UI code calls it automatically before play.
bool prepare_client_bridge_for_profile(const config::Config& cfg,
                                       const instances::Instance& instance,
                                       std::string* error = nullptr);

// Load persisted settings from Config into the static UI singletons.
void load_performance_settings(const config::Config& cfg);
void load_social_settings(const config::Config& cfg);
void load_mod_settings(const config::Config& cfg);

// Save current UI singleton values back to Config (used by sync_ui_config).
void save_performance_settings(config::Config& cfg);
void save_social_settings(config::Config& cfg);
void save_mod_settings(config::Config& cfg);

}  // namespace aml::ui
