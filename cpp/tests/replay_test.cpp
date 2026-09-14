#include "core/game_mode_telemetry.h"
#include "core/replay.h"

#include <cstdio>
#include <iostream>

int main() {
    const std::string path = "amalgam_telemetry_test.amrl";
    std::remove(path.c_str());
    aml::replay::Recorder recorder;
    if (!recorder.start(path)) {
        std::cerr << "FAILED: start\n";
        return 1;
    }
    aml::telemetry::State state;
    state.active = true;
    state.mode = aml::telemetry::Mode::Practice;
    state.match_id = 9;
    state.confidence = 88;
    state.match_elapsed_ms = 1000;
    state.kills = 2;
    recorder.append_state(state);
    aml::telemetry::Marker marker{9, 500, "manual", "test"};
    recorder.append_marker(marker);
    aml::telemetry::MatchSummary summary{state, 2000, "test_end"};
    recorder.append_summary(summary);
    recorder.stop();

    aml::replay::Player player;
    if (!player.open(path)) {
        std::cerr << "FAILED: open\n";
        return 1;
    }
    int count = 0;
    aml::replay::Record record;
    bool saw_state = false;
    bool saw_marker = false;
    bool saw_summary = false;
    while (player.next(record)) {
        ++count;
        saw_state |= record.type == aml::replay::RecordType::State;
        saw_marker |= record.type == aml::replay::RecordType::Marker;
        saw_summary |= record.type == aml::replay::RecordType::Summary;
    }
    player.close();
    std::remove(path.c_str());
    if (count != 3 || !saw_state || !saw_marker || !saw_summary) {
        std::cerr << "FAILED: records\n";
        return 1;
    }
    return 0;
}
