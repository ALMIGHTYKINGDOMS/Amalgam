#include "render/imgui_layer.h"
#include "render/input_state.h"
#include "core/module.h"
#include "core/tracker.h"
#include "render/hud.h"
#include "client/client_ui.h"
#include "client/client_input.h"
#include "client/client_core.h"
#include "client/client_notifications.h"

#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"
#include "MinHook.h"

#include <windows.h>

#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

namespace aml::render {

namespace {

using SwapFn = BOOL(HDC);

SwapFn* g_swap_original = nullptr;

std::atomic<bool> g_installed{false};
std::atomic<bool> g_imgui_ready{false};
std::atomic<bool> g_menu_visible{false};
std::atomic<bool> g_shutdown_requested{false};
std::once_flag g_init_once;
std::mutex g_frame_mu;
std::thread g_installer_thread;

bool g_menu_key_held = false;
bool g_cursor_forced = false;

std::string g_config_path;
std::string g_profile_name = "default";
bool g_config_dirty = false;

void load_config();
void save_config();

std::string profile_file(const std::string& name) {
    std::string safe;
    for (char c : name) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_') safe += c;
    }
    if (safe.empty()) safe = "default";
    size_t slash = g_config_path.find_last_of("\\/");
    std::string dir = slash == std::string::npos ? std::string() : g_config_path.substr(0, slash + 1);
    return dir + "amalgam-" + safe + ".cfg";
}

std::string safe_profile_name(const std::string& name) {
    std::string safe;
    for (char c : name) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_') safe += c;
    }
    return safe.empty() ? "default" : safe;
}

void load_profile(const std::string& name) {
    g_profile_name = safe_profile_name(name);
    g_config_path = profile_file(g_profile_name);
    modules_reset_defaults();
    load_config();
    g_config_dirty = false;
}

ImGuiKey vk_to_imgui(int vk) {
    if (vk >= 'A' && vk <= 'Z') return static_cast<ImGuiKey>(ImGuiKey_A + (vk - 'A'));
    if (vk >= '0' && vk <= '9') return static_cast<ImGuiKey>(ImGuiKey_0 + (vk - '0'));
    switch (vk) {
        case VK_ESCAPE: return ImGuiKey_Escape;
        case VK_INSERT: return ImGuiKey_Insert;
        case VK_DELETE: return ImGuiKey_Delete;
        case VK_HOME: return ImGuiKey_Home;
        case VK_END: return ImGuiKey_End;
        case VK_PRIOR: return ImGuiKey_PageUp;
        case VK_NEXT: return ImGuiKey_PageDown;
        case VK_LEFT: return ImGuiKey_LeftArrow;
        case VK_RIGHT: return ImGuiKey_RightArrow;
        case VK_UP: return ImGuiKey_UpArrow;
        case VK_DOWN: return ImGuiKey_DownArrow;
        case VK_RETURN: return ImGuiKey_Enter;
        case VK_BACK: return ImGuiKey_Backspace;
        case VK_TAB: return ImGuiKey_Tab;
        case VK_SPACE: return ImGuiKey_Space;
        case VK_LSHIFT: return ImGuiKey_LeftShift;
        case VK_RSHIFT: return ImGuiKey_RightShift;
        case VK_LCONTROL: return ImGuiKey_LeftCtrl;
        case VK_RCONTROL: return ImGuiKey_RightCtrl;
        case VK_LMENU: return ImGuiKey_LeftAlt;
        case VK_RMENU: return ImGuiKey_RightAlt;
        default: return ImGuiKey_None;
    }
}

ImGuiKey vk_to_imgui(int vk);
int vk_for_imgui_key(ImGuiKey key);

void pump_keyboard() {
    ImGuiIO& io = ImGui::GetIO();
    for (int vk = 0; vk < 256; ++vk) {
        bool down = input::key_down(vk);
        ImGuiKey key = vk_to_imgui(vk);
        if (key == ImGuiKey_None) continue;
        io.AddKeyEvent(key, down);
    }
    ImGuiKey mods[] = {ImGuiKey_LeftCtrl, ImGuiKey_RightCtrl, ImGuiKey_LeftShift,
                       ImGuiKey_RightShift, ImGuiKey_LeftAlt, ImGuiKey_RightAlt};
    for (ImGuiKey k : mods) io.AddKeyEvent(k, input::key_down(vk_for_imgui_key(k)));
}

