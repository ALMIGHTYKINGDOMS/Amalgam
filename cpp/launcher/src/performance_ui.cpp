#include "ui.h"
#include "ui_internal.h"
#include "supabase.h"
#include "net.h"
#include "config.h"
#include "performance.h"
#include "performance_cache.h"
#include "sync_manager.h"
#include "ui_async_request.h"

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
#include <exception>
#include <limits>
#include <thread>
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
std::string format_bytes_per_second(uint64_t bytes_per_second);
std::string format_duration(int64_t microseconds);

constexpr float kUnavailableCpu = -1.0f;
constexpr uint64_t kUnavailableMemory = std::numeric_limits<uint64_t>::max();

constexpr char kCacheScanAction[] = "performance-cache-scan";
constexpr char kCacheCleanupAction[] = "performance-cache-cleanup";
constexpr char kOfflineManualSyncAction[] = "performance-offline-manual-sync";
constexpr char kGpuProbeAction[] = "performance-gpu-probe";

struct CacheUsageDisplay {
    bool has_value = false;
    bool stale = false;
    uint64_t total_bytes = 0;
    uint64_t item_count = 0;
    uint64_t mod_metadata_bytes = 0;
    uint64_t instance_config_bytes = 0;
    uint64_t download_bytes = 0;
    uint64_t temporary_bytes = 0;
    uint64_t other_launcher_bytes = 0;
    std::string warning;
};

// GPU adapter discovery can create a DXGI factory and enumerate every adapter.
// Keep its last completed snapshot on the render thread instead of repeating
// that driver-facing work for every monitor frame.
struct GpuInfoDisplay {
    bool has_value = false;
    bool stale = false;
    std::string name;
    uint64_t memory_bytes = 0;
    std::string warning;
};

// ---------------------------------------------------------------------------
// Performance UI State
// ---------------------------------------------------------------------------

struct PerformanceUIState {
    int current_tab = 0; // 0=monitor, 1=cache, 2=offline, 3=settings
    
    // Monitor
    bool monitoring_enabled = true;
    int64_t last_update_time = 0;
    GpuInfoDisplay gpu_info;
    bool gpu_probe_attempted = false;
    AsyncUiRequestState gpu_probe_request;
    
    // Cache
    std::string cache_filter;
    bool cache_cleanup_open = false;
    // Cache scans and cleanup both run as joined, single-flight workers.  The
    // display cache itself is touched only by the render thread after a
    // completed hand-off is consumed, so it needs no additional lock.
    CacheUsageDisplay cache_usage;
    bool cache_scan_attempted = false;
    std::string active_cache_cleanup_label;
    AsyncUiRequestState cache_scan_request;
    AsyncUiRequestState cache_cleanup_request;
    
