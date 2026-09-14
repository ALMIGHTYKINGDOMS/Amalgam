#include "ui_internal.h"

#include <algorithm>

namespace aml::ui {

void enable_per_monitor_dpi_awareness() {
    using SetDpiAwareness = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    using SetThreadDpiAwareness = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
    const auto set_thread_awareness = reinterpret_cast<SetThreadDpiAwareness>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext"));
    if (set_thread_awareness &&
        set_thread_awareness(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != nullptr)
        return;
    const auto set_awareness = reinterpret_cast<SetDpiAwareness>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
    if (set_awareness && set_awareness(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return;
    SetProcessDPIAware();
}

float dpi_scale(UINT dpi) {
    return std::clamp(static_cast<float>(dpi) / 96.0f, 0.75f, 3.0f);
}

float initial_dpi_scale() {
    using GetDpiForSystemFn = UINT(WINAPI*)();
    const auto get_dpi = reinterpret_cast<GetDpiForSystemFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForSystem"));
    const UINT dpi = get_dpi ? get_dpi() : 96u;
    return dpi_scale(dpi);
}

float visible_window_width(HWND hwnd) {
    // ImGui lays out in viewport pixels after DPI scaling.  Reading the raw
    // Win32 window rectangle here mixes physical pixels with ImGui pixels and
    // makes the sidebar/main split too wide on scaled displays.  Use the
    // viewport's work area so every page receives the same coordinate space.
    (void)hwnd;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    return viewport ? viewport->WorkSize.x : 0.0f;
}

void set_next_adaptive_window(float preferred_width, float preferred_height,
                              float minimum_width, float minimum_height,
                              ImGuiCond condition) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float margin = ui_px(24.0f);
    const float max_width = std::max(1.0f, viewport->WorkSize.x - margin * 2.0f);
    const float max_height = std::max(1.0f, viewport->WorkSize.y - margin * 2.0f);
    const float min_width = std::min(ui_px(minimum_width), max_width);
    const float min_height = std::min(ui_px(minimum_height), max_height);
    const float width = std::min(ui_px(preferred_width), max_width);
    const float height = preferred_height > 0.0f ? std::min(ui_px(preferred_height), max_height) : 0.0f;
    ImGui::SetNextWindowSizeConstraints(ImVec2(min_width, min_height),
                                        ImVec2(max_width, max_height));
    ImGui::SetNextWindowSize(ImVec2(std::max(min_width, width), height), condition);
}

void set_next_adaptive_right_panel(float preferred_width, float preferred_height,
                                   float minimum_width) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float margin = ui_px(12.0f);
    const float max_width = std::max(1.0f, viewport->WorkSize.x - margin * 2.0f);
    const float width = std::min(ui_px(preferred_width), max_width);
    const float top = viewport->WorkPos.y + ui_px(76.0f);
    const float max_height = std::max(1.0f,
                                      viewport->WorkPos.y + viewport->WorkSize.y - top - margin);
    const float min_width = std::min(ui_px(minimum_width), max_width);
    const float min_height = std::min(ui_px(180.0f), max_height);
    const float height = std::min(ui_px(preferred_height), max_height);
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - width - margin,
                                   top), ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(ImVec2(min_width, min_height),
                                        ImVec2(max_width, max_height));
    ImGui::SetNextWindowSize(ImVec2(std::max(min_width, width),
                                    std::max(min_height, height)), ImGuiCond_Always);
}

}  // namespace aml::ui