int vk_for_imgui_key(ImGuiKey key) {
    switch (key) {
        case ImGuiKey_LeftCtrl: return VK_LCONTROL;
        case ImGuiKey_RightCtrl: return VK_RCONTROL;
        case ImGuiKey_LeftShift: return VK_LSHIFT;
        case ImGuiKey_RightShift: return VK_RSHIFT;
        case ImGuiKey_LeftAlt: return VK_LMENU;
        case ImGuiKey_RightAlt: return VK_RMENU;
        default: return 0;
    }
}

void pump_mouse(HWND wnd) {
    ImGuiIO& io = ImGui::GetIO();
    POINT p{};
    GetCursorPos(&p);
    RECT rc{};
    GetClientRect(wnd, &rc);
    POINT origin{0, 0};
    ClientToScreen(wnd, &origin);
    io.AddMousePosEvent(static_cast<float>(p.x - origin.x), static_cast<float>(p.y - origin.y));
    int buttons[] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON};
    for (int b = 0; b < 3; ++b) {
        bool down = input::key_down(buttons[b]);
        if (down != io.MouseDown[b]) io.AddMouseButtonEvent(b, down);
    }
}

void set_cursor_visibility(bool show) {
    if (show == g_cursor_forced) return;
    if (show) {
        while (ShowCursor(TRUE) < 0) {}
    } else {
        while (ShowCursor(FALSE) >= 0) {}
    }
    g_cursor_forced = show;
}

void save_config() {
    if (g_config_path.empty()) return;
    const std::string temp_path = g_config_path + ".tmp";
    std::ofstream f(temp_path, std::ios::trunc);
    if (!f.is_open()) return;
    for (size_t i = 0; i < modules_count(); ++i) {
        Module m = module_snapshot(i);
        f << static_cast<int>(m.id) << '=' << (m.enabled ? 1 : 0);
        for (float p : m.params) f << '|' << p;
        f << '\n';
    }
    f.flush();
    if (!f.good()) {
        f.close();
        std::remove(temp_path.c_str());
        return;
    }
    f.close();
    if (!MoveFileExA(temp_path.c_str(), g_config_path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::remove(temp_path.c_str());
        return;
    }
    g_config_dirty = false;
}

void load_config() {
    std::ifstream f(g_config_path);
    if (!f.is_open()) return;
    std::string line;
    while (std::getline(f, line)) {
        int id = 0;
        int en = 0;
        if (std::sscanf(line.c_str(), "%d=%d", &id, &en) != 2) continue;
        ModuleId module_id = static_cast<ModuleId>(id);
        if (!module_get(module_id)) continue;
        module_set_enabled(module_id, en != 0);
        const char* p = std::strchr(line.c_str(), '=');
        if (!p) continue;
        for (int k = 0; k < 6; ++k) {
            p = std::strchr(p, '|');
            if (!p) break;
            char* end = nullptr;
            const float value = std::strtof(p + 1, &end);
            if (end != p + 1 && std::isfinite(value)) module_set_param(module_id, k, value);
            ++p;
        }
    }
}

void draw_menu() {
    if (!g_menu_visible.load()) return;
    ImGui::SetNextWindowSize(ImVec2(520.f, 680.f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Amalgam")) {
        ImGui::End();
        return;
    }
    char profile[96]{};
    strncpy_s(profile, g_profile_name.c_str(), _TRUNCATE);
    ImGui::InputText("Profile", profile, sizeof(profile));
    ImGui::SameLine();
    if (ImGui::Button("Load profile")) load_profile(profile);
    ImGui::SameLine();
    if (ImGui::Button("Save profile")) save_config();
    if (g_config_dirty) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.f, 0.75f, 0.2f, 1.f), "unsaved changes");
    }
    int panel_profile = hud::panel_profile();
    if (ImGui::Combo("Panel profile", &panel_profile,
                     "Auto\0Survival\0PvP\0Practice\0BedWars\0SkyWars\0UHC\0Crystal PvP\0SMP/Factions\0Modded\0", 10))
        hud::set_panel_profile(panel_profile);
    ImGui::TextDisabled("Insert toggles this DLL panel on every supported loader.");
    bool safe_mode = hud::server_safe();
    if (ImGui::Checkbox("Server-safe mode", &safe_mode)) {
        hud::set_server_safe(safe_mode);
        if (safe_mode) {
            for (size_t i = 0; i < modules_count(); ++i)
                module_set_enabled(module_snapshot(i).id, false);
        }
        g_config_dirty = true;
    }
    if (safe_mode)
        ImGui::TextColored(ImVec4(1.f, 0.75f, 0.2f, 1.f),
                           "Module actions are blocked while Server-safe mode is enabled.");
    if (ImGui::Button("Enable all modules") && !safe_mode) {
        for (size_t i = 0; i < modules_count(); ++i)
            module_set_enabled(module_snapshot(i).id, true);
        g_config_dirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Disable all modules")) {
        for (size_t i = 0; i < modules_count(); ++i)
            module_set_enabled(module_snapshot(i).id, false);
        g_config_dirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset modules")) {
        modules_reset_defaults();
        g_config_dirty = true;
    }
    ImGui::Separator();
    ImGui::BeginDisabled(safe_mode);
    ImGui::BeginChild("##module_controls", ImVec2(0.f, 320.f), true);
    for (size_t i = 0; i < modules_count(); ++i) {
        Module m = module_snapshot(i);
        bool enabled = m.enabled;
        ImGui::PushID(static_cast<int>(m.id));
        if (ImGui::Checkbox(m.name, &enabled)) {
            module_set_enabled(m.id, enabled);
            g_config_dirty = true;
        }
        if (m.id == MOD_FREECAM) ImGui::SameLine();
        if (m.id == MOD_FREECAM) ImGui::TextDisabled("camera-only");
        if (m.id == MOD_AUTOTOOL) ImGui::SameLine();
        if (m.id == MOD_AUTOTOOL) ImGui::TextDisabled("automatic slot selection");
        auto param = [&](int index, const char* label, float min_value, float max_value) {
            float value = module_param(m.id, index);
            if (ImGui::SliderFloat(label, &value, min_value, max_value)) {
                module_set_param(m.id, index, value);
                g_config_dirty = true;
            }
        };
        switch (m.id) {
            case MOD_FREECAM:
                param(0, "Speed", 0.05f, 2.f);
                param(1, "Sensitivity", 0.1f, 4.f);
                break;
            case MOD_FLY:
                param(0, "Speed (m/s)", 1.f, 20.f);
                break;
            case MOD_SPEED:
                param(0, "Multiplier", 1.f, 3.f);
                break;
            case MOD_NOFALL:
                param(0, "Height", 1.f, 8.f);
                break;
            case MOD_KILLAURA:
                param(0, "Range", 1.f, 6.f);
                param(1, "Cooldown (ticks)", 1.f, 20.f);
                break;
            default:
                break;
        }
        ImGui::Spacing();
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::EndDisabled();
    if (ImGui::Button("Save config")) save_config();
    ImGui::SameLine();
    if (ImGui::Button("Save all")) {
        save_config();
        hud::save();
    }
    hud::draw_editor();
    ImGui::SameLine();
    ImGui::Text("Tick %s | queued %u", tick_active().load() ? "ACTIVE" : "idle", pipeline().pending());
    ImGui::End();
}

