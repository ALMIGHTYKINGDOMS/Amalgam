#pragma once

#include <cstddef>
#include <mutex>
#include <string>

namespace aml::telemetry {
struct State;
}

namespace aml::diagnostics {

class Outbox {
public:
    Outbox();
    ~Outbox();
    bool set_enabled(bool enabled);
    bool enabled() const;
    void set_path(const std::wstring& path);

    bool enqueue(const telemetry::State& state);
    size_t pending() const;
    bool export_csv(const std::wstring& path) const;
    bool delete_all();

private:
    bool open_locked();
    void close_locked();
    bool exec_locked(const char* sql) const;

    mutable std::mutex mutex_;
    void* db_ = nullptr;
    std::wstring path_;
    bool enabled_ = false;
};

Outbox& outbox();

}  // namespace aml::diagnostics
