#include "ui.h"
#include "ui_internal.h"
#include "supabase.h"
#include "net.h"
#include "config.h"
#include "performance.h"
#include "sync_manager.h"

#include <windows.h>
#include <iphlpapi.h>
#include <dxgi.h>
#include <pdh.h>
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <vector>

#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "dxgi.lib")

namespace aml::ui {

float get_cpu_usage();
int get_cpu_core_count();
uint64_t get_memory_usage();
uint64_t get_total_memory();
uint64_t get_storage_usage();
uint64_t get_total_storage();
uint64_t get_download_speed();
uint64_t get_upload_speed();
uint64_t get_total_download();
uint64_t get_total_upload();
uint64_t get_process_uptime();
std::string get_gpu_name();
uint64_t get_gpu_memory();
bool clear_all_cache();
bool clear_mod_metadata_cache();
bool clear_download_cache();
bool clear_temp_files();
uint64_t get_cache_size();
int get_cache_item_count();
uint64_t get_mod_metadata_cache_size();
uint64_t get_instance_config_cache_size();
uint64_t get_download_cache_size();
uint64_t get_temp_file_cache_size();
std::string format_bytes_per_second(uint64_t bytes_per_second);
std::string format_duration(int64_t microseconds);

constexpr float kUnavailableCpu = -1.0f;
constexpr uint64_t kUnavailableMemory = std::numeric_limits<uint64_t>::max();

// ---------------------------------------------------------------------------
// Performance UI State
// ---------------------------------------------------------------------------

struct PerformanceUIState {
    int current_tab = 0; // 0=monitor, 1=cache, 2=offline, 3=settings
    
    // Monitor
    bool monitoring_enabled = true;
    int64_t last_update_time = 0;
    
    // Cache
    std::string cache_filter;
    bool cache_cleanup_open = false;
    
    // Offline
    bool offline_mode_enabled = false;
    std::string offline_sync_status;
    
    // Settings
    bool enable_hardware_acceleration = true;
    bool enable_preloading = true;
    int max_cache_size_gb = 5;
    int max_memory_usage_mb = 2048;
    bool enable_compression = true;
};

static PerformanceUIState& get_performance_ui_state() {
    static PerformanceUIState state;
    return state;
}

static void draw_performance_history(const std::vector<float>& values, const ImVec4& color) {
    const ImVec2 size(ImGui::GetContentRegionAvail().x, ui_px(42.0f));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, p + size, c32(k.surface2), ui_px(4.0f));
    if (values.size() > 1) {
        for (size_t i = 1; i < values.size(); ++i) {
            const float x0 = p.x + size.x * static_cast<float>(i - 1) / static_cast<float>(values.size() - 1);
            const float x1 = p.x + size.x * static_cast<float>(i) / static_cast<float>(values.size() - 1);
            const float y0 = p.y + size.y - size.y * std::clamp(values[i - 1], 0.0f, 100.0f) / 100.0f;
            const float y1 = p.y + size.y - size.y * std::clamp(values[i], 0.0f, 100.0f) / 100.0f;
            dl->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), c32(color), ui_px(2.0f));
        }
    }
    ImGui::Dummy(size);
}

void load_performance_settings(const config::Config& cfg) {
    auto& s = get_performance_ui_state();
    s.enable_hardware_acceleration = cfg.perf_hardware_acceleration;
    s.enable_preloading = cfg.perf_preloading;
    s.enable_compression = cfg.perf_compression;
    s.max_cache_size_gb = cfg.perf_max_cache_gb;
    s.max_memory_usage_mb = cfg.perf_max_memory_mb;
}

void save_performance_settings(config::Config& cfg) {
    const auto& s = get_performance_ui_state();
    cfg.perf_hardware_acceleration = s.enable_hardware_acceleration;
    cfg.perf_preloading = s.enable_preloading;
    cfg.perf_compression = s.enable_compression;
    cfg.perf_max_cache_gb = s.max_cache_size_gb;
    cfg.perf_max_memory_mb = s.max_memory_usage_mb;
}

// ---------------------------------------------------------------------------
// Performance Monitor Tab
// ---------------------------------------------------------------------------

