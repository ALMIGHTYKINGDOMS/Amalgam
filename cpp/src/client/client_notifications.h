#pragma once

#include <string>
#include <vector>
#include <functional>
#include <cstdint>
#include <mutex>

namespace aml::client {

enum class NotifyLevel {
    Info,
    Success,
    Warning,
    Error
};

struct Notification {
    std::string title;
    std::string body;
    NotifyLevel level = NotifyLevel::Info;
    int64_t timestamp = 0;
    bool read = false;
    std::function<void()> on_click;
};

class NotificationManager {
public:
    static NotificationManager& instance();

    void push(const std::string& title, const std::string& body,
              NotifyLevel level = NotifyLevel::Info,
              std::function<void()> on_click = nullptr);

    void push_toast(const std::string& title, const std::string& body,
                    NotifyLevel level = NotifyLevel::Info);

    std::vector<Notification> get_all() const;
    std::vector<Notification> get_unread() const;
    int unread_count() const;

    void mark_read(int index);
    void mark_all_read();
    void clear();

    void render_toasts(float dt);
    void render_panel();

private:
    NotificationManager() = default;

    mutable std::mutex mu_;
    std::vector<Notification> notifications_;

    struct Toast {
        Notification notification;
        float age = 0.0f;
        float alpha = 1.0f;
        float target_alpha = 1.0f;
    };
    std::vector<Toast> active_toasts_;
    static constexpr float kToastDuration = 5.0f;
    static constexpr float kToastFadeTime = 0.5f;
    static constexpr int kMaxToasts = 5;
};

}  // namespace aml::client