BOOL hook_swap(HDC hdc) {
    if (g_shutdown_requested.load(std::memory_order_acquire))
        return g_swap_original ? g_swap_original(hdc) : TRUE;
    std::call_once(g_init_once, [&]() {
        if (g_shutdown_requested.load(std::memory_order_acquire)) return;
        if (!g_imgui_ready.load()) {
            ImGui::CreateContext();
            ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
            ImGui::StyleColorsDark();
            ImGui_ImplOpenGL3_Init("#version 130");
            g_imgui_ready.store(true);
        }
    });
    if (g_imgui_ready.load()) {
        HWND wnd = WindowFromDC(hdc);
        RECT rc{};
        if (wnd && GetClientRect(wnd, &rc)) {
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(static_cast<float>(rc.right - rc.left),
                                    static_cast<float>(rc.bottom - rc.top));
        }
        LARGE_INTEGER freq{};
        LARGE_INTEGER now{};
        static LARGE_INTEGER last{};
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&now);
        static bool have_last = false;
        if (have_last && freq.QuadPart > 0) {
            double dt = static_cast<double>(now.QuadPart - last.QuadPart) / freq.QuadPart;
            ImGui::GetIO().DeltaTime = static_cast<float>(dt < 0.0001 ? 0.0001 : dt);
        }
        last = now;
        have_last = true;

        input::poll();

        // New client system: edge-triggered menu toggle
        if (client::menu_key_pressed_this_frame()) {
            client::ClientUI::instance().toggle_menu();
            g_menu_visible.store(client::ClientUI::instance().is_menu_open());
        }

        set_cursor_visibility(g_menu_visible.load());
        {
            std::lock_guard<std::mutex> lock(g_frame_mu);
            if (g_menu_visible.load()) {
                pump_keyboard();
                pump_mouse(wnd ? wnd : WindowFromDC(hdc));
            }
            ImGui_ImplOpenGL3_NewFrame();
            ImGui::NewFrame();

            // Draw existing HUD elements always
            hud::draw(tracker::store().snapshot());

            // New client UI (menu + HUD overlay + toasts)
            float dt = ImGui::GetIO().DeltaTime;
            client::ClientUI::instance().render(dt);

            ImGui::Render();
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        }
    }
    return g_swap_original ? g_swap_original(hdc) : TRUE;
}

}  // namespace