void draw_performance_monitor(UiState& st) {
    auto& perf_ui = get_performance_ui_state();
    static std::vector<float> cpu_history;
    static std::vector<float> memory_history;
    static std::vector<float> storage_history;
    static uint64_t last_sample = 0;
    
    page_title("Performance Monitor", "Monitor system resources and launcher performance");

    {
        const float art_h = ui_px(104.0f);
        const ImVec2 art_pos = ImGui::GetCursorScreenPos();
        const ImVec2 art_size(ImGui::GetContentRegionAvail().x, art_h);
        draw_local_image(st, st.exe_dir + L"\\branding\\ai\\launcher-performance-ai.png",
                         art_pos, art_size, c32(k.brand_dk));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(art_pos, art_pos + art_size,
                          c32(ImVec4(0.02f, 0.01f, 0.06f, 0.42f)), ui_px(12.0f));
        dl->AddRect(art_pos, art_pos + art_size,
                    c32(ImVec4(k.brand.x, k.brand.y, k.brand.z, 0.32f)), ui_px(12.0f),
                    0, ui_px(1.0f));
        ImGui::SetCursorScreenPos(art_pos + ImVec2(ui_px(18.0f), ui_px(14.0f)));
        ImGui::PushFont(f_title);
        ImGui::TextColored(k.text, "See what your world is doing");
        ImGui::PopFont();
        ImGui::TextColored(k.muted, "Live system telemetry, cache health, and offline controls in one place.");
        ImGui::SetCursorScreenPos(ImVec2(art_pos.x, art_pos.y + art_h + ui_px(12.0f)));
    }
    
    card_begin("##performance_monitor_header");
    
    const float monitor_actions_width = ui_px(260.0f);
    const float monitor_header_available = ImGui::GetContentRegionAvail().x;
    const bool stack_monitor_actions = monitor_header_available < monitor_actions_width + ui_px(220.0f);
    if (!stack_monitor_actions)
        ImGui::SameLine(ImGui::GetCursorPosX() + monitor_header_available - monitor_actions_width);
    else
        ImGui::Spacing();
    if (ghost_button("Refresh", ImVec2(ui_px(100.0f), ui_px(32.0f)))) {
        perf_ui.last_update_time = std::chrono::system_clock::now().time_since_epoch().count();
    }
    
    if (!stack_monitor_actions) ImGui::SameLine();
    
    ImGui::Checkbox("Enable Monitoring", &perf_ui.monitoring_enabled);
    
    card_end();
    
    ImGui::Spacing();
    
    // System stats cards use explicit widths so four metrics never run off the
    // right edge on compact windows.
    const float metric_gap = ui_px(10.0f);
    const float metric_available = ImGui::GetContentRegionAvail().x;
    const int metric_columns = metric_available >= ui_px(1180.0f) ? 4 :
                               metric_available >= ui_px(680.0f) ? 2 : 1;
    const float metric_width = std::max(ui_px(180.0f),
        (metric_available - metric_gap * static_cast<float>(metric_columns - 1)) /
            static_cast<float>(metric_columns));
    int metric_index = 0;

    // System stats cards
    ImGui::BeginGroup();
    
    // CPU card
    card_begin("##performance_cpu", ImVec2(metric_width, 0));
    ImGui::TextUnformatted("CPU");
    ImGui::PushFont(f_title);
    
    const float cpu_usage = get_cpu_usage();
    const bool cpu_available = cpu_usage != kUnavailableCpu;
    const uint64_t now = GetTickCount64();
    const uint64_t memory_total_now = get_total_memory();
    const uint64_t memory_used_now = get_memory_usage();
    const bool memory_available = memory_total_now != kUnavailableMemory &&
                                  memory_used_now != kUnavailableMemory &&
                                  memory_total_now > 0 &&
                                  memory_used_now <= memory_total_now;
    const uint64_t storage_total_now = get_total_storage();
    const float memory_usage_now = memory_available
        ? static_cast<float>(memory_used_now) * 100.0f / static_cast<float>(memory_total_now) : 0.0f;
    const float storage_usage_now = storage_total_now
        ? static_cast<float>(get_storage_usage()) * 100.0f / static_cast<float>(storage_total_now) : 0.0f;
    if (perf_ui.monitoring_enabled && (last_sample == 0 || now - last_sample >= 1000)) {
        last_sample = now;
        if (cpu_available) cpu_history.push_back(cpu_usage);
        if (memory_available) memory_history.push_back(memory_usage_now);
        storage_history.push_back(storage_usage_now);
        if (cpu_history.size() > 60) cpu_history.erase(cpu_history.begin());
        if (memory_history.size() > 60) memory_history.erase(memory_history.begin());
        if (storage_history.size() > 60) storage_history.erase(storage_history.begin());
    }
    if (cpu_available) {
        ImGui::Text("%.1f%%", cpu_usage);
    } else {
        ImGui::TextColored(k.muted, "Unavailable");
    }
    ImGui::PopFont();
    
    // CPU progress bar
    if (cpu_available) {
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, k.brand);
        ImGui::ProgressBar(cpu_usage / 100.0f, ImVec2(-1, ui_px(8.0f)));
        ImGui::PopStyleColor();
    } else {
        ImGui::TextColored(k.muted, "CPU usage unavailable");
    }
    
    const int cpu_core_count = get_cpu_core_count();
    if (cpu_core_count > 0) {
        ImGui::TextColored(k.muted, "%d cores", cpu_core_count);
    } else {
        ImGui::TextColored(k.muted, "Cores: Unavailable");
    }
    card_end();
    
    ImGui::EndGroup();
    ++metric_index;
    if (metric_index % metric_columns) ImGui::SameLine(0, metric_gap);
    
    ImGui::BeginGroup();
    
    // Memory card
    card_begin("##performance_memory", ImVec2(metric_width, 0));
    ImGui::TextUnformatted("Memory");
    ImGui::PushFont(f_title);
    
    if (memory_available) {
        const float memory_percent = static_cast<float>(memory_used_now) /
            static_cast<float>(memory_total_now) * 100.0f;
        ImGui::Text("%.1f%%", memory_percent);
        ImGui::PopFont();
        
        // Memory progress bar
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, k.blue);
        ImGui::ProgressBar(memory_percent / 100.0f, ImVec2(-1, ui_px(8.0f)));
        ImGui::PopStyleColor();
        
        ImGui::TextColored(k.muted, "%s / %s", 
                         format_bytes(memory_used_now).c_str(),
                         format_bytes(memory_total_now).c_str());
    } else {
        ImGui::TextColored(k.muted, "Unavailable");
        ImGui::PopFont();
        ImGui::TextColored(k.muted, "Memory usage unavailable");
    }
    card_end();
    
    ImGui::EndGroup();
    ++metric_index;
    if (metric_index % metric_columns) ImGui::SameLine(0, metric_gap);
    
    ImGui::BeginGroup();
    
    // Storage card
    card_begin("##performance_storage", ImVec2(metric_width, 0));
    ImGui::TextUnformatted("Storage");
    ImGui::PushFont(f_title);
    
    uint64_t storage_used = get_storage_usage();
    uint64_t storage_total = get_total_storage();
    float storage_percent = storage_total > 0
        ? static_cast<float>(storage_used) / static_cast<float>(storage_total) * 100.0f
        : 0.0f;
    
    ImGui::Text("%.1f%%", storage_percent);
    ImGui::PopFont();
    
    // Storage progress bar
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, k.green);
    ImGui::ProgressBar(storage_percent / 100.0f, ImVec2(-1, ui_px(8.0f)));
    ImGui::PopStyleColor();
    
    ImGui::TextColored(k.muted, "%s / %s", 
                     format_bytes(storage_used).c_str(),
                     format_bytes(storage_total).c_str());
    card_end();
    
    ImGui::EndGroup();
    ++metric_index;
    if (metric_index % metric_columns) ImGui::SameLine(0, metric_gap);
    
    ImGui::BeginGroup();
    
    // Network card
    card_begin("##performance_network", ImVec2(metric_width, 0));
    ImGui::TextUnformatted("Network");
    ImGui::PushFont(f_title);
    
    uint64_t download_speed = get_download_speed();
    uint64_t upload_speed = get_upload_speed();
    
    ImGui::Text("↓ %s", format_bytes_per_second(download_speed).c_str());
    ImGui::PopFont();
    
    ImGui::TextColored(k.muted, "↑ %s", format_bytes_per_second(upload_speed).c_str());
    card_end();
    
    ImGui::EndGroup();
    
    ImGui::Spacing();
    
    // Detailed stats
    card_begin("##performance_detailed_stats");
    ImGui::TextUnformatted("Detailed Statistics");
    ImGui::Separator();
    ImGui::Spacing();
    
    // Process info
    ImGui::TextUnformatted("Process Information");
    ImGui::TextColored(k.muted, "Launcher PID: %d", GetCurrentProcessId());
    ImGui::TextColored(k.muted, "Uptime: %s", format_duration(get_process_uptime()).c_str());
    
    ImGui::Spacing();
    
    // GPU info
    ImGui::TextUnformatted("GPU Information");
    ImGui::TextColored(k.muted, "GPU: %s", get_gpu_name().c_str());
    ImGui::TextColored(k.muted, "VRAM: %s", format_bytes(get_gpu_memory()).c_str());
    
    ImGui::Spacing();
    
    // Network stats
    ImGui::TextUnformatted("Network Statistics");
    ImGui::TextColored(k.muted, "Total Download: %s", format_bytes(get_total_download()).c_str());
    ImGui::TextColored(k.muted, "Total Upload: %s", format_bytes(get_total_upload()).c_str());
    
    card_end();
    
    ImGui::Spacing();
    
    // Performance graph
    card_begin("##performance_graph");
    ImGui::TextUnformatted("Performance Graph");
    ImGui::Separator();
    ImGui::Spacing();
    
    ImGui::TextColored(k.muted, "CPU history");
    draw_performance_history(cpu_history, k.brand);
    ImGui::Spacing();
    ImGui::TextColored(k.muted, "Memory history");
    draw_performance_history(memory_history, k.blue);
    ImGui::Spacing();
    ImGui::TextColored(k.muted, "Storage history");
    draw_performance_history(storage_history, k.green);
    
    card_end();
}

