#include "ui.h"
#include "ui_internal.h"
#include "ui_motion.h"
#include "loading_screen.h"

#include "ai.h"
#include "ai_ui.h"
#include "ai_install_ui.h"
#include "diagnostics.h"
#include "ai_core.h"
#include "admin_auth.h"
#include "admin_ui.h"
#include "account_manager.h"
#include "account_ui.h"
#include "auth.h"
#include "auth_wizard.h"
#include "bedrock_ui.h"
#include "mod_manager_ui.h"
#include "performance_ui.h"
#include "social_ui.h"
#include "theme_ui.h"
#include "essentials_ui.h"
#include "essentials_sync.h"
#include "essentials_manager.h"
#include "essentials_session.h"
#include "essentials.h"
#include "supabase.h"
#include "updater.h"
#include "bedrock.h"
#include "java.h"
#include "json.h"
#include "instances.h"
#include "import_pack.h"
#include "launch.h"
#include "official_launcher_bridge.h"
#include "online_config.h"
#include "model.h"
#include "mods.h"
#include "provider_config.h"
#include "net.h"
#include "performance.h"
#include "readiness.h"
#include "entitlements.h"
#include "ui_model.h"
#include "version_catalog.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_win32.h"

#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <wincodec.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <GL/gl.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <chrono>

#pragma comment(lib, "iphlpapi.lib")
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <memory>
#include <set>
#include <vector>
#include <unordered_map>

#pragma comment(lib, "dwmapi.lib")

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
#ifndef DWMWA_TEXT_COLOR
#define DWMWA_TEXT_COLOR 36
#endif
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam,
                                                             LPARAM lParam);

namespace aml::ui {

static std::vector<std::string> available_profile_versions(UiState& st,
                                                             const std::string& loader) {
    std::vector<std::string> result = version_catalog::profile_versions(loader);
    std::vector<std::string> live_ids;
    {
        std::lock_guard<std::mutex> lock(st.version_mu);
        live_ids.reserve(st.versions.size());
        for (const auto& entry : st.versions) {
            if (entry.type == "release") live_ids.push_back(entry.id);
        }
    }
    version_catalog::merge_live_releases(result, live_ids, loader);
    return result;
}

// Canonical release identity. The same version is used by the launcher,
// the About page, the update manifest, diagnostics and bug reports. The
// channel participates in the updater policy: stable builds receive stable
// updates, and beta builds can never jump to stable (or vice versa).
static const char* kVersion = "1.0.0";
static const char* kChannel = "stable";

// ---------------------------------------------------------------------------
// Theme / fonts (shared via ui_internal.h extern declarations)
// ---------------------------------------------------------------------------
ImFont* f_body = nullptr;   // base body
ImFont* f_bold = nullptr;   // bold body
ImFont* f_title = nullptr;  // big brand / page title
ImFont* f_h2 = nullptr;     // section titles
ImFont* f_small = nullptr;  // captions
ImFont* f_mono = nullptr;   // version ids / log
float g_ui_scale = 1.0f;
float g_pending_ui_scale = 0.0f;
Theme k;

void init_theme() {
    // Reference palette: almost-black navy foundations, cool slate panels,
    // and a concentrated violet accent. The contrast is deliberate so cover
    // art and status colours stay readable on every page.
    k.bg = ImVec4(0.017f, 0.029f, 0.051f, 1.0f);
    k.sidebar = ImVec4(0.012f, 0.022f, 0.040f, 1.0f);
    k.surface = ImVec4(0.032f, 0.052f, 0.086f, 1.0f);
    k.surface2 = ImVec4(0.049f, 0.078f, 0.122f, 1.0f);
    k.border = ImVec4(0.115f, 0.165f, 0.240f, 1.0f);
    k.text = ImVec4(0.93f, 0.94f, 0.97f, 1.0f);
    k.muted = ImVec4(0.60f, 0.67f, 0.77f, 1.0f);
    k.brand = ImVec4(0.48f, 0.19f, 0.90f, 1.0f);
    k.brand_hov = ImVec4(0.72f, 0.43f, 1.00f, 1.0f);
    k.brand_dk = ImVec4(0.24f, 0.07f, 0.52f, 1.0f);
    k.sel = ImVec4(0.20f, 0.07f, 0.43f, 1.0f);
    k.hover = ImVec4(0.075f, 0.115f, 0.178f, 1.0f);
    k.red = ImVec4(0.95f, 0.33f, 0.36f, 1.0f);
    k.blue = ImVec4(0.35f, 0.62f, 0.96f, 1.0f);
    k.orange = ImVec4(0.96f, 0.66f, 0.25f, 1.0f);
    k.yellow = ImVec4(0.91f, 0.80f, 0.31f, 1.0f);
    k.green = ImVec4(0.11f, 0.86f, 0.42f, 1.0f);
}

void apply_theme() {
    ImGuiStyle& s = ImGui::GetStyle();
    // Rebuild from a clean base every time.  WM_DPICHANGED can arrive after
    // the launcher has already scaled the style once; layering another scale
    // over that state causes oversized, fuzzy controls on a second monitor.
    ImGui::StyleColorsDark(&s);
    s.WindowRounding = 8.0f;
    s.ChildRounding = 8.0f;
    s.FrameRounding = 6.0f;
    s.PopupRounding = 8.0f;
    s.ScrollbarRounding = 6.0f;
    s.GrabRounding = 5.0f;
    s.TabRounding = 5.0f;
    s.WindowBorderSize = 1.0f;
    s.ChildBorderSize = 1.0f;
    s.FrameBorderSize = 1.0f;
    s.WindowPadding = ImVec2(12, 10);
    s.FramePadding = ImVec2(9, 5);
    s.ItemSpacing = ImVec2(7, 5);
    s.ItemInnerSpacing = ImVec2(5, 3);
    s.ScrollbarSize = 9.0f;
    s.ScrollbarRounding = 6.0f;
    s.GrabMinSize = 9.0f;
    s.AntiAliasedLines = true;
    s.AntiAliasedFill = true;

    ImGui::GetStyle().Colors[ImGuiCol_WindowBg] = k.bg;
    ImGui::GetStyle().Colors[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    ImGui::GetStyle().Colors[ImGuiCol_PopupBg] = k.surface;
    ImGui::GetStyle().Colors[ImGuiCol_Border] = k.border;
    ImGui::GetStyle().Colors[ImGuiCol_FrameBg] = k.surface2;
    ImGui::GetStyle().Colors[ImGuiCol_FrameBgHovered] =
        k.surface2 + ImVec4(0.04f, 0.04f, 0.04f, 0);
    ImGui::GetStyle().Colors[ImGuiCol_FrameBgActive] = k.brand_dk;
    ImGui::GetStyle().Colors[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    ImGui::GetStyle().Colors[ImGuiCol_TitleBg] = k.sidebar;
    ImGui::GetStyle().Colors[ImGuiCol_TitleBgActive] = k.sidebar;
    ImGui::GetStyle().Colors[ImGuiCol_MenuBarBg] = k.sidebar;
    ImGui::GetStyle().Colors[ImGuiCol_Header] = k.sel;
    ImGui::GetStyle().Colors[ImGuiCol_HeaderHovered] = k.hover;
    ImGui::GetStyle().Colors[ImGuiCol_HeaderActive] = k.brand_dk;
    ImGui::GetStyle().Colors[ImGuiCol_Button] = k.surface2;
    ImGui::GetStyle().Colors[ImGuiCol_ButtonHovered] =
        k.surface2 + ImVec4(0.05f, 0.05f, 0.05f, 0);
    ImGui::GetStyle().Colors[ImGuiCol_ButtonActive] = k.brand_dk;
    ImGui::GetStyle().Colors[ImGuiCol_CheckMark] = k.brand;
    ImGui::GetStyle().Colors[ImGuiCol_SliderGrab] = k.brand;
    ImGui::GetStyle().Colors[ImGuiCol_SliderGrabActive] = k.brand_hov;
    ImGui::GetStyle().Colors[ImGuiCol_TextSelectedBg] = k.sel;
    ImGui::GetStyle().Colors[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0.30f);
    ImGui::GetStyle().Colors[ImGuiCol_ScrollbarGrab] = k.surface2;
    ImGui::GetStyle().Colors[ImGuiCol_ScrollbarGrabHovered] = k.surface2 + ImVec4(0.05f, 0.05f, 0.05f, 0);
    ImGui::GetStyle().Colors[ImGuiCol_ScrollbarGrabActive] = k.brand;
    ImGui::GetStyle().Colors[ImGuiCol_Separator] = k.border;
    ImGui::GetStyle().Colors[ImGuiCol_Tab] = k.surface;
    ImGui::GetStyle().Colors[ImGuiCol_TabHovered] = k.hover;
    ImGui::GetStyle().Colors[ImGuiCol_TabActive] = k.sel;
    ImGui::GetStyle().Colors[ImGuiCol_Text] = k.text;
    ImGui::GetStyle().Colors[ImGuiCol_TextDisabled] = k.muted;
    // Visible keyboard focus: brand outline around the focused item so the
    // launcher is navigable without a mouse.
    ImGui::GetStyle().Colors[ImGuiCol_NavHighlight] = k.brand_hov;
    s.ScaleAllSizes(g_ui_scale);
}

// ---- Deferred CJK merge ----
// Loading and rasterizing the 19 MB msyh.ttc for CJK ranges is the single
// biggest startup cost.  We defer it until after the first frame renders so
// the launcher appears instantly; CJK glyphs appear on the next frame.
static bool g_cjk_deferred_pending = false;

void build_font_atlas() {
    ImGuiIO& io = ImGui::GetIO();
    // ClearFonts() removes font data without destroying atlas texture pixels.
    // io.Fonts->Clear() calls ClearTexData() which calls DestroyPixels() on all
    // atlas textures when RendererHasTextures is true, creating textures with
    // Status=Destroyed + Pixels=NULL.  ImGui then auto-converts them to WantCreate
    // and the backend calls GetPixels() on NULL, triggering the assertion at
    // imgui.h:3596 ("Pixels != 0").  ClearFonts() avoids this by only clearing
    // font objects and input data, leaving texture lifecycle to Build().
    io.Fonts->ClearFonts();
    io.FontGlobalScale = 1.0f;

    ImFontConfig sharp_font;
    sharp_font.OversampleH = 2;
    sharp_font.OversampleV = 1;
    sharp_font.PixelSnapH = false;
    static ImVector<ImWchar> extended_ranges;
    static ImVector<ImWchar> cjk_ranges;
    if (extended_ranges.empty()) {
        ImFontGlyphRangesBuilder builder;
        builder.AddRanges(io.Fonts->GetGlyphRangesDefault());
        // Provider descriptions commonly contain typographic punctuation and
        // extended Latin.  Rendering those glyphs avoids replacement diamonds
        // while keeping the primary atlas compact.
        static const ImWchar common_ranges[] = {
            0x00A0, 0x024F,  // Latin-1 / extended Latin
            0x2000, 0x206F,  // general punctuation
            0x20A0, 0x20CF,  // currency symbols
            0x2190, 0x21FF,  // arrows
            0x25A0, 0x25FF,  // geometric symbols
            0x2600, 0x27BF,  // commonly used UI symbols
            0,
        };
        builder.AddRanges(common_ranges);
        builder.AddChar(0x2605);  // ★ favorite
        builder.AddChar(0x2606);  // ☆ not favorite
        builder.AddChar(0x25B2);  // console up
        builder.AddChar(0x25BC);  // console down
        builder.BuildRanges(&extended_ranges);
    }
    if (cjk_ranges.empty()) {
        ImFontGlyphRangesBuilder builder;
        builder.AddRanges(io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
        builder.BuildRanges(&cjk_ranges);
    }
    const bool cjk_font_available =
        GetFileAttributesW(L"C:\\Windows\\Fonts\\msyh.ttc") != INVALID_FILE_ATTRIBUTES;
    const bool cjk_bold_font_available =
        GetFileAttributesW(L"C:\\Windows\\Fonts\\msyhbd.ttc") != INVALID_FILE_ATTRIBUTES;
    auto merge_cjk_fallback = [&](float pixels, bool bold) {
        if (!cjk_font_available) return;
        ImFontConfig fallback = sharp_font;
        fallback.MergeMode = true;
        fallback.OversampleH = 2;
        fallback.OversampleV = 1;
        const char* font = bold && cjk_bold_font_available
            ? "C:\\Windows\\Fonts\\msyhbd.ttc"
            : "C:\\Windows\\Fonts\\msyh.ttc";
        io.Fonts->AddFontFromFileTTF(font, pixels, &fallback, cjk_ranges.Data);
    };
    io.FontDefault = io.Fonts->AddFontDefault(&sharp_font);
    f_body = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf",
                                           16.0f * g_ui_scale, &sharp_font,
                                           extended_ranges.Data);
    // Defer CJK merge: skip on first launch for instant appearance.
    // The expensive 19 MB msyh.ttc rasterization happens after the first
    // frame renders so the window appears immediately.
    if (!g_cjk_deferred_pending) {
        merge_cjk_fallback(16.0f * g_ui_scale, false);
    }
    if (f_body) io.FontDefault = f_body;
    f_bold = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisb.ttf",
                                           16.0f * g_ui_scale, &sharp_font,
                                           extended_ranges.Data);
    if (!g_cjk_deferred_pending) {
        merge_cjk_fallback(16.0f * g_ui_scale, true);
    }
    f_title = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisb.ttf",
                                            26.0f * g_ui_scale, &sharp_font,
                                            extended_ranges.Data);
    if (!g_cjk_deferred_pending) {
        merge_cjk_fallback(26.0f * g_ui_scale, true);
    }
    f_h2 = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisb.ttf",
                                        18.0f * g_ui_scale, &sharp_font,
                                        extended_ranges.Data);
    f_small = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf",
                                           13.0f * g_ui_scale, &sharp_font,
                                           extended_ranges.Data);
    f_mono = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\consola.ttf",
                                          14.0f * g_ui_scale, &sharp_font,
                                          extended_ranges.Data);
    if (!f_bold) f_bold = f_body ? f_body : io.FontDefault;
    if (!f_title) f_title = f_bold;
    if (!f_h2) f_h2 = f_bold;
    if (!f_small) f_small = f_body ? f_body : io.FontDefault;
    if (!f_mono) f_mono = f_body ? f_body : io.FontDefault;
}

// Merge CJK fallback fonts into the existing atlas WITHOUT calling
// ClearFonts().  The full build_font_atlas() path calls ClearFonts()
// which destroys font objects and input data, triggering texture lifecycle
// conflicts (assertion: Pixels != 0) once the OpenGL renderer is active.
// Instead we append CJK-merged fallbacks directly — ImGui's dynamic atlas
// will repack on the next frame.
void merge_cjk_deferred_fonts() {
    ImGuiIO& io = ImGui::GetIO();
    const bool cjk_font_available =
        GetFileAttributesW(L"C:\\Windows\\Fonts\\msyh.ttc") != INVALID_FILE_ATTRIBUTES;
    const bool cjk_bold_font_available =
        GetFileAttributesW(L"C:\\Windows\\Fonts\\msyhbd.ttc") != INVALID_FILE_ATTRIBUTES;
    if (!cjk_font_available) return;

    ImFontConfig fallback;
    fallback.MergeMode = true;
    fallback.OversampleH = 2;
    fallback.OversampleV = 1;
    static ImVector<ImWchar> cjk_ranges;
    if (cjk_ranges.empty()) {
        ImFontGlyphRangesBuilder builder;
        builder.AddRanges(io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
        builder.BuildRanges(&cjk_ranges);
    }
    // Merge CJK into each existing font that we own.
    auto add_merge = [&](ImFont* font, float size, bool bold) {
        if (!font) return;
        fallback.DstFont = font;
        const char* path = bold && cjk_bold_font_available
            ? "C:\\Windows\\Fonts\\msyhbd.ttc"
            : "C:\\Windows\\Fonts\\msyh.ttc";
        io.Fonts->AddFontFromFileTTF(path, size, &fallback, cjk_ranges.Data);
        fallback.DstFont = nullptr;
    };
    add_merge(f_body, 16.0f * g_ui_scale, false);
    add_merge(f_bold, 16.0f * g_ui_scale, true);
    add_merge(f_title, 26.0f * g_ui_scale, true);
    add_merge(f_h2, 18.0f * g_ui_scale, true);
    add_merge(f_small, 13.0f * g_ui_scale, false);
    add_merge(f_mono, 14.0f * g_ui_scale, false);
}

void rebuild_dpi_resources(float scale) {
    if (std::fabs(scale - g_ui_scale) < 0.01f) return;
    g_ui_scale = scale;
    apply_theme();
    // Build() creates new texture objects for the new scale;
    // ImGui's frame processing uploads them automatically.
    build_font_atlas();
}

UiState* app = nullptr;

// Declared here because profile-affecting operations run throughout the file;
// the definition sits next to the launch checks it aggregates.
void mark_home_readiness_dirty(UiState& st);

// A project page is rendered by the catalog route. Clear its state whenever a
// player explicitly goes somewhere else so each sidebar action always lands
// on the requested surface.
void navigate_to(UiState& st, int sidebar_item, int tab, int provider_tab) {
    if (st.project_detail_open) {
        st.project_detail_open = false;
        st.project_loading = false;
        st.project_file_selected = -1;
        st.project_translation_request.fetch_add(1);
    }
    st.sidebar_item = sidebar_item;
    st.active_tab = tab;
    if (provider_tab >= 0) st.provider_tab = provider_tab;
}

void seed_visual_fixture(UiState& st) {
    // Never call this from the normal launcher. It exists solely so release
    // review can exercise the complete responsive shell without network data,
    // a Microsoft account, or a user's own profiles.
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    st.selected = "1.21.1";
    st.fetching = false;
    st.home_fetching = false;
    st.account.username = "Visual Review";
    st.account.uuid = "visual-fixture";
    st.auth_checked = true;
    st.auth_status = "Visual-review fixture";

    instances::Instance first;
    first.id = "fixture-astral";
    first.name = "Astral Frontier";
    first.minecraft_version = "1.21.1";
    first.loader = "fabric";
    first.pack_source = "modrinth";
    first.pack_project = "astral-frontier";
    first.pack_version = "fixture-a";
    first.favorite = true;
    first.last_played = now - 60 * 38;
    first.directory = st.exe_dir + L"\\visual-fixture\\astral-frontier";

    instances::Instance second;
    second.id = "fixture-forge";
    second.name = "Forge & Fables";
    second.minecraft_version = "1.20.1";
    second.loader = "forge";
    second.pack_source = "curseforge";
    second.pack_project = "fixture-forge";
    second.pack_version = "fixture-b";
    second.last_played = now - 60 * 60 * 24;
    second.directory = st.exe_dir + L"\\visual-fixture\\forge-fables";

    instances::Instance third;
    third.id = "fixture-custom";
    third.name = "Creative Workshop";
    third.minecraft_version = "1.21.1";
    third.loader = "neoforge";
    third.performance_profile = "performance";
    third.last_played = now - 60 * 60 * 24 * 4;
    third.directory = st.exe_dir + L"\\visual-fixture\\creative-workshop";
    st.instance_list = {first, second, third};
    st.instances_loaded = true;
    st.selected_instance = first;
    st.active_instance_dir = first.directory;
    // Give the visual fixture the same healthy filesystem shape as a real
    // profile so screenshots showcase the intended ready state, not setup
    // errors caused by missing test directories.
    for (const auto& instance : st.instance_list) {
        std::error_code fixture_error;
        std::filesystem::create_directories(
            std::filesystem::path(instance.directory) / L"mods", fixture_error);
        std::filesystem::create_directories(
            std::filesystem::path(instance.directory) / L"screenshots", fixture_error);
        std::ofstream install_marker(
            std::filesystem::path(instance.directory) / L".amalgam-install.json",
            std::ios::binary);
        if (install_marker) install_marker << "{}";
    }

    auto project = [](const char* slug, const char* title, const char* description,
                      const char* source, int64_t downloads) {
        mods::SearchResult item;
        item.slug = slug;
        item.title = title;
        item.description = description;
        item.source = source;
        item.type = mods::ProjectType::Modpack;
        item.downloads = downloads;
        item.loaders = {"fabric", "forge"};
        return item;
    };
    st.home_packs = {
        project("all-the-mods-10", "All the Mods 10", "A complete modern Minecraft adventure.",
                "curseforge", 3100000),
        project("fabulously-optimized", "Fabulously Optimized", "Smooth, fast Minecraft for everyone.",
                "modrinth", 12400000),
        project("better-mc", "Better MC", "A curated progression and exploration pack.",
                "curseforge", 5700000),
        project("rlcraft", "RLCraft", "A demanding survival challenge.", "curseforge", 20100000),
    };
    st.home_mods = {
        project("sodium", "Sodium", "Modern rendering performance.", "modrinth", 98000000),
        project("create", "Create", "Mechanical creativity and automation.", "curseforge", 80000000),
        project("iris", "Iris Shaders", "Shaders without sacrificing control.", "modrinth", 44000000),
        project("jei", "Just Enough Items", "In-game recipe and item lookup.", "curseforge", 320000000),
    };
    for (auto& item : st.home_mods) item.type = mods::ProjectType::Mod;
    const uint64_t request_id = st.next_request_id.fetch_add(1);
    st.home_screen.begin(request_id);
    std::vector<mods::SearchResult> combined = st.home_packs;
    combined.insert(combined.end(), st.home_mods.begin(), st.home_mods.end());
    st.home_screen.accept(request_id, std::move(combined), true);

    UiState::DownloadJob completed;
    completed.id = st.next_job_id++;
    completed.kind = "visual-fixture";
    completed.label = "Fixture setup";
    completed.detail = "Ready for visual review";
    completed.phase = completed.detail;
    completed.active = false;
    completed.completed = true;
    completed.progress = 1.0f;
    completed.created_at = static_cast<uint64_t>(now - 18);
    completed.started_at = completed.created_at;
    completed.finished_at = static_cast<uint64_t>(now - 4);
    st.jobs.push_back(std::move(completed));

    UiState::DownloadJob running;
    running.id = st.next_job_id++;
    running.kind = "install_project";
    running.label = "Install Sodium";
    running.detail = "Downloading verified release";
    running.phase = "Downloading 3.2 MB / 6.4 MB";
    running.target_profile = first.name;
    running.provider = "modrinth";
    running.progress = 0.5f;
    running.bytes_done = 3200000;
    running.bytes_total = 6400000;
    running.bytes_per_second = 850000;
    running.eta_seconds = 4.0;
    running.exclusive = true;
    running.created_at = static_cast<uint64_t>(now - 8);
    running.started_at = running.created_at;
    st.jobs.push_back(std::move(running));

    UiState::DownloadJob queued;
    queued.id = st.next_job_id++;
    queued.kind = "update_profile";
    queued.label = "Update Forge & Fables";
    queued.detail = "Waiting for the current profile change";
    queued.phase = "Queued for protected profile access";
    queued.target_profile = second.name;
    queued.queued = true;
    queued.exclusive = true;
    queued.resumable = true;
    queued.created_at = static_cast<uint64_t>(now - 2);
    st.jobs.push_back(std::move(queued));

    UiState::DownloadJob failed_download;
    failed_download.id = st.next_job_id++;
    failed_download.kind = "install_project";
    failed_download.label = "Install Better MC";
    failed_download.detail = "http status 403";
    failed_download.phase = "http status 403";
    failed_download.target_profile = first.name;
    failed_download.provider = "curseforge";
    failed_download.failed = true;
    failed_download.active = false;
    failed_download.resumable = true;
    failed_download.retry_action = "install_project";
    failed_download.retry_payload = std::string("{") + char(34) + "source" + char(34) + ":" + char(34) + "curseforge" + char(34) + "," + char(34) + "slug" + char(34) + ":" + char(34) + "better-mc" + char(34) + "}"; // "{\\"source\\":\\"curseforge\\",\\"slug\\":\\"better-mc\\"}";
    failed_download.error = "http status 403: provider rejected the download request";
    failed_download.created_at = static_cast<uint64_t>(now - 45);
    failed_download.started_at = failed_download.created_at;
    failed_download.finished_at = static_cast<uint64_t>(now - 40);
    failed_download.history = {"Queued", "Downloading", "http status 403"};
    // Keep the failure near the top of the fixture so compact visual-review
    // captures exercise the recovery card without scrolling.
    st.jobs.insert(st.jobs.begin() + 1, std::move(failed_download));

    // ── Server fixture ────────────────────────────────────────────
    // Give the Servers page realistic local + cloud data so release review
    // can exercise the detail panel, cards, and cloud views.
    {
        server::ServerConfig local;
        local.name = "Forsaken World SMP";
        local.software = server::ServerSoftware::Fabric;
        local.minecraft_version = "1.20.1";
        local.allocated_ram_mb = 8192;
        local.max_players = 20;
        local.port = 25565;
        local.server_directory =
            aml::net::to_utf8(st.exe_dir) + "\\visual-fixture\\server-forsaken";
        local.stage = server::ServerStage::Running;
        local.status_message = "02:31:42 uptime";

        server::ServerConfig stopped;
        stopped.name = "Creative Build Box";
        stopped.software = server::ServerSoftware::Paper;
        stopped.minecraft_version = "1.21.1";
        stopped.allocated_ram_mb = 4096;
        stopped.max_players = 10;
        stopped.port = 25566;
        stopped.server_directory =
            aml::net::to_utf8(st.exe_dir) + "\\visual-fixture\\server-creative";
        stopped.stage = server::ServerStage::Stopped;
        st.servers = {local, stopped};

        // Give the local fixture servers a world folder and properties file so
        // the detail panel shows populated World / Properties tabs.
        for (const auto& sv : st.servers) {
            std::error_code fixture_error;
            std::filesystem::create_directories(
                std::filesystem::path(sv.server_directory) / L"world", fixture_error);
            std::ofstream props(
                std::filesystem::path(sv.server_directory) / L"server.properties",
                std::ios::binary);
            if (props) {
                props << "#Server properties generated by Amalgam\n"
                      << "online-mode=true\n"
                      << "max-players=" << sv.max_players << "\n"
                      << "difficulty=normal\n"
                      << "gamemode=survival\n";
            }
        }

        st.server_metrics.valid = true;
        st.server_metrics.ram_mb = 3277;
        st.server_metrics.ram_percent = 40.0f;
        st.server_metrics.cpu_percent = 14.0f;
        st.server_metrics.players_online = 4;
        st.server_metrics.tps = 19.8f;

        // Console log fixture for the console tab
        st.server_console_log.push_back({"[12:00:01]", "[Server thread/INFO]: Starting minecraft server version 1.20.1"});
        st.server_console_log.push_back({"[12:00:02]", "[Server thread/INFO]: Loading properties"});
        st.server_console_log.push_back({"[12:00:03]", "[Server thread/INFO]: Preparing level \"world\""});
        st.server_console_log.push_back({"[12:00:04]", "[Server thread/INFO]: Done (1.234s)! For help, type \"help\""});
        st.server_console_log.push_back({"[12:00:05]", "[Server thread/INFO]: Player connected: Alex"});

        // ── Essentials fixture ────────────────────────────────────
        // Visual-review only: gives the AAA social hub real-looking rows so
        // snapshots showcase the populated 3-column layout.
        {
            namespace ess = aml::essentials;
            auto mk_friend = [](const char* id, const char* username,
                                const char* display, ess::FriendStatus status,
                                const char* activity, const char* version,
                                int64_t last_seen) {
                ess::EssentialsFriend f;
                f.user_id = id;
                f.username = username;
                f.display_name = display;
                f.status = status;
                f.current_profile_name = activity ? activity : "";
                f.current_game_version = version ? version : "";
                f.last_seen = last_seen;
                if (status == ess::FriendStatus::Online && !activity) {
                    f.status_message = "Online";
                }
                return f;
            };
            std::vector<ess::EssentialsFriend> fixture_friends = {
                mk_friend("fixture-alex", "alex", "Alex", ess::FriendStatus::Playing,
                          "Forsaken World", "1.20.1 Forge", 0),
                mk_friend("fixture-steve", "steve", "Steve", ess::FriendStatus::InLauncher,
                          nullptr, nullptr, now - 120),
                mk_friend("fixture-sarah", "sarah", "Sarah", ess::FriendStatus::Playing,
                          "Amalgam SMP", "1.20.1 Fabric", 0),
                mk_friend("fixture-notch", "notch", "Notch", ess::FriendStatus::InLauncher,
                          nullptr, nullptr, now - 300),
                mk_friend("fixture-herobrine", "herobrine", "Herobrine",
                          ess::FriendStatus::Playing, "Skyblock", "1.21.1 Fabric", 0),
                mk_friend("fixture-ender", "enderking", "EnderKing", ess::FriendStatus::Offline,
                          nullptr, nullptr, now - 2 * 86400),
                mk_friend("fixture-creeper", "creeperboy", "CreeperBoy",
                          ess::FriendStatus::Offline, nullptr, nullptr, now - 5 * 86400),
                mk_friend("fixture-piglin", "piglintrader", "PiglinTrader",
                          ess::FriendStatus::Offline, nullptr, nullptr, now - 7 * 86400),
            };
            ess::FriendsManager::instance().seed_fixture_friends(fixture_friends);

            // Fixture notifications feed the Recent Activity panel.
            auto mk_notif = [&](const char* id, const char* title, const char* body,
                                const char* from, int64_t ago) {
                ess::EssentialsNotification n;
                n.id = id;
                n.title = title;
                n.body = body;
                n.from_username = from;
                n.created_at = now - ago;
                n.read = false;
                return n;
            };
            std::vector<ess::EssentialsNotification> fixture_notifs = {
                mk_notif("fix-n1", "Alex joined Forsaken Survival", "", "Alex", 120),
                mk_notif("fix-n2", "Steve sent you a friend request", "", "Steve", 900),
                mk_notif("fix-n3", "Sarah invited you to Amalgam SMP", "", "Sarah", 3600),
                mk_notif("fix-n4", "Herobrine left Skyblock", "", "Herobrine", 7200),
                mk_notif("fix-n5", "Notch is now playing Forsaken World", "", "Notch", 10800),
            };
            ess::SessionManager::instance().seed_fixture_notifications(fixture_notifs);
        }
    }
}


bool is_legacy_placeholder_identity(const std::string& name) {
    return name.empty() || name == "AmalgamPlayer" || name == "AmalgamUser";
}

std::string linked_account_name(UiState& st) {
    std::lock_guard<std::mutex> lock(st.auth_mu);
    return st.account.username;
}

bool has_linked_account(UiState& st) {
    // This helper drives the launcher account indicator. It represents the
    // Amalgam account, not Minecraft authentication; the official launcher
    // owns Microsoft sign-in at Play time.
    auto& supabase = aml::supabase::SupabaseManager::instance();
    if (supabase.is_initialized() && supabase.is_authenticated()) return true;
    return aml::account::AccountManager::instance().is_authenticated();
}

std::string player_display_name(UiState& st) {
    // Check if we have an Amalgam account authenticated
    auto& account_manager = aml::account::AccountManager::instance();
    if (account_manager.is_authenticated()) {
        auto profile = account_manager.get_profile();
        if (!profile.display_name.empty()) {
            return profile.display_name;
        }
        if (!profile.username.empty()) {
            return profile.username;
        }
    }
    
    // Fallback to existing check for legacy compatibility
    const std::string account_name = linked_account_name(st);
    if (!account_name.empty()) return account_name;
    return is_legacy_placeholder_identity(st.ui_username) ? std::string() : st.ui_username;
}

void set_mod_status(UiState& st, std::string value) {
    std::lock_guard<std::mutex> lock(st.mod_status_mu);
    st.mod_status = std::move(value);
}

std::string mod_status_snapshot(UiState& st) {
    std::lock_guard<std::mutex> lock(st.mod_status_mu);
    return st.mod_status;
}

void set_settings_status(UiState& st, std::string value) {
    std::lock_guard<std::mutex> lock(st.async_text_mu);
    st.settings_status = std::move(value);
}

std::string settings_status_snapshot(UiState& st) {
    std::lock_guard<std::mutex> lock(st.async_text_mu);
    return st.settings_status;
}

void sync_ui_config(UiState& st) {
    st.cfg->base_dir = net::to_wide(st.ui_base);
    st.cfg->assets_dir = net::to_wide(st.ui_assets);
    st.cfg->java_cache_dir = net::to_wide(st.ui_java_cache);
    st.cfg->username = net::to_wide(st.ui_username);
    st.cfg->test_server = net::to_wide(st.ui_server);
    st.cfg->extra_jvm = st.ui_jvm;
    save_performance_settings(*st.cfg);
    save_social_settings(*st.cfg);
    save_mod_settings(*st.cfg);
}

void push_notice(UiState& st, ui_model::NoticeLevel level, std::string title,
                 std::string body, std::string action_label,
                 std::string action_id, bool sticky) {
    ui_model::Notice notice;
    {
        std::lock_guard<std::mutex> lock(st.notice_mu);
        notice.id = st.next_notice_id++;
        notice.level = level;
        notice.title = std::move(title);
        notice.body = body.empty() ? std::string() : humanize_error(body);
        notice.action_label = std::move(action_label);
        notice.action_id = std::move(action_id);
        notice.sticky = sticky;
        st.notices.push_back(notice);
        while (st.notices.size() > 32) st.notices.pop_front();
    }
}

std::vector<ui_model::Notice> notices_snapshot(UiState& st) {
    std::lock_guard<std::mutex> lock(st.notice_mu);
    return std::vector<ui_model::Notice>(st.notices.begin(), st.notices.end());
}

void dismiss_notice(UiState& st, uint64_t id) {
    std::lock_guard<std::mutex> lock(st.notice_mu);
    st.notices.erase(std::remove_if(st.notices.begin(), st.notices.end(),
                                    [id](const ui_model::Notice& notice) {
                                        return notice.id == id;
                                    }),
                     st.notices.end());
}

bool save_ui_config(UiState& st) {
    sync_ui_config(st);
    if (!config::save(st.exe_dir + L"\\launcher.json", *st.cfg)) {
        const char* detail = st.cfg->has_unreadable_secrets
            ? "A protected credential belongs to another Windows account. Re-enter it or explicitly forget it first."
            : "Windows could not protect or write the launcher settings.";
        set_settings_status(st, detail);
        push_notice(st, ui_model::NoticeLevel::Error, "Settings could not be saved",
                    detail, "Open Settings",
                    "settings", true);
        return false;
    }
    st.settings_dirty = false;
    set_settings_status(st, "Settings saved");
    push_notice(st, ui_model::NoticeLevel::Success, "Settings saved",
                "Your launcher configuration is ready for the next launch.");
    return true;
}

void set_mod_install_log(UiState& st, std::vector<std::string> value) {
    std::lock_guard<std::mutex> lock(st.mod_status_mu);
    st.mod_install_log = std::move(value);
}

std::vector<std::string> mod_install_log_snapshot(UiState& st) {
    std::lock_guard<std::mutex> lock(st.mod_status_mu);
    return st.mod_install_log;
}

void set_pack_summary(UiState& st, std::string value) {
    std::lock_guard<std::mutex> lock(st.async_text_mu);
    st.pack_summary = std::move(value);
}

std::string pack_summary_snapshot(UiState& st) {
    std::lock_guard<std::mutex> lock(st.async_text_mu);
    return st.pack_summary;
}

void log_line(UiState& st, const std::wstring& s) {
    std::lock_guard<std::mutex> lock(st.log_mu);
    const std::string line = net::to_utf8(s);
    st.logs.push_back(line);
    while (st.logs.size() > 600) st.logs.pop_front();
    st.log_revision.fetch_add(1, std::memory_order_relaxed);
    if (!st.session_log_path.empty()) {
        std::ofstream file(std::filesystem::path(st.session_log_path),
                           std::ios::binary | std::ios::app);
        if (file) file << line << "\r\n";
    }
}

void test_curseforge_connection(UiState& st, config::Config snapshot) {
    mods::ApiCfg api = provider_config::make(snapshot);
    std::string error;
    bool ok = mods::test_provider(api, "curseforge", &error);
    if (ok)
        set_settings_status(st, "CurseForge connection successful");
    else if (!mods::curseforge_available(api))
        set_settings_status(st, mods::curseforge_proxy_configured(api)
            ? "Sign in to your Amalgam account to use the secure CurseForge catalog"
            : "Connect Amalgam online services or enter a personal CurseForge key");
    else
        set_settings_status(st, "CurseForge test failed: " +
                                (error.empty() ? "no CurseForge results" : error));
    if (ok)
        push_notice(st, ui_model::NoticeLevel::Success, "CurseForge connected",
                    "Search and installs can use the secure backend or your personal key.");
    else
        push_notice(st, ui_model::NoticeLevel::Error, "CurseForge connection failed",
                    error.empty() ? "Check Amalgam sign-in, provider access, and the network." : error,
                    "Open Settings", "settings", true);
    st.curseforge_testing = false;
}

void run_player_readiness(UiState& st, config::Config snapshot) {
    readiness::Options options;
    options.launcher_dir = st.exe_dir;
    options.java_cache_dir = snapshot.java_cache_dir.empty()
                                  ? st.exe_dir + L"\\runtimes\\java"
                                 : snapshot.java_cache_dir;
    options.provider_api = provider_config::make(snapshot);
    options.verify_providers = true;
    readiness::Report report = readiness::run(options);
    const bool java_ready = report.java_play_ready();
    const int attention = report.attention_count();
    {
        std::lock_guard<std::mutex> lock(st.readiness_mu);
        st.readiness_report = std::move(report);
        st.readiness_checked = true;
    }
    st.home_readiness.dirty = true;
    if (java_ready) {
        set_settings_status(st, "Player readiness checked: Java Edition is ready");
        push_notice(st, ui_model::NoticeLevel::Success, "Player readiness checked",
                    "Java Edition can launch. Optional or edition-specific items are listed in Settings.");
    } else {
        set_settings_status(st, "Player readiness checked: " + std::to_string(attention) +
                                    " action(s) needed");
        push_notice(st, ui_model::NoticeLevel::Info, "Player readiness needs attention",
                    "Open Settings to complete the items marked Action before launching Java Edition.",
                    "Open Settings", "settings", true);
    }
    st.readiness_testing = false;
}

ui_model::OperationSnapshot operation_snapshot(const UiState::DownloadJob& job) {
    ui_model::OperationSnapshot out;
    out.id = job.id;
    out.kind = job.kind;
    out.label = job.label;
    out.detail = job.detail;
    out.phase = job.phase;
    out.current_item = job.current_item;
    out.target_profile = job.target_profile;
    out.provider = job.provider;
    out.progress = job.progress;
    out.bytes_done = job.bytes_done;
    out.bytes_total = job.bytes_total;
    out.bytes_per_second = job.bytes_per_second;
    out.eta_seconds = job.eta_seconds;
    out.created_at = job.created_at;
    out.started_at = job.started_at;
    out.finished_at = job.finished_at;
    out.resumable = job.resumable;
    out.retryable = !job.retry_action.empty();
    out.cancel_requested = job.cancel_requested;
    out.error = job.error;
    if (job.cancelled) out.state = ui_model::OperationState::Cancelled;
    else if (job.active && job.cancel_requested) out.state = ui_model::OperationState::Cancelling;
    else if (job.active && job.queued) out.state = ui_model::OperationState::Queued;
    else if (job.active && job.paused) out.state = ui_model::OperationState::Paused;
    else if (job.active) out.state = ui_model::OperationState::Running;
    else if (job.completed) out.state = ui_model::OperationState::Completed;
    else if (job.failed) out.state = ui_model::OperationState::Failed;
    return out;
}

void persist_jobs(UiState& st) {
    if (st.exe_dir.empty()) return;
    Json root = Json::obj();
    Json entries = Json::arr();
    {
        std::lock_guard<std::mutex> lock(st.jobs_mu);
        size_t first = st.jobs.size() > 100 ? st.jobs.size() - 100 : 0;
        for (size_t i = first; i < st.jobs.size(); ++i) {
            const auto& job = st.jobs[i];
            Json entry = Json::obj();
            entry.set("id", Json::num(job.id));
            entry.set("kind", Json::str(job.kind));
            entry.set("label", Json::str(job.label));
            entry.set("detail", Json::str(job.detail));
            entry.set("phase", Json::str(job.phase));
            entry.set("current_item", Json::str(job.current_item));
            entry.set("target_profile", Json::str(job.target_profile));
            entry.set("provider", Json::str(job.provider));
            entry.set("progress", Json::num(job.progress));
            entry.set("bytes_done", Json::num(static_cast<double>(job.bytes_done)));
            entry.set("bytes_total", Json::num(static_cast<double>(job.bytes_total)));
            entry.set("bytes_per_second", Json::num(job.bytes_per_second));
            entry.set("eta_seconds", Json::num(job.eta_seconds));
            entry.set("created_at", Json::num(static_cast<double>(job.created_at)));
            entry.set("started_at", Json::num(static_cast<double>(job.started_at)));
            entry.set("finished_at", Json::num(static_cast<double>(job.finished_at)));
            entry.set("active", Json::boolean(job.active));
            entry.set("paused", Json::boolean(job.paused));
            entry.set("completed", Json::boolean(job.completed));
            entry.set("failed", Json::boolean(job.failed));
            entry.set("cancelled", Json::boolean(job.cancelled));
            entry.set("resumable", Json::boolean(job.resumable));
            entry.set("retry_action", Json::str(job.retry_action));
            entry.set("retry_payload", Json::str(job.retry_payload));
            entry.set("queued", Json::boolean(job.queued));
            entry.set("exclusive", Json::boolean(job.exclusive));
            Json history = Json::arr();
            for (const auto& step : job.history) history.push(Json::str(step));
            entry.set("history", history);
            entry.set("error", Json::str(job.error));
            entries.push(entry);
        }
    }
    root.set("jobs", entries);
    std::string error;
    const std::wstring path = st.exe_dir + L"\\downloads.json";
    const std::wstring temporary = path + L".tmp";
    if (!json_write_file(temporary, root, &error)) return;
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
        DeleteFileW(temporary.c_str());
}

void load_jobs(UiState& st) {
    Json root;
    if (!json_parse_file(st.exe_dir + L"\\downloads.json", root, nullptr)) return;
    int interrupted = 0;
    {
        std::lock_guard<std::mutex> lock(st.jobs_mu);
        for (const auto& entry : root.get("jobs").items()) {
            UiState::DownloadJob job;
            job.id = static_cast<int>(entry.get("id").as_int(0));
            job.kind = entry.get("kind").as_str();
            job.label = entry.get("label").as_str();
            job.detail = entry.get("detail").as_str();
            job.phase = entry.get("phase").as_str();
            job.current_item = entry.get("current_item").as_str();
            job.target_profile = entry.get("target_profile").as_str();
            job.provider = entry.get("provider").as_str();
            job.progress = static_cast<float>(entry.get("progress").as_num(0.0));
            job.bytes_done = static_cast<uint64_t>(entry.get("bytes_done").as_int(0));
            job.bytes_total = static_cast<uint64_t>(entry.get("bytes_total").as_int(0));
            job.bytes_per_second = entry.get("bytes_per_second").as_num(0.0);
            job.eta_seconds = entry.get("eta_seconds").as_num(-1.0);
            job.created_at = static_cast<uint64_t>(entry.get("created_at").as_int(0));
            job.started_at = static_cast<uint64_t>(entry.get("started_at").as_int(0));
            job.finished_at = static_cast<uint64_t>(entry.get("finished_at").as_int(0));
            job.active = entry.get("active").as_bool(false);
            job.paused = entry.get("paused").as_bool(false);
            job.completed = entry.get("completed").as_bool(false);
            job.failed = entry.get("failed").as_bool(false);
            job.cancelled = entry.get("cancelled").as_bool(false);
            job.resumable = entry.get("resumable").as_bool(false);
            job.retry_action = entry.get("retry_action").as_str();
            job.retry_payload = entry.get("retry_payload").as_str();
            job.queued = entry.get("queued").as_bool(false);
            job.exclusive = entry.get("exclusive").as_bool(false);
            for (const auto& step : entry.get("history").items()) {
                const std::string text = step.as_str();
                if (!text.empty() && job.history.size() < 16) job.history.push_back(text);
            }
            job.error = entry.get("error").as_str();
            if (job.active) {
                job.active = false;
                job.failed = true;
                job.detail = "Interrupted by launcher restart";
                job.error = job.detail;
                ++interrupted;
            }
            if (job.id <= 0 || job.label.empty()) continue;
            st.next_job_id = std::max(st.next_job_id, job.id + 1);
            st.jobs.push_back(std::move(job));
        }
    }
    if (interrupted > 0) {
        push_notice(st, ui_model::NoticeLevel::Warning, "Operations interrupted",
                    std::to_string(interrupted) + " operation(s) were interrupted when the launcher closed. Retry or resume them from Downloads.",
                    "Open Downloads", "downloads", true);
    }
}

bool job_cancelled(UiState& st, int id) {
    if (st.shutting_down) return true;
    std::lock_guard<std::mutex> lock(st.jobs_mu);
    for (const auto& job : st.jobs) if (job.id == id) return job.cancel_requested;
    return false;
}

// Transfers call this from their progress callback.  Pausing never discards
// an in-flight file: it waits between chunks, and closing the launcher turns
// the wait into a normal cancellation so worker shutdown cannot deadlock.
bool job_transfer_allowed(UiState& st, int id) {
    for (;;) {
        if (st.shutting_down) return false;
        bool found = false;
        bool paused = false;
        bool cancelled = false;
        {
            std::lock_guard<std::mutex> lock(st.jobs_mu);
            for (const auto& job : st.jobs) {
                if (job.id != id) continue;
                found = true;
                paused = job.paused;
                cancelled = job.cancel_requested;
                break;
            }
        }
        if (!found || cancelled) return false;
        if (!paused) return true;
        Sleep(40);
    }
}

void set_job_paused(UiState& st, int id, bool paused) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(st.jobs_mu);
        for (auto& job : st.jobs) {
            if (job.id != id || !job.active || job.cancel_requested) continue;
            if (job.paused != paused) {
                job.paused = paused;
                job.phase = paused ? "Paused safely between transfer chunks" : "Resuming transfer";
                if (job.history.empty() || job.history.back() != job.phase) {
                    job.history.push_back(job.phase);
                    if (job.history.size() > 16) job.history.erase(job.history.begin());
                }
                changed = true;
            }
            break;
        }
    }
    if (changed) persist_jobs(st);
}

int start_job(UiState& st, const std::string& label, const std::string& detail,
              const std::string& retry_action = {}, const std::string& retry_payload = {},
              bool exclusive = false) {
    int id = 0;
    {
        std::lock_guard<std::mutex> lock(st.jobs_mu);
        id = st.next_job_id++;
        UiState::DownloadJob job;
        job.id = id;
        job.kind = retry_action.empty() ? "operation" : retry_action;
        job.label = label;
        job.detail = detail;
        job.phase = exclusive ? "Queued for protected profile access" : detail;
        job.retry_action = retry_action;
        job.retry_payload = retry_payload;
        job.resumable = !retry_action.empty();
        job.queued = exclusive;
        job.exclusive = exclusive;
        job.created_at = static_cast<uint64_t>(std::time(nullptr));
        if (!exclusive) job.started_at = job.created_at;
        job.history.push_back(job.phase);
        if (!retry_payload.empty()) {
            std::string parse_error;
            Json payload = Json::parse(retry_payload, &parse_error);
            if (parse_error.empty()) {
                job.target_profile = payload.get("name").as_str();
                if (job.target_profile.empty())
                    job.target_profile = payload.get("directory").as_str();
                job.provider = payload.get("source").as_str();
                job.current_item = payload.get("slug").as_str();
            }
        }
        st.jobs.push_back(std::move(job));
    }
    persist_jobs(st);
    return id;
}

// A profile installation can touch hundreds of files and create restore
// points.  Letting two of those operations overlap is a reliable way to leave
// a profile half-updated.  This compact queue is intentionally durable: a
// restart turns a queued/running action into the existing retryable recovery
// card rather than silently resuming unknown file writes.
bool wait_for_exclusive_job(UiState& st, int id) {
    for (;;) {
        if (st.shutting_down) return false;
        bool found = false;
        bool cancelled = false;
        bool waiting_for_earlier_job = false;
        bool started = false;
        {
            std::lock_guard<std::mutex> lock(st.jobs_mu);
            for (size_t index = 0; index < st.jobs.size(); ++index) {
                auto& job = st.jobs[index];
                if (job.id != id) continue;
                found = true;
                cancelled = job.cancel_requested;
                if (!job.exclusive) return !cancelled;
                for (size_t earlier = 0; earlier < index; ++earlier) {
                    const auto& previous = st.jobs[earlier];
                    if (previous.active && previous.exclusive) {
                        waiting_for_earlier_job = true;
                        break;
                    }
                }
                if (!waiting_for_earlier_job && !cancelled && job.queued) {
                    job.queued = false;
                    job.phase = "Starting protected profile operation";
                    job.started_at = static_cast<uint64_t>(std::time(nullptr));
                    if (job.history.empty() || job.history.back() != job.phase) {
                        job.history.push_back(job.phase);
                        if (job.history.size() > 16) job.history.erase(job.history.begin());
                    }
                    started = true;
                }
                break;
            }
        }
        if (!found || cancelled) return false;
        if (started) persist_jobs(st);
        if (!waiting_for_earlier_job) return true;
        Sleep(60);
    }
}

std::string retry_payload(const std::string& slug, const std::string& source,
                          const instances::Instance& target) {
    Json payload = Json::obj();
    payload.set("slug", Json::str(slug));
    payload.set("source", Json::str(source));
    payload.set("directory", Json::str(net::to_utf8(target.directory)));
    payload.set("id", Json::str(target.id));
    payload.set("name", Json::str(target.name));
    payload.set("version", Json::str(target.minecraft_version));
    payload.set("loader", Json::str(target.loader));
    return payload.dump();
}

std::string retry_payload(const mods::SearchResult& project) {
    Json payload = Json::obj();
    payload.set("slug", Json::str(project.slug));
    payload.set("title", Json::str(project.title));
    payload.set("source", Json::str(project.source));
    return payload.dump();
}

std::string retry_payload(const instances::Instance& target) {
    Json payload = Json::obj();
    payload.set("directory", Json::str(net::to_utf8(target.directory)));
    payload.set("id", Json::str(target.id));
    payload.set("name", Json::str(target.name));
    payload.set("version", Json::str(target.minecraft_version));
    payload.set("loader", Json::str(target.loader));
    return payload.dump();
}

std::string retry_payload_published_pack(const instances::Instance& target, bool create_copy) {
    Json payload = Json::obj();
    payload.set("directory", Json::str(net::to_utf8(target.directory)));
    payload.set("id", Json::str(target.id));
    payload.set("name", Json::str(target.name));
    payload.set("version", Json::str(target.minecraft_version));
    payload.set("loader", Json::str(target.loader));
    payload.set("source", Json::str(target.pack_source));
    payload.set("slug", Json::str(target.pack_project));
    payload.set("project", Json::str(target.pack_project));
    payload.set("copy", Json::boolean(create_copy));
    return payload.dump();
}

void update_job(UiState& st, int id, float progress, const std::string& detail) {
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(st.jobs_mu);
        for (auto& job : st.jobs) {
            if (job.id != id) continue;
            job.progress = std::clamp(progress, 0.0f, 1.0f);
            if (!detail.empty()) job.detail = detail;
            if (!detail.empty()) {
                job.phase = detail;
                if (job.history.empty() || job.history.back() != detail) {
                    job.history.push_back(detail);
                    if (job.history.size() > 16) job.history.erase(job.history.begin());
                }
            }
            found = true;
            break;
        }
    }
    if (found && (progress <= 0.0f || progress >= 1.0f)) persist_jobs(st);
}

void update_job_transfer(UiState& st, int id, uint64_t done, uint64_t total) {
    const uint64_t now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    bool persist = false;
    {
        std::lock_guard<std::mutex> lock(st.jobs_mu);
        for (auto& job : st.jobs) {
            if (job.id != id) continue;
            job.bytes_done = done;
            job.bytes_total = total;
            job.progress = ui_model::fraction(done, total, job.progress);
            job.resumable = true;
            if (job.last_sample_ms > 0 && now > job.last_sample_ms && done >= job.last_sample_bytes) {
                const double seconds = static_cast<double>(now - job.last_sample_ms) / 1000.0;
                if (seconds >= 0.20) {
                    job.bytes_per_second = static_cast<double>(done - job.last_sample_bytes) / seconds;
                    if (job.bytes_per_second > 0.0 && total > done)
                        job.eta_seconds = static_cast<double>(total - done) / job.bytes_per_second;
                    job.last_sample_bytes = done;
                    job.last_sample_ms = now;
                    persist = true;
                }
            } else if (job.last_sample_ms == 0) {
                job.last_sample_bytes = done;
                job.last_sample_ms = now;
            }
            if (total > 0)
                job.phase = "Downloading " + format_bytes(done) + " / " + format_bytes(total);
            break;
        }
    }
    if (persist) persist_jobs(st);
}

void finish_job(UiState& st, int id, bool ok, const std::string& detail) {
    std::string label;
    std::string failure;
    bool cancelled = false;
    {
        std::lock_guard<std::mutex> lock(st.jobs_mu);
        for (auto& job : st.jobs) {
            if (job.id != id) continue;
            label = job.label;
            cancelled = job.cancel_requested && !ok;
            job.progress = ok ? 1.0f : job.progress;
            job.active = false;
            job.completed = ok;
            job.failed = !ok && !cancelled;
            job.cancelled = cancelled;
            job.finished_at = static_cast<uint64_t>(std::time(nullptr));
            if (!detail.empty()) job.detail = detail;
            if (!detail.empty()) {
                job.phase = detail;
                if (job.history.empty() || job.history.back() != detail) {
                    job.history.push_back(detail);
                    if (job.history.size() > 16) job.history.erase(job.history.begin());
                }
            }
            job.error = ok || cancelled ? std::string() : detail;
            failure = job.error;
            break;
        }
    }
    persist_jobs(st);
    log_line(st, net::to_wide("[job] " + (label.empty() ? std::string("operation") : label) +
                             (ok ? " complete: " : " failed: ") + detail));
    if (ok) {
        push_notice(st, ui_model::NoticeLevel::Success, label.empty() ? "Operation complete" : label,
                    detail.empty() ? "The operation completed successfully." : detail);
    } else if (cancelled) {
        push_notice(st, ui_model::NoticeLevel::Warning, label.empty() ? "Operation cancelled" : label,
                    detail.empty() ? "The operation was cancelled safely; partial data remains resumable." : detail);
    } else {
        const std::string lower_label = [&label] {
            std::string value = label;
            std::transform(value.begin(), value.end(), value.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return value;
        }();
        const bool install_related = lower_label.find("install") != std::string::npos ||
                                     lower_label.find("update") != std::string::npos ||
                                     lower_label.find("forge") != std::string::npos;
        push_notice(st, ui_model::NoticeLevel::Error, label.empty() ? "Operation failed" : label,
                    failure.empty() ? "The operation failed. Review Downloads for recovery." : failure,
                    install_related ? "Open Logs" : "Open Downloads",
                    install_related ? "logs" : "downloads", true);
    }
}

bool decode_image(const std::vector<uint8_t>& bytes, std::vector<uint8_t>& rgba, int& width,
                  int& height) {
    if (bytes.empty()) return false;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    bool ok = false;
    do {
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&factory))))
            break;
        if (FAILED(factory->CreateStream(&stream))) break;
        if (FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()),
                                                 static_cast<DWORD>(bytes.size()))))
            break;
        if (FAILED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad,
                                                    &decoder)))
            break;
        if (FAILED(decoder->GetFrame(0, &frame))) break;
        if (FAILED(frame->GetSize(reinterpret_cast<UINT*>(&width), reinterpret_cast<UINT*>(&height))))
            break;
        if (width <= 0 || height <= 0 || static_cast<uint64_t>(width) * static_cast<uint64_t>(height) > 16ull * 1024 * 1024)
            break;
        if (FAILED(factory->CreateFormatConverter(&converter))) break;
        if (FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA,
                                         WICBitmapDitherTypeNone, nullptr, 0.0,
                                         WICBitmapPaletteTypeCustom)))
            break;
        rgba.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
        if (FAILED(converter->CopyPixels(nullptr, static_cast<UINT>(width * 4),
                                         static_cast<UINT>(rgba.size()), rgba.data())))
            break;
        ok = true;
    } while (false);
    if (converter) converter->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (stream) stream->Release();
    if (factory) factory->Release();
    CoUninitialize();
    return ok;
}

HICON create_window_icon(const std::wstring& path) {
    std::ifstream file(std::filesystem::path(path), std::ios::binary);
    if (!file.is_open()) return nullptr;
    file.seekg(0, std::ios::end);
    const std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    if (size <= 0 || size > 16 * 1024 * 1024) return nullptr;
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(bytes.data()), size);
    std::vector<uint8_t> rgba;
    int width = 0;
    int height = 0;
    if (!decode_image(bytes, rgba, width, height) || width <= 0 || height <= 0) return nullptr;

    BITMAPINFO bitmap_info{};
    bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_info.bmiHeader.biWidth = 32;
    bitmap_info.bmiHeader.biHeight = -32;
    bitmap_info.bmiHeader.biPlanes = 1;
    bitmap_info.bmiHeader.biBitCount = 32;
    bitmap_info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(screen, &bitmap_info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!color || !pixels) {
        if (color) DeleteObject(color);
        return nullptr;
    }
    auto* destination = static_cast<uint8_t*>(pixels);
    for (int y = 0; y < 32; ++y) {
        const int source_y = std::min(height - 1, y * height / 32);
        for (int x = 0; x < 32; ++x) {
            const int source_x = std::min(width - 1, x * width / 32);
            const size_t source = (static_cast<size_t>(source_y) * static_cast<size_t>(width) +
                                   static_cast<size_t>(source_x)) * 4;
            const size_t target = (static_cast<size_t>(y) * 32 + static_cast<size_t>(x)) * 4;
            destination[target + 0] = rgba[source + 2];
            destination[target + 1] = rgba[source + 1];
            destination[target + 2] = rgba[source + 0];
            destination[target + 3] = rgba[source + 3];
        }
    }
    HBITMAP mask = CreateBitmap(32, 32, 1, 1, nullptr);
    if (!mask) {
        DeleteObject(color);
        return nullptr;
    }
    ICONINFO icon_info{};
    icon_info.fIcon = TRUE;
    icon_info.hbmColor = color;
    icon_info.hbmMask = mask;
    HICON icon = CreateIconIndirect(&icon_info);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

void spawn_worker(UiState& st, std::thread worker) {
    std::lock_guard<std::mutex> lock(st.workers_mu);
    st.workers.push_back(std::move(worker));
}

void join_workers(UiState& st) {
    std::vector<std::thread> workers;
    {
        std::lock_guard<std::mutex> lock(st.workers_mu);
        workers.swap(st.workers);
    }
    for (auto& worker : workers) {
        if (worker.joinable()) worker.join();
    }
}

// ---------------------------------------------------------------------------
// Launcher self-update
// ---------------------------------------------------------------------------

// Runs on a worker thread. Fetches the backend release manifest, compares
// versions, and records the result in st.update_status. Never blocks the UI.
void do_update_check(UiState& st) {
    st.update_status.state = updater::CheckState::Checking;

    updater::UpdateInfo info;
    std::string err;
    if (!updater::fetch_manifest(updater::default_manifest_url(), info, &err)) {
        // Distinguish "provider unreachable" from "provider reached but the
        // manifest is unusable" so the UI can show the right state.
        const bool provider_error = err.rfind("update manifest", 0) != 0;
        st.update_status.state = provider_error ? updater::CheckState::Offline
                                                : updater::CheckState::Error;
        st.update_status.error = err;
        return;
    }

    // parse_manifest_text fails closed when a signed manifest arrives without
    // a configured key; surface the signed state for the UI.
    st.update_status.manifest_signed = !info.manifest_signature.empty();

    const int cmp = updater::compare_versions(info.version, kVersion);
    if (cmp <= 0) {
        st.update_status.state = updater::CheckState::UpToDate;
        st.update_status.error.clear();
        return;
    }

    if (!info.min_version.empty() &&
        updater::compare_versions(kVersion, info.min_version) < 0) {
        st.update_status.state = updater::CheckState::Error;
        st.update_status.error =
            "This launcher version is too old to apply the available update.";
        return;
    }

    st.update_status.state = info.mandatory ? updater::CheckState::Mandatory
                                            : updater::CheckState::Available;
    st.update_status.available_version = info.version;
    st.update_status.notes = info.notes;
    st.update_status.error.clear();
}

// Runs on a worker thread. Re-fetches the manifest, stages the payload with
// size + SHA-256 verification, prepares the swap helper, then launches the
// helper and closes the launcher so the helper can replace the executable
// and restart. On any failure the running launcher is left untouched.
void do_update_install(UiState& st) {
    st.update_status.state = updater::CheckState::Checking;

    updater::UpdateInfo info;
    std::string err;
    if (!updater::fetch_manifest(updater::default_manifest_url(), info, &err)) {
        st.update_status.state = updater::CheckState::Error;
        st.update_status.error = "Update check failed: " + err;
        return;
    }

    // Anti-rollback: re-apply the version policy at install time so a stale
    // or tampered manifest served between the check and the install can
    // never downgrade the launcher or cross into the beta channel.
    if (!updater::should_apply(info, kVersion, kChannel, &err)) {
        st.update_status.state = updater::CheckState::Error;
        st.update_status.error = "Update refused: " + err;
        return;
    }

    std::wstring staged;
    if (!updater::stage_update(info, st.exe_dir, staged, &err)) {
        st.update_status.state = updater::CheckState::Error;
        st.update_status.error = "Update download failed: " + err;
        return;
    }

    std::wstring helper;
    if (!updater::prepare_apply(info, st.exe_dir, staged, helper, &err)) {
        st.update_status.state = updater::CheckState::Error;
        st.update_status.error = "Update could not be prepared: " + err;
        return;
    }

    st.update_status.staged = true;

    // Signature verification ran inside stage_update for signed manifests;
    // record the result so the UI can surface it.
    st.update_status.signature_verified =
        info.signature.empty() ? false : true;

    // Launch the helper (it waits for this process to exit, then swaps the
    // executable and relaunches) and close the launcher.
    ShellExecuteW(st.hwnd, L"open", helper.c_str(), nullptr,
                  st.exe_dir.c_str(), SW_HIDE);
    PostMessageW(st.hwnd, WM_CLOSE, 0, 0);
}


void start_microsoft_login(UiState& st) {
    st.wizard_open = false;
    constexpr bool kDirectMicrosoftLoginEnabled = false;
    if (!kDirectMicrosoftLoginEnabled) {
        const bool launcher_installed = official_launcher::IsOfficialLauncherInstalled();
        const bool launcher_opened = launcher_installed && official_launcher::OpenOfficialLauncher();
        std::lock_guard<std::mutex> lock(st.auth_mu);
        st.login_wizard_state = 3;
        st.login_error = launcher_opened
            ? "The official Minecraft Launcher is open. Sign in there, then return to Amalgam and press Play on a prepared profile."
            : (launcher_installed
                ? "The official Minecraft Launcher is installed, but Windows could not open it. Use Open Minecraft Launcher to try again."
                : "Microsoft approval for direct Amalgam sign-in is pending. Install the official Minecraft Launcher to sign in and play for now.");
        st.login_wizard_open = true;
        st.microsoft_login_popup_open = true;
        st.auth_working = false;
        return;
    }
    const std::string client_id = st.cfg ? st.cfg->microsoft_client_id : std::string();
    if (!auth::valid_client_id(client_id)) {
        st.login_wizard_state = 3;
        st.login_error = client_id.empty()
            ? "Microsoft sign-in needs an application ID in launcher settings."
            : "The Microsoft application ID is invalid.";
        st.login_wizard_open = true;
        st.microsoft_login_popup_open = true;
        return;
    }

    bool expected = false;
    if (!st.auth_working.compare_exchange_strong(expected, true)) {
        st.login_wizard_open = true;
        st.microsoft_login_popup_open = true;
        return;
    }
    {
        std::lock_guard<std::mutex> lock(st.auth_mu);
        st.login_wizard_state = 0;
        st.login_verification_uri.clear();
        st.login_user_code.clear();
        st.login_error.clear();
        st.login_expires_in = 0;
        st.login_wizard_open = true;
        st.microsoft_login_popup_open = true;
    }

    spawn_worker(st, std::thread([&st, client_id]() {
        auth::Account account;
        std::string error;
        const bool ok = auth::login_device(
            account, client_id,
            [&st](const std::wstring& message) { log_line(st, message); },
            &error,
            [&st](const auth::DeviceLoginPrompt& prompt) {
                std::lock_guard<std::mutex> lock(st.auth_mu);
                st.login_wizard_state = 1;
                st.login_verification_uri = prompt.verification_uri;
                st.login_user_code = prompt.user_code;
                st.login_expires_in = prompt.expires_in;
            }, false);

        std::lock_guard<std::mutex> lock(st.auth_mu);
        st.auth_working = false;
        if (ok) {
            st.account = std::move(account);
            st.auth_checked = true;
            st.auth_status = "Signed in as " + st.account.username;
            st.login_wizard_state = 2;
            st.login_error.clear();
            auth::save(st.account, nullptr);
            auto& account_manager = aml::account::AccountManager::instance();
            if (account_manager.is_authenticated())
                account_manager.link_microsoft_account(st.account.access_token);
        } else {
            st.login_wizard_state = 3;
            st.login_error = error.empty() ? "Microsoft sign-in failed." : error;
        }
    }));
}

// Bounded LRU for the image cache. Called with image_mu held. Evicts the
// oldest entries (by last_used) once the cache exceeds the cap, deleting GPU
// textures so browsing large catalogs does not leak memory. 512 entries keeps
// Discover smooth without unbounded texture growth.
static void evict_image_cache(UiState& st) {
    constexpr size_t kMaxImages = 512;
    if (st.images.size() <= kMaxImages) return;
    const uint64_t now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    std::vector<std::pair<uint64_t, std::string>> by_age;
    by_age.reserve(st.images.size());
    for (const auto& entry : st.images) {
        const uint64_t used = entry.second->last_used > 0 ? entry.second->last_used : now;
        by_age.emplace_back(used, entry.first);
    }
    std::sort(by_age.begin(), by_age.end());
    const size_t to_remove = st.images.size() - kMaxImages;
    for (size_t i = 0; i < to_remove; ++i) {
        auto it = st.images.find(by_age[i].second);
        if (it == st.images.end()) continue;
        if (it->second && it->second->texture) glDeleteTextures(1, &it->second->texture);
        st.images.erase(it);
    }
}

std::shared_ptr<UiState::Image> request_image(UiState& st, const std::string& url) {
    if (url.empty()) return nullptr;
    std::shared_ptr<UiState::Image> image;
    {
        std::lock_guard<std::mutex> lock(st.image_mu);
        auto found = st.images.find(url);
        if (found != st.images.end()) {
            found->second->last_used = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            return found->second;
        }
        evict_image_cache(st);
        image = std::make_shared<UiState::Image>();
        image->loading = true;
        image->last_used = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        st.images.emplace(url, image);
    }
    spawn_worker(st, std::thread([&st, image, url]() {
        std::vector<uint8_t> bytes;
        std::vector<uint8_t> pixels;
        std::string err;
        int width = 0;
        int height = 0;
        bool ok = net::get(net::to_wide(url), bytes, &err) &&
                  decode_image(bytes, pixels, width, height);
        std::lock_guard<std::mutex> lock(st.image_mu);
        if (ok) image->rgba = std::move(pixels);
        image->width = width;
        image->height = height;
        image->loading = false;
        image->failed = !ok;
    }));
    return image;
}

std::shared_ptr<UiState::Image> request_local_image(UiState& st, const std::wstring& path) {
    if (path.empty()) return nullptr;
    const std::string key = "file://" + net::to_utf8(path);
    std::shared_ptr<UiState::Image> image;
    {
        std::lock_guard<std::mutex> lock(st.image_mu);
        auto found = st.images.find(key);
        if (found != st.images.end()) {
            found->second->last_used = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            return found->second;
        }
        evict_image_cache(st);
        image = std::make_shared<UiState::Image>();
        image->loading = true;
        image->last_used = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        st.images.emplace(key, image);
    }
    spawn_worker(st, std::thread([&st, image, path]() {
        std::vector<uint8_t> bytes;
        std::vector<uint8_t> pixels;
        std::ifstream file(std::filesystem::path(path), std::ios::binary);
        if (file.is_open()) {
            file.seekg(0, std::ios::end);
            std::streamsize size = file.tellg();
            file.seekg(0, std::ios::beg);
            if (size > 0 && size <= 64 * 1024 * 1024) {
                bytes.resize(static_cast<size_t>(size));
                file.read(reinterpret_cast<char*>(bytes.data()), size);
            }
        }
        int width = 0;
        int height = 0;
        bool ok = decode_image(bytes, pixels, width, height);
        std::lock_guard<std::mutex> lock(st.image_mu);
        if (ok) image->rgba = std::move(pixels);
        image->width = width;
        image->height = height;
        image->loading = false;
        image->failed = !ok;
    }));
    return image;
}

ImTextureID upload_image(UiState& st, const std::shared_ptr<UiState::Image>& image) {
    if (!image) return ImTextureID_Invalid;
    std::lock_guard<std::mutex> lock(st.image_mu);
    image->last_used = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    if (!image->texture && !image->rgba.empty()) {
        glGenTextures(1, &image->texture);
        glBindTexture(GL_TEXTURE_2D, image->texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image->width, image->height, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, image->rgba.data());
        image->rgba.clear();
        image->rgba.shrink_to_fit();
    }
    return image->texture ? static_cast<ImTextureID>(image->texture) : ImTextureID_Invalid;
}

using ImageFit = ui_model::ImageFit;

struct ImagePlacement {
    ImVec2 position;
    ImVec2 size;
    ImVec2 uv_min{0.0f, 0.0f};
    ImVec2 uv_max{1.0f, 1.0f};
};

ImagePlacement place_image(const ImVec2& position, const ImVec2& size, int source_width,
                           int source_height, ImageFit fit) {
    const ui_model::ImagePlacement model = ui_model::place_image(
        position.x, position.y, size.x, size.y, source_width, source_height, fit);
    return ImagePlacement{ImVec2(model.x, model.y), ImVec2(model.width, model.height),
                          ImVec2(model.uv_min_x, model.uv_min_y),
                          ImVec2(model.uv_max_x, model.uv_max_y)};
}

ImagePlacement image_placement(UiState& st, const std::shared_ptr<UiState::Image>& image,
                               const ImVec2& position, const ImVec2& size, ImageFit fit) {
    int source_width = 0;
    int source_height = 0;
    if (image) {
        std::lock_guard<std::mutex> lock(st.image_mu);
        source_width = image->width;
        source_height = image->height;
    }
    return place_image(position, size, source_width, source_height, fit);
}

void draw_project_image(UiState& st, const std::string& url, const ImVec2& pos,
                        const ImVec2& size, ImU32 fallback) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rounding = ui_px(8.0f);
    dl->AddRectFilled(pos, pos + size, fallback, rounding);
    const auto image = request_image(st, url);
    ImTextureID texture = upload_image(st, image);
    if (texture != ImTextureID_Invalid) {
        const ImagePlacement placement = image_placement(st, image, pos, size, ImageFit::Cover);
        dl->AddImageRounded(ImTextureRef(texture), placement.position,
                            placement.position + placement.size, placement.uv_min, placement.uv_max,
                            IM_COL32_WHITE, rounding);
    }
    else {
        // Provider thumbnails load asynchronously. Give cards a deliberate
        // loading composition immediately instead of a blank colour block,
        // without inventing substitute project artwork.
        const float inset = std::max(ui_px(8.0f), std::min(size.x, size.y) * 0.12f);
        dl->AddCircleFilled(pos + ImVec2(size.x * 0.76f, size.y * 0.28f),
                            std::min(size.x, size.y) * 0.22f,
                            c32(ImVec4(k.brand_hov.x, k.brand_hov.y, k.brand_hov.z, 0.12f)), 24);
        dl->AddLine(pos + ImVec2(inset, size.y - inset),
                    pos + ImVec2(size.x * 0.46f, size.y * 0.40f),
                    c32(ImVec4(k.text.x, k.text.y, k.text.z, 0.18f)), ui_px(1.2f));
        dl->AddLine(pos + ImVec2(size.x * 0.46f, size.y * 0.40f),
                    pos + ImVec2(size.x - inset, size.y - inset),
                    c32(ImVec4(k.text.x, k.text.y, k.text.z, 0.12f)), ui_px(1.2f));
        draw_brand_mark(dl, pos + ImVec2(size.x * 0.5f, size.y * 0.5f),
                        std::min(size.x, size.y) / 42.0f);
    }
}

unsigned int instance_art_seed(const instances::Instance& instance) {
    const std::string key = instance.id.empty() ? instance.name : instance.id;
    unsigned int seed = 2166136261u;
    for (unsigned char c : key) {
        seed ^= c;
        seed *= 16777619u;
    }
    return seed;
}

// Local profiles do not always have upstream artwork.  Keep a deterministic
// procedural fallback for development and recovery, then prefer the bundled
// original Minecraft-style Amalgam covers in normal packaged builds.
void draw_instance_art_placeholder(const instances::Instance& instance, const ImVec2& pos,
                                   const ImVec2& size, float rounding = 8.0f) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const unsigned int seed = instance_art_seed(instance);
    const float tint = 0.03f * static_cast<float>(seed % 5u);
    const ImVec4 top_left(0.035f + tint, 0.056f, 0.115f + tint, 1.0f);
    const ImVec4 top_right(0.105f + tint, 0.040f, 0.205f + tint, 1.0f);
    const ImVec4 bottom_right(0.024f, 0.068f, 0.105f + tint, 1.0f);
    const ImVec4 bottom_left(0.022f, 0.038f, 0.078f + tint, 1.0f);
    dl->AddRectFilled(pos, pos + size, c32(k.surface2), rounding);
    dl->AddRectFilledMultiColor(pos, pos + size, c32(top_left), c32(top_right),
                                c32(bottom_right), c32(bottom_left));

    const float unit = std::max(1.0f, std::min(size.x, size.y));
    const ImVec2 glow_center(pos.x + size.x * (0.78f + 0.04f * static_cast<float>((seed >> 4) % 3u)),
                             pos.y + size.y * 0.30f);
    dl->AddCircleFilled(glow_center, unit * 0.25f, c32(ImVec4(k.brand_hov.x, k.brand_hov.y,
                                                              k.brand_hov.z, 0.16f)), 32);
    dl->AddCircle(glow_center, unit * 0.32f, c32(ImVec4(k.brand_hov.x, k.brand_hov.y,
                                                        k.brand_hov.z, 0.35f)), 32, ui_px(1.0f));

    const float ridge = pos.y + size.y * 0.70f;
    dl->AddTriangleFilled(ImVec2(pos.x, ridge + unit * 0.14f),
                          ImVec2(pos.x + size.x * 0.38f, ridge - unit * 0.18f),
                          ImVec2(pos.x + size.x * 0.70f, ridge + unit * 0.14f),
                          c32(ImVec4(0.035f, 0.080f, 0.135f, 0.94f)));
    dl->AddTriangleFilled(ImVec2(pos.x + size.x * 0.42f, ridge + unit * 0.15f),
                          ImVec2(pos.x + size.x * 0.73f, ridge - unit * 0.12f),
                          ImVec2(pos.x + size.x, ridge + unit * 0.15f),
                          c32(ImVec4(0.028f, 0.051f, 0.095f, 0.98f)));
    dl->AddRectFilled(ImVec2(pos.x, ridge + unit * 0.10f), pos + size,
                      c32(ImVec4(0.015f, 0.032f, 0.062f, 0.74f)));

    draw_brand_mark(dl, ImVec2(pos.x + size.x * 0.78f, pos.y + size.y * 0.39f),
                    std::max(0.65f, unit / 86.0f));
    if (size.x >= ui_px(150.0f) && size.y >= ui_px(72.0f)) {
        const char* loader = instance.loader.empty() ? "CUSTOM PROFILE" : instance.loader.c_str();
        dl->AddText(f_small, f_small->LegacySize,
                    ImVec2(pos.x + ui_px(12.0f), pos.y + ui_px(12.0f)),
                    c32(ImVec4(k.text.x, k.text.y, k.text.z, 0.78f)), loader);
    }
}

void draw_local_image(UiState& st, const std::wstring& path, const ImVec2& pos,
                      const ImVec2& size, ImU32 fallback, ImageFit fit) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rounding = ui_px(10.0f);
    dl->AddRectFilled(pos, pos + size, fallback, rounding);
    const auto image = request_local_image(st, path);
    ImTextureID texture = upload_image(st, image);
    if (texture != ImTextureID_Invalid) {
        const ImagePlacement placement = image_placement(st, image, pos, size, fit);
        dl->AddImageRounded(ImTextureRef(texture), placement.position,
                            placement.position + placement.size, placement.uv_min, placement.uv_max,
                            IM_COL32_WHITE, rounding);
    }
}

// Provider artwork is preferred whenever it exists.  Catalog results can
// legitimately arrive without a thumbnail, though, especially while a
// provider is still filling in details or when a visual review is offline.
// Use a deterministic Minecraft cover in that case so the UI remains a
// finished product instead of exposing a developer/logo loading placeholder.
std::wstring bundled_catalog_art_path(const UiState& st,
                                      const mods::SearchResult& project,
                                      int presentation_variant = -1) {
    static const wchar_t* const modpack_covers[] = {
        L"amalgam-cover-portal.png", L"amalgam-cover-forge.png",
        L"amalgam-cover-sky.png",
    };
    static const wchar_t* const content_covers[] = {
        L"amalgam-cover-sky.png", L"amalgam-cover-portal.png",
        L"amalgam-cover-forge.png",
    };
    const auto& covers = project.type == mods::ProjectType::Modpack
                             ? modpack_covers
                             : content_covers;
    const size_t cover_count = project.type == mods::ProjectType::Modpack
                                   ? (sizeof(modpack_covers) / sizeof(modpack_covers[0]))
                                   : (sizeof(content_covers) / sizeof(content_covers[0]));
    std::string key = project.source + "|" + project.slug + "|" + project.title;
    unsigned int seed = 2166136261u;
    for (unsigned char c : key) {
        seed ^= c;
        seed *= 16777619u;
    }
    seed ^= seed >> 16;
    seed *= 0x7feb352du;
    seed ^= seed >> 15;
    seed *= 0x846ca68bu;
    seed ^= seed >> 16;

    // The public providers occasionally omit an icon, including for several
    // high-profile modpacks. Rotate through the supplied Minecraft art set in
    // that case so an offline/catalog fixture does not show three copies of
    // the same cover. These are bundled assets, not mock project metadata.
    if (project.type == mods::ProjectType::Modpack) {
        static const wchar_t* const premium_covers[] = {
            L"profile-cover-fabric-ai.png", L"profile-cover-forge-ai.png",
            L"profile-cover-neoforge-ai.png", L"profile-cover-quilt-ai.png",
            L"profile-cover-vanilla-ai.png",
        };
        const size_t premium_count = sizeof(premium_covers) / sizeof(premium_covers[0]);
        // A horizontal featured rail supplies its visible index. Honor it for
        // bundled fallback artwork so adjacent cards never collapse into the
        // same scene merely because two provider IDs hash to the same slot.
        const size_t cover_index = presentation_variant >= 0
                                       ? static_cast<size_t>(presentation_variant) % premium_count
                                       : seed % premium_count;
        const std::wstring premium = st.exe_dir + L"\\branding\\ai\\" +
                                     premium_covers[cover_index];
        std::error_code premium_error;
        if (std::filesystem::exists(premium, premium_error) && !premium_error)
            return premium;
    }
    return st.exe_dir + L"\\branding\\" + covers[seed % cover_count];
}

void draw_catalog_project_art(UiState& st, const mods::SearchResult& project,
                               const ImVec2& pos, const ImVec2& size, ImU32 fallback,
                               ImageFit fit = ImageFit::Cover,
                               int presentation_variant = -1) {
    if (!project.icon_url.empty()) {
        draw_project_image(st, project.icon_url, pos, size, fallback);
        return;
    }
    const std::wstring local_art = bundled_catalog_art_path(st, project, presentation_variant);
    std::error_code error;
    if (std::filesystem::exists(local_art, error) && !error) {
        draw_local_image(st, local_art, pos, size, fallback, fit);
        return;
    }
    draw_project_image(st, {}, pos, size, fallback);
}

std::wstring bundled_instance_art_path(const UiState& st, const instances::Instance& instance) {
    static const wchar_t* const covers[] = {
        L"amalgam-cover-portal.png",
        L"amalgam-cover-forge.png",
        L"amalgam-cover-sky.png",
    };
    // Finalize the FNV seed before reducing it to three covers. Similar IDs
    // (for example imported profiles with a shared prefix) then do not all
    // land on the same artwork just because their raw hash has the same modulo.
    unsigned int seed = instance_art_seed(instance);
    seed ^= seed >> 16;
    seed *= 0x7feb352du;
    seed ^= seed >> 15;
    seed *= 0x846ca68bu;
    seed ^= seed >> 16;
    const wchar_t* cover = covers[seed % (sizeof(covers) / sizeof(covers[0]))];

    // AI-generated art is an optional enhancement. The deterministic bundled
    // cover remains the fallback so offline and credential-free packages stay
    // complete. Loader-specific art is selected only when it exists locally.
    const wchar_t* ai_cover = nullptr;
    const std::string loader = instance.loader;
    if (loader.find("fabric") != std::string::npos) ai_cover = L"profile-cover-fabric-ai.png";
    else if (loader.find("neoforge") != std::string::npos) ai_cover = L"profile-cover-neoforge-ai.png";
    else if (loader.find("forge") != std::string::npos) ai_cover = L"profile-cover-forge-ai.png";
    else if (loader.find("quilt") != std::string::npos) ai_cover = L"profile-cover-quilt-ai.png";
    else if (loader.find("bedrock") != std::string::npos) ai_cover = L"profile-cover-bedrock-ai.png";
    else if (loader.empty() || loader.find("vanilla") != std::string::npos) ai_cover = L"profile-cover-vanilla-ai.png";
    if (ai_cover) {
        const std::wstring candidate = st.exe_dir + L"\\branding\\ai\\" + ai_cover;
        std::error_code error;
        if (std::filesystem::exists(candidate, error) && !error) return candidate;
    }
    return st.exe_dir + L"\\branding\\" + cover;
}

void draw_instance_art(UiState& st, const instances::Instance& instance, const ImVec2& pos,
                       const ImVec2& size, ImU32 fallback, float rounding = 8.0f) {
    if (!instance.icon_url.empty()) {
        draw_project_image(st, instance.icon_url, pos, size, fallback);
        return;
    }

    const std::wstring art_path = bundled_instance_art_path(st, instance);
    std::error_code art_error;
    if (!art_path.empty() && std::filesystem::exists(art_path, art_error) && !art_error) {
        draw_local_image(st, art_path, pos, size, fallback, ImageFit::Cover);
        // Provider artwork always wins above; bundled covers stay clean and
        // presentation-ready when a local profile has no provider image.
        return;
    }
    draw_instance_art_placeholder(instance, pos, size, rounding);
}

bool save_bgra_png(const std::wstring& path, const std::vector<uint8_t>& pixels,
                   UINT width, UINT height) {
    if (path.empty() || width == 0 || height == 0 ||
        pixels.size() != static_cast<size_t>(width) * static_cast<size_t>(height) * 4u)
        return false;
    std::error_code filesystem_error;
    const std::filesystem::path output(path);
    if (!output.parent_path().empty())
        std::filesystem::create_directories(output.parent_path(), filesystem_error);
    if (filesystem_error) return false;
    // WIC's filename stream does not reliably replace an existing PNG. Write
    // to a process-scoped sibling and atomically replace the review artifact
    // after the encoder has released every handle.
    const std::wstring temp_path = path + L".tmp-" + std::to_wstring(GetCurrentProcessId());
    DeleteFileW(temp_path.c_str());

    const HRESULT com_status = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninitialize = SUCCEEDED(com_status);
    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* properties = nullptr;
    bool encoded = false;
    do {
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&factory)))) break;
        if (FAILED(factory->CreateStream(&stream))) break;
        if (FAILED(stream->InitializeFromFilename(temp_path.c_str(), GENERIC_WRITE))) break;
        if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))) break;
        if (FAILED(encoder->Initialize(stream, WICBitmapEncoderNoCache))) break;
        if (FAILED(encoder->CreateNewFrame(&frame, &properties))) break;
        if (FAILED(frame->Initialize(properties))) break;
        if (FAILED(frame->SetSize(width, height))) break;
        WICPixelFormatGUID pixel_format = GUID_WICPixelFormat32bppBGRA;
        if (FAILED(frame->SetPixelFormat(&pixel_format))) break;
        if (!IsEqualGUID(pixel_format, GUID_WICPixelFormat32bppBGRA)) break;
        const UINT stride = width * 4u;
        if (FAILED(frame->WritePixels(height, stride, static_cast<UINT>(pixels.size()),
                                      const_cast<BYTE*>(pixels.data())))) break;
        if (FAILED(frame->Commit())) break;
        if (FAILED(encoder->Commit())) break;
        encoded = true;
    } while (false);
    if (properties) properties->Release();
    if (frame) frame->Release();
    if (encoder) encoder->Release();
    if (stream) stream->Release();
    if (factory) factory->Release();
    if (uninitialize) CoUninitialize();
    if (!encoded) {
        DeleteFileW(temp_path.c_str());
        return false;
    }
    const bool replaced = MoveFileExW(temp_path.c_str(), path.c_str(),
                                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!replaced) DeleteFileW(temp_path.c_str());
    return replaced;
}

bool capture_gl_frame(const std::wstring& path, int width, int height) {
    if (path.empty() || width <= 0 || height <= 0) return false;
    const size_t row_bytes = static_cast<size_t>(width) * 4u;
    const size_t byte_count = row_bytes * static_cast<size_t>(height);
    if (byte_count > static_cast<size_t>(UINT_MAX)) return false;
    std::vector<uint8_t> rgba(byte_count);
    while (glGetError() != GL_NO_ERROR) {}
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    if (glGetError() != GL_NO_ERROR) return false;

    // OpenGL returns bottom-up RGBA; WIC expects top-down BGRA rows.
    std::vector<uint8_t> bgra(byte_count);
    for (int y = 0; y < height; ++y) {
        const uint8_t* source = rgba.data() + static_cast<size_t>(height - y - 1) * row_bytes;
        uint8_t* target = bgra.data() + static_cast<size_t>(y) * row_bytes;
        for (int x = 0; x < width; ++x) {
            target[x * 4 + 0] = source[x * 4 + 2];
            target[x * 4 + 1] = source[x * 4 + 1];
            target[x * 4 + 2] = source[x * 4 + 0];
            target[x * 4 + 3] = source[x * 4 + 3];
        }
    }
    return save_bgra_png(path, bgra, static_cast<UINT>(width), static_cast<UINT>(height));
}






















// ---------------------------------------------------------------------------
// Background workers (unchanged logic)
// ---------------------------------------------------------------------------
void fetch_versions(UiState& st) {
    st.fetching = true;
    std::string err;
    std::vector<model::ManifestEntry> all = model::fetch_manifest(&err);
    std::vector<model::ManifestEntry> filtered;
    for (const auto& e : all) {
        if (model::rank(e.id) >= 1120) filtered.push_back(e);
    }
    std::sort(filtered.begin(), filtered.end(), [](const model::ManifestEntry& a,
                                                   const model::ManifestEntry& b) {
        int ra = model::rank(a.id);
        int rb = model::rank(b.id);
        if (ra != rb) return ra > rb;
        return a.id > b.id;
    });
    {
        std::lock_guard<std::mutex> lock(st.version_mu);
        st.versions = std::move(filtered);
    }
    log_line(st, err.empty() ? L"[ui] manifest loaded" : L"[ui] manifest error: " +
                                                                  net::to_wide(err));
    st.fetching = false;
}

void fetch_home_catalog(UiState& st) {
    const uint64_t request_id = st.next_request_id.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(st.mod_mu);
        st.home_screen.begin(request_id);
    }
    mods::ApiCfg cfg = provider_config::make(*st.cfg);
    std::vector<mods::SearchResult> packs;
    std::vector<mods::SearchResult> mod_items;
    std::string pack_error;
    std::string mod_error;
    std::string version = st.selected;
    std::string loader = st.cfg->loader == "auto" ? "" : st.cfg->loader;
    const bool packs_ok = mods::search(cfg, "", loader, version, mods::Facet::Modpack, packs, &pack_error);
    const bool mods_ok = mods::search(cfg, "", loader, version, mods::Facet::Optimization, mod_items, &mod_error);
    bool current_request = false;
    {
        std::lock_guard<std::mutex> lock(st.mod_mu);
        if (request_id == st.home_screen.request_id) {
            current_request = true;
            st.home_packs = std::move(packs);
            st.home_mods = std::move(mod_items);
            st.home_error.clear();
            if (!packs_ok && !mods_ok) {
                st.home_error = pack_error.empty() ? mod_error : pack_error;
                st.home_screen.fail(request_id, st.home_error, true);
            } else {
                std::vector<mods::SearchResult> combined = st.home_packs;
                combined.insert(combined.end(), st.home_mods.begin(), st.home_mods.end());
                st.home_screen.accept(request_id, std::move(combined),
                                       !st.home_packs.empty() || !st.home_mods.empty());
            }
        }
    }
    if (current_request) st.home_fetching = false;
    log_line(st, (packs_ok || mods_ok) ? L"[browse] catalog loaded"
                                      : L"[browse] catalog failed: " + net::to_wide(pack_error));
}

static uint16_t find_integrated_server_port(DWORD process_id) {
    ULONG size = 0;
    if (GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0)
        != ERROR_INSUFFICIENT_BUFFER) return 0;
    std::vector<uint8_t> storage(size);
    auto* table = reinterpret_cast<PMIB_TCPTABLE_OWNER_PID>(storage.data());
    if (GetExtendedTcpTable(table, &size, FALSE, AF_INET,
                            TCP_TABLE_OWNER_PID_LISTENER, 0) != NO_ERROR) return 0;
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const auto& row = table->table[i];
        if (row.dwOwningPid == process_id) {
            return ntohs(static_cast<u_short>(row.dwLocalPort));
        }
    }
    return 0;
}

static void monitor_singleplayer_lan(UiState& st, DWORD process_id,
                                     const instances::Instance& instance) {
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, process_id);
    if (!process) return;

    bool hosting_started = false;
    while (WaitForSingleObject(process, 500) == WAIT_TIMEOUT && !st.shutting_down) {
        const uint16_t lan_port = find_integrated_server_port(process_id);
        if (hosting_started && lan_port == 0 &&
            aml::essentials::WorldHost::instance().is_hosting()) {
            aml::essentials::WorldHost::instance().stop_hosting();
            hosting_started = false;
        }
        if (!hosting_started) {
            const uint16_t port = lan_port;
            if (port != 0) {
                aml::essentials::HostOptions options;
                options.profile_id = instance.id;
                options.world_name = instance.name;
                options.server_port = port;
                hosting_started = aml::essentials::WorldHost::instance().start_hosting(
                    options, [&st](float, const std::string& status) {
                        log_line(st, L"[essentials] " + net::to_wide(status));
                    });
                if (hosting_started) {
                    push_notice(st, ui_model::NoticeLevel::Success,
                                "World Available", "Your LAN world is now available to invited friends.");
                }
            }
        }
    }

    if (hosting_started && aml::essentials::WorldHost::instance().is_hosting()) {
        aml::essentials::WorldHost::instance().stop_hosting();
    }
    CloseHandle(process);
}

std::string client_bridge_value(std::string value) {
    for (char& c : value) {
        if (c == '\r' || c == '\n') c = ' ';
    }
    return value;
}

bool write_client_profile_bridge(const instances::Instance& instance,
                                 const std::filesystem::path& client_dir,
                                 std::string* error) {
    std::error_code directory_error;
    std::filesystem::create_directories(client_dir, directory_error);
    if (directory_error) {
        if (error) *error = "Could not create the selected profile's client data directory";
        return false;
    }

    int mod_count = 0;
    for (const auto& content : instances::list_content(instance, nullptr)) {
        if (content.type == instances::ContentType::Mod) ++mod_count;
    }

    const std::filesystem::path target = client_dir / L"shared_profile.txt";
    const std::filesystem::path temporary = target.wstring() + L".tmp-" +
        std::to_wstring(GetCurrentProcessId());
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        if (error) *error = "Could not create shared_profile.txt";
        return false;
    }
    out << "# Amalgam launcher -> in-game client profile bridge\n"
        << "profile_id=" << client_bridge_value(instance.id) << "\n"
        << "profile_name=" << client_bridge_value(instance.name) << "\n"
        << "minecraft_version=" << client_bridge_value(instance.minecraft_version) << "\n"
        << "loader=" << client_bridge_value(instance.loader) << "\n"
        << "loader_version=" << client_bridge_value(instance.loader_version) << "\n"
        << "mod_count=" << mod_count << "\n"
        << "launcher_version=" << kVersion << "\n"
        << "instance_directory=" << client_bridge_value(net::to_utf8(instance.directory)) << "\n"
        << "generated_at=" << static_cast<long long>(std::time(nullptr)) << "\n";
    out.flush();
    const bool wrote = out.good();
    out.close();
    if (!wrote || !MoveFileExW(temporary.c_str(), target.c_str(),
                               MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        if (error) *error = "Could not publish shared_profile.txt";
        return false;
    }
    return true;
}

bool prepare_client_bridge_for_profile(const config::Config& snapshot,
                                       const instances::Instance& instance,
                                       std::string* error) {
    const std::filesystem::path client_dir =
        std::filesystem::path(instance.directory) / L".amalgam" / L"client";
    if (!write_client_bridge(snapshot, client_dir.wstring(), error)) return false;
    return write_client_profile_bridge(instance, client_dir, error);
}

void do_launch(UiState& st, const std::string& mc_id, const config::Config& snapshot,
               const std::wstring& instance_dir, bool dry_run = false) {
    auto& loading = aml::ui::LoadingScreen::instance();
    if (loading.is_visible()) loading.advance_step("Validating profile...");

    launch::Options opt;
    opt.mc_id = mc_id;
    opt.loader = snapshot.loader;
    opt.base_dir = snapshot.base_dir.empty() ? st.exe_dir : snapshot.base_dir;
    if (!instance_dir.empty()) opt.instance_dir = instance_dir;
    opt.assets_dir = snapshot.assets_dir.empty() ? st.exe_dir + L"\\assets" : snapshot.assets_dir;
    if (!instance_dir.empty() && snapshot.assets_dir.empty()) opt.assets_dir = instance_dir + L"\\assets";
    opt.java_cache_dir = snapshot.java_cache_dir.empty() ? st.exe_dir + L"\\runtimes\\java"
                                                         : snapshot.java_cache_dir;
    opt.username = snapshot.username.empty() ? L"Player" : snapshot.username;
    opt.width = snapshot.width;
    opt.height = snapshot.height;
    opt.extra_jvm = snapshot.extra_jvm;
    opt.test_server = snapshot.test_server;
    opt.performance_profile = snapshot.performance_profile;
    opt.addon = snapshot.addon;
    opt.java_overrides = snapshot.java_overrides;
    opt.dll_path = st.exe_dir + L"\\amalgam.dll";
    opt.bridges_dir = st.exe_dir + L"\\bridges";
    opt.dry_run = dry_run;
    opt.require_account = !dry_run;
    instances::Instance loaded_instance;
    bool loaded_profile = false;
    if (!instance_dir.empty()) {
        if (instances::load(instance_dir, loaded_instance, nullptr)) {
            loaded_profile = true;
            if (!loaded_instance.loader.empty() && loaded_instance.loader != "auto") opt.loader = loaded_instance.loader;
            if (!loaded_instance.loader_version.empty()) opt.loader_version = net::to_wide(loaded_instance.loader_version);
            if (!loaded_instance.java_path.empty()) opt.java_path = net::to_wide(loaded_instance.java_path);
            if (loaded_instance.memory_mb > 0) opt.memory_mb = loaded_instance.memory_mb;
            if (!loaded_instance.performance_profile.empty()) opt.performance_profile = loaded_instance.performance_profile;
        }
    }

    if (!dry_run && loaded_profile && opt.addon) {
        std::string bridge_error;
        if (!prepare_client_bridge_for_profile(snapshot, loaded_instance, &bridge_error)) {
            log_line(st, L"[client] profile sync warning: " + net::to_wide(bridge_error));
            push_notice(st, ui_model::NoticeLevel::Warning, "Client sync needs attention",
                        bridge_error + ". Minecraft can still launch, but some in-game client pages may be empty.");
        } else {
            log_line(st, L"[client] profile data synchronized");
        }
    }    // Launch routing.  "official_launcher" prepares the profile, loader,
    // and client bridge, then hands off to the official Minecraft Launcher,
    // which signs the player in with its own Microsoft account.  "microsoft"
    // uses the player's Amalgam-connected Microsoft account and starts the
    // game directly.  The handoff is also the fallback whenever no Microsoft
    // account is connected, so Play always has a working path.
    auth::Account launch_account;
    const bool microsoft_signed_in = !dry_run && auth::load(launch_account, nullptr) &&
                                     auth::valid_client_id(launch_account.microsoft_client_id) &&
                                     launch_account.expires_at > std::time(nullptr);
    const bool handoff = !dry_run && loaded_profile &&
        (snapshot.launch_mode != "microsoft" || !microsoft_signed_in);
    if (handoff) {
        if (!official_launcher::IsOfficialLauncherInstalled()) {
            if (loading.is_visible()) loading.hide();
            st.running = false;
            push_notice(st, ui_model::NoticeLevel::Warning,
                        "Minecraft Launcher Not Found",
                        "Install the official Minecraft Launcher from minecraft.net\n"
                        "to sign in and play. It handles authentication automatically.");
            return;
        }
        const int java_major = model::default_java_major(model::rank(loaded_instance.minecraft_version));
        auto prepared = official_launcher::FromInstance(loaded_instance, java_major);
        prepared.native_agent_path = opt.addon && net::file_exists(opt.dll_path)
            ? opt.dll_path : L"";
        if (opt.addon) {
            const std::wstring bridge_loader = loaded_instance.loader.empty() || loaded_instance.loader == "auto"
                ? net::to_wide(snapshot.loader) : net::to_wide(loaded_instance.loader);
            const std::wstring bridge_name = L"\\bridges\\amalgam-" + bridge_loader +
                L"-" + net::to_wide(loaded_instance.minecraft_version) + L".jar";
            prepared.bridge_jar_path = st.exe_dir + bridge_name;
            if (!net::file_exists(prepared.bridge_jar_path) && loaded_instance.loader == "quilt") {
                prepared.bridge_jar_path = st.exe_dir + L"\\bridges\\amalgam-fabric-" +
                    net::to_wide(loaded_instance.minecraft_version) + L".jar";
            }
            if (!net::file_exists(prepared.bridge_jar_path)) prepared.bridge_jar_path.clear();
        }
        std::string bridge_error;
        if (prepared.java_executable.empty()) {
            java::JavaRuntimeManager runtime_manager(opt.java_cache_dir);
            const std::wstring java_home = runtime_manager.Resolve(
                java_major, java::scan_installed(), {}, &bridge_error);
            if (!java_home.empty()) prepared.java_executable = java_home + L"\\bin\\java.exe";
        }
        if (prepared.java_executable.empty()) {
            if (loading.is_visible()) loading.hide();
            st.running = false;
            push_notice(st, ui_model::NoticeLevel::Error, "Java Required",
                        "A Java runtime is needed to install mod loaders.\n"
                        "Go to Settings > Java to install one.");
            return;
        }
        // Step 1: Prepare the game directory structure and bridge mod.
        if (!official_launcher::PrepareProfile(prepared, &bridge_error)) {
            if (loading.is_visible()) loading.hide();
            st.running = false;
            push_notice(st, ui_model::NoticeLevel::Error, "Profile preparation failed",
                        bridge_error.empty() ? "Could not prepare the profile directory." : bridge_error);
            return;
        }
        // Step 2: Install the mod loader (Forge/Fabric/NeoForge/Quilt) into
        // the official launcher's .minecraft/ directory.  This downloads and
        // runs the legitimate installer so the official launcher can find
        // the required version JSON and libraries.
        if (loading.is_visible()) loading.advance_step("Installing mod loader");
        log_line(st, L"[launch] preparing loader for official launcher");
        {
            std::string install_error;
            auto install_progress = [&](float pct, const std::string& msg) -> bool {
                if (!msg.empty()) log_line(st, L"[loader] " + net::to_wide(msg));
                return !st.shutting_down;
            };
            if (!official_launcher::InstallLoader(prepared, install_progress, &install_error)) {
                if (loading.is_visible()) loading.hide();
                st.running = false;
                push_notice(st, ui_model::NoticeLevel::Error, "Loader installation failed",
                            install_error.empty() ? "The mod loader could not be installed." : install_error);
                return;
            }
        }
        // Step 3: Register the profile in the official launcher's launcher_profiles.json.
        if (!official_launcher::RegisterInstallation(prepared, &bridge_error)) {
            if (loading.is_visible()) loading.hide();
            st.running = false;
            push_notice(st, ui_model::NoticeLevel::Error, "Registration failed",
                        bridge_error.empty() ? "Could not register with the Minecraft Launcher." : bridge_error);
            return;
        }
        // Step 4: Open the official Minecraft Launcher.
        if (!official_launcher::OpenOfficialLauncher()) {
            if (loading.is_visible()) loading.hide();
            st.running = false;
            push_notice(st, ui_model::NoticeLevel::Error, "Could not open Minecraft Launcher",
                        "The Minecraft Launcher could not be opened.");
            return;
        }
        if (loading.is_visible()) loading.hide();
        st.running = false;
        push_notice(st, ui_model::NoticeLevel::Success, "Profile Ready",
                    loaded_instance.name + " has been prepared.\n\n"
                    "The Minecraft Launcher will open. Select the Amalgam profile\n"
                    "and press Play to start.",
                    "Open Minecraft Launcher", "open_mc_launcher");
        return;
    }

    if (loading.is_visible()) loading.advance_step("Building classpath");

    // Microsoft mode launches with the player's connected account.  Refresh
    // if needed (ensure_valid handles the device-account token lifecycle) so
    // a session that outlived the access token still plays.
    if (!dry_run && snapshot.launch_mode == "microsoft") {
        std::string account_error;
        if (!auth::ensure_valid(launch_account, [&](const std::wstring& s) { log_line(st, s); },
                                &account_error)) {
            if (loading.is_visible()) loading.hide();
            st.running = false;
            push_notice(st, ui_model::NoticeLevel::Warning, "Microsoft sign-in needed",
                        account_error.empty()
                            ? "Connect a Microsoft account to play in Amalgam mode."
                            : account_error,
                        "Sign in", "open_signin");
            return;
        }
        opt.auth_access_token = launch_account.access_token;
        opt.auth_uuid = launch_account.uuid;
        opt.auth_user_type = "msa";
    }

    std::string err;
    launch::Result res;
    bool ok = launch::run(opt, [&](const std::wstring& s) { log_line(st, s); }, &res, &err);

    if (loading.is_visible()) {
        if (ok) {
            loading.advance_step("Starting Minecraft process");
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            loading.advance_step("Game launched successfully!");
            std::this_thread::sleep_for(std::chrono::milliseconds(800));
        }
        loading.hide();
    }

    st.running = false;
    if (!ok) {
        log_line(st, L"[launch] FAILED: " + net::to_wide(err));
        push_notice(st, ui_model::NoticeLevel::Error, "Minecraft could not launch",
                    err.empty() ? "Review the launch log and preflight checks." : err,
                    "Open Downloads", "downloads", true);
        return;
    }
    log_line(st, L"[launch] game running (pid " + std::to_wstring(res.pid) + L")");
    push_notice(st, ui_model::NoticeLevel::Success, "Minecraft launched",
                "The game process is running.");

    if (!instance_dir.empty() && res.pid != 0) {
        instances::Instance instance;
        if (instances::load(instance_dir, instance, nullptr)) {
            spawn_worker(st, std::thread([&st, pid = res.pid, instance]() {
                monitor_singleplayer_lan(st, pid, instance);
            }));
        }
    }
}

void export_profile_manifest(UiState& st, const instances::Instance& instance) {
    Json manifest = Json::obj();
    manifest.set("format", Json::num(1));
    manifest.set("name", Json::str(instance.name));
    manifest.set("minecraft_version", Json::str(instance.minecraft_version));
    manifest.set("loader", Json::str(instance.loader));
    manifest.set("loader_version", Json::str(instance.loader_version));
    manifest.set("performance_profile", Json::str(instance.performance_profile));
    manifest.set("memory_mb", Json::num(instance.memory_mb));
    Json files = Json::arr();
    for (const auto& entry : instances::list_content(instance, nullptr)) {
        Json file = Json::obj();
        file.set("path", Json::str(entry.filename));
        file.set("type", Json::str(instances::content_type_name(entry.type)));
        file.set("enabled", Json::boolean(entry.enabled));
        file.set("size", Json::num(static_cast<double>(entry.size)));
        files.push(file);
    }
    manifest.set("content", files);
    std::wstring folder = st.exe_dir + L"\\exports";
    net::mkdirs(folder);
    std::wstring path = folder + L"\\" + net::to_wide(instance.id) + L"-profile.json";
    std::string error;
    bool manifest_ok = json_write_file(path, manifest, &error);
    std::wstring mrpack = folder + L"\\" + net::to_wide(instance.id) + L".mrpack";
    bool pack_ok = import_pack::export_mrpack(instance, mrpack, &error);
    std::wstring curseforge = folder + L"\\" + net::to_wide(instance.id) + L"-curseforge.zip";
    bool curseforge_ok = import_pack::export_curseforge(instance, curseforge, &error);
    if (manifest_ok && pack_ok && curseforge_ok)
        log_line(st, L"[profile] exported " + path + L", " + mrpack + L", and " + curseforge);
    else
        log_line(st, L"[profile] export failed: " + net::to_wide(error));
}

void open_instance_detail(UiState& st, const instances::Instance& instance, int tab = 0) {
    st.selected_instance = instance;
    st.active_instance_dir = instance.directory;
    st.instance_detail_open = true;
    st.instance_detail_tab = std::clamp(tab, 0, 7);
    st.sidebar_item = 3;
    st.active_tab = 6;
}

void duplicate_profile(UiState& st, const instances::Instance& instance) {
    instances::Instance duplicate;
    const std::wstring root = (st.cfg->base_dir.empty() ? st.exe_dir : st.cfg->base_dir) +
                              L"\\instances";
    std::string error;
    if (!instances::duplicate(instance, root, instance.name + " Copy", duplicate, &error)) {
        st.content_status = error.empty() ? "Could not duplicate profile" : error;
        push_notice(st, ui_model::NoticeLevel::Error, "Profile could not be duplicated",
                    st.content_status);
        return;
    }
    st.instances_loaded = false;
    st.content_status = "Profile duplicated";
    push_notice(st, ui_model::NoticeLevel::Success, "Profile duplicated",
                "A separate copy is ready in your Library.", "Open Library", "library");
}

void draw_instance_overflow_menu(UiState& st, instances::Instance& instance,
                                 const char* popup_id) {
    if (!ImGui::BeginPopup(popup_id)) return;
    ImGui::PushFont(f_small);
    // ── Primary actions ──────────────────────────────────────────
    if (ImGui::MenuItem("Play")) {
        st.selected = instance.minecraft_version;
        st.pending_id = st.selected;
        st.active_instance_dir = instance.directory;
        st.pending_instance_dir = instance.directory;
        st.pending_launch = true;
    }
    if (ImGui::MenuItem("Open")) open_instance_detail(st, instance);
    if (ImGui::MenuItem("Edit")) open_instance_detail(st, instance, 6);
    ImGui::Separator();
    // ── Management ───────────────────────────────────────────────
    if (ImGui::MenuItem(instance.favorite ? "Unfavorite" : "Favorite")) {
        instance.favorite = !instance.favorite;
        std::string error;
        if (!instances::save(instance, &error)) {
            instance.favorite = !instance.favorite;
            log_line(st, L"[instances] save failed: " + net::to_wide(error));
        }
    }
    if (ImGui::MenuItem("Duplicate")) duplicate_profile(st, instance);
    if (ImGui::MenuItem("Export")) export_profile_manifest(st, instance);
    if (ImGui::MenuItem("Open Folder"))
        ShellExecuteW(st.hwnd, L"open", instance.directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    ImGui::Separator();
    // ── Maintenance ──────────────────────────────────────────────
    if (ImGui::MenuItem("Scan & Repair") && !st.running) {
        config::Config snapshot = *st.cfg;
        instances::Instance target = instance;
        spawn_worker(st, std::thread([&st, snapshot, target]() {
            std::wstring rp; std::string be;
            if (!instances::create_restore_point(target, &rp, &be)) {
                log_line(st, L"[profile] repair cancelled: " + net::to_wide(be)); return;
            }
            log_line(st, L"[profile] restore point: " + rp);
            do_launch(st, target.minecraft_version, snapshot, target.directory, true);
        }));
    }
    if (ImGui::MenuItem("Restore Latest")) {
        std::string error;
        if (instances::restore_latest(instance, &error)) {
            st.content_status = "Restored";
            st.instances_loaded = false;
        } else st.content_status = error;
    }
    if (ImGui::MenuItem("Deploy to Cloud")) {
        ShellExecuteW(st.hwnd, L"open",
                      aml::net::to_wide(aml::online::config().cloud_url()).c_str(),
                      nullptr, nullptr, SW_SHOWNORMAL);
    }
    ImGui::Separator();
    // ── Danger zone ──────────────────────────────────────────────
    ImGui::PushStyleColor(ImGuiCol_Text, k.red);
    if (ImGui::MenuItem("Delete")) {
        st.delete_target = instance.id;
        ImGui::OpenPopup("##confirm_delete_profile");
    }
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::EndPopup();

    // Nested delete confirmation
    if (ImGui::BeginPopup("##confirm_delete_profile", ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Delete \"%s\"?", instance.name.empty() ? instance.id.c_str() : instance.name.c_str());
        ImGui::TextColored(k.muted, "This moves the profile to recovery (not permanent).");
        if (primary_button("Delete", ImVec2(ui_px(90.0f), ui_px(30.0f)))) {
            std::string error;
            if (instances::remove(instance, &error)) {
                push_notice(st, ui_model::NoticeLevel::Success, "Profile moved to recovery",
                            "Your profile is safely recoverable.", "Open Library", "library");
                st.instance_detail_open = false;
                st.instances_loaded = false;
            } else log_line(st, L"[instances] delete failed: " + net::to_wide(error));
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ghost_button("Cancel", ImVec2(ui_px(80.0f), ui_px(30.0f))))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

std::string profile_activity_label(const instances::Instance& instance) {
    if (instance.last_played <= 0) return "Not played yet";
    const int64_t age = std::max<int64_t>(0, static_cast<int64_t>(std::time(nullptr)) -
                                             instance.last_played);
    if (age < 60) return "Played just now";
    if (age < 60 * 60) return "Played " + std::to_string(age / 60) + "m ago";
    if (age < 24 * 60 * 60) return "Played " + std::to_string(age / (60 * 60)) + "h ago";
    return "Played " + std::to_string(age / (24 * 60 * 60)) + "d ago";
}

std::vector<std::string> profile_health(const UiState& st, const instances::Instance& instance) {
    std::vector<std::string> issues;
    if (instance.minecraft_version.empty()) issues.push_back("Minecraft version is not configured");
    if (instance.loader.empty() || instance.loader == "auto") issues.push_back("Loader is not pinned");
    if (!net::directory_exists(instance.directory)) issues.push_back("Instance directory is missing");
    // Vanilla profiles do not need a mods directory; flag it only for loaders
    // that actually consume one so a clean vanilla profile stays healthy.
    if (!instance.loader.empty() && instance.loader != "auto" && instance.loader != "vanilla" &&
        !net::directory_exists(instance.directory + L"\\mods"))
        issues.push_back("Mods directory is missing");
    if (!st.fixture_mode && !instance.minecraft_version.empty() && !instance.loader.empty() &&
        instance.loader != "vanilla") {
        std::wstring bridge = st.exe_dir + L"\\bridges\\amalgam-" + net::to_wide(instance.loader) + L"-" +
                              net::to_wide(instance.minecraft_version) + L".jar";
        if (instance.loader == "quilt" && !net::file_exists(bridge))
            bridge = st.exe_dir + L"\\bridges\\amalgam-fabric-" + net::to_wide(instance.minecraft_version) + L".jar";
        if (!net::file_exists(bridge)) issues.push_back("Amalgam bridge is unavailable for this profile");
    }
    std::error_code ec;
    if (std::filesystem::exists(std::filesystem::path(instance.directory) / L".amalgam-install.json", ec)) {
        // Installer metadata is present; the launch preparation path will verify the actual files.
    } else if (instance.loader == "forge" || instance.loader == "neoforge") {
        issues.push_back("Loader installation has not been pinned");
    }
    return issues;
}

std::vector<UiState::LaunchCheck> evaluate_launch(const UiState& st, const std::string& mc_id,
                                                  const std::wstring& instance_dir) {
    std::vector<UiState::LaunchCheck> checks;
    auto add = [&](std::string label, std::string detail, bool passed, bool blocking) {
        checks.push_back(UiState::LaunchCheck{std::move(label), std::move(detail), passed, blocking});
    };
    add("Minecraft version", mc_id.empty() ? "Choose a Minecraft version first." : mc_id,
        !mc_id.empty(), true);

    instances::Instance instance;
    const bool has_instance = !instance_dir.empty() && instances::load(instance_dir, instance, nullptr);
    if (!instance_dir.empty()) {
        const bool exists = net::directory_exists(instance_dir);
        add("Profile directory", exists ? net::to_utf8(instance_dir) :
             "The selected profile directory is missing.", exists, true);
        if (has_instance) {
            const auto issues = profile_health(st, instance);
            if (issues.empty()) {
                add("Profile health", "Version, loader, and managed content are ready.", true, false);
            } else {
                for (const auto& issue : issues)
                    add("Profile health", issue, false, false);
            }
        }
    } else {
        add("Profile target", "A temporary/default instance will be used.", true, false);
    }

    if (st.cfg->addon) {
        const std::wstring dll = st.exe_dir + L"\\amalgam.dll";
        add("Native bridge", net::file_exists(dll) ? "amalgam.dll is present" :
            "amalgam.dll is missing from the launcher folder.", net::file_exists(dll), true);
    } else {
        add("Native bridge", "Optional native bridge is disabled in Settings.", true, false);
    }

    const std::string loader = has_instance && !instance.loader.empty() ? instance.loader : st.cfg->loader;
    if (!loader.empty() && loader != "auto" && loader != "vanilla") {
        const std::wstring bridges = st.exe_dir + L"\\bridges";
        add("Loader bridge", net::directory_exists(bridges) ?
            "Bridge directory is available; the launch step will resolve the exact version." :
            "Bridge directory is missing. Build or install the supported bridge first.",
            net::directory_exists(bridges), true);
    }

    auth::Account account;
    std::string account_error;
    const bool signed_in = auth::load(account, &account_error) && !account.access_token.empty();
    if (signed_in) {
        add("Microsoft account", "A saved account is available.", true, false);
    } else {
        add("Minecraft authentication", "The official Minecraft Launcher will handle Microsoft sign-in.",
            official_launcher::IsOfficialLauncherInstalled(), true);
    }

    const auto java = java::scan_installed();
    const std::wstring java_cache = st.cfg->java_cache_dir.empty() ? st.exe_dir + L"\\runtimes\\java" :
                                    st.cfg->java_cache_dir;
    java::JavaRuntimeManager runtime_manager(java_cache);
    const bool cached_java = !runtime_manager.GetInstalledRuntimes().empty();
    const bool java_ready = !java.empty() || cached_java;
    add("Java runtime", java_ready ?
        "A system or cached Java runtime is available." :
        "Java will need to be provisioned before this profile can launch.",
        java_ready, false);

    ULARGE_INTEGER free_bytes{}, total_bytes{}, total_free_bytes{};
    const std::wstring disk_root = instance_dir.empty() ? st.exe_dir : instance_dir;
    const bool disk_ok = GetDiskFreeSpaceExW(disk_root.c_str(), &free_bytes, &total_bytes,
                                             &total_free_bytes) != FALSE;
    if (disk_ok) {
        const bool enough = free_bytes.QuadPart >= 2ull * 1024ull * 1024ull * 1024ull;
        add("Disk space", format_bytes(free_bytes.QuadPart) + " free",
            enough, false);
    } else {
        add("Disk space", "Could not read free space for the selected target.", true, false);
    }
    return checks;
}

bool has_failed_launch_check(const std::vector<UiState::LaunchCheck>& checks, bool blocking_only) {
    return std::any_of(checks.begin(), checks.end(), [blocking_only](const UiState::LaunchCheck& check) {
        return !check.passed && (!blocking_only || check.blocking);
    });
}

// Recompute the Home readiness summary off the render thread. evaluate_launch
// probes every installed JDK with a java.exe -version subprocess, so doing
// this inline on the draw path froze navigation for seconds whenever the
// summary went stale. A generation counter discards results produced before
// the latest profile change.
void refresh_home_readiness_async(UiState& st) {
    if (st.home_readiness.computing.exchange(true)) return;
    const uint64_t generation = st.home_readiness.generation.load();
    readiness::Report report;
    bool checked = false;
    {
        std::lock_guard<std::mutex> lock(st.readiness_mu);
        report = st.readiness_report;
        checked = st.readiness_checked;
    }
    // Callers run on the render thread, so this copy is taken without racing
    // the instance-list writers (which are also render-thread only).
    std::vector<instances::Instance> instances = st.instance_list;
    spawn_worker(st, std::thread([&st, report, checked, generation,
                                  instances = std::move(instances)]() {
        int ready = 0;
        int attention = 0;
        std::string first_issue;
        if (checked) {
            for (const auto& instance : instances) {
                if (st.home_readiness.generation.load() != generation) break;
                const auto issues = profile_health(st, instance);
                if (!issues.empty()) {
                    ++attention;
                    if (first_issue.empty()) {
                        const std::string name = instance.name.empty() ? instance.id : instance.name;
                        first_issue = name + ": " + issues.front();
                    }
                    continue;
                }
                const auto checks = evaluate_launch(st, instance.minecraft_version, instance.directory);
                const bool checks_pass = !has_failed_launch_check(checks, false);
                const auto failed = std::find_if(checks.begin(), checks.end(),
                                                 [](const UiState::LaunchCheck& check) {
                                                     return !check.passed;
                                                 });
                if (checks_pass && report.java_play_ready()) {
                    ++ready;
                } else {
                    ++attention;
                    if (first_issue.empty()) {
                        const std::string name = instance.name.empty() ? instance.id : instance.name;
                        first_issue = name + ": " +
                            (failed == checks.end() ? "player readiness needs attention" : failed->detail);
                    }
                }
            }
        }
        if (st.home_readiness.generation.load() == generation) {
            std::lock_guard<std::mutex> lock(st.home_readiness.mu);
            st.home_readiness.ready_profiles = ready;
            st.home_readiness.attention_profiles = attention;
            st.home_readiness.first_issue = std::move(first_issue);
        }
        st.home_readiness.computing.store(false);
        // Keep asking for a recompute when the data changed while we worked.
        st.home_readiness.dirty.store(st.home_readiness.generation.load() != generation);
    }));
}

void mark_home_readiness_dirty(UiState& st) {
    st.home_readiness.generation.fetch_add(1);
    st.home_readiness.dirty.store(true);
}

void start_pending_launch(UiState& st) {
    auto& loading = aml::ui::LoadingScreen::instance();
    loading.show_fullscreen("Launching Minecraft", "Getting ready...");
    loading.begin_steps({"Validate Profile", "Prepare Game Files", "Install Loader", "Launch Minecraft"});

    st.running = true;
    st.cfg->base_dir = net::to_wide(st.ui_base);
    st.cfg->assets_dir = net::to_wide(st.ui_assets);
    st.cfg->java_cache_dir = net::to_wide(st.ui_java_cache);
    st.cfg->username = net::to_wide(st.ui_username);
    st.cfg->test_server = net::to_wide(st.ui_server);
    st.cfg->extra_jvm = st.ui_jvm;
    config::Config snap = *st.cfg;
    std::string id = st.pending_id;
    std::wstring instance_dir = st.pending_instance_dir;
    st.pending_instance_dir.clear();
    st.launch_review_approved = false;
    spawn_worker(st, std::thread([&st, snap, id, instance_dir]() {
        do_launch(st, id, snap, instance_dir);
    }));
}

void mark_profile_modified(UiState& st, instances::Instance& instance) {
    if (instance.pack_source.empty() || instance.pack_modified) return;
    instance.pack_modified = true;
    std::string error;
    if (!instances::save(instance, &error))
        log_line(st, L"[profile] could not mark profile modified: " + net::to_wide(error));
    st.instances_loaded = false;
}

void show_browse(UiState& st, std::wstring* target) {
    wchar_t path[MAX_PATH] = {0};
    if (!target->empty()) {
        wcscpy_s(path, target->c_str());
    }
    BROWSEINFOW bi{};
    bi.hwndOwner = st.hwnd;
    bi.lpszTitle = L"Select folder";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        if (SHGetPathFromIDListW(pidl, path)) *target = path;
        CoTaskMemFree(pidl);
    }
}

bool show_open_archive(UiState& st, std::string& target) {
    wchar_t path[MAX_PATH] = {};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = st.hwnd;
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrFilter = L"Modpack archives\0*.mrpack;*.zip\0All files\0*.*\0\0";
    dialog.lpstrTitle = L"Import modpack archive";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&dialog)) return false;
    target = net::to_utf8(path);
    return true;
}

bool show_open_content(UiState& st, std::string& target) {
    wchar_t path[MAX_PATH] = {};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = st.hwnd;
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrFilter = L"Java mods and content\0*.jar;*.zip\0Java mods\0*.jar\0ZIP content\0*.zip\0All files\0*.*\0\0";
    dialog.lpstrTitle = L"Import local profile content";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&dialog)) return false;
    target = net::to_utf8(path);
    return true;
}

bool show_open_addon(UiState& st, std::string& target) {
    wchar_t path[MAX_PATH] = {};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = st.hwnd;
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrFilter = L"Bedrock addons\0*.mcpack;*.mcaddon\0All files\0*.*\0\0";
    dialog.lpstrTitle = L"Import Bedrock addon";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&dialog)) return false;
    target = net::to_utf8(path);
    return true;
}

void do_mod_search(UiState& st, uint64_t request_id) {
    st.mod_fetching = true;
    const int page = std::max(0, st.browse_page);
    set_mod_status(st, page == 0 ? "Searching Modrinth and CurseForge..."
                                 : "Loading more provider results...");
    mods::ApiCfg cfg = provider_config::make(*st.cfg);
    std::string query = st.mod_query;
    std::string loader = st.mod_loader;
    std::string gv;
    if (!st.mod_version.empty()) {
        gv = st.mod_version;
    } else if (!st.selected.empty() && st.selected.size() >= 4 && st.selected[0] == '1' &&
        st.selected.find('.') != std::string::npos) {
        gv = st.selected;
    }
    mods::Facet facet = static_cast<mods::Facet>(st.mod_facet);
    std::vector<mods::SearchResult> results;
    std::string err;
    const std::string requested_source = st.browse_source == 1 ? "modrinth" :
                                         st.browse_source == 2 ? "curseforge" : "";
    bool ok = mods::search(cfg, query, loader, gv, facet, results, &err, page,
                           requested_source);
    int modrinth_count = 0;
    int curseforge_count = 0;
    for (const auto& result : results) {
        if (result.source == "curseforge") ++curseforge_count;
        else if (result.source == "modrinth") ++modrinth_count;
    }
    bool accepted = false;
    {
        std::lock_guard<std::mutex> lock(st.mod_mu);
        if (request_id == st.mod_screen.request_id) {
            if (page == 0) {
                st.mod_results = std::move(results);
                st.mod_selected = -1;
            } else {
                for (auto& result : results) {
                    const bool duplicate = std::any_of(st.mod_results.begin(), st.mod_results.end(),
                        [&](const mods::SearchResult& existing) {
                            return existing.source == result.source && existing.slug == result.slug;
                        });
                    if (!duplicate) st.mod_results.push_back(std::move(result));
                }
            }
            if (ok) {
                if (page == 0) {
                    st.modrinth_result_count = modrinth_count;
                    st.curseforge_result_count = curseforge_count;
                } else {
                    st.modrinth_result_count += modrinth_count;
                    st.curseforge_result_count += curseforge_count;
                }
                st.mod_screen.accept(request_id, st.mod_results, !st.mod_results.empty());
            } else {
                st.mod_screen.fail(request_id, err.empty() ? "Provider search failed" : err, true);
            }
            accepted = true;
        }
    }
    if (!accepted) return;
    st.mod_fetching = false;
    set_mod_status(st, ok ? "Search complete" : "Search failed: " + err);
    if (!ok)
        push_notice(st, ui_model::NoticeLevel::Error, "Provider search failed",
                    err.empty() ? "Try again or check provider settings." : err,
                    "Open Discover", "discover", true);
    log_line(st, ok ? L"[mods] search done" : L"[mods] search failed: " + net::to_wide(err));
}

void launch_mod_search(UiState& st) {
    uint64_t request_id = 0;
    {
        std::lock_guard<std::mutex> lock(st.mod_mu);
        request_id = st.next_request_id++;
        st.mod_screen.begin(request_id);
    }
    // Set this before the worker starts so the first visible frame shows a
    // loading state instead of briefly claiming that the catalog is empty.
    st.mod_fetching = true;
    spawn_worker(st, std::thread(do_mod_search, std::ref(st), request_id));
}

void request_project_translation(UiState& st, const mods::SearchResult& project,
                                 const mods::ModInfo& info, bool player_requested) {
    if (!st.cfg) return;
    const config::Config settings = *st.cfg;
    if (!player_requested && !settings.auto_translate_project_text) return;
    const std::string source_text = project_source_text(project, info);
    if (source_text.empty()) return;

    st.project_translation_request.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(st.project_translation_mu);
        st.project_translation_text.clear();
        st.project_translation_error.clear();
        st.project_show_original_text = false;
    }
    // Translation is intentionally disabled for the local-only AI product.
    // Provider descriptions remain available in their original language; no
    // project text is sent to an external model.
    {
        std::lock_guard<std::mutex> lock(st.project_translation_mu);
        st.project_translation_error = "Translation is unavailable in local-only AI mode. No project text was sent.";
    }
    push_notice(st, ui_model::NoticeLevel::Info, "Local-only AI mode",
                "Online translation is disabled. The original provider description remains available.");
    return;

}

void do_project_detail(UiState& st, const mods::SearchResult& project, uint64_t request_id) {
    st.project_loading = true;
    {
        std::lock_guard<std::mutex> lock(st.project_mu);
        st.project_error.clear();
    }
    mods::ApiCfg cfg = provider_config::make(*st.cfg);
    mods::ModInfo info;
    std::string error;
    bool ok = mods::project_files(cfg, project.slug, project.source, info, &error);
    {
        std::lock_guard<std::mutex> lock(st.project_mu);
        if (request_id != st.project_screen.request_id) {
            return;
        }
        if (ok) {
            st.project_info = info;
            st.project_screen.accept(request_id, info, !st.project_info.files.empty());
        } else {
            st.project_error = error;
            st.project_screen.fail(request_id, error.empty() ? "Project details unavailable" : error, true);
        }
    }
    st.project_loading = false;
    if (ok && st.cfg && st.cfg->auto_translate_project_text)
        request_project_translation(st, project, info, false);
    if (!ok)
        push_notice(st, ui_model::NoticeLevel::Error, "Project details unavailable",
                    error.empty() ? "Try again or choose another provider result." : error);
    log_line(st, ok ? L"[browse] project details loaded"
                    : L"[browse] project details failed: " + net::to_wide(error));
}

void open_project_detail(UiState& st, const mods::SearchResult& project) {
    const bool same_project = st.project_detail_open &&
        st.project_detail.source == project.source && st.project_detail.slug == project.slug;
    if (!st.project_detail_open) {
        st.project_return_tab = st.active_tab;
        st.project_return_sidebar_item = st.sidebar_item;
        st.project_return_provider_tab = st.provider_tab;
        st.project_return_browse_category = st.browse_category;
    }
    st.project_detail = project;
    st.project_detail_open = true;
    if (!same_project) {
        st.project_detail_tab = 0;
        st.project_file_selected = -1;
        st.project_translation_request.fetch_add(1);
        st.project_translation_working = false;
        std::lock_guard<std::mutex> lock(st.project_translation_mu);
        st.project_translation_text.clear();
        st.project_translation_error.clear();
        st.project_show_original_text = false;
    }
    st.project_loading = true;
    st.selected_source = project.source;
    // The project page is rendered by the catalog route. Previously Home and
    // curated Discover cards only populated the state, leaving the UI on the
    // old route so a perfectly valid click appeared to do nothing.
    st.sidebar_item = 2;
    st.active_tab = 1;
    st.provider_tab = 1;
    switch (project.type) {
        case mods::ProjectType::Modpack: st.browse_category = 1; break;
        case mods::ProjectType::Shader: st.browse_category = 2; break;
        case mods::ProjectType::ResourcePack: st.browse_category = 3; break;
        default: st.browse_category = 0; break;
    }
    const uint64_t request_id = st.next_request_id.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(st.project_mu);
        st.project_screen.begin(request_id);
    }
    spawn_worker(st, std::thread(do_project_detail, std::ref(st), project, request_id));
}

void close_project_detail(UiState& st) {
    st.project_detail_open = false;
    st.project_loading = false;
    st.project_file_selected = -1;
    st.active_tab = st.project_return_tab;
    st.sidebar_item = st.project_return_sidebar_item;
    st.provider_tab = st.project_return_provider_tab;
    st.browse_category = st.project_return_browse_category;
}

// A fixed-height, image-aware row used by the catalog surfaces. Drawing text
// at explicit positions means image rendering never steals layout space from
// metadata, which keeps every row compact at any DPI scale.
bool draw_catalog_compact_row(UiState& st, const mods::SearchResult& project,
                              const char* id, const ImVec2& size,
                              bool show_description) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui_px(10.0f), ui_px(7.0f)));
    card_begin(id, size);
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float inner_width = ImGui::GetContentRegionAvail().x;
    const float inner_height = ImGui::GetContentRegionAvail().y;
    const float icon_size = show_description ? ui_px(44.0f) : ui_px(36.0f);
    const float icon_y = start.y + std::max(0.0f, (inner_height - icon_size) * 0.5f);
    draw_catalog_project_art(st, project, ImVec2(start.x, icon_y),
                             ImVec2(icon_size, icon_size), c32(k.surface2));

    const float text_x = start.x + icon_size + ui_px(10.0f);
    const float text_width = std::max(0.0f, inner_width - icon_size - ui_px(10.0f));
    const std::string provider = project.source == "curseforge" ? "CurseForge" :
                                 project.source == "modrinth" ? "Modrinth" : "Catalog";
    const std::string meta = provider + " | " + format_download_count(project.downloads) + " downloads";

    ImGui::SetCursorScreenPos(ImVec2(text_x, start.y));
    ImGui::PushFont(f_bold);
    const std::string title = elide_to_width(project.title.empty() ? project.slug : project.title, text_width);
    ImGui::TextUnformatted(title.c_str());
    ImGui::PopFont();

    if (show_description) {
        ImGui::SetCursorScreenPos(ImVec2(text_x, start.y + ui_px(22.0f)));
        ImGui::PushFont(f_small);
        const std::string description = elide_to_width(
            project.description.empty() ? "No project description available." : project.description, text_width);
        ImGui::TextColored(k.muted, "%s", description.c_str());
        ImGui::PopFont();
    }

    ImGui::SetCursorScreenPos(ImVec2(text_x, start.y +
        std::max(0.0f, inner_height - ImGui::GetTextLineHeight())));
    ImGui::PushFont(f_small);
    const std::string compact_meta = elide_to_width(meta, text_width);
    ImGui::TextColored(k.muted, "%s", compact_meta.c_str());
    ImGui::PopFont();
    card_end();
    ImGui::PopStyleVar();
    return ImGui::IsItemClicked(ImGuiMouseButton_Left);
}

void request_project_install(UiState& st, const mods::SearchResult& project,
                             const instances::Instance& target, bool modpack) {
    st.install_confirm_project = project;
    instances::Instance resolved_target = target;
    st.install_confirm_modpack = modpack;
    st.install_confirm_dependencies.clear();
    st.install_confirm_conflict_files.clear();
    st.install_confirm_conflicts = 0;

    mods::ModInfo info;
    {
        std::lock_guard<std::mutex> lock(st.project_mu);
        info = st.project_info;
    }
    std::string preferred_loader = target.loader;
    if (preferred_loader.empty() || preferred_loader == "auto")
        preferred_loader = st.mod_loader.empty() ? st.cfg->loader : st.mod_loader;
    std::string preferred_version = target.minecraft_version;
    if (preferred_version.empty())
        preferred_version = st.mod_version.empty() ? st.selected : st.mod_version;

    // A new modpack is not installed into the currently selected profile.
    // Resolve its own published release first so an existing vanilla filter
    // cannot incorrectly reject a Forge/Fabric pack.
    if (modpack) {
        mods::CompatibleRelease release;
        if (mods::select_compatible_release(info, preferred_loader, preferred_version,
                                            release, nullptr)) {
            resolved_target.minecraft_version = release.game_version;
            resolved_target.loader = release.loader;
        }
    }
    st.install_confirm_target = resolved_target;

    std::string loader = resolved_target.loader == "auto" ? st.cfg->loader : resolved_target.loader;
    if (loader == "auto") loader.clear();
    const std::string version = resolved_target.minecraft_version;
    const mods::FileInfo* chosen = nullptr;
    if (modpack) {
        mods::CompatibleRelease release;
        if (mods::select_compatible_release(info, resolved_target.loader, version, release, nullptr)) {
            for (const auto& file : info.files) {
                if (file.id == release.file_id) {
                    chosen = &file;
                    break;
                }
            }
        }
    } else {
        for (const auto& file : info.files) {
            const bool loader_ok = loader.empty() || file.loaders.empty() ||
                                   std::find(file.loaders.begin(), file.loaders.end(), loader) !=
                                       file.loaders.end();
            const bool version_ok = version.empty() || file.game_versions.empty() ||
                                    std::find(file.game_versions.begin(), file.game_versions.end(), version) !=
                                        file.game_versions.end();
            if (loader_ok && version_ok) {
                chosen = &file;
                if (file.primary) break;
            }
        }
    }
    if (!chosen && !info.files.empty()) chosen = &info.files.front();
    if (chosen) {
        for (const auto& dependency : chosen->dependencies) {
            const bool duplicate = std::any_of(st.install_confirm_dependencies.begin(),
                st.install_confirm_dependencies.end(), [&](const mods::DepInfo& existing) {
                    return existing.project_id == dependency.project_id;
                });
            if (!duplicate) st.install_confirm_dependencies.push_back(dependency);
        }
        if (!resolved_target.directory.empty()) {
            std::string list_error;
            for (const auto& entry : instances::list_content(resolved_target, &list_error)) {
                if (entry.filename == chosen->filename) {
                    ++st.install_confirm_conflicts;
                    std::string description = entry.filename;
                    if (entry.managed && !entry.owner_project.empty())
                        description += " (managed by " + entry.owner_project + ")";
                    else
                        description += " (local/unmanaged)";
                    st.install_confirm_conflict_files.push_back(std::move(description));
                }
            }
        }
    }
    st.install_confirm_open = true;
    ImGui::OpenPopup("Confirm content install");
}

void do_mod_install_target(UiState& st, const std::string& slug, const std::string& source,
                           instances::Instance target) {
    auto& loading = aml::ui::LoadingScreen::instance();
    loading.show_fullscreen("Installing Mod", "Adding " + slug + "...");
    loading.begin_steps({"Check Compatibility", "Download Files", "Install to Profile", "Done"});

    st.mod_installing = true;
    set_mod_status(st, "Installing " + slug + "...");

    loading.advance_step("Checking compatibility...");
    int job_id = start_job(st, "Install project", slug, "install_project",
                           retry_payload(slug, source, target), true);
    if (!wait_for_exclusive_job(st, job_id)) {
        st.mod_installing = false;
        loading.hide();
        set_mod_status(st, "Install cancelled before it began");
        finish_job(st, job_id, false, "Cancelled while waiting for protected profile access");
        return;
    }
    update_job(st, job_id, 0.08f, "Resolving compatible files...");
    set_mod_install_log(st, {});
    mods::ApiCfg cfg = provider_config::make(*st.cfg);
    std::vector<std::string> install_log;
    std::string loader = target.loader == "auto" ? st.cfg->loader : target.loader;
    if (loader == "auto" || loader.empty()) loader.clear();
    const std::string gv = target.minecraft_version;
    std::wstring mods_dir = target.directory + L"\\mods";
    std::string err;
    mods::ModInfo compatibility;
    if (!mods::project_files(cfg, slug, source, compatibility, &err) ||
        mods::pick_file(compatibility, loader, gv).empty()) {
        if (err.empty()) err = "no compatible release for the selected profile";
        st.mod_installing = false;
        loading.hide();
        set_mod_status(st, "Install blocked: " + err);
        finish_job(st, job_id, false, err);
        return;
    }

    loading.advance_step("Downloading files...");
    const auto progress = [&](uint64_t done, uint64_t total) {
        update_job_transfer(st, job_id, done, total);
        return job_transfer_allowed(st, job_id);
    };
    loading.advance_step("Installing to profile...");
    bool ok = mods::install_mod(cfg, slug, source, loader, gv, mods_dir, install_log,
                                &err, progress);
    set_mod_install_log(st, std::move(install_log));
    st.mod_installing = false;

    if (ok) {
        loading.complete_all_steps();
        loading.set_status("Install complete!");
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        loading.hide();
        set_mod_status(st, "Install complete");
        mark_profile_modified(st, target);
        st.instances_loaded = false;
    } else {
        loading.fail_step(err);
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        loading.hide();
        set_mod_status(st, "Install failed: " + err);
    }
    finish_job(st, job_id, ok, ok ? "Installed" : err);
    log_line(st, ok ? L"[mods] install complete" : L"[mods] install failed: " + net::to_wide(err));
}

void do_mod_install(UiState& st, const std::string& slug) {
    instances::Instance target;
    if (!st.active_instance_dir.empty())
        instances::load(st.active_instance_dir, target, nullptr);
    if (target.directory.empty()) target.directory = st.active_instance_dir;
    do_mod_install_target(st, slug, st.selected_source, target);
}

void do_modpack_install(UiState& st, mods::SearchResult project,
                        instances::Instance target = {}) {
    st.mod_installing = true;
    set_mod_status(st, "Downloading the creator's modpack profile...");
    int job_id = start_job(st, "Install modpack", project.title, "install_modpack",
                           retry_payload(project), true);
    if (!wait_for_exclusive_job(st, job_id)) {
        st.mod_installing = false;
        set_mod_status(st, "Modpack install cancelled before it began");
        finish_job(st, job_id, false, "Cancelled while waiting for protected profile access");
        return;
    }
    std::string error;
    mods::ApiCfg cfg = provider_config::make(*st.cfg);
    mods::ModInfo info;
    std::string preferred_loader = target.loader;
    if (preferred_loader.empty() || preferred_loader == "auto")
        preferred_loader = st.mod_loader.empty() ? st.cfg->loader : st.mod_loader;
    const std::string preferred_version = target.minecraft_version.empty()
        ? (st.mod_version.empty() ? st.selected : st.mod_version)
        : target.minecraft_version;
    set_mod_status(st, "Reading pack metadata...");
    if (!mods::project_files(cfg, project.slug, project.source, info, &error)) {
        set_mod_status(st, "Could not read pack info: " + error);
        finish_job(st, job_id, false, error);
        st.mod_installing = false;
        return;
    }
    set_mod_status(st, "Finding compatible version...");
    mods::CompatibleRelease release;
    if (!mods::select_compatible_release(info, preferred_loader, preferred_version,
                                         release, &error)) {
        set_mod_status(st, "No compatible version found: " + error);
        finish_job(st, job_id, false, error);
        st.mod_installing = false;
        return;
    }
    const std::string file_id = release.file_id;
    const mods::FileInfo* selected = nullptr;
    for (const auto& file : info.files) if (file.id == file_id) { selected = &file; break; }
    mods::FileInfo selected_file;
    if (selected) selected_file = *selected;
    set_mod_status(st, "Resolving download...");
    if (!selected || !mods::resolve_download_url(cfg, project.slug, project.source,
                                                 selected_file, &error)) {
        if (error.empty()) error = "Provider returned no downloadable file for this pack version.";
        set_mod_status(st, "Download unavailable: " + error);
        finish_job(st, job_id, false, error);
        st.mod_installing = false;
        return;
    }
    const std::wstring base = st.cfg->base_dir.empty() ? st.exe_dir : st.cfg->base_dir;
    const std::wstring instances_dir = base + L"\\instances";
    const std::wstring downloads_dir = base + L"\\downloads";
    net::mkdirs(downloads_dir);
    std::wstring archive_path = downloads_dir + L"\\.amalgam-modpack-" +
                                std::to_wstring(GetCurrentProcessId()) + L".archive";
    set_mod_status(st, "Downloading pack...");
    if (!net::download(net::to_wide(selected_file.url), archive_path,
                       [&](uint64_t done, uint64_t total) {
                           update_job_transfer(st, job_id, done, total);
                           return job_transfer_allowed(st, job_id);
                       }, &error,
                       selected_file.sha1, selected_file.size > 0 ? selected_file.size : -1)) {
        set_mod_status(st, "Download failed: " + error);
        finish_job(st, job_id, false, error);
        st.mod_installing = false;
        return;
    }
    set_mod_status(st, "Extracting pack...");
    instances::Instance imported;
    bool ok = import_pack::archive(archive_path, instances_dir, cfg, imported,
                                        [&](float progress, const std::string& label) {
                                            update_job(st, job_id, progress, label);
                                            return job_transfer_allowed(st, job_id);
                                        }, &error);
    DeleteFileW(archive_path.c_str());
    if (ok) {
        // The provider result is the authoritative project/version identity.
        // Archive manifests are not required to carry the project slug, so
        // persist it here to make future published-pack update checks reliable.
        imported.pack_source = project.source;
        imported.pack_project = project.slug;
        imported.pack_version = selected_file.id;
        imported.pack_modified = false;
        if (imported.minecraft_version.empty()) imported.minecraft_version = release.game_version;
        if (imported.loader.empty() || imported.loader == "auto") imported.loader = release.loader;
        std::string metadata_error;
        if (!instances::save(imported, &metadata_error)) {
            set_mod_status(st, "Modpack created, but update metadata could not be saved: " + metadata_error);
            finish_job(st, job_id, false, metadata_error);
            st.mod_installing = false;
            return;
        }
        st.selected_instance = imported;
        st.active_instance_dir = imported.directory;
        st.instances_loaded = false;
        set_mod_status(st, "Profile ready: " + imported.name);
        finish_job(st, job_id, true, imported.name);
    } else {
        set_mod_status(st, "Install failed: " + error);
        finish_job(st, job_id, false, error);
    }
    st.mod_installing = false;
}

bool existing_profile_target(UiState& st, instances::Instance& target) {
    if (st.active_instance_dir.empty()) {
        set_mod_status(st, "Select an existing profile before installing content");
        st.sidebar_item = 0;
        st.active_tab = 0;
        return false;
    }
    if (!instances::load(st.active_instance_dir, target, nullptr)) {
        if (st.selected_instance.directory == st.active_instance_dir) target = st.selected_instance;
        else {
            set_mod_status(st, "Selected profile metadata could not be loaded");
            return false;
        }
    }
    target.directory = st.active_instance_dir;
    return true;
}

void do_optimize_profile(UiState& st, instances::Instance target) {
    st.mod_installing = true;
    set_mod_status(st, "Installing recommended performance mods...");
    set_mod_install_log(st, {});
    int job_id = start_job(st, "Optimize profile", target.name, {}, {}, true);
    if (!wait_for_exclusive_job(st, job_id)) {
        st.mod_installing = false;
        set_mod_status(st, "Profile optimization cancelled before it began");
        finish_job(st, job_id, false, "Cancelled while waiting for protected profile access");
        return;
    }
    mods::ApiCfg cfg = provider_config::make(*st.cfg);
    const std::string loader = target.loader == "auto" || target.loader == "quilt"
                                   ? "fabric" : target.loader;
    const bool shaders = performance::normalize_profile(target.performance_profile) == "shaders";
    const std::vector<std::string> recommendations = performance::recommended_mods(
        loader, target.minecraft_version, shaders);
    std::vector<std::string> install_log;
    const std::wstring mods_dir = target.directory + L"\\mods";
    size_t completed = 0;
    bool ok = true;
    std::string last_error;
    for (const std::string& slug : recommendations) {
        std::string error;
        if (!mods::install_mod(cfg, slug, "modrinth", loader, target.minecraft_version,
                               mods_dir, install_log, &error,
                               [&](uint64_t done, uint64_t total) {
                                   update_job_transfer(st, job_id, done, total);
                                   return job_transfer_allowed(st, job_id);
                               })) {
            install_log.push_back("FAILED " + slug + ": " + error);
            last_error = error;
            ok = false;
        }
        update_job(st, job_id, static_cast<float>(++completed) /
                              static_cast<float>(std::max<size_t>(1, recommendations.size())), slug);
    }
    set_mod_install_log(st, std::move(install_log));
    st.mod_installing = false;
    set_mod_status(st, ok ? "Performance mods installed" : "Some performance mods failed: " + last_error);
    finish_job(st, job_id, ok, ok ? "Performance mods installed" : last_error);
    log_line(st, ok ? L"[performance] optimization complete"
                    : L"[performance] optimization completed with failures");
}

void do_profile_update(UiState& st, const config::Config& snapshot, instances::Instance target) {
    st.mod_installing = true;
    int job_id = start_job(st, "Update profile", target.name, "update_profile",
                           retry_payload(target), true);
    if (!wait_for_exclusive_job(st, job_id)) {
        st.mod_installing = false;
        set_mod_status(st, "Profile update cancelled before it began");
        finish_job(st, job_id, false, "Cancelled while waiting for protected profile access");
        return;
    }
    std::wstring restore;
    std::string error;
    if (!instances::create_restore_point(target, &restore, &error)) {
        set_mod_status(st, "Update cancelled: backup failed: " + error);
        finish_job(st, job_id, false, error);
        st.mod_installing = false;
        return;
    }
    log_line(st, L"[profile] update restore point: " + restore);
    mods::ApiCfg cfg = provider_config::make(snapshot);
    std::set<std::string> selected_files;
    {
        std::lock_guard<std::mutex> lock(st.owned_update_mu);
        selected_files = st.owned_update_selected;
    }
    set_mod_install_log(st, {});
    std::vector<std::string> install_log;
    bool ok = mods::update_owned(cfg, target.directory, target.loader, target.minecraft_version,
                                 install_log, &error, selected_files,
                                 [&](uint64_t done, uint64_t total) {
                                     update_job_transfer(st, job_id, done, total);
                                      return job_transfer_allowed(st, job_id);
                                  });
    if (!ok) {
        std::string restore_error;
        if (!instances::restore_latest(target, &restore_error)) {
            if (!error.empty()) error += "; ";
            error += "automatic rollback failed: " + restore_error;
        } else {
            log_line(st, L"[profile] failed update rolled back to its restore point");
        }
    }
    set_mod_install_log(st, std::move(install_log));
    st.mod_installing = false;
    set_mod_status(st, ok ? "Profile update complete" : "Profile update failed: " + error);
    finish_job(st, job_id, ok, ok ? "Updated" : error);
    log_line(st, ok ? L"[profile] update complete" : L"[profile] update failed: " + net::to_wide(error));
}

void do_owned_update_check(UiState& st, const config::Config& snapshot, instances::Instance target) {
    st.owned_update_checking = true;
    std::vector<mods::UpdateEntry> updates;
    std::string error;
    mods::ApiCfg cfg = provider_config::make(snapshot);
    const bool ok = mods::preview_owned(cfg, target.directory, target.loader,
                                        target.minecraft_version, updates, &error);
    {
        std::lock_guard<std::mutex> lock(st.owned_update_mu);
        st.owned_updates = std::move(updates);
        st.owned_update_selected.clear();
        for (const auto& update : st.owned_updates) st.owned_update_selected.insert(update.file);
        st.owned_update_error = ok ? std::string() : error;
        st.owned_update_directory = target.directory;
    }
    st.owned_update_checking = false;
}

void do_published_pack_update(UiState& st, const config::Config& snapshot,
                              instances::Instance source_target, bool create_copy) {
    st.mod_installing = true;
    const std::string action = create_copy ? "Create updated modpack copy" : "Update published modpack";
    const int job_id = start_job(st, action, source_target.name, "update_published_pack",
                                 retry_payload_published_pack(source_target, create_copy), true);
    if (!wait_for_exclusive_job(st, job_id)) {
        st.mod_installing = false;
        set_mod_status(st, action + " cancelled before it began");
        finish_job(st, job_id, false, "Cancelled while waiting for protected profile access");
        return;
    }
    update_job(st, job_id, 0.04f, "Checking the creator's latest release...");
    set_mod_status(st, "Checking the creator's latest modpack release...");

    auto fail = [&](const std::string& detail) {
        st.mod_installing = false;
        set_mod_status(st, action + " failed: " + detail);
        finish_job(st, job_id, false, detail);
        log_line(st, L"[profile] published pack update failed: " + net::to_wide(detail));
    };
    if (source_target.pack_source.empty() || source_target.pack_project.empty()) {
        fail("This profile has no published-pack source metadata.");
        return;
    }

    mods::ApiCfg cfg = provider_config::make(snapshot);
    mods::UpdateInfo update;
    std::string error;
    if (!mods::check_project_update(cfg, source_target.pack_project, source_target.pack_source,
                                    source_target.loader, source_target.minecraft_version,
                                    source_target.pack_version, update, &error)) {
        fail(error.empty() ? "The creator's latest release could not be checked." : error);
        return;
    }
    if (!update.available) {
        st.mod_installing = false;
        set_mod_status(st, "This published modpack is already up to date.");
        finish_job(st, job_id, true, "Already up to date");
        return;
    }

    mods::FileInfo archive;
    archive.id = update.latest_file_id;
    archive.filename = update.latest_filename;
    archive.url = update.latest_url;
    archive.sha1 = update.latest_sha1;
    archive.size = update.latest_size;
    if (archive.id.empty() ||
        !mods::resolve_download_url(cfg, source_target.pack_project, source_target.pack_source,
                                    archive, &error)) {
        fail(error.empty() ? "The creator's update archive has no downloadable file." : error);
        return;
    }

    const std::wstring base = snapshot.base_dir.empty() ? st.exe_dir : snapshot.base_dir;
    const std::wstring downloads_dir = base + L"\\downloads";
    if (!net::mkdirs(downloads_dir)) {
        fail("Could not create the launcher download folder.");
        return;
    }
    const std::wstring archive_path = downloads_dir + L"\\.amalgam-pack-update-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L".archive";
    update_job(st, job_id, 0.10f, "Downloading the creator's update archive...");
    if (!net::download(net::to_wide(archive.url), archive_path,
                       [&](uint64_t done, uint64_t total) {
                           update_job_transfer(st, job_id, done, total);
                           return job_transfer_allowed(st, job_id);
                       }, &error, archive.sha1, archive.size > 0 ? archive.size : -1)) {
        DeleteFileW(archive_path.c_str());
        fail(error.empty() ? "The update archive download did not complete." : error);
        return;
    }
    if (job_cancelled(st, job_id)) {
        DeleteFileW(archive_path.c_str());
        st.mod_installing = false;
        set_mod_status(st, "Published modpack update cancelled before profile changes.");
        finish_job(st, job_id, false, "Cancelled before profile changes");
        return;
    }

    import_pack::ArchivePreview archive_preview;
    std::string preview_error;
    if (import_pack::preview_archive(archive_path, source_target, archive_preview, &preview_error)) {
        const std::string summary = "Archive review: " + std::to_string(archive_preview.added) +
            " added, " + std::to_string(archive_preview.replaced) + " replaced, " +
            std::to_string(archive_preview.removed) + " removed" +
            (archive_preview.provider_file_names_resolved ? "" :
                " (provider file names resolve during staging)");
        update_job(st, job_id, 0.68f, summary);
        log_line(st, L"[profile] " + net::to_wide(summary));
    } else {
        // Preview is an additional safety explanation. The authoritative
        // archive import still validates and stages every file before commit.
        log_line(st, L"[profile] archive preview unavailable: " + net::to_wide(preview_error));
    }

    instances::Instance target = source_target;
    if (create_copy) {
        const std::wstring instances_dir = base + L"\\instances";
        update_job(st, job_id, 0.70f, "Creating a safe copy of your current profile...");
        const std::string copy_name = source_target.name + " - " + update.latest_version;
        if (!instances::duplicate(source_target, instances_dir, copy_name, target, &error)) {
            DeleteFileW(archive_path.c_str());
            fail(error.empty() ? "Could not create a safe updated copy." : error);
            return;
        }
    }

    update_job(st, job_id, 0.78f, "Creating a restore point before applying the creator update...");
    std::wstring restore;
    if (!instances::create_restore_point(target, &restore, &error)) {
        DeleteFileW(archive_path.c_str());
        fail(error.empty() ? "Could not create a restore point." : error);
        return;
    }
    log_line(st, L"[profile] published pack update restore point: " + restore);

    update_job(st, job_id, 0.82f, "Staging the creator update before it replaces profile content...");
    instances::Instance updated;
    const bool ok = import_pack::archive_into(archive_path, target, cfg, updated,
        [&](float progress, const std::string& detail) {
            update_job(st, job_id, 0.82f + std::clamp(progress, 0.0f, 1.0f) * 0.17f, detail);
            return job_transfer_allowed(st, job_id);
        }, &error);
    DeleteFileW(archive_path.c_str());
    if (!ok) {
        fail(error.empty() ? "The creator update could not be staged safely." : error);
        return;
    }

    // Keep the provider's stable project/file identity even when an archive
    // omits it from its internal manifest. That makes the next update check
    // deterministic for both Modrinth and CurseForge packs.
    updated.pack_source = source_target.pack_source;
    updated.pack_project = source_target.pack_project;
    updated.pack_version = update.latest_file_id;
    updated.pack_modified = false;
    if (!instances::save(updated, &error)) {
        fail("The creator update was applied, but its update metadata could not be saved: " + error);
        return;
    }

    st.selected_instance = updated;
    st.active_instance_dir = updated.directory;
    st.instances_loaded = false;
    st.mod_installing = false;
    set_mod_status(st, create_copy ? "Updated copy created from the creator's latest release." :
                                    "Published modpack update complete.");
    finish_job(st, job_id, true, create_copy ? "Updated copy ready" : "Creator update applied");
    push_notice(st, ui_model::NoticeLevel::Success,
                create_copy ? "Updated modpack copy created" : "Published modpack updated",
                "A restore point was created before creator-managed content changed.",
                "Open Library", "library");
    log_line(st, L"[profile] published pack update complete: " + updated.directory);
}

void retry_job(UiState& st, const UiState::DownloadJob& job) {
    if (job.retry_action.empty() || job.retry_payload.empty() || job.active) return;
    std::string parse_error;
    Json payload = Json::parse(job.retry_payload, &parse_error);
    if (!parse_error.empty()) {
        set_mod_status(st, "Retry unavailable: invalid job metadata");
        return;
    }
    if (job.retry_action == "install_project") {
        instances::Instance target;
        target.id = payload.get("id").as_str();
        target.name = payload.get("name").as_str();
        target.minecraft_version = payload.get("version").as_str();
        target.loader = payload.get("loader").as_str("auto");
        target.directory = net::to_wide(payload.get("directory").as_str());
        const std::string slug = payload.get("slug").as_str();
        const std::string source = payload.get("source").as_str("modrinth");
        if (slug.empty() || target.directory.empty()) {
            set_mod_status(st, "Retry unavailable: target profile metadata is missing");
            return;
        }
        spawn_worker(st, std::thread(do_mod_install_target, std::ref(st), slug, source, target));
    } else if (job.retry_action == "install_modpack") {
        mods::SearchResult project;
        project.slug = payload.get("slug").as_str();
        project.title = payload.get("title").as_str(project.slug);
        project.source = payload.get("source").as_str("modrinth");
        project.type = mods::ProjectType::Modpack;
        if (project.slug.empty()) {
            set_mod_status(st, "Retry unavailable: project metadata is missing");
            return;
        }
        spawn_worker(st, std::thread(do_modpack_install, std::ref(st), project, instances::Instance{}));
    } else if (job.retry_action == "update_profile") {
        instances::Instance target;
        target.id = payload.get("id").as_str();
        target.name = payload.get("name").as_str();
        target.minecraft_version = payload.get("version").as_str();
        target.loader = payload.get("loader").as_str("auto");
        target.directory = net::to_wide(payload.get("directory").as_str());
        if (target.directory.empty()) {
            set_mod_status(st, "Retry unavailable: profile directory is missing");
            return;
        }
        config::Config snapshot = *st.cfg;
        spawn_worker(st, std::thread([&st, snapshot, target]() {
            do_profile_update(st, snapshot, target);
        }));
    } else if (job.retry_action == "update_published_pack") {
        instances::Instance target;
        const std::wstring directory = net::to_wide(payload.get("directory").as_str());
        if (!directory.empty()) instances::load(directory, target, nullptr);
        if (target.directory.empty()) {
            target.id = payload.get("id").as_str();
            target.name = payload.get("name").as_str();
            target.minecraft_version = payload.get("version").as_str();
            target.loader = payload.get("loader").as_str("auto");
            target.directory = directory;
            target.pack_source = payload.get("source").as_str();
            target.pack_project = payload.get("project").as_str();
        }
        if (target.directory.empty() || target.pack_source.empty() || target.pack_project.empty()) {
            set_mod_status(st, "Retry unavailable: published-pack metadata is missing");
            return;
        }
        const bool create_copy = payload.get("copy").as_bool(false);
        const config::Config snapshot = *st.cfg;
        spawn_worker(st, std::thread([&st, snapshot, target, create_copy]() {
            do_published_pack_update(st, snapshot, target, create_copy);
        }));
    }
}

void remove_download_job(UiState& st, int job_id) {
    bool removed = false;
    {
        std::lock_guard<std::mutex> lock(st.jobs_mu);
        const auto before = st.jobs.size();
        st.jobs.erase(std::remove_if(st.jobs.begin(), st.jobs.end(),
            [job_id](const UiState::DownloadJob& job) {
                return job.id == job_id && !job.active;
            }), st.jobs.end());
        removed = st.jobs.size() != before;
    }
    if (removed) {
        if (st.expanded_operation_id == job_id) st.expanded_operation_id = 0;
        persist_jobs(st);
        push_notice(st, ui_model::NoticeLevel::Info, "Download removed",
                    "The failed operation was removed from download history.");
    }
}

void do_pack_update_check(UiState& st, const config::Config& snapshot, instances::Instance target) {
    st.pack_update_checking = true;
    mods::UpdateInfo update;
    std::string update_error;
    if (target.pack_source.empty() || target.pack_project.empty()) {
        update_error = "This is a custom profile without upstream project metadata";
        {
            std::lock_guard<std::mutex> lock(st.pack_update_mu);
            st.pack_update = std::move(update);
            st.pack_update_error = std::move(update_error);
            st.pack_update_directory = target.directory;
        }
        st.pack_update_checking = false;
        return;
    }
    mods::ApiCfg cfg = provider_config::make(snapshot);
    std::string error;
    if (!mods::check_project_update(cfg, target.pack_project, target.pack_source, target.loader,
                                    target.minecraft_version, target.pack_version,
                                    update, &error))
        update_error = error.empty() ? "The profile update check failed." : error;
    {
        std::lock_guard<std::mutex> lock(st.pack_update_mu);
        st.pack_update = std::move(update);
        st.pack_update_error = std::move(update_error);
        st.pack_update_directory = target.directory;
    }
    st.pack_update_checking = false;
}

void do_pack_build(UiState& st) {
    st.pack_building = true;
    const int job_id = start_job(st, "AI modpack plan", "Preparing a compatible content plan");
    update_job(st, job_id, 0.08f, "Contacting configured AI provider...");
    set_pack_summary(st, {});
    std::vector<mods::Match> plan;
    std::string err;
    const bool wizard_ai = st.wizard_source == 3 && !st.wizard_prompt.empty();
    std::string prompt = wizard_ai ? st.wizard_prompt : st.pack_prompt;
    std::string gv = wizard_ai ? st.wizard_version : st.selected;
    std::string loader = wizard_ai ? st.wizard_loader : st.mod_loader;

    ai::ChatRequest req;
    std::string system =
        "You are a Minecraft modpack curator. Given a user request, a target game version "
        "(<" + gv + ">), pick a curated set of well-known, compatible mods. Reply with ONLY a "
        "JSON array of objects, one per mod, with fields \"slug\" (Modrinth project slug or "
        "human name if unsure), \"source\" (\"modrinth\" by default; use \"curseforge\" only "
        "with a numeric CurseForge project id), \"loader\" (" + loader + "), and one-line \"why\". "
        "Prefer 8-15 mods. Do not include markdown.";
    req.messages.push_back({"system", system});
    req.messages.push_back({"user", prompt});
    std::string reply;
    if (!ai::chat(req, reply, &err)) {
        set_pack_summary(st, "AI failed: " + err);
        st.pack_building = false;
        finish_job(st, job_id, false, err.empty() ? "AI provider failed" : err);
        return;
    }
    set_pack_summary(st, reply);
    Json j = Json::parse(reply, &err);
    if (!err.empty() || !j.is(Json::Type::Arr)) {
        set_pack_summary(st, reply + "\n(parse: " + err + ")");
        st.pack_building = false;
        finish_job(st, job_id, false, "AI response was not a valid modpack plan");
        return;
    }
    std::vector<mods::Match> out;
    for (const auto& item : j.items()) {
        mods::Match m;
        m.slug = item.get("slug").as_str();
        m.source = mods::canonical_source(item.get("source").as_str("modrinth"));
        if (m.source.empty()) m.source = "modrinth";
        m.loader = item.get("loader").as_str(loader);
        if (m.loader.empty()) m.loader = loader;
        if (m.slug.empty()) continue;
        m.title = m.slug;
        out.push_back(std::move(m));
    }
    {
        std::lock_guard<std::mutex> lock(st.pack_mu);
        st.pack_plan = std::move(out);
    }
    st.pack_building = false;
    update_job(st, job_id, 1.0f, "Plan ready for review");
    finish_job(st, job_id, true, std::to_string(st.pack_plan.size()) + " projects suggested");
    log_line(st, L"[pack] AI returned " +
                     std::to_wstring(st.pack_plan.size()) + L" mods");
}

// ---------------------------------------------------------------------------
// Sidebar navigation
// ---------------------------------------------------------------------------
enum class NavIcon {
    Home,
    Discover,
    Packs,
    Downloads,
    Servers,
    Screenshots,
    Bell,
    Java,
    Bedrock,
    Settings,
    Logs,
    Backups,
    Config,
    Friends,
};

struct NavItem {
    NavIcon icon;
    const char* label;
    int tab;
    int id;
};

const NavItem kNav[] = {
    {NavIcon::Home, "Home", 0, 0},
    {NavIcon::Discover, "Discover", 16, 2},
    {NavIcon::Packs, "Library", 6, 3},
    {NavIcon::Downloads, "Downloads", 17, 4},
    {NavIcon::Friends, "Essentials", 23, 23},
    {NavIcon::Servers, "Servers", 8, 8},
    {NavIcon::Settings, "Settings", 4, 12},
};

const NavItem kPlayNav[] = {
    {NavIcon::Java, "Java Edition",
     aml::ui_model::java_edition_route().active_tab,
     aml::ui_model::java_edition_route().sidebar_item},
    {NavIcon::Bedrock, "Bedrock Edition", 3, 17},
};

// Sliding selection indicator: the highlight bar eases from its previous
// position to the newly selected nav item over ~160ms instead of snapping.
static AnimFloat g_nav_indicator_y;
static float g_nav_indicator_height = 0.0f;
static bool g_nav_indicator_initialized = false;

void nav_indicator_reset() {
    g_nav_indicator_initialized = false;
}

void draw_nav_icon(ImDrawList* dl, NavIcon icon, const ImVec2& center, float scale, ImU32 color) {
    const float r = 8.0f * scale;
    const float line = std::max(1.5f, 1.8f * scale);
    switch (icon) {
        case NavIcon::Home:
            dl->AddTriangleFilled(ImVec2(center.x, center.y - r),
                                  ImVec2(center.x - r - 1.0f * scale, center.y),
                                  ImVec2(center.x + r + 1.0f * scale, center.y), color);
            dl->AddRectFilled(ImVec2(center.x - r + 1.0f * scale, center.y - 1.0f * scale),
                              ImVec2(center.x + r - 1.0f * scale, center.y + r), color);
            dl->AddRectFilled(ImVec2(center.x - 2.0f * scale, center.y + 2.0f * scale),
                              ImVec2(center.x + 2.0f * scale, center.y + r), c32(k.sidebar));
            break;
        case NavIcon::Discover:
            dl->AddCircle(ImVec2(center.x - 2.0f * scale, center.y - 2.0f * scale),
                          r - 2.0f * scale, color, 16, line);
            dl->AddLine(ImVec2(center.x + 4.0f * scale, center.y + 4.0f * scale),
                        ImVec2(center.x + r + 1.0f * scale, center.y + r + 1.0f * scale),
                        color, line);
            break;
        case NavIcon::Packs:
            dl->AddRect(ImVec2(center.x - r, center.y - r + 2.0f * scale),
                        ImVec2(center.x + r, center.y + 1.0f * scale), color, 2.0f * scale, 0, line);
            dl->AddRect(ImVec2(center.x - r, center.y + 1.0f * scale),
                        ImVec2(center.x + r, center.y + r), color, 2.0f * scale, 0, line);
            dl->AddLine(ImVec2(center.x - 2.0f * scale, center.y - r + 2.0f * scale),
                        ImVec2(center.x - 2.0f * scale, center.y + r), color, line);
            break;
        case NavIcon::Downloads:
            dl->AddLine(ImVec2(center.x, center.y - r), ImVec2(center.x, center.y + 4.0f * scale), color, line);
            dl->AddLine(ImVec2(center.x, center.y + 4.0f * scale),
                        ImVec2(center.x - 5.0f * scale, center.y - 1.0f * scale), color, line);
            dl->AddLine(ImVec2(center.x, center.y + 4.0f * scale),
                        ImVec2(center.x + 5.0f * scale, center.y - 1.0f * scale), color, line);
            dl->AddLine(ImVec2(center.x - r, center.y + r), ImVec2(center.x + r, center.y + r), color, line);
            break;
        case NavIcon::Servers:
            for (int i = -1; i <= 1; ++i) {
                const float y = center.y + static_cast<float>(i) * 6.0f * scale;
                dl->AddRectFilled(ImVec2(center.x - r, y - 2.0f * scale),
                                  ImVec2(center.x + r, y + 2.0f * scale), color, 2.0f * scale);
                dl->AddCircleFilled(ImVec2(center.x + 4.0f * scale, y), 1.0f * scale, c32(k.sidebar));
            }
            break;
        case NavIcon::Screenshots:
            dl->AddRect(ImVec2(center.x - r, center.y - r), ImVec2(center.x + r, center.y + r),
                        color, 2.0f * scale, 0, line);
            dl->AddCircleFilled(ImVec2(center.x - 3.0f * scale, center.y - 3.0f * scale),
                                2.0f * scale, color);
            dl->AddTriangleFilled(ImVec2(center.x - 7.0f * scale, center.y + 6.0f * scale),
                                  ImVec2(center.x - 1.0f * scale, center.y),
                                  ImVec2(center.x + 3.0f * scale, center.y + 6.0f * scale), color);
            break;
        case NavIcon::Bell:
            dl->AddCircle(ImVec2(center.x, center.y - 1.0f * scale), r - 2.0f * scale,
                          color, 16, line);
            dl->AddRectFilled(ImVec2(center.x - r + 2.0f * scale, center.y + 2.0f * scale),
                              ImVec2(center.x + r - 2.0f * scale, center.y + r - 1.0f * scale),
                              color, 2.0f * scale);
            dl->AddLine(ImVec2(center.x - r - 1.0f * scale, center.y + r - 1.0f * scale),
                        ImVec2(center.x + r + 1.0f * scale, center.y + r - 1.0f * scale),
                        color, line);
            dl->AddCircleFilled(ImVec2(center.x, center.y + r + 2.0f * scale),
                                1.5f * scale, color);
            break;
        case NavIcon::Java:
            dl->AddLine(ImVec2(center.x + 3.0f * scale, center.y - r),
                        ImVec2(center.x + 3.0f * scale, center.y + 4.0f * scale), color, line);
            dl->AddBezierCubic(ImVec2(center.x + 3.0f * scale, center.y + 4.0f * scale),
                               ImVec2(center.x + 3.0f * scale, center.y + r),
                               ImVec2(center.x - r, center.y + r),
                               ImVec2(center.x - r, center.y + 3.0f * scale), color, line);
            dl->AddLine(ImVec2(center.x - r, center.y + r), ImVec2(center.x + r, center.y + r), color, line);
            break;
        case NavIcon::Bedrock:
            dl->AddRect(ImVec2(center.x - r, center.y - r + 2.0f * scale),
                        ImVec2(center.x + r, center.y + r), color, 2.0f * scale, 0, line);
            dl->AddLine(ImVec2(center.x - r, center.y - r + 2.0f * scale),
                        ImVec2(center.x, center.y - 2.0f * scale), color, line);
            dl->AddLine(ImVec2(center.x, center.y - 2.0f * scale),
                        ImVec2(center.x + r, center.y - r + 2.0f * scale), color, line);
            dl->AddLine(ImVec2(center.x, center.y - 2.0f * scale),
                        ImVec2(center.x, center.y + r), color, line);
            break;
        case NavIcon::Settings:
            dl->AddCircle(center, r - 2.0f * scale, color, 16, line);
            dl->AddCircleFilled(center, 2.5f * scale, color);
            for (int i = 0; i < 8; ++i) {
                const float angle = static_cast<float>(i) * 3.14159265f / 4.0f;
                const ImVec2 a(center.x + std::cos(angle) * (r - 1.0f * scale),
                               center.y + std::sin(angle) * (r - 1.0f * scale));
                const ImVec2 b(center.x + std::cos(angle) * (r + 2.0f * scale),
                               center.y + std::sin(angle) * (r + 2.0f * scale));
                dl->AddLine(a, b, color, line);
            }
            break;
        case NavIcon::Logs:
            dl->AddRectFilled(ImVec2(center.x - r, center.y - r),
                             ImVec2(center.x - r + 4.0f * scale, center.y + r), color);
            dl->AddRectFilled(ImVec2(center.x - r + 5.0f * scale, center.y - r),
                             ImVec2(center.x - r + 9.0f * scale, center.y + r), color);
            dl->AddRectFilled(ImVec2(center.x - r + 10.0f * scale, center.y - r),
                             ImVec2(center.x - r + 14.0f * scale, center.y + r), color);
            break;
        case NavIcon::Backups:
            dl->AddRect(ImVec2(center.x - r, center.y - r),
                        ImVec2(center.x + r, center.y + r), color, 0, 0, line);
            dl->AddLine(ImVec2(center.x - r, center.y - r),
                        ImVec2(center.x + r, center.y + r), color, line);
            dl->AddLine(ImVec2(center.x - r, center.y + r),
                        ImVec2(center.x + r, center.y - r), color, line);
            break;
        case NavIcon::Config:
            dl->AddCircle(center, r, color, 16, line);
            dl->AddLine(ImVec2(center.x - r, center.y),
                        ImVec2(center.x + r, center.y), color, line);
            dl->AddLine(ImVec2(center.x, center.y - r),
                        ImVec2(center.x, center.y + r), color, line);
            break;
        case NavIcon::Friends:
            dl->AddCircleFilled(ImVec2(center.x - 3.0f * scale, center.y - 2.0f * scale),
                                3.5f * scale, color);
            dl->AddCircleFilled(ImVec2(center.x + 3.0f * scale, center.y - 2.0f * scale),
                                3.5f * scale, color);
            dl->AddCircle(ImVec2(center.x - 3.0f * scale, center.y + 4.0f * scale),
                          5.0f * scale, color, 12, line);
            dl->AddCircle(ImVec2(center.x + 3.0f * scale, center.y + 4.0f * scale),
                          5.0f * scale, color, 12, line);
            break;
    }
}

void draw_home_stat_icon(ImDrawList* dl, int index, const ImVec2& center, float scale, ImU32 color) {
    const float r = 9.0f * scale;
    const float line = std::max(1.5f, 1.8f * scale);
    if (index == 0) {
        draw_nav_icon(dl, NavIcon::Packs, center, scale, color);
    } else if (index == 1) {
        draw_nav_icon(dl, NavIcon::Packs, center, scale, color);
        dl->AddCircleFilled(ImVec2(center.x + 5.0f * scale, center.y + 5.0f * scale),
                            2.5f * scale, c32(k.green));
    } else if (index == 2) {
        dl->AddCircle(center, r, color, 16, line);
        dl->AddLine(center, ImVec2(center.x, center.y - 5.0f * scale), color, line);
        dl->AddLine(center, ImVec2(center.x + 4.0f * scale, center.y + 3.0f * scale), color, line);
    } else if (index == 3) {
        draw_nav_icon(dl, NavIcon::Downloads, center, scale, color);
    } else {
        draw_nav_icon(dl, NavIcon::Servers, center, scale, color);
    }
}

void draw_favorite_star(ImDrawList* dl, const ImVec2& center, float outer_radius,
                        ImU32 color, bool filled) {
    ImVec2 points[10];
    const float inner_radius = outer_radius * 0.46f;
    for (int i = 0; i < 10; ++i) {
        const float angle = -1.57079633f + static_cast<float>(i) * 3.14159265f / 5.0f;
        const float radius = (i % 2 == 0) ? outer_radius : inner_radius;
        points[i] = ImVec2(center.x + std::cos(angle) * radius,
                           center.y + std::sin(angle) * radius);
    }
    if (filled)
        dl->AddConvexPolyFilled(points, 10, color);
    else
        dl->AddPolyline(points, 10, color, ImDrawFlags_Closed, std::max(1.5f, ui_px(1.5f)));
}

bool favorite_button(const char* id, bool favorite) {
    const ImVec2 size(ui_px(30.0f), ui_px(26.0f));
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, size);
    const bool hovered = ImGui::IsItemHovered();
    const bool focused = ImGui::IsItemFocused();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (hovered || focused)
        dl->AddRectFilled(pos, pos + size, c32(k.hover), ui_px(6.0f));
    draw_favorite_star(dl, pos + size * 0.5f, ui_px(8.0f),
                       c32(favorite ? k.brand_hov : k.muted), favorite);
    if (hovered) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(favorite ? "Remove from favorites" : "Add to favorites");
        ImGui::EndTooltip();
    }
    return ImGui::IsItemClicked();
}

bool nav_button(UiState& st, const NavItem& item) {
    const bool selected = st.sidebar_item == item.id;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    // Scale the rail rows to the available desktop height so the existing
    // navigation and account footer fit without forcing a sidebar scrollbar.
    const float h = std::clamp(ImGui::GetWindowHeight() / 24.0f,
                               ui_px(30.0f), ui_px(42.0f));
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImVec2 p1(p0.x + avail.x, p0.y + h);
    const std::string button_id = std::string("##nav_") + item.label;
    ImGui::InvisibleButton(button_id.c_str(), ImVec2(avail.x, h));
    const bool hovered = ImGui::IsItemHovered() && !st.running;
    const bool focused = ImGui::IsItemFocused();
    const bool clicked = !st.running && ImGui::IsItemClicked();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    if (selected) {
        // Record the target rect; the shared indicator (drawn by draw_sidebar)
        // animates a single highlight bar between items instead of snapping.
        g_nav_indicator_height = h;
        g_nav_indicator_y.target(p0.y);
        if (!g_nav_indicator_initialized) {
            g_nav_indicator_y.set(p0.y);
            g_nav_indicator_initialized = true;
        }
    } else if (hovered) {
        dl->AddRectFilled(p0, p1, c32(k.hover), 8.0f);
    } else if (focused) {
        // Visible keyboard focus ring.
        dl->AddRect(p0 + ImVec2(ui_px(2.0f), ui_px(2.0f)),
                    p1 - ImVec2(ui_px(2.0f), ui_px(2.0f)),
                    c32(k.brand_hov), ui_px(8.0f), 0, ui_px(1.5f));
    }
    const float icon_x = st.sidebar_compact ? p0.x + avail.x * 0.5f : p0.x + ui_px(25.0f);
    draw_nav_icon(dl, item.icon,
                  ImVec2(icon_x, p0.y + h * 0.5f), ui_px(0.82f),
                  c32(selected ? k.brand : k.text));
    if (!st.sidebar_compact) {
        const ImVec4 label_color = st.running ? k.muted : (selected ? k.text : k.muted);
        dl->AddText(ImVec2(p0.x + ui_px(46.0f), p0.y + (h - ImGui::GetTextLineHeight()) * 0.5f),
                    c32(label_color), item.label);
    } else if (hovered) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(item.label);
        ImGui::EndTooltip();
    }
    return clicked;
}

void draw_brand_mark(ImDrawList* dl, const ImVec2& center, float scale) {
    const ImU32 amber = c32(k.brand);
    const ImU32 blue = c32(k.brand_hov);
    ImVec2 left(center.x - 16.0f * scale, center.y + 15.0f * scale);
    ImVec2 top(center.x, center.y - 17.0f * scale);
    ImVec2 right(center.x + 16.0f * scale, center.y + 15.0f * scale);
    dl->AddLine(left, top, amber, 5.0f * scale);
    dl->AddLine(top, right, amber, 5.0f * scale);
    dl->AddLine(ImVec2(center.x - 8.0f * scale, center.y + 3.0f * scale),
                ImVec2(center.x + 8.0f * scale, center.y + 3.0f * scale), amber, 5.0f * scale);
    dl->AddLine(ImVec2(center.x + 6.0f * scale, center.y - 14.0f * scale), right, blue,
                3.5f * scale);
    dl->AddBezierCubic(ImVec2(center.x - 22.0f * scale, center.y + 8.0f * scale),
                       ImVec2(center.x - 4.0f * scale, center.y + 25.0f * scale),
                       ImVec2(center.x + 28.0f * scale, center.y + 12.0f * scale),
                       ImVec2(center.x + 24.0f * scale, center.y - 13.0f * scale), blue,
                       2.5f * scale);
    ImVec2 diamond_top(center.x, center.y - 5.0f * scale);
    ImVec2 diamond_right(center.x + 5.0f * scale, center.y + 2.0f * scale);
    ImVec2 diamond_bottom(center.x, center.y + 9.0f * scale);
    ImVec2 diamond_left(center.x - 5.0f * scale, center.y + 2.0f * scale);
    dl->AddQuadFilled(diamond_top, diamond_right, diamond_bottom, diamond_left, blue);
}

void draw_sidebar(UiState& st) {
    const float window_width = visible_window_width(st.hwnd);
    st.sidebar_compact = st.sidebar_collapsed || window_width < ui_px(1180.0f);
    const float sidebar_width = st.sidebar_compact ? ui_px(72.0f) :
                                std::clamp(window_width * 0.19f,
                                           ui_px(224.0f), ui_px(260.0f));
    // The rail is a fixed shell surface.  It must never take ownership of a
    // wheel event intended for the page, otherwise compact windows make the
    // logo/navigation slide away while the main content appears stationary.
    // Keep a scrollbar available for keyboard/drag recovery if an unusually
    // small or high-DPI window cannot fit the complete rail.
    ImGui::BeginChild("##sidebar", ImVec2(sidebar_width, -1), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollWithMouse);
    const ImVec2 sidebar_min = ImGui::GetWindowPos();
    const ImVec2 sidebar_max = sidebar_min + ImGui::GetWindowSize();
    ImGui::GetWindowDrawList()->AddRectFilled(sidebar_min, sidebar_max, c32(k.sidebar));

    // Sliding selection indicator (eases between nav items over ~160ms).
    if (g_nav_indicator_initialized) {
        g_nav_indicator_y.duration = 0.16f;
        const float y = g_nav_indicator_y.update(ImGui::GetIO().DeltaTime);
        const float h = g_nav_indicator_height > 0.0f ? g_nav_indicator_height : ui_px(46.0f);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float ind_left = st.sidebar_compact ? sidebar_min.x + ui_px(4.0f) : sidebar_min.x + ui_px(10.0f);
        const float ind_right = sidebar_max.x - ui_px(4.0f);
        dl->AddRectFilled(ImVec2(ind_left, y), ImVec2(ind_right, y + h), c32(k.sel), ui_px(8.0f));
        // Brand accent bar on the leading edge.
        dl->AddRectFilled(ImVec2(ind_left, y + ui_px(8.0f)),
                          ImVec2(ind_left + ui_px(3.0f), y + h - ui_px(8.0f)),
                          c32(k.brand), ui_px(1.5f));
    }

    ImGui::Dummy(ImVec2(0, ui_px(12.0f)));

    // ─── Logo Header ─────────────────────────────────────────────────────
    if (st.sidebar_compact) {
        const ImVec2 mark_pos = ImGui::GetCursorScreenPos();
        const ImVec2 mark_size(sidebar_width - ui_px(16.0f), ui_px(80.0f));
        draw_local_image(st, st.exe_dir + L"\\branding\\amalgam-logo.png", mark_pos, mark_size,
                         c32(k.sidebar), ImageFit::Contain);
        ImGui::Dummy(mark_size);
        ImGui::Dummy(ImVec2(0, ui_px(6.0f)));
        ImGui::SetCursorPosX((sidebar_width - ui_px(34.0f)) * 0.5f);
        if (ghost_button("<<", ImVec2(ui_px(34.0f), ui_px(22.0f))))
            st.sidebar_collapsed = !st.sidebar_collapsed;
    } else {
        // Real brand mark + title + collapse button on the same line. The
        // source artwork includes the launcher's full identity, so give it a
        // protected tile instead of shrinking it down to a default icon.
        const ImVec2 logo_pos = ImGui::GetCursorScreenPos();
        const float logo_h = ui_px(44.0f);
        ImDrawList* logo_dl = ImGui::GetWindowDrawList();
        ImVec4 logo_halo = k.brand;
        logo_halo.w = 0.13f;
        ImVec4 logo_tile = k.surface2;
        logo_tile.w = 0.72f;
        ImVec4 logo_border = k.brand_hov;
        logo_border.w = 0.52f;
        logo_dl->AddCircleFilled(logo_pos + ImVec2(logo_h * 0.5f, logo_h * 0.5f),
                                 logo_h * 0.54f, c32(logo_halo), 28);
        logo_dl->AddRectFilled(logo_pos, logo_pos + ImVec2(logo_h, logo_h),
                               c32(logo_tile), ui_px(12.0f));
        logo_dl->AddRect(logo_pos, logo_pos + ImVec2(logo_h, logo_h),
                         c32(logo_border), ui_px(12.0f), 0, ui_px(1.0f));
        draw_local_image(st, st.exe_dir + L"\\branding\\amalgam-logo.png", logo_pos,
                         ImVec2(logo_h, logo_h), c32(k.sidebar), ImageFit::Contain);
        ImGui::Dummy(ImVec2(logo_h, logo_h));
        ImGui::SameLine();
        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, logo_pos.y + ui_px(4.0f)));
        ImGui::PushFont(f_bold);
        ImGui::TextColored(k.text, "AMALGAM");
        ImGui::PopFont();
        ImGui::SameLine(0, ui_px(4.0f));
        ImGui::PushFont(f_small);
        ImGui::TextColored(k.muted, "LAUNCHER");
        ImGui::PopFont();
        // Collapse button aligned right
        ImGui::SameLine(sidebar_width - ui_px(44.0f));
        if (ghost_button("<<", ImVec2(ui_px(28.0f), ui_px(22.0f))))
            st.sidebar_collapsed = !st.sidebar_collapsed;
        ImGui::Dummy(ImVec2(0, ui_px(16.0f)));
    }

    // ─── MAIN Section ────────────────────────────────────────────────────
    if (!st.sidebar_compact) {
        ImGui::SetCursorPosX(ui_px(16.0f));
        ImGui::PushFont(f_small);
        ImGui::TextColored(k.muted, "MAIN");
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, ui_px(8.0f)));
    }

    // ─── Navigation Items ────────────────────────────────────────────────
    for (const auto& item : kNav) {
        if (nav_button(st, item)) {
            navigate_to(st, item.id, item.tab);
        }
        if (item.id == 23) {
            int badge = 0;
            try {
                badge += (int)aml::essentials::FriendsManager::instance().get_pending_requests().size();
                for (auto& inv : aml::essentials::InviteManager::instance().get_received_invites()) {
                    if (inv.status == aml::essentials::InviteStatus::Pending) ++badge;
                }
            } catch (...) {}
            if (badge > 0) {
                ImVec2 ir_min = ImGui::GetItemRectMin();
                ImVec2 ir_max = ImGui::GetItemRectMax();
                ImVec2 bp(ir_max.x - ui_px(16.0f), ir_min.y + ui_px(12.0f));
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddCircleFilled(bp, ui_px(8.0f), c32(k.red));
                std::string bl = std::to_string(badge);
                ImVec2 ts = ImGui::CalcTextSize(bl.c_str());
                dl->AddText(ImVec2(bp.x - ts.x * 0.5f, bp.y - ts.y * 0.5f),
                            c32(k.text), bl.c_str());
            }
        }
        ImGui::Dummy(ImVec2(0, ui_px(2.0f)));
    }

    // ─── Separator ───────────────────────────────────────────────────────
    ImGui::Dummy(ImVec2(0, ui_px(8.0f)));
    if (!st.sidebar_compact) {
        const ImVec2 sep_pos = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(
            ImVec2(sep_pos.x + ui_px(16.0f), sep_pos.y),
            ImVec2(sep_pos.x + sidebar_width - ui_px(16.0f), sep_pos.y),
            c32(k.border));
        ImGui::Dummy(ImVec2(0, ui_px(12.0f)));
    }

    // ─── PLAY Section ────────────────────────────────────────────────────
    if (!st.sidebar_compact) {
        ImGui::SetCursorPosX(ui_px(16.0f));
        ImGui::PushFont(f_small);
        ImGui::TextColored(k.muted, "PLAY");
        ImGui::PopFont();
        ImGui::SameLine(sidebar_width - ui_px(30.0f));
        ImGui::TextColored(k.muted, "v");
        ImGui::Dummy(ImVec2(0, ui_px(8.0f)));
    }

    for (const auto& item : kPlayNav) {
        if (nav_button(st, item)) {
            navigate_to(st, item.id, item.tab);
        }
        ImGui::Dummy(ImVec2(0, ui_px(3.0f)));
    }

    // ─── Spacer (pushes profile to bottom) ───────────────────────────────
    const float remaining = ImGui::GetContentRegionAvail().y;
    const float profile_card_h = ui_px(68.0f);
    const float play_btn_h = ui_px(40.0f);
    const float footer_h = profile_card_h + play_btn_h + ui_px(24.0f);
    if (remaining > footer_h + ui_px(20.0f)) {
        ImGui::Dummy(ImVec2(0, remaining - footer_h));
    }

    // ─── Profile Card (Amalgam Account) ──────────────────────────────────
    auto& supabase = aml::supabase::SupabaseManager::instance();
    const bool am_authenticated = supabase.is_authenticated();
    bool mc_authenticated;
    {
        std::lock_guard<std::mutex> lock(st.auth_mu);
        mc_authenticated = !st.account.username.empty();
    }
    auto am_user = am_authenticated ? supabase.get_current_user() : aml::supabase::SupabaseUser();

    ImGui::SetCursorPosX(ui_px(10.0f));
    card_begin("##profile", ImVec2(-1, profile_card_h));
    {
        ImVec2 card_pos = ImGui::GetCursorScreenPos();

        if (!st.sidebar_compact) {
            // Instance info style (matching the image)
            const bool has_profile = !st.selected_instance.id.empty() &&
                                     !st.selected_instance.minecraft_version.empty();
            const char* title = has_profile ? (st.selected_instance.name.empty()
                ? st.selected_instance.id.c_str() : st.selected_instance.name.c_str())
                : "No profile selected";
            const char* subtitle = has_profile ? st.selected_instance.minecraft_version.c_str()
                : (st.selected.empty() ? "Choose a modpack" : st.selected.c_str());

            ImGui::PushFont(f_bold);
            ImGui::TextUnformatted(title);
            ImGui::PopFont();
            ImGui::TextColored(k.muted, "%s", subtitle);
            // Show Amalgam+ badge if premium
            {
                auto& sidebar_ents = aml::entitlements::EntitlementManager::instance();
                if (sidebar_ents.is_plus()) {
                    ImVec2 bp = ImGui::GetCursorScreenPos();
                    ImVec4 plus_bg = k.brand; plus_bg.w = 0.15f;
                    draw_badge(ImGui::GetWindowDrawList(), bp, "AMALGAM+", k.brand, plus_bg);
                    ImGui::Dummy(ImVec2(
                        ImGui::CalcTextSize("AMALGAM+").x + ui_px(14.0f), ui_px(20.0f)));
                } else {
                    ImGui::TextColored(k.muted, "Select a profile");
                }
            }
        } else {
            // Compact: small avatar
            const float avatar_r = ui_px(10.0f);
            ImVec2 avatar_center(card_pos.x + sidebar_width * 0.5f - ui_px(10.0f),
                                 card_pos.y + ui_px(14.0f));
            ImU32 avatar_color = am_authenticated ? c32(k.brand) : c32(k.muted);
            ImGui::GetWindowDrawList()->AddCircleFilled(avatar_center, avatar_r, avatar_color);
            if (am_authenticated) {
                std::string display = am_user.display_name.empty() ? am_user.email : am_user.display_name;
                char initial = display.empty() ? '?' : static_cast<char>(std::toupper(display[0]));
                char initial_str[2] = {initial, '\0'};
                ImVec2 ts = ImGui::CalcTextSize(initial_str);
                ImGui::GetWindowDrawList()->AddText(
                    ImVec2(avatar_center.x - ts.x * 0.5f, avatar_center.y - ts.y * 0.5f),
                    c32(k.text), initial_str);
            }
        }
        card_end();

        // Clickable overlay for dropdown
        ImGui::SetCursorScreenPos(card_pos);
        if (ImGui::InvisibleButton("##profile_click",
                                   ImVec2(sidebar_width - ui_px(20.0f), profile_card_h))) {
            st.account_dropdown_open = true;
        }
    }

    // ─── Account Dropdown Popup ──────────────────────────────────────────
    if (st.account_dropdown_open) {
        ImGui::OpenPopup("##account_dropdown");
        st.account_dropdown_open = false;
    }
    if (ImGui::BeginPopup("##account_dropdown", ImGuiWindowFlags_NoMove)) {
        if (am_authenticated) {
            auto user = supabase.get_current_user();
            std::string display = user.display_name.empty() ? user.email : user.display_name;
            ImGui::TextColored(k.brand, "%s", display.c_str());
            ImGui::TextDisabled("%s", user.email.c_str());
            ImGui::Separator();

            if (mc_authenticated) {
                ImGui::TextColored(k.green, "Minecraft: %s", st.account.username.c_str());
            } else {
                if (ghost_button("Connect Minecraft Account", ImVec2(-1, 0))) {
                    st.ms_connect_popup_open = true;
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::Separator();

            if (ImGui::MenuItem("Account Settings")) {
                navigate_to(st, 15, 15);
            }
            if (ImGui::MenuItem("Security")) {
                navigate_to(st, 15, 15);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Sign Out")) {
                aml::account::AccountManager::instance().end_current_session();
                supabase.sign_out();
                st.account.username.clear();
                push_notice(st, ui_model::NoticeLevel::Info, "Signed out",
                            "You have been signed out of Amalgam");
            }
        } else {
            ImGui::TextDisabled("Amalgam Account");
            ImGui::Separator();
            if (ImGui::MenuItem("Sign In")) {
                ImGui::CloseCurrentPopup();
                st.auth_prompt_dismissed = false;
                st.login_popup_open = true;
            }
            if (ImGui::MenuItem("Create Account")) {
                ImGui::CloseCurrentPopup();
                st.auth_prompt_dismissed = false;
                st.auth_wizard_state = AuthWizardState();
                st.register_popup_open = true;
            }
        }
        ImGui::EndPopup();
    }

    // Auth popups are rendered once from draw_shell after all navigation
    // content. Rendering them here as well caused duplicate popup stacks and
    // Dear ImGui PushStyle assertions.
    if (st.ms_connect_popup_open) {
        ImGui::OpenPopup("Connect Minecraft Account");
        st.ms_connect_popup_open = false;
    }
    ImGui::SetNextWindowSize(ImVec2(
        std::min(ui_px(620.0f), ImGui::GetMainViewport()->WorkSize.x - ui_px(40.0f)),
        std::min(ui_px(500.0f), ImGui::GetMainViewport()->WorkSize.y - ui_px(40.0f))),
        ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    bool connect_open = true;
    if (ImGui::BeginPopupModal("Connect Minecraft Account", &connect_open,
                                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse)) {
        card_begin("##connect_minecraft_hero", ImVec2(-1, ui_px(92.0f)));
        const ImVec2 hero_origin = ImGui::GetCursorScreenPos();
        ImDrawList* hero_draw = ImGui::GetWindowDrawList();
        hero_draw->AddCircleFilled(hero_origin + ImVec2(ui_px(30.0f), ui_px(30.0f)), ui_px(25.0f),
                                   c32(ImVec4(k.brand_dk.x, k.brand_dk.y, k.brand_dk.z, 0.92f)));
        draw_brand_mark(hero_draw, hero_origin + ImVec2(ui_px(30.0f), ui_px(30.0f)), ui_px(1.0f));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ui_px(66.0f));
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("Connect Microsoft");
        ImGui::PopFont();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ui_px(66.0f));
        ImGui::TextColored(k.muted, "Microsoft owns the sign-in. Amalgam prepares your Minecraft profiles.");
        card_end();
        ImGui::Spacing();
        if (st.account.username.empty()) {
            card_begin("##connect_minecraft_steps", ImVec2(-1, 0));
            ImGui::TextColored(k.muted, "SECURE SIGN-IN");
            ImGui::BulletText("Open Microsoft’s secure sign-in page from the next screen.");
            ImGui::BulletText("Enter a one-time code and sign in with the account that owns Minecraft.");
            ImGui::BulletText("Return to Amalgam when Microsoft confirms the connection.");
            card_end();
            ImGui::Spacing();
            if (primary_button("Continue to secure Microsoft sign-in", ImVec2(-1, ui_px(40.0f)))) {
                start_microsoft_login(st);
                ImGui::CloseCurrentPopup();
            }
        } else {
            card_begin("##connect_minecraft_connected", ImVec2(-1, 0));
            ImGui::TextColored(k.green, "MINECRAFT ACCOUNT CONNECTED");
            ImGui::PushFont(f_h2);
            ImGui::TextUnformatted(st.account.username.c_str());
            ImGui::PopFont();
            card_end();
            ImGui::Spacing();
            if (primary_button("Done", ImVec2(-1, ui_px(40.0f)))) {
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::Spacing();
        if (ghost_button("Skip for Now", ImVec2(-1, ui_px(28.0f)))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (!connect_open) st.ms_connect_popup_open = false;

    // ─── Intelligent PLAY Section ──────────────────────────────────────────
    ImGui::Spacing();
    ImGui::SetCursorPosX(ui_px(10.0f));
    if (!st.sidebar_compact) {
        // Show selected profile intelligence above the play button
        const bool has_profile = !st.selected_instance.id.empty() &&
                                 !st.selected_instance.minecraft_version.empty();
        if (has_profile) {
            const auto& inst = st.selected_instance;
            const std::string profile_name = inst.name.empty() ? inst.id : inst.name;
            // Profile status row — derived from real profile health
            {
                const auto health = profile_health(st, inst);
                const bool ready = health.empty();
                ImGui::PushFont(f_small);
                ImGui::TextColored(ready ? k.green : k.yellow,
                                   ready ? "READY" : (std::to_string(health.size()) + " ISSUE(S)").c_str());
                ImGui::PopFont();
                ImGui::Spacing();
            }
            // Profile name
            ImGui::PushFont(f_bold);
            ImGui::TextUnformatted(elide_to_width(profile_name, sidebar_width - ui_px(30.0f)).c_str());
            ImGui::PopFont();
            // Version + Loader
            ImGui::PushFont(f_small);
            ImGui::TextColored(k.muted, "%s  %s",
                               inst.minecraft_version.c_str(),
                               inst.loader.empty() ? "" : inst.loader.c_str());
            ImGui::PopFont();
            ImGui::Spacing();
        } else {
            ImGui::PushFont(f_small);
            ImGui::TextColored(k.muted, "NO PROFILE SELECTED");
            ImGui::PopFont();
            ImGui::PushFont(f_small);
            ImGui::TextColored(k.muted, "Choose a profile");
            ImGui::PopFont();
            ImGui::Spacing();
        }
    }
    // Choose button text based on authentication state
    const bool mc_account = !st.account.username.empty();
    const char* play_label = st.running ? "LAUNCHING..."
        : (mc_account ? "PLAY" : "PLAY VIA MINECRAFT");
    if (primary_button(play_label, ImVec2(-1, play_btn_h), st.running) && !st.running) {
        if (!st.selected.empty()) {
            st.pending_id = st.selected;
            st.pending_instance_dir = st.active_instance_dir;
            st.pending_launch = true;
        } else {
            st.sidebar_item = 3;
            st.active_tab = 6;
        }
    }
    if (!st.sidebar_compact) {
        ImGui::PushFont(f_small);
        if (st.safe_mode) {
            ImGui::TextColored(k.yellow, "SAFE MODE \u2014 optional network features are off");
        }
        if (!mc_account && official_launcher::IsOfficialLauncherInstalled()) {
            ImGui::TextDisabled("Plays via official Minecraft Launcher");
        } else {
            ImGui::TextDisabled("Amalgam Launcher v%s", kVersion);
        }
        ImGui::PopFont();
    }

    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// Top bar
// ---------------------------------------------------------------------------
bool topbar_icon_button(const char* id, NavIcon icon, const char* tooltip, int badge = 0) {
    const ImVec2 size(ui_px(36.0f), ui_px(36.0f));
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, size);
    const bool hovered = ImGui::IsItemHovered();
    const bool focused = ImGui::IsItemFocused();
    if (hovered || focused) {
        ImGui::GetWindowDrawList()->AddRectFilled(p0, p0 + size, c32(k.hover), 8.0f);
    }
    draw_nav_icon(ImGui::GetWindowDrawList(), icon,
                  p0 + ImVec2(size.x * 0.5f, size.y * 0.5f), ui_px(0.74f), c32(k.text));
    if (badge > 0) {
        const ImVec2 badge_center(p0.x + size.x - ui_px(5.0f), p0.y + ui_px(6.0f));
        ImGui::GetWindowDrawList()->AddCircleFilled(badge_center, ui_px(8.0f), c32(k.brand));
        char count[12]{};
        std::snprintf(count, sizeof(count), "%d", badge);
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(badge_center.x - ImGui::CalcTextSize(count).x * 0.5f,
                   badge_center.y - ImGui::GetTextLineHeight() * 0.5f),
            c32(k.text), count);
    }
    if (hovered && tooltip) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(tooltip);
        ImGui::EndTooltip();
    }
    return ImGui::IsItemClicked();
}

bool topbar_compact_account_button(UiState&, const char* label, bool signed_in) {
    const ImVec2 size(ui_px(36.0f), ui_px(36.0f));
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##topbar_account_compact", size);
    const bool hovered = ImGui::IsItemHovered();
    const bool focused = ImGui::IsItemFocused();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (hovered || focused) dl->AddRectFilled(p0, p0 + size, c32(k.hover), ui_px(8.0f));
    const ImVec2 avatar_center = p0 + size * 0.5f;
    dl->AddCircleFilled(avatar_center, ui_px(13.0f), c32(k.brand_dk));
    draw_brand_mark(dl, avatar_center, ui_px(0.33f));
    dl->AddCircleFilled(ImVec2(avatar_center.x + ui_px(9.0f), avatar_center.y + ui_px(9.0f)),
                        ui_px(3.5f), c32(signed_in ? k.green : k.yellow));
    if (hovered) {
        ImGui::BeginTooltip();
        if (signed_in)
            ImGui::Text("Amalgam account: %s", label);
        else
            ImGui::TextUnformatted("Sign in to Amalgam");
        ImGui::EndTooltip();
    }
    return ImGui::IsItemClicked();
}

bool topbar_user_button(UiState&, const char* label, bool signed_in) {
    const float width = ui_px(166.0f);
    const ImVec2 size(width, ui_px(38.0f));
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##topbar_user", size);
    const bool hovered = ImGui::IsItemHovered();
    const bool focused = ImGui::IsItemFocused();
    if (hovered || focused)
        ImGui::GetWindowDrawList()->AddRectFilled(p0, p0 + size, c32(k.hover), 8.0f);
    const ImVec2 avatar_center(p0.x + ui_px(20.0f), p0.y + size.y * 0.5f);
    ImGui::GetWindowDrawList()->AddCircleFilled(avatar_center, ui_px(15.0f), c32(k.brand_dk));
    draw_brand_mark(ImGui::GetWindowDrawList(), avatar_center, ui_px(0.38f));
    ImGui::GetWindowDrawList()->AddCircleFilled(
        ImVec2(avatar_center.x + ui_px(10.0f), avatar_center.y + ui_px(10.0f)),
        ui_px(4.0f), c32(signed_in ? k.green : k.yellow));
    ImGui::PushFont(f_bold);
    ImGui::GetWindowDrawList()->AddText(ImVec2(p0.x + ui_px(43.0f), p0.y + ui_px(7.0f)),
                                        c32(k.text), label);
    ImGui::PopFont();
    ImGui::PushFont(f_small);
    ImGui::GetWindowDrawList()->AddText(ImVec2(p0.x + ui_px(43.0f), p0.y + ui_px(22.0f)),
                                        c32(signed_in ? k.green : k.yellow),
                                        signed_in ? "Amalgam account connected" : "Sign in to Amalgam");
    ImGui::PopFont();
    draw_nav_icon(ImGui::GetWindowDrawList(), NavIcon::Discover,
                  ImVec2(p0.x + width - ui_px(14.0f), p0.y + size.y * 0.5f),
                  ui_px(0.42f), c32(k.muted));
    return ImGui::IsItemClicked();
}

bool topbar_window_button(UiState& st, const char* id, int kind, const char* tooltip) {
    const ImVec2 size(ui_px(34.0f), ui_px(34.0f));
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, size);
    const bool hovered = ImGui::IsItemHovered();
    const bool focused = ImGui::IsItemFocused();
    // Use the foreground draw list so window controls are never clipped by
    // the ImGui child window that contains the topbar.
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (hovered || focused)
        dl->AddRectFilled(p0, p0 + size, c32(kind == 2 ? k.red : k.hover), 6.0f);
    const ImVec2 center = p0 + size * 0.5f;
    const ImU32 color = c32(k.text);
    const float line = ui_px(1.6f);
    if (kind == 0) {
        dl->AddLine(ImVec2(center.x - ui_px(7.0f), center.y),
                    ImVec2(center.x + ui_px(7.0f), center.y), color, line);
    } else if (kind == 1) {
        dl->AddRect(ImVec2(center.x - ui_px(6.0f), center.y - ui_px(6.0f)),
                    ImVec2(center.x + ui_px(6.0f), center.y + ui_px(6.0f)), color, 0.0f, 0, line);
    } else {
        dl->AddLine(ImVec2(center.x - ui_px(6.0f), center.y - ui_px(6.0f)),
                    ImVec2(center.x + ui_px(6.0f), center.y + ui_px(6.0f)), color, line);
        dl->AddLine(ImVec2(center.x + ui_px(6.0f), center.y - ui_px(6.0f)),
                    ImVec2(center.x - ui_px(6.0f), center.y + ui_px(6.0f)), color, line);
    }
    if (hovered) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(tooltip);
        ImGui::EndTooltip();
    }
    if (!ImGui::IsItemClicked()) return false;
    if (kind == 0) {
        ShowWindow(st.hwnd, SW_MINIMIZE);
    } else if (kind == 1) {
        ShowWindow(st.hwnd, IsZoomed(st.hwnd) ? SW_RESTORE : SW_MAXIMIZE);
    } else {
        PostMessageW(st.hwnd, WM_CLOSE, 0, 0);
    }
    return true;
}

void draw_topbar(UiState& st) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    float h = ui_px(68.0f);
    // This child already knows its drawable width.  Deriving it from a
    // screen-space Win32 rectangle mixes coordinate systems on high-DPI
    // monitors and is what caused the top bar to clip or look soft there.
    const float actual_width = std::max(0.0f, ImGui::GetContentRegionAvail().x);
    ImVec2 p1(p0.x + actual_width, p0.y + h);
    const bool compact = actual_width < ui_px(1180.0f);

    dl->AddRectFilled(p0, p1, c32(k.bg));
    dl->AddRectFilled(ImVec2(p0.x, p1.y - ui_px(2.0f)), ImVec2(p1.x, p1.y), c32(k.brand_dk));
    ImGui::SetCursorPos(ImVec2(ui_px(14.0f), ui_px(14.0f)));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, k.surface);
    ImGui::PushStyleColor(ImGuiCol_Border, k.border);
    int active_downloads = 0;
    {
        std::lock_guard<std::mutex> lock(st.jobs_mu);
        for (const auto& job : st.jobs) if (job.active) ++active_downloads;
    }
    int active_notices = 0;
    {
        std::lock_guard<std::mutex> lock(st.notice_mu);
        active_notices = static_cast<int>(st.notices.size());
    }
    const bool signed_in = has_linked_account(st);
    const std::string display_name = player_display_name(st);
    const std::string user_label = display_name.empty() ? "Connect account" : display_name;
    const float icon_width = ui_px(36.0f);
    const float window_width = ui_px(34.0f);
    const float action_gap = ui_px(6.0f);
    const float account_width = compact ? icon_width : ui_px(166.0f);
    const float action_width = ui_model::topbar_action_width(compact, g_ui_scale, account_width);
    const float search_available = std::max(0.0f, actual_width - action_width - ui_px(36.0f));
    const bool show_search = search_available >= ui_px(compact ? 150.0f : 240.0f);
    const float search_width = std::min(ui_px(compact ? 360.0f : 500.0f), search_available);
    if (show_search) {
        ImGui::SetNextItemWidth(search_width);
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_K)) st.focus_global_search = true;
        if (st.focus_global_search) {
            ImGui::SetKeyboardFocusHere();
            st.focus_global_search = false;
        }
        input_text_hint("##global_search", "Search mods, modpacks, shaders...  Ctrl + K", &st.search);
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) && ImGui::IsItemActive() &&
            !st.search.empty() && !st.mod_fetching && !st.mod_installing) {
            st.mod_query = st.search;
            st.browse_category = 0;
            navigate_to(st, 2, 1, 1);
            st.browse_page = 0;
            launch_mod_search(st);
        }
    } else {
        ImGui::Dummy(ImVec2(0, ui_px(34.0f)));
    }
    ImGui::PopStyleColor(2);
    // Anchor the action cluster to the actual right edge of this child, while
    // also clamping it to the viewport. Some ImGui child combinations report
    // a final padding gutter as available width; without this clamp the last
    // compact account icon can land a few pixels beyond the physical window.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    float action_x = ui_model::topbar_action_start(
        p0.x, actual_width, viewport->WorkPos.x, viewport->WorkSize.x,
        action_width, g_ui_scale);
    const float action_y = p0.y + ui_px(14.0f);

    // Window controls (min/max/close) go to the left of the action icons
    // so they're always visible and don't get pushed off-screen.
    {
        float win_x = action_x - (window_width + ui_px(2.0f)) * 3.0f;
        ImGui::SetCursorScreenPos(ImVec2(win_x, action_y));
        topbar_window_button(st, "##window_min", 0, "Minimize");
        win_x += window_width + ui_px(2.0f);
        ImGui::SetCursorScreenPos(ImVec2(win_x, action_y));
        topbar_window_button(st, "##window_max", 1, IsZoomed(st.hwnd) ? "Restore" : "Maximize");
        win_x += window_width + ui_px(2.0f);
        ImGui::SetCursorScreenPos(ImVec2(win_x, action_y));
        topbar_window_button(st, "##window_close", 2, "Close");
    }

    ImGui::SetCursorScreenPos(ImVec2(action_x, action_y));
    if (topbar_icon_button("##top_downloads", NavIcon::Downloads, "Downloads", active_downloads)) {
        st.downloads_open = true;
        navigate_to(st, 4, 14);
    }
    action_x += icon_width + action_gap;
    ImGui::SetCursorScreenPos(ImVec2(action_x, action_y));
    if (topbar_icon_button("##top_alerts", NavIcon::Bell, "Alerts", active_notices))
        st.notice_center_open = !st.notice_center_open;
    action_x += icon_width + action_gap;
    ImGui::SetCursorScreenPos(ImVec2(action_x, action_y));
    if (topbar_icon_button("##top_settings", NavIcon::Settings, "Settings")) {
        navigate_to(st, 12, 4);
    }
    action_x += icon_width + action_gap;
    if (!compact) {
        ImGui::SetCursorScreenPos(ImVec2(action_x, action_y));
        if (topbar_user_button(st, user_label.c_str(), signed_in)) {
            st.account_dropdown_open = true;
        }
    } else {
        ImGui::SetCursorScreenPos(ImVec2(action_x, action_y));
        if (topbar_compact_account_button(st, user_label.c_str(), signed_in)) {
            st.account_dropdown_open = true;
        }
    }

    // The controls are positioned inside the bar; reserve exactly the bar
    // height instead of adding the height after the controls a second time.
    ImGui::SetCursorPosY(h);
    ImGui::Dummy(ImVec2(0, 0));
}

void ensure_instance_list(UiState& st) {
    if (st.instances_loaded) return;
    std::wstring instances_root = (st.cfg->base_dir.empty() ? st.exe_dir : st.cfg->base_dir) +
                                   L"\\instances";
    std::string scan_error;
    st.instance_list = instances::scan(instances_root, &scan_error);
    st.instances_loaded = true;
    st.home_readiness.dirty = true;
    if (!scan_error.empty()) log_line(st, L"[instances] scan failed: " + net::to_wide(scan_error));
}

void draw_home_profile_card(UiState& st, instances::Instance& instance, float width, int index) {
    const float height = ui_px(232.0f);
    ImGui::BeginChild((std::string("##home_recent_") + std::to_string(index)).c_str(),
                      ImVec2(width, height), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const ImVec2 card_min = ImGui::GetWindowPos();
    const ImVec2 card_max = card_min + ImGui::GetWindowSize();
    ImGui::GetWindowDrawList()->AddRectFilled(card_min, card_max, c32(k.surface), ui_px(10.0f));
    const bool card_hovered = ImGui::IsWindowHovered();
    static aml::ui::AnimFloat home_hover[8];
    static bool home_hover_init = false;
    if (!home_hover_init) {
        for (auto& a : home_hover) a.set(0.0f);
        home_hover_init = true;
    }
    aml::ui::AnimFloat& hov = home_hover[index % 8];
    hov.target(card_hovered ? 1.0f : 0.0f);
    const float hov_a = hov.update(ImGui::GetIO().DeltaTime);
    ImGui::GetWindowDrawList()->AddRect(card_min, card_max,
        c32(motion_lerp_color(k.border, k.brand, hov_a)), ui_px(10.0f), 0, ui_px(1.0f));
    if (hov_a > 0.01f) {
        const float glow = hov_a * ui_px(6.0f);
        ImGui::GetWindowDrawList()->AddRect(card_min - ImVec2(glow, glow),
            card_max + ImVec2(glow, glow),
            c32(ImVec4(k.brand.x, k.brand.y, k.brand.z, 0.22f * hov_a)),
            ui_px(14.0f), 0, ui_px(2.0f));
    }
    const float image_height = ui_px(96.0f);
    // Size artwork to the child content region, not the outer card width.  The
    // latter includes window padding and caused covers to bleed into the next
    // card at compact widths and non-100% DPI.
    const ImVec2 art_pos = ImGui::GetCursorScreenPos();
    const float art_width = std::max(ui_px(80.0f), ImGui::GetContentRegionAvail().x);
    draw_instance_art(st, instance, art_pos,
                      ImVec2(art_width, image_height), c32(k.brand_dk));
    // Favorite star overlay on top-right of the art
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 star_pos(art_pos.x + art_width - ui_px(24.0f), art_pos.y + ui_px(10.0f));
        // Subtle dark circle behind star for readability
        dl->AddCircleFilled(star_pos + ImVec2(ui_px(2.0f), ui_px(2.0f)),
                            ui_px(11.0f), c32(ImVec4(0, 0, 0, 0.5f)));
        draw_favorite_star(dl, star_pos, ui_px(9.0f),
                           c32(instance.favorite ? k.brand_hov : ImVec4(0.7f, 0.7f, 0.7f, 0.8f)),
                           instance.favorite);
    }
    ImGui::Dummy(ImVec2(0, image_height + ui_px(8.0f)));
    const bool art_clicked = ImGui::IsItemClicked();
    ImGui::PushFont(f_bold);
    ImGui::TextUnformatted(instance.name.empty() ? instance.id.c_str() : instance.name.c_str());
    ImGui::PopFont();
    // Version + loader line
    ImGui::PushFont(f_small);
    ImGui::TextColored(k.muted, "%s  %s", instance.minecraft_version.c_str(),
                       instance.loader.empty() ? "" : instance.loader.c_str());
    ImGui::PopFont();
    // Source + activity + health status
    ImGui::PushFont(f_small);
    const ImVec2 meta_start = ImGui::GetCursorScreenPos();
    // Source badge
    if (!instance.pack_source.empty()) {
        ImVec4 src_col = instance.pack_modified ? k.yellow : k.green;
        ImGui::TextColored(src_col, "%s", instance.pack_source.c_str());
        ImGui::SameLine(0, ui_px(8.0f));
    }
    // Activity label
    ImGui::TextColored(k.muted, "%s", profile_activity_label(instance).c_str());
    ImGui::PopFont();
    ImGui::Spacing();
    const float open_width = ui_px(58.0f);
    const float more_width = ui_px(30.0f);
    const float action_width = std::max(ui_px(64.0f),
                                        ImGui::GetContentRegionAvail().x - open_width - more_width - ui_px(12.0f));
    if (primary_button("PLAY", ImVec2(action_width, ui_px(34.0f))) && !st.running) {
        st.selected = instance.minecraft_version;
        st.pending_id = st.selected;
        st.active_instance_dir = instance.directory;
        st.pending_instance_dir = instance.directory;
        st.pending_launch = true;
        show_toast("Launching", ("Starting '" + (instance.name.empty() ? instance.id : instance.name) + "'").c_str(), k.blue, 2.0f);
    }
    ImGui::SameLine(0, ui_px(6.0f));
    if (ghost_button("Open", ImVec2(open_width, ui_px(34.0f))))
        open_instance_detail(st, instance);
    ImGui::SameLine(0, ui_px(6.0f));
    const std::string menu_id = "##home_profile_more_" + std::to_string(index);
    if (ghost_button("...", ImVec2(ui_px(30.0f), ui_px(34.0f)))) {
        ImGui::OpenPopup(menu_id.c_str());
    }
    draw_instance_overflow_menu(st, instance, menu_id.c_str());
    const bool open_card = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup) &&
                           ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                           !ImGui::IsAnyItemHovered();
    if (art_clicked || open_card) open_instance_detail(st, instance);
    ImGui::EndChild();
}

void draw_home_create_card(UiState& st, float width, int index) {
    const float height = ui_px(232.0f);
    ImGui::BeginChild((std::string("##home_create_") + std::to_string(index)).c_str(),
                      ImVec2(width, height), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const ImVec2 card_min = ImGui::GetWindowPos();
    const ImVec2 card_max = card_min + ImGui::GetWindowSize();
    ImGui::GetWindowDrawList()->AddRectFilled(card_min, card_max, c32(k.bg), ui_px(10.0f));
    static AnimFloat create_hover;
    create_hover.target(ImGui::IsWindowHovered() ? 1.0f : 0.0f);
    const float create_hov = create_hover.update(ImGui::GetIO().DeltaTime);
    ImGui::GetWindowDrawList()->AddRect(card_min, card_max,
        c32(motion_lerp_color(k.brand, k.brand_hov, create_hov)), ui_px(10.0f),
        0, ui_px(create_hov > 0.5f ? 2.0f : 1.0f));
    // Original bundled scenery supports the first-run action only. Provider
    // project covers are always kept as their real upstream artwork.
    const std::wstring onboarding_art = st.exe_dir + L"\\branding\\ai\\onboarding-portal-ai.png";
    std::error_code onboarding_error;
    const std::wstring onboarding_fallback =
        std::filesystem::exists(onboarding_art, onboarding_error) && !onboarding_error
            ? onboarding_art
            : st.exe_dir + L"\\branding\\amalgam-onboarding-portal.png";
    draw_local_image(st, onboarding_fallback, card_min,
                     ImVec2(width, height), c32(k.surface2));
    ImGui::GetWindowDrawList()->AddRectFilledMultiColor(
        card_min, card_min + ImVec2(width, height),
        c32(ImVec4(k.sidebar.x, k.sidebar.y, k.sidebar.z, 0.93f)),
        c32(ImVec4(k.sidebar.x, k.sidebar.y, k.sidebar.z, 0.38f)),
        c32(ImVec4(k.sidebar.x, k.sidebar.y, k.sidebar.z, 0.18f)),
        c32(ImVec4(k.sidebar.x, k.sidebar.y, k.sidebar.z, 0.86f)));
    const ImVec2 center = card_min + ImVec2(width * 0.5f, ui_px(84.0f));
    const float radius = ui_px(23.0f);
    ImGui::GetWindowDrawList()->AddCircle(center, radius, c32(k.brand_hov),
        std::max(12, static_cast<int>(ui_px(32.0f))), ui_px(2.0f));
    ImGui::GetWindowDrawList()->AddLine(ImVec2(center.x - ui_px(10.0f), center.y),
                                        ImVec2(center.x + ui_px(10.0f), center.y), c32(k.brand_hov), ui_px(2.0f));
    ImGui::GetWindowDrawList()->AddLine(ImVec2(center.x, center.y - ui_px(10.0f)),
                                        ImVec2(center.x, center.y + ui_px(10.0f)), c32(k.brand_hov), ui_px(2.0f));
    ImGui::SetCursorPos(ImVec2(0, ui_px(122.0f)));
    ImGui::PushFont(f_bold);
    const char* title = "Install a new modpack";
    ImGui::SetCursorPosX(std::max(ui_px(8.0f), (width - ImGui::CalcTextSize(title).x) * 0.5f));
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    ImGui::PushFont(f_small);
    const char* detail = "Create a custom profile or import a pack";
    ImGui::SetCursorPosX(std::max(ui_px(8.0f), (width - ImGui::CalcTextSize(detail).x) * 0.5f));
    ImGui::TextColored(k.muted, "%s", detail);
    ImGui::PopFont();
    ImGui::SetCursorPos(ImVec2(0, ui_px(180.0f)));
    ImGui::InvisibleButton("##home_create_action", ImVec2(width, ui_px(42.0f)));
    const bool account_modal_open =
        ImGui::IsPopupOpen("Amalgam Account Setup") ||
        ImGui::IsPopupOpen("Sign In to Amalgam") ||
        ImGui::IsPopupOpen("Microsoft Sign In");
    if (!account_modal_open && (ImGui::IsItemClicked() ||
        (ImGui::IsMouseHoveringRect(card_min, card_min + ImVec2(width, height)) &&
         ImGui::IsMouseClicked(ImGuiMouseButton_Left))))
        st.wizard_open = true;
    ImGui::EndChild();
}

bool draw_home_panel_header(const char* title, const char* action = nullptr) {
    bool clicked = false;
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    if (action) {
        const float target_x = ImGui::GetWindowWidth() - ui_px(116.0f);
        ImGui::SameLine(std::max(ImGui::GetCursorPosX(), target_x));
        clicked = ghost_button(action, ImVec2(ui_px(70.0f), ui_px(28.0f)));
    }
    return clicked;
}

void draw_home_tab(UiState& st) {
    ensure_instance_list(st);
    const float width = ImGui::GetContentRegionAvail().x;
    const std::string username = player_display_name(st);
    const bool signed_in = has_linked_account(st);
    draw_breadcrumbs({"Home"});
    ImGui::PushFont(f_title);
    if (username.empty())
        ImGui::TextUnformatted("Welcome to Amalgam");
    else
        ImGui::Text("Welcome back, %s!", username.c_str());
    ImGui::PopFont();
    ImGui::TextColored(k.muted, signed_in
                       ? "Manage profiles and mods here. The official Minecraft Launcher handles sign-in when you press Play."
                       : "Sign in to Amalgam for account features; local profiles and mod tools remain available.");
    if (!signed_in) {
        ImGui::SameLine();
        if (ghost_button("Sign in to Amalgam##home", ImVec2(ui_px(178.0f), ui_px(30.0f)))) {
            st.auth_prompt_dismissed = false;
            st.login_popup_open = true;
        }
    }
    ImGui::Spacing();

    // The brand art is a welcome accent, not the whole dashboard. This cap
    // keeps Recently Played and first-run actions visible on normal desktop
    // heights as well as high-DPI displays.
    const float banner_height = std::clamp(width * 0.16f, ui_px(128.0f), ui_px(196.0f));
    card_begin("##home_brand_banner", ImVec2(-1, banner_height));
    const ImVec2 banner_pos = ImGui::GetCursorScreenPos();
    const ImVec2 banner_size = ImGui::GetContentRegionAvail();
    // Use scenery without embedded typography.  The previous banner already
    // contained the Amalgam wordmark, so rotating copy rendered on top of it
    // and made the dashboard look blurred/duplicated.
    const std::wstring discover_ai = st.exe_dir + L"\\branding\\ai\\amalgam-discover-hero-ai.png";
    std::error_code discover_error;
    const std::wstring discover_art =
        std::filesystem::exists(discover_ai, discover_error) && !discover_error
            ? discover_ai
            : st.exe_dir + L"\\branding\\amalgam-discover-hero.png";
    draw_local_image(st, discover_art, banner_pos, banner_size, c32(k.brand_dk));
    // Overlay text on the hero banner (rotates between content types)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        // Gradient overlay at bottom for text readability
        dl->AddRectFilledMultiColor(
            banner_pos,
            ImVec2(banner_pos.x + banner_size.x, banner_pos.y + banner_size.y),
            c32(ImVec4(0, 0, 0, 0)),
            c32(ImVec4(0, 0, 0, 0)),
            c32(ImVec4(0, 0, 0, 0.6f)),
            c32(ImVec4(0, 0, 0, 0.6f)));

        // Rotating hero content: news / launcher update / featured pack /
        // announcement. Crossfades every 7s with a 250ms ease.
        struct HeroSlide {
            const char* title;
            const char* subtitle;
            const char* action;   // button label, empty = none
            const char* nav_action; // 'discover' | 'settings' | 'downloads' | ''
        };
        static const std::string kHeroVersionTitle = std::string("AMALGAM ") + kVersion;
        static const HeroSlide kHeroSlides[] = {
            {kHeroVersionTitle.c_str(),
             "Your unified Minecraft platform — launch, discover, and play together.",
             "", ""},
            {"Discover Your Next Adventure",
             "Browse thousands of modpacks, mods, shaders, and resource packs.",
             "Explore", "discover"},
            {"Play With Friends",
             "Host worlds and join sessions through Essentials, free with your account.",
             "Open Essentials", "essentials"},
            {"Built-in AI",
             "Local AI for coding, vision, and art — no cloud API required.",
             "", ""},
        };
        constexpr int kHeroCount = 4;
        constexpr float kHeroDuration = 7.0f;
        static int hero_index = 0;
        static float hero_elapsed = 0.0f;
        static AnimFloat hero_fade;
        hero_fade.set(1.0f);
        hero_fade.duration = 0.25f;
        hero_elapsed += ImGui::GetIO().DeltaTime;
        if (hero_elapsed >= kHeroDuration) {
            hero_elapsed = 0.0f;
            hero_index = (hero_index + 1) % kHeroCount;
            hero_fade.set(0.0f);
            hero_fade.target(1.0f);
        }
        const float fade = hero_fade.update(ImGui::GetIO().DeltaTime);
        const auto& slide = kHeroSlides[hero_index];

        const float title_y = banner_pos.y + banner_size.y - ui_px(72.0f);
        ImGui::PushFont(f_title);
        dl->AddText(ImVec2(banner_pos.x + ui_px(24.0f), title_y),
                    c32(ImVec4(1, 1, 1, fade)), slide.title);
        ImGui::PopFont();
        ImGui::PushFont(f_body);
        dl->AddText(ImVec2(banner_pos.x + ui_px(24.0f),
                           banner_pos.y + banner_size.y - ui_px(40.0f)),
                    c32(ImVec4(0.9f, 0.9f, 0.9f, 0.9f * fade)),
                    slide.subtitle);
        ImGui::PopFont();

        // Slide action button (drawn, not interactive — click target below).
        if (slide.action[0] && fade > 0.5f) {
            const ImVec2 btn_pos(banner_pos.x + banner_size.x - ui_px(140.0f),
                                 banner_pos.y + banner_size.y - ui_px(56.0f));
            dl->AddRectFilled(btn_pos, btn_pos + ImVec2(ui_px(116.0f), ui_px(34.0f)),
                              c32(k.brand), ui_px(8.0f));
            ImGui::PushFont(f_bold);
            dl->AddText(ImVec2(btn_pos.x + ui_px(14.0f), btn_pos.y + ui_px(7.0f)),
                        IM_COL32(255,255,255,255), slide.action);
            ImGui::PopFont();
        }

        // Click target for the hero action (full-width strip over the button).
        if (slide.nav_action[0]) {
            const ImVec2 btn_pos(banner_pos.x + banner_size.x - ui_px(140.0f),
                                 banner_pos.y + banner_size.y - ui_px(56.0f));
            ImGui::SetCursorScreenPos(btn_pos);
            if (ImGui::InvisibleButton("##hero_action", ImVec2(ui_px(116.0f), ui_px(34.0f)))) {
                if (std::string(slide.nav_action) == "discover") navigate_to(st, 2, 16, 0);
                else if (std::string(slide.nav_action) == "essentials") navigate_to(st, 23, 23);
                else if (std::string(slide.nav_action) == "cloud") {
                    ShellExecuteW(st.hwnd, L"open",
                                  aml::net::to_wide(aml::online::config().plans_url()).c_str(),
                                  nullptr, nullptr, SW_SHOWNORMAL);
                }
            }
        }
    }
    ImGui::Dummy(ImVec2(0, banner_size.y));
    card_end();
    ImGui::Spacing();

    // ── Quick Continue ────────────────────────────────────────────────
    // Show the most recently played profile prominently for instant resume
    {
        instances::Instance* most_recent = nullptr;
        for (auto& inst : st.instance_list) {
            if (inst.last_played > 0) {
                if (!most_recent || inst.last_played > most_recent->last_played)
                    most_recent = &inst;
            }
        }
        if (most_recent) {
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ui_px(10.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ui_px(16.0f), ui_px(12.0f)));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, k.surface);
            ImGui::PushStyleColor(ImGuiCol_Border, k.brand);
            const float qc_height = ui_px(82.0f);
            ImGui::BeginChild("##home_quick_continue", ImVec2(-1, qc_height),
                              ImGuiChildFlags_Borders,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            const ImVec2 qc_pos = ImGui::GetWindowPos();
            const ImVec2 qc_size = ImGui::GetWindowSize();
            // Subtle brand accent line on left
            ImGui::GetWindowDrawList()->AddRectFilled(
                qc_pos,                              ImVec2(qc_pos.x + ui_px(3.0f), qc_pos.y + qc_size.y),
                c32(k.brand), ui_px(2.0f));
            // Icon area
            draw_instance_art(st, *most_recent,
                              ImVec2(qc_pos.x + ui_px(14.0f), qc_pos.y + ui_px(17.0f)),
                              ImVec2(ui_px(48.0f), ui_px(48.0f)), c32(k.brand_dk), 8.0f);
            // Profile info: pin each line to the info column so the meta
            // row can never drift over the art regardless of name width.
            const float info_x = qc_pos.x + ui_px(88.0f);
            const float info_width = std::max(ui_px(80.0f), qc_size.x - ui_px(224.0f));
            const float name_y = qc_pos.y + ui_px(16.0f);
            ImGui::SetCursorScreenPos(ImVec2(info_x, name_y));

            ImGui::PushFont(f_h2);
            ImGui::TextUnformatted(
                elide_to_width(most_recent->name.empty() ? most_recent->id : most_recent->name,
                               info_width).c_str());
            ImGui::PopFont();
            ImGui::SetCursorScreenPos(ImVec2(info_x, name_y + ImGui::GetTextLineHeight() + ui_px(6.0f)));
            ImGui::PushFont(f_small);
            ImGui::TextColored(k.muted, "%s  |  %s  %s",
                               profile_activity_label(*most_recent).c_str(),
                               most_recent->minecraft_version.c_str(),
                               most_recent->loader.empty() ? "" : most_recent->loader.c_str());
            ImGui::PopFont();
            // Play button
            ImGui::SetCursorScreenPos(ImVec2(qc_pos.x + qc_size.x - ui_px(120.0f),
                                              qc_pos.y + (qc_height - ui_px(36.0f)) * 0.5f));
            if (primary_button("QUICK PLAY", ImVec2(ui_px(100.0f), ui_px(36.0f))) && !st.running) {
                st.selected = most_recent->minecraft_version;
                st.pending_id = st.selected;
                st.active_instance_dir = most_recent->directory;
                st.pending_instance_dir = most_recent->directory;
                st.pending_launch = true;
                show_toast("Launching",
                           ("Starting '" + (most_recent->name.empty() ? most_recent->id : most_recent->name) + "'").c_str(),
                           k.blue, 2.0f);
            }
            ImGui::EndChild();
            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar(2);
            ImGui::Spacing();
        }
    }

    if (draw_home_panel_header("Recently Played", "View all")) {
        st.sidebar_item = 3;
        st.active_tab = 6;
    }
    ImGui::Spacing();
    std::vector<instances::Instance*> recent;
    for (auto& instance : st.instance_list) recent.push_back(&instance);
    std::stable_sort(recent.begin(), recent.end(), [](const auto* a, const auto* b) {
        if (a->favorite != b->favorite) return a->favorite;
        return a->last_played > b->last_played;
    });
    const int columns = width >= ui_px(1240.0f) ? 5 : width >= ui_px(780.0f) ? 3 : 2;
    const float gap = ui_px(12.0f);
    // Reserve the scrollbar/border gutter explicitly.  Child windows can expose
    // their vertical scrollbar after this measurement, and without the reserve
    // the final card can be clipped by a few pixels at compact desktop widths.
    const float grid_width = std::max(0.0f, width - ui_px(18.0f));
    const float card_width = std::max(ui_px(170.0f), (grid_width - gap * static_cast<float>(columns - 1)) /
                                                       static_cast<float>(columns));
    const int recent_count = std::min(4, static_cast<int>(recent.size()));
    int rendered = 0;
    for (int i = 0; i < recent_count; ++i) {
        if (rendered % columns) ImGui::SameLine(0, gap);
        draw_home_profile_card(st, *recent[i], card_width, rendered++);
    }
    if (rendered % columns) ImGui::SameLine(0, gap);
    draw_home_create_card(st, card_width, rendered);
    ImGui::Spacing();

    const float panel_gap = ui_px(12.0f);
    const int panel_columns = width >= ui_px(1050.0f) ? 3 : 1;
    const float panel_width = panel_columns == 3 ?
        (width - panel_gap * 2.0f) / 3.0f : width;
    auto panel_begin = [&](const char* id) {
        ImGui::BeginChild(id, ImVec2(panel_width, ui_px(252.0f)), ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        const ImVec2 panel_min = ImGui::GetWindowPos();
        const ImVec2 panel_max = panel_min + ImGui::GetWindowSize();
        ImGui::GetWindowDrawList()->AddRectFilled(panel_min, panel_max, c32(k.surface), ui_px(10.0f));
        ImGui::GetWindowDrawList()->AddRect(panel_min, panel_max, c32(k.border), ui_px(10.0f),
                                             0, ui_px(1.0f));
    };
    auto panel_end = [&]() {
        ImGui::EndChild();
    };
    panel_begin("##home_updates");
    draw_home_panel_header("Updates");
    ImGui::Separator();
    bool readiness_checked = false;
    {
        std::lock_guard<std::mutex> lock(st.readiness_mu);
        readiness_checked = st.readiness_checked;
    }
    // Render the cached summary; a background worker recomputes it when the
    // underlying data changed (see refresh_home_readiness_async).
    if (st.home_readiness.dirty.exchange(false)) {
        refresh_home_readiness_async(st);
    }
    int ready_profiles = 0;
    int profiles_needing_attention = 0;
    std::string first_profile_issue;
    {
        std::lock_guard<std::mutex> lock(st.home_readiness.mu);
        ready_profiles = st.home_readiness.ready_profiles;
        profiles_needing_attention = st.home_readiness.attention_profiles;
        first_profile_issue = st.home_readiness.first_issue;
    }
    if (!readiness_checked || st.instance_list.empty()) {
        ImGui::TextColored(k.muted, "Profile readiness: Not checked");
    } else if (st.home_readiness.computing.load() && ready_profiles == 0 &&
               profiles_needing_attention == 0) {
        ImGui::TextColored(k.muted, "Profile readiness: checking...");
    } else if (profiles_needing_attention == 0) {
        ImGui::TextColored(k.green, "%d profile(s) ready to play", ready_profiles);
    } else if (ready_profiles > 0) {
        ImGui::TextColored(k.yellow, "%d profile(s) ready; %d need attention: %s",
                           ready_profiles, profiles_needing_attention, first_profile_issue.c_str());
    } else {
        ImGui::TextColored(k.yellow, "Profiles need attention: %s", first_profile_issue.c_str());
    }
    ImGui::Spacing();
    if (primary_button("Manage all downloads", ImVec2(-1, ui_px(36.0f)))) {
        st.sidebar_item = 4;
        st.active_tab = 17;
        st.downloads_open = true;
    }
    ImGui::Spacing();
    ImGui::TextColored(k.muted, "Keep your profiles and installed content up to date from one place.");
    panel_end();
    if (panel_columns == 3) ImGui::SameLine(0, panel_gap);

    panel_begin("##home_activity");
    if (draw_home_panel_header("Library activity", "View all")) {
        st.sidebar_item = 3;
        st.active_tab = 6;
    }
    ImGui::Separator();
    if (st.instance_list.empty()) {
        ImGui::PushFont(f_bold);
        ImGui::TextUnformatted("No profiles yet");
        ImGui::PopFont();
        ImGui::TextColored(k.muted, "Create a profile or install a modpack to get started.");
        ImGui::Spacing();
        if (primary_button("Create Profile", ImVec2(ui_px(160.0f), ui_px(36.0f)))) st.wizard_open = true;
        ImGui::SameLine();
        if (ghost_button("Browse Modpacks", ImVec2(ui_px(160.0f), ui_px(36.0f)))) { st.sidebar_item = 2; }
    } else {
        const auto* newest = &st.instance_list.front();
        for (const auto& instance : st.instance_list) {
            if (instance.last_played > newest->last_played) newest = &instance;
        }
        ImGui::PushFont(f_bold);
        ImGui::TextUnformatted(newest->name.empty() ? newest->id.c_str() : newest->name.c_str());
        ImGui::PopFont();
        ImGui::TextColored(k.muted, "%s  |  %s", profile_activity_label(*newest).c_str(),
                           newest->pack_source.empty() ? "Custom profile" : newest->pack_source.c_str());
        ImGui::Separator();
        ImGui::TextColored(k.muted, "%d profile(s) saved locally", static_cast<int>(st.instance_list.size()));
        ImGui::Spacing();
        if (ghost_button("Open Library", ImVec2(-1, ui_px(34.0f)))) {
            st.sidebar_item = 3;
            st.active_tab = 6;
        }
    }
    panel_end();
    if (panel_columns == 3) ImGui::SameLine(0, panel_gap);

    panel_begin("##home_recommended");
    if (draw_home_panel_header("Recommended for you", "View all")) {
        st.sidebar_item = 2;
        st.active_tab = 16;
    }
    ImGui::Separator();
    std::vector<mods::SearchResult> recommendations;
    {
        std::lock_guard<std::mutex> lock(st.mod_mu);
        recommendations = st.home_packs;
    }
    const int recommendation_count = std::min(4, static_cast<int>(recommendations.size()));
    for (int i = 0; i < recommendation_count; ++i) {
        const auto& item = recommendations[i];
        const std::string row_id = "##home_recommended_" + std::to_string(i);
        if (draw_catalog_compact_row(st, item, row_id.c_str(),
                                     ImVec2(-1, ui_px(48.0f)), false)) {
            open_project_detail(st, item);
        }
        if (i + 1 < recommendation_count) ImGui::Dummy(ImVec2(0.0f, ui_px(2.0f)));
    }
    if (recommendation_count == 0)
        ImGui::TextColored(k.muted, "Recommendations will appear after the catalog loads.");
    panel_end();

    ImGui::Spacing();
    int completed_downloads = 0;
    {
        std::lock_guard<std::mutex> lock(st.jobs_mu);
        for (const auto& job : st.jobs) if (job.completed) ++completed_downloads;
    }
    const int recently_played = static_cast<int>(std::count_if(
        st.instance_list.begin(), st.instance_list.end(), [](const instances::Instance& instance) {
            return instance.last_played > 0;
        }));
    const int favorites = static_cast<int>(std::count_if(
        st.instance_list.begin(), st.instance_list.end(), [](const instances::Instance& instance) {
            return instance.favorite;
        }));
    const int stats[] = {static_cast<int>(st.instance_list.size()), recently_played, favorites,
                         completed_downloads, static_cast<int>(st.cfg->servers.size())};
    const char* stat_labels[] = {"Profiles", "Recently played", "Favorites", "Downloads", "Servers"};
    const float stat_gap = ui_px(8.0f);
    const int stat_columns = width >= ui_px(1150.0f) ? 5 : width >= ui_px(760.0f) ? 3 : 2;
    const float stat_width = (width - stat_gap * static_cast<float>(stat_columns - 1)) /
                             static_cast<float>(stat_columns);
    for (int i = 0; i < 5; ++i) {
        if (i && i % stat_columns) ImGui::SameLine(0, stat_gap);
        else if (i) ImGui::Spacing();
        card_begin((std::string("##home_stat_") + std::to_string(i)).c_str(), ImVec2(stat_width, ui_px(72.0f)));
        draw_home_stat_icon(ImGui::GetWindowDrawList(), i,
                            ImGui::GetCursorScreenPos() + ImVec2(ui_px(11.0f), ui_px(11.0f)),
                            ui_px(0.85f), c32(k.brand_hov));
        ImGui::Dummy(ImVec2(ui_px(22.0f), ui_px(28.0f)));
        ImGui::SameLine(0, ui_px(16.0f));
        ImGui::PushFont(f_h2);
        ImGui::Text("%d", stats[i]);
        ImGui::PopFont();
        ImGui::TextColored(k.muted, "%s", stat_labels[i]);
        card_end();
    }
}

// ---------------------------------------------------------------------------
// Discover / home tab
// ---------------------------------------------------------------------------
void discover_card(UiState& st, const char* title, const char* version, const char* stats, int accent,
                   bool selected, const char* image_url = nullptr,
                   const mods::SearchResult* project = nullptr, int project_index = -1) {
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float h = ui_px(156.0f);
    const float artwork_h = ui_px(92.0f);
    const float rounded = ui_px(10.0f);
    ImVec2 p1(p0.x + avail.x, p0.y + h);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec4 c = accent == 1 ? k.green : accent == 2 ? k.orange : k.brand;
    dl->AddRectFilled(p0, p1, c32(k.surface), rounded);
    dl->AddRectFilled(p0, ImVec2(p1.x, p0.y + artwork_h),
                      c32(ImVec4(c.x * 0.38f, c.y * 0.38f, c.z * 0.38f, 1)), rounded);
    dl->AddRectFilled(ImVec2(p0.x, p0.y + artwork_h - ui_px(30.0f)),
                      ImVec2(p1.x, p0.y + artwork_h),
                      c32(ImVec4(c.x * 0.38f, c.y * 0.38f, c.z * 0.38f, 1)));
    const bool has_provider_cover = image_url && *image_url;
    if (project)
        draw_catalog_project_art(st, *project, p0, ImVec2(p1.x - p0.x, artwork_h),
                                 c32(ImVec4(c.x * 0.38f, c.y * 0.38f, c.z * 0.38f, 1)),
                                 ImageFit::Cover, project_index);
    else if (has_provider_cover)
        draw_project_image(st, image_url, p0, ImVec2(p1.x - p0.x, artwork_h),
                           c32(ImVec4(c.x * 0.38f, c.y * 0.38f, c.z * 0.38f, 1)));
    else {
        const wchar_t* cover = accent == 1 ? L"amalgam-cover-sky.png" :
                               accent == 2 ? L"amalgam-cover-forge.png" :
                                             L"amalgam-cover-portal.png";
        draw_local_image(st, st.exe_dir + L"\\branding\\" + cover, p0,
                         ImVec2(p1.x - p0.x, artwork_h),
                         c32(ImVec4(c.x * 0.38f, c.y * 0.38f, c.z * 0.38f, 1)),
                         ImageFit::Cover);
    }
    if (selected) dl->AddRect(p0, p1, c32(k.brand_hov), rounded, 0, ui_px(2.0f));
    ImGui::PushFont(f_bold);
    dl->AddText(ImVec2(p0.x + ui_px(12.0f), p0.y + ui_px(103.0f)), c32(k.text), title);
    ImGui::PopFont();
    ImGui::PushFont(f_small);
    dl->AddText(ImVec2(p0.x + ui_px(12.0f), p0.y + ui_px(126.0f)), c32(k.muted), version);
    dl->AddText(ImVec2(p1.x - ImGui::CalcTextSize(stats).x - ui_px(12.0f),
                        p0.y + ui_px(126.0f)), c32(k.muted), stats);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(avail.x, h + ui_px(10.0f)));
    if (ImGui::IsItemClicked()) {
        if (project) {
            if (project_index >= 0) st.home_pack_selected = project_index;
            open_project_detail(st, *project);
        } else {
            st.active_tab = 1;
            st.sidebar_item = 2;
            st.browse_category = 1;
            st.mod_query = title;
        }
    }
}

void draw_provider_tabs(UiState& st) {
    const char* tabs[] = {"Discover", "All content", "My Modpacks"};
    for (int i = 0; i < 3; ++i) {
        if (i) ImGui::SameLine(0, ui_px(28.0f));
        const bool active = i == st.provider_tab;
        const ImVec2 label_size = ImGui::CalcTextSize(tabs[i]);
        const ImVec2 tab_size(label_size.x + ui_px(22.0f), ui_px(34.0f));
        const ImVec2 tab_pos = ImGui::GetCursorScreenPos();
        ImGui::PushStyleColor(ImGuiCol_Text, active ? k.text : k.muted);
        ImGui::PushFont(active ? f_bold : f_body);
        if (ImGui::Selectable((std::string("##provider_tab_") + std::to_string(i)).c_str(),
                             active, ImGuiSelectableFlags_None, tab_size)) {
            st.provider_tab = i;
            if (i == 0) {
                navigate_to(st, 2, 15, 0);
            } else if (i == 2) {
                navigate_to(st, 3, 6, 2);
            } else {
                navigate_to(st, 2, 1, 1);
                st.browse_category = 0;
            }
        }
        ImGui::PopFont();
        ImGui::PopStyleColor();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 tab_color = c32(active ? k.brand : k.border);
        dl->AddRectFilled(ImVec2(tab_pos.x, tab_pos.y + tab_size.y - ui_px(2.0f)),
                          ImVec2(tab_pos.x + tab_size.x, tab_pos.y + tab_size.y),
                          tab_color, ui_px(1.0f));
        dl->AddText(ImVec2(tab_pos.x + ui_px(11.0f), tab_pos.y + ui_px(7.0f)),
                    c32(active ? k.text : k.muted), tabs[i]);
    }
    ImGui::Separator();
}

void draw_mods_tab(UiState& st);

void draw_discover_tab(UiState& st) {
    // When a content sub-tab is active, delegate to draw_mods_tab
    if (st.discover_sub_tab > 0) {
        draw_mods_tab(st);
        return;
    }

    // ── Context header (adding to profile) ────────────────────────────────
    if (!st.active_instance_dir.empty() && !st.selected.empty()) {
        card_begin("##ctx_header", ImVec2(-1, ui_px(40.0f)));
        ImGui::TextColored(k.muted, "Adding to:");
        ImGui::SameLine();
        ImGui::PushFont(f_bold);
        ImGui::Text("%s", st.selected_instance.name.c_str());
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::TextColored(k.muted, "  %s • %s",
                           st.selected_instance.minecraft_version.c_str(),
                           st.selected_instance.loader.c_str());
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - ui_px(70.0f));
        if (ghost_button("Change", ImVec2(ui_px(60.0f), ui_px(26.0f)))) {
            st.active_instance_dir.clear();
            st.selected.clear();
        }
        card_end();
        ImGui::Spacing();
    }

    // ── Discover title ────────────────────────────────────────────────────
    draw_page_emblem(st, "discover-emblem-ai.png");
    draw_breadcrumbs({"Home", "Discover"});
    ImGui::PushFont(f_title);
    ImGui::TextUnformatted("Discover");
    ImGui::PopFont();
    ImGui::Spacing();

    // ── Content type tabs ─────────────────────────────────────────────────
    struct ContentTab { const char* label; int category; int idx; };
    const ContentTab ctabs[] = {
        {"Modpacks", 1, 1}, {"Mods", 0, 2}, {"Shaders", 2, 3},
        {"Resource Packs", 3, 4}, {"Data Packs", 4, 5},
    };
    for (const auto& ct : ctabs) {
        ImGui::SameLine(0, ui_px(4.0f));
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImVec2 sz = ImGui::CalcTextSize(ct.label) + ImVec2(ui_px(16.0f), ui_px(6.0f));
        const bool active = st.discover_sub_tab == ct.idx;
        const bool hovered = ImGui::IsMouseHoveringRect(p, p + sz);
        ImGui::InvisibleButton(("##ctab_" + std::to_string(ct.idx)).c_str(), sz);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (active) {
            dl->AddRectFilled(p, p + sz, c32(k.brand), ui_px(4.0f));
            dl->AddRect(p, p + sz, c32(k.brand_hov), ui_px(4.0f), 0, ui_px(1.0f));
        }
        const ImVec4 text_color = active || hovered ? k.text : k.muted;
        dl->AddText(p + ImVec2(ui_px(8.0f), ui_px(3.0f)), c32(text_color), ct.label);
        if (ImGui::IsItemClicked()) {
            st.discover_sub_tab = ct.idx;
            st.browse_category = ct.category;
            const int browse_facets[] = {0, 7, 9, 8, 10};
            if (ct.category >= 0 && ct.category < 5 && browse_facets[ct.category] >= 0)
                st.mod_facet = browse_facets[ct.category];
            st.browse_page = 0;
            st.browse_initial_request_sent = false;
            if (!st.mod_fetching && !st.mod_installing) launch_mod_search(st);
        }
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── Featured Modpacks ─────────────────────────────────────────────────
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted("Featured Modpacks");
    ImGui::PopFont();
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - ui_px(84.0f));
    if (ghost_button("View all", ImVec2(ui_px(74.0f), ui_px(28.0f)))) {
        st.discover_sub_tab = 1;
        st.browse_category = 1;
        st.mod_facet = 7;
        st.browse_page = 0;
        if (!st.mod_fetching && !st.mod_installing) {
            st.browse_initial_request_sent = true;
            launch_mod_search(st);
        } else {
            st.browse_initial_request_sent = false;
        }
    }
    const float featured_gap = ui_px(10.0f);
    std::vector<mods::SearchResult> home_packs;
    { std::lock_guard<std::mutex> lock(st.mod_mu); home_packs = st.home_packs; }
    int pack_count = std::min(6, static_cast<int>(home_packs.size()));
    const float fw = ImGui::GetContentRegionAvail().x;
    // Four cards need the full desktop content width. At 1280px window width
    // the sidebar and gutters leave less than a true four-column canvas; use
    // three cards there so the last card never clips against the right edge.
    const int fc = fw >= ui_px(1180.0f) ? 4 : fw >= ui_px(640.0f) ? 3 : 2;
    const float card_w = std::max(ui_px(120.0f), (fw - featured_gap * (fc - 1)) / fc);
    const float card_h = ui_px(178.0f);
    const float artwork_h = ui_px(92.0f);
    int frows = std::max(1, (pack_count + fc - 1) / fc);
    ImGui::BeginChild("##featured", ImVec2(0, frows * (card_h + featured_gap)));
    for (int i = 0; i < pack_count; ++i) {
        if (i && i % fc) ImGui::SameLine(0, featured_gap);
        ImGui::BeginChild((std::string("##fc") + std::to_string(i)).c_str(), ImVec2(card_w, card_h), false);
        const auto& pack = home_packs[i];
        std::string stats = format_download_count(pack.downloads) + " downloads";
        const char* provider = pack.source == "curseforge" ? "CurseForge" : "Modrinth";
        discover_card(st, pack.title.c_str(), provider, stats.c_str(), i,
                      false, pack.icon_url.empty() ? nullptr : pack.icon_url.c_str(),
                      &pack, i);
        ImGui::EndChild();
    }
    if (home_packs.empty()) {
        std::string he;
        { std::lock_guard<std::mutex> lock(st.mod_mu); he = st.home_error; }
        if (st.home_fetching) {
            // Skeleton loading for featured modpacks
            for (int i = 0; i < 4; ++i) {
                if (i && i % fc) ImGui::SameLine(0, featured_gap);
                ImGui::BeginChild(("##skel_fc" + std::to_string(i)).c_str(), ImVec2(card_w, card_h), false);
                ImVec2 skel_pos = ImGui::GetCursorScreenPos();
                draw_skeleton_rect(skel_pos, ImVec2(card_w, artwork_h), ui_px(10.0f));
                draw_skeleton_text(ImVec2(skel_pos.x + ui_px(12.0f), skel_pos.y + artwork_h + ui_px(12.0f)), card_w * 0.6f, ui_px(16.0f));
                draw_skeleton_text(ImVec2(skel_pos.x + ui_px(12.0f), skel_pos.y + artwork_h + ui_px(36.0f)), card_w * 0.4f, ui_px(12.0f));
                ImGui::Dummy(ImVec2(card_w, card_h));
                ImGui::EndChild();
            }
        } else if (!he.empty()) {
            ImGui::TextColored(k.red, "%s", he.c_str());
        } else {
            illustrated_empty_state(IconId::Cube, "No modpacks found",
                                    "Try adjusting your search or filters to find content.");
        }
    }
    ImGui::EndChild();
    ImGui::Spacing();

    // ── Trending Mods ─────────────────────────────────────────────────────
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted("Trending Mods");
    ImGui::PopFont();
    ImGui::SameLine(0, ui_px(10));
    ImGui::TextColored(k.muted, "Live catalog");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - ui_px(84.0f));
    if (ghost_button("View all", ImVec2(ui_px(74.0f), ui_px(28.0f)))) {
        st.discover_sub_tab = 2;
        st.browse_category = 0;
        st.mod_facet = 0;
        st.browse_page = 0;
        if (!st.mod_fetching && !st.mod_installing) {
            st.browse_initial_request_sent = true;
            launch_mod_search(st);
        } else {
            st.browse_initial_request_sent = false;
        }
    }
    ImGui::Spacing();
    std::vector<mods::SearchResult> home_mods;
    { std::lock_guard<std::mutex> lock(st.mod_mu); home_mods = st.home_mods; }
    int mod_count = std::min(6, static_cast<int>(home_mods.size()));
    const float tw = ImGui::GetContentRegionAvail().x;
    const int tc = tw >= ui_px(560.0f) ? 2 : 1;
    const float tw2 = std::max(ui_px(220.0f), (tw - featured_gap * (tc - 1)) / tc);
    for (int i = 0; i < mod_count; ++i) {
        if (i && i % tc) ImGui::SameLine(0, featured_gap);
        if (draw_catalog_compact_row(st, home_mods[i], ("##tm" + std::to_string(i)).c_str(),
                                     ImVec2(tw2, ui_px(82.0f)), true))
            open_project_detail(st, home_mods[i]);
    }
    if (home_mods.empty()) {
        std::string he;
        { std::lock_guard<std::mutex> lock(st.mod_mu); he = st.home_error; }
        if (st.home_fetching) {
            // Skeleton loading for trending mods
            for (int i = 0; i < 4; ++i) {
                if (i && i % tc) ImGui::SameLine(0, featured_gap);
                ImVec2 skel_pos = ImGui::GetCursorScreenPos();
                draw_skeleton_rect(skel_pos, ImVec2(tw2, ui_px(82.0f)), ui_px(10.0f));
                ImGui::Dummy(ImVec2(tw2, ui_px(82.0f)));
            }
        } else {
            ImGui::TextColored(he.empty() ? k.muted : k.red, "%s",
                               he.empty() ? "No mod data available." : he.c_str());
        }
    }
}

// ---------------------------------------------------------------------------
// Play / version tab
// ---------------------------------------------------------------------------
void draw_version_list(UiState& st) {
    card_begin("##vcard", ImVec2(340, -1));
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted("Versions");
    ImGui::PopFont();
    ImGui::PushFont(f_small);
    ImGui::SameLine();
    ImGui::TextDisabled("(%d)", static_cast<int>(st.versions.size()));
    ImGui::PopFont();
    ImGui::SetNextItemWidth(-1);
    input_text("##search", &st.search);
    ImGui::Spacing();

    ImGui::BeginChild("##vscroll");
    std::vector<model::ManifestEntry> snap;
    {
        std::lock_guard<std::mutex> lock(st.version_mu);
        snap = st.versions;
    }
    for (const auto& e : snap) {
        if (!st.search.empty() && e.id.find(st.search) == std::string::npos) continue;
        ImGui::PushID(e.id.c_str());
        const bool selected = st.selected == e.id;
        ImVec2 avail = ImGui::GetContentRegionAvail();
        const float rh = 42.0f;
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImVec2 p1(p0.x + avail.x, p0.y + rh);
        bool hovered = ImGui::IsMouseHoveringRect(p0, p1);
        bool clicked = hovered && ImGui::IsMouseClicked(0);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (selected) {
            dl->AddRectFilled(p0, p1, c32(k.sel), 8.0f);
        } else if (hovered) {
            dl->AddRectFilled(p0, p1, c32(k.hover), 8.0f);
        }
        ImGui::PushFont(f_mono);
        dl->AddText(ImVec2(p0.x + 12, p0.y + 11), c32(selected ? k.text : k.muted),
                    e.id.c_str());
        ImGui::PopFont();
        ImVec2 bt = ImGui::CalcTextSize(type_text(e.type));
        ImVec2 badge_pos(p1.x - bt.x - 26, p0.y + (rh - 20) * 0.5f);
        draw_badge_vert(dl, badge_pos, type_text(e.type), k.bg, type_color(e.type));
        if (clicked) {
            st.selected = e.id;
            st.selected_type = e.type;
        }
        ImGui::Dummy(ImVec2(avail.x, rh));
        ImGui::PopID();
    }
    ImGui::EndChild();
    card_end();
}

void draw_play_tab(UiState& st) {
    draw_discover_tab(st);
}

// ---------------------------------------------------------------------------
// Mods tab
// ---------------------------------------------------------------------------
const char* facet_labels[] = {"Mods", "Modpacks", "Resource Packs", "Shaders", "Datapacks",
                              "Optimization", "Weapons", "Armor", "Cosmetic", "Magic",
                              "Technology", "Adventure", "Storage", "Food", "Utility"};

void draw_project_detail_tab(UiState& st) {
    if (ghost_button("<  Back to Discover", ImVec2(ui_px(170.0f), ui_px(34.0f)))) {
        close_project_detail(st);
        return;
    }
    ImGui::Spacing();
    card_begin("##projectheader", ImVec2(-1, 0));
    bool project_compatible = true;
    std::string compatibility_label;
    const bool is_modpack = st.project_detail.type == mods::ProjectType::Modpack;
    mods::CompatibleRelease selected_release;
    if (st.project_loading) {
        project_compatible = false;
        compatibility_label = "Loading compatible releases...";
    } else if (is_modpack) {
        std::string preferred_loader = st.mod_loader;
        std::string preferred_version = st.mod_version.empty() ? st.selected : st.mod_version;
        if (!st.active_instance_dir.empty()) {
            preferred_loader = st.selected_instance.loader;
            preferred_version = st.selected_instance.minecraft_version;
        }
        if (preferred_loader.empty() || preferred_loader == "auto")
            preferred_loader = st.cfg->loader;
        mods::ModInfo compatibility_info;
        {
            std::lock_guard<std::mutex> lock(st.project_mu);
            compatibility_info = st.project_info;
        }
        project_compatible = mods::select_compatible_release(
            compatibility_info, preferred_loader, preferred_version, selected_release, nullptr);
        compatibility_label = project_compatible
                                  ? "Compatible release: " + selected_release.game_version +
                                        " / " + selected_release.loader
                                  : "No downloadable release matches this project";
    } else if (!st.project_loading && !st.active_instance_dir.empty()) {
        mods::ModInfo compatibility_info;
        {
            std::lock_guard<std::mutex> lock(st.project_mu);
            compatibility_info = st.project_info;
        }
        std::string loader = st.selected_instance.loader == "auto" ? st.cfg->loader : st.selected_instance.loader;
        if (loader == "auto" || loader.empty()) loader.clear();
        const std::string selected_file = mods::pick_file(compatibility_info, loader,
                                                          st.selected_instance.minecraft_version);
        project_compatible = !selected_file.empty();
        compatibility_label = project_compatible
                                  ? "Compatible release available for " + st.selected_instance.minecraft_version
                                  : "No compatible release for " + st.selected_instance.minecraft_version +
                                        (loader.empty() ? "" : " / " + loader);
    }
    const char* provider_label = st.project_detail.source == "curseforge" ? "CURSEFORGE" : "MODRINTH";
    // Provider icons are square artwork. Stretching them into a wide banner
    // was the main reason this view looked blurry and unfinished. Keep the
    // artwork at an honest card size and use a crisp native-drawn hero behind
    // it instead.
    const ImVec2 hero = ImGui::GetCursorScreenPos();
    const float hero_width = ImGui::GetContentRegionAvail().x;
    const float hero_height = ui_px(112.0f);
    ImDrawList* hero_draw = ImGui::GetWindowDrawList();
    hero_draw->AddRectFilled(hero, hero + ImVec2(hero_width, hero_height), c32(k.surface2), ui_px(10.0f));
    // The reference project pages use a cinematic Minecraft banner. Provider
    // icons remain square and crisp, while the bundled banner supplies the
    // wide visual language when a provider has not returned gallery artwork.
    const std::wstring hero_art = st.exe_dir + L"\\branding\\ai\\modpack-banner-ai.png";
    std::error_code hero_error;
    if (std::filesystem::exists(hero_art, hero_error) && !hero_error) {
        draw_local_image(st, hero_art, hero, ImVec2(hero_width, hero_height),
                         c32(k.surface2), ImageFit::Cover);
        hero_draw->AddRectFilledMultiColor(
            hero, hero + ImVec2(hero_width, hero_height),
            c32(ImVec4(k.sidebar.x, k.sidebar.y, k.sidebar.z, 0.82f)),
            c32(ImVec4(k.sidebar.x, k.sidebar.y, k.sidebar.z, 0.38f)),
            c32(ImVec4(k.sidebar.x, k.sidebar.y, k.sidebar.z, 0.58f)),
            c32(ImVec4(k.sidebar.x, k.sidebar.y, k.sidebar.z, 0.90f)));
    }
    hero_draw->AddRect(hero, hero + ImVec2(hero_width, hero_height), c32(k.border), ui_px(10.0f));
    hero_draw->AddRectFilled(hero, hero + ImVec2(ui_px(5.0f), hero_height), c32(k.brand), ui_px(3.0f));
    const float icon_size = std::min(ui_px(76.0f), std::max(ui_px(52.0f), hero_height - ui_px(28.0f)));
    const ImVec2 project_icon_pos =
        hero + ImVec2(ui_px(22.0f), (hero_height - icon_size) * 0.5f);
    if (st.fixture_mode && st.project_detail.icon_url.empty()) {
        draw_local_image(st, st.exe_dir + L"\\branding\\amalgam-cover-portal.png",
                         project_icon_pos, ImVec2(icon_size, icon_size), c32(k.brand_dk),
                         ImageFit::Cover);
    } else {
        draw_project_image(st, st.project_detail.icon_url, project_icon_pos,
                           ImVec2(icon_size, icon_size), c32(k.brand_dk));
    }
    const float text_left = hero.x + icon_size + ui_px(40.0f);
    const float text_right = hero.x + hero_width - ui_px(22.0f);
    ImGui::SetCursorScreenPos(ImVec2(text_left, hero.y + ui_px(13.0f)));
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted(st.project_detail.title.empty() ? st.project_detail.slug.c_str()
                                                            : st.project_detail.title.c_str());
    ImGui::PopFont();
    draw_badge(hero_draw, ImVec2(text_left, hero.y + ui_px(45.0f)), provider_label,
               st.project_detail.source == "curseforge" ? k.orange : k.green, k.surface);
    const std::string project_kind = is_modpack ? "Modpack" : "Minecraft content";
    ImGui::SetCursorScreenPos(ImVec2(text_left + ui_px(108.0f), hero.y + ui_px(47.0f)));
    ImGui::TextColored(k.muted, "%s  |  %s downloads", project_kind.c_str(),
                       format_download_count(st.project_detail.downloads).c_str());
    const std::string header_summary = compact_provider_text(st.project_detail.description, 360);
    if (!header_summary.empty()) {
        ImGui::SetCursorScreenPos(ImVec2(text_left, hero.y + ui_px(72.0f)));
        ImGui::PushTextWrapPos(text_right);
        ImGui::TextColored(k.muted, "%s", header_summary.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::SetCursorScreenPos(ImVec2(hero.x, hero.y + hero_height + ui_px(12.0f)));
    if (st.active_instance_dir.empty() && !is_modpack) {
        ImGui::TextColored(k.yellow, "Select an existing profile before installing this project.");
        ImGui::TextColored(k.muted, "Amalgam will not silently create or modify a profile for you.");
        if (primary_button("Open Home / Select profile", ImVec2(ui_px(230.0f), ui_px(40.0f)))) {
            st.sidebar_item = 0;
            st.active_tab = 0;
        }
    } else if (is_modpack) {
        ImGui::TextColored(k.muted,
                           "Creates a new isolated profile with the creator's version, loader, and content.");
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, project_compatible ? 1.0f : 0.45f);
        bool install_modpack_clicked = primary_button(st.mod_installing ? "Installing..." : "Install as new profile", ImVec2(ui_px(210.0f), ui_px(34.0f)));
        ImGui::PopStyleVar();
        if (install_modpack_clicked && project_compatible && !st.mod_installing) {
            instances::Instance empty_target;
            empty_target.minecraft_version = selected_release.game_version;
            empty_target.loader = selected_release.loader;
            request_project_install(st, st.project_detail, empty_target, true);
        }
    } else {
        ImGui::TextColored(k.muted, "Target profile: %s", net::to_utf8(st.active_instance_dir).c_str());
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, project_compatible ? 1.0f : 0.45f);
        bool install_mod_clicked = primary_button(st.mod_installing ? "Installing..." : "Install into profile", ImVec2(ui_px(190.0f), ui_px(34.0f)));
        ImGui::PopStyleVar();
        if (install_mod_clicked && project_compatible && !st.mod_installing) {
            instances::Instance target = st.selected_instance;
            if (target.directory.empty()) target.directory = st.active_instance_dir;
            request_project_install(st, st.project_detail, target, false);
        }
    }
    ImGui::TextColored(k.muted, "%s", st.project_detail.slug.c_str());
    if (!compatibility_label.empty())
        ImGui::TextColored(project_compatible ? k.green : k.red, "%s", compatibility_label.c_str());
    ImGui::TextColored(k.muted, "%s  |  Compatible content from Modrinth + CurseForge",
                       is_modpack ? "Modpack" : st.project_detail.source.c_str());
    mods::ModInfo header_info;
    {
        std::lock_guard<std::mutex> lock(st.project_mu);
        header_info = st.project_info;
    }
    auto open_external_url = [&](const std::string& url) {
        std::string url_error;
        if (!net::validate_url(net::to_wide(url), &url_error)) {
            set_mod_status(st, "Blocked external URL: " + url_error);
            return;
        }
        ShellExecuteA(st.hwnd, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    };
    if (!header_info.website_url.empty() && ghost_button("Open website", ImVec2(ui_px(130.0f), ui_px(30.0f))))
        open_external_url(header_info.website_url);
    if (!header_info.source_url.empty() && header_info.source_url != header_info.website_url) {
        ImGui::SameLine();
        if (ghost_button("Open source", ImVec2(ui_px(120.0f), ui_px(30.0f))))
            open_external_url(header_info.source_url);
    }
    if (!header_info.issues_url.empty()) {
        ImGui::SameLine();
        if (ghost_button("Open issues", ImVec2(ui_px(110.0f), ui_px(30.0f))))
            open_external_url(header_info.issues_url);
    }
    if (!is_modpack) {
        ensure_instance_list(st);
        std::vector<std::string> profile_labels;
        profile_labels.push_back("Select an existing profile...");
        int target_index = 0;
        for (size_t i = 0; i < st.instance_list.size(); ++i) {
            const auto& instance = st.instance_list[i];
            profile_labels.push_back((instance.name.empty() ? instance.id : instance.name) + "  |  " +
                                     instance.minecraft_version + " " + instance.loader);
            if (!st.active_instance_dir.empty() && instance.directory == st.active_instance_dir)
                target_index = static_cast<int>(i + 1);
        }
        std::vector<const char*> profile_items;
        for (const auto& label : profile_labels) profile_items.push_back(label.c_str());
        ImGui::TextUnformatted("Target profile");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(auto_item_width(360.0f, 180.0f));
        if (ImGui::Combo("##projecttarget", &target_index, profile_items.data(),
                         static_cast<int>(profile_items.size())) && target_index > 0) {
            st.selected_instance = st.instance_list[static_cast<size_t>(target_index - 1)];
            st.active_instance_dir = st.selected_instance.directory;
            st.selected = st.selected_instance.minecraft_version;
            st.mod_loader = st.selected_instance.loader == "auto" ? "" : st.selected_instance.loader;
            st.mod_version = st.selected_instance.minecraft_version;
        }
    }
    card_end();
    ImGui::Spacing();
    const char* tabs[] = {"Info", "Content", "Changelog", "Versions"};
    for (int i = 0; i < 4; ++i) {
        if (i) ImGui::SameLine(0, ui_px(12));
        bool selected = st.project_detail_tab == i;
        const ImVec2 tab_text = ImGui::CalcTextSize(tabs[i]);
        const ImVec2 tab_size(tab_text.x + ui_px(18.0f), ui_px(32.0f));
        const ImVec2 tab_pos = ImGui::GetCursorScreenPos();
        ImGui::PushStyleColor(ImGuiCol_Text, selected ? k.text : k.muted);
        if (ImGui::Selectable((std::string("##project_tab_") + std::to_string(i)).c_str(), selected,
                             ImGuiSelectableFlags_None, tab_size))
            st.project_detail_tab = i;
        ImGui::PopStyleColor();
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(tab_pos.x, tab_pos.y + tab_size.y - ui_px(2.0f)),
            ImVec2(tab_pos.x + tab_size.x, tab_pos.y + tab_size.y),
            c32(selected ? k.brand : k.border), ui_px(1.0f));
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(tab_pos.x + ui_px(9.0f), tab_pos.y + ui_px(7.0f)),
            c32(selected ? k.text : k.muted), tabs[i]);
    }
    ImGui::Separator();
    ImGui::Spacing();
    card_begin("##projectdetail", ImVec2(-1, 0));
    ui_model::ScreenPhase project_phase = ui_model::ScreenPhase::Idle;
    std::string project_screen_error;
    bool project_retryable = false;
    {
        std::lock_guard<std::mutex> lock(st.project_mu);
        project_phase = st.project_screen.phase;
        project_screen_error = st.project_screen.error;
        project_retryable = st.project_screen.retryable;
    }
    if (project_phase == ui_model::ScreenPhase::Loading || st.project_loading) {
        ImGui::TextColored(k.yellow, "Loading project versions...");
        ImGui::TextColored(k.muted, "Provider details are loading in the background.");
    } else if (project_phase == ui_model::ScreenPhase::Error && !project_screen_error.empty()) {
        ImGui::TextColored(k.red, "Project details could not be loaded");
        ImGui::TextWrapped("%s", project_screen_error.c_str());
        ImGui::Spacing();
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("Info still available");
        ImGui::PopFont();
        ImGui::TextWrapped("%s", st.project_detail.description.empty()
                                       ? "The catalog entry is available, but the provider did not return its full release data yet."
                                       : st.project_detail.description.c_str());
        ImGui::TextColored(k.muted, "%s | %s downloads",
                           st.project_detail.source == "curseforge" ? "CurseForge" : "Modrinth",
                           format_download_count(st.project_detail.downloads).c_str());
        if (project_retryable && ghost_button("Retry project details", ImVec2(ui_px(150.0f), ui_px(30.0f))))
            open_project_detail(st, st.project_detail);
    } else {
        std::string project_error;
        {
            std::lock_guard<std::mutex> lock(st.project_mu);
            project_error = st.project_error;
        }
        if (!project_error.empty()) {
            ImGui::TextColored(k.red, "%s", project_error.c_str());
            card_end();
            return;
        }
        mods::ModInfo info;
        {
            std::lock_guard<std::mutex> lock(st.project_mu);
            info = st.project_info;
        }
        if (st.project_detail_tab == 0) {
            std::string translated_text;
            std::string translation_error;
            bool show_original = false;
            {
                std::lock_guard<std::mutex> lock(st.project_translation_mu);
                translated_text = st.project_translation_text;
                translation_error = st.project_translation_error;
                show_original = st.project_show_original_text;
            }
            const std::string original_text = project_source_text(st.project_detail, info);
            const bool showing_translation = !translated_text.empty() && !show_original;
            ImGui::PushFont(f_h2);
            ImGui::TextUnformatted(showing_translation ? "Overview — translated" : "Overview");
            ImGui::PopFont();
            ImGui::TextColored(k.muted, showing_translation
                                             ? "AI translation of public project text. Verify important details with the creator."
                                             : "Project description supplied by the provider.");
            if (st.project_translation_working) {
                ImGui::SameLine();
                ImGui::TextColored(k.yellow, "Translating to %s...", st.cfg->translation_target_language.c_str());
            }
            ImGui::Spacing();
            if (showing_translation) {
                ImGui::TextWrapped("%s", translated_text.c_str());
                if (ghost_button("Show original", ImVec2(ui_px(130.0f), ui_px(30.0f)))) {
                    std::lock_guard<std::mutex> lock(st.project_translation_mu);
                    st.project_show_original_text = true;
                }
            } else {
                ImGui::TextWrapped("%s", original_text.empty()
                                           ? "The provider did not include a full project description."
                                           : original_text.c_str());
                ImGui::Spacing();
                if (!st.project_translation_working &&
                    ghost_button("Translate description", ImVec2(ui_px(178.0f), ui_px(30.0f)))) {
                    request_project_translation(st, st.project_detail, info, true);
                }
                if (!translated_text.empty()) {
                    ImGui::SameLine();
                    if (ghost_button("Show translation", ImVec2(ui_px(152.0f), ui_px(30.0f)))) {
                        std::lock_guard<std::mutex> lock(st.project_translation_mu);
                        st.project_show_original_text = false;
                    }
                }
            }
            if (!translation_error.empty())
                ImGui::TextColored(k.yellow, "%s", translation_error.c_str());
            ImGui::Spacing();
            std::string categories;
            for (const auto& category : info.categories.empty() ? st.project_detail.categories : info.categories) {
                if (!categories.empty()) categories += ", ";
                categories += category;
            }
            ImGui::TextColored(k.muted, "Categories: %s", categories.empty() ? "general" : categories.c_str());
            ImGui::TextColored(k.muted, "Files: %d  |  Loaders: %s",
                               static_cast<int>(info.files.size()),
                               info.loaders.empty() ? "provider-compatible" : info.loaders.front().c_str());
            if (!info.author.empty())
                ImGui::TextColored(k.muted, "Creator: %s", info.author.c_str());
            if (!info.license.empty())
                ImGui::TextColored(k.muted, "License: %s", info.license.c_str());
            if (!info.date_updated.empty())
                ImGui::TextColored(k.muted, "Last updated: %s", info.date_updated.c_str());
            if (!info.gallery_urls.empty()) {
                ImGui::Spacing();
                ImGui::PushFont(f_h2);
                ImGui::TextUnformatted("Gallery");
                ImGui::PopFont();
                ImGui::TextColored(k.muted, "Images supplied by the project creator. Select one to view it full size.");
                ImGui::Spacing();
                const int gallery_count = std::min(3, static_cast<int>(info.gallery_urls.size()));
                const float gallery_gap = ui_px(8.0f);
                const float gallery_width = (ImGui::GetContentRegionAvail().x -
                                             gallery_gap * static_cast<float>(gallery_count - 1)) /
                                            static_cast<float>(gallery_count);
                const ImVec2 gallery_size(std::max(ui_px(120.0f), gallery_width), ui_px(132.0f));
                for (int i = 0; i < gallery_count; ++i) {
                    if (i) ImGui::SameLine(0, gallery_gap);
                    const ImVec2 image_pos = ImGui::GetCursorScreenPos();
                    draw_project_image(st, info.gallery_urls[static_cast<size_t>(i)], image_pos,
                                       gallery_size, c32(k.surface2));
                    const std::string image_id = "##project_gallery_" + std::to_string(i);
                    ImGui::InvisibleButton(image_id.c_str(), gallery_size);
                    if (ImGui::IsItemHovered())
                        ImGui::GetWindowDrawList()->AddRect(image_pos, image_pos + gallery_size,
                                                            c32(k.brand_hov), ui_px(8.0f), 0,
                                                            ui_px(1.5f));
                    if (ImGui::IsItemClicked())
                        open_external_url(info.gallery_urls[static_cast<size_t>(i)]);
                }
            }
        } else if (st.project_detail_tab == 1 || st.project_detail_tab == 3) {
            ImGui::PushFont(f_h2);
            ImGui::TextUnformatted(st.project_detail_tab == 1 ? "Content" : "Versions");
            ImGui::PopFont();
            ImGui::TextColored(k.muted, st.project_detail_tab == 1
                                         ? "Compatible published files and dependency information for this project."
                                         : "All provider releases returned for this project.");
            ImGui::Spacing();
            int shown = 0;
            for (const auto& file : info.files) {
                if (st.project_detail_tab == 1) {
                    std::string loader = st.selected_instance.loader == "auto"
                                             ? st.cfg->loader
                                             : st.selected_instance.loader;
                    if (loader == "auto") loader.clear();
                    const bool loader_ok = loader.empty() || file.loaders.empty() ||
                                           std::find(file.loaders.begin(), file.loaders.end(), loader) !=
                                               file.loaders.end();
                    const bool version_ok = st.selected_instance.minecraft_version.empty() ||
                                            file.game_versions.empty() ||
                                            std::find(file.game_versions.begin(), file.game_versions.end(),
                                                      st.selected_instance.minecraft_version) !=
                                                file.game_versions.end();
                    if (!loader_ok || !version_ok) continue;
                }
                if (shown++ >= 20) break;
                const bool selected_file = st.project_file_selected == shown;
                const float file_card_height = selected_file
                                                    ? (file.dependencies.empty() ? ui_px(84.0f) : ui_px(108.0f))
                                                    : ui_px(58.0f);
                card_begin((std::string("##projectfile") + std::to_string(shown)).c_str(),
                           ImVec2(-1, file_card_height));
                if (selected_file)
                    ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                                        c32(k.brand_hov), 8.0f, 0, 1.5f);
                ImGui::PushFont(f_bold);
                ImGui::TextUnformatted(file.filename.c_str());
                ImGui::PopFont();
                std::string versions;
                for (const auto& version : file.game_versions) {
                    if (!versions.empty()) versions += ", ";
                    versions += version;
                    if (versions.size() > 70) break;
                }
                ImGui::TextColored(k.muted, "%s  |  %s", versions.c_str(),
                                   file.loaders.empty() ? "all loaders" : file.loaders.front().c_str());
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - 112);
                const std::string file_button = std::string(selected_file ? "Selected" : "Inspect") +
                                                "##file" + std::to_string(shown);
                 if (ghost_button(file_button.c_str(), ImVec2(ui_px(92.0f), ui_px(28.0f)))) {
                    st.project_file_selected = shown;
                }
                if (selected_file) {
                    ImGui::TextColored(k.muted, "%s  |  %.1f MB  |  %s",
                                       file.date_published.empty() ? "Provider release" : file.date_published.c_str(),
                                       static_cast<double>(file.size) / (1024.0 * 1024.0),
                                       file.version_name.empty() ? file.version_number.c_str() : file.version_name.c_str());
                    if (!file.dependencies.empty()) {
                        std::string dependencies;
                        for (const auto& dependency : file.dependencies) {
                            if (!dependencies.empty()) dependencies += ", ";
                            dependencies += dependency.required ? "required " : "optional ";
                            dependencies += dependency.project_id;
                        }
                        ImGui::TextColored(k.muted, "Dependencies: %s", dependencies.c_str());
                    }
                }
                card_end();
            }
            if (shown == 0) ImGui::TextColored(k.muted, "No file versions were returned by the provider.");
        } else if (st.project_detail_tab == 2) {
            ImGui::PushFont(f_h2);
            ImGui::TextUnformatted("Changelog");
            ImGui::PopFont();
            int shown = 0;
            for (const auto& file : info.files) {
                if (file.changelog.empty() || shown++ >= 8) continue;
                card_begin((std::string("##changelog") + std::to_string(shown)).c_str(), ImVec2(-1, 0));
                ImGui::PushFont(f_bold);
                ImGui::TextUnformatted(file.version_name.empty() ? file.version_number.c_str()
                                                                  : file.version_name.c_str());
                ImGui::PopFont();
                ImGui::TextColored(k.muted, "%s", file.date_published.empty() ? "Provider release" : file.date_published.c_str());
                ImGui::TextWrapped("%s", file.changelog.c_str());
                card_end();
            }
            if (shown == 0)
                ImGui::TextColored(k.muted, "This provider did not publish changelog text for the available files.");
        }
    }
    card_end();

    if (st.install_confirm_open) {
        set_next_adaptive_window(520.0f, 0.0f, 360.0f, 0.0f);
        bool open = true;
        if (ImGui::BeginPopupModal("Confirm content install", &open,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            if (!open) {
                st.install_confirm_open = false;
                ImGui::CloseCurrentPopup();
            } else {
                ImGui::PushFont(f_h2);
                ImGui::TextUnformatted(st.install_confirm_modpack ? "Install modpack" : "Install project");
                ImGui::PopFont();
                ImGui::TextWrapped("%s  |  %s", st.install_confirm_project.title.c_str(),
                                   st.install_confirm_project.source == "curseforge" ? "CurseForge" : "Modrinth");
                if (st.install_confirm_modpack) {
                    ImGui::TextColored(k.muted, "A new isolated profile will be created from the published manifest.");
                } else {
                    const auto& target = st.install_confirm_target;
                    ImGui::TextColored(k.muted, "Target: %s  |  %s  |  %s",
                                       target.name.empty() ? target.id.c_str() : target.name.c_str(),
                                       target.minecraft_version.c_str(),
                                       target.loader.empty() ? "auto loader" : target.loader.c_str());
                }
                const int required = static_cast<int>(std::count_if(
                    st.install_confirm_dependencies.begin(), st.install_confirm_dependencies.end(),
                    [](const mods::DepInfo& dependency) { return dependency.required; }));
                const int optional = static_cast<int>(st.install_confirm_dependencies.size()) - required;
                ImGui::TextColored(k.muted, "Dependencies: %d required, %d optional", required, optional);
                if (st.install_confirm_conflicts > 0)
                    ImGui::TextColored(k.yellow, "%d existing file(s) may be replaced.",
                                       st.install_confirm_conflicts);
                for (const auto& conflict : st.install_confirm_conflict_files)
                    ImGui::BulletText("%s", conflict.c_str());
                ImGui::PushStyleColor(ImGuiCol_Text, k.green);
                ImGui::TextWrapped("Safe install: files download and verify first; replaced files move to profile recovery.");
                ImGui::PopStyleColor();
                if (!st.install_confirm_dependencies.empty()) {
                    ImGui::Spacing();
                    ImGui::TextUnformatted("The installer will resolve these dependencies automatically:");
                    int shown = 0;
                    for (const auto& dependency : st.install_confirm_dependencies) {
                        if (shown++ >= 8) {
                            ImGui::TextColored(k.muted, "...and %d more", required + optional - 8);
                            break;
                        }
                        ImGui::BulletText("%s%s", dependency.required ? "required " : "optional ",
                                          dependency.project_id.c_str());
                    }
                }
                ImGui::Spacing();
                 bool confirm = primary_button(st.mod_installing ? "Installing..." : "Confirm install",
                                               ImVec2(ui_px(150.0f), ui_px(36.0f)));
                ImGui::SameLine();
                 bool cancel = ghost_button("Cancel", ImVec2(ui_px(100.0f), ui_px(36.0f)));
                if (cancel) {
                    st.install_confirm_open = false;
                    ImGui::CloseCurrentPopup();
                } else if (confirm && !st.mod_installing) {
                    const auto project = st.install_confirm_project;
                    const auto target = st.install_confirm_target;
                    st.install_confirm_open = false;
                    ImGui::CloseCurrentPopup();
                    if (st.install_confirm_modpack)
                        spawn_worker(st, std::thread(do_modpack_install, std::ref(st), project, target));
                    else
                        spawn_worker(st, std::thread(do_mod_install_target, std::ref(st), project.slug,
                                                     project.source, target));
                }
            }
            ImGui::EndPopup();
        }
    }
}

void draw_mods_tab(UiState& st) {
    if (st.project_detail_open) {
        draw_project_detail_tab(st);
        return;
    }

    const int browse_facets[] = {0, 7, 9, 8, 10};
    const bool curseforge_ready = mods::curseforge_available(provider_config::make(*st.cfg));
    const bool source_ready = st.browse_source != 2 || curseforge_ready;

    // --- Ctrl+K: focus search ---
    if (ImGui::IsKeyPressed(ImGuiKey_K) && ImGui::GetIO().KeyCtrl)
        st.browse_search_focused = true;

    // ── Back to Discover ──────────────────────────────────────────────────
    if (st.discover_sub_tab > 0) {
        const float arrow_w = ui_px(16.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, k.muted);
        if (ImGui::Selectable("##disc_back", false, ImGuiSelectableFlags_None,
                              ImVec2(arrow_w + ImGui::CalcTextSize("Discover").x + ui_px(12.0f), ui_px(22.0f))))
            st.discover_sub_tab = 0;
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 bp = ImGui::GetItemRectMin();
        dl->AddText(bp + ImVec2(0, ui_px(2.0f)), c32(k.muted), "<");
        dl->AddText(bp + ImVec2(arrow_w, 0), c32(k.text), "Discover");
        ImGui::Spacing();
    }

    // ── Content type tabs ─────────────────────────────────────────────────
    struct CTab { const char* label; int cat; int idx; };
    const CTab cts[] = {
        {"Modpacks", 1, 1}, {"Mods", 0, 2}, {"Shaders", 2, 3},
        {"Resource Packs", 3, 4}, {"Data Packs", 4, 5},
    };
    for (const auto& ct : cts) {
        ImGui::SameLine(0, ui_px(4.0f));
        bool active = st.discover_sub_tab == ct.idx;
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImVec2 sz = ImGui::CalcTextSize(ct.label) + ImVec2(ui_px(16.0f), ui_px(6.0f));
        if (active) ImGui::GetWindowDrawList()->AddRectFilled(p, p + sz, c32(k.brand), ui_px(2.0f));
        ImGui::InvisibleButton(("##ct_" + std::to_string(ct.idx)).c_str(), sz);
        if (active)
            ImGui::GetWindowDrawList()->AddText(p + ImVec2(ui_px(8.0f), ui_px(3.0f)), c32(k.text), ct.label);
        else
            ImGui::GetWindowDrawList()->AddText(p + ImVec2(ui_px(8.0f), ui_px(3.0f)), c32(k.muted), ct.label);
        if (ImGui::IsItemClicked() && !active) {
            st.discover_sub_tab = ct.idx;
            st.browse_category = ct.cat;
            if (ct.cat >= 0 && ct.cat < 5 && browse_facets[ct.cat] >= 0)
                st.mod_facet = browse_facets[ct.cat];
            st.browse_page = 0;
            if (!st.mod_fetching && !st.mod_installing) {
                st.browse_initial_request_sent = true;
                launch_mod_search(st);
            } else {
                st.browse_initial_request_sent = false;
            }
        }
    }
    ImGui::Spacing();

    // ── Search bar ────────────────────────────────────────────────────────
    const char* search_hints[] = {"Search...", "Search modpacks...", "Search mods...",
                                  "Search shaders...", "Search resource packs...", "Search data packs..."};
    const char* hint = (st.discover_sub_tab >= 0 && st.discover_sub_tab <= 5)
                       ? search_hints[st.discover_sub_tab] : search_hints[0];
    if (st.browse_search_focused) { ImGui::SetKeyboardFocusHere(); st.browse_search_focused = false; }
    ImGui::SetNextItemWidth(auto_item_width(500.0f, 200.0f, 400.0f));
    const bool query_changed = input_text_hint("##modquery", hint, &st.mod_query);
    ImGui::SameLine();
    if (!st.mod_query.empty()) {
        if (ghost_button("X", ImVec2(ui_px(24.0f), ui_px(26.0f)))) {
            st.mod_query.clear();
            st.browse_page = 0;
            if (!st.mod_fetching && !st.mod_installing) launch_mod_search(st);
        }
        ImGui::SameLine();
    }

    // ── Filter row ────────────────────────────────────────────────────────
    // Keep the controls in a single line on desktop, but let them flow into
    // a readable vertical stack on narrow windows instead of pushing the
    // source/sort controls outside the content region.
    const bool narrow_filters = ImGui::GetContentRegionAvail().x < ui_px(720.0f);
    auto filter_same_line = [&](float reserve) {
        if (!narrow_filters && ImGui::GetContentRegionAvail().x > ui_px(reserve))
            ImGui::SameLine();
    };
    bool filter_changed = false;
    const std::string version_loader = st.mod_loader.empty() ? "auto" : st.mod_loader;
    if (!st.mod_version.empty() && !version_catalog::supports(version_loader, st.mod_version)) {
        // Older configuration files could retain an arbitrary text filter.
        // Do not silently send an unsupported target back to a content provider.
        st.mod_version.clear();
        filter_changed = true;
    }
    ImGui::SetNextItemWidth(auto_item_width(142.0f, 100.0f));
    const char* version_preview = st.mod_version.empty() ? "All supported versions" : st.mod_version.c_str();
    if (ImGui::BeginCombo("##mcver", version_preview)) {
        if (ImGui::Selectable("All supported versions", st.mod_version.empty())) {
            st.mod_version.clear();
            filter_changed = true;
        }
        for (const std::string& version : available_profile_versions(st, version_loader)) {
            const bool selected = st.mod_version == version;
            if (ImGui::Selectable(version.c_str(), selected)) {
                st.mod_version = version;
                filter_changed = true;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    filter_same_line(520.0f);
    ImGui::SetNextItemWidth(auto_item_width(118.0f, 88.0f));
    const char* loaders[] = {"", "fabric", "forge", "neoforge", "quilt", "vanilla"};
    int li = 0;
    for (int i = 0; i < 6; ++i) if (st.mod_loader == loaders[i]) li = i;
    if (ImGui::Combo("##loader", &li, "Any Loader\0Fabric\0Forge\0NeoForge\0Quilt\0Vanilla\0", 6)) {
        st.mod_loader = loaders[li];
        if (!st.mod_version.empty() &&
            !version_catalog::supports(st.mod_loader.empty() ? "auto" : st.mod_loader, st.mod_version))
            st.mod_version.clear();
        st.browse_page = 0;
        filter_changed = true;
    }
    filter_same_line(380.0f);
    ImGui::SetNextItemWidth(auto_item_width(118.0f, 96.0f));
    if (ImGui::Combo("##source", &st.browse_source, "All Sources\0Modrinth\0CurseForge\0", 3))
        filter_changed = true;
    filter_same_line(240.0f);
    ImGui::SetNextItemWidth(auto_item_width(118.0f, 96.0f));
    if (ImGui::Combo("##sort", &st.browse_sort, "Relevance\0Popular\0Downloads\0Updated\0Newest\0", 5))
        filter_changed = true;
    filter_same_line(72.0f);
    // Grid/List toggle
    if (ghost_button(st.browse_grid_view ? "▦" : "☰", ImVec2(ui_px(28.0f), ui_px(26.0f))))
        st.browse_grid_view = !st.browse_grid_view;

    // Changing any selector intentionally queues a fresh catalog request on
    // the next frame. That avoids stale mixed-provider results and respects a
    // CurseForge-only selection that cannot currently be queried.
    if (filter_changed) {
        st.browse_page = 0;
        st.browse_initial_request_sent = false;
        st.browse_search_debounce = {};
    }

    // Debounced auto-search
    if (query_changed && source_ready && !st.mod_fetching && !st.mod_installing)
        st.browse_search_debounce = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    if (st.browse_search_debounce.time_since_epoch().count() > 0 &&
        std::chrono::steady_clock::now() >= st.browse_search_debounce &&
        source_ready && !st.mod_fetching && !st.mod_installing) {
        st.browse_search_debounce = {};
        st.browse_page = 0;
        launch_mod_search(st);
    }
    // Auto-load on first open
    if (!filter_changed && !st.browse_initial_request_sent && !st.mod_fetching && !st.mod_installing && source_ready) {
        st.browse_initial_request_sent = true;
        st.browse_page = 0;
        launch_mod_search(st);
    }
    if (st.browse_source == 2 && !curseforge_ready) {
        const auto api = provider_config::make(*st.cfg);
        ImGui::TextColored(k.yellow, "%s", mods::curseforge_proxy_configured(api)
            ? "Sign in to your Amalgam account to browse the shared CurseForge catalog."
            : "CurseForge requires Amalgam online services or a personal API key.");
    }
    ImGui::Spacing();

    // ── Loading / error states ────────────────────────────────────────────
    ui_model::ScreenPhase mod_phase = ui_model::ScreenPhase::Idle;
    std::string mod_screen_error;
    bool mod_screen_retryable = false;
    {
        std::lock_guard<std::mutex> lock(st.mod_mu);
        mod_phase = st.mod_screen.phase;
        mod_screen_error = st.mod_screen.error;
        mod_screen_retryable = st.mod_screen.retryable;
    }
    if (mod_phase == ui_model::ScreenPhase::Loading) {
        // Skeleton rows
        for (int s = 0; s < 5; ++s) {
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p, p + ImVec2(ui_px(40.0f), ui_px(40.0f)), c32(k.surface), ui_px(6.0f));
            dl->AddRectFilled(p + ImVec2(ui_px(52.0f), ui_px(4.0f)),
                              p + ImVec2(ui_px(260.0f), ui_px(12.0f)), c32(k.surface), ui_px(3.0f));
            dl->AddRectFilled(p + ImVec2(ui_px(52.0f), ui_px(22.0f)),
                              p + ImVec2(ui_px(180.0f), ui_px(8.0f)), c32(k.surface), ui_px(3.0f));
            ImGui::Dummy(ImVec2(0, ui_px(52.0f)));
        }
    } else if (mod_phase == ui_model::ScreenPhase::Error && !mod_screen_error.empty()) {
        card_begin("##moderror", ImVec2(-1, 60));
        ImGui::TextColored(k.red, "Could not load results: %s", mod_screen_error.c_str());
        if (mod_screen_retryable && ghost_button("Retry", ImVec2(ui_px(80), ui_px(26))) && !st.mod_fetching)
            launch_mod_search(st);
        card_end();
        ImGui::Spacing();
    }

    // ── Results ───────────────────────────────────────────────────────────
    std::vector<mods::SearchResult> snap;
    { std::lock_guard<std::mutex> lock(st.mod_mu); snap = st.mod_results; }
    if (st.browse_source != 0) {
        const char* wanted = st.browse_source == 1 ? "modrinth" : "curseforge";
        snap.erase(std::remove_if(snap.begin(), snap.end(), [wanted](const mods::SearchResult& r) {
            return r.source != wanted;
        }), snap.end());
    }
    if (st.browse_sort != 0) {
        std::stable_sort(snap.begin(), snap.end(), [&st](const mods::SearchResult& a, const mods::SearchResult& b) {
            if (st.browse_sort == 1) return a.downloads > b.downloads;
            if (st.browse_sort == 2) return a.downloads > b.downloads;
            if (st.browse_sort == 3) return a.date_modified > b.date_modified;
            return a.date_modified > b.date_modified;
        });
    }
    int modrinth_count = 0, curseforge_count = 0;
    {
        std::lock_guard<std::mutex> lock(st.mod_mu);
        modrinth_count = st.modrinth_result_count;
        curseforge_count = st.curseforge_result_count;
    }

    // Result count
    ImGui::TextColored(k.muted, "%d results", (int)snap.size());
    ImGui::SameLine();
    if (curseforge_count == 0 && !curseforge_ready) {
        const auto api = provider_config::make(*st.cfg);
        ImGui::TextColored(k.muted, mods::curseforge_proxy_configured(api)
            ? "  Modrinth %d  |  CurseForge sign-in required"
            : "  Modrinth %d  |  CurseForge unavailable", modrinth_count);
    }
    else
        ImGui::TextColored(k.muted, "  Modrinth %d  |  CurseForge %d", modrinth_count, curseforge_count);
    ImGui::Spacing();

    // Grid view for modpacks/shaders
    const bool use_grid = st.browse_grid_view && (st.browse_category == 1 || st.browse_category == 2 || st.browse_category == 3);
    if (use_grid) {
        const float gap = ui_px(10.0f);
        const float avail = ImGui::GetContentRegionAvail().x;
        const int cols = avail >= ui_px(1180.0f) ? 4 : avail >= ui_px(640.0f) ? 3 : 2;
        const float cw = std::max(ui_px(140.0f), (avail - gap * (cols - 1)) / cols);
        const float ch = ui_px(220.0f);
        static aml::ui::AnimFloat browse_hover[32];
        static bool browse_hover_init = false;
        if (!browse_hover_init) {
            for (auto& a : browse_hover) a.set(0.0f);
            browse_hover_init = true;
        }
        for (size_t i = 0; i < snap.size(); ++i) {
            if (i % cols) ImGui::SameLine(0, gap);
            const auto& r = snap[i];
            ImGui::PushID((int)i);
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImVec2 p1(p0.x + cw, p0.y + ch);
            bool hovered = ImGui::IsMouseHoveringRect(p0, p1);
            aml::ui::AnimFloat& bh = browse_hover[i % 32];
            bh.target(hovered ? 1.0f : 0.0f);
            const float bh_a = bh.update(ImGui::GetIO().DeltaTime);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p0, p1,
                c32(motion_lerp_color(k.surface, k.hover, bh_a)), ui_px(8.0f));
            dl->AddRect(p0, p1,
                c32(motion_lerp_color(k.border, k.brand, bh_a * 0.8f)),
                ui_px(8.0f), 0, ui_px(1.0f + bh_a));
            // Icon
            draw_catalog_project_art(st, r, p0 + ImVec2(ui_px(4.0f), ui_px(4.0f)),
                                     ImVec2(cw - ui_px(8.0f), ui_px(120.0f)), c32(k.surface2));
            // Text
            float tx = p0.x + ui_px(8.0f);
            float ty = p0.y + ui_px(128.0f);
            dl->AddText(ImVec2(tx, ty), c32(k.text),
                        elide_to_width(r.title.empty() ? r.slug : r.title, cw - ui_px(16.0f)).c_str());
            ty += ui_px(18.0f);
            dl->AddText(ImVec2(tx, ty), c32(k.muted),
                        elide_to_width(format_download_count(r.downloads) + " downloads", cw - ui_px(16.0f)).c_str());
            ty += ui_px(16.0f);
            // Loaders
            std::string loader_str;
            for (size_t li2 = 0; li2 < r.loaders.size() && li2 < 3; ++li2) {
                if (!loader_str.empty()) loader_str += " • ";
                loader_str += r.loaders[li2];
            }
            dl->AddText(ImVec2(tx, ty), c32(k.muted), elide_to_width(loader_str, cw - ui_px(16.0f)).c_str());
            // Source badge
            const char* src = r.source == "curseforge" ? "CF" : "MR";
            ImVec2 badge_p(p1.x - ui_px(30.0f), p0.y + ui_px(8.0f));
            dl->AddRectFilled(badge_p, badge_p + ImVec2(ui_px(22.0f), ui_px(16.0f)),
                              c32(r.source == "curseforge" ? k.orange : k.green), ui_px(3.0f));
            dl->AddText(badge_p + ImVec2(ui_px(4.0f), ui_px(1.0f)), c32(k.text), src);
            // Buttons
            float btn_y = p1.y - ui_px(34.0f);
            ImGui::SetCursorScreenPos(ImVec2(p0.x + ui_px(8.0f), btn_y));
            if (ghost_button("Details", ImVec2(ui_px(70.0f), ui_px(26.0f))))
                open_project_detail(st, r);
            ImGui::SetCursorScreenPos(ImVec2(p1.x - ui_px(78.0f), btn_y));
            if (r.type == mods::ProjectType::Modpack) {
                if (primary_button("Install", ImVec2(ui_px(70.0f), ui_px(26.0f))) && !st.mod_installing) {
                    st.selected_source = r.source;
                    spawn_worker(st, std::thread(do_modpack_install, std::ref(st), r, instances::Instance{}));
                }
            } else {
                if (primary_button("Install", ImVec2(ui_px(70.0f), ui_px(26.0f))) && !st.mod_installing) {
                    instances::Instance target;
                    if (existing_profile_target(st, target)) {
                        st.selected_source = r.source;
                        spawn_worker(st, std::thread(do_mod_install_target, std::ref(st), r.slug, r.source, target));
                    }
                }
            }
            ImGui::SetCursorScreenPos(p0);
            ImGui::Dummy(ImVec2(cw, ch));
            if (hovered && ImGui::IsMouseClicked(0)) open_project_detail(st, r);
            ImGui::PopID();
        }
    } else {
        // List view
        static aml::ui::AnimFloat list_hover[32];
        static bool list_hover_init = false;
        if (!list_hover_init) {
            for (auto& a : list_hover) a.set(0.0f);
            list_hover_init = true;
        }
        for (size_t i = 0; i < snap.size(); ++i) {
            const auto& r = snap[i];
            ImVec2 avail = ImGui::GetContentRegionAvail();
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            const float row_h = ui_px(72.0f);
            ImVec2 p1(p0.x + avail.x, p0.y + row_h);
            bool hovered = ImGui::IsMouseHoveringRect(p0, p1);
            aml::ui::AnimFloat& lh = list_hover[i % 32];
            lh.target(hovered ? 1.0f : 0.0f);
            const float lh_a = lh.update(ImGui::GetIO().DeltaTime);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            if (lh_a > 0.01f) dl->AddRectFilled(p0, p1,
                c32(ImVec4(k.hover.x, k.hover.y, k.hover.z, k.hover.w * lh_a)),
                ui_px(6.0f));

            const float pad = ui_px(8.0f);
            const float icon_sz = ui_px(48.0f);
            // Icon
            draw_catalog_project_art(st, r, ImVec2(p0.x + pad, p0.y + pad),
                                     ImVec2(icon_sz, icon_sz), c32(k.surface2));

            const float text_x = p0.x + pad + icon_sz + pad;
            const bool has_install = r.type == mods::ProjectType::Mod || r.type == mods::ProjectType::Modpack;
            const float btn_w = ui_px(70.0f);
            const float inst_w = ui_px(80.0f);
            const float btn_gap = ui_px(4.0f);
            const float right_edge = p1.x - pad;
            const float inst_x = has_install ? right_edge - inst_w : right_edge;
            const float det_x = has_install ? inst_x - btn_gap - btn_w : right_edge - btn_w;
            const float text_w = std::max(ui_px(100.0f), det_x - text_x - pad);

            // Title + source badge
            ImGui::PushFont(f_bold);
            dl->AddText(ImVec2(text_x, p0.y + pad), c32(k.text),
                        elide_to_width(r.title.empty() ? r.slug : r.title, text_w).c_str());
            ImGui::PopFont();
            // Source badge
            ImVec2 bp2(text_x + ImGui::CalcTextSize((r.title.empty() ? r.slug : r.title).c_str()).x + ui_px(8.0f),
                       p0.y + pad + ui_px(1.0f));
            const char* src_label = r.source == "curseforge" ? "CF" : "MR";
            ImVec2 badge_sz(ui_px(22.0f), ui_px(14.0f));
            dl->AddRectFilled(bp2, bp2 + badge_sz, c32(r.source == "curseforge" ? k.orange : k.green), ui_px(3.0f));
            dl->AddText(bp2 + ImVec2(ui_px(4.0f), 0), c32(k.text), src_label);

            // Author / metadata
            ImGui::PushFont(f_small);
            std::string meta = format_download_count(r.downloads) + " downloads";
            if (!r.date_modified.empty()) meta += " • Updated " + r.date_modified;
            dl->AddText(ImVec2(text_x, p0.y + pad + ui_px(20.0f)), c32(k.muted),
                        elide_to_width(meta, text_w).c_str());
            ImGui::PopFont();

            // Description
            ImGui::PushFont(f_small);
            dl->AddText(ImVec2(text_x, p0.y + pad + ui_px(36.0f)), c32(k.muted),
                        elide_to_width(r.description.empty() ? "No description" : r.description, text_w).c_str());
            ImGui::PopFont();

            // Loaders line
            ImGui::PushFont(f_small);
            std::string loaders_line;
            for (size_t li2 = 0; li2 < r.loaders.size() && li2 < 4; ++li2) {
                if (!loaders_line.empty()) loaders_line += " • ";
                loaders_line += r.loaders[li2];
            }
            if (!r.categories.empty()) {
                for (size_t ci = 0; ci < r.categories.size() && ci < 2; ++ci) {
                    if (!loaders_line.empty()) loaders_line += " • ";
                    loaders_line += r.categories[ci];
                }
            }
            dl->AddText(ImVec2(text_x, p0.y + pad + ui_px(50.0f)), c32(k.muted),
                        elide_to_width(loaders_line, text_w).c_str());
            ImGui::PopFont();

            // Buttons
            ImGui::SetCursorScreenPos(ImVec2(det_x, p0.y + (row_h - ui_px(26.0f)) * 0.5f));
            if (ghost_button(("Details##" + std::to_string(i)).c_str(), ImVec2(btn_w, ui_px(26.0f))))
                open_project_detail(st, r);
            if (has_install) {
                ImGui::SetCursorScreenPos(ImVec2(inst_x, p0.y + (row_h - ui_px(26.0f)) * 0.5f));
                if (primary_button(("Install##" + std::to_string(i)).c_str(), ImVec2(inst_w, ui_px(26.0f))) &&
                    !st.mod_installing) {
                    if (r.type == mods::ProjectType::Modpack) {
                        st.selected_source = r.source;
                        spawn_worker(st, std::thread(do_modpack_install, std::ref(st), r, instances::Instance{}));
                    } else {
                        instances::Instance target;
                        if (existing_profile_target(st, target)) {
                            st.selected_source = r.source;
                            spawn_worker(st, std::thread(do_mod_install_target, std::ref(st), r.slug, r.source, target));
                        }
                    }
                }
            }
            ImGui::SetCursorScreenPos(p0);
            ImGui::Dummy(ImVec2(avail.x, row_h));
            if (hovered && ImGui::IsMouseClicked(0)) open_project_detail(st, r);
        }
    }

    // ── Empty state ───────────────────────────────────────────────────────
    if (snap.empty() && !st.mod_fetching && mod_phase != ui_model::ScreenPhase::Loading) {
        ImGui::Spacing();
        ImGui::PushFont(f_h2);
        ImGui::TextColored(k.muted, "No results found");
        ImGui::PopFont();
        if (!st.mod_query.empty()) {
            ImGui::TextColored(k.muted, "No projects match \"%s\"", st.mod_query.c_str());
        } else {
            ImGui::TextColored(k.muted, "Try adjusting your filters or search query.");
        }
        ImGui::Spacing();
        if (ghost_button("Clear filters", ImVec2(ui_px(120.0f), ui_px(26.0f)))) {
            st.mod_query.clear();
            st.mod_loader.clear();
            st.mod_version.clear();
            st.browse_source = 0;
            st.browse_sort = 0;
            st.browse_page = 0;
            if (!st.mod_fetching) launch_mod_search(st);
        }
    }

    // ── Load more ─────────────────────────────────────────────────────────
    if (snap.size() >= 30 && !st.mod_fetching) {
        ImGui::Spacing();
        if (ghost_button("Load more results", ImVec2(ui_px(140.0f), ui_px(28.0f))) && !st.mod_installing) {
            ++st.browse_page;
            launch_mod_search(st);
        }
        ImGui::SameLine();
        ImGui::TextColored(k.muted, "Showing %d results", (int)snap.size());
    }
}

// ---------------------------------------------------------------------------
// Modpack AI tab
// ---------------------------------------------------------------------------
void draw_modpack_tab(UiState& st) {
    page_title("Modpack AI", "describe a modpack and the AI assembles it");

    card_begin("##packcard", ImVec2(-1, -1));
    const std::wstring generated_banner = st.exe_dir + L"\\modpack-banner.png";
    const std::wstring bundled_banner = st.exe_dir + L"\\branding\\ai\\modpack-banner-ai.png";
    std::error_code banner_error;
    const std::wstring banner_art =
        std::filesystem::exists(generated_banner, banner_error) && !banner_error
            ? generated_banner
            : bundled_banner;
    if (std::filesystem::exists(banner_art, banner_error) && !banner_error) {
        const ImVec2 banner_pos = ImGui::GetCursorScreenPos();
        const ImVec2 banner_size(ImGui::GetContentRegionAvail().x, ui_px(156.0f));
        draw_local_image(st, banner_art, banner_pos, banner_size, c32(k.surface2));
        ImGui::Dummy(banner_size);
        ImGui::Spacing();
    }
    ImGui::PushFont(f_small);
    ImGui::TextDisabled("needs an AI provider configured in Settings");
    ImGui::PopFont();
    ImGui::Spacing();
    ImGui::SetNextItemWidth(-1);
    input_text("##packprompt", &st.pack_prompt);
    ImGui::Spacing();
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, (st.pack_building || st.pack_prompt.empty()) ? 0.5f : 1.0f);
    if (primary_button("BUILD MODPACK", ImVec2(ui_px(200.0f), ui_px(38.0f))) && !st.pack_building &&
        !st.pack_prompt.empty()) {
        spawn_worker(st, std::thread(do_pack_build, std::ref(st)));
    }
    ImGui::PopStyleVar();
    if (st.pack_building) {
        ImGui::SameLine();
        ImGui::TextColored(k.yellow, "AI thinking...");
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    std::string pack_summary = pack_summary_snapshot(st);
    if (!pack_summary.empty()) {
        ImGui::PushFont(f_bold);
        ImGui::TextUnformatted("AI response");
        ImGui::PopFont();
        ImGui::TextWrapped("%s", pack_summary.c_str());
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
    }

    std::vector<mods::Match> plan;
    {
        std::lock_guard<std::mutex> lock(st.pack_mu);
        plan = st.pack_plan;
    }
    ImGui::PushFont(f_h2);
    ImGui::Text("Plan (%d mods)", static_cast<int>(plan.size()));
    ImGui::PopFont();
    if (plan.empty() && !st.pack_building) {
        ImGui::TextColored(k.muted, "Describe a modpack above and click BUILD MODPACK to get started.");
    }
    for (const auto& m : plan) {
        ImGui::BulletText("%s  [%s | %s]", m.title.c_str(),
                           m.loader.empty() ? "selected loader" : m.loader.c_str(),
                           m.source.c_str());
    }

    if (!plan.empty() && !st.pack_building) {
        ImGui::Spacing();
        if (primary_button("INSTALL ALL", ImVec2(ui_px(160.0f), ui_px(34.0f))) && !st.mod_installing) {
            instances::Instance target;
            if (existing_profile_target(st, target)) {
                set_mod_install_log(st, {});
                const std::wstring mods_dir = target.directory + L"\\mods";
                const std::string gv = target.minecraft_version;
                mods::ApiCfg cfg = provider_config::make(*st.cfg);
                const std::string target_loader = target.loader == "auto" ? "fabric" : target.loader;
                spawn_worker(st, std::thread([&st, cfg, plan, mods_dir, gv, target_loader]() {
                st.mod_installing = true;
                std::vector<std::string> install_log;
                for (const auto& m : plan) {
                    std::string err;
                    const std::string provider = mods::canonical_source(m.source);
                    const std::string loader = m.loader.empty() ? target_loader : m.loader;
                    if (!mods::install_mod(cfg, m.slug,
                                           provider.empty() ? "modrinth" : provider, loader, gv, mods_dir,
                                           install_log, &err)) {
                        install_log.push_back("FAILED " + m.slug + ": " + err);
                    }
                }
                set_mod_install_log(st, std::move(install_log));
                st.mod_installing = false;
                st.instances_loaded = false;
                log_line(st, L"[pack] install done");
                }));
            }
        }
        ImGui::SameLine();
        if (ghost_button("Export manifest", ImVec2(ui_px(160.0f), ui_px(34.0f)))) {
            Json manifest = Json::obj();
            Json files = Json::arr();
            for (const auto& m : plan) {
                Json e = Json::obj();
                e.set("slug", Json::str(m.slug));
                e.set("source", Json::str(m.source));
                e.set("loader", Json::str(m.loader));
                files.push(e);
            }
            manifest.set("name", Json::str("Amalgam pack"));
            manifest.set("game_version", Json::str(st.selected));
            manifest.set("loader", Json::str(st.mod_loader.empty() ? "fabric" : st.mod_loader));
            manifest.set("files", files);
            manifest.set("notes", Json::str(pack_summary));
            std::string err;
            std::wstring out_path = st.exe_dir + L"\\modpack-amalgam.json";
            if (json_write_file(out_path, manifest, &err)) {
                log_line(st, L"[pack] manifest written to " + out_path);
            } else {
                log_line(st, L"[pack] manifest write failed: " + net::to_wide(err));
            }
        }
        ImGui::SameLine();
        if (ghost_button("Generate banner", ImVec2(ui_px(160.0f), ui_px(34.0f)))) {
            std::string prompt =
                "Minecraft modpack banner art, epic cinematic, wide 16:9, no text: " +
                st.pack_prompt;
            ai::ImageRequest req;
            req.prompt = prompt;
            req.size = "1792x1024";
            spawn_worker(st, std::thread([&st, req]() {
                std::vector<uint8_t> png;
                std::string err;
                if (ai::image(req, png, &err)) {
                    std::wstring p = st.exe_dir + L"\\modpack-banner.png";
                    if (net::mkdirs(st.exe_dir)) {
                        FILE* f = nullptr;
                        if (_wfopen_s(&f, p.c_str(), L"wb") == 0 && f) {
                            fwrite(png.data(), 1, png.size(), f);
                            fclose(f);
                            log_line(st, L"[pack] banner saved to " + p);
                        }
                    }
                } else {
                    log_line(st, L"[pack] image gen failed: " + net::to_wide(err));
                }
            }));
        }
        if (st.mod_installing) {
            ImGui::SameLine();
            ImGui::TextColored(k.yellow, "installing...");
        }
        for (const auto& l : mod_install_log_snapshot(st)) ImGui::TextUnformatted(l.c_str());
    }
    card_end();

}

// ---------------------------------------------------------------------------
// Bedrock tab
// Bedrock UI functions are now in bedrock_ui.cpp

// ---------------------------------------------------------------------------
// Bedrock functions are now in bedrock_ui.cpp
void draw_admin_settings(UiState& st) {
    config::Config& c = *st.cfg;
    const uint64_t now = GetTickCount64();
    static uint64_t last_staff_check = 0;
    if (st.admin_unlocked && now >= st.admin_unlock_until_ms) {
        st.admin_unlocked = false;
        st.admin_password.clear();
        st.admin_password_confirm.clear();
        st.admin_status = "Admin session expired after 15 minutes.";
    }
    if (!st.admin_unlocked && now - last_staff_check > 5000) {
        last_staff_check = now;
        auto* client = aml::supabase::SupabaseManager::instance().client();
        if (client && client->is_current_user_staff()) {
            st.admin_unlocked = true;
            st.admin_unlock_until_ms = now + 15ull * 60ull * 1000ull;
            st.admin_status = "Staff access enabled for this account.";
        }
    }

    card_begin("##adminaccess", ImVec2(-1, 0));
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted("Admin control center");
    ImGui::PopFont();
    ImGui::TextColored(k.muted,
                       "Publisher and credential controls are local to this Windows account. They are never included in exported profiles or release packages.");
    ImGui::Spacing();

    if (!admin_auth::configured(c) && !st.admin_unlocked) {
        auto* client = aml::supabase::SupabaseManager::instance().client();
        if (client && client->is_authenticated()) {
            ImGui::TextColored(k.yellow, "Staff role required.");
            ImGui::TextWrapped("Admin access is granted server-side to approved staff accounts in public.staff_roles.");
            ImGui::TextColored(k.muted, "Signed in account: %s", client->current_user().email.c_str());
            if (ghost_button("Refresh staff access", ImVec2(ui_px(170.0f), ui_px(32.0f))))
                last_staff_check = 0;
        } else {
            ImGui::TextColored(k.yellow, "Amalgam account sign-in required.");
            ImGui::TextWrapped("Sign into your Amalgam account before requesting staff access.");
        }
    } else if (!st.admin_unlocked) {
        ImGui::TextColored(k.yellow, "Admin controls are locked.");
        ImGui::SetNextItemWidth(auto_item_width(320.0f, 180.0f));
        input_secret("##admin_unlock_password", &st.admin_password);
        ImGui::SameLine();
        ImGui::TextColored(k.muted, "password");
        const bool retry_wait = now < st.admin_retry_after_ms;
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, retry_wait ? 0.45f : 1.0f);
        if (primary_button(retry_wait ? "Try again shortly" : "Unlock Admin", ImVec2(ui_px(150.0f), ui_px(34.0f))) &&
            !retry_wait) {
            if (admin_auth::verify_password(c, st.admin_password)) {
                st.admin_unlocked = true;
                st.admin_unlock_until_ms = now + 15ull * 60ull * 1000ull;
                st.admin_failed_attempts = 0;
                st.admin_status = "Admin unlocked for 15 minutes.";
            } else {
                ++st.admin_failed_attempts;
                const uint64_t delay = std::min<uint64_t>(30000, 1000ull <<
                                                          std::min(st.admin_failed_attempts - 1, 5));
                st.admin_retry_after_ms = now + delay;
                st.admin_status = "Incorrect Admin password.";
            }
            st.admin_password.clear();
        }
        ImGui::PopStyleVar();
    } else {
        ImGui::SameLine();
        ImGui::TextColored(k.green, "Unlocked");
        ImGui::SameLine();
        if (ghost_button("Lock now", ImVec2(ui_px(100.0f), ui_px(30.0f)))) {
            st.admin_unlocked = false;
            st.admin_password.clear();
            st.admin_password_confirm.clear();
            st.admin_status = "Admin locked.";
        }
        ImGui::TextColored(k.muted, "Session expires automatically after 15 minutes.");
        ImGui::Spacing();
        if (primary_button("Open Admin Dashboard", ImVec2(ui_px(190.0f), ui_px(32.0f)))) {
            st.sidebar_item = 18;
            st.active_tab = 18;
        }

        const char* admin_tabs[] = {"Overview", "Secrets", "Providers", "Runtime", "Publisher", "Diagnostics"};
        ImGui::Spacing();
        if (ImGui::BeginTabBar("##admintabs")) {
            for (int i = 0; i < static_cast<int>(std::size(admin_tabs)); ++i) {
                if (!ImGui::BeginTabItem(admin_tabs[i])) continue;
                st.admin_section = i;
                if (i == 0) {
                    ImGui::TextColored(k.muted, "This workspace controls the local launcher only.");
                    ImGui::Spacing();
                    ImGui::Text("Provider secrets: %s", (c.modrinth_token.empty() && c.curseforge_key.empty())
                                                         ? "none configured" : "configured");
                    ImGui::Text("AI providers: %d", static_cast<int>(c.ai_providers.size()));
                    ImGui::Text("Microsoft sign-in: %s",
                                auth::valid_client_id(c.microsoft_client_id) ? "client ID ready" : "client ID needs setup");
                    ImGui::Text("Protected config: %s", c.has_unreadable_secrets ? "needs recovery" : "healthy");
                    ImGui::Spacing();
                    ImGui::TextColored(k.muted, "Use Secrets to add or forget credentials. Use Publisher for the public Microsoft application ID.");
                } else if (i == 1) {
                    ImGui::TextColored(k.muted, "Credentials are masked in the UI and encrypted with Windows DPAPI when saved.");
                    ImGui::Spacing();
                    ImGui::TextUnformatted("Modrinth");
                    ImGui::SetNextItemWidth(auto_item_width(360.0f, 180.0f));
                    if (input_secret("##admin_modrinth", &c.modrinth_token)) st.settings_dirty = true;
                    ImGui::SameLine();
                    if (ghost_button("Forget##admin_modrinth", ImVec2(ui_px(82.0f), 0))) {
                        c.modrinth_token.clear();
                        st.settings_dirty = true;
                    }
                    ImGui::TextColored(c.modrinth_token.empty() ? k.yellow : k.green,
                                       c.modrinth_token.empty() ? "not configured" : "configured");
                    ImGui::Spacing();
                    ImGui::TextUnformatted("CurseForge");
                    ImGui::SetNextItemWidth(auto_item_width(360.0f, 180.0f));
                    if (input_secret("##admin_curseforge", &c.curseforge_key)) st.settings_dirty = true;
                    ImGui::SameLine();
                    if (ghost_button("Forget##admin_curseforge", ImVec2(ui_px(82.0f), 0))) {
                        c.curseforge_key.clear();
                        st.settings_dirty = true;
                    }
                    ImGui::TextColored(c.curseforge_key.empty() ? k.yellow : k.green,
                                       c.curseforge_key.empty() ? "not configured" : "configured");
                    if (st.curseforge_testing) {
                        ImGui::SameLine();
                        ImGui::TextColored(k.yellow, "testing...");
                    } else if (ghost_button("Test CurseForge", ImVec2(ui_px(140.0f), ui_px(32.0f)))) {
                        config::Config snapshot = c;
                        st.curseforge_testing = true;
                        set_settings_status(st, "Testing CurseForge connection...");
                        spawn_worker(st, std::thread(test_curseforge_connection, std::ref(st), snapshot));
                    }
                    if (c.has_unreadable_secrets) {
                        ImGui::Spacing();
                        ImGui::TextColored(k.red, "Some protected credentials could not be opened by this Windows account.");
                        if (ghost_button("Forget inaccessible credentials", ImVec2(ui_px(240.0f), ui_px(32.0f)))) {
                            c.has_unreadable_secrets = false;
                            st.settings_dirty = true;
                        }
                    }
                } else if (i == 2) {
                    ImGui::TextColored(k.brand, "Built-in local AI");
                    ImGui::Spacing();
                    ImGui::TextWrapped("Amalgam uses its installed llama.cpp and stable-diffusion.cpp runtimes with the pinned local models. External provider URLs and API keys are not supported.");
                    ImGui::Spacing();
                    std::string local_error;
                    const bool local_ready = aml::ai::local_runtime_available(&local_error);
                    ImGui::TextColored(local_ready ? k.green : k.yellow,
                                       local_ready ? "Brain runtime ready" : "AI setup required");
                    if (!local_ready && !local_error.empty())
                        ImGui::TextColored(k.muted, "%s", local_error.c_str());
                    ImGui::TextColored(k.muted, "Install or repair Amalgam AI from Settings > AI.");
                } else if (i == 3) {
                    ImGui::TextColored(k.muted, "Advanced runtime controls apply only when you explicitly launch a profile.");
                    bool advanced_mode = c.advanced_mode;
                    if (ImGui::Checkbox("Enable advanced profile tools", &advanced_mode)) {
                        c.advanced_mode = advanced_mode;
                        st.settings_dirty = true;
                    }
                    ImGui::TextUnformatted("Additional JVM arguments");
                    ImGui::SetNextItemWidth(auto_item_width(560.0f, 220.0f));
                    if (input_text("##admin_jvm", &st.ui_jvm)) st.settings_dirty = true;
                    ImGui::TextUnformatted("Server address");
                    ImGui::SetNextItemWidth(auto_item_width(420.0f, 200.0f));
                    if (input_text("##admin_test_server", &st.ui_server)) st.settings_dirty = true;
                } else if (i == 4) {
                    ImGui::TextColored(k.muted, "The client ID identifies this launcher application. It is public, not a player secret.");
                    ImGui::SetNextItemWidth(auto_item_width(460.0f, 220.0f));
                    if (input_text_hint("##admin_microsoft_client_id", "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx",
                                        &c.microsoft_client_id))
                        st.settings_dirty = true;
                    ImGui::SameLine();
                    ImGui::TextColored(auth::valid_client_id(c.microsoft_client_id) ? k.green : k.yellow,
                                       auth::valid_client_id(c.microsoft_client_id) ? "ready" : "needs setup");
                    if (ghost_button("Open Microsoft setup guide", ImVec2(ui_px(206.0f), ui_px(32.0f))))
                        ShellExecuteW(st.hwnd, L"open",
                                      L"https://learn.microsoft.com/en-us/entra/identity-platform/quickstart-register-app",
                                      nullptr, nullptr, SW_SHOWNORMAL);
                } else {
                    const std::wstring data_root = c.base_dir.empty() ? st.exe_dir : c.base_dir;
                    ImGui::TextColored(k.muted, "Config file: %s",
                                       net::to_utf8(st.exe_dir + L"\\launcher.json").c_str());
                    ImGui::TextColored(k.muted, "Profile data: %s", net::to_utf8(data_root).c_str());
                    if (ghost_button("Open launcher folder", ImVec2(ui_px(170.0f), ui_px(32.0f))))
                        ShellExecuteW(st.hwnd, L"open", st.exe_dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    ImGui::SameLine();
                    if (ghost_button("Open profile data", ImVec2(ui_px(160.0f), ui_px(32.0f))))
                        ShellExecuteW(st.hwnd, L"open", data_root.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    ImGui::TextColored(k.muted, "Protected credentials are saved only after you press Save settings.");
                    if (ghost_button("Change Admin password", ImVec2(ui_px(180.0f), ui_px(32.0f)))) {
                        st.admin_password.clear();
                        st.admin_password_confirm.clear();
                        ImGui::OpenPopup("Change Admin password##admin");
                    }
                    if (ImGui::BeginPopupModal("Change Admin password##admin", nullptr,
                                               ImGuiWindowFlags_AlwaysAutoResize)) {
                        ImGui::TextUnformatted("Choose a new Admin password.");
                        input_secret("##admin_change_password", &st.admin_password);
                        input_secret("##admin_change_confirm", &st.admin_password_confirm);
                        if (primary_button("Save new password", ImVec2(ui_px(160.0f), ui_px(32.0f)))) {
                            if (st.admin_password != st.admin_password_confirm) {
                                st.admin_status = "The two passwords do not match.";
                            } else {
                                config::Config backup = c;
                                std::string error;
                                if (admin_auth::set_password(c, st.admin_password, &error) && save_ui_config(st)) {
                                    st.admin_password.clear();
                                    st.admin_password_confirm.clear();
                                    st.admin_status = "Admin password changed.";
                                    ImGui::CloseCurrentPopup();
                                } else {
                                    c = std::move(backup);
                                    st.admin_status = error.empty() ? "Admin password could not be saved." : error;
                                }
                            }
                        }
                        ImGui::SameLine();
                        if (ghost_button("Cancel", ImVec2(ui_px(90.0f), ui_px(32.0f)))) ImGui::CloseCurrentPopup();
                        ImGui::EndPopup();
                    }
                }
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    if (!st.admin_status.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(st.admin_status.find("Incorrect") != std::string::npos ? k.red : k.muted,
                           "%s", st.admin_status.c_str());
    }
    card_end();
}

// ---------------------------------------------------------------------------
// Settings tab
// ---------------------------------------------------------------------------
void draw_settings_tab(UiState& st) {
    config::Config& c = *st.cfg;
    draw_page_emblem(st, "settings-emblem-ai.png");
    page_title("Settings", "launcher configuration");
    draw_breadcrumbs({"Home", "Settings"});
    // Settings search
    static char settings_search_buf[256] = "";
    ImGui::SetNextItemWidth(std::min(ui_px(320.0f), ImGui::GetContentRegionAvail().x));
    ImGui::InputTextWithHint("##settings_search", "Search settings...", settings_search_buf, sizeof(settings_search_buf));
    ImGui::Spacing();
    if (st.settings_dirty) {
        ImGui::SameLine();
        ImGui::TextColored(k.yellow, "Unsaved changes");
    }
    const std::string settings_status = settings_status_snapshot(st);
    if (!settings_status.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(settings_status.find("failed") != std::string::npos ||
                                   settings_status.find("Could not") != std::string::npos
                               ? k.red
                               : k.green,
                           "%s", settings_status.c_str());
    }

    // Keep the dense settings surface navigable at the same scale as the
    // reference launcher. On narrow windows the vertical rail becomes a
    // compact selector so the actual controls retain usable width.
    const char* settings_nav[] = {"General", "Amalgam Account", "Minecraft", "Launcher", "Downloads",
                                   "Modpacks", "Java", "Performance", "Notifications",
                                   "Privacy", "Advanced", "Translation", "Admin (Staff)",
                                   "Diagnostics"};
    const char* settings_descriptions[] = {
        "Account health, sign-in status, and friends",
        "Amalgam account, sign-in, and profile sync",
        "Game folders, versions, and profile locations",
        "Appearance, launcher defaults, and accessibility",
        "Download jobs, history, and recovery",
        "Mod providers, mod manager, and AI settings",
        "Java runtime detection, versions, and downloads",
        "Memory, performance profiles, and optimization",
        "Alert and notification preferences",
        "Local data, privacy, and telemetry",
        "Developer and publisher controls",
        "Language and translation settings",
        "Staff-only protected settings",
        "System health, runtime checks, and diagnostics"
    };
    std::vector<int> visible_settings;
    for (int i = 0; i < static_cast<int>(std::size(settings_nav)); ++i) {
        #if !defined(AMALGAM_DEVELOPER_UI)
        if (i == 9) continue;
        #endif
        if (settings_search_filter(settings_search_buf, settings_nav[i], settings_descriptions[i]))
            visible_settings.push_back(i);
    }
    if (visible_settings.empty()) {
        card_begin("##settings_search_empty", ImVec2(-1, 0));
        empty_state("No settings found", "Try a different name, category, or keyword.");
        card_end();
        return;
    }
    if (std::find(visible_settings.begin(), visible_settings.end(), st.settings_section) ==
        visible_settings.end())
        st.settings_section = visible_settings.front();

    const bool compact_settings = ImGui::GetContentRegionAvail().x < ui_px(860.0f);
    if (compact_settings) {
        int selected_settings = 0;
        for (size_t i = 0; i < visible_settings.size(); ++i)
            if (visible_settings[i] == st.settings_section) selected_settings = static_cast<int>(i);
        ImGui::TextColored(k.muted, "Settings section");
        ImGui::SetNextItemWidth(std::min(ui_px(360.0f), ImGui::GetContentRegionAvail().x));
        if (ImGui::BeginCombo("##settings_section_compact", settings_nav[visible_settings[selected_settings]])) {
            for (size_t i = 0; i < visible_settings.size(); ++i) {
                const int section = visible_settings[i];
                if (ImGui::Selectable(settings_nav[section], section == st.settings_section))
                    st.settings_section = section;
            }
            ImGui::EndCombo();
        }
        ImGui::Spacing();
    } else {
        ImGui::BeginChild("##settingsnav", ImVec2(ui_px(184.0f), 0), ImGuiChildFlags_Borders);
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("Settings");
        ImGui::PopFont();
        ImGui::Spacing();
        // Category group headers for better organization
        auto draw_settings_group = [&](const char* label) {
            ImGui::PushFont(f_small);
            ImGui::TextColored(k.muted, "%s", label);
            ImGui::PopFont();
            ImGui::Dummy(ImVec2(0, ui_px(2.0f)));
        };
        for (const int section : visible_settings) {
            // Insert group headers at natural breaks
            if (section == 0) draw_settings_group("ACCOUNT");
            if (section == 2) draw_settings_group("MINECRAFT");
            if (section == 4) draw_settings_group("LAUNCHER");
            if (section == 8) draw_settings_group("SYSTEM");
            if (section == 11) draw_settings_group("ADMIN");
            if (section == 13) draw_settings_group("DIAGNOSTICS");
            const bool selected = section == st.settings_section;
            const float row_height = ui_px(36.0f);
            const ImVec2 row_min = ImGui::GetCursorScreenPos();
            const float row_width = ImGui::GetContentRegionAvail().x;
            const std::string row_id = std::string("##settings_nav_") + std::to_string(section);
            ImGui::InvisibleButton(row_id.c_str(), ImVec2(row_width, row_height));
            const bool hovered = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked()) st.settings_section = section;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            if (selected || hovered) {
                dl->AddRectFilled(row_min, row_min + ImVec2(row_width, row_height),
                                  c32(selected ? k.sel : k.hover), ui_px(7.0f));
            }
            if (selected) {
                dl->AddRectFilled(row_min, row_min + ImVec2(ui_px(3.0f), row_height),
                                  c32(k.brand), ui_px(1.0f));
            }
            dl->AddText(row_min + ImVec2(ui_px(12.0f),
                                         (row_height - ImGui::GetTextLineHeight()) * 0.5f),
                        c32(selected ? k.text : k.muted), settings_nav[section]);
            ImGui::Dummy(ImVec2(0, ui_px(3.0f)));
        }
        ImGui::EndChild();
        ImGui::SameLine(0, ui_px(14.0f));
    }
    ImGui::BeginChild("##settingscontent", ImVec2(0, 0));

    if (st.settings_section == 0) {
    card_begin("##account", ImVec2(-1, 0));
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted("Minecraft account");
    ImGui::PopFont();
    if (!st.auth_checked) {
        std::string error;
        {
            std::lock_guard<std::mutex> lock(st.auth_mu);
            st.auth_checked = true;
            if (auth::load(st.account, &error))
                st.auth_status = "Signed in as " + st.account.username;
            else
                st.auth_status = "No Microsoft account connected";
        }
    }
    std::string auth_status;
    {
        std::lock_guard<std::mutex> lock(st.auth_mu);
        auth_status = st.auth_status;
    }
    ImGui::TextColored(k.muted, "%s", auth_status.c_str());
    const bool sign_in_configured = auth::valid_client_id(c.microsoft_client_id);
    if (!sign_in_configured) {
        ImGui::TextColored(k.yellow,
                           "Microsoft sign-in is unavailable in this build.");
    }
    if (st.auth_working) {
        ImGui::SameLine();
        ImGui::TextColored(k.yellow, "sign-in is waiting for Microsoft...");
        ImGui::SameLine();
        if (ghost_button("Open sign-in wizard", ImVec2(ui_px(170.0f), ui_px(32.0f)))) st.login_wizard_open = true;
    } else if (ghost_button("Connect Microsoft account", ImVec2(ui_px(210.0f), ui_px(32.0f)))) {
        start_microsoft_login(st);
    }
    // Only show Disconnect when an account is actually connected
    const bool has_account = !st.account.username.empty();
    if (has_account) {
    ImGui::SameLine();
    if (ghost_button("Disconnect", ImVec2(ui_px(110.0f), ui_px(32.0f)))) {
        std::string error;
        if (auth::logout(&error)) {
            std::lock_guard<std::mutex> lock(st.auth_mu);
            st.account = {};
            st.auth_status = "No Microsoft account connected";
        } else {
            std::lock_guard<std::mutex> lock(st.auth_mu);
            st.auth_status = "Logout failed: " + error;
        }
    }
    } // has_account
    card_end();

    ImGui::Spacing();
    card_begin("##launchmode", ImVec2(-1, 0));
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted("How Play starts the game");
    ImGui::PopFont();
    ImGui::TextColored(k.muted,
                       "Choose whether Amalgam launches Minecraft itself or hands the\n"
                       "prepared profile to the official Minecraft Launcher.");
    const bool microsoft_available = sign_in_configured && auth::valid_client_id(c.microsoft_client_id);
    int mode_index = c.launch_mode == "microsoft" ? 0 : 1;
    if (ImGui::RadioButton("Amalgam (sign in with Microsoft here)", &mode_index, 0)) {
        c.launch_mode = "microsoft";
        st.settings_dirty = true;
    }
    if (!microsoft_available) {
        ImGui::SameLine();
        ImGui::TextColored(k.yellow, "unavailable in this build");
    }
    if (ImGui::RadioButton("Official Minecraft Launcher (use its Microsoft sign-in)", &mode_index, 1)) {
        c.launch_mode = "official_launcher";
        st.settings_dirty = true;
    }
    ImGui::TextColored(k.muted,
                       c.launch_mode == "microsoft"
                           ? "Play opens the game directly with your connected account."
                           : "Play prepares the profile, then opens the Minecraft Launcher.\n"
                             "Select the Amalgam profile there and press Play.");
    card_end();

    ImGui::Spacing();
    card_begin("##health", ImVec2(-1, 0));
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted("Player readiness");
    ImGui::PopFont();
    ImGui::TextColored(k.muted,
                       "Checks the files, account, providers, and Windows edition needed before play.");
    auto health_row = [&](const readiness::Check& check) {
        const ImVec4 color = check.state == readiness::State::Ready
                                 ? k.green
                                 : (check.state == readiness::State::Attention ? k.yellow : k.muted);
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::TextUnformatted(readiness::state_name(check.state));
        ImGui::PopStyleColor();
        ImGui::SameLine(ui_px(92.0f));
        ImGui::PushFont(f_bold);
        ImGui::TextUnformatted(check.label.c_str());
        ImGui::PopFont();
        ImGui::SameLine(ui_px(230.0f));
        ImGui::TextColored(k.muted, "%s", check.detail.c_str());
    };
    readiness::Report report;
    bool readiness_checked = false;
    {
        std::lock_guard<std::mutex> lock(st.readiness_mu);
        report = st.readiness_report;
        readiness_checked = st.readiness_checked;
    }
    if (readiness_checked) {
        for (const auto& check : report.checks) health_row(check);
        ImGui::Spacing();
        ImGui::TextColored(report.java_play_ready() ? k.green : k.yellow,
                           "%s", report.java_play_ready()
                                      ? "Java Edition is ready to launch."
                                      : "Complete the Action items before launching Java Edition.");
    } else {
        ImGui::Spacing();
        ImGui::TextDisabled("Run a live check to see exactly what this PC and account still need.");
    }
    ImGui::Spacing();
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, st.readiness_testing ? 0.55f : 1.0f);
    if (primary_button(st.readiness_testing ? "CHECKING..." : "RUN LIVE READINESS CHECK",
                       ImVec2(ui_px(226.0f), ui_px(32.0f))) && !st.readiness_testing) {
        const config::Config snapshot = c;
        st.readiness_testing = true;
        spawn_worker(st, std::thread(run_player_readiness, std::ref(st), snapshot));
    }
    ImGui::PopStyleVar();
    ImGui::SameLine();
    if (ghost_button("Refresh local data", ImVec2(ui_px(160.0f), ui_px(32.0f)))) {
        st.java_scanned = false;
        st.bedrock_scanned = false;
        {
            std::lock_guard<std::mutex> lock(st.readiness_mu);
            st.readiness_checked = false;
            st.readiness_report = {};
        }
        st.home_readiness.dirty = true;
        push_notice(st, ui_model::NoticeLevel::Info, "Health checks refreshed",
                    "Local runtime and Bedrock state will refresh before the next readiness check.");
    }
    card_end();

    ImGui::Spacing();
    card_begin("##feedback", ImVec2(-1, 0));
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted("Send Feedback");
    ImGui::PopFont();
    ImGui::TextColored(k.muted, "Report bugs, request features, or share general feedback.");
    ImGui::Spacing();
    if (ghost_button("Send Feedback", ImVec2(ui_px(150.0f), ui_px(32.0f))))
        st.local_feedback_open = true;
    card_end();
    }

    if (st.settings_section == 1) {
        // Amalgam Account Settings
        draw_account_page(st);
    }
    
    if (st.settings_section == 2) {
    ImGui::Spacing();

    card_begin("##set1", ImVec2(-1, 0));
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted("Folders");
    ImGui::PopFont();
    ImGui::Spacing();

    ImGui::Text("Instance base");
    ImGui::SetNextItemWidth(auto_item_width(320.0f, 160.0f, 108.0f));
    if (input_text("##basedir", &st.ui_base)) st.settings_dirty = true;
    ImGui::SameLine();
    if (ghost_button("Browse##base", ImVec2(ui_px(90.0f), 0))) {
        std::wstring target = net::to_wide(st.ui_base);
        show_browse(st, &target);
        st.ui_base = net::to_utf8(target);
        st.settings_dirty = true;
    }

    ImGui::Text("Assets dir");
    ImGui::SetNextItemWidth(auto_item_width(320.0f, 160.0f, 108.0f));
    if (input_text("##assetsdir", &st.ui_assets)) st.settings_dirty = true;
    ImGui::SameLine();
    if (ghost_button("Browse##assets", ImVec2(ui_px(90.0f), 0))) {
        std::wstring target = net::to_wide(st.ui_assets);
        show_browse(st, &target);
        st.ui_assets = net::to_utf8(target);
        st.settings_dirty = true;
    }

    ImGui::Text("Java cache");
    ImGui::SetNextItemWidth(auto_item_width(320.0f, 160.0f, 108.0f));
    if (input_text("##javacache", &st.ui_java_cache)) st.settings_dirty = true;
    ImGui::SameLine();
    if (ghost_button("Browse##java", ImVec2(ui_px(90.0f), 0))) {
        std::wstring target = net::to_wide(st.ui_java_cache);
        show_browse(st, &target);
        st.ui_java_cache = net::to_utf8(target);
        st.settings_dirty = true;
    }
    card_end();
    }

    if (st.settings_section == 2) {
    ImGui::Spacing();
    card_begin("##launcherdefaults", ImVec2(-1, 0));
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted("Launcher defaults");
    ImGui::PopFont();
    ImGui::TextColored(k.muted, "These choices seed new profiles and make the home screen feel like yours.");
    ImGui::Spacing();
    ImGui::TextUnformatted("Display name");
    ImGui::SetNextItemWidth(auto_item_width(360.0f, 180.0f));
    if (input_text("##settingsusername", &st.ui_username)) st.settings_dirty = true;
    ImGui::TextColored(k.muted,
                       "Used only as a local launcher label before a Minecraft account is connected.");
    ImGui::TextUnformatted("Default loader");
    const char* loaders[] = {"auto", "vanilla", "fabric", "quilt", "forge", "neoforge"};
    int loader_index = 0;
    for (int i = 0; i < 6; ++i) if (c.loader == loaders[i]) loader_index = i;
    ImGui::SetNextItemWidth(auto_item_width(220.0f, 140.0f));
    if (ImGui::Combo("##settingsloader", &loader_index,
                     "auto\0vanilla\0fabric\0quilt\0forge\0neoforge\0", 6)) {
        c.loader = loaders[loader_index];
        st.settings_dirty = true;
    }
    #if defined(AMALGAM_DEVELOPER_UI)
    ImGui::TextUnformatted("Additional JVM arguments");
    ImGui::SetNextItemWidth(auto_item_width(560.0f, 220.0f));
    if (input_text("##settingsjvm", &st.ui_jvm)) st.settings_dirty = true;
    ImGui::TextColored(k.muted, "Applied after Amalgam's compatibility and performance arguments.");
    #else
    ImGui::TextColored(k.muted, "Java memory and compatibility tuning are managed by the selected performance profile.");
    #endif
    card_end();

    ImGui::Spacing();
    card_begin("##launcherfiles", ImVec2(-1, 0));
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted("Launcher files");
    ImGui::PopFont();
    const std::wstring data_root = c.base_dir.empty() ? st.exe_dir : c.base_dir;
    ImGui::TextColored(k.muted, "Launcher: %s", net::to_utf8(st.exe_dir).c_str());
    ImGui::TextColored(k.muted, "Profile data: %s", net::to_utf8(data_root).c_str());
    if (ghost_button("Open launcher folder", ImVec2(ui_px(170.0f), ui_px(32.0f))))
        ShellExecuteW(st.hwnd, L"open", st.exe_dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    ImGui::SameLine();
    if (ghost_button("Open profile data", ImVec2(ui_px(160.0f), ui_px(32.0f))))
        ShellExecuteW(st.hwnd, L"open", data_root.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    card_end();
    }

    if (st.settings_section == 5) {
    ImGui::Spacing();
    card_begin("##set2", ImVec2(-1, 0));
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted("Modding APIs");
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, k.yellow);
    ImGui::TextWrapped("Provider keys are protected with Windows encryption when saved. Never share them in packs, screenshots, diagnostics, or release archives.");
    ImGui::PopStyleColor();
    if (c.secrets_need_migration) {
        ImGui::TextColored(k.yellow,
                           "Legacy plaintext credentials will be encrypted the next time you save Settings.");
    }
    if (c.has_unreadable_secrets) {
        ImGui::TextColored(k.red,
                           "One or more credentials belong to another Windows account and were not loaded.");
        ImGui::TextWrapped("Re-enter the credentials below, or explicitly forget the inaccessible values before saving other settings.");
        if (ghost_button("Forget inaccessible credentials", ImVec2(ui_px(240.0f), ui_px(32.0f)))) {
            c.has_unreadable_secrets = false;
            st.settings_dirty = true;
            push_notice(st, ui_model::NoticeLevel::Warning, "Inaccessible credentials cleared",
                        "Save Settings to keep the remaining configuration. Re-enter any provider keys you still need.");
        }
    }
    ImGui::Spacing();
    #if defined(AMALGAM_DEVELOPER_UI)
    ImGui::Text("Modrinth token (optional)");
    ImGui::SetNextItemWidth(auto_item_width(320.0f, 180.0f));
    if (input_secret("##modrinth_token", &c.modrinth_token)) st.settings_dirty = true;
    ImGui::Text("CurseForge API key");
    ImGui::SetNextItemWidth(auto_item_width(320.0f, 180.0f, 170.0f));
    if (input_secret("##curseforge_key", &c.curseforge_key)) st.settings_dirty = true;
    ImGui::SameLine();
    const auto curseforge_api = provider_config::make(c);
    const bool curseforge_backend_ready = mods::curseforge_available(curseforge_api);
    const bool curseforge_backend_configured = mods::curseforge_proxy_configured(curseforge_api);
    ImGui::TextColored(curseforge_backend_ready ? k.green : k.yellow,
                       curseforge_backend_ready ? "connected"
                           : curseforge_backend_configured ? "sign-in required" : "not configured");
    if (ghost_button(st.curseforge_testing ? "Testing..." : "Test CurseForge", ImVec2(ui_px(140.0f), ui_px(32.0f))) &&
        !st.curseforge_testing) {
        config::Config snapshot = c;
        st.curseforge_testing = true;
        set_settings_status(st, "Testing CurseForge connection...");
        spawn_worker(st, std::thread(test_curseforge_connection, std::ref(st), snapshot));
    }
    ImGui::SameLine();
    ImGui::TextColored(k.muted, "optional override; official builds use the secure Amalgam backend");
    #else
    ImGui::TextColored(k.muted, "Provider credentials are not exposed in the published launcher.");
    const auto curseforge_api = provider_config::make(c);
    const bool curseforge_backend_ready = mods::curseforge_available(curseforge_api);
    const bool curseforge_backend_configured = mods::curseforge_proxy_configured(curseforge_api);
    ImGui::TextColored(curseforge_backend_ready ? k.green : k.yellow,
                       curseforge_backend_ready ? "CurseForge secure backend connected"
                           : curseforge_backend_configured
                               ? "CurseForge secure backend ready; Amalgam sign-in required"
                               : "CurseForge integration unavailable");
    #endif
    card_end();

    ImGui::Spacing();
    card_begin("##set3", ImVec2(-1, 0));
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted("AI providers (legacy)");
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, k.yellow);
    ImGui::TextWrapped("Provider keys are private account credentials. They are Windows-encrypted locally; exported packs and release packages never include them.");
    ImGui::PopStyleColor();
    ImGui::PushFont(f_small);
    ImGui::TextDisabled("External AI providers are disabled; Amalgam uses its installed local models.");
    ImGui::PopFont();
    ImGui::Spacing();
    #if defined(AMALGAM_DEVELOPER_UI)
    ImGui::TextColored(k.yellow, "Developer-only legacy provider settings are disabled in local-only builds.");
    for (size_t i = 0; i < c.ai_providers.size(); ++i) {
        auto& ap = c.ai_providers[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::PushFont(f_bold);
        ImGui::Text("%d", static_cast<int>(i));
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ui_px(90.0f));
        if (input_text("##pname", &ap.name)) st.settings_dirty = true;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ui_px(220.0f));
        if (input_text("##purl", &ap.base_url)) st.settings_dirty = true;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ui_px(180.0f));
        if (input_secret("##pkey", &ap.api_key)) st.settings_dirty = true;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ui_px(160.0f));
        if (input_text("##pchat", &ap.chat_model)) st.settings_dirty = true;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ui_px(140.0f));
        if (input_text("##pimg", &ap.image_model)) st.settings_dirty = true;
        ImGui::SameLine();
        if (ghost_button("up", ImVec2(ui_px(40.0f), 0)) && i > 0) {
            std::swap(c.ai_providers[i], c.ai_providers[i - 1]);
            st.settings_dirty = true;
        }
        ImGui::SameLine();
        if (ghost_button("down", ImVec2(ui_px(40.0f), 0)) && i + 1 < c.ai_providers.size()) {
            std::swap(c.ai_providers[i], c.ai_providers[i + 1]);
            st.settings_dirty = true;
        }
        ImGui::SameLine();
        if (ghost_button("del", ImVec2(ui_px(40.0f), 0))) {
            c.ai_providers.erase(c.ai_providers.begin() + static_cast<ptrdiff_t>(i));
            st.settings_dirty = true;
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    if (ghost_button("Add provider", ImVec2(ui_px(130.0f), ui_px(32.0f)))) {
        config::AiProvider ap;
        ap.name = "new";
        ap.base_url = "https://api.openai.com/v1";
        ap.chat_model = "gpt-4o";
        c.ai_providers.push_back(ap);
        st.settings_dirty = true;
    }
    ImGui::SameLine();
    ImGui::PushFont(f_small);
    ImGui::TextDisabled("empty name shows too; they are tried in list order");
    ImGui::PopFont();
    #else
    ImGui::TextColored(k.muted, "Built-in local AI models are used. No external provider or API key is required.");
    #endif
    card_end();

    // ── Local AI installation (in-launcher GUI) ────────────────────────
    ImGui::Spacing();
    card_begin("##ai_install", ImVec2(-1, 0));
    aml::ai_install_ui::draw_ai_install_panel(st);
    card_end();
    }

    if (st.settings_section == 6) {
    ImGui::Spacing();
    card_begin("##set4", ImVec2(-1, 0));
    ImGui::PushFont(f_h2);
    std::vector<java::Install> java_snapshot;
    {
        std::lock_guard<std::mutex> lock(st.java_mu);
        java_snapshot = st.javas;
    }
    ImGui::Text("Detected Java runtimes (%d)", static_cast<int>(java_snapshot.size()));
    ImGui::PopFont();
    ImGui::Spacing();
    for (const auto& j : java_snapshot) {
        ImGui::PushFont(f_small);
        ImGui::BulletText("Java %d  %s", j.major, net::to_utf8(j.home).c_str());
        ImGui::PopFont();
    }
    card_end();
    }

    if (st.settings_section == 3 || st.settings_section == 4 || st.settings_section == 7 ||
        st.settings_section == 8 || st.settings_section == 9 || st.settings_section == 10 ||
        st.settings_section == 11 || st.settings_section == 12 || st.settings_section == 13) {
        ImGui::Spacing();
        card_begin("##settingssection", ImVec2(-1, 0));
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted(settings_nav[st.settings_section]);
        ImGui::PopFont();
        if (st.settings_section == 3) {
            ImGui::TextColored(k.muted, "Configure global launcher behavior and appearance.");
            ImGui::Spacing();
            ImGui::TextUnformatted("Theme");
            ImGui::SetNextItemWidth(ui_px(200.0f));
            {
                extern void draw_theme_selector_inline(UiState& st, config::Config& c);
                draw_theme_selector_inline(st, c);
            }
            ImGui::SameLine(0, ui_px(12.0f));
            if (draw_theme_toggle()) {
                st.settings_dirty = true;
            }
            ImGui::Spacing();
            bool kb = c.keyboard_navigation;
            if (ImGui::Checkbox("Keyboard navigation shortcuts", &kb)) {
                c.keyboard_navigation = kb;
                st.settings_dirty = true;
            }
            ImGui::TextColored(k.muted, "Enable keyboard shortcuts for navigation between tabs and sections.");
            bool sr = c.screen_reader_support;
            if (ImGui::Checkbox("Screen reader announcements", &sr)) {
                c.screen_reader_support = sr;
                st.settings_dirty = true;
            }
            ImGui::TextColored(k.muted, "Announce tab changes and important state updates for assistive technology.");
            if (ghost_button("Send feedback", ImVec2(ui_px(150.0f), ui_px(32.0f))))
                st.feedback_open = true;
        } else if (st.settings_section == 4) {
            int active_jobs = 0;
            int completed_jobs = 0;
            int failed_jobs = 0;
            int resumable_jobs = 0;
            {
                std::lock_guard<std::mutex> lock(st.jobs_mu);
                for (const auto& job : st.jobs) {
                    if (job.active) ++active_jobs;
                    if (job.completed) ++completed_jobs;
                    if (job.failed || job.cancelled) ++failed_jobs;
                    if (job.resumable) ++resumable_jobs;
                }
            }
            ImGui::TextColored(k.muted,
                               "Persistent jobs keep their target and retry information after a restart.");
            ImGui::Spacing();
            ImGui::Text("%d active  |  %d completed  |  %d needs attention  |  %d resumable",
                        active_jobs, completed_jobs, failed_jobs, resumable_jobs);
            if (primary_button("Open Downloads", ImVec2(ui_px(150.0f), ui_px(32.0f)))) {
                st.sidebar_item = 17;
                st.active_tab = 17;
                st.downloads_open = true;
            }
            ImGui::SameLine();
            const bool can_clear = completed_jobs > 0;
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, can_clear ? 1.0f : 0.45f);
            if (ghost_button("Clear completed history", ImVec2(ui_px(180.0f), ui_px(32.0f))) && can_clear) {
                {
                    std::lock_guard<std::mutex> lock(st.jobs_mu);
                    st.jobs.erase(std::remove_if(st.jobs.begin(), st.jobs.end(),
                                                 [](const UiState::DownloadJob& job) {
                                                     return job.completed && !job.active;
                                                 }), st.jobs.end());
                }
                persist_jobs(st);
                push_notice(st, ui_model::NoticeLevel::Success, "Completed history cleared",
                            "Active and failed operations remain available for recovery.");
            }
            ImGui::PopStyleVar();
        } else if (st.settings_section == 7) {
            ImGui::TextColored(k.muted, "Choose the default performance policy used by new profiles.");
            const char* performance_ids[] = {"auto", "low_end", "balanced", "shaders",
                                              "heavy_modpack", "custom"};
            const char* performance_items = "Auto\0Low-end device\0Balanced\0Shaders\0Heavy modpack\0Custom\0";
            int performance_index = 0;
            const std::string current = performance::normalize_profile(c.performance_profile);
            for (int i = 0; i < 6; ++i) if (current == performance_ids[i]) performance_index = i;
            ImGui::SetNextItemWidth(ui_px(260.0f));
            if (ImGui::Combo("##settings_performance", &performance_index, performance_items, 6)) {
                c.performance_profile = performance_ids[performance_index];
                st.settings_dirty = true;
            }
            const auto tuning = performance::make_tuning(c.performance_profile,
                                                           performance::physical_memory_mb(), 21);
            const uint64_t physical_memory = performance::physical_memory_mb();
            ImGui::Spacing();
            ImGui::TextColored(k.muted, "This device: %s RAM", 
                               physical_memory > 0 ?
                                   (std::to_string(physical_memory / 1024) + " GB").c_str() :
                                   "memory unavailable");
            ImGui::TextColored(k.muted, "New profiles: %s heap, %s minimum heap",
                               tuning.heap_mb > 0 ? (std::to_string(tuning.heap_mb) + " MB").c_str() : "Automatic",
                               tuning.min_heap_mb > 0 ? (std::to_string(tuning.min_heap_mb) + " MB").c_str() : "Automatic");
            ImGui::TextColored(k.muted, "%d game option(s) will be available to apply per profile.",
                               static_cast<int>(tuning.game_options.size()));
        } else if (st.settings_section == 8) {
            const auto notices = notices_snapshot(st);
            int warnings = 0;
            int errors = 0;
            int sticky = 0;
            for (const auto& notice : notices) {
                if (notice.level == ui_model::NoticeLevel::Warning) ++warnings;
                if (notice.level == ui_model::NoticeLevel::Error) ++errors;
                if (notice.sticky) ++sticky;
            }
            ImGui::TextColored(k.muted, "%d alert(s): %d warning, %d error, %d pinned for recovery.",
                               static_cast<int>(notices.size()), warnings, errors, sticky);
            if (primary_button("Open Alerts", ImVec2(ui_px(130.0f), ui_px(32.0f)))) st.notice_center_open = true;
            ImGui::SameLine();
            const bool can_clear = !notices.empty();
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, can_clear ? 1.0f : 0.45f);
            if (ghost_button("Clear non-pinned", ImVec2(ui_px(150.0f), ui_px(32.0f))) && can_clear) {
                std::lock_guard<std::mutex> lock(st.notice_mu);
                st.notices.erase(std::remove_if(st.notices.begin(), st.notices.end(),
                                                [](const ui_model::Notice& notice) {
                                                    return !notice.sticky;
                                                }), st.notices.end());
            }
            ImGui::PopStyleVar();
            ImGui::TextColored(k.muted, "Pinned recovery alerts remain until you dismiss them explicitly.");
        } else if (st.settings_section == 9) {
            const std::wstring data_root = c.base_dir.empty() ? st.exe_dir : c.base_dir;
            ImGui::TextColored(k.muted, "Credentials and profile data stay on this Windows device.");
            ImGui::TextColored(k.green, "No remote telemetry upload is enabled by this launcher.");
            ImGui::Spacing();
            ImGui::TextColored(k.muted, "Local profile data: %s", net::to_utf8(data_root).c_str());
            if (ghost_button("Open local data", ImVec2(ui_px(150.0f), ui_px(32.0f))))
                ShellExecuteW(st.hwnd, L"open", data_root.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            #if defined(AMALGAM_DEVELOPER_UI)
            ImGui::SameLine();
            if (ghost_button("Open diagnostics", ImVec2(ui_px(160.0f), ui_px(32.0f)))) st.diagnostics_open = true;
            #endif
        } else if (st.settings_section == 11) {
            ImGui::TextColored(k.muted,
                                "Translate public project descriptions from Modrinth and CurseForge into your chosen language.");
            ImGui::Spacing();
            bool auto_translate = c.auto_translate_project_text;
            if (ImGui::Checkbox("Automatically translate newly opened project pages", &auto_translate)) {
                c.auto_translate_project_text = auto_translate;
                st.settings_dirty = true;
            }
            ImGui::TextColored(k.muted,
                               "Online translation is unavailable in local-only AI mode. Provider text stays local to this launcher.");
            const char* languages[] = {"English", "Spanish", "French", "German", "Portuguese",
                                       "Chinese (Simplified)", "Japanese", "Korean"};
            int language_index = 0;
            for (int i = 0; i < static_cast<int>(std::size(languages)); ++i) {
                if (c.translation_target_language == languages[i]) language_index = i;
            }
            ImGui::SetNextItemWidth(auto_item_width(260.0f, 180.0f));
            if (ImGui::Combo("Translate into", &language_index, languages,
                             static_cast<int>(std::size(languages)))) {
                c.translation_target_language = languages[language_index];
                st.settings_dirty = true;
            }
            ImGui::Spacing();
            ImGui::TextColored(k.yellow, "Online translation is disabled");
            ImGui::TextColored(k.muted, "The built-in AI does not send provider descriptions to external APIs.");
        } else if (st.settings_section == 12) {
            draw_admin_settings(st);
        } else if (st.settings_section == 13) {
            // Diagnostics page
            ImGui::PushFont(f_h2);
            ImGui::TextColored(k.brand, "SYSTEM DIAGNOSTICS");
            ImGui::PopFont();
            ImGui::Spacing();
            ImGui::TextColored(k.muted, "Runtime health checks for all Amalgam systems.");
            ImGui::Spacing();
            if (ghost_button("Run Diagnostics", ImVec2(ui_px(200.0f), ui_px(30.0f)))) {
                st.ui_diagnostics_results = aml::diagnostics::run_all();
                st.ui_diagnostics_ran = true;
            }
            ImGui::Spacing();
            if (st.ui_diagnostics_ran && !st.ui_diagnostics_results.empty()) {
                int pass = 0, warn = 0, fail = 0;
                for (const auto& r : st.ui_diagnostics_results) {
                    if (r.status == aml::diagnostics::CheckStatus::PASS) pass++;
                    else if (r.status == aml::diagnostics::CheckStatus::WARN) warn++;
                    else if (r.status == aml::diagnostics::CheckStatus::FAIL) fail++;
                }
                // Summary bar
                ImGui::TextColored(k.green, "OK: %d", pass);
                ImGui::SameLine();
                ImGui::TextColored(k.yellow, "Warnings: %d", warn);
                ImGui::SameLine();
                ImGui::TextColored(k.red, "Errors: %d", fail);
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                // Per-check rows
                ImGui::BeginChild("##diag_checks", ImVec2(-1, ui_px(340.0f)), true);
                for (const auto& r : st.ui_diagnostics_results) {
                    ImVec4 color = k.green;
                    const char* icon = "OK";
                    if (r.status == aml::diagnostics::CheckStatus::WARN) { color = k.yellow; icon = "WARN"; }
                    else if (r.status == aml::diagnostics::CheckStatus::FAIL) { color = k.red; icon = "FAIL"; }
                    ImGui::TextColored(color, "[%s]", icon);
                    ImGui::SameLine();
                    ImGui::Text("%s / %s", r.system.c_str(), r.check.c_str());
                    if (!r.detail.empty()) {
                        ImGui::Indent(ui_px(24.0f));
                        ImGui::TextColored(k.muted, "%s", r.detail.c_str());
                        ImGui::Unindent(ui_px(24.0f));
                    }
                    ImGui::Spacing();
                }
                ImGui::EndChild();
            } else if (!st.ui_diagnostics_ran) {
                ImGui::TextColored(k.muted, "Click Run Diagnostics to check all systems.");
            }
        } else if (st.settings_section == 10) {
            #if !defined(AMALGAM_DEVELOPER_UI)
            ImGui::TextColored(k.muted, "Publisher-only controls are not included in this release build.");
            ImGui::TextColored(k.muted, "Sign-in registration and developer transport settings are managed before publishing.");
            #else
            ImGui::TextColored(k.muted, "Advanced controls are explicit and remain local to this launcher.");
            bool advanced_mode = c.advanced_mode;
            if (ImGui::Checkbox("Enable advanced profile tools", &advanced_mode)) {
                c.advanced_mode = advanced_mode;
                st.settings_dirty = true;
            }
            ImGui::TextColored(k.muted,
                               "Shows profile delete actions and optional per-instance tuning controls; it never enables automatic game actions.");
            ImGui::Spacing();
            ImGui::TextUnformatted("Test server address (optional)");
            ImGui::SetNextItemWidth(auto_item_width(420.0f, 200.0f));
            if (input_text("##settingstestserver", &st.ui_server)) st.settings_dirty = true;
            ImGui::TextColored(k.muted, "Used only when you choose an explicit server launch workflow.");
            ImGui::Spacing();
            #endif
        }
        card_end();
    }

    ImGui::Spacing();

    // ── About Section ────────────────────────────────────────────
    {
        card_begin("##about_section", ImVec2(-1, 0));
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("About Amalgam");
        ImGui::PopFont();
        ImGui::Spacing();

        // Real packaged logo, with a small vector fallback for recovery mode
        // when a user launches from an incomplete development directory.
        ImVec2 logo_pos = ImGui::GetCursorScreenPos();
        ImVec2 logo_size(ui_px(56.0f), ui_px(56.0f));
        draw_local_image(st, st.exe_dir + L"\\branding\\amalgam-logo.png",
                         logo_pos, logo_size, c32(k.brand), ui_model::ImageFit::Contain);
        ImGui::Dummy(logo_size);

        ImGui::SameLine(ui_px(72.0f));
        ImGui::PushFont(f_h2);
        ImGui::TextColored(k.brand, "AMALGAM LAUNCHER");
        ImGui::PopFont();
        ImGui::TextColored(k.brand, "Version %s", kVersion);
        ImGui::TextColored(k.muted, "%s channel  \u2022  report issues so we can fix them",
                           kChannel);
        ImGui::TextColored(k.muted, "The ultimate Minecraft launcher.");
        ImGui::TextColored(k.muted, "Play. Create. Host. Together.");
        ImGui::Spacing();
        ImGui::TextColored(k.muted, "Copyright 2024 Amalgam. All rights reserved.");
        ImGui::Spacing();

        float link_w = ui_px(120.0f);
        if (ghost_button("Website", ImVec2(link_w, ui_px(28.0f)))) {
            ShellExecuteW(st.hwnd, L"open", L"https://amalgam-net.com",
                          nullptr, nullptr, SW_SHOWNORMAL);
        }
        ImGui::SameLine(0, ui_px(8.0f));
        if (ghost_button("Known Issues", ImVec2(link_w, ui_px(28.0f))))
            st.known_issues_open = true;
        ImGui::SameLine(0, ui_px(8.0f));
        if (ghost_button("Report a Bug", ImVec2(link_w, ui_px(28.0f))))
            st.feedback_open = true;
        ImGui::SameLine(0, ui_px(8.0f));
        if (ghost_button("Discord", ImVec2(link_w, ui_px(28.0f)))) {
            ShellExecuteW(st.hwnd, L"open", L"https://discord.gg/amalgam",
                          nullptr, nullptr, SW_SHOWNORMAL);
        }
        ImGui::SameLine(0, ui_px(8.0f));
        if (ghost_button("GitHub", ImVec2(link_w, ui_px(28.0f)))) {
            ShellExecuteW(st.hwnd, L"open", L"https://github.com/amalgam",
                          nullptr, nullptr, SW_SHOWNORMAL);
        }

        // ── Launcher update ─────────────────────────────────────────
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        const updater::CheckState update_state = st.update_status.state.load();
        switch (update_state) {
            case updater::CheckState::Idle:
                ImGui::TextColored(k.muted, "Update: not checked yet");
                break;
            case updater::CheckState::Checking:
                ImGui::TextColored(k.muted, "Update: checking...");
                break;
            case updater::CheckState::UpToDate:
                ImGui::TextColored(k.muted, "Update: you are on the latest version");
                break;
            case updater::CheckState::Available:
                ImGui::TextColored(k.brand, "Update available: v%s",
                                   st.update_status.available_version.c_str());
                break;
            case updater::CheckState::Mandatory:
                ImGui::TextColored(k.red, "Mandatory update: v%s",
                                   st.update_status.available_version.c_str());
                break;
            case updater::CheckState::Offline:
                ImGui::TextColored(k.muted, "Update: offline - could not reach the update server");
                break;
            case updater::CheckState::Error:
                ImGui::TextColored(k.red, "Update error: %s",
                                   st.update_status.error.c_str());
                break;
        }
        ImGui::Spacing();
        const bool checkable = update_state == updater::CheckState::Idle ||
                               update_state == updater::CheckState::UpToDate ||
                               update_state == updater::CheckState::Offline ||
                               update_state == updater::CheckState::Error;
        if (checkable) {
            if (ghost_button("Check for Updates", ImVec2(ui_px(160.0f), ui_px(28.0f)))) {
                st.update_status.state = updater::CheckState::Checking;
                spawn_worker(st, std::thread([&st]() { do_update_check(st); }));
            }
        } else if (update_state == updater::CheckState::Available ||
                   update_state == updater::CheckState::Mandatory) {
            if (primary_button(st.update_status.staged.load() ? "Install & Restart"
                                                              : "Download & Install",
                               ImVec2(ui_px(180.0f), ui_px(28.0f)))) {
                st.update_status.state = updater::CheckState::Checking;
                spawn_worker(st, std::thread([&st]() { do_update_install(st); }));
            }
        }
        if (!st.update_status.notes.empty() &&
            (update_state == updater::CheckState::Available ||
             update_state == updater::CheckState::Mandatory)) {
            ImGui::Spacing();
            ImGui::TextWrapped("%s", st.update_status.notes.c_str());
        }
        card_end();
    }

    ImGui::Spacing();
    if (primary_button("Save settings", ImVec2(ui_px(150.0f), ui_px(36.0f)))) save_ui_config(st);
    ImGui::SameLine();
    if (ghost_button("Reload saved", ImVec2(ui_px(130.0f), ui_px(36.0f)))) {
        config::Config fresh;
        if (config::load(st.exe_dir + L"\\launcher.json", fresh)) {
            *st.cfg = std::move(fresh);
            st.ui_base = net::to_utf8(st.cfg->base_dir);
            st.ui_assets = net::to_utf8(st.cfg->assets_dir);
            st.ui_java_cache = net::to_utf8(st.cfg->java_cache_dir);
            st.ui_username = net::to_utf8(st.cfg->username);
            st.ui_server = net::to_utf8(st.cfg->test_server);
            st.ui_jvm = st.cfg->extra_jvm;
            load_performance_settings(*st.cfg);
            load_social_settings(*st.cfg);
            load_mod_settings(*st.cfg);
            st.settings_dirty = false;
            set_settings_status(st, "Saved settings reloaded");
        } else {
            set_settings_status(st, "Could not reload launcher settings");
        }
    }
    ImGui::EndChild();
}

void draw_instances_tab(UiState& st);

void draw_my_games_tab(UiState& st) {
    draw_instances_tab(st);
}

// Profile content, counts, and storage are all filesystem-backed. Keep the
// render thread limited to copying the last completed result; the actual
// directory scans run on workers so a large modpack never freezes navigation.
std::vector<instances::ContentEntry> cached_instance_content(
    UiState& st, const instances::Instance& inst) {
    const uint64_t now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    constexpr uint64_t kContentTtlMs = 1000;
    bool start_scan = false;
    uint64_t generation = 0;
    std::vector<instances::ContentEntry> cached;
    {
        std::lock_guard<std::mutex> lock(st.detail_cache.mu);
        if (st.detail_cache.directory != inst.directory) {
            st.detail_cache.directory = inst.directory;
            ++st.detail_cache.generation;
            st.detail_cache.content_at_ms = 0;
            st.detail_cache.content.clear();
            st.detail_cache.content_scan_pending = false;
            st.detail_cache.counts_at_ms = 0;
            st.detail_cache.counts_scan_pending = false;
            st.detail_cache.storage_at_ms = 0;
            st.detail_cache.storage_scan_pending = false;
            st.detail_cache.world_sizes.clear();
            st.detail_cache.world_directories.clear();
            st.detail_cache.screenshot_paths.clear();
            st.detail_cache.storage_mods = 0;
            st.detail_cache.storage_worlds = 0;
            st.detail_cache.storage_shots = 0;
            st.detail_cache.storage_total = 0;
        }
        const bool stale = st.detail_cache.content_at_ms == 0 ||
            now - st.detail_cache.content_at_ms > kContentTtlMs;
        if (stale && !st.detail_cache.content_scan_pending) {
            st.detail_cache.content_scan_pending = true;
            generation = st.detail_cache.generation;
            start_scan = true;
        }
        cached = st.detail_cache.content;
    }
    if (start_scan) {
        const std::wstring directory = inst.directory;
        spawn_worker(st, std::thread([&st, directory, generation]() {
            instances::Instance snapshot;
            snapshot.directory = directory;
            std::string error;
            auto content = instances::list_content(snapshot, &error);
            const uint64_t completed_at = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            std::lock_guard<std::mutex> lock(st.detail_cache.mu);
            if (st.detail_cache.directory == directory &&
                st.detail_cache.generation == generation) {
                st.detail_cache.content = std::move(content);
                st.detail_cache.content_at_ms = completed_at;
                st.detail_cache.content_scan_pending = false;
            }
        }));
    }
    return cached;
}

struct InstanceStorageSizes {
    uint64_t mods = 0;
    uint64_t worlds = 0;
    uint64_t shots = 0;
    uint64_t total = 0;
    bool pending = false;
};

struct InstanceCounts {
    int worlds = 0;
    int screenshots = 0;
};

InstanceCounts cached_instance_counts(UiState& st, const instances::Instance& inst) {
    const uint64_t now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    constexpr uint64_t kCountsTtlMs = 1200;
    bool start_scan = false;
    uint64_t generation = 0;
    InstanceCounts out;
    {
        std::lock_guard<std::mutex> lock(st.detail_cache.mu);
        if (st.detail_cache.directory != inst.directory) {
            st.detail_cache.directory = inst.directory;
            ++st.detail_cache.generation;
            st.detail_cache.content_at_ms = 0;
            st.detail_cache.content_scan_pending = false;
            st.detail_cache.storage_at_ms = 0;
            st.detail_cache.storage_scan_pending = false;
            st.detail_cache.world_sizes.clear();
            st.detail_cache.counts_at_ms = 0;
            st.detail_cache.counts_scan_pending = false;
            st.detail_cache.world_directories.clear();
            st.detail_cache.screenshot_paths.clear();
            st.detail_cache.world_count = 0;
            st.detail_cache.screenshot_count = 0;
        }
        const bool stale = st.detail_cache.counts_at_ms == 0 ||
            now - st.detail_cache.counts_at_ms > kCountsTtlMs;
        if (stale && !st.detail_cache.counts_scan_pending) {
            st.detail_cache.counts_scan_pending = true;
            generation = st.detail_cache.generation;
            start_scan = true;
        }
        out = {st.detail_cache.world_count, st.detail_cache.screenshot_count};
    }
    if (start_scan) {
        const std::wstring directory = inst.directory;
        spawn_worker(st, std::thread([&st, directory, generation]() {
            int worlds = 0;
            int screenshots = 0;
            std::vector<std::wstring> world_directories;
            std::vector<std::wstring> screenshot_paths;
            std::error_code ec;
            const std::filesystem::path root(directory);
            const std::filesystem::path saves = root / L"saves";
            if (std::filesystem::exists(saves, ec) && !ec) {
                for (std::filesystem::directory_iterator it(saves, ec), end;
                     !ec && it != end; it.increment(ec)) {
                    std::error_code type_ec;
                    const std::wstring name = it->path().filename().wstring();
                    if (!type_ec && it->is_directory(type_ec) && name.rfind(L".amalgam-") != 0) {
                        ++worlds;
                        world_directories.push_back(it->path().wstring());
                    }
                }
            }
            ec.clear();
            const std::filesystem::path shots = root / L"screenshots";
            if (std::filesystem::exists(shots, ec) && !ec) {
                for (std::filesystem::directory_iterator it(shots, ec), end;
                     !ec && it != end; it.increment(ec)) {
                    std::error_code file_ec;
                    if (!it->is_regular_file(file_ec) || file_ec) continue;
                    const std::wstring ext = it->path().extension().wstring();
                    if (_wcsicmp(ext.c_str(), L".png") == 0 ||
                        _wcsicmp(ext.c_str(), L".jpg") == 0 ||
                        _wcsicmp(ext.c_str(), L".jpeg") == 0) {
                        ++screenshots;
                        screenshot_paths.push_back(it->path().wstring());
                    }
                }
            }
            const uint64_t completed_at = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            std::lock_guard<std::mutex> lock(st.detail_cache.mu);
            if (st.detail_cache.directory == directory &&
                st.detail_cache.generation == generation) {
                st.detail_cache.world_count = worlds;
                st.detail_cache.screenshot_count = screenshots;
                st.detail_cache.world_directories = std::move(world_directories);
                st.detail_cache.screenshot_paths = std::move(screenshot_paths);
                st.detail_cache.counts_at_ms = completed_at;
                st.detail_cache.counts_scan_pending = false;
            }
        }));
    }
    return out;
}

InstanceStorageSizes cached_instance_storage(UiState& st,
                                             const instances::Instance& inst) {
    const uint64_t now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    constexpr uint64_t kStorageTtlMs = 5000;
    bool start_scan = false;
    uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(st.detail_cache.mu);
        if (st.detail_cache.directory != inst.directory) {
            st.detail_cache.directory = inst.directory;
            ++st.detail_cache.generation;
            st.detail_cache.content_at_ms = 0;
            st.detail_cache.content.clear();
            st.detail_cache.content_scan_pending = false;
            st.detail_cache.counts_at_ms = 0;
            st.detail_cache.counts_scan_pending = false;
            st.detail_cache.storage_at_ms = 0;
            st.detail_cache.storage_scan_pending = false;
            st.detail_cache.storage_mods = 0;
            st.detail_cache.storage_worlds = 0;
            st.detail_cache.storage_shots = 0;
            st.detail_cache.storage_total = 0;
            st.detail_cache.world_sizes.clear();
            st.detail_cache.world_directories.clear();
            st.detail_cache.screenshot_paths.clear();
        }
        const bool stale = st.detail_cache.storage_at_ms == 0 ||
                           now - st.detail_cache.storage_at_ms > kStorageTtlMs;
        if (stale && !st.detail_cache.storage_scan_pending) {
            st.detail_cache.storage_scan_pending = true;
            generation = st.detail_cache.generation;
            start_scan = true;
        }
    }

    // Recursive profile sizing is deliberately off the render thread. The
    // old implementation caused visible multi-second stalls on large worlds.
    if (start_scan) {
        const std::wstring directory = inst.directory;
        spawn_worker(st, std::thread([&st, directory, generation]() {
            uint64_t mods_sz = 0, worlds_sz = 0, shots_sz = 0, stored_total = 0;
            auto dir_size = [](const std::filesystem::path& path) -> uint64_t {
                uint64_t size = 0;
                std::error_code ec;
                if (!std::filesystem::exists(path, ec) || ec) return size;
                for (std::filesystem::recursive_directory_iterator it(path, ec), end;
                     !ec && it != end; it.increment(ec)) {
                    std::error_code file_ec;
                    if (it->is_regular_file(file_ec) && !file_ec) {
                        const uintmax_t bytes = it->file_size(file_ec);
                        if (!file_ec) size += static_cast<uint64_t>(bytes);
                    }
                }
                return size;
            };
            const std::filesystem::path root(directory);
            std::unordered_map<std::wstring, uint64_t> world_sizes;
            mods_sz = dir_size(root / L"mods");
            worlds_sz = dir_size(root / L"saves");
            shots_sz = dir_size(root / L"screenshots");
            stored_total = dir_size(root);
            std::error_code saves_ec;
            const std::filesystem::path saves = root / L"saves";
            if (std::filesystem::exists(saves, saves_ec) && !saves_ec) {
                for (std::filesystem::directory_iterator it(saves, saves_ec), end;
                     !saves_ec && it != end; it.increment(saves_ec)) {
                    std::error_code type_ec;
                    const std::wstring name = it->path().filename().wstring();
                    if (!it->is_directory(type_ec) || type_ec || name.rfind(L".amalgam-") == 0)
                        continue;
                    world_sizes.emplace(it->path().wstring(), dir_size(it->path()));
                }
            }

            const uint64_t completed_at = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            std::lock_guard<std::mutex> lock(st.detail_cache.mu);
            if (st.detail_cache.directory == directory &&
                st.detail_cache.generation == generation) {
                st.detail_cache.storage_mods = mods_sz;
                st.detail_cache.storage_worlds = worlds_sz;
                st.detail_cache.storage_shots = shots_sz;
                st.detail_cache.storage_total = stored_total;
                st.detail_cache.world_sizes = std::move(world_sizes);
                st.detail_cache.storage_at_ms = completed_at;
                st.detail_cache.storage_scan_pending = false;
            }
        }));
    }
    InstanceStorageSizes out;
    {
        std::lock_guard<std::mutex> lock(st.detail_cache.mu);
        out.mods = st.detail_cache.storage_mods;
        out.worlds = st.detail_cache.storage_worlds;
        out.shots = st.detail_cache.storage_shots;
        out.total = st.detail_cache.storage_total;
        out.pending = st.detail_cache.storage_scan_pending;
    }
    return out;
}

uint64_t cached_world_size(UiState& st, const std::wstring& world_directory) {
    std::lock_guard<std::mutex> lock(st.detail_cache.mu);
    const auto it = st.detail_cache.world_sizes.find(world_directory);
    return it == st.detail_cache.world_sizes.end() ? 0 : it->second;
}

void invalidate_instance_detail_cache(UiState& st) {
    {
        std::lock_guard<std::mutex> lock(st.detail_cache.mu);
        ++st.detail_cache.generation;
        st.detail_cache.content_at_ms = 0;
        st.detail_cache.content_scan_pending = false;
        st.detail_cache.counts_at_ms = 0;
        st.detail_cache.counts_scan_pending = false;
        st.detail_cache.storage_at_ms = 0;
        st.detail_cache.world_sizes.clear();
        st.detail_cache.world_directories.clear();
        st.detail_cache.screenshot_paths.clear();
    }
    {
        std::lock_guard<std::mutex> lock(st.file_index_mu);
        ++st.library_worlds_scan_generation;
        st.library_worlds_scan_pending = false;
        st.library_worlds_cache_at_ms = 0;
        ++st.screenshots_scan_generation;
        st.screenshots_scan_pending = false;
        st.screenshots_cache_at_ms = 0;
    }
    st.quick_search_index_at_ms = 0;
}

void draw_instance_detail(UiState& st) {
    const instances::Instance& inst = st.selected_instance;
    st.instance_detail_tab = std::clamp(st.instance_detail_tab, 0, 6);

    // ── Breadcrumb ────────────────────────────────────────────────────────
    {
        const float arrow_w = ui_px(16.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, k.muted);
        if (ImGui::Selectable("##breadcrumb_back", false, ImGuiSelectableFlags_None,
                              ImVec2(arrow_w + ImGui::CalcTextSize("Library").x + ui_px(12.0f), ui_px(22.0f))))
            st.instance_detail_open = false;
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 bp = ImGui::GetItemRectMin();
        dl->AddText(bp + ImVec2(0, ui_px(2.0f)), c32(k.muted), "<");
        dl->AddText(bp + ImVec2(arrow_w, 0), c32(k.text), "Library");
        ImVec2 label_end = bp + ImVec2(arrow_w + ImGui::CalcTextSize("Library").x, 0);
        dl->AddText(label_end + ImVec2(ui_px(6.0f), ui_px(2.0f)), c32(k.muted), ">");
        ImVec2 sep_end = label_end + ImVec2(ui_px(24.0f), 0);
        const char* name_ptr = inst.name.c_str();
        ImVec2 name_size = ImGui::CalcTextSize(name_ptr);
        dl->AddText(sep_end + ImVec2(ui_px(6.0f), ui_px(2.0f)), c32(k.brand), name_ptr);
    }
    ImGui::Spacing();

    // ── Hero banner with gradient overlay ─────────────────────────────────
    const float hero_card_width = ImGui::GetContentRegionAvail().x;
    card_begin("##instancehero", ImVec2(hero_card_width, 0));
    ImVec2 banner_pos = ImGui::GetCursorScreenPos();
    ImVec2 banner_avail = ImGui::GetContentRegionAvail();
    ImVec2 banner_size(banner_avail.x, ui_px(84.0f));
    draw_instance_art(st, inst, banner_pos, banner_size, c32(k.brand_dk));
    // dark gradient overlay at bottom
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImVec2 grad_top = banner_pos + ImVec2(0, banner_size.y * 0.55f);
    dl->AddRectFilledMultiColor(banner_pos + ImVec2(0, banner_size.y * 0.55f),
                                banner_pos + banner_size,
                                IM_COL32(0,0,0,0), IM_COL32(0,0,0,0),
                                IM_COL32(0,0,0,200), IM_COL32(0,0,0,200));
    ImGui::Dummy(ImVec2(0, banner_size.y));

    // ── Profile info overlay (below banner) ───────────────────────────────
    const char* mc_ver = inst.minecraft_version.empty() ? "unconfigured" : inst.minecraft_version.c_str();
    const char* loader_name = inst.loader.empty() || inst.loader == "auto" ? "vanilla" : inst.loader.c_str();
    auto issues = profile_health(st, inst);
    bool healthy = issues.empty();

    // Count content (cached; list_content scans five content directories)
    int mod_count = 0;
    {
        const auto all_content = cached_instance_content(st, inst);
        for (const auto& e : all_content) {
            if (e.type == instances::ContentType::Mod) ++mod_count;
        }
    }
    const InstanceCounts detail_counts = cached_instance_counts(st, inst);
    const InstanceStorageSizes detail_storage = cached_instance_storage(st, inst);

    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted(inst.name.c_str());
    ImGui::PopFont();

    // Status badges row
    auto badge = [&](const char* text, ImU32 bg) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImVec2 sz = ImGui::CalcTextSize(text) + ImVec2(ui_px(12.0f), ui_px(4.0f));
        ImGui::GetWindowDrawList()->AddRectFilled(p, p + sz, bg, ui_px(4.0f));
        ImGui::GetWindowDrawList()->AddText(p + ImVec2(ui_px(6.0f), ui_px(2.0f)), c32(k.text), text);
        ImGui::Dummy(sz);
        ImGui::SameLine(0, ui_px(4.0f));
    };
    badge(mc_ver, c32(k.surface));
    badge(loader_name, c32(k.surface));
    if (mod_count > 0)
        badge((std::to_string(mod_count) + (mod_count == 1 ? " Mod" : " Mods")).c_str(), c32(k.surface));
    badge(inst.pack_source.empty() ? "Custom" : (inst.pack_modified ? "Modified" : "Published"),
          inst.pack_source.empty() ? c32(k.surface) : (inst.pack_modified ? c32(k.yellow) : c32(k.green)));
    badge(healthy ? "Ready" : "Issues", healthy ? c32(k.green) : c32(k.yellow));

    ImGui::Spacing();

    // ── Play controls row ─────────────────────────────────────────────────
    const float play_w = ui_px(130.0f);
    const float btn_h = ui_px(32.0f);
    const float quick_w = ui_px(108.0f);
    const float dots_w = ui_px(32.0f);

    // Put the action cluster on its own row so long profile metadata never
    // collides with Play or Add Content at narrower widths.
    float cluster_w = play_w + ui_px(8.0f) + quick_w + ui_px(8.0f) + dots_w;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(),
                                  ImGui::GetContentRegionAvail().x - cluster_w));

    if (primary_button(st.running ? "Running" : "Play", ImVec2(play_w, btn_h)) && !st.running &&
        !inst.minecraft_version.empty()) {
        st.selected = inst.minecraft_version;
        st.active_instance_dir = inst.directory;
        st.pending_instance_dir = inst.directory;
        st.pending_id = st.selected;
        st.pending_launch = true;
    }
    ImGui::SameLine(0, ui_px(8.0f));
    if (ghost_button("+ Add Content", ImVec2(quick_w, btn_h))) {
        st.active_instance_dir = inst.directory;
        st.mod_loader = inst.loader == "auto" ? "" : inst.loader;
        st.mod_version = inst.minecraft_version;
        st.mod_query.clear();
        st.project_detail_open = false;
        st.active_tab = 1;
        st.sidebar_item = 2;
        st.browse_category = 0;
    }
    ImGui::SameLine(0, ui_px(8.0f));
    if (ghost_button("...", ImVec2(dots_w, btn_h)))
        ImGui::OpenPopup("##instance_hero_more");
    
    // Essentials host/invite actions live on the Essentials page so the
    // compact profile hero does not overflow horizontally.
    draw_instance_overflow_menu(st, st.selected_instance, "##instance_hero_more");
    card_end();
    ImGui::SetCursorPosX(0.0f);
    ImGui::Spacing();

    // ── Tab bar with thin underline ───────────────────────────────────────
    struct InstanceTab { const char* label; int value; int counter; };
    // Counters come from the short-lived profile index above; no directory
    // enumeration occurs in the tab layout pass.
    const int world_count = detail_counts.worlds;
    const int screenshot_count = detail_counts.screenshots;
    InstanceTab tabs[] = {
        {"Overview", 0, 0}, {"Content", 1, mod_count}, {"Versions", 4, 0},
        {"Worlds", 2, world_count}, {"Screenshots", 3, screenshot_count},
        {"Logs", 5, 0}, {"Settings", 6, 0},
        // AI tab only shown for AI Profiles
        {"AI", 7, 0},
    };
    const float tab_gap = ui_px(4.0f);
    for (const auto& tab : tabs) {
        ImGui::SameLine(0, tab_gap);
        bool active = st.instance_detail_tab == tab.value;
        ImVec4 col = active ? k.brand : k.muted;
        std::string label = tab.label;
        if (tab.counter > 0) label += " " + std::to_string(tab.counter);
        ImVec2 ts = ImGui::CalcTextSize(label.c_str());
        ImVec2 btn_min = ImGui::GetCursorScreenPos();
        ImVec2 btn_size(ts.x + ui_px(14.0f), ui_px(26.0f));
        ImGui::InvisibleButton(("##tab_" + std::to_string(tab.value)).c_str(), btn_size);
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImDrawList* tdl = ImGui::GetWindowDrawList();
        tdl->AddText(btn_min + ImVec2(ui_px(7.0f), ui_px(3.0f)), c32(col), label.c_str());
        if (active) {
            tdl->AddRectFilled(btn_min + ImVec2(0, btn_size.y - ui_px(2.0f)),
                               btn_min + btn_size - ImVec2(0, 0), c32(k.brand), ui_px(1.0f));
        }
        if (ImGui::IsItemClicked()) st.instance_detail_tab = tab.value;
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── Tab content ───────────────────────────────────────────────────────
    card_begin("##instancedetailcontent", ImVec2(-1, -1));

    if (st.instance_detail_tab == 0) {
        // ─── OVERVIEW DASHBOARD ───────────────────────────────────────────
        const float avail = ImGui::GetContentRegionAvail().x;
        const bool two_col = avail > ui_px(500.0f);
        const float col1 = two_col ? avail * 0.55f : avail;

        // LEFT COLUMN
        if (two_col) {
            ImGui::Columns(2, "##ov_columns", false);
            ImGui::SetColumnWidth(0, col1);
        }
        // Profile Health - Visual check items
        card_begin("##health_card", ImVec2(-1, 0));
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("PROFILE HEALTH");
        ImGui::PopFont();
        if (healthy) {
            ImGui::TextColored(k.green, "Ready to Play");
        } else {
            ImGui::TextColored(k.yellow, "Attention Required — %d issue%s",
                               (int)issues.size(), issues.size() == 1 ? "" : "s");
        }
        ImGui::Spacing();
        // Visual health checks
        auto health_item = [&](const char* label, bool ok, const char* detail) {
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            // Status icon
            if (ok) {
                dl->AddCircleFilled(p + ImVec2(ui_px(8.0f), ui_px(8.0f)), ui_px(6.0f), c32(k.green));
                dl->AddText(f_small, f_small->LegacySize,
                            ImVec2(p.x + ui_px(5.0f), p.y + ui_px(3.0f)),
                            c32(k.text), "\xe2\x9c\x93");  // checkmark
            } else {
                dl->AddCircleFilled(p + ImVec2(ui_px(8.0f), ui_px(8.0f)), ui_px(6.0f), c32(k.red));
                dl->AddText(f_small, f_small->LegacySize,
                            ImVec2(p.x + ui_px(5.0f), p.y + ui_px(3.0f)),
                            c32(k.text), "!");
            }
            ImGui::Dummy(ImVec2(ui_px(18.0f), ui_px(16.0f)));
            ImGui::SameLine(0, ui_px(2.0f));
            ImGui::PushFont(f_small);
            ImGui::TextColored(ok ? k.green : k.red, "%s", label);
            if (detail) {
                ImGui::SameLine(ui_px(140.0f));
                ImGui::TextColored(ok ? k.green : k.yellow, "%s", detail);
            }
            ImGui::PopFont();
        };
        health_item("Minecraft", !inst.minecraft_version.empty(), mc_ver);
        health_item("Loader", !inst.loader.empty() && inst.loader != "auto", loader_name);
        health_item("Dependencies", issues.empty(), issues.empty() ? "OK" : issues.front().c_str());
        health_item("Game Files", true, (std::to_string(mod_count) + " mods installed").c_str());
        if (!healthy) {
            ImGui::Spacing();
            if (primary_button("Run Health Check", ImVec2(ui_px(160.0f), ui_px(32.0f))) && !st.running) {
                config::Config snapshot = *st.cfg;
                instances::Instance target = inst;
                spawn_worker(st, std::thread([&st, snapshot, target]() {
                    std::wstring rp; std::string be;
                    if (!instances::create_restore_point(target, &rp, &be)) {
                        log_line(st, L"[profile] repair cancelled: " + net::to_wide(be)); return;
                    }
                    log_line(st, L"[profile] restore point: " + rp);
                    do_launch(st, target.minecraft_version, snapshot, target.directory, true);
                }));
            }
        }
        card_end();
        ImGui::Spacing();

        // Profile Information
        card_begin("##info_card", ImVec2(-1, 0));
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("PROFILE INFORMATION");
        ImGui::PopFont();
        // Profile size is refreshed asynchronously; opening a large world
        // should never block the first interactive frame.
        const uint64_t total = detail_storage.total;
        const float scw = (ImGui::GetContentRegionAvail().x - ui_px(10.0f)) * 0.5f;
        auto stat_row = [&](const char* l1, const char* v1, const ImVec4& a1,
                            const char* l2, const char* v2, const ImVec4& a2) {
            draw_stat_card(l1, v1, -1.0f, a1, scw);
            ImGui::SameLine(0, ui_px(10.0f));
            draw_stat_card(l2, v2, -1.0f, a2, scw);
        };
        std::string loader_label = loader_name;
        if (!inst.loader_version.empty()) loader_label += " " + inst.loader_version;
        stat_row("MINECRAFT", mc_ver, k.brand,
                 "LOADER", loader_label.c_str(), k.brand);
        stat_row("MODS", (std::to_string(mod_count) + " installed").c_str(), k.brand,
                 "WORLDS", (std::to_string(world_count) + " saved").c_str(), k.brand);
        stat_row("SCREENSHOTS", (std::to_string(screenshot_count) + " captured").c_str(), k.brand,
                 "PROFILE SIZE", detail_storage.pending ? "Calculating..." : format_bytes(total).c_str(), k.brand);
        if (inst.last_played > 0) {
            stat_row("LAST PLAYED", profile_activity_label(inst).c_str(), k.muted,
                     "STATUS", healthy ? "HEALTHY" : "NEEDS ATTENTION",
                     healthy ? k.green : k.orange);
        }
        card_end();
        ImGui::Spacing();

        // Recent Activity
        card_begin("##activity_card", ImVec2(-1, 0));
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("RECENT ACTIVITY");
        ImGui::PopFont();
        if (inst.last_played > 0) {
            ImGui::TextColored(k.text, "Last session");
            ImGui::TextColored(k.muted, "%s", profile_activity_label(inst).c_str());
        } else {
            ImGui::TextColored(k.muted, "No activity recorded yet.");
        }
        card_end();

        if (two_col) {
            ImGui::NextColumn();
        }

        // RIGHT COLUMN
        // Updates
        card_begin("##updates_card", ImVec2(-1, 0));
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("UPDATES");
        ImGui::PopFont();
        mods::UpdateInfo pack_update;
        std::string pack_update_error;
        std::wstring pack_update_directory;
        {
            std::lock_guard<std::mutex> lock(st.pack_update_mu);
            pack_update = st.pack_update;
            pack_update_error = st.pack_update_error;
            pack_update_directory = st.pack_update_directory;
            if (pack_update_directory != inst.directory) {
                pack_update = {};
                pack_update_error.clear();
                pack_update_directory.clear();
            }
        }
        const bool has_pack_metadata = !inst.pack_source.empty() && !inst.pack_project.empty();
        const bool pack_check_succeeded = has_pack_metadata && !st.pack_update_checking.load() &&
            !pack_update_directory.empty() && pack_update_directory == inst.directory &&
            pack_update_error.empty();
        if (!has_pack_metadata) {
            ImGui::TextColored(k.muted, "Update status unavailable for this custom profile");
        } else if (st.pack_update_checking.load()) {
            ImGui::TextColored(k.muted, "Checking for updates...");
        } else if (!pack_update_error.empty()) {
            ImGui::TextColored(k.red, "Update check failed: %s", pack_update_error.c_str());
        } else if (pack_check_succeeded && pack_update.available) {
            ImGui::TextColored(k.yellow, "Update available: %s", pack_update.latest_version.c_str());
            ImGui::Spacing();
            const bool create_copy = inst.pack_modified;
            if (create_copy) ImGui::TextColored(k.yellow, "Profile modified; will create updated copy.");
            const char* lbl = st.mod_installing ? "Updating..." :
                (create_copy ? "Create updated copy" : "Update Now");
            if (primary_button(lbl, ImVec2(ui_px(150.0f), ui_px(32.0f))) && !st.mod_installing) {
                st.pack_update_confirm_target = inst;
                st.pack_update_confirm_copy = create_copy;
                st.pack_update_confirm_version = pack_update.latest_version;
                st.pack_update_confirm_open = true;
                ImGui::OpenPopup("Confirm creator update");
            }
        } else if (pack_check_succeeded) {
            ImGui::TextColored(k.green, "Everything is up to date");
        } else {
            ImGui::TextColored(k.muted, "Not checked");
        }
        if (has_pack_metadata && !pack_update.available && !st.pack_update_checking.load()) {
            ImGui::SameLine();
            if (ghost_button("Check", ImVec2(ui_px(60.0f), ui_px(28.0f)))) {
                config::Config snap = *st.cfg;
                instances::Instance tgt = inst;
                spawn_worker(st, std::thread([&st, snap, tgt]() { do_pack_update_check(st, snap, tgt); }));
            }
        }
        card_end();
        ImGui::Spacing();

        // Storage
        card_begin("##storage_card", ImVec2(-1, 0));
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("STORAGE");
        ImGui::PopFont();
        {
            const InstanceStorageSizes storage = cached_instance_storage(st, inst);
            const uint64_t mods_sz = storage.mods;
            const uint64_t worlds_sz = storage.worlds;
            const uint64_t shots_sz = storage.shots;
            const uint64_t stored_total = storage.total;
            const uint64_t other_sz = stored_total > mods_sz + worlds_sz + shots_sz
                                          ? stored_total - mods_sz - worlds_sz - shots_sz : 0;
            ImGui::TextColored(k.text, "%s total", format_bytes(stored_total).c_str());
            ImGui::Spacing();
            // For very small profiles, use a compact text summary instead of misleading bars
            const bool tiny = stored_total < 1024 * 1024; // < 1 MB
            if (tiny) {
                ImGui::PushFont(f_small);
                if (mods_sz > 0) ImGui::TextColored(k.muted, "Mods: %s", format_bytes(mods_sz).c_str());
                if (worlds_sz > 0) ImGui::TextColored(k.muted, "Worlds: %s", format_bytes(worlds_sz).c_str());
                if (shots_sz > 0) ImGui::TextColored(k.muted, "Screenshots: %s", format_bytes(shots_sz).c_str());
                if (other_sz > 0) ImGui::TextColored(k.muted, "Other: %s", format_bytes(other_sz).c_str());
                ImGui::PopFont();
            } else {
            auto bar = [&](const char* label, uint64_t sz) {
                if (sz == 0 && stored_total == 0) return;
                float frac = stored_total > 0 ? static_cast<float>(sz) / static_cast<float>(stored_total) : 0;
                ImVec2 p = ImGui::GetCursorScreenPos();
                float bar_w = ImGui::GetContentRegionAvail().x - ui_px(100.0f);
                dl = ImGui::GetWindowDrawList();
                dl->AddRectFilled(p, p + ImVec2(bar_w, ui_px(8.0f)), c32(k.surface), ui_px(3.0f));
                dl->AddRectFilled(p, p + ImVec2(bar_w * frac, ui_px(8.0f)), c32(k.brand), ui_px(3.0f));
                ImGui::Dummy(ImVec2(0, ui_px(12.0f)));
                ImGui::SameLine();
                ImGui::TextColored(k.muted, "%-16s %s", label, format_bytes(sz).c_str());
            };
            bar("Mods", mods_sz);
            bar("Worlds", worlds_sz);
            bar("Screenshots", shots_sz);
            bar("Other", other_sz);
            } // !tiny
        }
        if (ghost_button("Open Folder", ImVec2(ui_px(120.0f), ui_px(28.0f))))
            ShellExecuteW(st.hwnd, L"open", inst.directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        card_end();

        if (two_col) ImGui::Columns(1);

    } else if (st.instance_detail_tab == 1) {
        // ─── CONTENT ──────────────────────────────────────────────────────
        const bool narrow = ImGui::GetContentRegionAvail().x < ui_px(700.0f);
        // Sub-tabs: All, Mods, Resource Packs, Shaders
        static const char* content_tabs[] = {"All", "Mods", "Resource Packs", "Shaders"};
        int content_counts[4] = {0, 0, 0, 0};
        const auto all_content = cached_instance_content(st, inst);
        for (const auto& e : all_content) {
            content_counts[0]++;
            if (e.type == instances::ContentType::Mod) content_counts[1]++;
            else if (e.type == instances::ContentType::ResourcePack) content_counts[2]++;
            else if (e.type == instances::ContentType::Shader) content_counts[3]++;
        }
        for (int i = 0; i < 4; ++i) {
            if (i > 0) ImGui::SameLine(0, ui_px(4.0f));
            std::string lbl = std::string(content_tabs[i]) + " " + std::to_string(content_counts[i]);
            bool active = st.content_filter == i;
            if (active) ImGui::PushStyleColor(ImGuiCol_Button, k.brand);
            else        ImGui::PushStyleColor(ImGuiCol_Button, k.surface);
            if (ImGui::Button(lbl.c_str())) st.content_filter = i;
            ImGui::PopStyleColor();
        }
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - ui_px(140.0f));
        if (primary_button("+ Add Content", ImVec2(ui_px(140.0f), ui_px(32.0f)))) {
            st.active_instance_dir = inst.directory;
            st.mod_loader = inst.loader == "auto" ? "" : inst.loader;
            st.mod_version = inst.minecraft_version;
            st.mod_query.clear();
            st.project_detail_open = false;
            st.active_tab = 1;
            st.sidebar_item = 2;
            st.browse_category = 0;
        }
        ImGui::Spacing();
        // Search + filter
        ImGui::SetNextItemWidth(narrow ? -1.0f : ui_px(250.0f));
        input_text("Search installed content", &st.content_search);
        if (!narrow) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ui_px(120.0f));
            static const char* sort_items[] = {"Name\0Recently Installed\0Recently Updated\0Provider\0"};
            static int content_sort = 0;
            ImGui::Combo("##csort", &content_sort, sort_items[0]);
        }
        ImGui::Spacing();

        // Update check row
        if (ghost_button(st.owned_update_checking ? "Checking..." : "Check updates", ImVec2(ui_px(130.0f), ui_px(28.0f))) &&
            !st.mod_installing && !st.owned_update_checking) {
            instances::Instance target = inst;
            config::Config snap = *st.cfg;
            spawn_worker(st, std::thread([&st, target, snap]() { do_owned_update_check(st, snap, target); }));
        }
        ImGui::SameLine();
        if (ghost_button("Upload local", ImVec2(ui_px(110.0f), ui_px(28.0f)))) {
            std::string path;
            if (show_open_content(st, path)) {
                instances::ContentType type = instances::ContentType::Mod;
                if (st.content_filter > 0) type = static_cast<instances::ContentType>(st.content_filter - 1);
                instances::ContentEntry imported;
                std::string error;
                if (instances::import_content(inst, net::to_wide(path), type, &imported, &error)) {
                    mark_profile_modified(st, st.selected_instance);
                    st.content_status = "Imported " + imported.filename;
                    invalidate_instance_detail_cache(st);
                } else st.content_status = error;
            }
        }

        // Owned updates preview
        std::vector<mods::UpdateEntry> owned_updates;
        {
            std::lock_guard<std::mutex> lock(st.owned_update_mu);
            owned_updates = st.owned_updates;
            if (st.owned_update_directory != inst.directory) owned_updates.clear();
        }
        if (!st.owned_update_checking && !owned_updates.empty()) {
            ImGui::Spacing();
            card_begin("##updp", ImVec2(-1, 0));
            ImGui::TextColored(k.yellow, "%d update(s) available", (int)owned_updates.size());
            for (size_t ui = 0; ui < owned_updates.size(); ++ui) {
                const auto& u = owned_updates[ui];
                bool sel = false;
                { std::lock_guard<std::mutex> lock(st.owned_update_mu);
                  sel = st.owned_update_selected.find(u.file) != st.owned_update_selected.end(); }
                ImGui::PushID((int)ui);
                if (ImGui::Checkbox("##usel", &sel)) {
                    std::lock_guard<std::mutex> lock(st.owned_update_mu);
                    if (sel) st.owned_update_selected.insert(u.file);
                    else     st.owned_update_selected.erase(u.file);
                }
                ImGui::SameLine();
                ImGui::Text("%s -> %s (%s)", u.file.c_str(),
                            u.latest_file.empty() ? u.file.c_str() : u.latest_file.c_str(),
                            u.latest_version.c_str());
                ImGui::PopID();
            }
            size_t sc = 0;
            { std::lock_guard<std::mutex> lock(st.owned_update_mu); sc = st.owned_update_selected.size(); }
            if (primary_button(st.mod_installing ? "Updating..." : "Apply Selected", ImVec2(ui_px(150.0f), ui_px(32.0f))) &&
                sc > 0 && !st.mod_installing) {
                instances::Instance tgt = inst;
                config::Config snap = *st.cfg;
                spawn_worker(st, std::thread([&st, tgt, snap]() { do_profile_update(st, snap, tgt); }));
                std::lock_guard<std::mutex> lock(st.owned_update_mu);
                st.owned_updates.clear();
                st.owned_update_selected.clear();
            }
            card_end();
        }

        ImGui::Spacing();
        // Content list
        int files = 0;
        for (const auto& entry : all_content) {
            int type_filter = st.content_filter - 1;
            if (type_filter >= 0 && static_cast<int>(entry.type) != type_filter) continue;
            if (!st.content_search.empty() && entry.filename.find(st.content_search) == std::string::npos) continue;
            ++files;
            const bool compact = narrow;
            card_begin((std::string("##ci") + std::to_string(files)).c_str(),
                       ImVec2(-1, compact ? ui_px(80.0f) : ui_px(52.0f)));
            ImGui::PushFont(f_bold);
            ImGui::TextUnformatted(entry.filename.c_str());
            ImGui::PopFont();
            if (!compact) {
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - ui_px(330));
                ImGui::TextColored(k.muted, "%s  %.1f KB  %s", instances::content_type_name(entry.type),
                                   entry.size / 1024.0, entry.enabled ? "Enabled" : "Disabled");
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - ui_px(160));
                ImGui::TextColored(entry.managed ? k.green : k.muted, "%s",
                                   entry.managed ? (entry.owner_source.empty() ? "Managed" : entry.owner_source.c_str()) : "Local");
            } else {
                ImGui::TextColored(k.muted, "%s  %.1f KB  %s  %s", instances::content_type_name(entry.type),
                                   entry.size / 1024.0, entry.enabled ? "Enabled" : "Disabled",
                                   entry.managed ? "Managed" : "Local");
            }
            if (ghost_button(entry.enabled ? "Disable" : "Enable", ImVec2(ui_px(76.0f), ui_px(26.0f)))) {
                std::string err;
                if (!instances::set_content_enabled(entry, !entry.enabled, &err)) st.content_status = err;
                else {
                    mark_profile_modified(st, st.selected_instance);
                    invalidate_instance_detail_cache(st);
                }
            }
            ImGui::SameLine();
            if (ghost_button("Remove", ImVec2(ui_px(72.0f), ui_px(26.0f)))) {
                std::wstring rp; std::string err;
                if (!instances::move_content_to_trash(inst, entry, &rp, &err)) st.content_status = err;
                else {
                    mark_profile_modified(st, st.selected_instance);
                    st.content_status = "Removed to recovery";
                    invalidate_instance_detail_cache(st);
                }
            }
            card_end();
        }
        if (files == 0) {
            ImGui::TextColored(k.muted, "No content matches your filters.");
        }
        if (!st.content_status.empty()) ImGui::TextColored(k.yellow, "%s", st.content_status.c_str());

    } else if (st.instance_detail_tab == 2) {
        // ─── WORLDS ───────────────────────────────────────────────────────
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("Worlds");
        ImGui::PopFont();
        std::filesystem::path saves = std::filesystem::path(inst.directory) / L"saves";
        std::error_code ec;
        int count = 0;
        std::vector<std::wstring> world_directories;
        {
            std::lock_guard<std::mutex> lock(st.detail_cache.mu);
            world_directories = st.detail_cache.world_directories;
        }
        if (true) {
            for (const auto& world_directory : world_directories) {
                const std::filesystem::path w(world_directory);
                std::wstring fn = w.filename().wstring();
                if (fn.rfind(L".amalgam-", 0) == 0) continue;
                ++count;
                std::string name = net::to_utf8(fn);
                const bool compact = ImGui::GetContentRegionAvail().x < ui_px(720.0f);
                card_begin((std::string("##w") + std::to_string(count)).c_str(),
                           ImVec2(-1, compact ? ui_px(90.0f) : ui_px(56.0f)));
                const std::filesystem::path icon = w / L"icon.png";
                if (std::filesystem::exists(icon, ec) && !ec) {
                    draw_local_image(st, icon.wstring(), ImGui::GetCursorScreenPos(),
                                     ImVec2(ui_px(38.0f), ui_px(38.0f)), c32(k.brand_dk));
                    ImGui::Dummy(ImVec2(ui_px(42.0f), 0));
                    ImGui::SameLine(0, ui_px(4.0f));
                }
                ImGui::PushFont(f_bold);
                ImGui::TextUnformatted(name.c_str());
                ImGui::PopFont();
                // World sizes are produced by the same background profile
                // scan as the storage card; never recurse through a world
                // while the overview is drawing.
                if (!compact) {
                    const uint64_t wsz = cached_world_size(st, w.wstring());
                    ImGui::SameLine();
                    ImGui::TextColored(k.muted, "  %s",
                                       wsz == 0 ? "Calculating..." : format_bytes(wsz).c_str());
                }
                if (!compact) ImGui::SameLine(ImGui::GetContentRegionAvail().x - ui_px(240));
                if (ghost_button("Open", ImVec2(ui_px(64.0f), ui_px(26.0f))))
                    ShellExecuteW(st.hwnd, L"open", w.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                ImGui::SameLine();
                if (ghost_button("Backup", ImVec2(ui_px(74.0f), ui_px(26.0f)))) {
                    std::wstring bp; std::string be;
                    if (instances::backup_world(inst, w.wstring(), &bp, &be))
                        st.content_status = "World backup created";
                    else st.content_status = be;
                }
                ImGui::SameLine();
                if (ghost_button("Delete", ImVec2(ui_px(68.0f), ui_px(26.0f)))) {
                    std::wstring rp; std::string re;
                    if (instances::move_world_to_trash(inst, w.wstring(), &rp, &re)) {
                        st.content_status = "World moved to recovery";
                        invalidate_instance_detail_cache(st);
                        card_end(); break;
                    } else st.content_status = re;
                }
                card_end();
            }
        }
        ImGui::TextColored(k.muted, "%d world(s)", count);
        ImGui::SameLine();
        if (ghost_button("Open saves folder", ImVec2(ui_px(150.0f), ui_px(28.0f))))
            ShellExecuteW(st.hwnd, L"open", saves.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);

    } else if (st.instance_detail_tab == 3) {
        // ─── SCREENSHOTS ──────────────────────────────────────────────────
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("Screenshots");
        ImGui::PopFont();
        std::filesystem::path shots = std::filesystem::path(inst.directory) / L"screenshots";
        std::vector<std::filesystem::path> shot_files;
        {
            std::lock_guard<std::mutex> lock(st.detail_cache.mu);
            for (const auto& path : st.detail_cache.screenshot_paths)
                shot_files.emplace_back(path);
        }
        std::sort(shot_files.begin(), shot_files.end());
        if (shot_files.empty()) {
            illustrated_empty_state(IconId::Image, "NO SCREENSHOTS YET",
                "Screenshots taken in this profile will appear here as a gallery.");
        } else {
            const float gap = ui_px(10.0f);
            const float avail = std::max(0.0f, ImGui::GetContentRegionAvail().x);
            const int cols = avail >= ui_px(900.0f) ? 4 : avail >= ui_px(620.0f) ? 3 :
                             avail >= ui_px(400.0f) ? 2 : 1;
            const float cw = std::max(ui_px(150.0f), (avail - gap * (cols - 1)) / cols);
            const float thumb_h = cw * 0.62f;
            int shown = 0;
            for (const auto& f : shot_files) {
                if (shown % cols) ImGui::SameLine(0, gap);
                ++shown;
                ImGui::PushID(static_cast<int>(shown));
                card_begin("##shot_card", ImVec2(cw, thumb_h + ui_px(58.0f)));
                const ImVec2 img_pos = ImGui::GetCursorScreenPos();
                draw_local_image(st, f.wstring(), img_pos,
                                 ImVec2(cw - ui_px(2.0f), thumb_h), c32(k.surface2));
                ImGui::InvisibleButton("##shot_open", ImVec2(cw - ui_px(2.0f), thumb_h));
                if (ImGui::IsItemHovered())
                    ImGui::GetWindowDrawList()->AddRect(img_pos,
                        img_pos + ImVec2(cw - ui_px(2.0f), thumb_h),
                        c32(k.brand_hov), ui_px(8.0f), 0, ui_px(1.5f));
                if (ImGui::IsItemClicked())
                    ShellExecuteW(st.hwnd, L"open", f.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                ImGui::PushFont(f_small);
                ImGui::TextUnformatted(
                    elide_to_width(f.filename().string(), cw - ui_px(12.0f)).c_str());
                ImGui::PopFont();
                if (ghost_button("Open", ImVec2((cw - ui_px(6.0f)) * 0.5f, ui_px(24.0f))))
                    ShellExecuteW(st.hwnd, L"open", f.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                ImGui::SameLine(0, ui_px(6.0f));
                if (ghost_button("Delete", ImVec2((cw - ui_px(6.0f)) * 0.5f, ui_px(24.0f)))) {
                    std::wstring rp; std::string re;
                    if (instances::move_screenshot_to_trash(inst, f.wstring(), &rp, &re)) {
                        st.content_status = "Screenshot moved to recovery";
                        invalidate_instance_detail_cache(st);
                        card_end(); ImGui::PopID(); break;
                    } else st.content_status = re;
                }
                card_end();
                ImGui::PopID();
            }
            ImGui::Spacing();
            ImGui::TextColored(k.muted, "%d screenshot(s)", (int)shot_files.size());
            ImGui::SameLine();
        }
        if (ghost_button("Open folder", ImVec2(ui_px(120.0f), ui_px(28.0f))))
            ShellExecuteW(st.hwnd, L"open", shots.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);

    } else if (st.instance_detail_tab == 4) {
        // ─── VERSIONS ─────────────────────────────────────────────────────
        page_title("Versions", "Manage the Minecraft version and mod loader for this profile.");
        card_begin("##ver_mc", ImVec2(-1, 0));
        ImGui::PushFont(f_bold);
        ImGui::TextUnformatted("MINECRAFT");
        ImGui::PopFont();
        ImGui::TextColored(k.muted, "Current:");
        ImGui::SameLine(ui_px(80.0f));
        ImGui::PushFont(f_bold);
        ImGui::Text("%s", mc_ver);
        ImGui::PopFont();
        ImGui::Spacing();
        ImGui::TextColored(k.muted,
                           "Changing the Minecraft version rebuilds the profile against a new game version.");
        if (ghost_button("Change Version", ImVec2(ui_px(150.0f), ui_px(30.0f)))) {
            st.version_change_kind = 1;
            st.version_change_backup = true;
            ImGui::OpenPopup("##version_change_modal");
        }
        card_end();
        ImGui::Spacing();
        card_begin("##ver_loader", ImVec2(-1, 0));
        ImGui::PushFont(f_bold);
        ImGui::TextUnformatted("MOD LOADER");
        ImGui::PopFont();
        ImGui::PushFont(f_bold);
        ImGui::Text("%s", loader_name);
        ImGui::PopFont();
        if (!inst.loader_version.empty()) {
            ImGui::TextColored(k.muted, "Current: %s", inst.loader_version.c_str());
        }
        ImGui::Spacing();
        ImGui::TextColored(k.muted,
                           "Switching loaders may leave installed mods incompatible until they are updated.");
        if (ghost_button("Change Loader", ImVec2(ui_px(150.0f), ui_px(30.0f)))) {
            st.version_change_kind = 2;
            st.version_change_backup = true;
            ImGui::OpenPopup("##version_change_modal");
        }
        card_end();
        ImGui::Spacing();
        card_begin("##ver_mem", ImVec2(-1, 0));
        ImGui::PushFont(f_bold);
        ImGui::TextUnformatted("MEMORY");
        ImGui::PopFont();
        ImGui::Text("%d MB allocated", inst.memory_mb);
        if (inst.memory_mb == 0) ImGui::TextColored(k.muted, "Using performance profile recommendation");
        card_end();

        // Backup-before-change safeguard
        set_next_adaptive_window(440.0f, 0.0f, 340.0f, 0.0f);
        bool version_change_open = true;
        if (ImGui::BeginPopupModal("Change Version##version_change_modal", &version_change_open,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            const bool changing_version = st.version_change_kind == 1;
            ImGui::PushFont(f_h2);
            ImGui::TextUnformatted(changing_version ? "CHANGE MINECRAFT VERSION" : "CHANGE MOD LOADER");
            ImGui::PopFont();
            ImGui::TextColored(k.muted,
                               changing_version
                                   ? "This will rebuild your profile against a new game version. Some mods and worlds may be incompatible until updated."
                                   : "Switching mod loaders changes how your mods are loaded. Installed mods built for the current loader will not work until re-installed.");
            ImGui::Spacing();
            ImGui::Checkbox("Create a restore point first", &st.version_change_backup);
            ImGui::TextColored(k.muted,
                               st.version_change_backup
                                   ? "Your current profile configuration and mods are saved so you can restore them later."
                                   : "Your profile will be changed without a backup. Restore points cannot recover this change.");
            ImGui::Spacing();
            bool confirmed = false;
            if (primary_button("Continue", ImVec2(ui_px(140.0f), ui_px(32.0f)))) confirmed = true;
            ImGui::SameLine();
            if (ghost_button("Cancel", ImVec2(ui_px(110.0f), ui_px(32.0f)))) {
                st.version_change_kind = 0;
                ImGui::CloseCurrentPopup();
            }
            if (confirmed) {
                if (st.version_change_backup) {
                    std::string backup_error;
                    std::wstring backup_path;
                    if (!instances::create_restore_point(inst, &backup_path, &backup_error)) {
                        push_notice(st, ui_model::NoticeLevel::Error, "Backup failed",
                                    backup_error.empty() ? "Could not create a restore point." : backup_error);
                    } else {
                        push_notice(st, ui_model::NoticeLevel::Success, "Restore point created",
                                    "Your profile was backed up before the change.");
                    }
                }
                st.version_change_kind = 0;
                st.wizard_open = true;
                st.wizard_step = 0;
                st.wizard_source = 0;
                st.wizard_name = inst.name.empty() ? inst.id : inst.name;
                st.wizard_version = inst.minecraft_version;
                st.wizard_loader = inst.loader == "auto" ? "fabric" : inst.loader;
                st.wizard_project.clear();
                st.wizard_prompt.clear();
                set_pack_summary(st, {});
                st.pack_plan.clear();
                st.wizard_memory = inst.memory_mb;
                st.wizard_performance_profile = inst.performance_profile;
                st.wizard_java.clear();
                st.sidebar_item = 3;
                st.active_tab = 6;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        if (!version_change_open) st.version_change_kind = 0;

    } else if (st.instance_detail_tab == 5) {
        // ─── LOGS ─────────────────────────────────────────────────────────
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("Logs");
        ImGui::PopFont();
        // Filter tabs
        static int log_tab = 0;
        const char* log_tabs[] = {"Latest", "Profile", "Launcher"};
        for (int i = 0; i < 3; ++i) {
            if (i > 0) ImGui::SameLine(0, ui_px(4.0f));
            bool active = log_tab == i;
            if (active) ImGui::PushStyleColor(ImGuiCol_Button, k.brand);
            else        ImGui::PushStyleColor(ImGuiCol_Button, k.surface);
            if (ImGui::Button(log_tabs[i])) log_tab = i;
            ImGui::PopStyleColor();
        }
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - ui_px(140.0f));
        if (ghost_button("Open log folder", ImVec2(ui_px(130.0f), ui_px(28.0f)))) {
            const std::filesystem::path lf = std::filesystem::path(st.exe_dir) / L"logs";
            std::error_code le;
            std::filesystem::create_directories(lf, le);
            if (!le) ShellExecuteW(st.hwnd, L"open", lf.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        ImGui::Spacing();
        std::vector<std::string> lines;
        { std::lock_guard<std::mutex> lock(st.log_mu); lines.assign(st.logs.begin(), st.logs.end()); }
        auto is_profile = [](const std::string& l) {
            return l.find("[profile]") != std::string::npos || l.find("[launch]") != std::string::npos ||
                   l.find("[mods]") != std::string::npos || l.find("[instances]") != std::string::npos;
        };
        std::string copy_text;
        int vis = 0;
        for (const auto& l : lines) {
            bool show = (log_tab == 0) || (log_tab == 1 && is_profile(l)) || (log_tab == 2 && !is_profile(l));
            if (!show) continue;
            ++vis;
            copy_text += l + "\n";
        }
        if (ghost_button("Copy visible", ImVec2(ui_px(110.0f), ui_px(28.0f))) && !copy_text.empty())
            ImGui::SetClipboardText(copy_text.c_str());
        ImGui::SameLine();
        ImGui::TextColored(k.muted, "%d line%s", vis, vis == 1 ? "" : "s");
        ImGui::Spacing();
        ImGui::PushFont(f_mono);
        if (vis == 0) {
            ImGui::TextColored(k.muted, "No log entries in this view.");
        } else {
            for (const auto& l : lines) {
                bool show = (log_tab == 0) || (log_tab == 1 && is_profile(l)) || (log_tab == 2 && !is_profile(l));
                if (!show) continue;
                ImVec4 color = (l.find("FAILED") != std::string::npos || l.find("failed") != std::string::npos)
                    ? k.red : (l.find("complete") != std::string::npos || l.find("running") != std::string::npos)
                    ? k.green : k.muted;
                ImGui::TextColored(color, "%s", l.c_str());
            }
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - ui_px(6.0f))
                ImGui::SetScrollHereY(1.0f);
        }
        ImGui::PopFont();

    } else if (st.instance_detail_tab == 6) {
        // ─── SETTINGS ─────────────────────────────────────────────────────
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("Settings");
        ImGui::PopFont();

        // General
        card_begin("##set_gen", ImVec2(-1, 0));
        ImGui::PushFont(f_bold);
        ImGui::TextUnformatted("GENERAL");
        ImGui::PopFont();
        ImGui::TextUnformatted("Profile name");
        ImGui::SetNextItemWidth(ui_px(260.0f));
        input_text("##pname", &st.selected_instance.name);
        ImGui::Spacing();
        if (ghost_button("Open instance folder", ImVec2(ui_px(170.0f), ui_px(28.0f))))
            ShellExecuteW(st.hwnd, L"open", inst.directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        card_end();
        ImGui::Spacing();

        // Minecraft
        card_begin("##set_mc", ImVec2(-1, 0));
        ImGui::PushFont(f_bold);
        ImGui::TextUnformatted("MINECRAFT");
        ImGui::PopFont();
        ImGui::Text("Version: %s    Loader: %s", mc_ver, loader_name);
        if (!inst.loader_version.empty()) ImGui::Text("Loader version: %s", inst.loader_version.c_str());
        card_end();
        ImGui::Spacing();

        // Java
        card_begin("##set_java", ImVec2(-1, 0));
        ImGui::PushFont(f_bold);
        ImGui::TextUnformatted("JAVA");
        ImGui::PopFont();
        ImGui::TextUnformatted("Java override (optional)");
        ImGui::SetNextItemWidth(ui_px(320.0f));
        input_text("##pjava", &st.selected_instance.java_path);
        card_end();
        ImGui::Spacing();

        // Memory
        card_begin("##set_mem", ImVec2(-1, 0));
        ImGui::PushFont(f_bold);
        ImGui::TextUnformatted("MEMORY");
        ImGui::PopFont();
        ImGui::SetNextItemWidth(ui_px(200.0f));
        ImGui::InputInt("##pmem", &st.selected_instance.memory_mb);
        st.selected_instance.memory_mb = std::max(1024, st.selected_instance.memory_mb);
        ImGui::TextColored(k.muted, "MB allocated. 0 = auto from performance profile.");
        card_end();
        ImGui::Spacing();

        // Performance
        card_begin("##set_perf", ImVec2(-1, 0));
        ImGui::PushFont(f_bold);
        ImGui::TextUnformatted("PERFORMANCE");
        ImGui::PopFont();
        const char* perf_ids[] = {"auto", "low_end", "balanced", "shaders", "heavy_modpack", "custom"};
        const char* perf_items = "Auto\0Low-end\0Balanced\0Shaders\0Heavy Modpack\0Custom\0";
        std::string cur_perf = performance::normalize_profile(inst.performance_profile);
        int perf_idx = 0;
        for (int i = 0; i < 6; ++i) if (cur_perf == perf_ids[i]) perf_idx = i;
        ImGui::SetNextItemWidth(ui_px(200.0f));
        if (ImGui::Combo("##perf", &perf_idx, perf_items, 6))
            st.selected_instance.performance_profile = perf_ids[perf_idx];
        ImGui::TextColored(k.muted, "Auto tunes heap; other profiles also adjust game options.");
        if (perf_idx != 0 && perf_idx != 5) {
            if (ghost_button("Apply options", ImVec2(ui_px(130.0f), ui_px(28.0f)))) {
                std::string err;
                if (performance::apply_game_options(inst.directory, inst.performance_profile, &err))
                    st.content_status = "Options applied";
                else st.content_status = err;
            }
            ImGui::SameLine();
            if (ghost_button("Install mods", ImVec2(ui_px(130.0f), ui_px(28.0f))) && !st.mod_installing) {
                instances::Instance tgt = inst;
                std::string err; instances::save(tgt, &err);
                spawn_worker(st, std::thread(do_optimize_profile, std::ref(st), tgt));
            }
        }
        if (ghost_button("Restore options", ImVec2(ui_px(140.0f), ui_px(28.0f)))) {
            std::string err;
            if (performance::restore_game_options(inst.directory, &err)) st.content_status = "Options restored";
            else st.content_status = err;
        }
        card_end();
        ImGui::Spacing();

        // Actions (moved from old Manage tab)
        card_begin("##set_act", ImVec2(-1, 0));
        ImGui::PushFont(f_bold);
        ImGui::TextUnformatted("ACTIONS");
        ImGui::PopFont();
        if (primary_button("Save Settings", ImVec2(ui_px(140.0f), ui_px(32.0f)))) {
            std::string err;
            if (instances::save(st.selected_instance, &err)) {
                st.instances_loaded = false;
                st.content_status = "Settings saved";
            } else st.content_status = err;
        }
        ImGui::Spacing();
        if (ghost_button("Duplicate Profile", ImVec2(ui_px(150.0f), ui_px(28.0f)))) {
            instances::Instance dup;
            std::wstring root = (st.cfg->base_dir.empty() ? st.exe_dir : st.cfg->base_dir) + L"\\instances";
            std::string err;
            if (instances::duplicate(inst, root, inst.name + " Copy", dup, &err)) {
                st.instances_loaded = false;
                st.content_status = "Profile duplicated";
            } else st.content_status = err;
        }
        ImGui::SameLine();
        if (ghost_button("Export Profile", ImVec2(ui_px(130.0f), ui_px(28.0f))))
            export_profile_manifest(st, inst);
        ImGui::SameLine();
        if (ghost_button("Scan & Repair", ImVec2(ui_px(140.0f), ui_px(28.0f))) && !st.running) {
            config::Config snap = *st.cfg;
            instances::Instance tgt = inst;
            spawn_worker(st, std::thread([&st, snap, tgt]() {
                std::wstring rp; std::string be;
                if (!instances::create_restore_point(tgt, &rp, &be)) {
                    log_line(st, L"[profile] repair cancelled: " + net::to_wide(be)); return;
                }
                log_line(st, L"[profile] restore point: " + rp);
                do_launch(st, tgt.minecraft_version, snap, tgt.directory, true);
            }));
        }
        ImGui::SameLine();
        if (ghost_button("Restore Latest", ImVec2(ui_px(130.0f), ui_px(28.0f)))) {
            std::string err;
            if (instances::restore_latest(inst, &err)) {
                st.content_status = "Restored";
                st.instances_loaded = false;
            } else st.content_status = err;
        }
        if (st.cfg->advanced_mode) {
            ImGui::Spacing();
            if (ghost_button("Move to Recovery", ImVec2(ui_px(160.0f), ui_px(28.0f)))) {
                std::string err;
                if (instances::remove(inst, &err)) {
                    push_notice(st, ui_model::NoticeLevel::Success, "Moved to recovery",
                                "Profile removed from active library.", "Open Library", "library");
                    st.instance_detail_open = false;
                    st.instances_loaded = false;
                } else log_line(st, L"[instances] recovery failed: " + net::to_wide(err));
            }
        }
        card_end();
        ImGui::Spacing();
        if (!st.content_status.empty()) ImGui::TextColored(k.yellow, "%s", st.content_status.c_str());
    } else if (st.instance_detail_tab == 7) {
        // ─── AI ───────────────────────────────────────────────────────────
        std::wstring ai_root = inst.directory;
        if (!inst.is_ai_profile) {
            st.instance_detail_tab = 0;
        } else {
            aml::ai::draw_ai_profile_tab(st, inst.id, ai_root);
        }
    }
    card_end();

    // ── Creator update confirm popup ──────────────────────────────────────
    set_next_adaptive_window(560.0f, 0.0f, 340.0f, 0.0f);
    if (ImGui::BeginPopupModal("Confirm creator update", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        const instances::Instance& target = st.pack_update_confirm_target;
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted(st.pack_update_confirm_copy ? "Create updated copy" : "Apply creator update");
        ImGui::PopFont();
        ImGui::TextWrapped("Profile: %s", target.name.empty() ? target.id.c_str() : target.name.c_str());
        ImGui::TextColored(k.muted, "Version: %s",
                           st.pack_update_confirm_version.empty() ? "latest" : st.pack_update_confirm_version.c_str());
        ImGui::Spacing();
        if (st.pack_update_confirm_copy)
            ImGui::TextWrapped("A duplicate will be created and updated. Your current profile stays unchanged.");
        else
            ImGui::TextWrapped("Pack-managed content will be replaced. Worlds, screenshots, and logs are untouched.");
        ImGui::Spacing();
        const char* cl = st.pack_update_confirm_copy ? "Create updated copy" : "Apply creator update";
        if (primary_button(cl, ImVec2(ui_px(190.0f), ui_px(36.0f))) && !st.mod_installing) {
            const config::Config snap = *st.cfg;
            const instances::Instance ut = st.pack_update_confirm_target;
            const bool cc = st.pack_update_confirm_copy;
            st.pack_update_confirm_open = false;
            ImGui::CloseCurrentPopup();
            spawn_worker(st, std::thread([&st, snap, ut, cc]() { do_published_pack_update(st, snap, ut, cc); }));
        }
        ImGui::SameLine();
        if (ghost_button("Cancel", ImVec2(ui_px(110.0f), ui_px(36.0f)))) {
            st.pack_update_confirm_open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void draw_instances_tab(UiState& st) {
    std::wstring instances_root = (st.cfg->base_dir.empty() ? st.exe_dir : st.cfg->base_dir) +
                                  L"\\instances";
    if (!st.instances_loaded) {
        std::string scan_error;
        st.instance_list = instances::scan(instances_root, &scan_error);
        st.instances_loaded = true;
        {
            std::lock_guard<std::mutex> lock(st.file_index_mu);
            ++st.library_worlds_scan_generation;
            st.library_worlds_scan_pending = false;
            st.library_worlds_cache_at_ms = 0;
            ++st.screenshots_scan_generation;
            st.screenshots_scan_pending = false;
            st.screenshots_cache_at_ms = 0;
        }
        st.quick_search_index_at_ms = 0;
        st.home_readiness.dirty = true;
        if (!scan_error.empty()) log_line(st, L"[instances] scan failed: " + net::to_wide(scan_error));
    }
    if (st.instance_detail_open) {
        draw_instance_detail(st);
        return;
    }
    draw_page_emblem(st, "library-emblem-ai.png");
    page_title("Library", "Your modpacks, worlds, and collections in one place.");
    draw_breadcrumbs({"Home", "Library"});
    const char* tabs[] = {"My Modpacks", "My Worlds", "Collections"};
    for (int i = 0; i < 3; ++i) {
        if (i) ImGui::SameLine(0, ui_px(28));
        bool active = i == st.library_section;
        ImGui::PushStyleColor(ImGuiCol_Text, active ? k.text : k.muted);
        ImGui::PushFont(active ? f_h2 : f_body);
        if (ImGui::Selectable(tabs[i], active, ImGuiSelectableFlags_None,
                               ImVec2(ImGui::CalcTextSize(tabs[i]).x + ui_px(18.0f), ui_px(38.0f))))
            st.library_section = i;
        ImGui::PopFont();
        ImGui::PopStyleColor();
    }
    ImGui::Separator();
    ImGui::Spacing();
    if (st.library_section == 1) {
        struct LibraryWorld {
            const instances::Instance* instance = nullptr;
            std::filesystem::path directory;
            std::string name;
        };
        std::vector<LibraryWorld> library_worlds;
        const uint64_t world_index_now = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        constexpr uint64_t kWorldIndexTtlMs = 1500;
        bool start_world_scan = false;
        uint64_t world_generation = 0;
        std::vector<instances::Instance> world_instances;
        std::vector<UiState::WorldIndexEntry> cached_worlds;
        {
            std::lock_guard<std::mutex> lock(st.file_index_mu);
            const bool stale = st.library_worlds_cache_at_ms == 0 ||
                world_index_now - st.library_worlds_cache_at_ms > kWorldIndexTtlMs;
            if (stale && !st.library_worlds_scan_pending) {
                st.library_worlds_scan_pending = true;
                world_generation = ++st.library_worlds_scan_generation;
                world_instances = st.instance_list;
                start_world_scan = true;
            }
            cached_worlds = st.library_worlds_cache;
        }
        if (start_world_scan) {
            spawn_worker(st, std::thread([&st, world_instances = std::move(world_instances),
                                          world_generation]() {
                std::vector<UiState::WorldIndexEntry> found;
                for (const auto& instance : world_instances) {
                    const std::filesystem::path saves =
                        std::filesystem::path(instance.directory) / L"saves";
                    std::error_code world_error;
                    if (!std::filesystem::exists(saves, world_error) || world_error) continue;
                    for (std::filesystem::directory_iterator it(saves, world_error), end;
                         !world_error && it != end; it.increment(world_error)) {
                        std::error_code type_error;
                        if (!it->is_directory(type_error) || type_error) continue;
                        const std::wstring folder = it->path().filename().wstring();
                        if (folder.rfind(L".amalgam-", 0) == 0) continue;
                        found.push_back({instance.directory, it->path().wstring(),
                                         net::to_utf8(folder)});
                    }
                }
                const uint64_t completed_at = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
                std::lock_guard<std::mutex> lock(st.file_index_mu);
                if (st.library_worlds_scan_generation == world_generation) {
                    st.library_worlds_cache = std::move(found);
                    st.library_worlds_cache_at_ms = completed_at;
                    st.library_worlds_scan_pending = false;
                }
            }));
        }
        library_worlds.reserve(cached_worlds.size());
        for (const auto& cached : cached_worlds) {
            const auto it = std::find_if(st.instance_list.begin(), st.instance_list.end(),
                [&cached](const instances::Instance& instance) {
                    return instance.directory == cached.instance_directory;
                });
            if (it != st.instance_list.end())
                library_worlds.push_back({&*it, std::filesystem::path(cached.directory), cached.name});
        }
        std::sort(library_worlds.begin(), library_worlds.end(), [](const LibraryWorld& a,
                                                                     const LibraryWorld& b) {
            if (a.instance->last_played != b.instance->last_played)
                return a.instance->last_played > b.instance->last_played;
            if (a.name != b.name) return a.name < b.name;
            return a.instance->id < b.instance->id;
        });

        card_begin("##world_gallery", ImVec2(-1, 0));
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("My Worlds");
        ImGui::PopFont();
        ImGui::TextColored(k.muted,
                           "Worlds found inside your managed profiles. Open a profile for backup and recovery actions.");
        ImGui::Separator();
        if (library_worlds.empty()) {
            ImGui::Spacing();
            illustrated_empty_state(IconId::Globe, "NO WORLDS YET",
                                    "Create a world in Minecraft or import an existing save.\nWorlds from your managed profiles will appear here automatically.");
        } else {
            constexpr size_t kMaxVisibleWorlds = 60;
            const size_t visible_count = std::min(library_worlds.size(), kMaxVisibleWorlds);
            ImGui::TextColored(k.muted, "%d world%s across your profiles", static_cast<int>(library_worlds.size()),
                               library_worlds.size() == 1 ? "" : "s");
            if (library_worlds.size() > visible_count)
                ImGui::TextColored(k.muted, "Showing the first %d worlds to keep the library responsive.",
                                   static_cast<int>(visible_count));
            ImGui::Spacing();

            const float gap = ui_px(12.0f);
            const float available = std::max(0.0f, ImGui::GetContentRegionAvail().x - ui_px(18.0f));
            const int columns = available >= ui_px(1180.0f) ? 4 : available >= ui_px(820.0f) ? 3 :
                                available >= ui_px(520.0f) ? 2 : 1;
            const float card_width = std::max(ui_px(210.0f),
                (available - gap * static_cast<float>(columns - 1)) / static_cast<float>(columns));
            const float image_height = std::clamp(card_width * 0.50f, ui_px(112.0f), ui_px(172.0f));
            const float card_height = image_height + ui_px(112.0f);

            for (size_t index = 0; index < visible_count; ++index) {
                const LibraryWorld& world = library_worlds[index];
                if (index % static_cast<size_t>(columns)) ImGui::SameLine(0, gap);
                ImGui::PushID(static_cast<int>(index));
                ImGui::PushStyleColor(ImGuiCol_ChildBg, k.surface2);
                ImGui::PushStyleColor(ImGuiCol_Border, k.border);
                ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, ui_px(9.0f));
                ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, ui_px(1.0f));
                ImGui::BeginChild("##worldcard", ImVec2(card_width, card_height), ImGuiChildFlags_Borders);

                const ImVec2 image_pos = ImGui::GetCursorScreenPos();
                const ImVec2 image_size(card_width - ui_px(2.0f), image_height);
                const std::filesystem::path icon = world.directory / L"icon.png";
                std::error_code icon_error;
                if (std::filesystem::exists(icon, icon_error) && !icon_error)
                    draw_local_image(st, icon.wstring(), image_pos, image_size, c32(k.brand_dk));
                else
                    draw_instance_art_placeholder(*world.instance, image_pos, image_size, ui_px(9.0f));
                ImGui::InvisibleButton("##openworld", image_size);
                if (ImGui::IsItemHovered())
                    ImGui::GetWindowDrawList()->AddRect(image_pos, image_pos + image_size, c32(k.brand_hov),
                                                        ui_px(9.0f), 0, ui_px(1.5f));
                if (ImGui::IsItemClicked())
                    ShellExecuteW(st.hwnd, L"open", world.directory.wstring().c_str(), nullptr, nullptr,
                                  SW_SHOWNORMAL);

                std::string world_name = world.name;
                if (world_name.size() > 34) world_name = world_name.substr(0, 31) + "...";
                std::string profile_name = world.instance->name.empty() ? world.instance->id : world.instance->name;
                if (profile_name.size() > 34) profile_name = profile_name.substr(0, 31) + "...";
                ImGui::PushFont(f_bold);
                ImGui::TextUnformatted(world_name.c_str());
                ImGui::PopFont();
                ImGui::TextColored(k.muted, "%s | %s", profile_name.c_str(),
                                   world.instance->minecraft_version.c_str());
                if (primary_button("Open", ImVec2(ui_px(92.0f), ui_px(30.0f))))
                    ShellExecuteW(st.hwnd, L"open", world.directory.wstring().c_str(), nullptr, nullptr,
                                  SW_SHOWNORMAL);
                ImGui::SameLine();
                if (ghost_button("Profile", ImVec2(ui_px(96.0f), ui_px(30.0f)))) {
                    st.selected_instance = *world.instance;
                    st.active_instance_dir = world.instance->directory;
                    st.instance_detail_tab = 2;
                    st.instance_detail_open = true;
                }

                ImGui::EndChild();
                ImGui::PopStyleVar(2);
                ImGui::PopStyleColor(2);
                ImGui::PopID();
            }
        }
        card_end();
        return;
    }
    if (st.library_section == 2) {
        // ── COLLECTIONS ────────────────────────────────────────────────────
        page_title("Collections", "Favorites and groups keep large libraries organized.");

        std::vector<std::string> groups;
        for (const auto& instance : st.instance_list) {
            if (!instance.group.empty() &&
                std::find(groups.begin(), groups.end(), instance.group) == groups.end())
                groups.push_back(instance.group);
        }
        std::sort(groups.begin(), groups.end());
        int favorites = 0;
        for (const auto& instance : st.instance_list) if (instance.favorite) ++favorites;

        // Toolbar
        card_begin("##collections_toolbar", ImVec2(-1, 0));
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("COLLECTIONS");
        ImGui::PopFont();
        ImGui::TextColored(k.muted, "%d group(s)  |  %d favorited profile(s)  |  assign profiles to a group from their ⋮ menu",
                           static_cast<int>(groups.size()), favorites);
        card_end();
        ImGui::Spacing();

        // Collection cards: Favorites + one per group
        struct CollectionCard {
            std::string name;
            bool is_favorites = false;
            int count = 0;
            std::vector<const instances::Instance*> members;
        };
        std::vector<CollectionCard> cards;
        CollectionCard fav_card;
        fav_card.name = "Favorites";
        fav_card.is_favorites = true;
        for (const auto& instance : st.instance_list)
            if (instance.favorite) { fav_card.count++; fav_card.members.push_back(&instance); }
        cards.push_back(fav_card);
        for (const auto& group : groups) {
            CollectionCard card;
            card.name = group;
            for (const auto& instance : st.instance_list)
                if (instance.group == group) { card.count++; card.members.push_back(&instance); }
            cards.push_back(card);
        }

        if (cards.size() == 1 && cards[0].count == 0) {
            card_begin("##collections_empty", ImVec2(-1, 0));
            ImGui::Spacing();
            illustrated_empty_state(IconId::Star, "NO COLLECTIONS YET",
                                    "Organize your profiles with favorites and groups.\nOpen any profile's menu (⋮) and pick a group to start a collection.");
            ImGui::Spacing();
            if (primary_button("Browse Profiles", ImVec2(ui_px(150.0f), ui_px(34.0f)))) {
                st.instance_search.clear();
                st.instance_filter_idx = 0;
                st.library_section = 0;
            }
            card_end();
        } else {
            const float gap = ui_px(12.0f);
            const float available = std::max(0.0f, ImGui::GetContentRegionAvail().x - ui_px(18.0f));
            const int columns = available >= ui_px(1180.0f) ? 3 : available >= ui_px(820.0f) ? 2 : 1;
            const float card_width = std::max(ui_px(260.0f),
                (available - gap * static_cast<float>(columns - 1)) / static_cast<float>(columns));
            const float card_height = ui_px(206.0f);

            for (size_t index = 0; index < cards.size(); ++index) {
                const CollectionCard& card = cards[index];
                if (index % static_cast<size_t>(columns)) ImGui::SameLine(0, gap);
                ImGui::PushID(static_cast<int>(index));
                ImGui::PushStyleColor(ImGuiCol_ChildBg, k.surface2);
                ImGui::PushStyleColor(ImGuiCol_Border, k.border);
                ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, ui_px(10.0f));
                ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, ui_px(1.0f));
                ImGui::BeginChild("##collection_card", ImVec2(card_width, card_height), ImGuiChildFlags_Borders);

                const ImVec2 card_pos = ImGui::GetCursorScreenPos();
                const ImVec4 accent = card.is_favorites ? k.brand : k.surface2;
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddRectFilled(card_pos,
                                  ImVec2(card_pos.x + card_width, card_pos.y + ui_px(4.0f)),
                                  c32(accent), ui_px(4.0f));
                ImGui::Dummy(ImVec2(0, ui_px(14.0f)));

                // Header row: icon + name + count
                const ImVec2 icon_center = ImGui::GetCursorScreenPos() + ImVec2(ui_px(12.0f), ui_px(12.0f));
                draw_icon(card.is_favorites ? IconId::Star : IconId::Folder, icon_center, ui_px(9.0f),
                          c32(card.is_favorites ? k.brand : k.text));
                ImGui::SetCursorPosX(ui_px(30.0f));
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ui_px(6.0f));
                ImGui::PushFont(f_bold);
                std::string display_name = card.name;
                if (display_name.size() > 26) display_name = display_name.substr(0, 23) + "...";
                ImGui::TextUnformatted(display_name.c_str());
                ImGui::PopFont();
                ImGui::TextColored(k.muted, "%d profile%s", card.count, card.count == 1 ? "" : "s");

                ImGui::Dummy(ImVec2(0, ui_px(6.0f)));

                // Member chips (up to 5)
                const int chip_count = std::min(5, static_cast<int>(card.members.size()));
                for (int c = 0; c < chip_count; ++c) {
                    const instances::Instance* member = card.members[static_cast<size_t>(c)];
                    std::string chip = member->name.empty() ? member->id : member->name;
                    if (chip.size() > 22) chip = chip.substr(0, 19) + "...";
                    const ImVec2 chip_size(ui_px(124.0f), ui_px(22.0f));
                    const ImVec2 chip_pos = ImGui::GetCursorScreenPos();
                    dl->AddRectFilled(chip_pos, chip_pos + chip_size, c32(k.surface), ui_px(5.0f));
                    ImGui::InvisibleButton(("##chip_" + std::to_string(c)).c_str(), chip_size);
                    if (ImGui::IsItemClicked()) {
                        st.selected_instance = *member;
                        st.active_instance_dir = member->directory;
                        st.instance_detail_open = true;
                    }
                    if (ImGui::IsItemHovered())
                        dl->AddRect(chip_pos, chip_pos + chip_size, c32(k.brand_hov), ui_px(5.0f),
                                    0, ui_px(1.0f));
                    dl->AddCircleFilled(chip_pos + ImVec2(ui_px(11.0f), ui_px(11.0f)), ui_px(3.5f),
                                        c32(k.brand));
                    dl->AddText(f_small, ui_px(13.0f),
                                chip_pos + ImVec2(ui_px(20.0f), ui_px(4.0f)),
                                c32(k.muted), chip.c_str());
                    if (c % 2 == 0) ImGui::SameLine(0, ui_px(6.0f));
                    if (c % 2 == 1) ImGui::Dummy(ImVec2(0, ui_px(2.0f)));
                }
                if (card.members.size() > 5)
                    ImGui::TextColored(k.muted, "+%d more",
                                       static_cast<int>(card.members.size()) - 5);

                ImGui::Dummy(ImVec2(0, ui_px(10.0f)));

                // Footer actions
                const float btn_w = (card_width - ui_px(28.0f)) / 3.0f;
                if (card.is_favorites) {
                    if (primary_button("Open", ImVec2(btn_w, ui_px(28.0f)))) {
                        st.instance_filter_idx = 1;
                        st.library_section = 0;
                    }
                } else {
                    if (primary_button("Open", ImVec2(btn_w, ui_px(28.0f)))) {
                        int group_filter_idx = 2;
                        for (const auto& g : groups) {
                            if (g == card.name) break;
                            ++group_filter_idx;
                        }
                        st.instance_filter_idx = group_filter_idx;
                        st.library_section = 0;
                    }
                }
                if (!card.is_favorites) {
                    ImGui::SameLine(0, ui_px(6.0f));
                    if (ghost_button("Rename", ImVec2(btn_w, ui_px(28.0f)))) {
                        st.new_group_name = card.name;
                        st.rename_group_target = card.name;
                        ImGui::OpenPopup("##rename_group_modal");
                    }
                    ImGui::SameLine(0, ui_px(6.0f));
                    if (ghost_button("Delete", ImVec2(btn_w, ui_px(28.0f)))) {
                        st.delete_group_target = card.name;
                        ImGui::OpenPopup("##delete_group_modal");
                    }
                } else {
                    ImGui::SameLine(0, ui_px(6.0f));
                    ImGui::TextDisabled("Tap a star on any profile to pin it here.");
                }

                ImGui::EndChild();
                ImGui::PopStyleVar(2);
                ImGui::PopStyleColor(2);
                ImGui::PopID();
            }
        }

        // Rename group modal
        set_next_adaptive_window(380.0f, 0.0f, 300.0f, 0.0f);
        if (ImGui::BeginPopupModal("Rename Group##rename_group_modal", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted(st.rename_group_target.empty() ? "Group name:" : "Rename group:");
            input_text("##new_group_input", &st.new_group_name);
            ImGui::Spacing();
            bool confirmed = false;
            if (primary_button("Save", ImVec2(ui_px(120.0f), ui_px(32.0f)))) confirmed = true;
            ImGui::SameLine();
            if (ghost_button("Cancel", ImVec2(ui_px(100.0f), ui_px(32.0f)))) ImGui::CloseCurrentPopup();
            if (confirmed) {
                std::string group = st.new_group_name;
                if (!group.empty()) {
                    if (!st.rename_group_target.empty() && st.rename_group_target != group) {
                        for (auto& inst : st.instance_list)
                            if (inst.group == st.rename_group_target) {
                                inst.group = group;
                                std::string error;
                                if (!instances::save(inst, &error))
                                    log_line(st, L"[instances] save failed: " + net::to_wide(error));
                            }
                        push_notice(st, ui_model::NoticeLevel::Success, "Group renamed",
                                    "\"" + st.rename_group_target + "\" is now \"" + group + "\".");
                    } else if (st.group_target.empty()) {
                        log_line(st, L"[modpacks] group \"" + net::to_wide(group) + L"\" created");
                    }
                }
                st.rename_group_target.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Delete group confirmation
        set_next_adaptive_window(380.0f, 0.0f, 300.0f, 0.0f);
        if (ImGui::BeginPopupModal("Delete Group##delete_group_modal", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::PushFont(f_bold);
            ImGui::TextUnformatted("Delete this group?");
            ImGui::PopFont();
            ImGui::TextColored(k.muted,
                               "Profiles stay in your library — only the group label is removed.");
            ImGui::Spacing();
            bool confirmed = false;
            if (primary_button("Delete group", ImVec2(ui_px(130.0f), ui_px(32.0f)))) confirmed = true;
            ImGui::SameLine();
            if (ghost_button("Cancel", ImVec2(ui_px(100.0f), ui_px(32.0f)))) ImGui::CloseCurrentPopup();
            if (confirmed) {
                for (auto& inst : st.instance_list)
                    if (inst.group == st.delete_group_target) {
                        inst.group.clear();
                        std::string error;
                        if (!instances::save(inst, &error))
                            log_line(st, L"[instances] save failed: " + net::to_wide(error));
                    }
                push_notice(st, ui_model::NoticeLevel::Success, "Group deleted",
                            "\"" + st.delete_group_target + "\" was removed from your library.");
                st.delete_group_target.clear();
                st.instances_loaded = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        return;
    }
    ImGui::BeginChild("##packleft", ImVec2(0, 0));
    if (primary_button("+  Create Custom Profile", ImVec2(ui_px(190.0f), ui_px(38.0f)))) {
        st.wizard_open = true;
        st.wizard_step = 0;
        st.wizard_source = 0;
        st.wizard_preset = -1;
        st.wizard_name = "My Modpack";
        st.wizard_project.clear();
        st.wizard_version = st.selected;
        st.wizard_loader = st.cfg->loader == "auto" ? "fabric" : st.cfg->loader;
        st.wizard_prompt.clear();
        st.wizard_archive.clear();
        set_pack_summary(st, {});
        st.pack_plan.clear();
        st.wizard_memory = 0;
        st.wizard_performance_profile = st.cfg->performance_profile;
        st.wizard_java.clear();
    }
    ImGui::SameLine();
    if (ghost_button("Import", ImVec2(ui_px(105.0f), ui_px(38.0f)))) {
        st.wizard_open = true;
        st.wizard_step = 0;
        st.wizard_source = 4;
        st.wizard_name = "Imported Modpack";
        st.wizard_project.clear();
        st.wizard_performance_profile = st.cfg->performance_profile;
    }
    ImGui::SameLine();
    if (ghost_button("Create Group", ImVec2(ui_px(140.0f), ui_px(38.0f)))) {
        st.new_group_name.clear();
        st.group_target.clear();
        ImGui::OpenPopup("##new_group_modal");
    }
    const float toolbar_width = ImGui::GetContentRegionAvail().x;
    const bool narrow_toolbar = toolbar_width < ui_px(930.0f);
    const bool stacked_filters = toolbar_width < ui_px(720.0f);
    if (narrow_toolbar) {
        // The action buttons intentionally own the first row at narrow sizes;
        // forcing filters beside them clipped the primary actions.
        ImGui::NewLine();
    } else {
         const float filter_group_width = ui_px(510.0f);
        const float target_x = std::max(ImGui::GetCursorPosX(),
                                        ImGui::GetCursorPosX() + toolbar_width - filter_group_width);
        ImGui::SameLine(target_x);
    }
    ImGui::SetNextItemWidth(stacked_filters ? -1.0f : ui_px(220.0f));
    input_text_hint("##library_search", "Search modpacks", &st.instance_search);
    if (!stacked_filters) ImGui::SameLine(0, ui_px(8.0f));
    ImGui::SetNextItemWidth(stacked_filters ? -1.0f : ui_px(104.0f));
    const char* sort_labels[] = {"Sort: Recent", "Sort: Name A-Z", "Sort: Name Z-A", "Sort: Favorites"};
    ImGui::Combo("##sort", &st.instance_sort, sort_labels, 4);
    if (!stacked_filters) ImGui::SameLine(0, ui_px(8.0f));
    ImGui::SetNextItemWidth(stacked_filters ? -1.0f : ui_px(124.0f));
    std::vector<std::string> filter_labels;
    filter_labels.push_back("Filter: All");
    filter_labels.push_back("Filter: Favorites");
    std::vector<std::string> group_names;
    for (const auto& inst : st.instance_list) {
        if (!inst.group.empty() && std::find(group_names.begin(), group_names.end(), inst.group) == group_names.end())
            group_names.push_back(inst.group);
    }
    std::sort(group_names.begin(), group_names.end());
    for (const auto& g : group_names) filter_labels.push_back("Group: " + g);
    std::vector<const char*> filter_cstrs;
    for (const auto& l : filter_labels) filter_cstrs.push_back(l.c_str());
    if (st.instance_filter_idx >= static_cast<int>(filter_cstrs.size())) st.instance_filter_idx = 0;
    ImGui::Combo("##filter", &st.instance_filter_idx, filter_cstrs.data(),
                 static_cast<int>(filter_cstrs.size()));
    ImGui::Spacing();

    std::vector<instances::Instance*> visible;
    for (auto& inst : st.instance_list) {
        std::string name = inst.name.empty() ? inst.id : inst.name;
        if (!st.instance_search.empty()) {
            std::string name_lower = name, search_lower = st.instance_search;
            std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            std::transform(search_lower.begin(), search_lower.end(), search_lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (name_lower.find(search_lower) == std::string::npos) continue;
        }
        if (st.instance_filter_idx == 1 && !inst.favorite) continue;
        if (st.instance_filter_idx >= 2 && inst.group != group_names[st.instance_filter_idx - 2]) continue;
        visible.push_back(&inst);
    }
    std::stable_sort(visible.begin(), visible.end(),
                     [&](const instances::Instance* a, const instances::Instance* b) {
        switch (st.instance_sort) {
            case 1: return a->name < b->name;
            case 2: return a->name > b->name;
            case 3: return a->favorite != b->favorite ? a->favorite : a->last_played > b->last_played;
            default: return a->last_played > b->last_played;
        }
    });

    float grid_width = ImGui::GetContentRegionAvail().x;
    const int columns = grid_width >= ui_px(1000.0f) ? 3 : grid_width >= ui_px(650.0f) ? 2 : 1;
    const float card_spacing = ui_px(12.0f) * static_cast<float>(columns - 1);
    float card_width = std::max(ui_px(240.0f),
                                (grid_width - card_spacing) / static_cast<float>(columns));
    int count = 0;
    for (auto* instance_ptr : visible) {
        instances::Instance& instance = *instance_ptr;
        std::string name = instance.name.empty() ? instance.id : instance.name;
        if (count % columns) ImGui::SameLine(0, ui_px(12.0f));
        ++count;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, k.surface);
        ImGui::PushStyleColor(ImGuiCol_Border, k.border);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, ui_px(8.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, ui_px(1.0f));
        ImGui::BeginChild((std::string("##pack") + std::to_string(count)).c_str(),
                            ImVec2(card_width, ui_px(350.0f)), ImGuiChildFlags_Borders);
        ImVec2 art_pos = ImGui::GetCursorScreenPos();
        ImVec2 art_size(card_width - ui_px(2.0f), ui_px(176.0f));
        draw_instance_art(st, instance, art_pos, art_size, c32(k.brand_dk));
        ImGui::Dummy(ImVec2(0, ui_px(182.0f)));
        const bool art_clicked = ImGui::IsItemClicked();
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted(name.c_str());
        ImGui::PopFont();
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 34);
        const std::string favorite_id = "##library_profile_favorite_" + instance.id;
        if (favorite_button(favorite_id.c_str(), instance.favorite)) {
            instance.favorite = !instance.favorite;
            std::string error;
            if (!instances::save(instance, &error))
                log_line(st, L"[instances] save failed: " + net::to_wide(error));
        }
        ImGui::TextColored(k.muted, "%s  |  %s", instance.minecraft_version.empty() ? "unconfigured" : instance.minecraft_version.c_str(),
                           instance.loader.empty() ? "auto" : instance.loader.c_str());
        ImGui::TextColored(instance.pack_source.empty() ? k.muted :
                               (instance.pack_modified ? k.yellow : k.green), "%s",
                           instance.pack_source.empty() ? "Custom profile" :
                               (instance.pack_modified ? "Modified" : "Published pack"));
        auto health = profile_health(st, instance);
        ImGui::TextColored(health.empty() ? k.green : k.yellow, "%s",
                           health.empty() ? "Ready" : (std::to_string(health.size()) + " issue(s)").c_str());
        if (!instance.group.empty())
            ImGui::TextColored(k.muted, "group: %s", instance.group.c_str());
        ImGui::Spacing();
        const float library_more_width = ui_px(38.0f);
        const float library_open_width = ui_px(70.0f);
        const float library_play_width = std::max(ui_px(64.0f),
            ImGui::GetContentRegionAvail().x - library_more_width - library_open_width - ui_px(14.0f));
        if (primary_button("Play", ImVec2(library_play_width, ui_px(32.0f))) && !st.running &&
            !instance.minecraft_version.empty()) {
            st.selected = instance.minecraft_version;
            st.pending_instance_dir = instance.directory;
            st.active_instance_dir = instance.directory;
            st.pending_id = st.selected;
            st.pending_launch = true;
        }
        ImGui::SameLine();
        if (ghost_button("Open", ImVec2(library_open_width, ui_px(32.0f))))
            open_instance_detail(st, instance);
        ImGui::SameLine();
        const std::string menu_id = "##library_profile_more_" + instance.id;
        if (ghost_button("...", ImVec2(library_more_width, ui_px(32.0f))))
            ImGui::OpenPopup(menu_id.c_str());
        draw_instance_overflow_menu(st, instance, menu_id.c_str());
        if (ImGui::BeginPopupContextItem((std::string("##packctx") + instance.id).c_str())) {
            if (ImGui::MenuItem(instance.favorite ? "Remove favorite" : "Add to favorites")) {
                instance.favorite = !instance.favorite;
                std::string error;
                if (!instances::save(instance, &error))
                    log_line(st, L"[instances] save failed: " + net::to_wide(error));
            }
            if (ImGui::BeginMenu("Move to group")) {
                for (const auto& g : group_names) {
                    if (ImGui::MenuItem(g.c_str(), nullptr, instance.group == g)) {
                        instance.group = g;
                        std::string error;
                        if (!instances::save(instance, &error))
                            log_line(st, L"[instances] save failed: " + net::to_wide(error));
                    }
                }
                ImGui::Separator();
                if (ImGui::MenuItem("New group...")) {
                    st.new_group_name.clear();
                    st.group_target = instance.id;
                    ImGui::OpenPopup("##new_group_modal");
                }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Move to recovery...")) {
                st.delete_target = instance.id;
                ImGui::OpenPopup("##delete_modal");
            }
            ImGui::EndPopup();
        }
        const bool open_card = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup) &&
                               ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                               !ImGui::IsAnyItemHovered();
        if (art_clicked || open_card) open_instance_detail(st, instance);
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
    }
    if (count % columns) ImGui::SameLine(0, ui_px(12.0f));
    draw_home_create_card(st, card_width, count);
    ImGui::EndChild();

    set_next_adaptive_window(380.0f, 0.0f, 300.0f, 0.0f);
    if (ImGui::BeginPopupModal("New Group##new_group_modal", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Group name:");
        input_text("##new_group_input", &st.new_group_name);
        ImGui::Spacing();
        bool confirmed = false;
         if (primary_button("Create", ImVec2(ui_px(120.0f), ui_px(32.0f)))) confirmed = true;
        ImGui::SameLine();
         if (ghost_button("Cancel", ImVec2(ui_px(100.0f), ui_px(32.0f)))) ImGui::CloseCurrentPopup();
        if (confirmed) {
            std::string group = st.new_group_name;
            if (!group.empty()) {
                if (st.group_target.empty()) {
                    log_line(st, L"[modpacks] group \"" + net::to_wide(group) + L"\" created");
                } else {
                    for (auto& inst : st.instance_list) {
                        if (inst.id == st.group_target) {
                            inst.group = group;
                            std::string error;
                            if (!instances::save(inst, &error))
                                log_line(st, L"[instances] save failed: " + net::to_wide(error));
                        }
                    }
                }
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    set_next_adaptive_window(380.0f, 0.0f, 300.0f, 0.0f);
    if (ImGui::BeginPopupModal("Move Modpack to Recovery##delete_modal", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Move this modpack to recovery?");
        ImGui::TextColored(k.muted, "The full instance folder will leave your active library and remain recoverable.");
        ImGui::Spacing();
        bool confirmed = false;
        if (primary_button("Move to recovery", ImVec2(ui_px(155.0f), ui_px(32.0f)))) confirmed = true;
        ImGui::SameLine();
        if (ghost_button("Cancel", ImVec2(ui_px(100.0f), ui_px(32.0f)))) ImGui::CloseCurrentPopup();
        if (confirmed) {
            for (const auto& inst : st.instance_list) {
                if (inst.id == st.delete_target) {
                    std::string error;
                    if (!instances::remove(inst, &error))
                        log_line(st, L"[instances] recovery move failed: " + net::to_wide(error));
                    else
                        push_notice(st, ui_model::NoticeLevel::Success, "Profile moved to recovery",
                                    "The full profile left your active library without being permanently deleted.",
                                    "Open Library", "library");
                    break;
                }
            }
            st.instances_loaded = false;
            st.delete_target.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void draw_screenshots_tab(UiState& st) {
    page_title("Screenshots", "Captured screenshots from your managed Minecraft instances.");
    
    // ── Header with quick actions ─────────────────────────────────────────
    card_begin("##screenshots_header", ImVec2(-1, 0));
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted("Screenshot Library");
    ImGui::PopFont();
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - ui_px(200.0f));
    if (primary_button("Open Screenshots Folder", ImVec2(ui_px(180.0f), ui_px(32.0f)))) {
        std::filesystem::path screenshots;
        if (!st.active_instance_dir.empty()) {
            screenshots = std::filesystem::path(st.active_instance_dir) / L"screenshots";
        } else {
            screenshots = std::filesystem::path(st.exe_dir) / L"screenshots";
        }
        std::error_code folder_error;
        std::filesystem::create_directories(screenshots, folder_error);
        if (!folder_error)
            ShellExecuteW(st.hwnd, L"open", screenshots.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    card_end();
    ImGui::Spacing();

    // ── Profile selector ──────────────────────────────────────────────────
    card_begin("##screenshots_profile", ImVec2(-1, 0));
    ImGui::TextUnformatted("Profile");
    ImGui::SameLine();
    ImGui::TextColored(k.muted, "Select a profile to view its screenshots");
    ImGui::Spacing();
    
    const float combo_width = auto_item_width(400.0f, 280.0f);
    ImGui::SetNextItemWidth(combo_width);
    if (ImGui::BeginCombo("##screenshots_profile_select", 
                         st.active_instance_dir.empty() ? "All Profiles" : 
                         net::to_utf8(std::filesystem::path(st.active_instance_dir).filename()).c_str())) {
        if (ImGui::Selectable("All Profiles", st.active_instance_dir.empty())) {
            st.active_instance_dir.clear();
        }
        for (const auto& inst : st.instance_list) {
            std::string label = inst.name;
            bool is_selected = st.active_instance_dir == inst.directory;
            if (ImGui::Selectable(label.c_str(), is_selected)) {
                st.active_instance_dir = inst.directory;
            }
            if (is_selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    card_end();
    ImGui::Spacing();

    // ── Collect screenshots ────────────────────────────────────────────────
    std::vector<std::filesystem::path> image_files;
    const uint64_t screenshot_now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    constexpr uint64_t kScreenshotIndexTtlMs = 1200;
    bool start_screenshot_scan = false;
    uint64_t screenshot_generation = 0;
    const std::wstring screenshot_instance = st.active_instance_dir;
    std::vector<instances::Instance> screenshot_instances;
    std::vector<UiState::ScreenshotIndexEntry> cached_screenshots;
    {
        std::lock_guard<std::mutex> lock(st.file_index_mu);
        const bool stale = st.screenshots_cache_at_ms == 0 ||
            st.screenshots_cache_instance_directory != screenshot_instance ||
            screenshot_now - st.screenshots_cache_at_ms > kScreenshotIndexTtlMs;
        if (stale && !st.screenshots_scan_pending) {
            st.screenshots_scan_pending = true;
            screenshot_generation = ++st.screenshots_scan_generation;
            screenshot_instances = st.instance_list;
            start_screenshot_scan = true;
        }
        cached_screenshots = st.screenshots_cache;
    }
    if (start_screenshot_scan) {
        spawn_worker(st, std::thread([&st, screenshot_instance,
                                      screenshot_instances = std::move(screenshot_instances),
                                      screenshot_generation]() {
            std::vector<UiState::ScreenshotIndexEntry> found;
            auto collect = [&found](const std::filesystem::path& screenshots) {
                std::error_code error;
                if (!std::filesystem::exists(screenshots, error) || error) return;
                for (std::filesystem::directory_iterator it(screenshots, error), end;
                     !error && it != end; it.increment(error)) {
                    std::error_code file_error;
                    if (!it->is_regular_file(file_error) || file_error) continue;
                    const std::wstring ext = it->path().extension().wstring();
                    if (_wcsicmp(ext.c_str(), L".png") != 0 &&
                        _wcsicmp(ext.c_str(), L".jpg") != 0 &&
                        _wcsicmp(ext.c_str(), L".jpeg") != 0) continue;
                    found.push_back({it->path().wstring()});
                }
            };
            if (screenshot_instance.empty()) {
                for (const auto& inst : screenshot_instances)
                    collect(std::filesystem::path(inst.directory) / L"screenshots");
            } else {
                collect(std::filesystem::path(screenshot_instance) / L"screenshots");
            }
            const uint64_t completed_at = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            std::lock_guard<std::mutex> lock(st.file_index_mu);
            if (st.screenshots_scan_generation == screenshot_generation) {
                st.screenshots_cache = std::move(found);
                st.screenshots_cache_instance_directory = screenshot_instance;
                st.screenshots_cache_at_ms = completed_at;
                st.screenshots_scan_pending = false;
            }
        }));
    }
    image_files.reserve(cached_screenshots.size());
    for (const auto& cached : cached_screenshots)
        image_files.emplace_back(cached.path);
    
    // Sort newest first
    std::sort(image_files.begin(), image_files.end(),
              [](const std::filesystem::path& a, const std::filesystem::path& b) {
                  return a.filename().wstring() > b.filename().wstring();
              });

    // ── Filter bar ────────────────────────────────────────────────────────
    card_begin("##screenshots_filter", ImVec2(-1, 0));
    ImGui::TextUnformatted("Filter");
    ImGui::SameLine();
    ImGui::TextColored(k.muted, "%d screenshot%s", static_cast<int>(image_files.size()),
                       image_files.size() == 1 ? "" : "s");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - ui_px(240.0f));
    
    // View toggle
    static bool grid_view = true;
    if (ghost_button(grid_view ? "Grid" : "List", ImVec2(ui_px(32.0f), ui_px(32.0f)))) {
        grid_view = !grid_view;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s view", grid_view ? "Grid" : "List");
    }
    ImGui::SameLine();
    
    // Search
    ImGui::SetNextItemWidth(ui_px(180.0f));
    input_text_hint("##screenshot_search", "Search by filename...", &st.screenshot_search);
    card_end();
    ImGui::Spacing();

    // ── Apply filter ───────────────────────────────────────────────────────
    std::vector<std::filesystem::path> filtered_files;
    if (!st.screenshot_search.empty()) {
        std::string search_lower = st.screenshot_search;
        std::transform(search_lower.begin(), search_lower.end(), search_lower.begin(),
                       [](unsigned char c) {
                           return static_cast<char>(std::tolower(c));
                       });
        for (const auto& file : image_files) {
            std::string filename = net::to_utf8(file.filename().wstring());
            std::string filename_lower = filename;
            std::transform(filename_lower.begin(), filename_lower.end(), filename_lower.begin(),
                           [](unsigned char c) {
                           return static_cast<char>(std::tolower(c));
                       });
            if (filename_lower.find(search_lower) != std::string::npos) {
                filtered_files.push_back(file);
            }
        }
    } else {
        filtered_files = image_files;
    }

    // ── Empty state ────────────────────────────────────────────────────────
    if (filtered_files.empty()) {
        empty_state("No screenshots found", 
                    st.screenshot_search.empty() ? 
                    "Press F2 in Minecraft to capture screenshots.\nThey will appear here automatically." :
                    "No screenshots match your search.",
                    "screenshots");
        return;
    }

    // ── Grid view ─────────────────────────────────────────────────────────
    if (grid_view) {
        constexpr size_t kMaxVisibleScreenshots = 96;
        const size_t visible_count = std::min(filtered_files.size(), kMaxVisibleScreenshots);
        if (filtered_files.size() > visible_count) {
            ImGui::TextColored(k.muted, "Showing %d of %d screenshots",
                              static_cast<int>(visible_count), static_cast<int>(filtered_files.size()));
            ImGui::Spacing();
        }

        const float gap = ui_px(12.0f);
        const float available = std::max(0.0f, ImGui::GetContentRegionAvail().x - ui_px(18.0f));
        const int columns = available >= ui_px(1400.0f) ? 5 :
                          available >= ui_px(1100.0f) ? 4 :
                          available >= ui_px(780.0f) ? 3 :
                          available >= ui_px(460.0f) ? 2 : 1;
        const float card_width = std::max(ui_px(220.0f),
            (available - gap * static_cast<float>(columns - 1)) / static_cast<float>(columns));
        const float image_height = std::clamp(card_width * 0.56f, ui_px(126.0f), ui_px(200.0f));
        const float card_height = image_height + ui_px(110.0f);

        for (size_t index = 0; index < visible_count; ++index) {
            const std::filesystem::path& file = filtered_files[index];
            if (index % static_cast<size_t>(columns)) ImGui::SameLine(0, gap);
            
            ImGui::PushID(static_cast<int>(index));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, k.surface2);
            ImGui::PushStyleColor(ImGuiCol_Border, k.border);
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, ui_px(9.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, ui_px(1.0f));
            ImGui::BeginChild("##screenshotcard", ImVec2(card_width, card_height), ImGuiChildFlags_Borders);

            const ImVec2 image_pos = ImGui::GetCursorScreenPos();
            const ImVec2 image_size(card_width - ui_px(2.0f), image_height);
            draw_local_image(st, file.wstring(), image_pos, image_size, c32(k.brand_dk));
            ImGui::InvisibleButton("##openpreview", image_size);
            
            if (ImGui::IsItemHovered()) {
                ImGui::GetWindowDrawList()->AddRect(image_pos, image_pos + image_size, c32(k.brand_hov),
                                                    ui_px(9.0f), 0, ui_px(1.5f));
            }
            
            // Context menu
            if (ImGui::BeginPopupContextItem("screenshot_ctx")) {
                if (ImGui::MenuItem("Open")) {
                    ShellExecuteW(st.hwnd, L"open", file.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
                if (ImGui::MenuItem("Open in Explorer")) {
                    ShellExecuteW(st.hwnd, L"open", file.parent_path().wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
                if (ImGui::MenuItem("Delete", nullptr, false, false)) {
                    // Confirmation would go here
                }
                ImGui::EndPopup();
            }
            
            if (ImGui::IsItemClicked()) {
                ShellExecuteW(st.hwnd, L"open", file.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }

            std::string filename = net::to_utf8(file.filename().wstring());
            if (filename.size() > 42) filename = filename.substr(0, 39) + "...";
            ImGui::PushFont(f_bold);
            ImGui::TextUnformatted(filename.c_str());
            ImGui::PopFont();
            
            std::error_code size_error;
            const uintmax_t bytes = std::filesystem::file_size(file, size_error);
            ImGui::TextColored(k.muted, "%s", size_error ? "Open full size" : format_bytes(bytes).c_str());
            
            if (primary_button("Open", ImVec2(-1, ui_px(30.0f)))) {
                ShellExecuteW(st.hwnd, L"open", file.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }

            ImGui::EndChild();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(2);
            ImGui::PopID();
        }
    }
    // ── List view ─────────────────────────────────────────────────────────
    else {
        const float row_height = ui_px(72.0f);
        const float thumbnail_size = ui_px(56.0f);
        
        for (size_t index = 0; index < filtered_files.size(); ++index) {
            const std::filesystem::path& file = filtered_files[index];
            
            ImGui::PushID(static_cast<int>(index));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, k.surface2);
            ImGui::PushStyleColor(ImGuiCol_Border, k.border);
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, ui_px(6.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, ui_px(1.0f));
            ImGui::BeginChild("##screenshotrow", ImVec2(-1, row_height), ImGuiChildFlags_Borders);
            
            // Thumbnail
            ImGui::BeginChild("##thumb", ImVec2(thumbnail_size, row_height), ImGuiChildFlags_None);
            const ImVec2 image_pos = ImGui::GetCursorScreenPos();
            const ImVec2 image_size(thumbnail_size - ui_px(2.0f), thumbnail_size - ui_px(2.0f));
            draw_local_image(st, file.wstring(), image_pos, image_size, c32(k.brand_dk));
            ImGui::EndChild();
            ImGui::SameLine();
            
            // Info
            ImGui::BeginChild("##info", ImVec2(0, row_height), ImGuiChildFlags_None);
            std::string filename = net::to_utf8(file.filename().wstring());
            ImGui::PushFont(f_bold);
            ImGui::TextUnformatted(filename.c_str());
            ImGui::PopFont();
            
            std::error_code size_error;
            const uintmax_t bytes = std::filesystem::file_size(file, size_error);
            ImGui::TextColored(k.muted, "%s", size_error ? "Unknown size" : format_bytes(bytes).c_str());
            
            // Parent folder
            std::string parent = net::to_utf8(file.parent_path().filename().wstring());
            if (!parent.empty()) {
                ImGui::SameLine();
                ImGui::TextColored(k.muted, "  •  %s", parent.c_str());
            }
            ImGui::EndChild();
            
            // Actions
            ImGui::SameLine();
            ImGui::BeginChild("##actions", ImVec2(ui_px(120.0f), row_height), ImGuiChildFlags_None);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (row_height - ui_px(30.0f)) * 0.5f);
            if (primary_button("Open", ImVec2(-1, ui_px(30.0f)))) {
                ShellExecuteW(st.hwnd, L"open", file.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            ImGui::EndChild();
            
            ImGui::EndChild();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(2);
            ImGui::PopID();
        }
    }
}

void draw_profiles_tab(UiState& st) {
    page_title("Profiles", "Your launcher defaults and Minecraft account connection.");
    card_begin("##profilepage", ImVec2(-1, 0));
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted("Launcher defaults");
    ImGui::PopFont();
    ImGui::Spacing();
    const std::string account_name = linked_account_name(st);
    ImGui::TextUnformatted("Minecraft authentication");
    ImGui::TextColored(k.muted,
                       "Handled by the official Minecraft Launcher. It is not required to install mods or create profiles.");
    if (account_name.empty()) {
        if (primary_button("Open official launcher", ImVec2(ui_px(190.0f), ui_px(34.0f)))) {
            if (!official_launcher::OpenOfficialLauncher()) {
                push_notice(st, ui_model::NoticeLevel::Error,
                            "Minecraft Launcher not found",
                            "Install the official Minecraft Launcher, then open it to handle Minecraft sign-in.");
            }
        }
    } else {
        ImGui::TextColored(k.green, "Connected as %s", account_name.c_str());
    }
    ImGui::Spacing();
    ImGui::TextUnformatted("Display name (optional)");
    ImGui::SetNextItemWidth(auto_item_width(360.0f, 180.0f));
    input_text_hint("##profileusername", "Shown before account connection", &st.ui_username);
    ImGui::TextUnformatted("Default loader");
    const char* loaders[] = {"auto", "vanilla", "fabric", "quilt", "forge", "neoforge"};
    int selected = 0;
    for (int i = 0; i < 6; ++i) if (st.cfg->loader == loaders[i]) selected = i;
    ImGui::SetNextItemWidth(auto_item_width(220.0f, 140.0f));
    if (ImGui::Combo("##profileloader", &selected, "auto\0vanilla\0fabric\0quilt\0forge\0neoforge\0", 6))
        st.cfg->loader = loaders[selected];
    ImGui::TextUnformatted("Default performance profile");
    const char* performance_ids[] = {"auto", "low_end", "balanced", "shaders",
                                     "heavy_modpack", "custom"};
    const char* performance_items = "Auto\0Low-end device\0Balanced\0Shaders\0Heavy modpack\0Custom\0";
    int performance_index = 0;
    const std::string current_performance = performance::normalize_profile(st.cfg->performance_profile);
    for (int i = 0; i < 6; ++i) if (current_performance == performance_ids[i]) performance_index = i;
    ImGui::SetNextItemWidth(auto_item_width(220.0f, 140.0f));
    if (ImGui::Combo("##defaultperformance", &performance_index, performance_items, 6))
        st.cfg->performance_profile = performance_ids[performance_index];
    ImGui::TextColored(k.muted, "Use Low-end for integrated graphics and small-RAM systems; Shaders keeps render distance conservative.");
    ImGui::Spacing();
    if (primary_button("Save defaults", ImVec2(ui_px(150.0f), ui_px(36.0f)))) {
        st.cfg->username = net::to_wide(st.ui_username);
        config::save(st.exe_dir + L"\\launcher.json", *st.cfg);
        log_line(st, L"[profile] saved");
    }
    card_end();
}

static std::wstring choose_java_home(HWND owner) {
    BROWSEINFOW browse{};
    browse.hwndOwner = owner;
    browse.lpszTitle = L"Select a Java installation folder";
    browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE item = SHBrowseForFolderW(&browse);
    if (!item) return {};
    wchar_t path[MAX_PATH]{};
    const bool ok = SHGetPathFromIDListW(item, path) != FALSE;
    CoTaskMemFree(item);
    if (!ok) return {};
    std::wstring home = path;
    while (!home.empty() && (home.back() == L'\\' || home.back() == L'/')) home.pop_back();
    return net::file_exists(home + L"\\bin\\java.exe") ? home : std::wstring();
}

static void start_managed_java_install(UiState& st, int major) {
    bool expected = false;
    if (!st.java_installing.compare_exchange_strong(expected, true)) return;
    st.java_install_major = major;
    st.java_install_progress = 0;
    {
        std::lock_guard<std::mutex> lock(st.java_mu);
        st.java_install_message.clear();
        st.java_install_success = false;
    }
    const std::wstring root = st.cfg && !st.cfg->java_cache_dir.empty()
        ? st.cfg->java_cache_dir
        : st.exe_dir + L"\\runtimes\\java";
    spawn_worker(st, std::thread([&st, root, major]() {
        java::JavaRuntimeManager manager(root);
        java::DownloadResult result = manager.DownloadJava(
            major, java::Architecture::X64,
            [&st](uint64_t done, uint64_t total) {
                st.java_install_progress = total > 0
                    ? static_cast<int>(std::min<uint64_t>(100, done * 100 / total))
                    : -1;
                return true;
            });
        {
            std::lock_guard<std::mutex> lock(st.java_mu);
            st.java_install_success = result.success;
            st.java_install_message = result.success
                ? "Temurin Java " + std::to_string(major) + " installed."
                : (result.error.empty() ? "Java runtime installation failed." : result.error);
        }
        st.java_installing = false;
        st.java_scanned = false;
        st.home_readiness.dirty = true;
    }));
}

void draw_java_tab(UiState& st) {
    draw_page_emblem(st, "java-emblem-ai.png");
    page_title("Java Manager", "Installed runtimes detected for Minecraft versions and loaders.");
    const std::wstring managed_java_root = st.cfg && !st.cfg->java_cache_dir.empty()
        ? st.cfg->java_cache_dir
        : st.exe_dir + L"\\runtimes\\java";
    java::JavaRuntimeManager managed_java_manager(managed_java_root);
    static int pending_managed_remove = 0;

    if (!st.java_installing) {
        std::string install_message;
        bool install_success = false;
        {
            std::lock_guard<std::mutex> lock(st.java_mu);
            install_message.swap(st.java_install_message);
            install_success = st.java_install_success;
        }
        if (!install_message.empty()) {
            push_notice(st, install_success ? ui_model::NoticeLevel::Success
                                             : ui_model::NoticeLevel::Error,
                        install_success ? "Java runtime installed" : "Java runtime installation failed",
                        install_message);
        }
    }
    
    // ── Header with quick actions ─────────────────────────────────────────
    card_begin("##java_header", ImVec2(-1, 0));
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted("Java Runtimes");
    ImGui::PopFont();
    const float java_actions_width = ui_px(370.0f);
    const float java_header_available = ImGui::GetContentRegionAvail().x;
    const bool stack_java_actions = java_header_available < java_actions_width + ui_px(260.0f);
    if (!stack_java_actions)
        ImGui::SameLine(ImGui::GetCursorPosX() + java_header_available - java_actions_width);
    else
        ImGui::Spacing();
    if (primary_button("+ Install Java", ImVec2(ui_px(140.0f), ui_px(32.0f))))
        ImGui::OpenPopup("Install Managed Java");
    if (!stack_java_actions) ImGui::SameLine();
    if (ghost_button("Rescan", ImVec2(ui_px(92.0f), ui_px(32.0f)))) {
        st.java_scanned = false;
        std::lock_guard<std::mutex> lock(st.java_mu);
        st.javas.clear();
        st.home_readiness.dirty = true;
    }
    if (!stack_java_actions) ImGui::SameLine();
    if (ghost_button("Reset defaults", ImVec2(ui_px(110.0f), ui_px(32.0f)))) {
        st.cfg->java_overrides.clear();
        config::save(st.exe_dir + L"\\launcher.json", *st.cfg);
        push_notice(st, ui_model::NoticeLevel::Success, "Java defaults reset",
                    "Minecraft will use installed or automatically downloaded runtimes.");
    }
    card_end();
    ImGui::Spacing();

    bool install_java_open = true;
    if (ImGui::BeginPopupModal("Install Managed Java", &install_java_open,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Download a portable Eclipse Temurin runtime");
        ImGui::TextColored(k.muted, "Only the selected version is downloaded.");
        ImGui::Spacing();
        for (const int major : {8, 11, 17, 21, 25}) {
            ImGui::PushID(major);
            if (primary_button(("Install Java " + std::to_string(major)).c_str(),
                               ImVec2(ui_px(180.0f), ui_px(30.0f)))) {
                start_managed_java_install(st, major);
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopID();
        }
        ImGui::Spacing();
        if (ghost_button("Cancel", ImVec2(ui_px(100.0f), ui_px(30.0f))))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (pending_managed_remove > 0 &&
        ImGui::BeginPopupModal("Remove Managed Java", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Remove managed Temurin Java %d?", pending_managed_remove);
        ImGui::TextColored(k.muted, "This does not affect system Java installations.");
        ImGui::Spacing();
        if (primary_button("Remove", ImVec2(ui_px(100.0f), ui_px(30.0f)))) {
            std::string remove_error;
            if (managed_java_manager.RemoveManagedRuntime(pending_managed_remove, &remove_error)) {
                st.cfg->java_overrides.erase(
                    std::remove_if(st.cfg->java_overrides.begin(), st.cfg->java_overrides.end(),
                        [&](const auto& item) {
                            return item.first == pending_managed_remove;
                        }), st.cfg->java_overrides.end());
                config::save(st.exe_dir + L"\\launcher.json", *st.cfg);
                st.java_scanned = false;
                push_notice(st, ui_model::NoticeLevel::Success, "Java runtime removed",
                            "The managed runtime was removed safely.");
            } else {
                push_notice(st, ui_model::NoticeLevel::Error, "Java runtime removal failed",
                            remove_error);
            }
            pending_managed_remove = 0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ghost_button("Cancel", ImVec2(ui_px(100.0f), ui_px(30.0f)))) {
            pending_managed_remove = 0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (st.java_installing) {
        card_begin("##java_install_progress", ImVec2(-1, 0));
        const int progress = st.java_install_progress.load();
        ImGui::Text("Installing Temurin Java %d", st.java_install_major);
        if (progress >= 0) ImGui::ProgressBar(progress / 100.0f, ImVec2(-1, ui_px(18.0f)));
        else ImGui::TextColored(k.muted, "Downloading runtime metadata...");
        card_end();
        ImGui::Spacing();
    }

    // ── Get snapshot ───────────────────────────────────────────────────────
    std::vector<java::Install> java_snapshot;
    {
        std::lock_guard<std::mutex> lock(st.java_mu);
        java_snapshot = st.javas;
    }

    // ── Summary card ───────────────────────────────────────────────────────
    card_begin("##java_summary", ImVec2(-1, 0));
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted("Overview");
    ImGui::PopFont();
    ImGui::Spacing();
    
    // Count by version
    std::map<int, int> version_counts;
    for (const auto& j : java_snapshot) {
        version_counts[j.major]++;
    }
    
    if (ImGui::BeginTable("##java_summary_table", 4,
                          ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoBordersInBody)) {
        const std::pair<const char*, int> summary[] = {
            {"Total Runtimes", static_cast<int>(java_snapshot.size())},
            {"Java 21", version_counts[21]},
            {"Java 17", version_counts[17]},
            {"Java 8", version_counts[8]},
        };
        for (const auto& item : summary) {
            ImGui::TableNextColumn();
            ImGui::TextColored(k.muted, "%s", item.first);
            ImGui::Text("%d", item.second);
        }
        ImGui::EndTable();
    }
    
    card_end();
    ImGui::Spacing();

    // ── Installed runtimes list ───────────────────────────────────────────
    card_begin("##java_installed", ImVec2(-1, -1));
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted("Installed Runtimes");
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::TextColored(k.muted, "%d runtime%s", static_cast<int>(java_snapshot.size()),
                       java_snapshot.size() == 1 ? "" : "s");
    ImGui::Spacing();

    if (java_snapshot.empty()) {
        empty_state("No Java runtimes detected",
                    "Scan now or install a supported runtime from Adoptium Temurin.",
                    "J");
    } else {
        // Filter bar
        ImGui::SetNextItemWidth(ui_px(240.0f));
        input_text_hint("##java_filter", "Filter by version or path...", &st.java_filter);
        ImGui::SameLine();
        
        // Sort options
        static int java_sort = 0; // 0 = version desc, 1 = version asc, 2 = path
        ImGui::SetNextItemWidth(ui_px(160.0f));
        if (ImGui::BeginCombo("##java_sort", java_sort == 0 ? "Version (Newest)" : 
                                         java_sort == 1 ? "Version (Oldest)" : "Path")) {
            if (ImGui::Selectable("Version (Newest)", java_sort == 0)) java_sort = 0;
            if (ImGui::Selectable("Version (Oldest)", java_sort == 1)) java_sort = 1;
            if (ImGui::Selectable("Path", java_sort == 2)) java_sort = 2;
            ImGui::EndCombo();
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Apply filter and sort
        std::vector<java::Install> filtered;
        std::string filter_lower = st.java_filter;
        std::transform(filter_lower.begin(), filter_lower.end(), filter_lower.begin(),
                       [](unsigned char c) {
                           return static_cast<char>(std::tolower(c));
                       });
        
        for (const auto& j : java_snapshot) {
            std::string path_str = net::to_utf8(j.home);
            std::string path_lower = path_str;
            std::transform(path_lower.begin(), path_lower.end(), path_lower.begin(),
                           [](unsigned char c) {
                           return static_cast<char>(std::tolower(c));
                       });
            
            if (filter_lower.empty() || 
                path_lower.find(filter_lower) != std::string::npos ||
                std::to_string(j.major).find(filter_lower) != std::string::npos) {
                filtered.push_back(j);
            }
        }

        // Keep the common Minecraft runtime majors visible even before they
        // are installed. Launch still resolves future majors from version
        // metadata and downloads them on demand.
        const int common_majors[] = {8, 17, 21, 25};
        for (const int major : common_majors) {
            if (!filter_lower.empty() && std::to_string(major).find(filter_lower) == std::string::npos)
                continue;
            const bool present = std::any_of(filtered.begin(), filtered.end(),
                [major](const java::Install& install) { return install.major == major; });
            if (!present) {
                java::Install missing;
                missing.major = major;
                for (const auto& override : st.cfg->java_overrides) {
                    if (override.first == major && net::file_exists(override.second + L"\\bin\\java.exe")) {
                        missing.home = override.second;
                        missing.exe = override.second + L"\\bin\\java.exe";
                        break;
                    }
                }
                filtered.push_back(std::move(missing));
            }
        }
        
        // Sort
        if (java_sort == 0) {
            std::sort(filtered.begin(), filtered.end(), [](const java::Install& a, const java::Install& b) {
                return a.major > b.major;
            });
        } else if (java_sort == 1) {
            std::sort(filtered.begin(), filtered.end(), [](const java::Install& a, const java::Install& b) {
                return a.major < b.major;
            });
        } else {
            std::sort(filtered.begin(), filtered.end(), [](const java::Install& a, const java::Install& b) {
                return net::to_utf8(a.home) < net::to_utf8(b.home);
            });
        }

        // Display
        const float row_height = ui_px(64.0f);
        for (size_t i = 0; i < filtered.size(); ++i) {
            const auto& j = filtered[i];
            ImGui::PushID(static_cast<int>(i));
            const java::JavaRuntime managed_runtime = managed_java_manager.GetRuntime(j.major);
            const bool managed = !j.home.empty() && managed_runtime.home == j.home;
            
            // Selection
            const bool selected = st.java_selected_home == j.home;
            
            // Card
            ImGui::PushStyleColor(ImGuiCol_ChildBg, selected ? k.surface : k.surface2);
            ImGui::PushStyleColor(ImGuiCol_Border, selected ? k.brand : k.border);
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, ui_px(8.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, ui_px(1.0f));
            ImGui::BeginChild("##javarow", ImVec2(-1, row_height), ImGuiChildFlags_Borders);
            
            // Version badge
            ImGui::BeginChild("##javaversion", ImVec2(ui_px(80.0f), row_height), ImGuiChildFlags_None);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (row_height - ImGui::GetTextLineHeight()) * 0.5f);
            ImGui::PushFont(f_bold);
            ImGui::Text("Java %d", j.major);
            ImGui::PopFont();
            ImGui::EndChild();
            ImGui::SameLine();
            
             // Path
             ImGui::BeginChild("##javapath", ImVec2(0, row_height), ImGuiChildFlags_None);
             ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (row_height - ImGui::GetTextLineHeight()) * 0.5f);
             if (j.home.empty()) {
                 ImGui::TextColored(k.yellow, "Not installed; launch will download it automatically");
             } else {
                 ImGui::TextColored(k.text, "%s", net::to_utf8(j.home).c_str());
             }
             ImGui::EndChild();
            
            // Default indicator
            bool is_default = false;
            for (const auto& override : st.cfg->java_overrides) {
                if (override.first == j.major && override.second == j.home) {
                    is_default = true;
                    break;
                }
            }
            ImGui::SameLine();
            ImGui::BeginChild("##javadefault", ImVec2(ui_px(100.0f), row_height), ImGuiChildFlags_None);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (row_height - ImGui::GetTextLineHeight()) * 0.5f);
             if (is_default) {
                 ImGui::TextColored(k.green, "Default");
             } else if (!j.home.empty()) {
                 ImGui::TextColored(k.muted, "Auto");
             }
            ImGui::EndChild();
            
            // Actions
            ImGui::SameLine();
             ImGui::BeginChild("##javaactions", ImVec2(ui_px(340.0f), row_height), ImGuiChildFlags_None);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (row_height - ui_px(30.0f)) * 0.5f);
            
               if (j.home.empty()) {
                   if (ghost_button("Install", ImVec2(ui_px(80.0f), ui_px(30.0f)))) {
                       start_managed_java_install(st, j.major);
                   }
                  ImGui::SameLine();
                  if (ghost_button("Folder", ImVec2(ui_px(72.0f), ui_px(30.0f)))) {
                      const std::wstring selected_home = choose_java_home(st.hwnd);
                      if (!selected_home.empty()) {
                          bool replaced = false;
                          for (auto& override : st.cfg->java_overrides) {
                              if (override.first == j.major) {
                                  override.second = selected_home;
                                  replaced = true;
                                  break;
                              }
                          }
                          if (!replaced) st.cfg->java_overrides.emplace_back(j.major, selected_home);
                          config::save(st.exe_dir + L"\\launcher.json", *st.cfg);
                          st.java_scanned = false;
                      } else {
                          push_notice(st, ui_model::NoticeLevel::Warning, "Invalid Java folder",
                                      "Select a folder containing bin\\java.exe.");
                      }
                  }
              } else if (ghost_button("Select", ImVec2(ui_px(72.0f), ui_px(30.0f)))) {
                  st.java_selected_major = j.major;
                  st.java_selected_home = j.home;
              }
              if (!j.home.empty()) {
                 ImGui::SameLine();
                 if (ghost_button("Folder", ImVec2(ui_px(72.0f), ui_px(30.0f)))) {
                      ShellExecuteW(st.hwnd, L"open", j.home.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                  }
             }
             if (!j.home.empty()) {
                 ImGui::SameLine();
                 if (primary_button("Set Default", ImVec2(ui_px(92.0f), ui_px(30.0f)))) {
                     bool replaced = false;
                     for (auto& override : st.cfg->java_overrides) {
                         if (override.first == j.major) {
                             override.second = j.home;
                             replaced = true;
                             break;
                         }
                     }
                     if (!replaced) st.cfg->java_overrides.emplace_back(j.major, j.home);
                     config::save(st.exe_dir + L"\\launcher.json", *st.cfg);
                     push_notice(st, ui_model::NoticeLevel::Success, "Java default updated",
                                 "Minecraft version " + std::to_string(j.major) +
                                 " will use the selected runtime.");
                 }
                  if (is_default) {
                     ImGui::SameLine();
                     if (ghost_button("Reset", ImVec2(ui_px(62.0f), ui_px(30.0f)))) {
                         st.cfg->java_overrides.erase(
                             std::remove_if(st.cfg->java_overrides.begin(), st.cfg->java_overrides.end(),
                                 [&](const auto& item) { return item.first == j.major; }),
                             st.cfg->java_overrides.end());
                         config::save(st.exe_dir + L"\\launcher.json", *st.cfg);
                      }
                  }
                  if (managed) {
                      ImGui::SameLine();
                      if (ghost_button("Remove", ImVec2(ui_px(72.0f), ui_px(30.0f)))) {
                          pending_managed_remove = j.major;
                          ImGui::OpenPopup("Remove Managed Java");
                      }
                  }
             } else {
                 ImGui::TextColored(k.muted, "Auto");
             }
            ImGui::EndChild();
            
            ImGui::EndChild();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(2);
            ImGui::PopID();
            
            if (selected) {
                ImGui::GetWindowDrawList()->AddRect(
                    ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                    c32(k.brand), ui_px(8.0f), 0, ui_px(1.5f));
            }
        }
    }
    card_end();
}

static std::string normalize_loader_name(const std::string& raw) {
    std::string s = raw;
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (s == "fabric" || s == "quilt") return s;
    if (s == "forge") return s;
    if (s == "neoforge" || s == "neo forge" || s == "neoforged") return "neoforge";
    if (s == "liteloader") return "vanilla";
    return "";
}

// ---------------------------------------------------------------------------
// Backups Tool
// ---------------------------------------------------------------------------

void draw_backups_tab(UiState& st) {
    page_title("Backups", "Manage profile backups and restore points.");
    instances::Instance backup_instance = st.selected_instance;
    if (backup_instance.directory.empty() && !st.instance_list.empty())
        backup_instance = st.instance_list.front();
    std::string backup_error;
    const auto backup_entries = backup_instance.directory.empty()
        ? std::vector<instances::BackupEntry>{}
        : instances::list_restore_points(backup_instance, &backup_error);
    
    card_begin("##backups_header", ImVec2(-1, 0));
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted("Backup Manager");
    ImGui::PopFont();
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - ui_px(300.0f));
    
    if (primary_button("+ Create Backup", ImVec2(ui_px(130.0f), ui_px(32.0f)))) {
        if (backup_instance.directory.empty()) {
            push_notice(st, ui_model::NoticeLevel::Warning, "No profile selected",
                        "Select a profile before creating a backup.");
        } else {
            std::wstring output;
            if (instances::create_restore_point(backup_instance, &output, &backup_error))
                push_notice(st, ui_model::NoticeLevel::Success, "Backup created",
                            net::to_utf8(output));
            else
                push_notice(st, ui_model::NoticeLevel::Error, "Backup failed", backup_error);
        }
    }
    ImGui::SameLine();
    if (ghost_button("Refresh", ImVec2(ui_px(90.0f), ui_px(32.0f)))) {
        st.instances_loaded = false;
    }
    ImGui::SameLine();
    if (ghost_button("Settings", ImVec2(ui_px(70.0f), ui_px(32.0f)))) {
        st.settings_section = 4;
        st.sidebar_item = 4;
    }
    card_end();
    ImGui::Spacing();

    card_begin("##backups_summary", ImVec2(-1, 0));
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted("Storage");
    ImGui::PopFont();
    ImGui::Spacing();
    
    uint64_t total_size = 0;
    const int backup_count = static_cast<int>(backup_entries.size());
    for (const auto& backup : backup_entries) total_size += backup.size_bytes;
    
    ImGui::TextColored(k.muted, "Total Backups");
    ImGui::SameLine(ui_px(150.0f));
    ImGui::Text("%d", backup_count);
    
    ImGui::SameLine(ui_px(250.0f));
    ImGui::TextColored(k.muted, "Total Size");
    ImGui::SameLine(ui_px(350.0f));
    ImGui::Text("%s", format_bytes(total_size).c_str());
    
    card_end();
    ImGui::Spacing();

    card_begin("##backups_list", ImVec2(-1, -1));
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted("All Backups");
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::TextColored(k.muted, "%d backup%s", backup_count, backup_count == 1 ? "" : "s");
    ImGui::Spacing();

    if (!backup_error.empty()) {
        ImGui::TextColored(k.red, "%s", backup_error.c_str());
    } else if (backup_count == 0) {
        empty_state("No backups yet",
                    backup_instance.directory.empty()
                        ? "Select a profile, then create a restore point."
                        : "Create your first backup to protect your profile configuration and mods.",
                    "backup");
    } else {
        ImGui::SetNextItemWidth(ui_px(240.0f));
        input_text_hint("##backup_filter", "Filter by name...", &st.backup_filter);
        ImGui::SameLine();
        
        static int backup_sort = 0;
        ImGui::SetNextItemWidth(ui_px(160.0f));
        if (ImGui::BeginCombo("##backup_sort", backup_sort == 0 ? "Newest" : 
                                         backup_sort == 1 ? "Oldest" :
                                         backup_sort == 2 ? "Largest" : "Name")) {
            if (ImGui::Selectable("Newest", backup_sort == 0)) backup_sort = 0;
            if (ImGui::Selectable("Oldest", backup_sort == 1)) backup_sort = 1;
            if (ImGui::Selectable("Largest", backup_sort == 2)) backup_sort = 2;
            if (ImGui::Selectable("Name", backup_sort == 3)) backup_sort = 3;
            ImGui::EndCombo();
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::BeginTable("##restore_points", 4,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Restore point", 0, 0.45f);
            ImGui::TableSetupColumn("Modified", 0, 0.25f);
            ImGui::TableSetupColumn("Size", 0, 0.15f);
            ImGui::TableSetupColumn("Action", 0, 0.15f);
            ImGui::TableHeadersRow();
            for (const auto& backup : backup_entries) {
                if (!st.backup_filter.empty() && backup.name.find(st.backup_filter) == std::string::npos)
                    continue;
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(backup.name.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(k.muted, "%s", format_date(backup.modified_at).c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::TextColored(k.muted, "%s", format_bytes(backup.size_bytes).c_str());
                ImGui::TableSetColumnIndex(3);
                ImGui::PushID(backup.path.c_str());
                if (ghost_button("Delete", ImVec2(ui_px(68.0f), ui_px(24.0f)))) {
                    std::string remove_error;
                    if (instances::remove_restore_point(backup_instance, backup.path, &remove_error))
                        push_notice(st, ui_model::NoticeLevel::Success, "Backup deleted", backup.name);
                    else
                        push_notice(st, ui_model::NoticeLevel::Error, "Delete failed", remove_error);
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::Spacing();
        if (primary_button("Restore Latest", ImVec2(ui_px(140.0f), ui_px(30.0f)))) {
            std::string restore_error;
            if (instances::restore_latest(backup_instance, &restore_error))
                push_notice(st, ui_model::NoticeLevel::Success, "Profile restored",
                            "The latest restore point was applied.");
            else
                push_notice(st, ui_model::NoticeLevel::Error, "Restore failed", restore_error);
        }
    }
    card_end();
}

// ---------------------------------------------------------------------------
// Logs Tool
// ---------------------------------------------------------------------------

void draw_logs_tab(UiState& st) {
    page_title("Logs", "View and manage launcher and game logs.");
    
    card_begin("##logs_header", ImVec2(-1, 0));
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted("Log Viewer");
    ImGui::PopFont();
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - ui_px(250.0f));
    
    if (primary_button("Open Logs Folder", ImVec2(ui_px(150.0f), ui_px(32.0f)))) {
        std::filesystem::path logs_dir = std::filesystem::path(st.exe_dir) / L"logs";
        std::error_code ec;
        std::filesystem::create_directories(logs_dir, ec);
        if (!ec) {
            ShellExecuteW(st.hwnd, L"open", logs_dir.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }
    ImGui::SameLine();
    if (ghost_button("Clear", ImVec2(ui_px(80.0f), ui_px(32.0f)))) {
        std::lock_guard<std::mutex> lock(st.log_mu);
        st.logs.clear();
        st.log_revision.fetch_add(1, std::memory_order_relaxed);
    }
    card_end();
    ImGui::Spacing();

    card_begin("##logs_sources", ImVec2(-1, 0));
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted("Log Sources");
    ImGui::PopFont();
    ImGui::Spacing();
    
    static int log_source = 0; // 0 latest, 1 profile, 2 launcher, 3 game, 4 debug
    if (ImGui::BeginTabBar("##log_source_tabs")) {
        if (ImGui::BeginTabItem("Latest")) { log_source = 0; ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Profile")) { log_source = 1; ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Launcher")) { log_source = 2; ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Game")) { log_source = 3; ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Debug")) { log_source = 4; ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::Spacing();
    card_end();
    ImGui::Spacing();

    card_begin("##logs_viewer", ImVec2(-1, -1));
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted(log_source == 0 ? "Latest Activity" :
                         log_source == 1 ? "Profile / Install Log" :
                         log_source == 2 ? "Launcher Log" :
                         log_source == 3 ? "Game Log" : "Debug Log");
    ImGui::PopFont();
    ImGui::Spacing();

    ImGui::SetNextItemWidth(ui_px(240.0f));
    input_text_hint("##log_filter", "Filter...", &st.log_filter);
    ImGui::SameLine();
    
    static bool auto_scroll = true;
    ImGui::Checkbox("Auto-scroll", &auto_scroll);
    ImGui::SameLine();
    
    if (ghost_button("Clear", ImVec2(ui_px(80.0f), ui_px(30.0f)))) {
        std::lock_guard<std::mutex> lock(st.log_mu);
        st.logs.clear();
        st.log_revision.fetch_add(1, std::memory_order_relaxed);
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    const uint64_t log_revision = st.log_revision.load(std::memory_order_relaxed);
    if (st.log_cache_revision != log_revision || st.log_cache_source != log_source ||
        st.log_cache_filter != st.log_filter) {
        std::vector<std::string> snapshot;
        {
            std::lock_guard<std::mutex> lock(st.log_mu);
            snapshot.assign(st.logs.begin(), st.logs.end());
        }
        std::string filter_lower = st.log_filter;
        std::transform(filter_lower.begin(), filter_lower.end(), filter_lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        st.log_cache_filtered.clear();
        for (const auto& log : snapshot) {
            if (log_source == 1 && log.find("[profile]") == std::string::npos &&
                log.find("[mods]") == std::string::npos &&
                log.find("[modpack]") == std::string::npos &&
                log.find("[installer]") == std::string::npos &&
                log.find("[job]") == std::string::npos) continue;
            if (log_source == 2 && (log.find("[profile]") != std::string::npos ||
                                    log.find("[mods]") != std::string::npos ||
                                    log.find("[modpack]") != std::string::npos ||
                                    log.find("[installer]") != std::string::npos ||
                                    log.find("[job]") != std::string::npos)) continue;
            if (filter_lower.empty()) {
                st.log_cache_filtered.push_back(log);
            } else {
                std::string log_lower = log;
                std::transform(log_lower.begin(), log_lower.end(), log_lower.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (log_lower.find(filter_lower) != std::string::npos)
                    st.log_cache_filtered.push_back(log);
            }
        }
        st.log_cache_revision = log_revision;
        st.log_cache_source = log_source;
        st.log_cache_filter = st.log_filter;
    }

    ImGui::BeginChild("##log_content", ImVec2(-1, -1), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    
    if (st.log_cache_filtered.empty()) {
        ImGui::TextColored(k.muted, "No log entries yet.");
    } else {
        const auto& filtered_logs = st.log_cache_filtered;
        
        const size_t latest_start = log_source == 0 && filtered_logs.size() > 80
            ? filtered_logs.size() - 80 : 0;
        for (size_t i = latest_start; i < filtered_logs.size(); ++i) {
            // Level-based coloring so errors/warnings stand out at a glance.
            const std::string& line = filtered_logs[i];
            ImVec4 line_color = k.muted;
            if (line.find("[error]") != std::string::npos ||
                line.find("ERROR") != std::string::npos ||
                line.find("SEVERE") != std::string::npos ||
                line.find("failed") != std::string::npos)
                line_color = k.red;
            else if (line.find("[warn]") != std::string::npos ||
                     line.find("WARN") != std::string::npos)
                line_color = k.yellow;
            else if (line.find("[ok]") != std::string::npos ||
                     line.find("success") != std::string::npos)
                line_color = k.green;
            else if (line.find("[info]") != std::string::npos)
                line_color = k.text;
            ImGui::TextColored(line_color, "%s", line.c_str());
        }
        
        if (auto_scroll) {
            ImGui::SetScrollHereY(1.0f);
        }
    }
    
    ImGui::EndChild();
    card_end();
}

// ---------------------------------------------------------------------------
// Configuration Tool
// ---------------------------------------------------------------------------

void draw_config_tab(UiState& st) {
    config::Config& c = *st.cfg;
    page_title("Configuration", "Manage launcher settings and preferences.");
    
    card_begin("##config_header", ImVec2(-1, 0));
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted("Configuration Editor");
    ImGui::PopFont();
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - ui_px(200.0f));
    
    if (primary_button("Save", ImVec2(ui_px(80.0f), ui_px(32.0f)))) {
        if (config::save(st.exe_dir + L"\\launcher.json", *st.cfg)) {
            push_notice(st, ui_model::NoticeLevel::Success, "Configuration saved", "");
        } else {
            const char* detail = st.cfg->has_unreadable_secrets
                ? "A protected credential belongs to another Windows account."
                : "Windows could not protect or write the launcher settings.";
            push_notice(st, ui_model::NoticeLevel::Error,
                        "Configuration could not be saved", detail);
        }
    }
    ImGui::SameLine();
    if (ghost_button("Reset", ImVec2(ui_px(80.0f), ui_px(32.0f)))) {
        c.theme = "default_dark";
        c.language = "en";
        c.perf_max_memory_mb = 4096;
        c.extra_jvm.clear();
        st.settings_dirty = true;
        push_notice(st, ui_model::NoticeLevel::Info, "Configuration reset",
                    "Quick launcher settings were reset without changing accounts or provider credentials.");
    }
    card_end();
    ImGui::Spacing();

    card_begin("##config_quick", ImVec2(-1, 0));
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted("Quick Settings");
    ImGui::PopFont();
    ImGui::Spacing();
    
    ImGui::Columns(2, "##config_cols", false);
    
    ImGui::TextUnformatted("Theme");
    const char* themes[] = {"System", "Light", "Dark"};
    int current_theme = c.theme == "default_light" ? 1 : 0;
    ImGui::SetNextItemWidth(ui_px(160.0f));
    if (ImGui::Combo("##theme", &current_theme, "System\0Light\0Dark\0", 3)) {
        c.theme = current_theme == 1 ? "default_light" : "default_dark";
        st.settings_dirty = true;
    }
    
    ImGui::TextUnformatted("Language");
    ImGui::SetNextItemWidth(ui_px(160.0f));
    int language_index = c.language == "en" ? 0 : 1;
    if (ImGui::Combo("##language", &language_index, "English\0Spanish\0", 2)) {
        c.language = language_index == 0 ? "en" : "es";
        st.settings_dirty = true;
    }
    
    ImGui::NextColumn();
    ImGui::TextUnformatted("Max Memory (GB)");
    int memory_gb = std::max(1, c.perf_max_memory_mb / 1024);
    ImGui::SetNextItemWidth(ui_px(100.0f));
    if (ImGui::InputInt("##max_memory", &memory_gb, 1, 1)) {
        if (memory_gb < 1) memory_gb = 1;
        if (memory_gb > 32) memory_gb = 32;
        c.perf_max_memory_mb = memory_gb * 1024;
        st.settings_dirty = true;
    }
    
    ImGui::TextUnformatted("JVM Arguments");
    ImGui::SetNextItemWidth(ui_px(200.0f));
    if (ImGui::InputText("##jvm_args", &c.extra_jvm)) st.settings_dirty = true;
    
    ImGui::Columns(1);
    card_end();
    ImGui::Spacing();

    card_begin("##config_advanced", ImVec2(-1, -1));
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted("Advanced Configuration");
    ImGui::PopFont();
    ImGui::Spacing();
    
    ImGui::TextColored(k.muted, "Protected credentials are managed through the dedicated settings pages.");
    ImGui::Spacing();
    
    ImGui::BeginChild("##config_editor", ImVec2(-1, ui_px(300.0f)), ImGuiChildFlags_Borders);
    ImGui::TextUnformatted("Current configuration");
    ImGui::Separator();
    ImGui::Text("Theme: %s", c.theme.c_str());
    ImGui::Text("Language: %s", c.language.c_str());
    ImGui::Text("Loader: %s", c.loader.c_str());
    ImGui::Text("Base directory: %s", net::to_utf8(c.base_dir).c_str());
    ImGui::Text("Assets directory: %s", net::to_utf8(c.assets_dir).c_str());
    ImGui::Text("Java runtimes: %s", net::to_utf8(c.java_cache_dir).c_str());
    ImGui::Text("Provider credentials: %s", (c.has_unreadable_secrets ? "recovery required" : "protected"));
    ImGui::Spacing();
    ImGui::TextColored(k.muted, "Use the Launcher, Java, Performance, Modpacks, and Admin sections to edit these values safely.");
    ImGui::EndChild();
    
    card_end();
}


void do_wizard_resolve(UiState& st, const std::string& slug, const std::string& source) {
    st.wizard_resolving = true;
    {
        std::lock_guard<std::mutex> lock(st.wizard_resolve_mu);
        st.wizard_resolve_error.clear();
        st.wizard_resolved_loaders.clear();
        st.wizard_resolved_versions.clear();
        st.wizard_resolved_title.clear();
        st.wizard_resolved_icon.clear();
    }
    mods::ApiCfg cfg = provider_config::make(*st.cfg);
    mods::ModInfo info;
    std::string error;
    bool ok = mods::project_files(cfg, slug, source, info, &error);
    {
        std::lock_guard<std::mutex> lock(st.wizard_resolve_mu);
        if (ok) {
            st.wizard_resolved_title = info.title;
            st.wizard_resolved_icon = info.icon_url;
            std::set<std::string> loaders_set;
            for (const auto& l : info.loaders) {
                std::string n = normalize_loader_name(l);
                if (!n.empty()) loaders_set.insert(n);
            }
            for (const auto& f : info.files) {
                for (const auto& l : f.loaders) {
                    std::string n = normalize_loader_name(l);
                    if (!n.empty()) loaders_set.insert(n);
                }
                for (const auto& v : f.game_versions) {
                    if (!v.empty() && v.size() <= 8) st.wizard_resolved_versions.push_back(v);
                }
            }
            st.wizard_resolved_loaders.assign(loaders_set.begin(), loaders_set.end());
            std::sort(st.wizard_resolved_versions.begin(), st.wizard_resolved_versions.end(),
                      [](const std::string& a, const std::string& b) { return a > b; });
            st.wizard_resolved_versions.erase(
                std::unique(st.wizard_resolved_versions.begin(), st.wizard_resolved_versions.end()),
                st.wizard_resolved_versions.end());
            if (st.wizard_resolved_versions.size() > 20)
                st.wizard_resolved_versions.resize(20);
        } else {
            st.wizard_resolve_error = error.empty() ? "Could not load project details." : error;
        }
    }
    st.wizard_resolving = false;
}

void draw_pack_wizard(UiState& st) {
    const bool account_modal_open =
        ImGui::IsPopupOpen("Amalgam Account Setup") ||
        ImGui::IsPopupOpen("Sign In to Amalgam") ||
        ImGui::IsPopupOpen("Microsoft Sign In");
    if (account_modal_open) {
        st.wizard_open = false;
        if (ImGui::BeginPopupModal("Create Profile", nullptr, ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        return;
    }
    if (st.wizard_open) {
        ImGui::OpenPopup("Create Profile");
        st.wizard_preset = -1;
        {
            std::lock_guard<std::mutex> lock(st.wizard_resolve_mu);
            st.wizard_resolved_loaders.clear();
            st.wizard_resolved_versions.clear();
            st.wizard_resolve_error.clear();
            st.wizard_resolved_title.clear();
            st.wizard_resolved_icon.clear();
            st.wizard_last_project.clear();
        }
        st.wizard_open = false;
    }
    set_next_adaptive_window(840.0f, 660.0f, 640.0f, 500.0f);
    bool open = true;
    if (!ImGui::BeginPopupModal("Create Profile", &open, ImGuiWindowFlags_None)) return;
    if (!open) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    // New profiles are deliberately limited to targets with a maintained
    // Amalgam launch path. Existing/imported profiles remain untouched.
    const bool wizard_is_import = st.wizard_source == 4;
    if (!wizard_is_import &&
        !version_catalog::supports(st.wizard_loader, st.wizard_version)) {
        st.wizard_version = version_catalog::default_version(st.wizard_loader);
        if (st.wizard_version.empty())
            st.wizard_version = version_catalog::default_version();
    }

    const float w = ImGui::GetContentRegionAvail().x;
    const float pad = ui_px(24.0f);
    const ImVec4 col_done = k.green;
    const ImVec4 col_active = k.brand_hov;
    const ImVec4 col_future = k.muted;
    const ImVec4 col_text_done = k.green;
    const ImVec4 col_text_active = k.text;
    const ImVec4 col_text_future = k.muted;
    const ImVec4 col_error = k.red;

    {
        card_begin("##profile_wizard_hero", ImVec2(-1, ui_px(86.0f)));
        const ImVec2 hero_origin = ImGui::GetCursorScreenPos();
        draw_local_image(st, st.exe_dir + L"\\branding\\ai\\wizard-profile-ai-v2.png",
                         hero_origin, ImGui::GetWindowSize(),
                         c32(ImVec4(k.text.x, k.text.y, k.text.z, 0.18f)),
                         ui_model::ImageFit::Cover);
        ImDrawList* hero_draw = ImGui::GetWindowDrawList();
        hero_draw->AddCircleFilled(hero_origin + ImVec2(ui_px(28.0f), ui_px(28.0f)), ui_px(24.0f),
                                   c32(ImVec4(k.brand_dk.x, k.brand_dk.y, k.brand_dk.z, 0.90f)));
        draw_brand_mark(hero_draw, hero_origin + ImVec2(ui_px(28.0f), ui_px(28.0f)), ui_px(1.0f));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ui_px(62.0f));
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("Create a launchable profile");
        ImGui::PopFont();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ui_px(62.0f));
        ImGui::TextColored(k.muted, "Build an isolated profile with a validated source and performance plan.");
        card_end();
    }
    ImGui::Spacing();
    ImGui::Spacing();

    // Step indicator with circles and connecting lines
    {
        const char* step_labels[] = {"Source", "Target", "Performance", "Review"};
        const float circle_r = ui_px(13.0f);
        const float line_h = ui_px(2.0f);
        const float total_w = w - pad * 2.0f;
        const float segment = total_w / 3.0f;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float cy = ImGui::GetCursorScreenPos().y + circle_r + ui_px(4.0f);
        const float sx = ImGui::GetCursorScreenPos().x + pad;

        for (int i = 0; i < 4; ++i) {
            float cx = sx + segment * i;
            bool done = i < st.wizard_step;
            bool active = i == st.wizard_step;
            ImVec4 cc = done ? col_done : active ? col_active : col_future;
            ImVec4 tc = done ? col_text_done : active ? col_text_active : col_text_future;

            if (i > 0) {
                float prev_x = sx + segment * (i - 1);
                ImVec4 lc = (i <= st.wizard_step) ? col_done : col_future;
                dl->AddLine(ImVec2(prev_x + circle_r + ui_px(2.0f), cy),
                            ImVec2(cx - circle_r - ui_px(2.0f), cy),
                            ImGui::GetColorU32(lc), line_h);
            }
            dl->AddCircleFilled(ImVec2(cx, cy), circle_r, ImGui::GetColorU32(cc));
            dl->AddCircle(ImVec2(cx, cy), circle_r, ImGui::GetColorU32(ImVec4(1,1,1,0.15f)), 0, ui_px(1.0f));
            char num[2] = {};
            num[0] = static_cast<char>('1' + i);
            ImVec2 ts = ImGui::CalcTextSize(num);
            dl->AddText(ImVec2(cx - ts.x * 0.5f, cy - ts.y * 0.5f),
                        ImGui::GetColorU32(tc), num);

            ImVec2 lt = ImGui::CalcTextSize(step_labels[i]);
            dl->AddText(ImVec2(cx - lt.x * 0.5f, cy + circle_r + ui_px(6.0f)),
                        ImGui::GetColorU32(tc), step_labels[i]);
        }
        ImGui::SetCursorPosY(cy + circle_r + ui_px(6.0f) + ImGui::GetTextLineHeight() + ui_px(12.0f));
    }

    ImGui::Separator();
    ImGui::Spacing();
    ImGui::Spacing();

    // Scrollable content area
    ImGui::BeginChild("##wizard_content", ImVec2(0, -ui_px(56.0f)));

    if (st.wizard_step == 0) {
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("Choose a starting point");
        ImGui::PopFont();
        ImGui::TextColored(k.muted, "Pick a template or configure from scratch. Every option stays isolated and reversible.");
        ImGui::Spacing();
        ImGui::Spacing();

        // Quick start cards — 2 per row
        struct WizardPreset { const char* label; const char* desc; int source; const char* loader; };
        const WizardPreset presets[] = {
            {"Clean Vanilla",    "Fresh Minecraft with no mods installed",       0, "vanilla"},
            {"Fabric Mods",      "Browse Modrinth modpacks with Fabric loader",  1, "fabric"},
            {"Forge Mods",       "Browse CurseForge modpacks with Forge loader", 2, "forge"},
            {"Import Pack",      "Import an existing .mrpack or CurseForge zip", 4, "fabric"},
        };
        const float card_w = (w - pad * 2.0f - ui_px(12.0f)) * 0.5f;
        const float card_h = ui_px(52.0f);
        for (int i = 0; i < 4; i += 2) {
            for (int j = 0; j < 2; ++j) {
                int idx = i + j;
                if (j) ImGui::SameLine(0, ui_px(12.0f));
                bool active = (st.wizard_preset == idx);
                ImVec4 bg = active ? k.brand_dk : k.surface;
                ImVec4 border = active ? k.brand : k.border;
                ImGui::PushStyleColor(ImGuiCol_ChildBg, bg);
                ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, ui_px(10.0f));
                ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, active ? 2.0f : 1.0f);
                ImGui::PushStyleColor(ImGuiCol_Border, border);
                std::string cid = "##preset_" + std::to_string(idx);
                ImGui::BeginChild(cid.c_str(), ImVec2(card_w, card_h), ImGuiChildFlags_Borders);
                ImGui::SetCursorPosY(ui_px(10.0f));
                ImGui::SetCursorPosX(ui_px(14.0f));
                ImGui::PushFont(f_bold);
                ImGui::TextUnformatted(presets[idx].label);
                ImGui::PopFont();
                ImGui::SetCursorPosX(ui_px(14.0f));
                ImGui::TextColored(k.muted, "%s", presets[idx].desc);
                ImGui::EndChild();
                if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(0)) {
                    st.wizard_preset = idx;
                    st.wizard_source = presets[idx].source;
                    st.wizard_loader = presets[idx].loader;
                    if (st.wizard_source != 4 &&
                        !version_catalog::supports(st.wizard_loader, st.wizard_version))
                        st.wizard_version = version_catalog::default_version(st.wizard_loader);
                    if (st.wizard_name.empty()) st.wizard_name = presets[idx].label;
                }
                ImGui::PopStyleColor(2);
                ImGui::PopStyleVar(2);
            }
        }
        ImGui::Spacing();
        ImGui::Spacing();

        ImGui::PushFont(f_bold);
        ImGui::TextUnformatted("Profile name");
        ImGui::PopFont();
        ImGui::SetNextItemWidth(-1);
        input_text("##wizardname", &st.wizard_name);
        if (st.wizard_name.empty()) {
            ImGui::TextColored(col_error, "A name is required.");
        }
        ImGui::Spacing();
        ImGui::Spacing();

        ImGui::PushFont(f_bold);
        ImGui::TextUnformatted("Content source");
        ImGui::PopFont();
        ImGui::TextColored(k.muted, "Choose where mods and content come from.");
        ImGui::Spacing();

        const char* source_names[] = {"Vanilla", "Modrinth", "CurseForge", "AI co-pilot", "Import archive"};
        const char* source_desc[] = {
            "No downloaded content",
            "Modrinth project slug",
            "CurseForge project ID",
            "Describe and plan with AI",
            "Import .mrpack or .zip"
        };
        const float btn_w = (w - pad * 2.0f - ui_px(8.0f) * 4.0f) / 5.0f;
        const float btn_h = ui_px(56.0f);
        for (int i = 0; i < 5; ++i) {
            if (i) ImGui::SameLine(0, ui_px(8.0f));
            bool active = (st.wizard_source == i);
            ImVec4 bg = active ? k.brand_dk : k.surface;
            ImVec4 border = active ? k.brand : k.border;
            ImGui::PushStyleColor(ImGuiCol_ChildBg, bg);
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, ui_px(8.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, active ? 2.0f : 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Border, border);
            std::string sid = "##src_" + std::to_string(i);
            ImGui::BeginChild(sid.c_str(), ImVec2(btn_w, btn_h), ImGuiChildFlags_Borders);
            ImGui::SetCursorPosY(ui_px(8.0f));
            ImGui::SetCursorPosX(ui_px(8.0f));
            ImGui::PushFont(f_bold);
            ImGui::TextUnformatted(source_names[i]);
            ImGui::PopFont();
            ImGui::SetCursorPosX(ui_px(8.0f));
            ImGui::PushFont(f_small);
            ImGui::TextColored(k.muted, "%s", source_desc[i]);
            ImGui::PopFont();
            ImGui::EndChild();
            if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(0)) {
                st.wizard_source = i;
                st.wizard_preset = -1;
            }
            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar(2);
        }

        ImGui::Spacing();
        ImGui::Spacing();

        if (st.wizard_source == 1 || st.wizard_source == 2) {
            const char* field_label = st.wizard_source == 1 ? "Modrinth project slug" : "CurseForge project ID";
            ImGui::PushFont(f_bold);
            ImGui::TextUnformatted(field_label);
            ImGui::PopFont();
            ImGui::SetNextItemWidth(-1);
            if (input_text("##wizardproject", &st.wizard_project)) {
                std::lock_guard<std::mutex> lock(st.wizard_resolve_mu);
                st.wizard_resolved_loaders.clear();
                st.wizard_resolved_versions.clear();
                st.wizard_resolve_error.clear();
                st.wizard_resolved_title.clear();
                st.wizard_resolved_icon.clear();
                st.wizard_last_project.clear();
            }
            if (!st.wizard_project.empty() && st.wizard_project != st.wizard_last_project && !st.wizard_resolving) {
                st.wizard_last_project = st.wizard_project;
                std::string src = st.wizard_source == 1 ? "modrinth" : "curseforge";
                spawn_worker(st, std::thread(do_wizard_resolve, std::ref(st), st.wizard_project, src));
            }
            ImGui::Spacing();
            if (st.wizard_resolving) {
                ImGui::TextColored(k.brand_hov, "Resolving project...");
            } else if (!st.wizard_resolve_error.empty()) {
                ImGui::TextColored(col_error, "%s", humanize_error(st.wizard_resolve_error).c_str());
            } else if (!st.wizard_resolved_title.empty()) {
                ImGui::TextColored(k.green, "Found: %s", st.wizard_resolved_title.c_str());
                {
                    std::lock_guard<std::mutex> lock(st.wizard_resolve_mu);
                    if (!st.wizard_resolved_loaders.empty()) {
                        std::string loaders_str = "Loaders: ";
                        for (size_t i = 0; i < st.wizard_resolved_loaders.size(); ++i) {
                            if (i) loaders_str += ", ";
                            loaders_str += st.wizard_resolved_loaders[i];
                        }
                        ImGui::TextColored(k.muted, "%s", loaders_str.c_str());
                    }
                    if (!st.wizard_resolved_versions.empty()) {
                        ImGui::TextColored(k.muted, "Versions: %s (+%d more)",
                                           st.wizard_resolved_versions[0].c_str(),
                                           (int)st.wizard_resolved_versions.size() - 1);
                    }
                }
            }
        }
        if (st.wizard_source == 3) {
            ImGui::PushFont(f_bold);
            ImGui::TextUnformatted("AI co-pilot");
            ImGui::PopFont();
            ImGui::TextColored(k.muted, "Describe the experience you want and the AI will plan compatible content.");
            ImGui::SetNextItemWidth(-1);
            input_text("##wizardprompt", &st.wizard_prompt);
            if (st.wizard_prompt.empty()) {
                ImGui::TextColored(col_error, "A description is required for AI planning.");
            }
            bool ai_ready = !st.pack_building && !st.wizard_prompt.empty() && !st.wizard_version.empty();
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ai_ready ? 1.0f : 0.5f);
            if (primary_button(st.pack_building ? "Planning..." : "Plan with AI", ImVec2(ui_px(180.0f), ui_px(34.0f))) && ai_ready) {
                st.pack_prompt = st.wizard_prompt;
                spawn_worker(st, std::thread(do_pack_build, std::ref(st)));
            }
            ImGui::PopStyleVar();
            std::string pack_summary = pack_summary_snapshot(st);
            if (!pack_summary.empty()) {
                ImGui::Spacing();
                ImGui::TextColored(k.muted, "%s", pack_summary.c_str());
            }
        } else if (st.wizard_source == 4) {
            ImGui::PushFont(f_bold);
            ImGui::TextUnformatted("Archive file");
            ImGui::PopFont();
            ImGui::TextColored(k.muted, "Select a Modrinth .mrpack or CurseForge .zip archive.");
            ImGui::Spacing();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ui_px(116.0f));
            input_text("##wizardarchive", &st.wizard_archive);
            ImGui::SameLine();
            if (ghost_button("Browse...", ImVec2(ui_px(100.0f), ui_px(30.0f)))) show_open_archive(st, st.wizard_archive);
            if (st.wizard_archive.empty()) {
                ImGui::TextColored(col_error, "Select an archive file to import.");
            }
        }
    } else if (st.wizard_step == 1) {
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("Choose Minecraft version");
        ImGui::PopFont();
        ImGui::TextColored(k.muted, "This version becomes the pack's compatibility target.");
        ImGui::Spacing();
        ImGui::Spacing();

        const bool is_import = st.wizard_source == 4;
        bool has_resolved = false;
        std::vector<std::string> resolved_versions;
        {
            std::lock_guard<std::mutex> lock(st.wizard_resolve_mu);
            has_resolved = !st.wizard_resolved_versions.empty();
            resolved_versions = st.wizard_resolved_versions;
        }

        std::vector<std::string> suggested_versions;
        suggested_versions.reserve(resolved_versions.size());
        for (const std::string& version : resolved_versions) {
            if (version_catalog::supports("auto", version))
                suggested_versions.push_back(version);
        }
        const std::vector<std::string> supported_versions =
            available_profile_versions(st, "auto");
        if (!is_import && !version_catalog::supports("auto", st.wizard_version))
            st.wizard_version = version_catalog::default_version(st.wizard_loader);
        if (!is_import && st.wizard_version.empty() && !supported_versions.empty())
            st.wizard_version = supported_versions.front();

        if (!is_import && !suggested_versions.empty()) {
            ImGui::PushFont(f_bold);
            ImGui::TextColored(k.brand_hov, "Suggested versions from project");
            ImGui::PopFont();
            ImGui::Spacing();
            const float vbtn_w = ui_px(72.0f);
            const float vbtn_h = ui_px(32.0f);
            int cols = 0;
            for (size_t i = 0; i < suggested_versions.size() && i < 18; ++i) {
                bool active = (st.wizard_version == suggested_versions[i]);
                if (cols > 0) ImGui::SameLine(0, ui_px(6.0f));
                ImGui::PushStyleColor(ImGuiCol_Button, active ? k.brand_dk : k.surface2);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, k.brand);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, k.brand_hov);
                if (ImGui::Button(suggested_versions[i].c_str(), ImVec2(vbtn_w, vbtn_h)))
                    st.wizard_version = suggested_versions[i];
                ImGui::PopStyleColor(3);
                ++cols;
                if (cols >= 8) cols = 0;
            }
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
        }

        if (is_import) {
            card_begin("##wizard_archive_target", ImVec2(-1, 0));
            ImGui::PushFont(f_bold);
            ImGui::TextUnformatted("Archive-defined target");
            ImGui::PopFont();
            ImGui::TextColored(k.muted,
                               "Amalgam will read the Minecraft version and loader from the selected archive before installation.");
            card_end();
        } else {
            ImGui::PushFont(f_bold);
            ImGui::TextUnformatted("Minecraft version");
            ImGui::PopFont();
            ImGui::TextColored(k.muted,
                               "Choose a maintained target. These are the versions covered by the shipped launcher bridges.");
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##wizardversion", st.wizard_version.empty() ? "Choose a version" : st.wizard_version.c_str())) {
                for (const std::string& version : supported_versions) {
                    const bool selected = st.wizard_version == version;
                    if (ImGui::Selectable(version.c_str(), selected))
                        st.wizard_version = version;
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            if (!version_catalog::supports("auto", st.wizard_version)) {
                ImGui::TextColored(col_error, "Choose one of the maintained Minecraft targets.");
            }
            if (has_resolved && suggested_versions.empty()) {
                ImGui::TextColored(k.yellow,
                                   "This project did not report a version covered by the shipped bridges. Pick a compatible target before continuing.");
            }
            if (!st.selected.empty()) {
                if (version_catalog::supports("auto", st.selected)) {
                    std::string selected_label = "Use selected: " + st.selected;
                    if (ghost_button(selected_label.c_str(), ImVec2(ui_px(260.0f), ui_px(30.0f))))
                        st.wizard_version = st.selected;
                } else {
                    ImGui::TextColored(k.muted, "The currently selected profile uses an imported or legacy target.");
                }
            }
        }
    } else if (st.wizard_step == 2) {
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("Choose loader and performance");
        ImGui::PopFont();
        ImGui::TextColored(k.muted, "The loader is installed into this pack and stays isolated from others.");
        ImGui::Spacing();
        ImGui::Spacing();

        const bool is_import = st.wizard_source == 4;
        bool has_resolved = false;
        std::vector<std::string> resolved_loaders;
        {
            std::lock_guard<std::mutex> lock(st.wizard_resolve_mu);
            has_resolved = !st.wizard_resolved_loaders.empty();
            resolved_loaders = st.wizard_resolved_loaders;
        }

        if (!is_import && has_resolved) {
            ImGui::PushFont(f_bold);
            ImGui::TextColored(k.brand_hov, "Detected loaders from project");
            ImGui::PopFont();
            ImGui::Spacing();
            const float lbtn_w = ui_px(100.0f);
            const float lbtn_h = ui_px(34.0f);
            int shown = 0;
            for (const std::string& resolved_loader : resolved_loaders) {
                const std::string candidate = version_catalog::normalized_loader(resolved_loader);
                if (!version_catalog::supports(candidate, st.wizard_version)) continue;
                bool active = (st.wizard_loader == candidate);
                if (shown > 0) ImGui::SameLine(0, ui_px(8.0f));
                ImGui::PushStyleColor(ImGuiCol_Button, active ? k.brand_dk : k.surface2);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, k.brand);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, k.brand_hov);
                if (ImGui::Button(candidate.c_str(), ImVec2(lbtn_w, lbtn_h)))
                    st.wizard_loader = candidate;
                ImGui::PopStyleColor(3);
                ++shown;
            }
            if (shown == 0)
                ImGui::TextColored(k.muted, "The project did not report a maintained loader for the selected target.");
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
        }

        ImGui::PushFont(f_bold);
        ImGui::TextUnformatted("Mod loader");
        ImGui::PopFont();
        const char* loader_ids[] = {"vanilla", "fabric", "quilt", "forge", "neoforge"};
        const char* loader_labels[] = {"Vanilla", "Fabric", "Quilt", "Forge", "NeoForge"};
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##wizardloader",
                              st.wizard_loader.empty() ? "Choose a loader" : st.wizard_loader.c_str())) {
            for (int i = 0; i < 5; ++i) {
                const bool supported = is_import || version_catalog::supports(loader_ids[i], st.wizard_version);
                const bool selected = st.wizard_loader == loader_ids[i];
                if (!supported) ImGui::BeginDisabled();
                if (ImGui::Selectable(loader_labels[i], selected))
                    st.wizard_loader = loader_ids[i];
                if (selected) ImGui::SetItemDefaultFocus();
                if (!supported) {
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip("No maintained %s bridge for Minecraft %s.",
                                          loader_labels[i], st.wizard_version.c_str());
                    ImGui::EndDisabled();
                }
            }
            ImGui::EndCombo();
        }
        if (!is_import && !version_catalog::supports(st.wizard_loader, st.wizard_version)) {
            ImGui::TextColored(col_error,
                               "Choose a loader supported for Minecraft %s before continuing.",
                               st.wizard_version.c_str());
        }
        ImGui::Spacing();
        ImGui::Spacing();

        ImGui::PushFont(f_bold);
        ImGui::TextUnformatted("Performance profile");
        ImGui::PopFont();
        const char* performance_ids[] = {"auto", "low_end", "balanced", "shaders",
                                         "heavy_modpack", "custom"};
        const char* performance_items = "Auto\0Low-end device\0Balanced\0Shaders\0Heavy modpack\0Custom\0";
        int performance_index = 0;
        const std::string current_performance = performance::normalize_profile(st.wizard_performance_profile);
        for (int i = 0; i < 6; ++i) if (current_performance == performance_ids[i]) performance_index = i;
        ImGui::SetNextItemWidth(ui_px(260.0f));
        if (ImGui::Combo("##wizardperformance", &performance_index, performance_items, 6))
            st.wizard_performance_profile = performance_ids[performance_index];
        ImGui::TextColored(k.muted, "%s", performance::profile_label(st.wizard_performance_profile));

        if (st.cfg->advanced_mode) {
            ImGui::Spacing();
            ImGui::PushFont(f_bold);
            ImGui::TextUnformatted("Memory (MB)");
            ImGui::PopFont();
            ImGui::SetNextItemWidth(ui_px(200.0f));
            ImGui::InputInt("##wizardmemory", &st.wizard_memory);
            st.wizard_memory = std::max(0, st.wizard_memory);
            ImGui::Spacing();
            ImGui::PushFont(f_bold);
            ImGui::TextUnformatted("Java override (optional)");
            ImGui::PopFont();
            ImGui::SetNextItemWidth(-1);
            input_text("##wizardjava", &st.wizard_java);
        }
    } else {
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("Review your profile");
        ImGui::PopFont();
        ImGui::Spacing();
        ImGui::Spacing();

        // Project icon
        {
            std::lock_guard<std::mutex> lock(st.wizard_resolve_mu);
            if (!st.wizard_resolved_icon.empty()) {
                auto img = request_image(st, st.wizard_resolved_icon);
                if (img && img->texture && img->width > 0 && img->height > 0) {
                    ImGui::Image((ImTextureID)(intptr_t)img->texture, ImVec2(ui_px(48.0f), ui_px(48.0f)));
                    ImGui::SameLine(ui_px(56.0f));
                }
            }
        }

        // Review as a card
        card_begin("##review_card", ImVec2(-1, 0));
        auto review_row = [](const char* label, const char* value) {
            ImGui::TextColored(k.muted, "%s", label);
            ImGui::SameLine(ui_px(140.0f));
            ImGui::TextUnformatted(value);
        };
        review_row("Name", st.wizard_name.c_str());
        review_row("Minecraft", st.wizard_version.empty() ? "(set during install)" : st.wizard_version.c_str());
        review_row("Loader", st.wizard_loader.c_str());
        const char* review_sources[] = {"Vanilla", "Modrinth", "CurseForge", "AI co-pilot", "Import archive"};
        review_row("Source", review_sources[std::clamp(st.wizard_source, 0, 4)]);
        if ((st.wizard_source == 1 || st.wizard_source == 2) && !st.wizard_project.empty()) {
            review_row("Project", st.wizard_project.c_str());
            {
                std::lock_guard<std::mutex> lock(st.wizard_resolve_mu);
                if (!st.wizard_resolved_title.empty()) {
                    review_row("Title", st.wizard_resolved_title.c_str());
                }
            }
        }
        review_row("Performance", performance::profile_label(st.wizard_performance_profile));
        if (st.wizard_source == 3 && !st.wizard_prompt.empty()) {
            review_row("AI prompt", st.wizard_prompt.c_str());
        }
        if (st.wizard_source == 4 && !st.wizard_archive.empty()) {
            review_row("Archive", st.wizard_archive.c_str());
        }
        if (st.cfg->advanced_mode) {
            review_row("Memory", st.wizard_memory > 0 ? (std::to_string(st.wizard_memory) + " MB").c_str()
                                                        : "Auto (recommended)");
            if (!st.wizard_java.empty()) {
                review_row("Java", st.wizard_java.c_str());
            }
        }
        card_end();

        ImGui::Spacing();
        ImGui::TextColored(k.green, "This will create one isolated launchable profile. Existing profiles will not be modified.");
    }

    ImGui::EndChild();

    ImGui::Separator();
    ImGui::Spacing();

    // Footer buttons
    if (st.wizard_step > 0) {
        if (ghost_button("Back", ImVec2(ui_px(90.0f), ui_px(34.0f)))) --st.wizard_step;
        ImGui::SameLine();
    }
    if (ghost_button("Cancel", ImVec2(ui_px(90.0f), ui_px(34.0f)))) ImGui::CloseCurrentPopup();
    ImGui::SameLine(ImGui::GetWindowWidth() - ui_px(st.wizard_step == 3 ? 170.0f : 130.0f));
    if (st.wizard_step < 3) {
        bool valid = false;
        if (st.wizard_step == 0) {
            const bool source_ready = st.wizard_source == 4 ? !st.wizard_archive.empty()
                : (st.wizard_source == 1 || st.wizard_source == 2) ? !st.wizard_project.empty()
                : st.wizard_source == 3 ? !st.wizard_prompt.empty() : true;
            valid = !st.wizard_name.empty() && source_ready;
        } else if (st.wizard_step == 1) {
            valid = st.wizard_source == 4 ||
                version_catalog::supports("auto", st.wizard_version);
        } else {
            valid = st.wizard_source == 4 ||
                version_catalog::supports(st.wizard_loader, st.wizard_version);
        }
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, valid ? 1.0f : 0.45f);
        if (primary_button("Next", ImVec2(ui_px(110.0f), ui_px(34.0f))) && valid) ++st.wizard_step;
        ImGui::PopStyleVar();
    } else {
        if (primary_button("Create Profile", ImVec2(ui_px(150.0f), ui_px(34.0f))) &&
            (st.wizard_source == 4 ||
             version_catalog::supports(st.wizard_loader, st.wizard_version))) {
            if ((st.wizard_source == 1 || st.wizard_source == 2) && !st.wizard_project.empty()) {
                st.mod_loader = st.wizard_loader;
                st.mod_version = st.wizard_version;
                st.selected = st.wizard_version;
                st.selected_source = st.wizard_source == 1 ? "modrinth" : "curseforge";
                mods::SearchResult project;
                project.slug = st.wizard_project;
                project.title = st.wizard_name.empty() ? st.wizard_project : st.wizard_name;
                project.source = st.selected_source;
                project.type = mods::ProjectType::Modpack;
                spawn_worker(st, std::thread(do_modpack_install, std::ref(st), project, instances::Instance{}));
                ImGui::CloseCurrentPopup();
            } else if (st.wizard_source == 4) {
                int job_id = start_job(st, "Import modpack", st.wizard_archive, {}, {}, true);
                std::wstring archive_path = net::to_wide(st.wizard_archive);
                std::wstring import_root = (st.cfg->base_dir.empty() ? st.exe_dir : st.cfg->base_dir) + L"\\instances";
                mods::ApiCfg api = provider_config::make(*st.cfg);
                std::string performance_profile = st.wizard_performance_profile;
                spawn_worker(st, std::thread([&st, job_id, archive_path, import_root, api, performance_profile]() {
                    if (!wait_for_exclusive_job(st, job_id)) {
                        finish_job(st, job_id, false,
                                   "Cancelled while waiting for protected profile access");
                        return;
                    }
                    instances::Instance imported;
                    std::string error;
                    bool ok = import_pack::archive(archive_path, import_root, api, imported,
                        [&st, job_id](float progress, const std::string& detail) {
                            if (!job_transfer_allowed(st, job_id)) return false;
                            update_job(st, job_id, progress, detail);
                            return job_transfer_allowed(st, job_id);
                        }, &error);
                    if (ok) {
                        imported.performance_profile = performance_profile;
                        instances::save(imported, nullptr);
                        st.selected = imported.minecraft_version;
                        st.cfg->loader = imported.loader;
                        st.active_instance_dir = imported.directory;
                        st.instances_loaded = false;
                        finish_job(st, job_id, true, imported.name);
                        log_line(st, L"[modpack] imported " + imported.directory);
                    } else {
                        finish_job(st, job_id, false, error);
                        log_line(st, L"[modpack] import failed: " + net::to_wide(error));
                    }
                }));
                ImGui::CloseCurrentPopup();
            } else {
            instances::Instance source;
            source.name = st.wizard_name;
            source.minecraft_version = st.wizard_version;
            source.loader = st.wizard_loader;
            source.memory_mb = st.wizard_memory;
            source.performance_profile = st.wizard_performance_profile;
            source.java_path = st.wizard_java;
            std::wstring root = (st.cfg->base_dir.empty() ? st.exe_dir : st.cfg->base_dir) + L"\\instances";
            instances::Instance created;
            std::string error;
            if (instances::create(root, source, created, &error)) {
                st.selected = created.minecraft_version;
                st.cfg->loader = created.loader;
                st.active_instance_dir = created.directory;
                st.instances_loaded = false;
                log_line(st, L"[modpack] created " + created.directory);
                show_toast("Profile Created", ("'" + created.name + "' is ready to play").c_str(), k.green, 3.0f);
                std::vector<mods::Match> ai_plan;
                {
                    std::lock_guard<std::mutex> lock(st.pack_mu);
                    ai_plan = st.pack_plan;
                }
                if (st.wizard_source == 3 && !ai_plan.empty()) {
                    mods::ApiCfg api = provider_config::make(*st.cfg);
                    std::wstring mods_dir = created.directory + L"\\mods";
                    std::string game_version = created.minecraft_version;
                    std::string pack_loader = created.loader;
                    spawn_worker(st, std::thread([&st, api, ai_plan, mods_dir, game_version, pack_loader]() {
                        st.mod_installing = true;
                        int job_id = start_job(st, "Build AI modpack",
                                               std::to_string(ai_plan.size()) + " projects", {}, {}, true);
                        if (!wait_for_exclusive_job(st, job_id)) {
                            st.mod_installing = false;
                            set_mod_status(st, "AI modpack build cancelled before it began");
                            finish_job(st, job_id, false,
                                       "Cancelled while waiting for protected profile access");
                            return;
                        }
                        set_mod_install_log(st, {});
                        std::vector<std::string> install_log;
                        size_t completed = 0;
                        for (const auto& match : ai_plan) {
                            std::string error;
                            const std::string provider = mods::canonical_source(match.source);
                            const std::string loader = match.loader.empty() ? pack_loader : match.loader;
                            if (!mods::install_mod(api, match.slug,
                                                   provider.empty() ? "modrinth" : provider, loader, game_version,
                                                   mods_dir, install_log, &error,
                                                   [&](uint64_t done, uint64_t total) {
                                                       update_job_transfer(st, job_id, done, total);
                                                       return job_transfer_allowed(st, job_id);
                                                   }))
                                install_log.push_back("FAILED " + match.slug + ": " + error);
                            update_job(st, job_id, static_cast<float>(++completed) /
                                                   static_cast<float>(std::max<size_t>(1, ai_plan.size())),
                                       match.slug);
                        }
                        set_mod_install_log(st, std::move(install_log));
                        st.mod_installing = false;
                        set_mod_status(st, "AI modpack installation complete");
                        finish_job(st, job_id, true, "Installed into " + net::to_utf8(mods_dir));
                        log_line(st, L"[modpack] AI plan installed");
                    }));
                }
                ImGui::CloseCurrentPopup();
            } else {
                log_line(st, L"[modpack] create failed: " + net::to_wide(error));
                push_notice(st, ui_model::NoticeLevel::Error, "Profile creation failed", error);
                ImGui::CloseCurrentPopup();
            }
            }
        }
    }
    ImGui::EndPopup();
}

void draw_downloads_tab(UiState& st) {
    draw_page_emblem(st, "downloads-emblem-ai.png");
    page_title("Downloads", "Install, setup, and dependency activity for your modpacks.");
    draw_breadcrumbs({"Home", "Downloads"});

    std::vector<UiState::DownloadJob> jobs;
    {
        std::lock_guard<std::mutex> lock(st.jobs_mu);
        jobs = st.jobs;
    }

    // ── Summary stats ──────────────────────────────────────────────
    int active = 0, queued = 0, completed = 0, failed = 0;
    uint64_t total_bytes = 0;
    for (const auto& job : jobs) {
        if (job.active) ++active;
        if (job.active && job.queued) ++queued;
        if (job.completed) ++completed;
        if (job.failed) ++failed;
        total_bytes += job.bytes_done;
    }

    {
        const float avail_w = ImGui::GetContentRegionAvail().x;
        const float gap = ui_px(10.0f);
        const float stat_w = (avail_w - gap * 3.0f) * 0.25f;
        auto stat = [&](const char* label, int count, const ImVec4& accent) {
            draw_stat_card(label, std::to_string(count).c_str(), -1.0f, accent, stat_w);
        };
        stat("WORKING", active - queued, k.blue);
        ImGui::SameLine(0, gap);
        stat("QUEUED", queued, k.yellow);
        ImGui::SameLine(0, gap);
        stat("COMPLETED", completed, k.green);
        ImGui::SameLine(0, gap);
        stat("FAILED", failed, k.red);
        ImGui::Spacing();
    }

    // ── Toolbar ────────────────────────────────────────────────────
    const bool compact = ImGui::GetContentRegionAvail().x < ui_px(1020.0f);
    const char* operation_filters[] = {"All", "Active", "Queued", "Completed", "Failed"};
    for (int i = 0; i < 5; ++i) {
        if (i && (!compact || i % 3 != 0)) ImGui::SameLine(0, ui_px(6));
        const bool active_filter = i == st.operation_filter;
        const bool filter_clicked = active_filter
            ? primary_button(operation_filters[i], ImVec2(ui_px(80.0f), ui_px(26.0f)))
            : ghost_button(operation_filters[i], ImVec2(ui_px(80.0f), ui_px(26.0f)));
        if (filter_clicked) st.operation_filter = i;
    }
    if (compact) ImGui::NewLine();
    else ImGui::SameLine(0, ui_px(12));

    int active_unpaused = 0, paused_count = 0, failed_retryable = 0;
    for (const auto& job : jobs) {
        if (job.active && !job.paused && !job.completed && !job.failed) ++active_unpaused;
        if (job.paused) ++paused_count;
        if (job.failed && !job.retry_action.empty()) ++failed_retryable;
    }

    if (ghost_button("Pause All", ImVec2(ui_px(90.0f), ui_px(28.0f)), active_unpaused == 0)) {
        if (active_unpaused > 0) {
            {
                std::lock_guard<std::mutex> lock(st.jobs_mu);
                for (auto& job : st.jobs) {
                    if (job.active && !job.paused && !job.completed && !job.failed) {
                        job.paused = true;
                        job.phase = "Paused safely between transfer chunks";
                        if (job.history.empty() || job.history.back() != job.phase) {
                            job.history.push_back(job.phase);
                            if (job.history.size() > 16) job.history.erase(job.history.begin());
                        }
                    }
                }
            }
            persist_jobs(st);
            push_notice(st, ui_model::NoticeLevel::Info, "All transfers paused",
                        "Downloading jobs will resume where they left off.");
        }
    }
    ImGui::SameLine();
    if (ghost_button("Resume All", ImVec2(ui_px(100.0f), ui_px(28.0f)), paused_count == 0)) {
        if (paused_count > 0) {
            {
                std::lock_guard<std::mutex> lock(st.jobs_mu);
                for (auto& job : st.jobs) {
                    if (job.paused) {
                        job.paused = false;
                        job.phase = "Resuming transfer";
                        if (job.history.empty() || job.history.back() != job.phase) {
                            job.history.push_back(job.phase);
                            if (job.history.size() > 16) job.history.erase(job.history.begin());
                        }
                    }
                }
            }
            persist_jobs(st);
            push_notice(st, ui_model::NoticeLevel::Info, "Transfers resumed",
                        "Paused downloads are continuing.");
        }
    }
    ImGui::SameLine();
    if (ghost_button("Retry Failed", ImVec2(ui_px(100.0f), ui_px(28.0f)), failed_retryable == 0)) {
        if (failed_retryable > 0) {
            std::vector<UiState::DownloadJob> retry_snapshot;
            {
                std::lock_guard<std::mutex> lock(st.jobs_mu);
                retry_snapshot = st.jobs;
            }
            for (const auto& job : retry_snapshot)
                if (job.failed && !job.retry_action.empty()) retry_job(st, job);
        }
    }
    ImGui::SameLine();
    if (ghost_button("Clear completed", ImVec2(ui_px(120.0f), ui_px(28.0f)), completed == 0)) {
        {
            std::lock_guard<std::mutex> lock(st.jobs_mu);
            st.jobs.erase(std::remove_if(st.jobs.begin(), st.jobs.end(),
                [](const UiState::DownloadJob& j) { return j.completed && !j.active; }),
                st.jobs.end());
        }
        persist_jobs(st);
        jobs.clear();
        { std::lock_guard<std::mutex> lock(st.jobs_mu); jobs = st.jobs; }
    }
    ImGui::SameLine();
    if (ghost_button("Clear failed", ImVec2(ui_px(100.0f), ui_px(28.0f)), failed == 0)) {
        {
            std::lock_guard<std::mutex> lock(st.jobs_mu);
            st.jobs.erase(std::remove_if(st.jobs.begin(), st.jobs.end(),
                [](const UiState::DownloadJob& j) { return j.failed && !j.active; }),
                st.jobs.end());
        }
        persist_jobs(st);
        jobs.clear();
        { std::lock_guard<std::mutex> lock(st.jobs_mu); jobs = st.jobs; }
    }

    ImGui::Spacing();

    // ── Job list ───────────────────────────────────────────────────
    card_begin("##downloadspage", ImVec2(-1, 0));
    if (jobs.empty()) {
        if (st.operation_filter > 0) {
            illustrated_empty_state(IconId::Search, "No matching operations",
                                    "Try a different filter to see more activity.");
        } else {
            auto browse_action = [](UiState& st2) { navigate_to(st2, 2, 16, 0); };
            illustrated_empty_state(IconId::Download, "No downloads yet",
                                    "Install mods, modpacks, and shaders from the Discover page to see activity here.",
                                    "Browse Content", browse_action, &st);
        }
    } else {
        bool rendered_job = false;
        for (const auto& job : jobs) {
            if (st.operation_filter == 1 && !job.active) continue;
            if (st.operation_filter == 2 && !(job.active && job.queued)) continue;
            if (st.operation_filter == 3 && !job.completed) continue;
            if (st.operation_filter == 4 && !job.failed) continue;
            rendered_job = true;
            ImGui::PushID(job.id);

            card_begin((std::string("##dl_") + std::to_string(job.id)).c_str(), ImVec2(-1, 0));

            const ImVec4 state_color = job.failed ? k.red : job.completed ? k.green :
                                       job.cancelled ? k.orange : job.paused ? k.yellow : k.blue;

            // ── Header: label + provider badge + state badge ────────
            ImGui::PushFont(f_h2);
            ImGui::TextUnformatted(job.label.c_str());
            ImGui::PopFont();

            ImGui::SameLine(0, ui_px(8.0f));

            // Provider badge
            if (!job.provider.empty()) {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImVec2 bp = ImGui::GetCursorScreenPos();
                ImVec4 prov_bg = k.brand;
                prov_bg.w = 0.12f;
                std::string prov_text = job.provider;
                if (prov_text == "modrinth") prov_text = "Modrinth";
                else if (prov_text == "curseforge") prov_text = "CurseForge";
                draw_badge(dl, bp, prov_text.c_str(), k.brand, prov_bg);
                ImGui::Dummy(ImVec2(
                    ImGui::CalcTextSize(prov_text.c_str()).x + ui_px(14.0f),
                    ui_px(20.0f)));
                ImGui::SameLine(0, ui_px(6.0f));
            }

            // State badge
            {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImVec2 bp = ImGui::GetCursorScreenPos();
                ImVec4 sbg = state_color;
                sbg.w = 0.15f;
                draw_badge(dl, bp, ui_model::operation_state_name(operation_snapshot(job).state),
                           state_color, sbg);
                ImGui::Dummy(ImVec2(
                    ImGui::CalcTextSize(ui_model::operation_state_name(
                        operation_snapshot(job).state)).x + ui_px(14.0f),
                    ui_px(20.0f)));
            }

            // ── Detail row ─────────────────────────────────────────
            if (!job.detail.empty()) {
                // Filter raw HTTP status from user-visible detail
                const std::string& det = job.detail;
                bool show_detail = true;
                if (det.find("http status") != std::string::npos || det.find("HTTP ") != std::string::npos)
                    show_detail = false;
                if (show_detail) {
                    ImGui::Spacing();
                    ImGui::TextColored(k.muted, "%s", det.c_str());
                }
            }

            // ── Info row: target + phase + timing ──────────────────
            bool has_info = false;
            if (!job.target_profile.empty()) {
                ImGui::TextColored(k.muted, "Target: %s", job.target_profile.c_str());
                has_info = true;
            }
            if (!job.phase.empty()) {
                // Filter out raw HTTP status from phase display for users
                const std::string& ph = job.phase;
                bool show_phase = true;
                if (ph.find("http status") != std::string::npos || ph.find("HTTP ") != std::string::npos)
                    show_phase = false;
                if (show_phase) {
                    ImGui::SameLine(0, ui_px(12.0f));
                    ImGui::TextColored(k.muted, "Phase: %s", ph.c_str());
                    has_info = true;
                }
            }
            const std::string elapsed = format_elapsed(
                job.started_at ? job.started_at : job.created_at, job.finished_at);
            if (!elapsed.empty()) {
                ImGui::SameLine(0, ui_px(12.0f));
                ImGui::TextColored(k.muted, "%s: %s",
                    job.active ? "Elapsed" : "Finished in", elapsed.c_str());
                has_info = true;
            }

            // ── Transfer stats ─────────────────────────────────────
            if (job.bytes_total > 0) {
                ImGui::Spacing();
                const std::string transfer = format_bytes(job.bytes_done) + " / " +
                                             format_bytes(job.bytes_total);
                ImGui::TextColored(k.text, "%s", transfer.c_str());
                if (!format_rate(job.bytes_per_second).empty()) {
                    ImGui::SameLine(0, ui_px(12.0f));
                    ImGui::TextColored(k.blue, "%s", format_rate(job.bytes_per_second).c_str());
                }
                if (job.eta_seconds >= 0.0) {
                    ImGui::SameLine(0, ui_px(12.0f));
                    ImGui::TextColored(k.muted, "%s left", format_eta(job.eta_seconds).c_str());
                }
            }

            // ── Progress and transfer details ──────────────────────
            ImGui::Spacing();
            draw_operation_progress(job, &state_color);

            // ── Queue position ─────────────────────────────────────
            if (job.active && job.queued && job.exclusive) {
                int position = 1;
                for (const auto& earlier : jobs) {
                    if (earlier.id == job.id) break;
                    if (earlier.active && earlier.exclusive) ++position;
                }
                ImGui::TextColored(k.muted,
                    "Protected queue position %d — profiles stay safe while earlier changes finish.",
                    position);
            }

            // ── Action buttons ─────────────────────────────────────
            bool has_actions = false;
            if ((job.failed || job.cancelled) && !job.active && !job.retry_action.empty()) {
                if (ghost_button("Retry", ImVec2(ui_px(72.0f), ui_px(28.0f)))) retry_job(st, job);
                has_actions = true;
            }
            if ((job.failed || job.cancelled) && !job.active) {
                if (has_actions) ImGui::SameLine(0, ui_px(4.0f));
                if (ghost_button("Remove", ImVec2(ui_px(82.0f), ui_px(28.0f))))
                    remove_download_job(st, job.id);
                has_actions = true;
            }
            if (job.active && job.bytes_total > 0 && !job.cancel_requested) {
                if (has_actions) ImGui::SameLine(0, ui_px(4.0f));
                if (ghost_button(job.paused ? "Resume" : "Pause",
                                 ImVec2(ui_px(82.0f), ui_px(28.0f))))
                    set_job_paused(st, job.id, !job.paused);
                has_actions = true;
            }
            if (job.active) {
                if (has_actions) ImGui::SameLine(0, ui_px(4.0f));
                if (ghost_button(job.cancel_requested ? "Cancelling..." : "Cancel",
                                 ImVec2(ui_px(90.0f), ui_px(28.0f)))) {
                    {
                        std::lock_guard<std::mutex> lock(st.jobs_mu);
                        for (auto& item : st.jobs) if (item.id == job.id) {
                            item.cancel_requested = true;
                            item.paused = false;
                        }
                    }
                    persist_jobs(st);
                }
                has_actions = true;
            }

            // ── Expandable timeline ────────────────────────────────
            if (!job.history.empty()) {
                if (has_actions) ImGui::SameLine(0, ui_px(4.0f));
                if (ghost_button(st.expanded_operation_id == job.id ? "Hide steps" : "Show steps",
                                 ImVec2(ui_px(100.0f), ui_px(28.0f))))
                    st.expanded_operation_id =
                        st.expanded_operation_id == job.id ? 0 : job.id;

                if (st.expanded_operation_id == job.id) {
                    ImGui::Spacing();
                    ImGui::Indent(ui_px(8.0f));
                    ImGui::TextColored(k.brand, "Timeline");
                    ImGui::Spacing();
                    for (size_t si = 0; si < job.history.size(); ++si) {
                        ImVec4 dot_col = (si == job.history.size() - 1) ? state_color : k.muted;
                        ImGui::TextColored(dot_col, "\xe2\x97\x8f");
                        ImGui::SameLine(ui_px(16.0f));
                        const std::string step = job.history[si];
                        if (step.find("http status") != std::string::npos ||
                            step.find("HTTP ") != std::string::npos)
                            ImGui::TextColored(k.muted, "Download blocked by provider");
                        else
                            ImGui::TextColored(k.muted, "%s", step.c_str());
                    }
                    ImGui::Unindent(ui_px(8.0f));
                }
            }

            // ── Error card ─────────────────────────────────────────
            if (job.failed && !job.error.empty()) {
                ImGui::Spacing();
                const ImVec2 err_pos = ImGui::GetCursorScreenPos();
                const float err_h = ui_px(80.0f);
                const float err_w = ImGui::GetContentRegionAvail().x;
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddRectFilled(err_pos, ImVec2(err_pos.x + err_w, err_pos.y + err_h),
                                  c32(ImVec4(k.red.x, k.red.y, k.red.z, 0.08f)), ui_px(8.0f));
                dl->AddRect(err_pos, ImVec2(err_pos.x + err_w, err_pos.y + err_h),
                            c32(ImVec4(k.red.x, k.red.y, k.red.z, 0.25f)), ui_px(8.0f));
                ImGui::SetCursorPos(ImVec2(ui_px(12.0f), ImGui::GetCursorPosY() + ui_px(8.0f)));
                ImGui::PushFont(f_bold);
                ImGui::TextColored(k.red, "INSTALLATION FAILED");
                ImGui::PopFont();
                ImGui::SetCursorPosX(ui_px(12.0f));
                ImGui::PushFont(f_small);
                // Human-readable error explanation
                const std::string err_lower = job.error;
                std::string explanation;
                if (err_lower.find("403") != std::string::npos || err_lower.find("Forbidden") != std::string::npos)
                    explanation = "Download blocked by provider. This may be temporary — try again in a few minutes.";
                else if (err_lower.find("429") != std::string::npos || err_lower.find("rate") != std::string::npos)
                    explanation = "Too many requests. The provider is rate-limiting — wait a moment and retry.";
                else if (err_lower.find("404") != std::string::npos || err_lower.find("Not Found") != std::string::npos)
                    explanation = "The requested content was not found on the provider. It may have been removed.";
                else if (err_lower.find("timeout") != std::string::npos || err_lower.find("timed out") != std::string::npos)
                    explanation = "The request timed out. Check your network connection and try again.";
                else if (err_lower.find("SSL") != std::string::npos || err_lower.find("certificate") != std::string::npos)
                    explanation = "A certificate error occurred. Your network may be intercepting the connection.";
                else
                    explanation = job.error;
                ImGui::TextColored(k.muted, "%s", explanation.c_str());
                ImGui::PopFont();
                ImGui::Spacing();
                ImGui::SetCursorPosX(ui_px(12.0f));
                if (ghost_button("Retry", ImVec2(ui_px(80.0f), ui_px(24.0f)))) {
                    retry_job(st, job);
                }
                ImGui::SameLine(0, ui_px(6.0f));
                if (ghost_button("Remove", ImVec2(ui_px(82.0f), ui_px(24.0f))))
                    remove_download_job(st, job.id);
                ImGui::Dummy(ImVec2(0, err_h - ui_px(56.0f)));
            }

            card_end();
            ImGui::Spacing();
            ImGui::PopID();
        }
        if (!rendered_job) {
            ImGui::SetCursorPosX(
                ImGui::GetCursorPosX() +
                (ImGui::GetContentRegionAvail().x - ui_px(260.0f)) * 0.5f);
            ImGui::TextColored(k.muted, "No operations match this filter.");
        }
    }
    card_end();
}

void draw_notice_center(UiState& st) {
    if (!st.notice_center_open) return;
    set_next_adaptive_right_panel(380.0f, 330.0f, 300.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, k.surface);
    if (ImGui::Begin("Alerts", &st.notice_center_open,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        // Header with count
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("NOTIFICATIONS");
        ImGui::PopFont();
        const auto notices = notices_snapshot(st);
        if (!notices.empty()) {
            ImGui::SameLine(std::max(ImGui::GetCursorPosX(),
                                     ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ui_px(90.0f)));
            if (ghost_button("Clear all", ImVec2(ui_px(80.0f), ui_px(26.0f)))) {
                std::lock_guard<std::mutex> lock(st.notice_mu);
                st.notices.clear();
            }
        }
        ImGui::Spacing();
        if (notices.empty()) {
            ImGui::Spacing();
            ImGui::PushFont(f_bold);
            ImGui::TextUnformatted("ALL CAUGHT UP");
            ImGui::PopFont();
            ImGui::TextColored(k.muted, "Successful operations and alerts will appear here.");
        } else {
            ImGui::TextColored(k.muted, "%d notification%s", static_cast<int>(notices.size()),
                               notices.size() == 1 ? "" : "s");
            ImGui::Separator();
            ImGui::Spacing();
            for (auto it = notices.rbegin(); it != notices.rend(); ++it) {
                const auto& notice = *it;
                ImVec4 accent = k.blue;
                if (notice.level == ui_model::NoticeLevel::Success) accent = k.green;
                else if (notice.level == ui_model::NoticeLevel::Warning) accent = k.yellow;
                else if (notice.level == ui_model::NoticeLevel::Error) accent = k.red;
                ImGui::PushID(static_cast<int>(notice.id));
                // Notification card with accent strip
                const ImVec2 notif_pos = ImGui::GetCursorScreenPos();
                ImDrawList* dl = ImGui::GetWindowDrawList();
                // Accent strip on left
                dl->AddRectFilled(notif_pos,
                                  ImVec2(notif_pos.x + ui_px(3.0f), notif_pos.y + ui_px(8.0f)),
                                  c32(accent), ui_px(1.5f));
                ImGui::Dummy(ImVec2(ui_px(6.0f), 0));
                ImGui::SameLine(0, 0);
                // Title
                ImGui::PushFont(f_bold);
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() +
                                       ImGui::GetContentRegionAvail().x - ui_px(8.0f));
                ImGui::TextColored(accent, "%s", notice.title.c_str());
                ImGui::PopTextWrapPos();
                ImGui::PopFont();
                if (!notice.body.empty()) {
                    ImGui::PushFont(f_small);
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() +
                                           ImGui::GetContentRegionAvail().x - ui_px(8.0f));
                    ImGui::TextColored(k.muted, "%s", notice.body.c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::PopFont();
                }
                if (!notice.action_label.empty()) {
                    ImGui::Spacing();
                    if (ghost_button(notice.action_label.c_str(), ImVec2(ui_px(130.0f), ui_px(26.0f)))) {
                        st.notice_center_open = false;
                        if (notice.action_id == "downloads") {
                            st.sidebar_item = 4;
                            st.active_tab = 17;
                            st.downloads_open = true;
                        } else if (notice.action_id == "settings") {
                            st.sidebar_item = 12;
                            st.active_tab = 4;
                        } else if (notice.action_id == "discover") {
                            st.sidebar_item = 2;
                            st.active_tab = 16;
                        } else if (notice.action_id == "library") {
                            st.sidebar_item = 3;
                            st.active_tab = 6;
                        } else if (notice.action_id == "open_mc_launcher") {
                            official_launcher::OpenOfficialLauncher();
                        } else if (notice.action_id == "open_signin") {
                            st.sidebar_item = 9;
                            st.active_tab = 9;
                            st.settings_section = 0;
                        }
                    }
                    ImGui::SameLine();
                }
                if (ghost_button("Dismiss", ImVec2(ui_px(76.0f), ui_px(26.0f)))) dismiss_notice(st, notice.id);
                ImGui::Separator();
                ImGui::Spacing();
                ImGui::PopID();
            }
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
}

// ---------------------------------------------------------------------------
// Closed-beta dialogs
// ---------------------------------------------------------------------------

// Packaged beta Known Issues. These are the real, current limitations of the
// beta build, not invented content; they are updated with each beta release
// and rendered from the same source of truth as the release notes.
struct KnownIssue {
    const char* category;
    const char* title;
    const char* detail;
};

static const KnownIssue kKnownIssues[] = {
    {"Essentials", "Relay connections are in beta",
     "Direct peer-to-peer is the primary connection path. TURN relay is still being validated; when relay is unavailable the UI says so, and direct play keeps working."},
    {"Quilt", "Experimental",
     "Quilt loader support is experimental. If a Quilt profile fails, switch to Fabric or Vanilla and report the issue."},
    {"Cloud", "Coming soon",
     "Amalgam Cloud hosting is managed on the website and is not available from this beta build. Purchase and deploy controls stay disabled."},
    {"Bedrock", "Requires Minecraft for Windows",
     "Bedrock features appear only when Minecraft for Windows is installed. Machines without it see a clear notice instead of broken controls."},
    {"Updates", "Signed release channel",
     "This beta checks a signed update manifest. If no update is offered you are on the newest beta build published so far."},
    {"Account", "Beta accounts",
     "Some account features are still being finalized; sign-in and profile sync can change between beta builds."},
};
static const int kKnownIssuesCount =
    static_cast<int>(sizeof(kKnownIssues) / sizeof(kKnownIssues[0]));

static void draw_known_issues_dialog(UiState& st) {
    if (st.known_issues_open) ImGui::OpenPopup("Known Issues");
    bool open = st.known_issues_open;
    if (!ImGui::BeginPopupModal("Known Issues", &open,
                                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
        return;
    ImGui::TextColored(k.muted, "Known limitations in %s (%s channel).", kVersion, kChannel);
    ImGui::Separator();
    for (int i = 0; i < kKnownIssuesCount; ++i) {
        const KnownIssue& ki = kKnownIssues[i];
        ImGui::PushFont(f_bold);
        ImGui::TextColored(k.brand, "%s \u2014 %s", ki.category, ki.title);
        ImGui::PopFont();
        ImGui::TextWrapped("%s", ki.detail);
        if (i + 1 < kKnownIssuesCount) ImGui::Spacing();
    }
    ImGui::Separator();
    ImGui::Spacing();
    if (primary_button("Close", ImVec2(ui_px(120.0f), ui_px(30.0f)))) {
        st.known_issues_open = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine(0, ui_px(8.0f));
    if (ghost_button("Report a Bug", ImVec2(ui_px(130.0f), ui_px(30.0f)))) {
        st.known_issues_open = false;
        ImGui::CloseCurrentPopup();
        st.feedback_open = true;
    }
    ImGui::EndPopup();
    st.known_issues_open = open;
}



static void draw_recovery_dialog(UiState& st) {
    if (st.recovery_dialog_open) ImGui::OpenPopup("Amalgam did not close normally");
    bool open = st.recovery_dialog_open;
    if (!ImGui::BeginPopupModal("Amalgam did not close normally", &open,
                                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
        return;
    ImGui::TextWrapped("The launcher did not shut down cleanly last time. This can happen after a crash, a forced shutdown, or an interrupted update.");
    ImGui::TextWrapped("Your profiles, worlds and settings are safe and have not been changed.");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (primary_button("Continue Normally", ImVec2(ui_px(170.0f), ui_px(32.0f)))) {
        open = false;
        st.recovery_dialog_open = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine(0, ui_px(8.0f));
    if (ghost_button("Open Logs", ImVec2(ui_px(120.0f), ui_px(32.0f)))) {
        const std::wstring dir = st.exe_dir + L"\\logs";
        ShellExecuteW(st.hwnd, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    ImGui::SameLine(0, ui_px(8.0f));
    if (ghost_button("Start in Safe Mode", ImVec2(ui_px(170.0f), ui_px(32.0f)))) {
        // Restart with --safe-mode so the launcher skips optional network
        // activity from the first frame. Nothing is erased or reset.
        wchar_t self[MAX_PATH]{};
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        ShellExecuteW(st.hwnd, L"open", self, L"--safe-mode",
                      st.exe_dir.c_str(), SW_SHOWNORMAL);
        PostMessageW(st.hwnd, WM_CLOSE, 0, 0);
        open = false;
        st.recovery_dialog_open = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
    st.recovery_dialog_open = open;
}

static void draw_local_feedback_dialog(UiState& st) {
    if (st.local_feedback_open) ImGui::OpenPopup("Send Feedback");
    bool open = st.local_feedback_open;
    if (!ImGui::BeginPopupModal("Send Feedback", &open,
                                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
        return;

    ImGui::TextUnformatted("Send Feedback");
    ImGui::TextColored(k.muted, "Your feedback is saved locally for the development team.");
    ImGui::Separator();
    const char* categories[] = {"Bug Report", "Feature Request", "General Feedback"};
    ImGui::TextUnformatted("Category");
    ImGui::SetNextItemWidth(ui_px(200.0f));
    ImGui::Combo("##local_feedback_cat", &st.local_feedback_category, categories, 3);
    ImGui::Spacing();
    ImGui::TextUnformatted("Message");
    ImGui::InputTextMultiline("##local_feedback_msg", &st.local_feedback_message,
                              ImVec2(ui_px(460.0f), ui_px(160.0f)));
    ImGui::TextColored(k.muted, "Do not include passwords, tokens, or API keys.");
    ImGui::Spacing();
    if (primary_button("Send Feedback", ImVec2(ui_px(150.0f), ui_px(32.0f)))) {
        if (st.local_feedback_message.empty()) {
            push_notice(st, ui_model::NoticeLevel::Warning, "Feedback incomplete",
                        "Please enter a message before submitting.");
        } else {
            const auto now = std::chrono::system_clock::now();
            const std::time_t now_t = std::chrono::system_clock::to_time_t(now);
            struct tm tm_buf;
            localtime_s(&tm_buf, &now_t);
            char timestamp[32];
            std::strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", &tm_buf);

            std::wstring feedback_dir = net::get_local_app_data_path() + L"\\Amalgam\\feedback";
            std::error_code ec;
            std::filesystem::create_directories(feedback_dir, ec);

            std::wstring feedback_path = feedback_dir + L"\\feedback_" +
                                         net::to_wide(std::string(timestamp)) + L".json";

            std::string username;
            {
                auto& account_manager = aml::account::AccountManager::instance();
                if (account_manager.is_authenticated()) {
                    auto profile = account_manager.get_profile();
                    username = profile.display_name.empty() ? profile.username : profile.display_name;
                }
            }
            if (username.empty()) {
                const std::lock_guard<std::mutex> lock(st.auth_mu);
                username = st.account.username;
            }

            static const char* category_names[] = {"Bug Report", "Feature Request", "General Feedback"};
            Json feedback = Json::obj();
            feedback.set("timestamp", Json::str(std::string(timestamp)));
            feedback.set("category", Json::str(category_names[std::clamp(st.local_feedback_category, 0, 2)]));
            feedback.set("message", Json::str(st.local_feedback_message));
            feedback.set("launcher_version", Json::str(kVersion));
            feedback.set("username", Json::str(username));

            std::string write_error;
            if (json_write_file(feedback_path, feedback, &write_error)) {
                push_notice(st, ui_model::NoticeLevel::Success, "Feedback saved",
                            "Thank you! Your feedback has been saved locally.");
                st.local_feedback_message.clear();
                st.local_feedback_category = 0;
                open = false;
                ImGui::CloseCurrentPopup();
            } else {
                push_notice(st, ui_model::NoticeLevel::Error, "Feedback save failed",
                            write_error.empty() ? "Could not write feedback file." : write_error);
            }
        }
    }
    ImGui::SameLine();
    if (ghost_button("Cancel", ImVec2(ui_px(100.0f), ui_px(32.0f)))) {
        open = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
    st.local_feedback_open = open;
}

static void draw_feedback_dialog(UiState& st) {
    if (st.feedback_open) ImGui::OpenPopup("Beta Feedback");
    bool open = st.feedback_open;
    if (!ImGui::BeginPopupModal("Beta Feedback", &open,
                                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
        return;

    ImGui::TextUnformatted("Beta Feedback");
    ImGui::TextColored(k.muted, "Tell us what worked, what failed, or what should improve.");
    ImGui::Separator();
    const char* categories[] = {"Bug", "UI", "Performance", "Feature", "Other"};
    ImGui::SetNextItemWidth(ui_px(180.0f));
    ImGui::Combo("Category", &st.feedback_category, categories, 5);
    ImGui::TextUnformatted("Rating");
    for (int rating = 1; rating <= 5; ++rating) {
        if (rating > 1) ImGui::SameLine(0, ui_px(4.0f));
        ImGui::PushID(rating);
        if (primary_button(rating <= st.feedback_rating ? "*" : "-", ImVec2(ui_px(28.0f), ui_px(28.0f))))
            st.feedback_rating = rating;
        ImGui::PopID();
    }
    ImGui::TextUnformatted("Message");
    ImGui::InputTextMultiline("##beta_feedback_message", &st.feedback_message,
                              ImVec2(ui_px(460.0f), ui_px(120.0f)));
    ImGui::TextColored(k.muted, "Do not include passwords, tokens, API keys, or raw log files.");
    ImGui::Spacing();
    if (primary_button("Submit Feedback", ImVec2(ui_px(150.0f), ui_px(32.0f)))) {
        auto* client = aml::supabase::SupabaseManager::instance().client();
        if (!client || !client->is_authenticated()) {
            push_notice(st, ui_model::NoticeLevel::Warning, "Sign-in required",
                        "Sign into your Amalgam account before submitting beta feedback.");
        } else if (st.feedback_rating < 1 || st.feedback_message.empty()) {
            push_notice(st, ui_model::NoticeLevel::Warning, "Feedback incomplete",
                        "Choose a rating and enter a message.");
        } else {
            static const char* category_ids[] = {"bug", "ui", "performance", "feature", "other"};
            Json diagnostics = Json::obj();
            diagnostics.set("active_tab", Json::num(st.active_tab));
            diagnostics.set("has_java", Json::boolean(!st.javas.empty()));
            diagnostics.set("profile_loaded", Json::boolean(!st.selected_instance.id.empty()));
            Json args = Json::obj();
            args.set("category", Json::str(category_ids[std::clamp(st.feedback_category, 0, 4)]));
            args.set("rating", Json::num(st.feedback_rating));
            args.set("message", Json::str(st.feedback_message));
            args.set("page", Json::str("launcher-tab-" + std::to_string(st.active_tab)));
            args.set("app_version", Json::str(kVersion));
            args.set("diagnostics", diagnostics);
            const auto result = client->rpc("submit_beta_feedback", args);
            if (result.success) {
                push_notice(st, ui_model::NoticeLevel::Success, "Feedback submitted",
                            "Thank you for helping improve the beta.");
                st.feedback_message.clear();
                st.feedback_rating = 0;
                open = false;
                ImGui::CloseCurrentPopup();
            } else {
                push_notice(st, ui_model::NoticeLevel::Error, "Feedback failed", result.error);
            }
        }
    }
    ImGui::SameLine();
    if (ghost_button("Cancel", ImVec2(ui_px(100.0f), ui_px(32.0f)))) {
        open = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
    st.feedback_open = open;
}

void draw_launch_review(UiState& st) {
    if (st.launch_review_open) ImGui::OpenPopup("Launch preflight");
    bool open = st.launch_review_open;
    if (!ImGui::BeginPopupModal("Launch preflight", &open,
                                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
        return;
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted("Launch preflight");
    ImGui::PopFont();
    ImGui::TextWrapped("Review the selected target before starting Minecraft.");
    if (!st.pending_id.empty())
        ImGui::TextColored(k.muted, "Version: %s", st.pending_id.c_str());
    if (!st.pending_instance_dir.empty())
        ImGui::TextColored(k.muted, "Profile: %s", net::to_utf8(st.pending_instance_dir).c_str());
    ImGui::Separator();
    for (const auto& check : st.launch_checks) {
        ImGui::PushStyleColor(ImGuiCol_Text, check.passed ? k.green :
                               (check.blocking ? k.red : k.yellow));
        ImGui::TextUnformatted(check.passed ? "OK" : (check.blocking ? "BLOCKED" : "CHECK"));
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::PushFont(f_bold);
        ImGui::TextUnformatted(check.label.c_str());
        ImGui::PopFont();
        ImGui::TextWrapped("%s", check.detail.c_str());
    }
    const bool blocked = has_failed_launch_check(st.launch_checks, true);
    const bool official_launcher_installed = official_launcher::IsOfficialLauncherInstalled();
    if (blocked)
        ImGui::TextColored(k.red, "Launch is blocked until the required checks pass.");
    else
        ImGui::TextColored(k.yellow, "Warnings can be resolved later, but the launch may continue.");
    ImGui::Spacing();

    // Show the official launcher path prominently when no Microsoft account
    if (official_launcher_installed && !st.account.username.empty()) {
        // User has Microsoft account — direct launch available
        if (ghost_button(blocked ? "Close" : "Cancel", ImVec2(ui_px(100), ui_px(34)))) {
            st.launch_review_open = false;
            st.launch_review_approved = false;
            st.pending_id.clear();
            st.pending_instance_dir.clear();
            ImGui::CloseCurrentPopup();
        }
    } else if (official_launcher_installed && st.account.username.empty()) {
        // No Microsoft account — recommend official launcher path
        ImGui::PushFont(f_bold);
        ImGui::TextColored(k.green, "READY TO PLAY VIA MINECRAFT LAUNCHER");
        ImGui::PopFont();
        ImGui::TextColored(k.muted, "Amalgam will prepare your profile and open the Minecraft Launcher.");
        ImGui::TextColored(k.muted, "Sign in with your Microsoft account there and press Play.");
        ImGui::Spacing();

        if (primary_button("Play via Minecraft Launcher", ImVec2(ui_px(220), ui_px(38)))) {
            st.launch_review_open = false;
            st.launch_review_approved = false;
            st.pending_id.clear();
            st.pending_instance_dir.clear();
            ImGui::CloseCurrentPopup();
            st.pending_launch = true;
        }
        ImGui::SameLine(0, ui_px(8.0f));
        if (ghost_button("Cancel", ImVec2(ui_px(90), ui_px(38)))) {
            st.launch_review_open = false;
            st.launch_review_approved = false;
            st.pending_id.clear();
            st.pending_instance_dir.clear();
            ImGui::CloseCurrentPopup();
        }
    } else {
        // Official launcher not installed — need to get it
        ImGui::PushFont(f_bold);
        ImGui::TextColored(k.yellow, "MINECRAFT LAUNCHER NOT FOUND");
        ImGui::PopFont();
        ImGui::TextColored(k.muted, "Install the official Minecraft Launcher to sign in and launch.");
        ImGui::Spacing();

        if (primary_button("Get Minecraft Launcher", ImVec2(ui_px(200), ui_px(38)))) {
            ShellExecuteW(st.hwnd, L"open",
                          L"https://apps.microsoft.com/detail/9PGW18NPBZV5?hl=en&gl=US&ocid=pdpshare",
                          nullptr, nullptr, SW_SHOWNORMAL);
        }
        ImGui::SameLine(0, ui_px(8.0f));
        if (ghost_button("Cancel", ImVec2(ui_px(90), ui_px(38)))) {
            st.launch_review_open = false;
            st.launch_review_approved = false;
            st.pending_id.clear();
            st.pending_instance_dir.clear();
            ImGui::CloseCurrentPopup();
        }
    }

    if (ghost_button("Open Settings", ImVec2(ui_px(120), ui_px(28)))) {
        st.launch_review_open = false;
        st.sidebar_item = 12;
        st.active_tab = 4;
        ImGui::CloseCurrentPopup();
    }

    // Direct launch only when Microsoft account is available
    if (!blocked && !st.account.username.empty() && official_launcher_installed) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::PushFont(f_small);
        ImGui::TextColored(k.muted, "Or launch directly:");
        ImGui::PopFont();
        ImGui::SameLine();
        if (ghost_button("Launch directly", ImVec2(ui_px(120), ui_px(28)))) {
            st.launch_review_open = false;
            st.launch_review_approved = true;
            st.pending_launch = true;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndPopup();
    st.launch_review_open = open;
    if (!open) {
        st.launch_review_approved = false;
        st.pending_id.clear();
        st.pending_instance_dir.clear();
    }
}

void draw_downloads_panel(UiState& st) {
    if (!st.downloads_open) return;
    set_next_adaptive_right_panel(360.0f, 320.0f, 260.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, k.surface);
    if (ImGui::Begin("Downloads", &st.downloads_open,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::PushFont(f_h2);
        ImGui::TextUnformatted("Downloads");
        ImGui::PopFont();
        ImGui::Separator();
        std::vector<UiState::DownloadJob> jobs;
        {
            std::lock_guard<std::mutex> lock(st.jobs_mu);
            jobs = st.jobs;
        }
        if (jobs.empty()) {
            ImGui::TextColored(k.muted, "No active downloads.");
        } else {
            for (const auto& job : jobs) {
                ImGui::PushID(job.id);
                const auto operation = operation_snapshot(job);
                ImGui::PushFont(f_bold);
                ImGui::TextUnformatted(job.label.c_str());
                ImGui::PopFont();
                ImGui::SameLine();
                ImGui::TextColored(job.failed ? k.red : job.completed ? k.green :
                                   job.cancelled ? k.orange : job.paused ? k.yellow : k.muted,
                                   "%s", ui_model::operation_state_name(operation.state));
                if (!job.phase.empty())
                    ImGui::TextColored(k.muted, "%s", job.phase.c_str());
                const std::string elapsed = format_elapsed(job.started_at ? job.started_at : job.created_at,
                                                            job.finished_at);
                if (!elapsed.empty())
                    ImGui::TextColored(k.muted, "%s: %s", job.active ? "Elapsed" : "Finished in",
                                       elapsed.c_str());
                const ImVec4 panel_state_color = job.failed ? k.red : job.completed ? k.green :
                    job.cancelled ? k.orange : job.paused ? k.yellow : k.blue;
                progress_bar(job.progress, ImVec2(-1, ui_px(14.0f)), nullptr, &panel_state_color);
                if ((job.failed || job.cancelled) && !job.active && !job.retry_action.empty()) {
                     if (ghost_button("Retry", ImVec2(ui_px(64.0f), ui_px(24.0f)))) retry_job(st, job);
                    ImGui::SameLine();
                }
                if (job.active && job.bytes_total > 0 && !job.cancel_requested) {
                     if (ghost_button(job.paused ? "Resume" : "Pause", ImVec2(ui_px(72.0f), ui_px(24.0f))))
                        set_job_paused(st, job.id, !job.paused);
                    ImGui::SameLine();
                }
                if (job.active) {
                     if (ghost_button(job.cancel_requested ? "Cancelling" : "Cancel", ImVec2(ui_px(80.0f), ui_px(24.0f)))) {
                        {
                            std::lock_guard<std::mutex> lock(st.jobs_mu);
                            for (auto& item : st.jobs) if (item.id == job.id) {
                                item.cancel_requested = true;
                                item.paused = false;
                            }
                        }
                        persist_jobs(st);
                    }
                }
                ImGui::Separator();
                ImGui::PopID();
            }
        }
        ImGui::Spacing();
        std::string mod_status = mod_status_snapshot(st);
        if (!mod_status.empty()) ImGui::TextColored(k.muted, "%s", mod_status.c_str());
    }
    ImGui::End();
    ImGui::PopStyleColor();
}

// ---------------------------------------------------------------------------
// Console / log
// ---------------------------------------------------------------------------
void draw_log(UiState& st) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, k.sidebar);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::BeginChild("##log", ImVec2(0, st.log_h), ImGuiChildFlags_Borders);
    if (!st.diagnostics_open) {
        int active_downloads = 0;
        {
            std::lock_guard<std::mutex> lock(st.jobs_mu);
            for (const auto& job : st.jobs) if (job.active) ++active_downloads;
        }
        const int profiles = st.instances_loaded ? static_cast<int>(st.instance_list.size()) : 0;
        const std::string download_summary = active_downloads > 0
            ? std::to_string(active_downloads) + " active downloads"
            : "Downloads up to date";
        const float width = ImGui::GetContentRegionAvail().x;
        const float pad = ui_px(14.0f);
        const float button_width = std::clamp(width * 0.18f, ui_px(148.0f), ui_px(196.0f));
        const bool roomy = width >= ui_px(1000.0f);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 status_dot = ImGui::GetWindowPos() + ImVec2(pad, st.log_h * 0.5f);
        dl->AddCircleFilled(status_dot, ui_px(4.0f), c32(active_downloads > 0 ? k.blue : k.green));

        ImGui::SetCursorPos(ImVec2(pad + ui_px(12.0f), ui_px(18.0f)));
        ImGui::PushFont(f_small);
        ImGui::TextColored(k.text, "%s", active_downloads > 0 ? "Downloads are in progress"
                                                              : "Ready for your next world");
        ImGui::PopFont();
        if (roomy) {
            ImGui::SameLine(ui_px(295.0f));
            ImGui::PushFont(f_small);
            ImGui::TextColored(k.muted, "%d profiles", profiles);
            ImGui::SameLine(0, ui_px(20.0f));
            ImGui::TextColored(k.muted, "%s", download_summary.c_str());
            ImGui::SameLine(0, ui_px(20.0f));
            ImGui::TextColored(k.muted, "%d servers", static_cast<int>(st.cfg->servers.size()));
            ImGui::PopFont();
        }
        ImGui::SetCursorPos(ImVec2(std::max(pad, width - button_width - pad), ui_px(13.0f)));
        #if defined(AMALGAM_DEVELOPER_UI)
        if (ghost_button("Developer diagnostics", ImVec2(button_width, ui_px(34.0f))))
            st.diagnostics_open = true;
        #endif
        ImGui::SetCursorPos(ImVec2(0, ui_px(52.0f)));
        ImGui::Dummy(ImVec2(1, 0));
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
        return;
    }

    // The expanded diagnostics panel is a developer surface only; published
    // builds keep the compact player-facing status footer above.
    #if defined(AMALGAM_DEVELOPER_UI)
    ImGui::PushFont(f_small);
    if (ImGui::Button("<  Close diagnostics", ImVec2(0, ui_px(22)))) {
        st.diagnostics_open = false;
        ImGui::PopFont();
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
        return;
    }
    if (st.diagnostics_open) {
        ImGui::SameLine();
        if (ImGui::Button("Clear", ImVec2(ui_px(60), ui_px(22)))) {
            std::lock_guard<std::mutex> lock(st.log_mu);
            st.logs.clear();
            st.log_revision.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        ImGui::SameLine();
        ImGui::Dummy(ImVec2(ui_px(60), ui_px(22)));
    }
    ImGui::PopFont();
    ImGui::Separator();
    {
        std::vector<std::string> snapshot_lines;
        {
            std::lock_guard<std::mutex> lock(st.log_mu);
            snapshot_lines.assign(st.logs.begin(), st.logs.end());
        }
        ImGui::PushFont(f_mono);
        for (const auto& l : snapshot_lines) ImGui::TextUnformatted(l.c_str());
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
        ImGui::PopFont();
    }
    #endif

    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

// ---------------------------------------------------------------------------
// Main shell
// ---------------------------------------------------------------------------
static void handle_keyboard_navigation(UiState& st) {
    if (!st.cfg || !st.cfg->keyboard_navigation) return;
    const ImGuiIO& io = ImGui::GetIO();
    // Never steal number/comma shortcuts while the player is entering a
    // search, chat message, path, or other text value.
    if (io.WantTextInput) return;

    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_1)) {
        navigate_to(st, 0, 0);
    } else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_2)) {
        navigate_to(st, 2, 16, 0);
    } else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_3)) {
        navigate_to(st, 3, 6);
    } else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_4)) {
        navigate_to(st, 4, 17);
    } else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_5)) {
        navigate_to(st, 23, 23);
    } else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_6)) {
        navigate_to(st, 8, 8);
    } else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Comma)) {
        navigate_to(st, 12, 4);
    }
}

void draw_shell(UiState& st) {
    // One-time background update check. Never touches the network from the
    // render thread; the check runs on a worker and only records state.
    // Safe mode deliberately skips it (and the catalog prefetch below).
    if (!st.safe_mode && !st.update_check_started.exchange(true)) {
        spawn_worker(st, std::thread([&st]() { do_update_check(st); }));
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##shell", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
    ImGui::PopStyleVar(3);

    #if defined(AMALGAM_DEVELOPER_UI)
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_L))
        st.diagnostics_open = !st.diagnostics_open;
    #endif
    handle_keyboard_navigation(st);
    st.log_h = 0.0f;
    ImGui::BeginChild("##body", ImVec2(0, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    draw_sidebar(st);
    ImGui::SameLine();
    // Use the remaining ImGui layout width instead of reconstructing it from
    // the window rectangle. This keeps the content inside the viewport after
    // sidebar spacing, borders, and DPI scaling are applied.
    const float main_width = std::max(ui_px(320.0f), ImGui::GetContentRegionAvail().x);
    ImGui::BeginChild("##main", ImVec2(main_width, -1), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    draw_topbar(st);
    const float content_height = ImGui::GetContentRegionAvail().y;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui_px(20.0f), ui_px(14.0f)));
    ImGui::BeginChild("##content", ImVec2(0, content_height), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, k.bg);
    float content_available = ImGui::GetContentRegionAvail().x;
    float content_width = std::max(0.0f, content_available);
    ImGui::SetCursorPosX(0.0f);
    // Keep one page-level scroll surface for the main route. Individual cards
    // stay content-sized, so users do not get trapped in nested scroll areas.
    ImGui::BeginChild("##contentmax", ImVec2(content_width, 0),
                      ImGuiChildFlags_None, ImGuiWindowFlags_None);
    ImGui::PopStyleColor();

    switch (st.active_tab) {
        case 0:
            draw_home_tab(st);
            break;
        case 1:
            draw_mods_tab(st);
            break;
        case 2:
            draw_modpack_tab(st);
            break;
        case 3:
            draw_bedrock_tab(st);
            break;
        case 4:
            draw_settings_tab(st);
            break;
        case 6:
            draw_my_games_tab(st);
            break;
        case 7:
            draw_instances_tab(st);
            break;
        case 8:
            draw_server_manager(st);
            break;
        case 9:
            draw_screenshots_tab(st);
            break;
        case 10:
            draw_profiles_tab(st);
            break;
        case 15:
            draw_account_page(st);
            break;
        case 11:
            draw_java_tab(st);
            break;
        case 12:
            draw_backups_tab(st);
            break;
        case 13:
            draw_logs_tab(st);
            break;
        case 14:
            draw_config_tab(st);
            break;
        case 16:
            draw_discover_tab(st);
            break;
        case 17:
            draw_downloads_tab(st);
            break;
        case 18:
            draw_admin_page(st);
            break;
        case 19:
            // The standalone Social page was retired; its live friends,
            // messages, and party surfaces now share Essentials lifecycle and
            // data. Preserve old deep links without invoking dead page state.
            aml::essentials::draw_essentials_tab(st);
            break;
        case 20:
            draw_mod_manager_page(st);
            break;
        case 21:
            draw_performance_page(st);
            break;
        case 22:
            draw_theme_page(st);
            break;
        case 23:
            aml::essentials::draw_essentials_tab(st);
            break;
        default:
            draw_play_tab(st);
            break;
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::EndChild();
    ImGui::EndChild();

    if (st.login_popup_open) {
        st.wizard_open = false;
        ImGui::OpenPopup("Sign In to Amalgam");
        st.login_popup_open = false;
    }
    if (st.register_popup_open) {
        st.wizard_open = false;
        ImGui::OpenPopup("Amalgam Account Setup");
        st.register_popup_open = false;
    }
    if (st.password_reset_popup_open) {
        ImGui::OpenPopup("Reset Password");
        st.password_reset_popup_open = false;
    }
    draw_pack_wizard(st);
    draw_amalgam_login_wizard(st);
    draw_auth_wizard(st);
    draw_microsoft_login_dialog(st);
    
    // Password reset dialog
    draw_password_reset_dialog(st);
    
    // Account switcher dialog
    draw_account_switcher(st);
    
    draw_notice_center(st);
    draw_local_feedback_dialog(st);
    draw_feedback_dialog(st);
    draw_known_issues_dialog(st);
    draw_recovery_dialog(st);
    draw_launch_review(st);
    draw_downloads_panel(st);

    // V3 UI Components Integration
    // Check for Ctrl+K quick search shortcut
    if (ImGui::IsKeyPressed(ImGuiKey_K) && ImGui::GetIO().KeyCtrl) {
        open_quick_search();
    }

    // Populate quick search results from live data.  Search is intentionally
    // local and bounded: it never starts a provider request from the render
    // loop, and an empty query always clears the previous result set.
    if (g_quick_search_open()) {
        const uint64_t now = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        constexpr uint64_t kQuickSearchIndexTtlMs = 1500;
        if (st.quick_search_index_at_ms == 0 ||
            now - st.quick_search_index_at_ms > kQuickSearchIndexTtlMs) {
            ensure_instance_list(st);
            st.quick_search_index.clear();
            auto add_entry = [&st](std::string haystack, std::string label) {
                st.quick_search_index.push_back({std::move(haystack), std::move(label)});
            };
            for (const auto& inst : st.instance_list) {
                const std::string name = inst.name.empty() ? inst.id : inst.name;
                add_entry(name + " " + inst.minecraft_version + " " + inst.loader,
                          "[Profile] " + name + " (" + inst.minecraft_version + ")");
            }
            {
                std::lock_guard<std::mutex> lock(st.mod_mu);
                for (const auto& mod : st.home_packs)
                    add_entry(mod.title, "[Modpack] " + mod.title);
                for (const auto& mod : st.home_mods)
                    add_entry(mod.title, "[Mod] " + mod.title);
            }
            if (st.cfg) {
                for (const auto& server : st.cfg->servers) {
                    const std::string address = net::to_utf8(server.address);
                    add_entry(server.name + " " + address,
                              "[Server] " + server.name + " (" + address + ")");
                }
            }
            for (const auto& server : st.servers)
                add_entry(server.name + " " + server.minecraft_version,
                          "[Server] " + server.name + " (" + server.minecraft_version + ")");
            for (const auto& inst : st.instance_list) {
                const std::filesystem::path saves = std::filesystem::path(inst.directory) / L"saves";
                std::error_code world_error;
                if (!std::filesystem::exists(saves, world_error) || world_error) continue;
                for (std::filesystem::directory_iterator it(saves, world_error), end;
                     !world_error && it != end; it.increment(world_error)) {
                    std::error_code type_error;
                    if (!it->is_directory(type_error) || type_error) continue;
                    const std::wstring folder = it->path().filename().wstring();
                    if (folder.rfind(L".amalgam-") == 0) continue;
                    const std::string world_name = net::to_utf8(folder);
                    const std::string profile_name = inst.name.empty() ? inst.id : inst.name;
                    add_entry(world_name, "[World] " + world_name + " (" + profile_name + ")");
                }
            }
            add_entry("settings", "[Page] Settings");
            add_entry("downloads", "[Page] Downloads");
            add_entry("essentials friends", "[Page] Essentials");
            add_entry("bedrock", "[Page] Bedrock Edition");
            add_entry("servers", "[Page] Servers");
            st.quick_search_index_at_ms = now;
        }
        std::vector<std::string> results;
        const std::string query = g_quick_search_query();
        if (!query.empty()) {
            for (const auto& entry : st.quick_search_index) {
                if (ui_model::contains_case_insensitive(entry.haystack, query))
                    results.push_back(entry.label);
            }
        }
        g_quick_search_set_results(results);
    }

    // Render quick search dialog
    std::string search_result;
    if (quick_search_dialog(&search_result)) {
        // Navigate to selected item
        if (search_result.rfind("[Profile] ", 0) == 0) {
            std::string profile_name = search_result.substr(10);
            auto paren = profile_name.find(" (");
            if (paren != std::string::npos) profile_name = profile_name.substr(0, paren);
            for (auto& inst : st.instance_list) {
                std::string name = inst.name.empty() ? inst.id : inst.name;
                if (name == profile_name) {
                    open_instance_detail(st, inst);
                    break;
                }
            }
        } else if (search_result.rfind("[Modpack] ", 0) == 0 ||
                   search_result.rfind("[Mod] ", 0) == 0) {
            st.discover_sub_tab = 0;
            navigate_to(st, 2, 16, 0);
        } else if (search_result.rfind("[Server] ", 0) == 0) {
            navigate_to(st, 8, 8);
        } else if (search_result.rfind("[World] ", 0) == 0) {
            // Navigate to Library > My Worlds
            st.library_section = 1;
            navigate_to(st, 3, 6);
        } else if (search_result.rfind("[Page] ", 0) == 0) {
            const std::string page = search_result.substr(7);
            if (page == "Settings") navigate_to(st, 12, 4);
            else if (page == "Downloads") { st.sidebar_item = 4; st.active_tab = 17; st.downloads_open = true; }
            else if (page == "Essentials") navigate_to(st, 23, 23);
            else if (page == "Bedrock Edition") navigate_to(st, 3, 3);
            else if (page == "Servers") navigate_to(st, 8, 8);
        }
    }

    // Render notification toasts
    draw_toasts();

    // Draw a resize grip in the bottom-right corner so the user has a visual
    // affordance for window resizing.  WM_NCHITTEST already returns
    // HTBOTTOMRIGHT for the corner; this just draws the indicator.
    {
        const ImGuiViewport* grip_vp = ImGui::GetMainViewport();
        const float grip = ui_px(28.0f);
        const ImVec2 rp(grip_vp->WorkPos.x + grip_vp->WorkSize.x - grip,
                        grip_vp->WorkPos.y + grip_vp->WorkSize.y - grip);
        ImDrawList* fdl = ImGui::GetForegroundDrawList();
        const ImU32 col = c32(k.muted);
        const float s = grip;
        const int lines = 4;
        for (int i = 0; i < lines; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(lines - 1);
            const float inset = s * (0.15f + 0.55f * t);
            fdl->AddLine(ImVec2(rp.x + s - ui_px(1.0f), rp.y + inset),
                         ImVec2(rp.x + inset, rp.y + s - ui_px(1.0f)),
                         col, ui_px(1.5f));
        }
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Window plumbing
// ---------------------------------------------------------------------------
LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    // The launcher is a custom-drawn popup window, so Windows does not infer
    // resize hit areas from a standard caption frame.  Supply the eight resize
    // zones explicitly before ImGui handles client input.
    if (msg == WM_NCHITTEST) {
        RECT bounds{};
        GetWindowRect(hwnd, &bounds);
        const LONG x = GET_X_LPARAM(lparam);
        const LONG y = GET_Y_LPARAM(lparam);
        const LONG edge = std::max<LONG>(6, static_cast<LONG>(std::lround(ui_px(7.0f))));
        const bool left = x >= bounds.left && x < bounds.left + edge;
        const bool right = x < bounds.right && x >= bounds.right - edge;
        const bool top = y >= bounds.top && y < bounds.top + edge;
        const bool bottom = y < bounds.bottom && y >= bounds.bottom - edge;
        if (top && left) return HTTOPLEFT;
        if (top && right) return HTTOPRIGHT;
        if (bottom && left) return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        if (top) return HTTOP;
        if (bottom) return HTBOTTOM;

        // No HTCAPTION — window controls handle move via title bar buttons.
    }
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) return true;
    switch (msg) {
        case WM_SIZE:
            if (wparam != SIZE_MINIMIZED) {
                ImGui::GetIO().DisplaySize =
                    ImVec2(static_cast<float>(GET_X_LPARAM(lparam)),
                           static_cast<float>(GET_Y_LPARAM(lparam)));
            }
            return 0;
        case WM_DPICHANGED: {
            const UINT dpi = LOWORD(wparam);
            if (dpi != 0) g_pending_ui_scale = dpi_scale(dpi);
            const auto* suggested = reinterpret_cast<const RECT*>(lparam);
            if (suggested) {
                SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                             suggested->right - suggested->left,
                             suggested->bottom - suggested->top,
                             SWP_NOACTIVATE | SWP_NOZORDER);
            }
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
            limits->ptMinTrackSize.x = static_cast<LONG>(std::lround(ui_px(960.0f)));
            limits->ptMinTrackSize.y = static_cast<LONG>(std::lround(ui_px(600.0f)));
            // WS_POPUP windows overshoot the work area by the invisible
            // border when maximized.  Clamp to the nearest monitor work rect.
            const HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            if (mon && GetMonitorInfoW(mon, &mi)) {
                const RECT& work = mi.rcWork;
                limits->ptMaxPosition.x = work.left;
                limits->ptMaxPosition.y = work.top;
                limits->ptMaxSize.x = work.right - work.left;
                limits->ptMaxSize.y = work.bottom - work.top;
            }
            return 0;
        }
        case WM_DESTROY:
            if (app && app->cfg) {
                WINDOWPLACEMENT wp{};
                wp.length = sizeof(wp);
                if (GetWindowPlacement(hwnd, &wp)) {
                    const bool maximized = (wp.showCmd == SW_SHOWMAXIMIZED);
                    RECT normal = wp.rcNormalPosition;
                    MONITORINFO mi{};
                    mi.cbSize = sizeof(mi);
                    const HMONITOR mon = MonitorFromRect(&normal, MONITOR_DEFAULTTONEAREST);
                    if (mon && GetMonitorInfoW(mon, &mi)) {
                        app->cfg->window_x = normal.left - mi.rcWork.left;
                        app->cfg->window_y = normal.top - mi.rcWork.top;
                    } else {
                        app->cfg->window_x = normal.left;
                        app->cfg->window_y = normal.top;
                    }
                    app->cfg->width = normal.right - normal.left;
                    app->cfg->height = normal.bottom - normal.top;
                    app->cfg->window_maximized = maximized;
                    config::save(app->exe_dir + L"\\launcher.json", *app->cfg);
                }
            }
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

bool init_window(UiState& st, const RunOptions& options) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    st.window_icon = create_window_icon(st.exe_dir + L"\\branding\\amalgam-logo.png");
    wc.hIcon = st.window_icon;
    wc.hIconSm = st.window_icon;
    wc.lpszClassName = L"AmalgamLauncherWnd";
    if (!RegisterClassExW(&wc)) {
        if (st.window_icon) DestroyIcon(st.window_icon);
        st.window_icon = nullptr;
        return false;
    }

    // SPI_GETWORKAREA can still be DPI-virtualized on a process launched from
    // an older shell. Query the primary monitor directly so window sizing uses
    // the same physical-pixel space as the PerMonitorV2 manifest and OpenGL.
    RECT work{};
    MONITORINFO monitor_info{};
    monitor_info.cbSize = sizeof(monitor_info);
    const HMONITOR primary = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    if (primary && GetMonitorInfoW(primary, &monitor_info))
        work = monitor_info.rcWork;
    else
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int work_width = work.right - work.left;
    const int work_height = work.bottom - work.top;
    const int work_left = work.left;
    const int work_top = work.top;
    const int work_right = work.right;
    const int work_bottom = work.bottom;
    const int logical_width = options.initial_width > 0 ? options.initial_width :
        (st.cfg && st.cfg->width >= 1100 ? st.cfg->width : 1280);
    const int logical_height = options.initial_height > 0 ? options.initial_height :
        (st.cfg && st.cfg->height >= 680 ? st.cfg->height : 800);
    const int max_width = std::max(1, work_width - static_cast<int>(std::lround(ui_px(24.0f))));
    const int max_height = std::max(1, work_height - static_cast<int>(std::lround(ui_px(48.0f))));
    const int min_width = std::min(max_width, static_cast<int>(std::lround(ui_px(960.0f))));
    const int min_height = std::min(max_height, static_cast<int>(std::lround(ui_px(600.0f))));
    int desired_width = static_cast<int>(std::lround(logical_width * g_ui_scale));
    int desired_height = static_cast<int>(std::lround(logical_height * g_ui_scale));
    desired_width = std::clamp(desired_width, min_width, max_width);
    desired_height = std::clamp(desired_height, min_height, max_height);
    int pos_x = CW_USEDEFAULT;
    int pos_y = CW_USEDEFAULT;
    if (st.cfg && st.cfg->window_x >= 0 && st.cfg->window_y >= 0) {
        pos_x = static_cast<int>(std::lround(st.cfg->window_x * g_ui_scale));
        pos_y = static_cast<int>(std::lround(st.cfg->window_y * g_ui_scale));
        pos_x = std::clamp(pos_x, work_left, work_right - desired_width);
        pos_y = std::clamp(pos_y, work_top, work_bottom - desired_height);
    }
    RECT rc{0, 0, desired_width, desired_height};
    const DWORD window_style = WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX |
                               WS_MAXIMIZEBOX | WS_SYSMENU;
    st.hwnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L"Amalgam Launcher",
                              window_style,
                              pos_x, pos_y, rc.right - rc.left,
                              rc.bottom - rc.top, nullptr, nullptr, wc.hInstance, nullptr);
    if (st.hwnd && st.cfg && st.cfg->window_maximized) {
        ShowWindow(st.hwnd, SW_MAXIMIZE);
    }
    if (st.hwnd) {
        const BOOL dark = TRUE;
        const COLORREF caption = RGB(7, 13, 23);
        const COLORREF text = RGB(235, 238, 246);
        DwmSetWindowAttribute(st.hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        DwmSetWindowAttribute(st.hwnd, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
        DwmSetWindowAttribute(st.hwnd, DWMWA_TEXT_COLOR, &text, sizeof(text));
    }
    if (!st.hwnd) {
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        if (st.window_icon) DestroyIcon(st.window_icon);
        st.window_icon = nullptr;
    }
    return st.hwnd != nullptr;
}

bool run_window(config::Config* cfg, const RunOptions& options) {
    // Win32 would otherwise bitmap-scale the entire OpenGL surface on a
    // high-DPI display.  That produces blurry text and icons regardless of
    // the font atlas quality.  Opt in before any window or ImGui state exists.
    enable_per_monitor_dpi_awareness();
    g_ui_scale = initial_dpi_scale();
    g_pending_ui_scale = 0.0f;
    UiState st;
    st.cfg = cfg;
    st.fixture_mode = options.fixture_mode;
    st.safe_mode = options.safe_mode;
    st.capture_path = options.capture_path;
    st.capture_after_frames = st.capture_path.empty() ? 0 :
        std::max(1, options.capture_after_frames > 0 ? options.capture_after_frames : 120);
    // Open on the dashboard shown in the reference design.
    st.active_tab = 0;
    st.sidebar_item = 0;
    app = &st;
    // All ImGui geometry and font atlases are now scaled in the same physical
    // pixel space as the DPI-aware window, so Windows never stretches a
    // rendered frame after the fact.

    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring path = self;
    size_t slash = path.find_last_of(L"\\/");
    st.exe_dir = slash == std::wstring::npos ? L"." : path.substr(0, slash);
    {
        const std::filesystem::path log_folder = std::filesystem::path(st.exe_dir) / L"logs";
        std::error_code log_error;
        std::filesystem::create_directories(log_folder, log_error);
        if (!log_error) {
            const std::time_t now = std::time(nullptr);
            std::tm local_time{};
            localtime_s(&local_time, &now);
            wchar_t stamp[32]{};
            std::wcsftime(stamp, sizeof(stamp) / sizeof(stamp[0]), L"%Y%m%d-%H%M%S", &local_time);
            st.session_log_path = (log_folder /
                (std::wstring(L"amalgam-") + stamp + L"-" +
                 std::to_wstring(GetCurrentProcessId()) + L".log")).wstring();
            log_line(st, L"[launcher] session started");
        }
    }
    if (!st.fixture_mode) load_jobs(st);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    init_theme();
    apply_theme();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigWindowsResizeFromEdges = true;
    // Skip CJK merge on first build for instant startup.
    g_cjk_deferred_pending = true;
    build_font_atlas();
    g_cjk_deferred_pending = false;

    if (!init_window(st, options)) {
        ImGui::DestroyContext();
        app = nullptr;
        return false;
    }
    ShowWindow(st.hwnd, SW_SHOW);

    st.ui_base = net::to_utf8(st.cfg->base_dir);
    st.ui_assets = net::to_utf8(st.cfg->assets_dir);
    st.ui_java_cache = net::to_utf8(st.cfg->java_cache_dir);
    st.ui_username = net::to_utf8(st.cfg->username);
    st.ui_server = net::to_utf8(st.cfg->test_server);
    st.ui_jvm = st.cfg->extra_jvm;
    load_performance_settings(*st.cfg);
    load_social_settings(*st.cfg);
    load_mod_settings(*st.cfg);
    {
        std::string account_error;
        std::lock_guard<std::mutex> lock(st.auth_mu);
        st.auth_checked = true;
        if (auth::load(st.account, &account_error))
            st.auth_status = "Signed in as " + st.account.username;
        else
            st.auth_status = "No Microsoft account connected";
    }
    if (!cfg->supabase_url.empty() && !cfg->supabase_anon_key.empty()) {
        auto& supabase = aml::supabase::SupabaseManager::instance();
        supabase.initialize(
            cfg->supabase_url, cfg->supabase_anon_key, cfg->supabase_service_key);
        auto& accounts = aml::account::AccountManager::instance();
        auto session = accounts.get_current_session();
        if (!session.id.empty()) {
            if (session.is_expired()) {
                accounts.refresh_current_session();
            } else {
                supabase.auto_login(session.access_token, session.refresh_token);
                // Kick off an early entitlements fetch from Supabase (Whop-synced
                // subscription data) so the launcher knows the user's plan on first
                // render without waiting for the Account page.
                aml::entitlements::EntitlementManager::instance().request_refresh_from_supabase();
            }
        }
    }
    // Populate the online config (public client-safe endpoints) from the
    // launcher config so the account / cloud / plan pages know where the
    // Amalgam backend lives. Only public values are ever stored here.
    {
        auto& online = aml::online::config();
        if (!cfg->supabase_url.empty()) online.supabase_url = cfg->supabase_url;
        if (!cfg->supabase_anon_key.empty()) online.supabase_publishable_key = cfg->supabase_anon_key;
        if (!cfg->website_url.empty()) online.website_url = cfg->website_url;
        if (!cfg->api_url.empty()) online.api_url = cfg->api_url;
    }
    if (!st.fixture_mode && !options.initial_page.empty()) {
        const std::string& page = options.initial_page;
        if (page == "discover") navigate_to(st, 2, 16);
        else if (page == "library" || page == "profiles") navigate_to(st, 3, 6);
        else if (page == "downloads") navigate_to(st, 17, 17);
        else if (page == "account") navigate_to(st, 12, 15);
        else if (page == "servers") navigate_to(st, 8, 8);
        else if (page == "essentials" || page == "social") navigate_to(st, 23, 23);
        else if (page == "settings") navigate_to(st, 12, 4);
        else if (page == "java") navigate_to(st, 12, 11);
        else if (page == "performance") navigate_to(st, 12, 21);
        else if (page == "theme") navigate_to(st, 12, 22);
        else if (page == "bedrock") navigate_to(st, 17, 3);
        else if (page == "mods") navigate_to(st, 2, 20);
    }
    if (st.fixture_mode) {
        seed_visual_fixture(st);
        st.active_tab = options.fixture_tab;
        st.sidebar_item = options.fixture_sidebar;
        st.settings_section = options.fixture_settings_section;
        st.instance_detail_open = options.fixture_profile_detail;
        st.instance_detail_tab = 0;
        if (options.fixture_cloud) set_fixture_server_mode(1);
        if (options.fixture_server_detail_tab >= 0)
            set_fixture_server_detail(0, options.fixture_server_detail_tab);
        if (options.fixture_project_detail && !st.home_packs.empty()) {
            st.project_detail = st.home_packs.front();
            st.project_detail_open = true;
            st.project_detail_tab = 0;
            st.project_loading = false;
            st.project_info = {};
            st.project_info.slug = st.project_detail.slug;
            st.project_info.title = st.project_detail.title;
            st.project_info.description = st.project_detail.description;
            st.project_info.body = "A visual-review project page with creator information, compatible content, release notes, and versions."
                                   " The normal launcher replaces this fixture with live Modrinth or CurseForge data.";
            st.project_info.author = "Amalgam Visual Review";
            st.project_info.license = "All Rights Reserved";
            st.project_info.date_updated = "Today";
            st.project_info.type = st.project_detail.type;
            st.project_info.categories = {"adventure", "optimization", "multiplayer"};
            st.project_info.loaders = {"fabric", "forge"};
            mods::FileInfo release;
            release.id = "fixture-release";
            release.filename = "astral-frontier-1.21.1.mrpack";
            release.version_name = "1.21.1 Release";
            release.version_number = "1.21.1";
            release.game_versions = {"1.21.1"};
            release.loaders = {"fabric"};
            release.primary = true;
            release.size = 42ll * 1024ll * 1024ll;
            release.date_published = "Today";
            release.changelog = "Visual fixture release notes for the project detail page.";
            st.project_info.files = {std::move(release)};
            const uint64_t request_id = st.next_request_id.fetch_add(1);
            st.project_screen.begin(request_id);
            st.project_screen.accept(request_id, st.project_info, true);
        }
    }

    HDC hdc = GetDC(st.hwnd);
    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 24;
    pfd.cDepthBits = 24;
    const int pf = hdc ? ChoosePixelFormat(hdc, &pfd) : 0;
    const bool pixel_format_ready = pf != 0 && SetPixelFormat(hdc, pf, &pfd) != FALSE;
    HGLRC glrc = pixel_format_ready ? wglCreateContext(hdc) : nullptr;
    const bool context_ready = glrc && wglMakeCurrent(hdc, glrc) != FALSE;
    if (!context_ready) {
        log_line(st, L"[ui] OpenGL initialization failed; the launcher window cannot render safely.");
        if (glrc) wglDeleteContext(glrc);
        if (hdc) ReleaseDC(st.hwnd, hdc);
        DestroyWindow(st.hwnd);
        UnregisterClassW(L"AmalgamLauncherWnd", GetModuleHandleW(nullptr));
        if (st.window_icon) DestroyIcon(st.window_icon);
        ImGui::DestroyContext();
        app = nullptr;
        return false;
    }
    RECT client{};
    GetClientRect(st.hwnd, &client);
    glViewport(0, 0, std::max(1L, client.right - client.left),
               std::max(1L, client.bottom - client.top));

    ImGui_ImplWin32_Init(st.hwnd);
    ImGui_ImplOpenGL3_Init("#version 130");

    // Defer network fetches: fire after a short delay so the first few frames
    // render instantly without HTTP overhead competing for bandwidth/CPU.
    static int startup_frame_count = 0;
    startup_frame_count = 0;

    // Crash recovery: preserve the marker from the previous run so the
    // recovery dialog can offer Continue, Logs, or Safe Mode. The marker is
    // written now and removed only on a clean shutdown below.
    if (!st.fixture_mode) {
        const std::wstring recovery_marker = st.exe_dir + L"\\.amalgam_running";
        st.recovery_dialog_open = net::file_exists(recovery_marker);
        FILE* marker_file = nullptr;
        if (_wfopen_s(&marker_file, recovery_marker.c_str(), L"w") == 0 && marker_file) {
            fclose(marker_file);
        }
    }

    aml::ui::show_splash_screen(kVersion);

    MSG msg{};
    bool done = false;
    while (!done) {
        // Keep the desktop shell responsive without letting an uncapped render
        // loop consume a full CPU core while the user is idle.  The previous
        // one-millisecond sleep still allowed several hundred frames per
        // second on many systems, competing with Minecraft and background
        // downloads for CPU/GPU time.
        const auto frame_started = std::chrono::steady_clock::now();
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) done = true;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (done) break;

        if (g_pending_ui_scale > 0.0f) {
            const float requested_scale = g_pending_ui_scale;
            g_pending_ui_scale = 0.0f;
            rebuild_dpi_resources(requested_scale);
        }

        if (st.pending_launch) {
            st.pending_launch = false;
            if (!st.launch_review_approved) {
                st.launch_checks = evaluate_launch(st, st.pending_id, st.pending_instance_dir);
                if (has_failed_launch_check(st.launch_checks, false)) {
                    st.launch_review_open = true;
                } else {
                    start_pending_launch(st);
                }
            } else {
                start_pending_launch(st);
            }
        }
        if (!st.fixture_mode && !st.java_scanned && !st.fetching) {
            st.java_scanned = true;
            spawn_worker(st, std::thread([&st]() {
                auto list = java::scan_installed();
                const std::wstring root = st.cfg && !st.cfg->java_cache_dir.empty()
                    ? st.cfg->java_cache_dir
                    : st.exe_dir + L"\\runtimes\\java";
                java::JavaRuntimeManager manager(root);
                for (const auto& managed : manager.GetInstalledRuntimes()) {
                    const bool present = std::any_of(list.begin(), list.end(),
                        [&managed](const java::Install& install) {
                            return install.home == managed.home;
                        });
                    if (!present) {
                        java::Install install;
                        install.major = managed.major;
                        install.home = managed.home;
                        install.exe = managed.executable;
                        list.push_back(std::move(install));
                    }
                }
                std::sort(list.begin(), list.end(),
                          [](const java::Install& a, const java::Install& b) {
                              return a.major < b.major;
                          });
                std::lock_guard<std::mutex> lock(st.java_mu);
                st.javas = std::move(list);
                st.home_readiness.dirty = true;
            }));
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        if (!st.startup_metrics_logged) {
            RECT render_client{};
            GetClientRect(st.hwnd, &render_client);
            log_line(st, L"[ui] render surface " +
                std::to_wstring(render_client.right - render_client.left) + L"x" +
                std::to_wstring(render_client.bottom - render_client.top) + L" at " +
                std::to_wstring(GetDpiForWindow(st.hwnd)) + L" DPI");
            st.startup_metrics_logged = true;
            // After first frame renders, merge CJK glyphs so the launcher
            // appears instantly but gains full CJK support on next frame.
            // Uses merge_cjk_deferred_fonts() instead of build_font_atlas()
            // to avoid the Pixels != 0 assertion (ClearFonts() destroys
            // texture data after the OpenGL renderer is active).
            merge_cjk_deferred_fonts();
        }

        // Fire deferred startup network fetches after a few frames so the
        // window renders instantly, then content loads in the background.
        if (!st.fixture_mode && !st.safe_mode && startup_frame_count < 3) {
            startup_frame_count++;
            if (startup_frame_count == 3) {
                spawn_worker(st, std::thread(fetch_versions, std::ref(st)));
                spawn_worker(st, std::thread(fetch_home_catalog, std::ref(st)));
                st.browse_initial_request_sent = true;
                launch_mod_search(st);
            }
        }

        draw_shell(st);

        // Render the loading screen overlay on top of the UI
        {
            static bool startup_splash_pending = true;
            static float startup_splash_elapsed = 0.0f;
            static int startup_splash_stage = -1;
            const bool loading_visible = aml::ui::LoadingScreen::instance().is_visible();
            if (startup_splash_pending && loading_visible) {
                startup_splash_elapsed += 1.0f / 60.0f;
                const int stage = startup_splash_elapsed < 0.45f ? -1
                                : startup_splash_elapsed < 0.95f ? 0
                                : startup_splash_elapsed < 1.45f ? 1
                                : startup_splash_elapsed < 1.95f ? 2 : 3;
                if (stage > startup_splash_stage) {
                    startup_splash_stage = stage;
                    const char* statuses[] = {
                        "Loading configuration...",
                        "Preparing Modrinth and CurseForge...",
                        "Checking Java and Minecraft runtimes...",
                        "Ready for your next world."
                    };
                    if (stage >= 0) {
                        aml::ui::advance_loading(statuses[stage]);
                    }
                }
                // Keep the opening brand moment visible, but never hold the
                // launcher hostage if a provider or runtime is unavailable.
                if (startup_splash_elapsed >= 2.35f) {
                    startup_splash_pending = false;
                    aml::ui::hide_splash_screen();
                }
            } else if (!loading_visible) {
                startup_splash_elapsed = 0.0f;
            }
            float dt = 1.0f / 60.0f;
            aml::ui::LoadingScreen::instance().render(dt);
        }

        ImGui::Render();
        const ImVec2 display_size = ImGui::GetIO().DisplaySize;
        glViewport(0, 0, std::max(1, static_cast<int>(display_size.x)),
                   std::max(1, static_cast<int>(display_size.y)));
        glClearColor(k.bg.x, k.bg.y, k.bg.z, k.bg.w);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        if (!st.capture_path.empty() && ++st.rendered_frames >= st.capture_after_frames) {
            RECT capture_client{};
            GetClientRect(st.hwnd, &capture_client);
            const bool captured = capture_gl_frame(st.capture_path,
                                                   std::max(1, static_cast<int>(display_size.x)),
                                                   std::max(1, static_cast<int>(display_size.y)));
            const std::wstring dimensions = L" (ImGui " +
                std::to_wstring(static_cast<int>(display_size.x)) + L"x" +
                std::to_wstring(static_cast<int>(display_size.y)) + L", client " +
                std::to_wstring(capture_client.right - capture_client.left) + L"x" +
                std::to_wstring(capture_client.bottom - capture_client.top) + L")";
            log_line(st, (captured ? L"[ui] visual-review frame captured" :
                                      L"[ui] visual-review frame capture failed") + dimensions);
            st.capture_failed = !captured;
            st.capture_path.clear();
            done = true;
        }
        SwapBuffers(hdc);

        constexpr auto kFrameBudget = std::chrono::microseconds(16667); // ~60 FPS
        const auto remaining = kFrameBudget -
            (std::chrono::steady_clock::now() - frame_started);
        if (remaining > std::chrono::microseconds::zero()) {
            const auto sleep_ms = std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
            Sleep(static_cast<DWORD>(std::max<int64_t>(1, sleep_ms)));
        }
    }

    st.shutting_down = true;
    st.ai_request_id.fetch_add(1, std::memory_order_acq_rel);
    {
        std::lock_guard<std::mutex> lock(st.ai_mu);
        if (st.ai_chat_cancel_token) st.ai_chat_cancel_token->store(true, std::memory_order_release);
    }
    {
        std::lock_guard<std::mutex> lock(st.jobs_mu);
        for (auto& job : st.jobs) {
            if (!job.active) continue;
            job.cancel_requested = true;
            job.paused = false;
        }
    }
    if (!st.fixture_mode) persist_jobs(st);
    join_workers(st);
    if (!st.fixture_mode) {
        st.cfg->base_dir = net::to_wide(st.ui_base);
        st.cfg->assets_dir = net::to_wide(st.ui_assets);
        st.cfg->java_cache_dir = net::to_wide(st.ui_java_cache);
        st.cfg->username = net::to_wide(st.ui_username);
        st.cfg->test_server = net::to_wide(st.ui_server);
        st.cfg->extra_jvm = st.ui_jvm;
        config::save(st.exe_dir + L"\\launcher.json", *st.cfg);
        // Clean shutdown: remove the crash-recovery marker so diagnostics do
        // not carry a stale session into the next launch.
        DeleteFileW((st.exe_dir + L"\\.amalgam_running").c_str());
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplWin32_Shutdown();
    {
        std::lock_guard<std::mutex> lock(st.image_mu);
        for (auto& entry : st.images) {
            if (entry.second->texture) glDeleteTextures(1, &entry.second->texture);
        }
        st.images.clear();
    }
    ImGui::DestroyContext();
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(glrc);
    ReleaseDC(st.hwnd, hdc);
    DestroyWindow(st.hwnd);
    UnregisterClassW(L"AmalgamLauncherWnd", GetModuleHandleW(nullptr));
    if (st.window_icon) {
        DestroyIcon(st.window_icon);
        st.window_icon = nullptr;
    }
    app = nullptr;
    return !st.capture_failed;
}

}  // namespace aml::ui