bool installed() { return g_installed.load(); }

bool menu_visible() { return g_menu_visible.load(); }

void toggle_menu() { g_menu_visible = !g_menu_visible.load(); }

bool install() {
    if (g_installed.load()) return true;
    g_shutdown_requested.store(false, std::memory_order_release);
    char buf[MAX_PATH];
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       reinterpret_cast<LPCSTR>(&install), &self);
    GetModuleFileNameA(self, buf, MAX_PATH);
    char* slash = std::strrchr(buf, '\\');
    if (slash) *(slash + 1) = '\0';
    // The launcher starts Java with the selected profile as its working
    // directory. Keep module presets, HUD layouts and shared launcher data in
    // that profile instead of beside the DLL, where every installation used to
    // read the same stale state.
    std::error_code current_path_error;
    const std::filesystem::path game_dir = std::filesystem::current_path(current_path_error);
    const std::filesystem::path client_root = current_path_error
        ? std::filesystem::path(std::string(buf) + "client")
        : game_dir / ".amalgam" / "client";
    std::error_code create_error;
    std::filesystem::create_directories(client_root, create_error);

    g_config_path = (client_root / "amalgam-default.cfg").string();
    if (GetFileAttributesA(g_config_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        // One-time migration from builds that stored a global preset beside
        // amalgam.dll. Never overwrite a profile-specific preset.
        const std::string legacy_default = std::string(buf) + "amalgam-default.cfg";
        const std::string legacy = std::string(buf) + "amalgam.cfg";
        const std::string source = GetFileAttributesA(legacy_default.c_str()) != INVALID_FILE_ATTRIBUTES
            ? legacy_default : legacy;
        if (GetFileAttributesA(source.c_str()) != INVALID_FILE_ATTRIBUTES)
            CopyFileA(source.c_str(), g_config_path.c_str(), TRUE);
    }
    g_profile_name = "default";
    load_config();
    g_config_path = profile_file(g_profile_name);
    hud::init(g_config_path + ".hud");

    // Initialize the new client system
    const std::string client_config_dir = client_root.string();
    client::client_init(client_config_dir, current_path_error ? std::string() : game_dir.string());
    client::ClientUI::instance().init();

    g_installer_thread = std::thread([]() {
        HMODULE mod = nullptr;
        for (int tries = 0; tries < 600 && !mod &&
             !g_shutdown_requested.load(std::memory_order_acquire); ++tries) {
            mod = GetModuleHandleW(L"opengl32.dll");
            if (!mod) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!mod) {
            MH_Initialize();
            return;
        }
        auto swap = reinterpret_cast<SwapFn*>(GetProcAddress(mod, "wglSwapBuffers"));
        if (!swap) {
            MH_Initialize();
            return;
        }
        if (MH_Initialize() != MH_OK) return;
        if (MH_CreateHook(swap, reinterpret_cast<void*>(&hook_swap),
                          reinterpret_cast<void**>(&g_swap_original)) != MH_OK) {
            return;
        }
        if (MH_EnableHook(swap) == MH_OK) g_installed.store(true);
    });
    return true;
}

void think() {}

void shutdown() {
    g_shutdown_requested.store(true, std::memory_order_release);
    if (g_installer_thread.joinable()) g_installer_thread.join();
    std::lock_guard<std::mutex> frame_lock(g_frame_mu);
    if (g_installed.load()) {
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        g_installed.store(false);
    }
    if (g_imgui_ready.load()) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui::DestroyContext();
        g_imgui_ready.store(false);
    }
    save_config();
    hud::save();
    client::ClientUI::instance().shutdown();
    client::client_shutdown();
}

}  // namespace aml::render
