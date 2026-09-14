#pragma once

#include <array>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>

namespace aml::telemetry {
struct State;
struct Marker;
struct MatchSummary;
enum class Mode : int8_t;
}

namespace aml::replay {

enum class RecordType : uint8_t { State = 1, Marker = 2, Summary = 3 };

struct Record {
    RecordType type = RecordType::State;
    telemetry::Mode mode{};
    uint64_t timestamp_ms = 0;
    uint64_t match_id = 0;
    int confidence = 0;
    uint64_t elapsed_ms = 0;
    std::array<int32_t, 16> metrics{};
    std::string label;
};

class Recorder {
public:
    bool start(const std::string& path);
    void stop();
    bool active() const;
    std::string path() const;

    void append_state(const telemetry::State& state);
    void append_marker(const telemetry::Marker& marker);
    void append_summary(const telemetry::MatchSummary& summary);

private:
    void stop_locked();
    void write(const Record& record);

    mutable std::mutex mutex_;
    std::ofstream file_;
    std::string path_;
    uint64_t last_state_ms_ = 0;
    uint64_t last_match_id_ = 0;
};

class Player {
public:
    bool open(const std::string& path);
    void close();
    bool next(Record& record);
    bool is_open() const;

private:
    std::ifstream file_;
    bool open_ = false;
};

Recorder& recorder();

}  // namespace aml::replay