// ---------------------------------------------------------------------------
// Cache Management Tab
// ---------------------------------------------------------------------------

void draw_performance_cache(UiState& st) {
    auto& perf_ui = get_performance_ui_state();
    
    page_title("Cache Management", "Manage cached data and optimize storage");
    
    card_begin("##performance_cache_header");
    
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - ui_px(200.0f));
    
    if (primary_button("Clear All Cache", ImVec2(ui_px(150.0f), ui_px(32.0f)))) {
        perf_ui.cache_cleanup_open = true;
        request_popup("##clear_cache_confirm");
    }
    
    card_end();
    
    ImGui::Spacing();
    
    // Clear cache confirmation popup
    if (ImGui::BeginPopup("##clear_cache_confirm")) {
        ImGui::TextUnformatted("Clear All Cache?");
        ImGui::Separator();
        ImGui::Spacing();
        
        ImGui::TextColored(k.muted, "This will remove all cached data including:");
        ImGui::TextColored(k.muted, "- Mod metadata and icons");
        ImGui::TextColored(k.muted, "- Instance configurations");
        ImGui::TextColored(k.muted, "- Download cache");
        ImGui::TextColored(k.muted, "- Temporary files");
        
        ImGui::Spacing();
        
        if (primary_button("Clear All", ImVec2(ui_px(100.0f), ui_px(32.0f)))) {
            // Clear all cache
            if (clear_all_cache()) {
                push_notice(st, ui_model::NoticeLevel::Success, "Cache Cleared",
                            "All cached data has been cleared");
            } else {
                push_notice(st, ui_model::NoticeLevel::Error, "Cache Clear Failed",
                            "Some cached data could not be removed.");
            }
            ImGui::CloseCurrentPopup();
        }
        
        ImGui::SameLine();
        
        if (ghost_button("Cancel", ImVec2(ui_px(100.0f), ui_px(32.0f)))) {
            ImGui::CloseCurrentPopup();
        }
        
        ImGui::EndPopup();
    }
    
    // Cache statistics
    card_begin("##performance_cache_stats");
    ImGui::TextUnformatted("Cache Statistics");
    ImGui::Separator();
    ImGui::Spacing();
    
    uint64_t cache_size = get_cache_size();
    int cache_item_count = get_cache_item_count();
    
    ImGui::TextUnformatted("Total Cache Size");
    ImGui::TextColored(k.muted, "%s", format_bytes(cache_size).c_str());
    
    ImGui::Spacing();
    
    ImGui::TextUnformatted("Cache Items");
    ImGui::TextColored(k.muted, "%d items", cache_item_count);
    
    ImGui::Spacing();
    
    // Cache breakdown
    ImGui::TextUnformatted("Cache Breakdown");
    ImGui::Separator();
    ImGui::Spacing();
    
    // In a real implementation, this would show a breakdown of cache usage
    ImGui::TextColored(k.muted, "Mod Metadata: %s", format_bytes(get_mod_metadata_cache_size()).c_str());
    ImGui::TextColored(k.muted, "Instance Configs: %s", format_bytes(get_instance_config_cache_size()).c_str());
    ImGui::TextColored(k.muted, "Download Cache: %s", format_bytes(get_download_cache_size()).c_str());
    ImGui::TextColored(k.muted, "Temporary Files: %s", format_bytes(get_temp_file_cache_size()).c_str());
    
    card_end();
    
    ImGui::Spacing();
    
    // Cache cleanup options
    card_begin("##performance_cache_cleanup");
    ImGui::TextUnformatted("Cache Cleanup Options");
    ImGui::Separator();
    ImGui::Spacing();
    
    if (ghost_button("Clear Mod Metadata Cache", ImVec2(ui_px(200.0f), ui_px(32.0f)))) {
        if (clear_mod_metadata_cache()) {
            push_notice(st, ui_model::NoticeLevel::Success, "Cache Cleared",
                        "Mod metadata cache has been cleared");
        } else {
            push_notice(st, ui_model::NoticeLevel::Error, "Cache Clear Failed",
                        "Mod metadata cache could not be removed.");
        }
    }
    
    ImGui::Spacing();
    
    if (ghost_button("Clear Download Cache", ImVec2(ui_px(200.0f), ui_px(32.0f)))) {
        if (clear_download_cache()) {
            push_notice(st, ui_model::NoticeLevel::Success, "Cache Cleared",
                        "Download cache has been cleared");
        } else {
            push_notice(st, ui_model::NoticeLevel::Error, "Cache Clear Failed",
                        "Download cache could not be removed.");
        }
    }
    
    ImGui::Spacing();
    
    if (ghost_button("Clear Temporary Files", ImVec2(ui_px(200.0f), ui_px(32.0f)))) {
        if (clear_temp_files()) {
            push_notice(st, ui_model::NoticeLevel::Success, "Cache Cleared",
                        "Temporary files have been cleared");
        } else {
            push_notice(st, ui_model::NoticeLevel::Error, "Cache Clear Failed",
                        "Temporary files could not be removed.");
        }
    }
    
    card_end();
}

