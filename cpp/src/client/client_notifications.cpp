#include "client/client_notifications.h"
#include "client/client_core.h"
#include "imgui.h"

#include <windows.h>
#include <chrono>
#include <algorithm>
#include <cstdio>

namespace aml::client {

NotificationManager& NotificationManager::instance() {
    static NotificationManager mgr;
    return mgr;
}

void NotificationManager::push(const std::string& title, const std::string& body,
                                NotifyLevel level, std::function<void()> on_click) {
    std::lock_guard<std::mutex> lock(mu_);
    Notification n;
    n.title = title;
    n.body = body;
    n.level = level;
    n.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    n.on_click = std::move(on_click);
    notifications_.push_back(n);
}

void NotificationManager::push_toast(const std::string& title, const std::string& body,
                                      NotifyLevel level) {
    if (!settings().notifications_enabled) return;
    std::lock_guard<std::mutex> lock(mu_);
    Notification n;
    n.title = title;
    n.body = body;
    n.level = level;
    n.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    active_toasts_.push_back({n, 0.0f, 0.0f, 1.0f});
    if (static_cast<int>(active_toasts_.size()) > kMaxToasts) {
        active_toasts_.erase(active_toasts_.begin());
    }
}

std::vector<Notification> NotificationManager::get_all() const {
    std::lock_guard<std::mutex> lock(mu_);
    return notifications_;
}

std::vector<Notification> NotificationManager::get_unread() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<Notification> result;
    for (const auto& n : notifications_) {
        if (!n.read) result.push_back(n);
    }
    return result;
}

int NotificationManager::unread_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    int count = 0;
    for (const auto& n : notifications_) {
        if (!n.read) count++;
    }
    return count;
}

void NotificationManager::mark_read(int index) {
    std::lock_guard<std::mutex> lock(mu_);
    if (index >= 0 && index < static_cast<int>(notifications_.size())) {
        notifications_[index].read = true;
    }
}

void NotificationManager::mark_all_read() {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& n : notifications_) n.read = true;
}

void NotificationManager::clear() {
    std::lock_guard<std::mutex> lock(mu_);
    notifications_.clear();
    active_toasts_.clear();
}

static ImVec4 level_color(NotifyLevel level) {
    switch (level) {
        case NotifyLevel::Info: return ImVec4(0.4f, 0.6f, 1.0f, 1.0f);
        case NotifyLevel::Success: return theme().success;
        case NotifyLevel::Warning: return theme().warning;
        case NotifyLevel::Error: return theme().error;
    }
    return theme().text;
}

static const char* level_icon(NotifyLevel level) {
    switch (level) {
        case NotifyLevel::Info: return "[i]";
        case NotifyLevel::Success: return "[+]";
        case NotifyLevel::Warning: return "[!]";
        case NotifyLevel::Error: return "[X]";
    }
    return "";
}

void NotificationManager::render_toasts(float dt) {
    std::lock_guard<std::mutex> lock(mu_);

    ImGuiViewport* vp = ImGui::GetMainViewport();
    float toast_x = vp->Pos.x + vp->Size.x - 320.0f;
    float toast_y = vp->Pos.y + 60.0f;

    for (int i = static_cast<int>(active_toasts_.size()) - 1; i >= 0; --i) {
        auto& t = active_toasts_[i];
        t.age += dt;

        if (t.age > kToastDuration) {
            t.target_alpha = 0.0f;
        }
        if (t.age > kToastDuration + kToastFadeTime) {
            active_toasts_.erase(active_toasts_.begin() + i);
            continue;
        }

        if (t.target_alpha < t.alpha) {
            float fade = t.alpha - dt / kToastFadeTime;
            t.alpha = (fade > t.target_alpha) ? fade : t.target_alpha;
        }

        ImGui::SetNextWindowPos(ImVec2(toast_x, toast_y), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(300, 0), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.92f * t.alpha);

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                                  ImGuiWindowFlags_NoNav |
                                  ImGuiWindowFlags_NoMove |
                                  ImGuiWindowFlags_NoFocusOnAppearing |
                                  ImGuiWindowFlags_NoInputs;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 10));
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, t.alpha);

        char win_name[32];
        snprintf(win_name, sizeof(win_name), "##toast_%d", i);
        ImGui::Begin(win_name, nullptr, flags);

        ImVec4 col = level_color(t.notification.level);
        ImGui::TextColored(col, "%s %s", level_icon(t.notification.level), t.notification.title.c_str());
        if (!t.notification.body.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(theme().muted));
            ImGui::TextWrapped("%s", t.notification.body.c_str());
            ImGui::PopStyleColor();
        }

        ImGui::End();
        ImGui::PopStyleVar(3);

        toast_y += 60.0f;
    }
}

void NotificationManager::render_panel() {
    std::lock_guard<std::mutex> lock(mu_);

    ImGui::Text("Notifications");
    ImGui::Separator();

    if (ImGui::Button("Mark all read")) {
        for (auto& n : notifications_) n.read = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear all")) {
        notifications_.clear();
    }
    ImGui::Spacing();

    if (notifications_.empty()) {
        ImGui::TextDisabled("No notifications");
        return;
    }

    ImGui::BeginChild("##notif_list", ImVec2(0, 0), true);
    for (int i = static_cast<int>(notifications_.size()) - 1; i >= 0; --i) {
        const auto& n = notifications_[i];
        ImVec4 col = level_color(n.level);

        ImGui::PushID(i);
        if (!n.read) {
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            ImGui::Text("%s %s", level_icon(n.level), n.title.c_str());
            ImGui::PopStyleColor();
        } else {
            ImGui::TextDisabled("%s %s", level_icon(n.level), n.title.c_str());
        }
        if (!n.body.empty()) {
            ImGui::Indent();
            ImGui::TextDisabled("%s", n.body.c_str());
            ImGui::Unindent();
        }

        // Time ago
        int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        int64_t diff = now - n.timestamp;
        if (diff < 60) ImGui::TextDisabled("%llds ago", diff);
        else if (diff < 3600) ImGui::TextDisabled("%lldm ago", diff / 60);
        else ImGui::TextDisabled("%lldh ago", diff / 3600);

        ImGui::Spacing();
        ImGui::PopID();
    }
    ImGui::EndChild();
}

}  // namespace aml::client
