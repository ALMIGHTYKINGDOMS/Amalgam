#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace aml::telemetry {

enum class Mode : int8_t {
    Auto = -1,
    Unknown = 0,
    BedWars,
    SkyWars,
    Uhc,
    Crystal,
    Smp,
    Practice,
};

struct ScoreLine {
    std::string text;
    int score = -1;
};

struct Frame {
    std::string title;
    std::vector<ScoreLine> lines;
};

struct State {
    Mode mode = Mode::Unknown;
    Mode detected_mode = Mode::Unknown;
    Mode manual_mode = Mode::Auto;
    int confidence = 0;
    bool active = false;
    std::string title;
    std::vector<ScoreLine> lines;

    int players_alive = -1;
    int teams_alive = -1;
    int kills = -1;
    int final_kills = -1;
    int beds_alive = -1;
    int episode = -1;
    int timer_seconds = -1;
    int refill_seconds = -1;
    int generator_seconds = -1;
    int chest_seconds = -1;
    int border_seconds = -1;
    int teleport_seconds = -1;
    int crystals = -1;
    int explosions = -1;
    int round = -1;
    std::string map;
    std::string opponent;
    std::string faction;
    std::string claim;
    std::string objective;

    uint64_t match_id = 0;
    uint64_t match_started_ms = 0;
    uint64_t last_update_ms = 0;
    uint64_t match_elapsed_ms = 0;
    std::string alert;
};

struct Marker {
    uint64_t match_id = 0;
    uint64_t offset_ms = 0;
    std::string kind;
    std::string label;
};

struct MatchSummary {
    State final_state;
    uint64_t ended_ms = 0;
    std::string end_reason;
};

struct SessionStats {
    uint64_t matches = 0;
    uint64_t total_elapsed_ms = 0;
    uint64_t total_kills = 0;
    uint64_t total_final_kills = 0;
    uint64_t total_explosions = 0;
    uint64_t total_crystals = 0;
};

const char* mode_name(Mode mode);
Mode mode_from_index(int index);
int mode_to_index(Mode mode);

// TLM1 is intentionally separate from the action/snapshot wire format.
bool decode_payload(const uint8_t* data, size_t len, Frame& out, std::string* error = nullptr);

class Store {
public:
    void publish(const Frame& frame);
    void publish_payload(const uint8_t* data, size_t len);
    void clear();
    void begin_session();
    void end_session();
    bool add_marker(const std::string& kind, const std::string& label);

    State snapshot() const;
    bool export_report(const std::string& path) const;
    bool export_match_summaries(const std::string& path) const;
    bool export_markers(const std::string& path) const;
    bool export_session_stats(const std::string& path) const;
    std::vector<Marker> markers() const;
    std::vector<MatchSummary> match_summaries() const;
    SessionStats session_stats() const;
    void set_manual_mode(Mode mode);
    Mode manual_mode() const;

private:
    mutable std::mutex mutex_;
    Frame frame_;
    State state_{};
    Mode candidate_mode_ = Mode::Unknown;
    int candidate_frames_ = 0;
    std::vector<State> history_;
    std::vector<Marker> markers_;
    std::vector<MatchSummary> summaries_;
    SessionStats session_stats_{};
    bool session_active_ = false;

    void finalize_locked(const char* reason, uint64_t now);
    void add_marker_locked(const char* kind, const std::string& label, uint64_t offset_ms);
};

Store& store();

}  // namespace aml::telemetry