    // Offline
    bool offline_mode_enabled = false;
    std::string offline_sync_status;
    // Manual sync may perform both authenticated service reads and local
    // profile reconciliation, so it gets its own joined hand-off lane.
    AsyncUiRequestState offline_sync_request;
    
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

static const char* cache_scope_label(performance_cache::ClearScope scope) {
    switch (scope) {
        case performance_cache::ClearScope::All: return "all cached data";
        case performance_cache::ClearScope::ModMetadata: return "mod metadata cache";
        case performance_cache::ClearScope::Downloads: return "download cache";
        case performance_cache::ClearScope::TemporaryFiles: return "temporary files";
    }
    return "cached data";
}

static const char* cache_scope_success_detail(performance_cache::ClearScope scope) {
    switch (scope) {
        case performance_cache::ClearScope::All: return "All cached data has been cleared.";
        case performance_cache::ClearScope::ModMetadata: return "Mod metadata cache has been cleared.";
        case performance_cache::ClearScope::Downloads: return "Download cache has been cleared.";
        case performance_cache::ClearScope::TemporaryFiles: return "Temporary files have been cleared.";
    }
    return "Cached data has been cleared.";
}

static int64_t cache_counter_for_async(uint64_t value) {
    const uint64_t maximum = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    return static_cast<int64_t>(std::min(value, maximum));
}

static uint64_t cache_counter_from_async(const AsyncUiRequestResult& result, size_t index) {
    if (index >= result.numbers.size() || result.numbers[index] < 0) return 0;
    return static_cast<uint64_t>(result.numbers[index]);
}

static bool start_cache_usage_scan(UiState& st, PerformanceUIState& perf_ui) {
    if (st.fixture_mode || async_ui_request_is_reserved(perf_ui.cache_scan_request)) return false;

    uint64_t generation = 0;
    if (!begin_async_ui_request(perf_ui.cache_scan_request, kCacheScanAction, &generation)) return false;

    perf_ui.cache_scan_attempted = true;
    perf_ui.cache_usage.stale = perf_ui.cache_usage.has_value;
    spawn_worker(st, std::thread([&perf_ui, generation]() {
        AsyncUiRequestResult result;
        try {
            performance_cache::CacheRoots roots;
            std::string root_error;
            performance_cache::CacheUsage usage;
            if (!performance_cache::default_cache_roots(roots, &root_error)) {
                usage.complete = false;
                usage.error = root_error.empty()
                    ? "The launcher cache location could not be determined."
                    : root_error;
                result.success = false;
            } else {
                usage = performance_cache::scan_cache_usage(roots);
                result.success = true;
            }
            result.warning = !usage.complete;
            result.title = usage.complete ? "Cache usage refreshed" : "Cache usage partially refreshed";
            result.detail = usage.error;
            result.number_a = cache_counter_for_async(usage.total_bytes);
            result.numbers = {
                cache_counter_for_async(usage.item_count),
                cache_counter_for_async(usage.mod_metadata_bytes),
                cache_counter_for_async(usage.instance_config_bytes),
                cache_counter_for_async(usage.download_bytes),
                cache_counter_for_async(usage.temporary_bytes),
                cache_counter_for_async(usage.other_launcher_bytes),
            };
        } catch (const std::exception&) {
            result.success = false;
            result.title = "Cache usage unavailable";
            result.detail = "Cache usage could not be refreshed. Try again.";
        } catch (...) {
            result.success = false;
            result.title = "Cache usage unavailable";
            result.detail = "Cache usage could not be refreshed. Try again.";
        }
        complete_async_ui_request(perf_ui.cache_scan_request, kCacheScanAction,
                                  generation, std::move(result));
    }));
    return true;
}

static bool start_cache_cleanup(UiState& st, PerformanceUIState& perf_ui,
                                performance_cache::ClearScope scope) {
    if (st.fixture_mode || async_ui_request_is_reserved(perf_ui.cache_cleanup_request)) return false;

    uint64_t generation = 0;
    if (!begin_async_ui_request(perf_ui.cache_cleanup_request, kCacheCleanupAction, &generation)) return false;

    perf_ui.active_cache_cleanup_label = cache_scope_label(scope);
    perf_ui.cache_usage.stale = perf_ui.cache_usage.has_value;
    spawn_worker(st, std::thread([&perf_ui, generation, scope]() {
        AsyncUiRequestResult result;
        try {
            performance_cache::CacheRoots roots;
            std::string root_error;
            performance_cache::ClearResult cleanup;
            if (!performance_cache::default_cache_roots(roots, &root_error)) {
                cleanup.success = false;
                cleanup.error = root_error.empty()
                    ? "The launcher cache location could not be determined."
                    : root_error;
            } else {
                cleanup = performance_cache::clear_cache(roots, scope);
            }
            result.success = cleanup.success;
            result.title = cleanup.success ? "Cache Cleared" : "Cache Clear Failed";
            result.detail = cleanup.success ? cache_scope_success_detail(scope)
                                            : (cleanup.error.empty()
                                                ? "Some cached data could not be removed."
                                                : cleanup.error);
            result.payload_a = cache_scope_label(scope);
        } catch (const std::exception&) {
            result.success = false;
            result.title = "Cache Clear Failed";
            result.detail = "Some cached data could not be removed.";
        } catch (...) {
            result.success = false;
            result.title = "Cache Clear Failed";
            result.detail = "Some cached data could not be removed.";
        }
        complete_async_ui_request(perf_ui.cache_cleanup_request, kCacheCleanupAction,
                                  generation, std::move(result));
    }));
    return true;
}

static void consume_cache_async_results(UiState& st, PerformanceUIState& perf_ui) {
    AsyncUiRequestSnapshot completed;
    if (take_async_ui_request_result(perf_ui.cache_scan_request, &completed)) {
        const AsyncUiRequestResult& result = completed.result;
        if (result.success) {
            perf_ui.cache_usage.has_value = true;
            perf_ui.cache_usage.stale = false;
            perf_ui.cache_usage.total_bytes = result.number_a < 0
                ? 0 : static_cast<uint64_t>(result.number_a);
            perf_ui.cache_usage.item_count = cache_counter_from_async(result, 0);
            perf_ui.cache_usage.mod_metadata_bytes = cache_counter_from_async(result, 1);
            perf_ui.cache_usage.instance_config_bytes = cache_counter_from_async(result, 2);
            perf_ui.cache_usage.download_bytes = cache_counter_from_async(result, 3);
            perf_ui.cache_usage.temporary_bytes = cache_counter_from_async(result, 4);
            perf_ui.cache_usage.other_launcher_bytes = cache_counter_from_async(result, 5);
            perf_ui.cache_usage.warning = result.warning ? result.detail : "";
        } else {
            perf_ui.cache_usage.stale = perf_ui.cache_usage.has_value;
            perf_ui.cache_usage.warning = result.detail.empty()
                ? "Cache usage could not be refreshed. Try again."
                : result.detail;
        }
    }

    if (take_async_ui_request_result(perf_ui.cache_cleanup_request, &completed)) {
        perf_ui.active_cache_cleanup_label.clear();
        const AsyncUiRequestResult& result = completed.result;
        if (result.success) {
            push_notice(st, ui_model::NoticeLevel::Success, result.title, result.detail);
            perf_ui.cache_usage.stale = perf_ui.cache_usage.has_value;
            perf_ui.cache_usage.warning.clear();
            perf_ui.cache_scan_attempted = false;
            start_cache_usage_scan(st, perf_ui);
        } else {
            push_notice(st, ui_model::NoticeLevel::Error, result.title, result.detail);
            perf_ui.cache_usage.stale = perf_ui.cache_usage.has_value;
            perf_ui.cache_usage.warning = result.detail;
        }
    }
}

static bool start_offline_manual_sync(UiState& st, PerformanceUIState& perf_ui) {
    if (st.fixture_mode || async_ui_request_is_reserved(perf_ui.offline_sync_request)) return false;

    auto& sync = aml::sync::SyncManager::instance();
    if (sync.get_stats().is_syncing) {
        perf_ui.offline_sync_status = "A synchronization is already in progress.";
        return false;
    }

    uint64_t generation = 0;
    if (!begin_async_ui_request(perf_ui.offline_sync_request, kOfflineManualSyncAction,
                                &generation)) {
        return false;
    }

    perf_ui.offline_sync_status = "Synchronizing local and account-backed data...";
    spawn_worker(st, std::thread([&perf_ui, generation]() {
        AsyncUiRequestResult result;
        try {
            auto& sync = aml::sync::SyncManager::instance();
            result.success = sync.perform_sync(aml::sync::SyncType::Manual);
            result.title = result.success ? "Sync Complete" : "Sync Failed";
            if (result.success) {
                result.detail = "Local and account-backed data is up to date.";
            } else {
                const auto stats = sync.get_stats();
                result.detail = stats.is_syncing
                    ? "Another synchronization is already in progress."
                    : "The launcher could not synchronize offline data. Check your connection and sign in again.";
            }
        } catch (const std::exception&) {
            result.success = false;
            result.title = "Sync Failed";
            result.detail = "Synchronization could not be completed. Try again when you are online.";
        } catch (...) {
            result.success = false;
            result.title = "Sync Failed";
            result.detail = "Synchronization could not be completed. Try again when you are online.";
        }
        complete_async_ui_request(perf_ui.offline_sync_request, kOfflineManualSyncAction,
                                  generation, std::move(result));
    }));
    return true;
}

static void consume_offline_manual_sync_result(UiState& st, PerformanceUIState& perf_ui) {
    AsyncUiRequestSnapshot completed;
    if (!take_async_ui_request_result(perf_ui.offline_sync_request, &completed)) return;

    const AsyncUiRequestResult& result = completed.result;
    perf_ui.offline_sync_status = result.detail;
    push_notice(st, result.success ? ui_model::NoticeLevel::Success : ui_model::NoticeLevel::Error,
                result.title, result.detail);
}

static bool start_gpu_probe(UiState& st, PerformanceUIState& perf_ui) {
    if (st.fixture_mode || async_ui_request_is_reserved(perf_ui.gpu_probe_request)) return false;

    uint64_t generation = 0;
    if (!begin_async_ui_request(perf_ui.gpu_probe_request, kGpuProbeAction, &generation)) {
        return false;
    }

    perf_ui.gpu_probe_attempted = true;
    perf_ui.gpu_info.stale = perf_ui.gpu_info.has_value;
    spawn_worker(st, std::thread([&perf_ui, generation]() {
        AsyncUiRequestResult result;
        try {
            result.payload_a = get_gpu_name();
            result.number_a = cache_counter_for_async(get_gpu_memory());
            result.success = !result.payload_a.empty() &&
                             (result.payload_a != "Unknown GPU" || result.number_a > 0);
            result.title = result.success ? "GPU information refreshed"
                                          : "GPU information unavailable";
            result.detail = result.success ? ""
                                          : "The graphics adapter could not be identified. Try refreshing the monitor.";
        } catch (const std::exception&) {
            result.success = false;
            result.title = "GPU information unavailable";
            result.detail = "The graphics adapter could not be identified. Try refreshing the monitor.";
        } catch (...) {
            result.success = false;
            result.title = "GPU information unavailable";
            result.detail = "The graphics adapter could not be identified. Try refreshing the monitor.";
        }
        complete_async_ui_request(perf_ui.gpu_probe_request, kGpuProbeAction,
                                  generation, std::move(result));
    }));
    return true;
}

static void consume_gpu_probe_result(PerformanceUIState& perf_ui) {
    AsyncUiRequestSnapshot completed;
    if (!take_async_ui_request_result(perf_ui.gpu_probe_request, &completed)) return;

    const AsyncUiRequestResult& result = completed.result;
    if (result.success) {
        perf_ui.gpu_info.has_value = true;
        perf_ui.gpu_info.stale = false;
        perf_ui.gpu_info.name = result.payload_a;
        perf_ui.gpu_info.memory_bytes = result.number_a < 0
            ? 0 : static_cast<uint64_t>(result.number_a);
        perf_ui.gpu_info.warning.clear();
    } else {
        perf_ui.gpu_info.stale = perf_ui.gpu_info.has_value;
        perf_ui.gpu_info.warning = result.detail.empty()
            ? "The graphics adapter could not be identified. Try refreshing the monitor."
            : result.detail;
    }
}

// Keep a retryable adapter failure close to the Refresh control.  The detailed
// statistics card is useful context, but it can sit below the metric grid on a
// compact window; a failed background probe should never require scrolling to
// discover what needs attention.
static void draw_gpu_probe_status(const AsyncUiRequestSnapshot& gpu_probe,
                                  const GpuInfoDisplay& gpu_info) {
    const bool loading_without_prior_value = gpu_probe.working && !gpu_info.has_value;
    const bool retryable_failure = !gpu_probe.working && !gpu_info.warning.empty();
    if (!loading_without_prior_value && !retryable_failure) return;

    card_begin("##performance_gpu_probe_status");
    if (loading_without_prior_value) {
        ImGui::TextColored(k.brand_hov, "Checking graphics adapter");
        ImGui::TextColored(k.muted,
                           "Graphics information is loading in the background. You can keep browsing.");
    } else {
        ImGui::TextColored(k.red, "GPU information unavailable");
        ImGui::TextWrapped("%s", gpu_info.warning.c_str());
        ImGui::TextColored(k.muted, "%s", gpu_info.has_value
            ? "Showing the last completed graphics reading. Use Refresh to try again."
            : "Use Refresh to try again.");
    }
    card_end();
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

    // Consume a completed background probe before painting the monitor. The
    // worker is joined by UiState during shutdown, so it cannot outlive this
    // render-owned display cache.
    consume_gpu_probe_result(perf_ui);
    if (!perf_ui.gpu_probe_attempted &&
        !async_ui_request_is_reserved(perf_ui.gpu_probe_request)) {
        start_gpu_probe(st, perf_ui);
    }
    const AsyncUiRequestSnapshot gpu_probe =
        snapshot_async_ui_request(perf_ui.gpu_probe_request);
    
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
    if (ghost_button(gpu_probe.working ? "Refreshing..." : "Refresh",
                     ImVec2(ui_px(100.0f), ui_px(32.0f)), gpu_probe.working)) {
        perf_ui.last_update_time = std::chrono::system_clock::now().time_since_epoch().count();
        start_gpu_probe(st, perf_ui);
    }
    
    if (!stack_monitor_actions) ImGui::SameLine();
    
    ImGui::Checkbox("Enable Monitoring", &perf_ui.monitoring_enabled);
    
    card_end();
    draw_gpu_probe_status(gpu_probe, perf_ui.gpu_info);

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
    if (gpu_probe.working && !perf_ui.gpu_info.has_value) {
        ImGui::TextColored(k.muted, "GPU: Detecting graphics adapter…");
        ImGui::TextColored(k.muted, "VRAM: Waiting for adapter details");
    } else if (perf_ui.gpu_info.has_value) {
        ImGui::TextColored(k.muted, "GPU: %s", perf_ui.gpu_info.name.c_str());
        ImGui::TextColored(k.muted, "VRAM: %s", format_bytes(perf_ui.gpu_info.memory_bytes).c_str());
        if (gpu_probe.working) {
            ImGui::TextColored(k.brand_hov, "Refreshing graphics information in the background…");
        } else if (perf_ui.gpu_info.stale) {
            ImGui::TextColored(k.yellow,
                               "Showing the last completed graphics reading. Use Refresh to try again.");
        }
    } else {
        ImGui::TextColored(k.muted, "GPU: Unavailable");
        ImGui::TextColored(k.muted, "%s", perf_ui.gpu_info.warning.empty()
            ? "The graphics adapter could not be identified. Use Refresh to try again."
            : perf_ui.gpu_info.warning.c_str());
    }
    
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
    consume_cache_async_results(st, perf_ui);

    AsyncUiRequestSnapshot scan_request = snapshot_async_ui_request(perf_ui.cache_scan_request);
    AsyncUiRequestSnapshot cleanup_request = snapshot_async_ui_request(perf_ui.cache_cleanup_request);
    if (!perf_ui.cache_usage.has_value && !perf_ui.cache_scan_attempted &&
        !scan_request.working && !cleanup_request.working) {
        start_cache_usage_scan(st, perf_ui);
        scan_request = snapshot_async_ui_request(perf_ui.cache_scan_request);
    }

    const bool cache_busy = scan_request.working || cleanup_request.working;
    const bool cleanup_available = !cache_busy && !st.fixture_mode;

    page_title("Cache Management", "Manage cached data and optimize storage");

    card_begin("##performance_cache_header");
    ImGui::PushFont(f_bold);
    ImGui::TextUnformatted("Local cache");
    ImGui::PopFont();
    ImGui::SameLine();
    if (ghost_button("Refresh usage", ImVec2(ui_px(126.0f), ui_px(30.0f)), cache_busy || st.fixture_mode)) {
        start_cache_usage_scan(st, perf_ui);
    }
    ImGui::SameLine();
    const char* clear_all_label = cleanup_request.working ? "Cleaning cache..." : "Clear All Cache";
    if (primary_button(clear_all_label, ImVec2(ui_px(150.0f), ui_px(30.0f)),
                       cleanup_request.working, !cleanup_available)) {
        perf_ui.cache_cleanup_open = true;
        request_popup("##clear_cache_confirm");
    }
    if (cleanup_request.working) {
        ImGui::TextColored(k.brand_hov, "Cleaning %s in the background. You can keep browsing.",
                           perf_ui.active_cache_cleanup_label.empty()
                               ? "cached data" : perf_ui.active_cache_cleanup_label.c_str());
    } else if (scan_request.working) {
        ImGui::TextColored(k.muted, "Refreshing local cache usage in the background.");
    } else {
        ImGui::TextColored(k.muted,
                           "Usage is measured in the background so this page remains responsive.");
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
        
        if (primary_button("Clear All", ImVec2(ui_px(100.0f), ui_px(32.0f)), false,
                           !cleanup_available)) {
            if (start_cache_cleanup(st, perf_ui, performance_cache::ClearScope::All)) {
                perf_ui.cache_cleanup_open = false;
                ImGui::CloseCurrentPopup();
            }
        }

        ImGui::SameLine();
        if (ghost_button("Cancel", ImVec2(ui_px(100.0f), ui_px(32.0f)))) {
            perf_ui.cache_cleanup_open = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    // Cache statistics
    card_begin("##performance_cache_stats");
    ImGui::TextUnformatted("Cache Statistics");
    ImGui::Separator();
    ImGui::Spacing();

    if (!perf_ui.cache_usage.has_value) {
        ImGui::TextColored(k.muted, "%s", scan_request.working
            ? "Calculating local cache usage..."
            : "Cache usage is unavailable.");
        if (!perf_ui.cache_usage.warning.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(k.red, "%s", perf_ui.cache_usage.warning.c_str());
        }
    } else {
        ImGui::TextUnformatted("Total Cache Size");
        ImGui::TextColored(k.muted, "%s", format_bytes(perf_ui.cache_usage.total_bytes).c_str());

        ImGui::Spacing();

        ImGui::TextUnformatted("Cache Items");
        ImGui::TextColored(k.muted, "%llu items",
                           static_cast<unsigned long long>(perf_ui.cache_usage.item_count));

        ImGui::Spacing();

        ImGui::TextUnformatted("Cache Breakdown");
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextColored(k.muted, "Mod Metadata: %s",
                           format_bytes(perf_ui.cache_usage.mod_metadata_bytes).c_str());
        ImGui::TextColored(k.muted, "Instance Configs: %s",
                           format_bytes(perf_ui.cache_usage.instance_config_bytes).c_str());
        ImGui::TextColored(k.muted, "Download Cache: %s",
                           format_bytes(perf_ui.cache_usage.download_bytes).c_str());
        ImGui::TextColored(k.muted, "Temporary Files: %s",
                           format_bytes(perf_ui.cache_usage.temporary_bytes).c_str());
        if (perf_ui.cache_usage.other_launcher_bytes > 0) {
            ImGui::TextColored(k.muted, "Other Launcher Cache: %s",
                               format_bytes(perf_ui.cache_usage.other_launcher_bytes).c_str());
        }

        if (perf_ui.cache_usage.stale) {
            ImGui::Spacing();
            ImGui::TextColored(k.yellow, "Showing the last measured cache usage while it refreshes.");
        }
        if (!perf_ui.cache_usage.warning.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(k.yellow, "%s", perf_ui.cache_usage.warning.c_str());
        }
    }

    card_end();

    ImGui::Spacing();

    // Cache cleanup options
    card_begin("##performance_cache_cleanup");
    ImGui::TextUnformatted("Cache Cleanup Options");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextColored(k.muted, "Cleanup runs in the background and keeps the launcher usable.");
    ImGui::Spacing();

    if (ghost_button("Clear Mod Metadata Cache", ImVec2(ui_px(200.0f), ui_px(32.0f)),
                     !cleanup_available)) {
        start_cache_cleanup(st, perf_ui, performance_cache::ClearScope::ModMetadata);
    }

    ImGui::Spacing();

    if (ghost_button("Clear Download Cache", ImVec2(ui_px(200.0f), ui_px(32.0f)),
                     !cleanup_available)) {
        start_cache_cleanup(st, perf_ui, performance_cache::ClearScope::Downloads);
    }

    ImGui::Spacing();

    if (ghost_button("Clear Temporary Files", ImVec2(ui_px(200.0f), ui_px(32.0f)),
                     !cleanup_available)) {
        start_cache_cleanup(st, perf_ui, performance_cache::ClearScope::TemporaryFiles);
    }

    card_end();
}

// ---------------------------------------------------------------------------
// Offline Mode Tab
// ---------------------------------------------------------------------------

void draw_performance_offline(UiState& st) {
    auto& perf_ui = get_performance_ui_state();
    auto& sync = aml::sync::SyncManager::instance();
    consume_offline_manual_sync_result(st, perf_ui);

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
    
    const auto sync_stats = sync.get_stats();
    const AsyncUiRequestSnapshot manual_sync_request =
        snapshot_async_ui_request(perf_ui.offline_sync_request);
    const bool sync_busy = manual_sync_request.working || sync_stats.is_syncing;

    ImGui::TextUnformatted("Last Sync");
    ImGui::TextColored(k.muted, "%s", 
                     sync_stats.last_sync_time > 0 ? 
                     format_date(sync_stats.last_sync_time).c_str() : "Never");

    ImGui::Spacing();

    const char* sync_button_label = manual_sync_request.working ? "Syncing..." : "Force Sync Now";
    if (ghost_button(sync_button_label, ImVec2(ui_px(150.0f), ui_px(32.0f)),
                     sync_busy || st.fixture_mode)) {
        start_offline_manual_sync(st, perf_ui);
    }
    if (manual_sync_request.working) {
        ImGui::SameLine();
        ImGui::TextColored(k.brand_hov,
                           "Syncing in the background. You can keep browsing.");
    } else if (sync_stats.is_syncing) {
        ImGui::SameLine();
        ImGui::TextColored(k.muted, "Another synchronization is already in progress.");
    } else if (!perf_ui.offline_sync_status.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(sync_stats.is_online ? k.muted : k.yellow, "%s",
                           perf_ui.offline_sync_status.c_str());
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

// Visual-review captures should describe the performance UI without exposing a
// review machine's CPU, memory, disks, GPU, network activity, cache contents,
// or sync state.  Keep every fixture tab local and inert rather than calling
// the live monitor/cache/synchronization paths below.
static void draw_fixture_performance_metric(const char* id, const char* label,
                                            const char* value, const char* detail,
                                            float progress, const ImVec4& color) {
    card_begin(id, ImVec2(0, 0));
    ImGui::TextUnformatted(label);
    ImGui::PushFont(f_title);
    ImGui::TextUnformatted(value);
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, color);
    ImGui::ProgressBar(progress, ImVec2(-1, ui_px(8.0f)));
    ImGui::PopStyleColor();
    ImGui::TextColored(k.muted, "%s", detail);
    card_end();
}

// The live confirmation starts a serialized background cleanup, so its fixture
// counterpart is intentionally a self-contained static preview. It never
// enumerates or deletes cache files even if a reviewer clicks around the
// rendered dialog.
static void draw_fixture_clear_cache_confirmation(UiState& st) {
    if (st.fixture_case != "performance-clear-cache-confirm") return;

    static std::string dismissed_fixture_case;
    if (!dismissed_fixture_case.empty() && dismissed_fixture_case != st.fixture_case)
        dismissed_fixture_case.clear();
    if (dismissed_fixture_case == st.fixture_case) return;

    constexpr const char* kPopupId = "Clear All Cache##fixture_performance";
    ImGui::OpenPopup(kPopupId);
    ImGui::SetNextWindowSizeConstraints(ImVec2(ui_px(360.0f), 0.0f),
                                        ImVec2(ui_px(520.0f), ui_px(680.0f)));
    bool open = true;
    if (ImGui::BeginPopupModal(kPopupId, &open, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("Clear all cache?");
        ImGui::PopFont();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextColored(k.brand,
                           "LOCAL VISUAL-QA FIXTURE — no cache path is read or removed.");
        ImGui::TextColored(k.muted, "The normal cleanup can remove cached:");
        ImGui::BulletText("mod metadata and icons");
        ImGui::BulletText("download cache and temporary files");
        ImGui::BulletText("representative launcher cache indexes");
        ImGui::Spacing();
        ImGui::TextWrapped("This preview keeps the destructive control disabled so its hierarchy and recovery wording can be reviewed safely.");
        ImGui::Spacing();
        danger_button("Clear all", ImVec2(ui_px(108.0f), ui_px(32.0f)), true);
        ImGui::SameLine();
        if (ghost_button("Close preview", ImVec2(ui_px(118.0f), ui_px(32.0f)))) {
            dismissed_fixture_case = st.fixture_case;
            open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (!open) dismissed_fixture_case = st.fixture_case;
}

static void draw_fixture_performance_page(UiState& st, int tab) {
    page_title("Performance", "Representative local health and tuning state");
    ImGui::TextColored(k.brand_hov,
                       "Visual fixture: representative local data only; no host telemetry, cache, or sync service is read.");
    ImGui::Spacing();

    const char* tabs[] = {"Monitor", "Cache", "Offline", "Settings"};
    if (ImGui::BeginTabBar("##fixture_performance_tabs")) {
        for (int i = 0; i < 4; ++i) {
            if (ImGui::BeginTabItem(tabs[i], nullptr,
                                    i == tab ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None))
                ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::Spacing();

    switch (tab) {
        case 1: {
            page_title("Cache Management", "Representative local cache health");
            card_begin("##fixture_performance_cache_summary");
            const bool loading = st.fixture_case == "performance-cache-loading";
            const bool cleaning = st.fixture_case == "performance-cache-cleanup-working";
            const bool error = st.fixture_case == "performance-cache-error";
            if (loading) {
                ImGui::TextColored(k.brand_hov, "REFRESHING CACHE USAGE");
                ImGui::TextColored(k.muted,
                                   "Calculating local cache usage in the background. You can keep browsing.");
                ImGui::Spacing();
                draw_skeleton_rect(ImGui::GetCursorScreenPos(),
                                   ImVec2(ImGui::GetContentRegionAvail().x, ui_px(18.0f)), ui_px(5.0f));
                ImGui::Dummy(ImVec2(0, ui_px(24.0f)));
                draw_skeleton_rect(ImGui::GetCursorScreenPos(),
                                   ImVec2(ImGui::GetContentRegionAvail().x * 0.62f, ui_px(14.0f)), ui_px(5.0f));
                ImGui::Dummy(ImVec2(0, ui_px(26.0f)));
                ghost_button("Refreshing...", ImVec2(ui_px(150.0f), ui_px(32.0f)), true);
            } else if (cleaning) {
                ImGui::TextColored(k.brand_hov, "CLEANING DOWNLOAD CACHE");
                ImGui::TextColored(k.muted,
                                   "Cleanup runs in the background. Controls stay disabled until it finishes.");
                ImGui::Spacing();
                ImGui::TextColored(k.text, "826 MB");
                ImGui::TextColored(k.yellow, "Showing the last measured cache usage while cleanup completes.");
                ImGui::Spacing();
                primary_button("Cleaning cache...", ImVec2(ui_px(150.0f), ui_px(32.0f)), true, true);
            } else if (error) {
                ImGui::TextColored(k.red, "CACHE USAGE COULDN'T BE REFRESHED");
                ImGui::TextColored(k.muted,
                                   "The last measured local cache usage remains available. Try again when the device is ready.");
                ImGui::Spacing();
                ImGui::TextColored(k.text, "826 MB");
                ImGui::TextColored(k.muted, "184 representative cached items");
                ImGui::Spacing();
                ghost_button("Try Again", ImVec2(ui_px(110.0f), ui_px(32.0f)), true);
            } else {
                ImGui::TextUnformatted("Cache Statistics");
                ImGui::Separator();
                ImGui::TextColored(k.text, "826 MB");
                ImGui::TextColored(k.muted, "184 representative cached items");
                ImGui::Spacing();
                ImGui::TextColored(k.muted, "Mod metadata     412 MB");
                ImGui::TextColored(k.muted, "Instance configs  96 MB");
                ImGui::TextColored(k.muted, "Downloads        287 MB");
                ImGui::TextColored(k.muted, "Temporary files   31 MB");
                ImGui::Spacing();
                primary_button("Clear all cache", ImVec2(ui_px(150.0f), ui_px(32.0f)), false, true);
                ImGui::SameLine();
                ghost_button("Review cleanup", ImVec2(ui_px(150.0f), ui_px(32.0f)), true);
            }
            card_end();
            break;
        }
        case 2: {
            page_title("Offline Mode", "Representative availability and sync posture");
            card_begin("##fixture_performance_offline_status");
            const bool syncing = st.fixture_case == "performance-offline-syncing";
            const bool complete = st.fixture_case == "performance-offline-sync-complete";
            const bool error = st.fixture_case == "performance-offline-sync-error";
            if (syncing) {
                ImGui::TextColored(k.brand_hov, "SYNCING OFFLINE DATA");
                ImGui::TextColored(k.muted,
                                   "Local and account-backed data is synchronizing in the background. You can keep browsing.");
                ImGui::Spacing();
                primary_button("Syncing...", ImVec2(ui_px(130.0f), ui_px(32.0f)), true, true);
            } else if (complete) {
                ImGui::TextColored(k.green, "OFFLINE DATA SYNCHRONIZED");
                ImGui::TextColored(k.muted, "Representative sync complete just now.");
                ImGui::TextColored(k.muted, "Profile settings, installed content, and worlds stay available locally.");
                ImGui::Spacing();
                primary_button("Sync now", ImVec2(ui_px(110.0f), ui_px(32.0f)), false, true);
            } else if (error) {
                ImGui::TextColored(k.red, "SYNC DIDN'T COMPLETE");
                ImGui::TextColored(k.muted,
                                   "Your existing local profiles and worlds remain available. Retry when you are online.");
                ImGui::Spacing();
                ghost_button("Retry sync", ImVec2(ui_px(110.0f), ui_px(32.0f)), true);
            } else {
                ImGui::TextColored(k.green, "READY FOR OFFLINE USE");
                ImGui::TextColored(k.muted, "Last representative sync: today at 10:42 AM");
                ImGui::TextColored(k.muted, "Profile settings, installed content, and worlds stay available locally.");
                ImGui::Spacing();
                bool offline = true;
                ImGui::BeginDisabled();
                ImGui::Checkbox("Enable Offline Mode", &offline);
                ImGui::EndDisabled();
                primary_button("Sync now", ImVec2(ui_px(110.0f), ui_px(32.0f)), false, true);
            }
            card_end();
            ImGui::Spacing();
            card_begin("##fixture_performance_offline_scope");
            ImGui::TextUnformatted("Available without a connection");
            ImGui::BulletText("Prepared profiles and installed content");
            ImGui::BulletText("Local launcher preferences and worlds");
            ImGui::BulletText("Saved troubleshooting information");
            ImGui::TextColored(k.muted, "Social features and cloud backups reconnect when you are online.");
            card_end();
            break;
        }
        case 3: {
            page_title("Performance Settings", "Representative local tuning options");
            card_begin("##fixture_performance_settings_general");
            ImGui::TextUnformatted("Runtime choices");
            ImGui::Separator();
            ImGui::BeginDisabled();
            bool enabled = true;
            ImGui::Checkbox("Enable Hardware Acceleration", &enabled);
            ImGui::Checkbox("Preload common launcher data", &enabled);
            ImGui::Checkbox("Enable Compression", &enabled);
            ImGui::EndDisabled();
            ImGui::TextColored(k.muted, "Settings are disabled in visual fixtures and are never written to launcher configuration.");
            card_end();
            ImGui::Spacing();
            card_begin("##fixture_performance_settings_limits");
            ImGui::TextUnformatted("Representative limits");
            ImGui::TextColored(k.muted, "Cache limit: 5 GB");
            ImGui::TextColored(k.muted, "Launcher memory budget: 2 GB");
            card_end();
            break;
        }
        case 0:
        default: {
            page_title("Performance Monitor", "Representative local telemetry snapshot");
            const bool gpu_loading = st.fixture_case == "performance-monitor-gpu-loading";
            const bool gpu_error = st.fixture_case == "performance-monitor-gpu-error";
            if (gpu_loading || gpu_error) {
                card_begin("##fixture_performance_gpu_probe_status");
                if (gpu_loading) {
                    ImGui::TextColored(k.brand_hov, "CHECKING GRAPHICS ADAPTER");
                    ImGui::TextColored(k.muted,
                                       "Graphics information is loading in the background. You can keep browsing.");
                } else {
                    ImGui::TextColored(k.red, "GPU INFORMATION UNAVAILABLE");
                    ImGui::TextColored(k.muted,
                                       "The graphics adapter could not be identified. Use Refresh to try again.");
                    ImGui::TextColored(k.muted,
                                       "Visual fixture: the Refresh control is disabled for this capture.");
                }
                card_end();
                ImGui::Spacing();
            }
            const float available = ImGui::GetContentRegionAvail().x;
            const bool two_columns = available >= ui_px(560.0f);
            if (two_columns) ImGui::Columns(2, "##fixture_performance_metrics", false);
            draw_fixture_performance_metric("##fixture_perf_cpu", "CPU", "28%", "12 representative cores", 0.28f, k.brand);
            if (two_columns) ImGui::NextColumn();
            draw_fixture_performance_metric("##fixture_perf_memory", "Memory", "46%", "7.4 GB of 16.0 GB", 0.46f, k.blue);
            if (two_columns) ImGui::NextColumn();
            draw_fixture_performance_metric("##fixture_perf_storage", "Storage", "38%", "190 GB of 500 GB", 0.38f, k.green);
            if (two_columns) ImGui::NextColumn();
            draw_fixture_performance_metric("##fixture_perf_network", "Network", "1.2 MB/s", "0.3 MB/s upload", 0.30f, k.orange);
            if (two_columns) ImGui::Columns(1);
            ImGui::Spacing();
            card_begin("##fixture_performance_monitor_detail");
            ImGui::TextUnformatted("Review snapshot");
            ImGui::Separator();
            ImGui::TextColored(k.green, "System health: Stable");
            if (gpu_loading) {
                ImGui::TextColored(k.brand_hov, "CHECKING GRAPHICS ADAPTER");
                ImGui::TextColored(k.muted,
                                   "Graphics information is loading in the background. You can keep browsing.");
            } else if (gpu_error) {
                ImGui::TextColored(k.red, "GPU INFORMATION UNAVAILABLE");
                ImGui::TextColored(k.muted,
                                   "The graphics adapter could not be identified. Use Refresh to try again.");
                ghost_button("Refresh monitor", ImVec2(ui_px(140.0f), ui_px(32.0f)), true);
            } else {
                ImGui::TextColored(k.muted, "GPU: representative hardware profile");
            }
            ImGui::TextColored(k.muted, "Uptime and network totals are intentionally omitted from fixture evidence.");
            card_end();
            break;
        }
    }
    draw_fixture_clear_cache_confirmation(st);
}

void draw_performance_page(UiState& st) {
    auto& perf_ui = get_performance_ui_state();

    if (st.fixture_mode) {
        if (st.fixture_case == "performance-cache" ||
            st.fixture_case == "performance-cache-loading" ||
            st.fixture_case == "performance-cache-cleanup-working" ||
            st.fixture_case == "performance-cache-error" ||
            st.fixture_case == "performance-clear-cache-confirm") perf_ui.current_tab = 1;
        else if (st.fixture_case == "performance-offline" ||
                 st.fixture_case == "performance-offline-syncing" ||
                 st.fixture_case == "performance-offline-sync-complete" ||
                 st.fixture_case == "performance-offline-sync-error") perf_ui.current_tab = 2;
        else if (st.fixture_case == "performance-settings") perf_ui.current_tab = 3;
        else if (st.fixture_case == "performance" || st.fixture_case == "performance-monitor" ||
                 st.fixture_case == "performance-monitor-gpu-loading" ||
                 st.fixture_case == "performance-monitor-gpu-error")
            perf_ui.current_tab = 0;
        draw_fixture_performance_page(st, perf_ui.current_tab);
        return;
    }
    
    page_title("Performance", "Optimize launcher performance and manage resources");
    
    // Performance tabs
    if (ImGui::BeginTabBar("##performance_tabs")) {
    
    if (ImGui::BeginTabItem("Monitor", nullptr,
                            st.fixture_mode && perf_ui.current_tab == 0
                                ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None)) {
        perf_ui.current_tab = 0;
        ImGui::EndTabItem();
    }
    
    if (ImGui::BeginTabItem("Cache", nullptr,
                            st.fixture_mode && perf_ui.current_tab == 1
                                ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None)) {
        perf_ui.current_tab = 1;
        ImGui::EndTabItem();
    }
    
    if (ImGui::BeginTabItem("Offline", nullptr,
                            st.fixture_mode && perf_ui.current_tab == 2
                                ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None)) {
        perf_ui.current_tab = 2;
        ImGui::EndTabItem();
    }
    
    if (ImGui::BeginTabItem("Settings", nullptr,
                            st.fixture_mode && perf_ui.current_tab == 3
                                ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None)) {
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
    std::tm local_time{};
    if (localtime_s(&local_time, &time) != 0) return "Unavailable";
    // The single formatter is backed by Config and used by account, server,
    // backup, performance, and Bedrock surfaces through this shared helper.
    // When it is reached before the UI state exists, Config defaults remain
    // the safe, documented ISO/24-hour fallback.
    const config::Config defaults;
    return config::format_local_date_time(local_time, app && app->cfg ? *app->cfg : defaults);
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
