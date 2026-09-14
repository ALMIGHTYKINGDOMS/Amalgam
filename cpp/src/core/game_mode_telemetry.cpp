#include "core/game_mode_telemetry.h"

#include "core/diagnostics_outbox.h"
#include "core/replay.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstring>
#include <fstream>
#include <limits>

namespace aml::telemetry {

namespace {

constexpr uint32_t kMagic = 0x314D4C54u;  // "TLM1" little-endian
constexpr uint8_t kVersion = 1;
constexpr size_t kMaxPayload = 4096;
constexpr size_t kMaxLines = 15;
constexpr size_t kMaxTextBytes = 127;

uint64_t now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

uint16_t read_u16(const uint8_t* data, size_t at) {
    return static_cast<uint16_t>(data[at]) |
           static_cast<uint16_t>(data[at + 1] << 8);
}

uint32_t read_u32(const uint8_t* data, size_t at) {
    return static_cast<uint32_t>(data[at]) |
           (static_cast<uint32_t>(data[at + 1]) << 8) |
           (static_cast<uint32_t>(data[at + 2]) << 16) |
           (static_cast<uint32_t>(data[at + 3]) << 24);
}

std::string clean_text(const std::string& input) {
    std::string out;
    out.reserve(std::min(input.size(), kMaxTextBytes));
    bool space = false;
    for (size_t i = 0; i < input.size();) {
        unsigned char c = static_cast<unsigned char>(input[i]);
        if (c == 0xC2 && i + 1 < input.size() &&
            static_cast<unsigned char>(input[i + 1]) == 0xA7) {
            i += 2;
            if (i < input.size()) ++i;
            continue;
        }
        if (c == 0xA7) {
            i += std::min<size_t>(2, input.size() - i);
            continue;
        }
        if (c == 0xEF && i + 2 < input.size() &&
            static_cast<unsigned char>(input[i + 1]) == 0xBB &&
            static_cast<unsigned char>(input[i + 2]) == 0xBF) {
            i += 3;
            continue;
        }
        if (c == 0xE2 && i + 2 < input.size() &&
            static_cast<unsigned char>(input[i + 1]) == 0x80 &&
            (static_cast<unsigned char>(input[i + 2]) == 0x8B ||
             static_cast<unsigned char>(input[i + 2]) == 0x8C ||
             static_cast<unsigned char>(input[i + 2]) == 0x8D)) {
            i += 3;
            continue;
        }
        if (c < 0x20 || c == 0x7F) {
            ++i;
            continue;
        }
        if (std::isspace(c)) {
            space = !out.empty();
            ++i;
            continue;
        }
        if (space && !out.empty() && out.back() != ' ') out.push_back(' ');
        space = false;
        if (out.size() < kMaxTextBytes) out.push_back(static_cast<char>(c));
        ++i;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::string lower(std::string value) {
    for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

bool has(const std::string& value, const char* needle) {
    return value.find(needle) != std::string::npos;
}

int number_after(const std::string& text, const char* label) {
    size_t at = text.find(label);
    if (at == std::string::npos) return -1;
    at += std::strlen(label);
    while (at < text.size() && (text[at] == ' ' || text[at] == ':' || text[at] == '=')) ++at;
    bool negative = at < text.size() && text[at] == '-';
    if (negative) ++at;
    int value = 0;
    bool found = false;
    while (at < text.size() && std::isdigit(static_cast<unsigned char>(text[at]))) {
        found = true;
        if (value <= (std::numeric_limits<int>::max() - 9) / 10)
            value = value * 10 + (text[at] - '0');
        ++at;
    }
    return found ? (negative ? -value : value) : -1;
}

int timer_after(const std::string& text, const char* label) {
    size_t at = text.find(label);
    if (at == std::string::npos) return -1;
    at += std::strlen(label);
    while (at < text.size() && !std::isdigit(static_cast<unsigned char>(text[at]))) ++at;
    size_t start = at;
    while (at < text.size() && std::isdigit(static_cast<unsigned char>(text[at]))) ++at;
    if (at < text.size() && text[at] == ':') {
        int minutes = 0;
        for (size_t i = start; i < at; ++i) minutes = minutes * 10 + text[i] - '0';
        ++at;
        size_t seconds_start = at;
        while (at < text.size() && std::isdigit(static_cast<unsigned char>(text[at]))) ++at;
        if (seconds_start == at) return -1;
        int seconds = 0;
        for (size_t i = seconds_start; i < at; ++i) seconds = seconds * 10 + text[i] - '0';
        return minutes * 60 + seconds;
    }
    if (start == at) return -1;
    int value = 0;
    for (size_t i = start; i < at; ++i) value = value * 10 + text[i] - '0';
    return value;
}

std::string text_after(const std::string& text, const char* label) {
    size_t at = text.find(label);
    if (at == std::string::npos) return {};
    at += std::strlen(label);
    while (at < text.size() && (text[at] == ' ' || text[at] == ':' || text[at] == '=')) ++at;
    return clean_text(text.substr(at));
}

std::string csv_escape(const std::string& value) {
    if (value.find_first_of(",\"\r\n") == std::string::npos) return value;
    std::string out = "\"";
    for (char c : value) {
        if (c == '"') out += "\"\"";
        else out += c;
    }
    out += '"';
    return out;
}

struct Detection {
    Mode mode = Mode::Unknown;
    int score = 0;
};

Detection detect(const Frame& frame) {
    int scores[7] = {};
    const std::string title = lower(clean_text(frame.title));
    auto score_text = [&](const std::string& value, bool title_line) {
        const int multiplier = title_line ? 3 : 1;
        if (has(value, "bed wars") || has(value, "bedwars")) scores[1] += 35 * multiplier;
        if (has(value, "bed") || has(value, "final kill") || has(value, "generator")) scores[1] += 12;
        if (has(value, "sky wars") || has(value, "skywars")) scores[2] += 35 * multiplier;
        if (has(value, "refill") || has(value, "chest") || has(value, "players alive")) scores[2] += 12;
        if (has(value, "uhc") || has(value, "ultra hardcore")) scores[3] += 35 * multiplier;
        if (has(value, "episode") || has(value, "border") || has(value, "grace")) scores[3] += 12;
        if (has(value, "crystal pvp") || has(value, "crystal") || has(value, "anchor")) scores[4] += 35 * multiplier;
        if (has(value, "opponent") || has(value, "round") || has(value, "queue")) scores[4] += 12;
        if (has(value, "faction") || has(value, "claim") || has(value, "territory")) scores[5] += 30;
        if (has(value, "practice") || has(value, "duel") || has(value, "elo")) scores[6] += 30;
    };
    score_text(title, true);
    for (const auto& line : frame.lines) score_text(lower(clean_text(line.text)), false);
    int best = 0;
    Mode mode = Mode::Unknown;
    for (int i = 1; i <= 6; ++i) {
        if (scores[i] > best) {
            best = scores[i];
            mode = static_cast<Mode>(i);
        }
    }
    if (best < 30) return {};
    return {mode, std::min(100, best)};
}

State parse_state(const Frame& frame, const State& previous, Mode detected, int confidence,
                  Mode manual_mode, uint64_t now) {
    State out;
    out.manual_mode = manual_mode;
    out.detected_mode = detected;
    out.confidence = confidence;
    out.title = clean_text(frame.title);
    out.active = !out.title.empty() || !frame.lines.empty();
    out.mode = manual_mode == Mode::Auto ? detected : manual_mode;
    out.lines.reserve(std::min(frame.lines.size(), kMaxLines));
    for (const auto& input : frame.lines) {
        if (out.lines.size() >= kMaxLines) break;
        ScoreLine line;
        line.text = clean_text(input.text);
        line.score = input.score;
        if (!line.text.empty()) out.lines.push_back(std::move(line));
    }

    for (const auto& line : out.lines) {
        std::string value = lower(line.text);
        if (out.players_alive < 0) {
            out.players_alive = number_after(value, "players")
                >= 0 ? number_after(value, "players") : number_after(value, "alive");
        }
        if (out.teams_alive < 0) out.teams_alive = number_after(value, "teams");
        if (out.final_kills < 0) out.final_kills = number_after(value, "final kills");
        if (out.kills < 0) out.kills = number_after(value, "kills");
        if (out.beds_alive < 0) out.beds_alive = number_after(value, "beds");
        if (out.episode < 0) out.episode = number_after(value, "episode");
        if (out.round < 0) out.round = number_after(value, "round");
        if (out.refill_seconds < 0) out.refill_seconds = timer_after(value, "refill");
        if (out.generator_seconds < 0) out.generator_seconds = timer_after(value, "generator");
        if (out.generator_seconds < 0) out.generator_seconds = timer_after(value, "diamond");
        if (out.chest_seconds < 0) out.chest_seconds = timer_after(value, "chest");
        if (out.border_seconds < 0) out.border_seconds = timer_after(value, "border");
        if (out.teleport_seconds < 0) out.teleport_seconds = timer_after(value, "teleport");
        if (out.crystals < 0) out.crystals = number_after(value, "crystals");
        if (out.explosions < 0) out.explosions = number_after(value, "explosions");
        if (out.timer_seconds < 0) {
            out.timer_seconds = timer_after(value, "timer");
            if (out.timer_seconds < 0) out.timer_seconds = timer_after(value, "time");
        }
        if (out.map.empty()) out.map = text_after(value, "map");
        if (out.opponent.empty()) out.opponent = text_after(value, "opponent");
        if (out.faction.empty()) out.faction = text_after(value, "faction");
        if (out.claim.empty()) out.claim = text_after(value, "claim");
        if (out.objective.empty() && (has(value, "objective") || has(value, "goal")))
            out.objective = line.text;
    }
    if (out.objective.empty() && !out.title.empty()) out.objective = out.title;

    if (out.detected_mode != previous.detected_mode ||
        (out.detected_mode == Mode::Unknown && previous.active != out.active)) {
        out.match_id = previous.match_id + (out.active ? 1 : 0);
        out.match_started_ms = out.active ? now : 0;
    } else {
        out.match_id = previous.match_id;
        out.match_started_ms = previous.match_started_ms;
    }
    out.last_update_ms = now;
    out.match_elapsed_ms = out.match_started_ms == 0 || now < out.match_started_ms
        ? 0 : now - out.match_started_ms;
    if (out.beds_alive != previous.beds_alive && out.beds_alive >= 0 && previous.beds_alive >= 0)
        out.alert = "Bed status changed";
    else if (out.kills != previous.kills && out.kills >= 0 && previous.kills >= 0)
        out.alert = "Kills updated";
    else if (out.players_alive != previous.players_alive && out.players_alive >= 0 && previous.players_alive >= 0)
        out.alert = "Players remaining changed";
    return out;
}

}  // namespace

const char* mode_name(Mode mode) {
    switch (mode) {
        case Mode::Auto: return "Auto";
        case Mode::BedWars: return "BedWars";
        case Mode::SkyWars: return "SkyWars";
        case Mode::Uhc: return "UHC";
        case Mode::Crystal: return "Crystal PvP";
        case Mode::Smp: return "SMP/Factions";
        case Mode::Practice: return "Practice";
        default: return "Unknown";
    }
}

Mode mode_from_index(int index) {
    if (index < 0 || index > 7) return Mode::Auto;
    if (index == 0) return Mode::Auto;
    if (index == 1) return Mode::Unknown;
    return static_cast<Mode>(index - 1);
}

int mode_to_index(Mode mode) {
    if (mode == Mode::Auto) return 0;
    if (mode == Mode::Unknown) return 1;
    return static_cast<int>(mode) + 1;
}

bool decode_payload(const uint8_t* data, size_t len, Frame& out, std::string* error) {
    out = {};
    auto fail = [&](const char* message) {
        if (error) *error = message;
        return false;
    };
    if (!data || len < 10 || len > kMaxPayload) return fail("invalid telemetry payload size");
    if (read_u32(data, 0) != kMagic || data[4] != kVersion) return fail("unsupported telemetry payload");
    size_t at = 8;
    auto read_text = [&](std::string& target) {
        if (at + 2 > len) return false;
        uint16_t size = read_u16(data, at);
        at += 2;
        if (size > kMaxTextBytes || at + size > len) return false;
        target.assign(reinterpret_cast<const char*>(data + at), size);
        at += size;
        return true;
    };
    if (!read_text(out.title)) return fail("invalid telemetry title");
    size_t count = std::min<size_t>(data[5], kMaxLines);
    out.lines.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        ScoreLine line;
        if (!read_text(line.text)) return fail("invalid telemetry line");
        if (!line.text.empty()) out.lines.push_back(std::move(line));
    }
    return true;
}

void Store::publish(const Frame& input) {
    std::lock_guard<std::mutex> lock(mutex_);
    Frame frame;
    frame.title = clean_text(input.title);
    frame.lines.reserve(std::min(input.lines.size(), kMaxLines));
    for (const auto& input_line : input.lines) {
        if (frame.lines.size() >= kMaxLines) break;
        ScoreLine line{clean_text(input_line.text), input_line.score};
        if (!line.text.empty()) frame.lines.push_back(std::move(line));
    }
    frame_ = frame;
    Detection detection = detect(frame);
    if (detection.mode != candidate_mode_) {
        candidate_mode_ = detection.mode;
        candidate_frames_ = 1;
    } else {
        ++candidate_frames_;
    }
    uint64_t now = now_ms();
    Mode detected = state_.detected_mode;
    int confidence = state_.confidence;
    if (candidate_frames_ >= 2 || state_.detected_mode == Mode::Unknown) {
        detected = detection.mode;
        confidence = detection.score;
    }
    bool empty = frame.title.empty() && frame.lines.empty();
    bool mode_changed = state_.active && !empty && detected != Mode::Unknown &&
                        state_.detected_mode != Mode::Unknown && detected != state_.detected_mode;
    bool ending = state_.active && empty;
    State previous = state_;
    if (ending || mode_changed) {
        finalize_locked(ending ? "sidebar_hidden" : "mode_changed", now);
        previous = {};
        previous.manual_mode = state_.manual_mode;
        previous.match_id = state_.match_id;
        if (ending) {
            detected = Mode::Unknown;
            confidence = 0;
        }
    }
    State next = parse_state(frame, previous, detected, confidence,
                             state_.manual_mode, now);
    bool started = next.active && next.detected_mode != Mode::Unknown &&
                   (!state_.active || next.match_id != state_.match_id);
    state_ = std::move(next);
    replay::recorder().append_state(state_);
    if (started) add_marker_locked("match_start", mode_name(state_.mode), 0);
    if (!state_.alert.empty()) add_marker_locked("alert", state_.alert, state_.match_elapsed_ms);
}

void Store::publish_payload(const uint8_t* data, size_t len) {
    Frame frame;
    if (!decode_payload(data, len, frame, nullptr)) {
        clear();
        return;
    }
    publish(frame);
}

void Store::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    frame_ = {};
    candidate_mode_ = Mode::Unknown;
    candidate_frames_ = 0;
    Mode manual = state_.manual_mode;
    finalize_locked("sidebar_hidden", now_ms());
    State empty;
    empty.manual_mode = manual;
    empty.mode = manual == Mode::Auto ? Mode::Unknown : manual;
    empty.match_id = state_.match_id;
    state_ = std::move(empty);
}

void Store::begin_session() {
    std::lock_guard<std::mutex> lock(mutex_);
    finalize_locked("session_restart", now_ms());
    frame_ = {};
    state_ = {};
    candidate_mode_ = Mode::Unknown;
    candidate_frames_ = 0;
    history_.clear();
    markers_.clear();
    summaries_.clear();
    session_stats_ = {};
    session_active_ = true;
}

void Store::end_session() {
    std::lock_guard<std::mutex> lock(mutex_);
    finalize_locked("session_end", now_ms());
    session_active_ = false;
}

void Store::add_marker_locked(const char* kind, const std::string& label, uint64_t offset_ms) {
    if (!state_.active || state_.match_id == 0 || !kind) return;
    Marker marker;
    marker.match_id = state_.match_id;
    marker.offset_ms = offset_ms;
    marker.kind = clean_text(kind);
    marker.label = clean_text(label);
    if (marker.kind.empty()) marker.kind = "marker";
    if (marker.label.size() > kMaxTextBytes) marker.label.resize(kMaxTextBytes);
    markers_.push_back(std::move(marker));
    replay::recorder().append_marker(markers_.back());
    if (markers_.size() > 512) markers_.erase(markers_.begin());
}

void Store::finalize_locked(const char* reason, uint64_t now) {
    if (!state_.active || state_.match_id == 0) return;
    State finished = state_;
    if (finished.match_started_ms != 0 && now >= finished.match_started_ms)
        finished.match_elapsed_ms = now - finished.match_started_ms;
    add_marker_locked("match_end", reason ? reason : "ended", finished.match_elapsed_ms);
    MatchSummary summary{finished, now, reason ? reason : "ended"};
    summaries_.push_back(summary);
    if (summaries_.size() > 128) summaries_.erase(summaries_.begin());
    history_.push_back(finished);
    if (history_.size() > 128) history_.erase(history_.begin());
    ++session_stats_.matches;
    session_stats_.total_elapsed_ms += finished.match_elapsed_ms;
    if (finished.kills >= 0) session_stats_.total_kills += static_cast<uint64_t>(finished.kills);
    if (finished.final_kills >= 0)
        session_stats_.total_final_kills += static_cast<uint64_t>(finished.final_kills);
    if (finished.explosions >= 0)
        session_stats_.total_explosions += static_cast<uint64_t>(finished.explosions);
    if (finished.crystals >= 0)
        session_stats_.total_crystals += static_cast<uint64_t>(finished.crystals);
    diagnostics::outbox().enqueue(finished);
    replay::recorder().append_summary(summary);
}

bool Store::add_marker(const std::string& kind, const std::string& label) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!state_.active) return false;
    uint64_t offset = state_.match_started_ms == 0 ? 0 : state_.match_elapsed_ms;
    add_marker_locked(kind.c_str(), label, offset);
    return true;
}