// ---------------------------------------------------------------------------
// Offline Mode Tab
// ---------------------------------------------------------------------------

void draw_performance_offline(UiState& st) {
    auto& perf_ui = get_performance_ui_state();
    auto& sync = aml::sync::SyncManager::instance();
    
    page_title("Offline Mode", "Configure offline functionality and data synchronization");
    
    card_begin("##performance_offline_header");
    
    ImGui::Spacing();
    
    ImGui::Checkbox("Enable Offline Mode", &perf_ui.offline_mode_enabled);
    ImGui::TextColored(k.muted, "Enable offline mode to use the launcher without an internet connection");
    
    if (perf_ui.offline_mode_enabled) {
        ImGui::Spacing();
        ImGui::TextColored(k.yellow, "⚠ Some features will be limited while offline");
    }
    
    card_end();
    
    ImGui::Spacing();
    
    // Offline sync status
    card_begin("##performance_offline_sync");
    ImGui::TextUnformatted("Offline Data Synchronization");
    ImGui::Separator();
    ImGui::Spacing();
    
    auto sync_stats = sync.get_stats();
    
    ImGui::TextUnformatted("Last Sync");
    ImGui::TextColored(k.muted, "%s", 
                     sync_stats.last_sync_time > 0 ? 
                     format_date(sync_stats.last_sync_time).c_str() : "Never");
    
    ImGui::Spacing();
    
    if (ghost_button("Force Sync Now", ImVec2(ui_px(150.0f), ui_px(32.0f)))) {
        if (sync.perform_manual_sync()) {
            push_notice(st, ui_model::NoticeLevel::Success, "Sync Started",
                        "Forcing synchronization with server");
        } else {
            const std::string error = sync.get_last_error();
            push_notice(st, ui_model::NoticeLevel::Error, "Sync Failed",
                        error.empty() ? "Synchronization could not be completed." : error);
        }
    }
    
    card_end();
    
    ImGui::Spacing();
    
    // Offline data management
    card_begin("##performance_offline_data");
    ImGui::TextUnformatted("Offline Data Management");
    ImGui::Separator();
    ImGui::Spacing();
    
    ImGui::TextUnformatted("Cached Data");
    ImGui::TextColored(k.muted, "The following data is available offline:");
    
    ImGui::Spacing();
    
    ImGui::TextColored(k.muted, "- Installed mods and configurations");
    ImGui::TextColored(k.muted, "- Instance settings");
    ImGui::TextColored(k.muted, "- Launcher preferences");
    ImGui::TextColored(k.muted, "- Local profiles and worlds");
    
    ImGui::Spacing();
    
    ImGui::TextUnformatted("Data to Sync");
    ImGui::TextColored(k.muted, "The following data will sync when online:");
    
    ImGui::Spacing();
    
    ImGui::TextColored(k.muted, "- Mod updates and new versions");
    ImGui::TextColored(k.muted, "- Server status and player counts");
    ImGui::TextColored(k.muted, "- Social features (friends, messages)");
    ImGui::TextColored(k.muted, "- Cloud backups");
    
    card_end();
}

