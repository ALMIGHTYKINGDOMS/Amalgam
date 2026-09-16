#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aml::ui_model {

struct NavigationRoute {
    int sidebar_item = 0;
    int active_tab = 0;
};

// The Play section has its own routes. Keeping this mapping in the model
// makes it testable and prevents Java Edition from silently falling back to
// the Library surface when the sidebar is rearranged.
inline constexpr NavigationRoute java_edition_route() {
    return {11, 11};
}

// Rendering code consumes these states instead of inferring state from several
// unrelated booleans. The model is deliberately ImGui-free so it can be tested
// without creating a window.
enum class ScreenPhase { Idle, Loading, Ready, Empty, Error };

template <typename T>
struct ScreenState {
    ScreenPhase phase = ScreenPhase::Idle;
    uint64_t request_id = 0;
    T value{};
    std::string error;
    bool retryable = false;
    bool stale = false;

    bool loading() const { return phase == ScreenPhase::Loading; }
    bool ready() const { return phase == ScreenPhase::Ready; }
    bool empty() const { return phase == ScreenPhase::Empty; }
    bool failed() const { return phase == ScreenPhase::Error; }

    void begin(uint64_t id) {
        request_id = id;
        phase = ScreenPhase::Loading;
        error.clear();
        retryable = false;
        stale = false;
    }

    bool accept(uint64_t id, T next, bool has_value = true) {
        if (id != request_id) return false;
        value = std::move(next);
        phase = has_value ? ScreenPhase::Ready : ScreenPhase::Empty;
        error.clear();
        retryable = false;
        stale = false;
        return true;
    }

    bool fail(uint64_t id, std::string message, bool can_retry = true) {
        if (id != request_id) return false;
        phase = ScreenPhase::Error;
        error = std::move(message);
        retryable = can_retry;
        stale = false;
        return true;
    }
};

enum class NoticeLevel { Info, Success, Warning, Error };

struct Notice {
    uint64_t id = 0;
    NoticeLevel level = NoticeLevel::Info;
    std::string title;
    std::string body;
    std::string action_label;
    std::string action_id;
    bool sticky = false;
    bool dismissed = false;
};

enum class OperationState {
    Queued,
    Running,
    Paused,
    Cancelling,
    Completed,
    Failed,
    Cancelled,
};

struct OperationSnapshot {
    int id = 0;
    std::string kind;
    std::string label;
    std::string detail;
    std::string phase;
    std::string current_item;
    std::string target_profile;
    std::string provider;
    OperationState state = OperationState::Queued;
    float progress = 0.0f;
    uint64_t bytes_done = 0;
    uint64_t bytes_total = 0;
    double bytes_per_second = 0.0;
    double eta_seconds = -1.0;
    uint64_t created_at = 0;
    uint64_t started_at = 0;
    uint64_t finished_at = 0;
    bool resumable = false;
    bool retryable = false;
    bool cancel_requested = false;
    std::string error;
};

inline float fraction(uint64_t done, uint64_t total, float fallback = 0.0f) {
    if (total == 0) return std::clamp(fallback, 0.0f, 1.0f);
    return std::clamp(static_cast<float>(static_cast<double>(done) /
                                          static_cast<double>(total)), 0.0f, 1.0f);
}

// These small, allocation-free helpers keep search and progress behavior
// deterministic in both the renderer and ImGui-free regression tests.
inline bool contains_case_insensitive(std::string_view text, std::string_view query) {
    if (query.empty()) return true;
    if (query.size() > text.size()) return false;
    for (size_t start = 0; start + query.size() <= text.size(); ++start) {
        bool matches = true;
        for (size_t offset = 0; offset < query.size(); ++offset) {
            const unsigned char left = static_cast<unsigned char>(text[start + offset]);
            const unsigned char right = static_cast<unsigned char>(query[offset]);
            if (std::tolower(left) != std::tolower(right)) {
                matches = false;
                break;
            }
        }
        if (matches) return true;
    }
    return false;
}

inline int clamp_selection(int selected, size_t item_count) {
    if (item_count == 0) return 0;
    return std::clamp(selected, 0, static_cast<int>(item_count - 1));
}

inline float normalize_progress(float progress) {
    if (!std::isfinite(progress)) return progress < 0.0f ? -1.0f : 0.0f;
    if (progress < 0.0f) return -1.0f; // negative means indeterminate
    return std::clamp(progress, 0.0f, 1.0f);
}

inline const char* operation_state_name(OperationState state) {
    switch (state) {
        case OperationState::Queued: return "Queued";
        case OperationState::Running: return "Running";
        case OperationState::Paused: return "Paused";
        case OperationState::Cancelling: return "Cancelling";
        case OperationState::Completed: return "Completed";
        case OperationState::Failed: return "Failed";
        case OperationState::Cancelled: return "Cancelled";
    }
    return "Unknown";
}

enum class ImageFit { Cover, Contain, ContainMark };