State Store::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    State out = state_;
    uint64_t now = now_ms();
    if (out.match_started_ms != 0 && now >= out.match_started_ms)
        out.match_elapsed_ms = now - out.match_started_ms;
    return out;
}

bool Store::export_report(const std::string& path) const {
    if (path.empty()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open()) return false;
    file << "match_id,mode,confidence,elapsed_ms,players_alive,teams_alive,kills,final_kills,"
            "beds_alive,episode,timer_seconds,refill_seconds,generator_seconds,chest_seconds,"
            "border_seconds,teleport_seconds,crystals,explosions\n";
    auto write = [&](const State& row) {
        file << row.match_id << ',' << mode_name(row.mode) << ',' << row.confidence << ','
             << row.match_elapsed_ms << ',' << row.players_alive << ',' << row.teams_alive << ','
             << row.kills << ',' << row.final_kills << ',' << row.beds_alive << ',' << row.episode << ','
             << row.timer_seconds << ',' << row.refill_seconds << ',' << row.generator_seconds << ','
             << row.chest_seconds << ',' << row.border_seconds << ',' << row.teleport_seconds << ','
             << row.crystals << ',' << row.explosions << '\n';
    };
    for (const auto& row : history_) write(row);
    if (state_.active) write(state_);
    return file.good();
}