// ---------------------------------------------------------------------------
// Settings Tab
// ---------------------------------------------------------------------------

void draw_performance_settings(UiState& st) {
    auto& perf_ui = get_performance_ui_state();
    
    page_title("Performance Settings", "Configure performance-related options");
    
    card_begin("##performance_settings_general");
    ImGui::Separator();
    ImGui::Spacing();
    
    ImGui::Checkbox("Enable Hardware Acceleration", &perf_ui.enable_hardware_acceleration);
    ImGui::TextColored(k.muted, "Use hardware acceleration for UI rendering and other operations");
    
    ImGui::Spacing();
    
    ImGui::Checkbox("Enable Preloading", &perf_ui.enable_preloading);
    ImGui::TextColored(k.muted, "Preload frequently used data to improve performance");
    
    ImGui::Spacing();
    
    ImGui::Checkbox("Enable Compression", &perf_ui.enable_compression);
    ImGui::TextColored(k.muted, "Compress data to reduce memory usage and improve load times");
    
    card_end();
    
    ImGui::Spacing();
    
    card_begin("##performance_settings_cache");
    ImGui::TextUnformatted("Cache Settings");
    ImGui::Separator();
    ImGui::Spacing();
    
    ImGui::TextUnformatted("Maximum Cache Size");
    ImGui::SetNextItemWidth(ui_px(100.0f));
    ImGui::InputInt("##performance_max_cache_gb", &perf_ui.max_cache_size_gb);
    ImGui::SameLine();
    ImGui::TextColored(k.muted, "GB");
    ImGui::TextColored(k.muted, "Maximum size for cached data");
    
    ImGui::Spacing();
    
    ImGui::TextUnformatted("Maximum Memory Usage");
    ImGui::SetNextItemWidth(ui_px(100.0f));
    ImGui::InputInt("##performance_max_memory_mb", &perf_ui.max_memory_usage_mb);
    ImGui::SameLine();
    ImGui::TextColored(k.muted, "MB");
    ImGui::TextColored(k.muted, "Maximum memory usage for the launcher");
    
    card_end();
    
    ImGui::Spacing();
    
    card_begin("##performance_settings_actions");
    ImGui::TextUnformatted("Actions");
    ImGui::Separator();
    ImGui::Spacing();
    
    if (ghost_button("Save Settings", ImVec2(ui_px(150.0f), ui_px(36.0f)))) {
        save_performance_settings(*st.cfg);
        if (config::save(st.exe_dir + L"\\launcher.json", *st.cfg)) {
            push_notice(st, ui_model::NoticeLevel::Success, "Settings Saved",
                        "Performance settings have been saved");
        } else {
            push_notice(st, ui_model::NoticeLevel::Error, "Save Failed",
                        "Could not write launcher.json");
        }
    }
    
    ImGui::SameLine();
    
    if (ghost_button("Reset to Defaults", ImVec2(ui_px(150.0f), ui_px(36.0f)))) {
        // Reset to defaults
        perf_ui.enable_hardware_acceleration = true;
        perf_ui.enable_preloading = true;
        perf_ui.max_cache_size_gb = 5;
        perf_ui.max_memory_usage_mb = 2048;
        perf_ui.enable_compression = true;
    }
    
    card_end();
}

