#include "core/game_mode_telemetry.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

void put16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8));
}

void put32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value >> 16));
    out.push_back(static_cast<uint8_t>(value >> 24));
}

void text(std::vector<uint8_t>& out, const char* value) {
    std::string s(value);
    put16(out, static_cast<uint16_t>(s.size()));
    out.insert(out.end(), s.begin(), s.end());
}

std::vector<uint8_t> payload(const char* title, std::initializer_list<const char*> lines) {
    std::vector<uint8_t> out;
    put32(out, 0x314D4C54u);
    out.push_back(1);
    out.push_back(static_cast<uint8_t>(lines.size()));
    put16(out, 0);
    text(out, title);
    for (const char* line : lines) text(out, line);
    return out;
}

bool test_bedwars() {
    aml::telemetry::Store store;
    const aml::telemetry::Frame frame{"§cBED WARS", {{"Beds: 3"}, {"Final Kills: 2"},
                                                        {"Players: 8"}, {"Diamond generator: 00:30"}}};
    store.publish(frame);
    store.publish(frame);
    const auto state = store.snapshot();
    return state.mode == aml::telemetry::Mode::BedWars && state.beds_alive == 3 &&
           state.final_kills == 2 && state.players_alive == 8 && state.generator_seconds == 30 &&
           state.title == "BED WARS";
}

bool test_other_profiles() {
    struct Fixture {
        aml::telemetry::Mode mode;
        const char* title;
        const char* line;
    } fixtures[] = {
        {aml::telemetry::Mode::SkyWars, "SKYWARS", "Chest refill: 01:20"},
        {aml::telemetry::Mode::Uhc, "UHC", "Teleport: 00:45"},
        {aml::telemetry::Mode::Crystal, "Crystal PvP", "Crystals: 4"},
        {aml::telemetry::Mode::Smp, "Realm", "Faction: Builders"},
        {aml::telemetry::Mode::Practice, "Practice", "Round: 3"},
    };
    for (const auto& fixture : fixtures) {
        aml::telemetry::Store store;
        aml::telemetry::Frame frame{fixture.title, {{fixture.line}}};
        store.publish(frame);
        store.publish(frame);
        if (store.snapshot().mode != fixture.mode) return false;
    }
    return true;
}

bool test_manual_override_and_payload() {
    aml::telemetry::Store store;
    const auto bytes = payload("SKYWARS", {"Players: 12", "Refill: 01:00"});
    store.publish_payload(bytes.data(), bytes.size());
    store.publish_payload(bytes.data(), bytes.size());
    if (store.snapshot().mode != aml::telemetry::Mode::SkyWars) return false;
    store.set_manual_mode(aml::telemetry::Mode::Uhc);
    if (store.snapshot().mode != aml::telemetry::Mode::Uhc) return false;
    store.set_manual_mode(aml::telemetry::Mode::Auto);
    return store.snapshot().mode == aml::telemetry::Mode::SkyWars;
}

bool test_bounds_and_clear() {
    std::vector<uint8_t> bad = payload("BED WARS", {});
    bad[0] = 0;
    aml::telemetry::Frame out;
    std::string error;
    if (aml::telemetry::decode_payload(bad.data(), bad.size(), out, &error)) return false;

    aml::telemetry::Store store;
    auto good = payload("UHC", {"Border: 02:00"});
    store.publish_payload(good.data(), good.size());
    store.clear();
    const auto state = store.snapshot();
    return !state.active && state.mode == aml::telemetry::Mode::Unknown && state.lines.empty();
}

bool test_report() {
    aml::telemetry::Store store;
    aml::telemetry::Frame frame{"Practice", {{"Round: 3"}, {"Explosions: 5"}}};
    store.publish(frame);
    store.publish(frame);
    const char* path = "amalgam_game_mode_report_test.csv";
    if (!store.export_report(path)) return false;
    std::ifstream file(path);
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();
    std::remove(path);
    return content.find("match_id,mode,confidence") == 0 && content.find("Practice") != std::string::npos;
}

bool test_session_markers() {
    aml::telemetry::Store store;
    aml::telemetry::Frame frame{"Practice", {{"Round: 1"}, {"Kills: 2"}}};
    store.publish(frame);
    store.publish(frame);
    if (!store.add_marker("manual", "test marker")) return false;
    store.clear();
    auto summaries = store.match_summaries();
    auto markers = store.markers();
    auto stats = store.session_stats();
    if (summaries.size() != 1 || markers.size() < 3 || stats.matches != 1 || stats.total_kills != 2) {
        return false;
    }
    const char* paths[] = {"amalgam_matches_test.csv", "amalgam_markers_test.csv",
                           "amalgam_session_test.csv"};
    if (!store.export_match_summaries(paths[0]) || !store.export_markers(paths[1]) ||
        !store.export_session_stats(paths[2])) return false;
    for (const char* path : paths) std::remove(path);
    return true;
}

}  // namespace

int main() {
    struct Test {
        const char* name;
        bool (*run)();
    } tests[] = {{"bedwars", test_bedwars}, {"profiles", test_other_profiles},
                 {"manual_payload", test_manual_override_and_payload},
                 {"bounds_clear", test_bounds_and_clear}, {"report", test_report},
                 {"session_markers", test_session_markers}};
    bool ok = true;
    for (const auto& test : tests) {
        if (!test.run()) {
            std::cerr << "FAILED: " << test.name << "\n";
            ok = false;
        }
    }
    return ok ? 0 : 1;
}
