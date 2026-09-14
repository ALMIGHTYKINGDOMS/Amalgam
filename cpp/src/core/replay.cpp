#include "core/replay.h"

#include "core/game_mode_telemetry.h"

#include <algorithm>
#include <chrono>
#include <filesystem>

namespace aml::replay {

namespace {

constexpr char kMagic[] = "AMLRPL01";
constexpr uint32_t kVersion = 1;
constexpr size_t kMaxLabel = 127;

uint64_t now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

void put16(std::ostream& out, uint16_t value) {
    out.put(static_cast<char>(value));
    out.put(static_cast<char>(value >> 8));
}

void put32(std::ostream& out, uint32_t value) {
    for (int i = 0; i < 4; ++i) out.put(static_cast<char>(value >> (i * 8)));
}

void put64(std::ostream& out, uint64_t value) {
    for (int i = 0; i < 8; ++i) out.put(static_cast<char>(value >> (i * 8)));
}

uint16_t get16(std::istream& in, bool& ok) {
    int a = in.get();
    int b = in.get();
    if (a < 0 || b < 0) {
        ok = false;
        return 0;
    }
    return static_cast<uint16_t>(a | (b << 8));
}

uint32_t get32(std::istream& in, bool& ok) {
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        int byte = in.get();
        if (byte < 0) {
            ok = false;
            return 0;
        }
        value |= static_cast<uint32_t>(byte) << (i * 8);
    }
    return value;
}

uint64_t get64(std::istream& in, bool& ok) {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        int byte = in.get();
        if (byte < 0) {
            ok = false;
            return 0;
        }
        value |= static_cast<uint64_t>(byte) << (i * 8);
    }
    return value;
}

std::array<int32_t, 16> metrics(const telemetry::State& state) {
    return {state.players_alive, state.teams_alive, state.kills, state.final_kills,
            state.beds_alive, state.episode, state.timer_seconds, state.refill_seconds,
            state.generator_seconds, state.chest_seconds, state.border_seconds,
            state.teleport_seconds, state.crystals, state.explosions, 0, 0};
}

}  // namespace

bool Recorder::start(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_locked();
    if (path.empty()) return false;
    std::filesystem::path target(path);
    if (target.has_parent_path()) std::filesystem::create_directories(target.parent_path());
    file_.open(path, std::ios::binary | std::ios::trunc);
    if (!file_.is_open()) return false;
    file_.write(kMagic, sizeof(kMagic) - 1);
    put32(file_, kVersion);
    put32(file_, 0);
    file_.flush();
    if (!file_.good()) {
        file_.close();
        return false;
    }
    path_ = path;
    last_state_ms_ = 0;
    last_match_id_ = 0;
    return true;
}

void Recorder::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_locked();
}

void Recorder::stop_locked() {
    if (file_.is_open()) file_.close();
    path_.clear();
    last_state_ms_ = 0;
    last_match_id_ = 0;
}

bool Recorder::active() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return file_.is_open();
}

std::string Recorder::path() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return path_;
}

void Recorder::write(const Record& record) {
    if (!file_.is_open()) return;
    std::string label = record.label.substr(0, kMaxLabel);
    file_.put(static_cast<char>(record.type));
    file_.put(static_cast<char>(record.mode));
    put16(file_, static_cast<uint16_t>(label.size()));
    put32(file_, 0);
    put64(file_, record.timestamp_ms);
    put64(file_, record.match_id);
    put32(file_, static_cast<uint32_t>(record.confidence));
    put64(file_, record.elapsed_ms);
    for (int32_t metric : record.metrics) put32(file_, static_cast<uint32_t>(metric));
    file_.write(label.data(), static_cast<std::streamsize>(label.size()));
    file_.flush();
}

void Recorder::append_state(const telemetry::State& state) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!file_.is_open() || !state.active) return;
    uint64_t timestamp = now_ms();
    if (last_state_ms_ != 0 && timestamp - last_state_ms_ < 250 && state.match_id == last_match_id_)
        return;
    Record record;
    record.type = RecordType::State;
    record.mode = state.mode;
    record.timestamp_ms = timestamp;
    record.match_id = state.match_id;
    record.confidence = state.confidence;
    record.elapsed_ms = state.match_elapsed_ms;
    record.metrics = metrics(state);
    write(record);
    last_state_ms_ = timestamp;
    last_match_id_ = state.match_id;
}

void Recorder::append_marker(const telemetry::Marker& marker) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!file_.is_open()) return;
    Record record;
    record.type = RecordType::Marker;
    record.timestamp_ms = now_ms();
    record.match_id = marker.match_id;
    record.elapsed_ms = marker.offset_ms;
    record.label = marker.kind + ": " + marker.label;
    write(record);
}

void Recorder::append_summary(const telemetry::MatchSummary& summary) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!file_.is_open()) return;
    Record record;
    record.type = RecordType::Summary;
    record.mode = summary.final_state.mode;
    record.timestamp_ms = summary.ended_ms;
    record.match_id = summary.final_state.match_id;
    record.confidence = summary.final_state.confidence;
    record.elapsed_ms = summary.final_state.match_elapsed_ms;
    record.metrics = metrics(summary.final_state);
    record.label = summary.end_reason;
    write(record);
}

bool Player::open(const std::string& path) {
    close();
    file_.open(path, std::ios::binary);
    if (!file_.is_open()) return false;
    char magic[sizeof(kMagic) - 1]{};
    file_.read(magic, sizeof(magic));
    bool ok = file_.good() && std::equal(std::begin(magic), std::end(magic), kMagic);
    bool parse_ok = true;
    uint32_t version = get32(file_, parse_ok);
    (void)get32(file_, parse_ok);
    if (!ok || !parse_ok || version != kVersion) {
        close();
        return false;
    }
    open_ = true;
    return true;
}

void Player::close() {
    if (file_.is_open()) file_.close();
    open_ = false;
}

bool Player::next(Record& record) {
    if (!open_) return false;
    int type = file_.get();
    if (type == EOF) return false;
    int mode = file_.get();
    if (mode == EOF) {
        close();
        return false;
    }
    bool ok = true;
    uint16_t label_size = get16(file_, ok);
    (void)get32(file_, ok);
    record = {};
    record.type = static_cast<RecordType>(type);
    record.mode = static_cast<telemetry::Mode>(static_cast<int8_t>(mode));
    record.timestamp_ms = get64(file_, ok);
    record.match_id = get64(file_, ok);
    record.confidence = static_cast<int32_t>(get32(file_, ok));
    record.elapsed_ms = get64(file_, ok);
    for (auto& metric : record.metrics) metric = static_cast<int32_t>(get32(file_, ok));
    if (label_size > kMaxLabel) ok = false;
    if (ok) {
        record.label.resize(label_size);
        file_.read(record.label.data(), label_size);
        ok = file_.good();
    }
    if (!ok) {
        close();
        return false;
    }
    return true;
}

bool Player::is_open() const { return open_; }

Recorder& recorder() {
    static Recorder instance;
    return instance;
}

}  // namespace aml::replay