// ---------------------------------------------------------------------------
// Main Performance Page
// ---------------------------------------------------------------------------

void draw_performance_page(UiState& st) {
    auto& perf_ui = get_performance_ui_state();
    
    page_title("Performance", "Optimize launcher performance and manage resources");
    
    // Performance tabs
    if (ImGui::BeginTabBar("##performance_tabs")) {
    
    if (ImGui::BeginTabItem("Monitor")) {
        perf_ui.current_tab = 0;
        ImGui::EndTabItem();
    }
    
    if (ImGui::BeginTabItem("Cache")) {
        perf_ui.current_tab = 1;
        ImGui::EndTabItem();
    }
    
    if (ImGui::BeginTabItem("Offline")) {
        perf_ui.current_tab = 2;
        ImGui::EndTabItem();
    }
    
    if (ImGui::BeginTabItem("Settings")) {
        perf_ui.current_tab = 3;
        ImGui::EndTabItem();
    }
    
        ImGui::EndTabBar();
    }
    
    ImGui::Spacing();
    
    // Draw current tab
    switch (perf_ui.current_tab) {
        case 0:
        default:
            draw_performance_monitor(st);
            break;
        case 1:
            draw_performance_cache(st);
            break;
        case 2:
            draw_performance_offline(st);
            break;
        case 3:
            draw_performance_settings(st);
            break;
    }
}

// ---------------------------------------------------------------------------
// Helper Functions
// ---------------------------------------------------------------------------

float get_cpu_usage() {
    static PDH_HQUERY cpuQuery{};
    static PDH_HCOUNTER cpuTotal{};
    static bool initialized = false;
    if (!initialized) {
        if (PdhOpenQuery(nullptr, 0, &cpuQuery) != ERROR_SUCCESS)
            return kUnavailableCpu;
        if (PdhAddEnglishCounterW(cpuQuery, L"\\Processor(_Total)\\% Processor Time", 0,
                                  &cpuTotal) != ERROR_SUCCESS) {
            PdhCloseQuery(cpuQuery);
            cpuQuery = nullptr;
            return kUnavailableCpu;
        }
        initialized = true;
        if (PdhCollectQueryData(cpuQuery) != ERROR_SUCCESS)
            return kUnavailableCpu;
        return kUnavailableCpu;
    }
    if (PdhCollectQueryData(cpuQuery) != ERROR_SUCCESS)
        return kUnavailableCpu;
    PDH_FMT_COUNTERVALUE counterVal{};
    if (PdhGetFormattedCounterValue(cpuTotal, PDH_FMT_DOUBLE, nullptr, &counterVal) != ERROR_SUCCESS ||
        !std::isfinite(counterVal.doubleValue) || counterVal.doubleValue < 0.0) {
        return kUnavailableCpu;
    }
    return static_cast<float>(counterVal.doubleValue);
}

