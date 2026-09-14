// =============================================================================
// ai_install_ui.cpp — ImGui AI model install panel
// =============================================================================
//
// Professional in-launcher GUI replacing the old CMD window bootstrap.
// Shows: overall progress, speed, ETA, per-component status, cancel.

#include "ai_install_ui.h"
#include "ai_install.h"
#include "ui.h"
#include "ui_internal.h"
#include "net.h"

#include <cmath>
#include <filesystem>
#include <chrono>

using aml::ui::UiState;
using aml::ui::k;
using aml::ui::f_h2;
using aml::ui::f_bold;
using aml::ui::f_small;
using aml::ui::ui_px;
using aml::ui::card_begin;
using aml::ui::card_end;
using aml::ui::primary_button;
using aml::ui::ghost_button;
using aml::ui::push_notice;

namespace aml::ai_install_ui {

// ---- Formatting helpers ----------------------------------------------------
static std::string fmt_bytes(int64_t b) {
    if (b >= 1073741824LL) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.1f GB", b / 1073741824.0);
        return buf;
    }
    if (b >= 1048576LL) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.0f MB", b / 1048576.0);
        return buf;
    }
    if (b >= 1024LL) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.0f KB", b / 1024.0);
        return buf;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld B", (long long)b);
    return buf;
}

static std::string fmt_speed(double bps) {
    if (bps >= 1048576.0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.1f MB/s", bps / 1048576.0);
        return buf;
    }
    if (bps >= 1024.0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.0f KB/s", bps / 1024.0);
        return buf;
    }
    return "--";
}

static std::string fmt_duration(double seconds) {
    if (seconds < 0) return "--";
    int mins = static_cast<int>(seconds) / 60;
    int secs = static_cast<int>(seconds) % 60;
    char buf[32];
    if (mins > 0)
        snprintf(buf, sizeof(buf), "%dm %02ds", mins, secs);
    else
        snprintf(buf, sizeof(buf), "%ds", secs);
    return buf;
}

// ---- Status icons ----------------------------------------------------------
static const char* status_icon(ai_install::ComponentStatus s) {
    switch (s) {
        case ai_install::ComponentStatus::Unknown:     return " ";
        case ai_install::ComponentStatus::Missing:     return "[ ]";
        case ai_install::ComponentStatus::Downloading: return "[>]";
        case ai_install::ComponentStatus::Verifying:   return "[~]";
        case ai_install::ComponentStatus::Installed:   return "[v]";
        case ai_install::ComponentStatus::Failed:      return "[X]";
        case ai_install::ComponentStatus::Skipped:     return "[-]";
    }
    return " ";
}

static const char* status_label(ai_install::ComponentStatus s) {
    switch (s) {
        case ai_install::ComponentStatus::Unknown:     return "";
        case ai_install::ComponentStatus::Missing:     return "Missing";
        case ai_install::ComponentStatus::Downloading: return "Downloading";
        case ai_install::ComponentStatus::Verifying:   return "Verifying";
        case ai_install::ComponentStatus::Installed:   return "Installed";
        case ai_install::ComponentStatus::Failed:      return "Failed";
        case ai_install::ComponentStatus::Skipped:     return "Skipped";
    }
    return "";
}

static ImVec4 status_color(ai_install::ComponentStatus s) {
    switch (s) {
        case ai_install::ComponentStatus::Installed:   return k.green;
        case ai_install::ComponentStatus::Downloading: return k.blue;
        case ai_install::ComponentStatus::Verifying:   return ImVec4(0.91f, 0.80f, 0.31f, 1.0f);
        case ai_install::ComponentStatus::Failed:      return k.red;
        default:                                        return k.muted;
    }
}

// ---- Track install start time for elapsed/ETA ------------------------------
static std::chrono::steady_clock::time_point s_install_start;
static bool s_was_running = false;

