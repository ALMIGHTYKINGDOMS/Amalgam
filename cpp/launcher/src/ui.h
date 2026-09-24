#pragma once

#include "config.h"
#include "instances.h"

#include <set>
#include <string>

namespace aml::ui {

struct UiState;

// Used by the release visual-review tooling. Fixture mode is deliberately
// opt-in and never changes the normal player-facing launcher state.
struct RunOptions {
    bool fixture_mode = false;
    bool safe_mode = false;  // started with --safe-mode: no auto network calls
    std::wstring capture_path;
    // Fixture-only inputs used by the release visual-QA harness. They never
    // participate in normal launcher startup and keep snapshot data isolated
    // from the executable directory and from player-owned profiles.
    std::wstring fixture_root;
    std::string fixture_case;
    // 0 = top, 1 = middle, 2 = bottom. Applied to the page-level scroll host
    // after the requested fixture surface has rendered.
    int fixture_scroll_position = 0;
    int capture_after_frames = 0;
    int initial_width = 0;
    int initial_height = 0;
    std::string initial_page;
    int fixture_tab = 0;
    int fixture_sidebar = 0;
    int fixture_settings_section = 0;
    bool fixture_profile_detail = false;
    int fixture_instance_tab = 0;
    bool fixture_project_detail = false;
    bool fixture_cloud = false;
    int fixture_server_detail_tab = -1;
};

bool run_window(config::Config* cfg, const RunOptions& options = {});

// Returns whether a named, deterministic visual-review fixture is supported by
// the snapshot harness. Keeping this registry in the UI layer makes the CLI
// reject typos without allowing arbitrary runtime state to leak into a capture.
bool is_visual_fixture_case(const std::string& fixture_case);

// Canonical, sorted registry accepted by the deterministic snapshot harness.
// Release tooling consumes this through --list-ui-fixtures rather than
// reverse-engineering source, so a capture matrix is always checked against
// the exact binary it is about to execute.
const std::set<std::string>& visual_fixture_cases();

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