// branding/amalgam-logo.png is a stacked lockup: the mark fills the top 74% of
// the 1332x1181 artwork and the wordmark the remainder. Fitting the whole
// lockup into a square tile renders that wordmark a few pixels tall, where it
// reads as a second, garbled "AMALGAM" beside the header's own text, so
// Contain fits whole artwork and ContainMark fits the mark band only.
constexpr float kBrandMarkBand = 0.74f;

// Image fitting is a model concern rather than a rendering concern. Keeping
// it here makes the logo/banner/card behavior deterministic and testable at
// every DPI scale without constructing an ImGui draw list.
struct ImagePlacement {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float uv_min_x = 0.0f;
    float uv_min_y = 0.0f;
    float uv_max_x = 1.0f;
    float uv_max_y = 1.0f;
};

inline ImagePlacement place_image(float x, float y, float width, float height,
                                  int source_width, int source_height, ImageFit fit) {
    ImagePlacement placement{x, y, width, height};
    if (source_width <= 0 || source_height <= 0 || width <= 0.0f || height <= 0.0f)
        return placement;
    // ContainMark fits only the source's top band, so the band's own aspect
    // decides the placement and the band's edge becomes the texture's V limit.
    float visible_height = 1.0f;
    if (fit == ImageFit::ContainMark) {
        visible_height = kBrandMarkBand;
        placement.uv_max_y = visible_height;
    }
    const float source_aspect = static_cast<float>(source_width) /
                                (static_cast<float>(source_height) * visible_height);
    const float target_aspect = width / height;
    if (fit != ImageFit::Cover) {
        if (source_aspect > target_aspect) {
            placement.height = width / source_aspect;
            placement.y += (height - placement.height) * 0.5f;
        } else {
            placement.width = height * source_aspect;
            placement.x += (width - placement.width) * 0.5f;
        }
    } else if (source_aspect > target_aspect) {
        const float visible = std::clamp(target_aspect / source_aspect, 0.0f, 1.0f);
        placement.uv_min_x = (1.0f - visible) * 0.5f;
        placement.uv_max_x = placement.uv_min_x + visible;
    } else {
        const float visible = std::clamp(source_aspect / target_aspect, 0.0f, 1.0f);
        placement.uv_min_y = (1.0f - visible) * 0.5f;
        placement.uv_max_y = placement.uv_min_y + visible;
    }
    return placement;
}

inline float topbar_action_width(bool compact, float ui_scale, float account_width) {
    const float scale = std::max(0.75f, ui_scale);
    const float icon_width = 36.0f * scale;
    const float window_width = 34.0f * scale;
    const float gap = 6.0f * scale;
    return icon_width * 3.0f + gap * 2.0f + account_width +
        (compact ? gap : gap + 10.0f * scale + window_width * 3.0f + 4.0f * scale);
}

inline float topbar_action_start(float content_left, float content_width,
                                 float viewport_left, float viewport_width,
                                 float action_width, float ui_scale) {
    const float scale = std::max(0.75f, ui_scale);
    const float safe_right = std::min(content_left + std::max(0.0f, content_width),
        viewport_left + std::max(0.0f, viewport_width) - 12.0f * scale);
    return std::max(content_left + 12.0f * scale, safe_right - action_width);
}

// Keep breakpoint decisions in the testable UI model rather than scattering
// magic widths throughout the renderer. Values are physical pixels, matching
// the per-monitor-DPI shell used by the launcher.
inline bool discover_uses_side_detail(float available_width, float ui_scale = 1.0f) {
    return available_width >= 980.0f * std::max(0.75f, ui_scale);
}

inline int featured_card_columns(float available_width, float ui_scale = 1.0f) {
    const float scale = std::max(0.75f, ui_scale);
    if (available_width >= 1220.0f * scale) return 5;
    if (available_width >= 900.0f * scale) return 4;
    if (available_width >= 640.0f * scale) return 3;
    return 2;
}

// Shared breakpoints for metric cards. Keeping this decision in the model
// prevents account, cloud, and settings pages from independently overflowing
// at narrow widths or high DPI.
inline int stat_card_columns(float available_width, float ui_scale = 1.0f) {
    const float scale = std::max(0.75f, ui_scale);
    if (available_width >= 920.0f * scale) return 4;
    if (available_width >= 560.0f * scale) return 2;
    return 1;
}

// Essentials uses three information columns when there is room, and stacked
// scrollable panels when the content area is narrow. This keeps actions and
// presence details readable instead of shrinking them into unusable slivers.
inline bool essentials_uses_stacked_columns(float available_width,
                                            float ui_scale = 1.0f) {
    return available_width < 900.0f * std::max(0.75f, ui_scale);
}

inline float shell_status_height(bool diagnostics_open, float work_height,
                                 float ui_scale = 1.0f) {
    const float scale = std::max(0.75f, ui_scale);
    if (!diagnostics_open) return 64.0f * scale;
    return std::clamp(work_height * 0.36f, 220.0f * scale, 360.0f * scale);
}

}  // namespace aml::ui_model