std::vector<Marker> Store::markers() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return markers_;
}

std::vector<MatchSummary> Store::match_summaries() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return summaries_;
}

SessionStats Store::session_stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    SessionStats result = session_stats_;
    if (state_.active) {
        ++result.matches;
        result.total_elapsed_ms += state_.match_elapsed_ms;
        if (state_.kills >= 0) result.total_kills += static_cast<uint64_t>(state_.kills);
        if (state_.final_kills >= 0) result.total_final_kills += static_cast<uint64_t>(state_.final_kills);
        if (state_.explosions >= 0) result.total_explosions += static_cast<uint64_t>(state_.explosions);
        if (state_.crystals >= 0) result.total_crystals += static_cast<uint64_t>(state_.crystals);
    }
    return result;
}

bool Store::export_match_summaries(const std::string& path) const {
    if (path.empty()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open()) return false;
    file << "match_id,mode,ended_ms,end_reason,elapsed_ms,players_alive,teams_alive,kills,final_kills,"
            "beds_alive,episode,crystals,explosions\n";
    for (const auto& summary : summaries_) {
        const State& row = summary.final_state;
        file << row.match_id << ',' << mode_name(row.mode) << ',' << summary.ended_ms << ','
             << csv_escape(summary.end_reason) << ',' << row.match_elapsed_ms << ','
             << row.players_alive << ',' << row.teams_alive << ',' << row.kills << ','
             << row.final_kills << ',' << row.beds_alive << ',' << row.episode << ','
             << row.crystals << ',' << row.explosions << '\n';
    }
    return file.good();
}

bool Store::export_markers(const std::string& path) const {
    if (path.empty()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open()) return false;
    file << "match_id,offset_ms,kind,label\n";
    for (const auto& marker : markers_)
        file << marker.match_id << ',' << marker.offset_ms << ',' << csv_escape(marker.kind) << ','
             << csv_escape(marker.label) << '\n';
    return file.good();
}

bool Store::export_session_stats(const std::string& path) const {
    if (path.empty()) return false;
    SessionStats stats = session_stats();
    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open()) return false;
    file << "matches,total_elapsed_ms,total_kills,total_final_kills,total_explosions,total_crystals\n"
         << stats.matches << ',' << stats.total_elapsed_ms << ',' << stats.total_kills << ','
         << stats.total_final_kills << ',' << stats.total_explosions << ',' << stats.total_crystals << '\n';
    return file.good();
}

void Store::set_manual_mode(Mode mode) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.manual_mode = mode;
    state_ = parse_state(frame_, state_, state_.detected_mode, state_.confidence, mode, now_ms());
}

Mode Store::manual_mode() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_.manual_mode;
}

Store& store() {
    static Store instance;
    return instance;
}

}  // namespace aml::telemetry