// ---- Draw the AI install panel ---------------------------------------------
void draw_ai_install_panel(aml::ui::UiState& st) {
    auto& mgr = ai_install::InstallManager::instance();

    // Lazy-load manifest
    static bool manifest_loaded = false;
    if (!manifest_loaded) {
        std::wstring manifest_path = st.exe_dir + L"\\ai\\ai-package-manifest.json";
        if (std::filesystem::exists(manifest_path)) {
            if (mgr.load_manifest(manifest_path)) {
                // Hashing multi-GB models belongs on the owned worker, never in
                // the render loop. The panel will show Checking until it ends.
                mgr.start_verify();
                manifest_loaded = true;
            }
        }
    }

    auto comps = mgr.snapshot();
    if (comps.empty()) {
        ImGui::TextColored(k.muted, "AI manifest not found.");
        return;
    }

    // Track elapsed time. Verification has its own worker state because
    // hashing multi-GB models must never block the UI thread.
    bool running = mgr.is_running();
    const bool verifying = mgr.is_verifying();
    if (running && !s_was_running) {
        s_install_start = std::chrono::steady_clock::now();
    }
    s_was_running = running;

    // Overall stats
    int64_t total_installed = 0, total_needed = 0, total_downloaded = 0;
    int installed_count = 0, total_count = 0;
    double total_speed = 0.0;
    for (const auto& c : comps) {
        if (c.id == "knowledge") continue;
        total_count++;
        total_needed += c.size_bytes;
        if (c.status == ai_install::ComponentStatus::Installed) {
            total_installed += c.size_bytes;
            installed_count++;
        } else if (c.status == ai_install::ComponentStatus::Downloading) {
            total_downloaded += c.bytes_done;
            total_speed += c.speed_bps;
        }
    }

    int64_t effective_bytes = total_installed + total_downloaded;
    float overall = (total_needed > 0)
        ? static_cast<float>(static_cast<double>(effective_bytes) / total_needed)
        : 0.0f;

    // Elapsed + ETA
    double elapsed_sec = 0.0;
    double eta_sec = -1.0;
    if (running) {
        auto now = std::chrono::steady_clock::now();
        elapsed_sec = std::chrono::duration<double>(now - s_install_start).count();
        if (total_speed > 0 && total_needed > effective_bytes) {
            eta_sec = static_cast<double>(total_needed - effective_bytes) / total_speed;
        }
    }

    // ---- Header ------------------------------------------------------------
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted("AMALGAM AI");
    ImGui::PopFont();
    ImGui::PushFont(f_small);
    ImGui::TextColored(k.muted, "Local AI for your Minecraft profiles");
    ImGui::PopFont();
    ImGui::Spacing();

    // ---- Status line -------------------------------------------------------
    if (running) {
        ImGui::TextColored(k.blue, "Status: Installing...");
    } else if (verifying) {
        ImGui::TextColored(k.blue, "Status: Checking installed components...");
    } else if (total_installed == total_needed && total_count > 0) {
        ImGui::TextColored(k.green, "Status: All %d components installed and verified", total_count);
    } else {
        ImGui::TextColored(k.yellow, "Status: %d/%d components installed", installed_count, total_count);
    }

    // ---- Overall progress bar ----------------------------------------------
    if (running && total_needed > 0) {
        ImGui::Spacing();
        ImGui::Text("Overall:");
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, k.brand);
        ImGui::ProgressBar(overall, ImVec2(-1, ui_px(12.0f)));
        ImGui::PopStyleColor();

        // Speed + Downloaded + ETA on same line
        ImGui::PushFont(f_small);
        std::string info_line = fmt_speed(total_speed) + "  " +
                                fmt_bytes(effective_bytes) + " / " + fmt_bytes(total_needed);
        if (elapsed_sec > 0) info_line += "  elapsed: " + fmt_duration(elapsed_sec);
        if (eta_sec >= 0) info_line += "  ETA: " + fmt_duration(eta_sec);
        ImGui::TextColored(k.muted, "%s", info_line.c_str());
        ImGui::PopFont();
    } else if (total_installed < total_needed) {
        // Not running — show what's needed
        ImGui::Spacing();
        ImGui::PushFont(f_small);
        ImGui::TextColored(k.muted, "Downloaded: %s / %s",
                          fmt_bytes(total_installed).c_str(), fmt_bytes(total_needed).c_str());
        ImGui::PopFont();
    }
    ImGui::Spacing();

    // ---- Current component -------------------------------------------------
    if (running || verifying) {
        for (const auto& c : comps) {
            if (c.status == ai_install::ComponentStatus::Downloading ||
                c.status == ai_install::ComponentStatus::Verifying) {
                ImGui::PushFont(f_small);
                ImGui::TextColored(c.status == ai_install::ComponentStatus::Verifying
                                       ? ImVec4(0.91f, 0.80f, 0.31f, 1.0f) : k.blue,
                                   "Current: %s", c.name.c_str());
                ImGui::PopFont();
                break;
            }
        }
    }

    // ---- Component list ----------------------------------------------------
    ImGui::PushFont(f_bold);
    ImGui::TextUnformatted("Components:");
    ImGui::PopFont();
    ImGui::Spacing();

    for (int i = 0; i < static_cast<int>(comps.size()); ++i) {
        const auto& c = comps[i];
        if (c.id == "knowledge") continue;

        ImGui::PushID(i);

        ImVec4 col = status_color(c.status);

        // Status icon + name
        ImGui::TextColored(col, "%s", status_icon(c.status));
        ImGui::SameLine();
        ImGui::TextUnformatted(c.name.c_str());

        // Size
        ImGui::SameLine();
        ImGui::PushFont(f_small);
        ImGui::TextDisabled("(%s)", fmt_bytes(c.size_bytes).c_str());
        ImGui::PopFont();

        // Progress bar for active downloads
        if (c.status == ai_install::ComponentStatus::Downloading) {
            ImGui::Indent(ui_px(24.0f));
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, k.blue);
            ImGui::ProgressBar(c.progress, ImVec2(-1, ui_px(6.0f)));
            ImGui::PopStyleColor();
            ImGui::PushFont(f_small);
            ImGui::TextColored(k.muted, "%s  %s / %s",
                              fmt_speed(c.speed_bps).c_str(),
                              fmt_bytes(c.bytes_done).c_str(),
                              fmt_bytes(c.size_bytes).c_str());
            ImGui::PopFont();
            ImGui::Unindent(ui_px(24.0f));
        } else if (c.status == ai_install::ComponentStatus::Failed) {
            ImGui::Indent(ui_px(24.0f));
            ImGui::TextColored(k.red, "Failed: %s", c.error.c_str());
            ImGui::Unindent(ui_px(24.0f));
        } else if (c.status == ai_install::ComponentStatus::Verifying) {
            ImGui::Indent(ui_px(24.0f));
            ImGui::TextColored(k.muted, "Verifying SHA-256...");
            ImGui::Unindent(ui_px(24.0f));
        }

        ImGui::PopID();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ---- Action buttons ----------------------------------------------------
    if (!running && !verifying) {
        if (installed_count < total_count) {
            if (primary_button("Install AI Models", ImVec2(ui_px(200.0f), ui_px(36.0f)))) {
                std::wstring models_dir = mgr.models_dir().empty()
                    ? (net::to_wide(std::string(
#if _WIN32
                           std::getenv("LOCALAPPDATA")
                               ? std::getenv("LOCALAPPDATA")
                               : "C:\\"
#else
                           "."
#endif
                       )) + L"\\Amalgam\\AI\\Models")
                    : mgr.models_dir();
                s_install_start = std::chrono::steady_clock::now();
                mgr.start_install(models_dir, [](int, int64_t, double) {});
                push_notice(st, ui_model::NoticeLevel::Info,
                           "AI install started",
                           "Downloading and verifying AI models. This may take several minutes.");
            }
            ImGui::SameLine();
        }
        if (ImGui::SmallButton("Verify All")) {
            mgr.start_verify();
        }
    } else {
        if (ghost_button(verifying ? "Stop check" : "Cancel", ImVec2(ui_px(120.0f), ui_px(32.0f)))) {
            mgr.cancel();
            push_notice(st, ui_model::NoticeLevel::Warning,
                       verifying ? "AI check stopped" : "AI install cancelled",
                       verifying ? "The current verification was stopped; run it again when ready."
                                  : "Downloads stopped. Partial files are kept for resume.");
        }
    }

    // ---- Models directory info ---------------------------------------------
    ImGui::Spacing();
    ImGui::PushFont(f_small);
    if (!mgr.models_dir().empty()) {
        ImGui::TextColored(k.muted, "Models: %s", net::to_utf8(mgr.models_dir()).c_str());
    } else {
        ImGui::TextColored(k.muted, "Models: <not yet determined>");
    }
    ImGui::PopFont();
}

}  // namespace aml::ai_install_ui