int get_cpu_core_count() {
    SYSTEM_INFO sys_info{};
    GetSystemInfo(&sys_info);
    return sys_info.dwNumberOfProcessors > 0
        ? static_cast<int>(sys_info.dwNumberOfProcessors) : 0;
}

uint64_t get_memory_usage() {
    MEMORYSTATUSEX mem_info{};
    mem_info.dwLength = sizeof(MEMORYSTATUSEX);
    if (!GlobalMemoryStatusEx(&mem_info) || mem_info.ullTotalPhys == 0 ||
        mem_info.ullAvailPhys > mem_info.ullTotalPhys)
        return kUnavailableMemory;
    return mem_info.ullTotalPhys - mem_info.ullAvailPhys;
}

uint64_t get_total_memory() {
    MEMORYSTATUSEX mem_info{};
    mem_info.dwLength = sizeof(MEMORYSTATUSEX);
    if (!GlobalMemoryStatusEx(&mem_info) || mem_info.ullTotalPhys == 0 ||
        mem_info.ullAvailPhys > mem_info.ullTotalPhys)
        return kUnavailableMemory;
    return mem_info.ullTotalPhys;
}

uint64_t get_storage_usage() {
    ULARGE_INTEGER free_bytes{}, total_bytes{};
    if (GetDiskFreeSpaceExW(L"C:\\", &free_bytes, &total_bytes, nullptr)) {
        return static_cast<uint64_t>(total_bytes.QuadPart - free_bytes.QuadPart);
    }
    return 0;
}

uint64_t get_total_storage() {
    ULARGE_INTEGER free_bytes{}, total_bytes{};
    if (GetDiskFreeSpaceExW(L"C:\\", &free_bytes, &total_bytes, nullptr)) {
        return static_cast<uint64_t>(total_bytes.QuadPart);
    }
    return 0;
}

struct NetworkSample {
    uint64_t downloaded = 0;
    uint64_t uploaded = 0;
    uint64_t download_speed = 0;
    uint64_t upload_speed = 0;
    ULONGLONG timestamp = 0;
};

static NetworkSample& network_sample() {
    static NetworkSample sample;
    const ULONGLONG now = GetTickCount64();
    if (sample.timestamp == 0 || now - sample.timestamp >= 250) {
        ULONG table_size = 0;
        GetIfTable(nullptr, &table_size, FALSE);
        std::vector<uint8_t> table_bytes(table_size);
        auto* table = reinterpret_cast<MIB_IFTABLE*>(table_bytes.data());
        if (table_size > 0 && GetIfTable(table, &table_size, FALSE) == NO_ERROR) {
            uint64_t downloaded = 0;
            uint64_t uploaded = 0;
            for (ULONG i = 0; i < table->dwNumEntries; ++i) {
                const MIB_IFROW& row = table->table[i];
                downloaded += row.dwInOctets;
                uploaded += row.dwOutOctets;
            }
            if (sample.timestamp != 0 && now > sample.timestamp) {
                const uint64_t elapsed_ms = now - sample.timestamp;
                sample.download_speed = downloaded >= sample.downloaded
                    ? (downloaded - sample.downloaded) * 1000 / elapsed_ms : 0;
                sample.upload_speed = uploaded >= sample.uploaded
                    ? (uploaded - sample.uploaded) * 1000 / elapsed_ms : 0;
            }
            sample.downloaded = downloaded;
            sample.uploaded = uploaded;
        }
        sample.timestamp = now;
    }
    return sample;
}

uint64_t get_download_speed() {
    return network_sample().download_speed;
}

uint64_t get_upload_speed() {
    return network_sample().upload_speed;
}

uint64_t get_total_download() {
    return network_sample().downloaded;
}

uint64_t get_total_upload() {
    return network_sample().uploaded;
}

uint64_t get_process_uptime() {
    FILETIME creation = {}, exit_time = {}, kernel = {}, user = {};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit_time, &kernel, &user)) return 0;
    FILETIME now = {};
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER started = {}, current = {};
    started.LowPart = creation.dwLowDateTime;
    started.HighPart = creation.dwHighDateTime;
    current.LowPart = now.dwLowDateTime;
    current.HighPart = now.dwHighDateTime;
    return current.QuadPart >= started.QuadPart
        ? (current.QuadPart - started.QuadPart) / 10 : 0;
}

std::string get_gpu_name() {
    DISPLAY_DEVICEW dd = {};
    dd.cb = sizeof(dd);
    for (int i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i) {
        if (dd.StateFlags & DISPLAY_DEVICE_ACTIVE) {
            std::wstring name = dd.DeviceString;
            if (name.find(L"Display") != std::wstring::npos || name.find(L"3D") != std::wstring::npos) {
                return net::to_utf8(name);
            }
        }
    }
    return "Unknown GPU";
}

uint64_t get_gpu_memory() {
    IDXGIFactory1* factory = nullptr;
    if (CreateDXGIFactory1(IID_PPV_ARGS(&factory)) != S_OK) return 0;
    uint64_t memory = 0;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc = {};
        if (adapter->GetDesc1(&desc) == S_OK && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
            memory = std::max<uint64_t>(memory, desc.DedicatedVideoMemory);
        adapter->Release();
        adapter = nullptr;
    }
    factory->Release();
    return memory;
}

bool clear_all_cache() {
    std::error_code ec;
    const auto temp = std::filesystem::temp_directory_path(ec);
    if (ec) return false;
    bool ok = true;
    std::filesystem::remove_all(temp / "amalgam_cache", ec);
    if (ec) ok = false;
    ec.clear();
    std::filesystem::remove_all(temp / "amalgam", ec);
    if (ec) ok = false;
    return ok;
}

bool clear_mod_metadata_cache() {
    std::error_code ec;
    const auto temp = std::filesystem::temp_directory_path(ec);
    if (ec) return false;
    std::filesystem::remove_all(temp / "amalgam_cache" / "mods", ec);
    return !ec;
}

bool clear_download_cache() {
    std::error_code ec;
    const auto temp = std::filesystem::temp_directory_path(ec);
    if (ec) return false;
    std::filesystem::remove_all(temp / "amalgam_cache" / "downloads", ec);
    return !ec;
}

bool clear_temp_files() {
    std::error_code ec;
    const auto temp = std::filesystem::temp_directory_path(ec);
    if (ec) return false;
    std::filesystem::remove_all(temp / "amalgam", ec);
    return !ec;
}

static uint64_t dir_size(const std::filesystem::path& dir) {
    uint64_t total = 0;
    std::error_code ec;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, ec)) {
        if (ec) break;
        if (entry.is_regular_file(ec)) {
            total += entry.file_size(ec);
        }
    }
    return total;
}

uint64_t get_cache_size() {
    return dir_size(std::filesystem::temp_directory_path() / "amalgam_cache") +
           dir_size(std::filesystem::temp_directory_path() / "amalgam");
}

int get_cache_item_count() {
    int count = 0;
    std::error_code ec;
    auto cache_dir = std::filesystem::temp_directory_path() / "amalgam_cache";
    if (std::filesystem::exists(cache_dir)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(cache_dir, ec)) {
            if (ec) break;
            if (entry.is_regular_file(ec)) count++;
        }
    }
    auto temp_dir = std::filesystem::temp_directory_path() / "amalgam";
    if (std::filesystem::exists(temp_dir)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(temp_dir, ec)) {
            if (ec) break;
            if (entry.is_regular_file(ec)) count++;
        }
    }
    return count;
}

uint64_t get_mod_metadata_cache_size() {
    return dir_size(std::filesystem::temp_directory_path() / "amalgam_cache" / "mods");
}

uint64_t get_instance_config_cache_size() {
    return dir_size(std::filesystem::temp_directory_path() / "amalgam_cache" / "configs");
}

uint64_t get_download_cache_size() {
    return dir_size(std::filesystem::temp_directory_path() / "amalgam_cache" / "downloads");
}

uint64_t get_temp_file_cache_size() {
    return dir_size(std::filesystem::temp_directory_path() / "amalgam");
}

std::string format_bytes_per_second(uint64_t bytes_per_second) {
    const char* units[] = {"B/s", "KB/s", "MB/s", "GB/s"};
    int unit_index = 0;
    double speed = static_cast<double>(bytes_per_second);
    
    while (speed >= 1024 && unit_index < 3) {
        speed /= 1024;
        unit_index++;
    }
    
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%.2f %s", speed, units[unit_index]);
    return std::string(buffer);
}

std::string format_date(int64_t timestamp) {
    if (timestamp <= 0) {
        return "Never";
    }
    
    std::time_t time = static_cast<std::time_t>(timestamp);
    std::tm tm = *std::localtime(&time);
    
    char buffer[64];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &tm);
    return std::string(buffer);
}

std::string format_duration(int64_t microseconds) {
    int64_t seconds = microseconds / 1000000;
    int64_t minutes = seconds / 60;
    int64_t hours = minutes / 60;
    
    seconds %= 60;
    minutes %= 60;
    
    if (hours > 0) {
        return std::to_string(hours) + "h " + 
               std::to_string(minutes) + "m " + 
               std::to_string(seconds) + "s";
    } else if (minutes > 0) {
        return std::to_string(minutes) + "m " + 
               std::to_string(seconds) + "s";
    } else {
        return std::to_string(seconds) + "s";
    }
}

}  // namespace aml::ui
