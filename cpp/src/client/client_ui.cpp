#define IMGUI_DEFINE_MATH_OPERATORS
#include "client/client_ui.h"
#include "client/client_core.h"
#include "client/client_hud2.h"
#include "client/client_notifications.h"
#include "render/hud.h"
#include "core/module.h"
#include "core/player_stats.h"
#include "core/tracker.h"
#include "imgui.h"

#include <windows.h>
#include <wincodec.h>
#include <gl/GL.h>
#include <shellapi.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")

static const char* preset_combo_getter(void* data, int idx) {
    auto* v = static_cast<std::vector<std::string>*>(data);
    if (idx < 0 || idx >= static_cast<int>(v->size())) return nullptr;
    return (*v)[idx].c_str();
}

static bool copy_to_clipboard(const std::string& text) {
    if (!OpenClipboard(nullptr)) return false;
    EmptyClipboard();
    int wlen = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    if (wlen <= 0) { CloseClipboard(); return false; }
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(wlen + 1) * sizeof(wchar_t));
    if (!memory) {
        CloseClipboard();
        return false;
    }
    wchar_t* target = static_cast<wchar_t*>(GlobalLock(memory));
    if (!target) {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), target, wlen);
    target[wlen] = L'\0';
    GlobalUnlock(memory);
    if (!SetClipboardData(CF_UNICODETEXT, memory)) {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }
    CloseClipboard();
    return true;
}

namespace {

struct ClientArtTexture {
    GLuint id = 0;
    int width = 0;
    int height = 0;
};

static std::map<std::string, ClientArtTexture> g_client_art_textures;

static std::filesystem::path client_art_path(const char* filename) {
    HMODULE self = nullptr;
    char module_path[MAX_PATH]{};
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&client_art_path), &self) &&
        GetModuleFileNameA(self, module_path, MAX_PATH)) {
        std::filesystem::path candidate(module_path);
        std::error_code ec;
        const std::filesystem::path root = candidate.parent_path() / "branding";
        candidate = root / "ai" / filename;
        if (std::filesystem::exists(candidate, ec) && !ec) return candidate;
        ec.clear();
        candidate = root / filename;
        if (std::filesystem::exists(candidate, ec) && !ec) return candidate;
    }
    std::error_code ec;
    const std::filesystem::path root = std::filesystem::current_path(ec) / "branding";
    const std::filesystem::path ai_candidate = root / "ai" / filename;
    if (std::filesystem::exists(ai_candidate, ec) && !ec) return ai_candidate;
    return root / filename;
}

static bool load_client_art_texture(const char* filename, ClientArtTexture& out) {
    const std::string key(filename ? filename : "");
    if (key.empty()) return false;
    const auto cached = g_client_art_textures.find(key);
    if (cached != g_client_art_textures.end()) {
        out = cached->second;
        return out.id != 0;
    }

    const std::filesystem::path path = client_art_path(filename);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) return false;

    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    bool ok = false;
    const HRESULT com_init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize_com = SUCCEEDED(com_init);

    do {
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                    CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) break;
        const std::wstring wide_path = path.wstring();
        if (FAILED(factory->CreateDecoderFromFilename(
                wide_path.c_str(), nullptr, GENERIC_READ,
                WICDecodeMetadataCacheOnLoad, &decoder))) break;
        if (FAILED(decoder->GetFrame(0, &frame))) break;
        if (FAILED(factory->CreateFormatConverter(&converter))) break;
        if (FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA,
                                         WICBitmapDitherTypeNone, nullptr, 0.0,
                                         WICBitmapPaletteTypeCustom))) break;

        UINT width = 0;
        UINT height = 0;
        if (FAILED(converter->GetSize(&width, &height)) || width == 0 || height == 0 ||
            width > 8192 || height > 8192) break;
        std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
        if (FAILED(converter->CopyPixels(nullptr, width * 4,
                                         static_cast<UINT>(pixels.size()), pixels.data()))) break;

        GLuint texture = 0;
        glGenTextures(1, &texture);
        if (texture == 0) break;
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F /* GL_CLAMP_TO_EDGE */);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F /* GL_CLAMP_TO_EDGE */);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(width),
                     static_cast<GLsizei>(height), 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     pixels.data());
        glBindTexture(GL_TEXTURE_2D, 0);

        out = {texture, static_cast<int>(width), static_cast<int>(height)};
        g_client_art_textures.emplace(key, out);
        ok = true;
    } while (false);

    if (converter) converter->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (factory) factory->Release();
    if (uninitialize_com) CoUninitialize();
    return ok;
}

static bool draw_client_art(const char* filename, const ImVec2& pos,
                            const ImVec2& size, bool cover = true,
                            ImU32 tint = IM_COL32_WHITE) {
    ClientArtTexture texture;
    if (!load_client_art_texture(filename, texture)) return false;

    ImVec2 uv0(0.0f, 0.0f);
    ImVec2 uv1(1.0f, 1.0f);
    if (cover && texture.width > 0 && texture.height > 0 && size.x > 0 && size.y > 0) {
        const float source_aspect = static_cast<float>(texture.width) /
                                    static_cast<float>(texture.height);
        const float target_aspect = size.x / size.y;
        if (source_aspect > target_aspect) {
            const float visible = target_aspect / source_aspect;
            uv0.x = (1.0f - visible) * 0.5f;
            uv1.x = 1.0f - uv0.x;
        } else if (source_aspect < target_aspect) {
            const float visible = source_aspect / target_aspect;
            uv0.y = (1.0f - visible) * 0.5f;
            uv1.y = 1.0f - uv0.y;
        }
    }
    ImGui::GetWindowDrawList()->AddImage(
        static_cast<ImTextureID>(texture.id),
        pos, pos + size, uv0, uv1, tint);
    return true;
}

static bool draw_client_art_foreground(const char* filename, const ImVec2& pos,
                                       const ImVec2& size, ImU32 tint = IM_COL32_WHITE) {
    ClientArtTexture texture;
    if (!load_client_art_texture(filename, texture) || texture.width <= 0 ||
        texture.height <= 0 || size.x <= 0.0f || size.y <= 0.0f) return false;

    const float source_aspect = static_cast<float>(texture.width) /
                                static_cast<float>(texture.height);
    const float target_aspect = size.x / size.y;
    ImVec2 uv0(0.0f, 0.0f);
    ImVec2 uv1(1.0f, 1.0f);
    if (source_aspect > target_aspect) {
        const float visible = target_aspect / source_aspect;
        uv0.x = (1.0f - visible) * 0.5f;
        uv1.x = 1.0f - uv0.x;
    } else if (source_aspect < target_aspect) {
        const float visible = source_aspect / target_aspect;
        uv0.y = (1.0f - visible) * 0.5f;
        uv1.y = 1.0f - uv0.y;
    }
    ImGui::GetForegroundDrawList()->AddImage(
        static_cast<ImTextureID>(texture.id), pos, pos + size, uv0, uv1, tint);
    return true;
}

static bool draw_client_atlas_tile(const char* atlas, int cell, const ImVec2& pos,
                                   float size, ImU32 tint = IM_COL32_WHITE) {
    ClientArtTexture texture;
    if (!load_client_art_texture(atlas, texture)) return false;
    const int clamped = std::clamp(cell, 0, 15);
    const int column = clamped % 4;
    const int row = clamped / 4;
    const float cell_u = 0.25f;
    const float cell_v = 0.25f;
    const ImVec2 uv0(column * cell_u, row * cell_v);
    const ImVec2 uv1((column + 1) * cell_u, (row + 1) * cell_v);
    ImGui::GetWindowDrawList()->AddImage(
        static_cast<ImTextureID>(texture.id),
        pos, pos + ImVec2(size, size), uv0, uv1, tint);
    return true;
}

static bool draw_client_atlas_icon(int cell, const ImVec2& pos, float size,
                                   ImU32 tint = IM_COL32_WHITE) {
    return draw_client_atlas_tile("client-hud-icon-atlas-ai.png", cell, pos, size, tint);
}

}  // namespace

namespace aml::client {

static int draw_radial_menu(float cx, float cy, float radius);

static void lowercase_ascii(std::string& text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
}

// Reachable secondary pages that aren't primary sidebar tabs (reference
// panels: Essentials Overlay, Mod & Packs, and the Screenshots gallery).
enum class SubPage : int { None = 0, ModsAndPacks, Essentials, Screenshots };
static SubPage g_sub_page = SubPage::None;

static bool open_launcher_page(const char* page) {
    HMODULE self = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(&open_launcher_page), &self))
        return false;
    char module_path[MAX_PATH]{};
    if (!GetModuleFileNameA(self, module_path, MAX_PATH)) return false;
    const std::filesystem::path launcher =
        std::filesystem::path(module_path).parent_path() / "amalgam_launcher.exe";
    if (!std::filesystem::exists(launcher)) return false;
    const std::string arguments = std::string("--page ") + (page ? page : "home");
    const auto result = reinterpret_cast<INT_PTR>(ShellExecuteA(
        nullptr, "open", launcher.string().c_str(), arguments.c_str(),
        launcher.parent_path().string().c_str(), SW_SHOWNORMAL));
    return result > 32;
}

struct CrashDiagnosis {
    std::string cause = "Automatic diagnosis inconclusive";
    std::string issue = "Open the report to inspect the first Caused by entry";
    std::string solution = "Review the crash report and latest.log before changing or removing content.";
};

static CrashDiagnosis diagnose_crash_report(const std::string& path) {
    CrashDiagnosis diagnosis;
    if (std::filesystem::path(path).extension() == ".gz") {
        diagnosis.issue = "The newest crash report is compressed";
        diagnosis.solution = "Open the report from the crash-reports folder, or review logs/latest.log.";
        return diagnosis;
    }
    std::ifstream report(path, std::ios::binary);
    if (!report.is_open()) {
        diagnosis.issue = "The newest crash report could not be read";
        return diagnosis;
    }
    std::string contents(512 * 1024, '\0');
    report.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    contents.resize(static_cast<size_t>(report.gcount()));
    std::string lower = contents;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    if (lower.find("outofmemoryerror") != std::string::npos) {
        diagnosis.cause = "Java ran out of memory";
        diagnosis.issue = "java.lang.OutOfMemoryError was found in the report";
        diagnosis.solution = "Increase this profile's memory in the launcher, then close memory-heavy applications.";
    } else if (lower.find("mixinapplyerror") != std::string::npos ||
               lower.find("mixin application failed") != std::string::npos ||
               lower.find("invalidmixinexception") != std::string::npos) {
        diagnosis.cause = "Mixin compatibility failure";
        diagnosis.issue = "A mixin could not be applied by the active mod set";
        diagnosis.solution = "Update the named mod and loader, or disable the most recently changed mod before relaunching.";
    } else if (lower.find("modresolutionexception") != std::string::npos ||
               lower.find("incompatible mod set") != std::string::npos ||
               lower.find("missing mandatory dependencies") != std::string::npos ||
               lower.find("requires version") != std::string::npos) {
        diagnosis.cause = "Mod dependency or version conflict";
        diagnosis.issue = "A required or compatible mod version is missing";
        diagnosis.solution = "Open the report for the named dependency, then install its required version or change the conflicting mod.";
    } else if (lower.find("unsupportedclassversionerror") != std::string::npos) {
        diagnosis.cause = "Incorrect Java version";
        diagnosis.issue = "A class was compiled for a different Java runtime";
        diagnosis.solution = "Use the launcher Java Manager to assign the recommended Java version to this profile.";
    } else if (lower.find("glfw error") != std::string::npos ||
               lower.find("opengl") != std::string::npos && lower.find("failed") != std::string::npos) {
        diagnosis.cause = "Graphics or OpenGL initialization failure";
        diagnosis.issue = "Minecraft could not initialize its graphics context";
        diagnosis.solution = "Update the graphics driver, disable overlays, and retry with the default performance profile.";
    }
    return diagnosis;
}

static std::string server_endpoint(const ServerEntry& s) {
    return s.port > 0 ? s.address + ":" + std::to_string(s.port) : s.address;
}

ClientUI& ClientUI::instance() {
    static ClientUI ui;
    return ui;
}

// ---------------------------------------------------------------------------
// Theme-aware style setup
// ---------------------------------------------------------------------------

// Smooth animation state for menu open/close
static float g_menu_alpha = 0.0f;
static float g_menu_target_alpha = 0.0f;
static float g_tab_transition = 0.0f;
static int g_prev_tab = -1;

namespace {

std::string client_visible_label(const char* label) {
    if (!label) return {};
    const char* marker = std::strstr(label, "##");
    return marker ? std::string(label, marker) : std::string(label);
}

ImVec4 client_alpha(ImVec4 color, float alpha) {
    color.w = alpha;
    return color;
}

void draw_client_button_surface(const ImVec2& min, const ImVec2& max,
                                const ImVec4& top, const ImVec4& bottom,
                                const ImVec4& outline, bool focused) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = 8.0f * std::clamp(settings().ui_scale, 0.85f, 1.35f);
    dl->AddRectFilled(min, max, ImGui::GetColorU32(bottom), r);
    const ImVec2 inset(1.0f, 1.0f);
    dl->AddRectFilledMultiColor(min + inset, max - inset,
                                ImGui::GetColorU32(top), ImGui::GetColorU32(client_alpha(top, 0.88f)),
                                ImGui::GetColorU32(bottom), ImGui::GetColorU32(bottom));
    dl->AddLine(min + ImVec2(8.0f, 1.0f), ImVec2(max.x - 8.0f, min.y + 1.0f),
                ImGui::GetColorU32(client_alpha(theme().text, 0.18f)), 1.0f);
    dl->AddRect(min, max, ImGui::GetColorU32(outline), r, 0, 1.0f);
    if (focused) {
        dl->AddRect(min - ImVec2(2.0f, 2.0f), max + ImVec2(2.0f, 2.0f),
                    ImGui::GetColorU32(theme().accent_hover), r + 2.0f, 0, 1.25f);
    }
}

} // namespace

static void push_theme_style() {
    ImGuiStyle& s = ImGui::GetStyle();
    const float scale = std::clamp(settings().ui_scale, 0.85f, 1.35f);
    s.WindowRounding = 12.0f * scale;
    s.ChildRounding = 12.0f * scale;
    s.FrameRounding = 8.0f * scale;
    s.GrabRounding = 4.0f * scale;
    s.ScrollbarRounding = 6.0f * scale;
    s.TabRounding = 6.0f * scale;
    s.WindowPadding = ImVec2(16, 16) * scale;
    s.FramePadding = ImVec2(10, 6) * scale;
    s.ItemSpacing = ImVec2(10, 8) * scale;
    s.WindowBorderSize = 1.0f;
    s.FrameBorderSize = 1.0f;
    // Smooth animation for menu fade
    g_menu_target_alpha = 1.0f;
    g_menu_alpha += (g_menu_target_alpha - g_menu_alpha) * 0.15f;
    if (g_menu_alpha > 0.99f) g_menu_alpha = 1.0f;

    ImGui::PushStyleColor(ImGuiCol_WindowBg, theme().bg);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme().panel);
    ImGui::PushStyleColor(ImGuiCol_Border, theme().border);
    ImGui::PushStyleColor(ImGuiCol_Text, theme().text);
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, theme().muted);
    ImGui::PushStyleColor(ImGuiCol_Button, theme().accent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme().accent_hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme().accent_dark);
    ImGui::PushStyleColor(ImGuiCol_Header, theme().accent);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, theme().accent_hover);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, theme().panel);
    ImVec4 frame_hov = ImVec4(theme().panel.x * 1.3f, theme().panel.y * 1.3f, theme().panel.z * 1.3f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, frame_hov);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, theme().accent_dark);
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, theme().accent);
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, theme().accent_hover);
    ImGui::PushStyleColor(ImGuiCol_Tab, theme().tab_inactive);
    ImGui::PushStyleColor(ImGuiCol_TabHovered, theme().accent_hover);
    ImGui::PushStyleColor(ImGuiCol_TabActive, theme().accent);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, theme().bg);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(theme().bg.x, theme().bg.y, theme().bg.z, 0.6f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, theme().accent_dark);
    ImGui::PushStyleColor(ImGuiCol_Separator, theme().border);
}

static void pop_theme_style() {
    ImGui::PopStyleColor(22);
}

// ---------------------------------------------------------------------------
// Utility button helpers
// ---------------------------------------------------------------------------

bool client_primary_button(const char* label, const ImVec2& size) {
    const std::string display = client_visible_label(label);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 0));
    bool clicked = ImGui::Button(label, size);
    const ImVec2 bmin = ImGui::GetItemRectMin();
    const ImVec2 bmax = ImGui::GetItemRectMax();
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    draw_client_button_surface(bmin, bmax, hovered ? theme().accent_hover : theme().accent,
                               active ? theme().accent_dark : theme().accent_dark,
                               client_alpha(theme().accent_hover, hovered ? 0.95f : 0.68f),
                               ImGui::IsItemFocused());
    const ImVec2 text_size = ImGui::CalcTextSize(display.c_str());
    ImGui::GetWindowDrawList()->AddText(
        ImVec2(bmin.x + (bmax.x - bmin.x - text_size.x) * 0.5f,
               bmin.y + (bmax.y - bmin.y - text_size.y) * 0.5f),
        ImGui::GetColorU32(ImVec4(1, 1, 1, 1)), display.c_str());
    ImGui::PopStyleColor(4);
    return clicked;
}

bool client_secondary_button(const char* label, const ImVec2& size) {
    const std::string display = client_visible_label(label);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 0));
    bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    const ImVec2 bmin = ImGui::GetItemRectMin();
    const ImVec2 bmax = ImGui::GetItemRectMax();
    const bool hovered = ImGui::IsItemHovered();
    ImVec4 top = hovered ? ImVec4(theme().panel.x * 1.22f, theme().panel.y * 1.22f,
                                  theme().panel.z * 1.22f, 1.0f) : theme().panel;
    draw_client_button_surface(bmin, bmax, top, theme().bg,
                               hovered ? theme().accent : theme().border, ImGui::IsItemFocused());
    const ImVec2 text_size = ImGui::CalcTextSize(display.c_str());
    ImGui::GetWindowDrawList()->AddText(
        ImVec2(bmin.x + (bmax.x - bmin.x - text_size.x) * 0.5f,
               bmin.y + (bmax.y - bmin.y - text_size.y) * 0.5f),
        ImGui::GetColorU32(theme().text), display.c_str());
    return clicked;
}

bool client_success_button(const char* label, const ImVec2& size) {
    ImGui::PushStyleColor(ImGuiCol_Button, theme().success);
    ImVec4 hov = ImVec4(theme().success.x * 1.2f, theme().success.y * 1.1f, theme().success.z * 1.2f, 1.0f);
    ImVec4 act = ImVec4(theme().success.x * 0.7f, theme().success.y * 0.85f, theme().success.z * 0.7f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hov);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, act);
    bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    return clicked;
}

bool client_warning_button(const char* label, const ImVec2& size) {
    ImGui::PushStyleColor(ImGuiCol_Button, theme().warning);
    ImVec4 hov = ImVec4(theme().warning.x * 1.05f, theme().warning.y * 1.1f, theme().warning.z * 1.5f, 1.0f);
    ImVec4 act = ImVec4(theme().warning.x * 0.9f, theme().warning.y * 0.85f, theme().warning.z * 0.7f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hov);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, act);
    bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    return clicked;
}

bool client_error_button(const char* label, const ImVec2& size) {
    ImGui::PushStyleColor(ImGuiCol_Button, theme().error);
    ImVec4 hov = ImVec4(theme().error.x * 1.1f, theme().error.y * 1.5f, theme().error.z * 1.5f, 1.0f);
    ImVec4 act = ImVec4(theme().error.x * 0.85f, theme().error.y * 0.7f, theme().error.z * 0.7f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hov);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, act);
    bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    return clicked;
}

void client_text(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    ImGui::TextUnformatted(buf, buf + strlen(buf));
}

void client_text_disabled(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    ImGui::PushStyleColor(ImGuiCol_Text, theme().muted);
    ImGui::TextUnformatted(buf, buf + strlen(buf));
    ImGui::PopStyleColor();
}

void client_separator() { ImGui::Separator(); }

bool client_card_begin(const char* title) {
    // Several secondary pages stack cards vertically. AutoResizeY prevents the
    // old first card from swallowing the whole available page height, which was
    // the main reason those panels felt like unfinished prototypes.
    ImGui::BeginChild(title, ImVec2(0, 0),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
    const ImVec2 min = ImGui::GetWindowPos();
    const ImVec2 max = min + ImGui::GetWindowSize();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(min, max, ImGui::GetColorU32(theme().panel), 10.0f);
    dl->AddRectFilled(ImVec2(min.x + 1.0f, min.y + 1.0f),
                      ImVec2(max.x - 1.0f, min.y + 30.0f),
                      ImGui::GetColorU32(client_alpha(theme().header_bg, 0.9f)), 10.0f,
                      ImDrawFlags_RoundCornersTop);
    dl->AddLine(ImVec2(min.x + 10.0f, min.y + 1.0f), ImVec2(max.x - 10.0f, min.y + 1.0f),
                ImGui::GetColorU32(client_alpha(theme().accent_hover, 0.22f)), 1.0f);
    ImGui::TextColored(theme().accent, "%s", title);
    ImGui::Separator();
    return true;
}

void client_card_end() {
    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// Card with title + optional size
// ---------------------------------------------------------------------------

static bool begin_card(const char* label, const ImVec2& size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme().panel);
    ImGui::PushStyleColor(ImGuiCol_Border, theme().border);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::BeginChild(label, size, ImGuiChildFlags_Borders);
    const ImVec2 pmin = ImGui::GetWindowPos();
    const ImVec2 pmax = pmin + ImGui::GetWindowSize();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pmin, pmax, ImGui::GetColorU32(theme().panel), 10.0f);
    dl->AddRectFilled(ImVec2(pmin.x + 1.0f, pmin.y + 1.0f),
                      ImVec2(pmax.x - 1.0f, pmin.y + std::min(34.0f, pmax.y - pmin.y - 1.0f)),
                      ImGui::GetColorU32(client_alpha(theme().header_bg, 0.62f)), 10.0f,
                      ImDrawFlags_RoundCornersTop);
    dl->AddLine(ImVec2(pmin.x + 10.0f, pmin.y + 1.0f), ImVec2(pmax.x - 10.0f, pmin.y + 1.0f),
                ImGui::GetColorU32(client_alpha(theme().accent_hover, 0.20f)), 1.0f);
    // Subtle hover elevation effect
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
        ImVec4 glow = theme().accent;
        glow.w = 0.06f;
        dl->AddRect(pmin - ImVec2(1, 1), pmax + ImVec2(1, 1),
            ImGui::GetColorU32(glow), 9.0f, 0, 1.0f);
    }
    return true;
}

static void end_card() {
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

// ---------------------------------------------------------------------------
// Sparkline helper
// ---------------------------------------------------------------------------

static void draw_sparkline(const float* data, int count, const ImVec2& size, const ImVec4& color) {
    if (count < 2) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float mn = data[0], mx = data[0];
    for (int i = 1; i < count; ++i) { if (data[i] < mn) mn = data[i]; if (data[i] > mx) mx = data[i]; }
    if (mx - mn < 0.001f) mx = mn + 1.0f;
    for (int i = 1; i < count; ++i) {
        float x0 = p.x + (float(i - 1) / float(count - 1)) * size.x;
        float y0 = p.y + size.y - ((data[i - 1] - mn) / (mx - mn)) * size.y;
        float x1 = p.x + (float(i) / float(count - 1)) * size.x;
        float y1 = p.y + size.y - ((data[i] - mn) / (mx - mn)) * size.y;
        dl->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), ImGui::GetColorU32(color), 1.5f);
    }
    ImGui::Dummy(size);
}

// ---------------------------------------------------------------------------
// Tab button helper
// ---------------------------------------------------------------------------

static bool draw_tab_button(const char* label, bool active, const ImVec2& size) {
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, theme().accent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme().accent_hover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme().accent_dark);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme().accent_hover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme().accent_dark);
        ImGui::PushStyleColor(ImGuiCol_Text, theme().text);
    }
    
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12, 8) *
                        std::clamp(settings().ui_scale, 0.85f, 1.35f));
    
    bool clicked = ImGui::Button(label, size);
    
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);
    
    return clicked;
}

// ---------------------------------------------------------------------------
// Module preset buttons
// ---------------------------------------------------------------------------

static void draw_module_preset_buttons() {
    ImGui::Text("Quick Presets:");
    ImGui::Spacing();

    if (client_primary_button("Combat")) {
        aml::module_set_enabled(aml::MOD_KILLAURA, true);
        aml::module_set_enabled(aml::MOD_AUTOTOOL, true);
    }
    ImGui::SameLine(0, 8.0f);

    if (client_primary_button("Movement")) {
        aml::module_set_enabled(aml::MOD_FLY, true);
        aml::module_set_enabled(aml::MOD_SPEED, true);
    }
    ImGui::SameLine(0, 8.0f);

    if (client_primary_button("Utility")) {
        aml::module_set_enabled(aml::MOD_NOFALL, true);
        aml::module_set_enabled(aml::MOD_FREECAM, true);
    }

    ImGui::Spacing();

    if (client_secondary_button("Reset All")) {
        aml::modules_reset_defaults();
    }
}

// ---------------------------------------------------------------------------
// Init / Shutdown / Toggle
// ---------------------------------------------------------------------------

void ClientUI::init() {
    HudManager::instance().init();
    startup_splash_visible_ = true;
    startup_splash_elapsed_ = 0.0f;
}

void ClientUI::shutdown() {
    startup_splash_visible_ = false;
    startup_splash_elapsed_ = 0.0f;
}

void ClientUI::toggle_menu() {
    menu_open_ = !menu_open_;
    if (!menu_open_) {
        g_menu_target_alpha = 0.0f;
        quick_menu_open_ = false;
    }
}

void ClientUI::open_menu() { menu_open_ = true; }
void ClientUI::close_menu() { menu_open_ = false; g_menu_target_alpha = 0.0f; quick_menu_open_ = false; }
void ClientUI::toggle_quick_menu() { quick_menu_open_ = !quick_menu_open_; }

void ClientUI::set_active_tab(ClientTab tab) { active_tab_ = tab; }

void ClientUI::render_startup_splash(float dt) {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float total = 3.35f;
    startup_splash_elapsed_ += std::max(0.0f, dt);
    const float t = startup_splash_elapsed_;
    if (t >= total) {
        startup_splash_visible_ = false;
        return;
    }

    float alpha = std::min(1.0f, t / 0.42f);
    if (t > total - 0.55f) alpha = std::min(alpha, (total - t) / 0.55f);
    alpha = std::clamp(alpha, 0.0f, 1.0f);
    const ImU32 fade = IM_COL32(255, 255, 255, static_cast<int>(alpha * 255.0f));
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(ImVec2(0, 0), display, IM_COL32(3, 2, 10, 255));

    if (!draw_client_art_foreground("amalgam-loading-hero-ai.png",
                                    ImVec2(0, 0), display, fade)) {
        draw_client_art_foreground("client-splash-ai.png", ImVec2(0, 0), display, fade);
    }
    dl->AddRectFilled(ImVec2(0, 0), display, IM_COL32(3, 2, 12,
        static_cast<int>(178.0f * alpha)));

    const float scale = std::clamp(display.y / 900.0f, 0.75f, 1.25f);
    const ImVec2 center(display.x * 0.285f, display.y * 0.46f);
    ClientArtTexture logo;
    if (load_client_art_texture("amalgam-logo.png", logo) && logo.width > 0 && logo.height > 0) {
        const float logo_w = 260.0f * scale;
        const float logo_h = logo_w * static_cast<float>(logo.height) /
                             static_cast<float>(logo.width);
        draw_client_art_foreground("amalgam-logo.png",
            ImVec2(center.x - logo_w * 0.5f, center.y - logo_h * 0.72f),
            ImVec2(logo_w, logo_h), IM_COL32(255, 255, 255, static_cast<int>(235.0f * alpha)));
    }

    const ImU32 white = IM_COL32(245, 242, 255, static_cast<int>(245.0f * alpha));
    const ImU32 muted = IM_COL32(178, 168, 210, static_cast<int>(235.0f * alpha));
    const ImU32 violet = IM_COL32(174, 76, 255, static_cast<int>(255.0f * alpha));
    const char* status = t < 0.82f ? "Initializing client"
                       : t < 1.58f ? "Loading HUD modules"
                       : t < 2.38f ? "Syncing profile and settings"
                                   : "Ready for your next world";
    const char* version = "AMALGAM CLIENT  •  IN-GAME CLIENT";
    const ImVec2 status_size = ImGui::CalcTextSize(status);
    const ImVec2 version_size = ImGui::CalcTextSize(version);
    dl->AddText(ImVec2(center.x - version_size.x * 0.5f,
                       center.y + 98.0f * scale), muted, version);
    dl->AddText(ImVec2(center.x - status_size.x * 0.5f,
                       center.y + 132.0f * scale), white, status);

    const float bar_w = std::min(430.0f * scale, display.x * 0.36f);
    const float bar_h = 5.0f * scale;
    const ImVec2 bar_min(center.x - bar_w * 0.5f, center.y + 170.0f * scale);
    const ImVec2 bar_max(bar_min.x + bar_w, bar_min.y + bar_h);
    dl->AddRectFilled(bar_min, bar_max, IM_COL32(32, 22, 58, static_cast<int>(230.0f * alpha)), bar_h);
    const float progress = std::clamp(t / total, 0.0f, 1.0f);
    dl->AddRectFilled(bar_min, ImVec2(bar_min.x + bar_w * progress, bar_max.y), violet, bar_h);

    const float spin = t * 2.4f;
    dl->PathClear();
    dl->PathArcTo(center, 205.0f * scale, spin, spin + 4.6f, 48);
    dl->PathStroke(IM_COL32(128, 55, 255, static_cast<int>(155.0f * alpha)), false,
                   2.0f * scale);
}

// ---------------------------------------------------------------------------
// Main render entry (called from hook_swap each frame)
// ---------------------------------------------------------------------------

void ClientUI::render(float dt) {
    if (startup_splash_visible_) {
        render_startup_splash(dt);
        return;
    }
    client_tick(dt);
    NotificationManager::instance().render_toasts(dt);

    if (!menu_open_) {
        if (quick_menu_open_) {
            // Render the radial quick menu as a floating overlay
            float qm_scale = settings().ui_scale;
            float cx = ImGui::GetIO().DisplaySize.x * 0.5f;
            float cy = ImGui::GetIO().DisplaySize.y * 0.5f;
            float radius = 120.0f * qm_scale;
            int selected = draw_radial_menu(cx, cy, radius);
            if (selected >= 0) {
                active_tab_ = static_cast<ClientTab>(selected);
                quick_menu_open_ = false;
                menu_open_ = true;
                g_menu_target_alpha = 1.0f;
                if (active_tab_ == ClientTab::HUD)
                    HudManager::instance().set_editor_active(true);
            }
            HudManager::instance().render_hud();
            return;
        }
        HudManager::instance().render_hud();
        return;
    }

    push_theme_style();

    // ── Keyboard Shortcuts ──────────────────────────────────────
    // Number keys 1-9 for tab switching
    for (int k = 0; k < 9; ++k) {
        if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_1 + k))) {
            g_sub_page = SubPage::None;
            active_tab_ = static_cast<ClientTab>(k);
            if (active_tab_ == ClientTab::HUD)
                HudManager::instance().set_editor_active(true);
        }
    }
    // F5 = refresh data
    if (ImGui::IsKeyPressed(ImGuiKey_F5)) {
        refresh_client_data();
        NotificationManager::instance().push_toast(
            "Refreshed", "Client data refreshed.", NotifyLevel::Info);
    }
    // F1 = toggle HUD elements
    if (ImGui::IsKeyPressed(ImGuiKey_F1)) {
        settings().show_fps = !settings().show_fps;
    }

    float scale = settings().ui_scale;
    float menu_w = 1100.0f * scale;
    float menu_h = 700.0f * scale;
    if (menu_w > ImGui::GetIO().DisplaySize.x * 0.92f) menu_w = ImGui::GetIO().DisplaySize.x * 0.92f;
    if (menu_h > ImGui::GetIO().DisplaySize.y * 0.92f) menu_h = ImGui::GetIO().DisplaySize.y * 0.92f;

    ImGui::SetNextWindowSize(ImVec2(menu_w, menu_h), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(820.0f * scale, 520.0f * scale),
        ImVec2(ImGui::GetIO().DisplaySize.x * 0.94f,
               ImGui::GetIO().DisplaySize.y * 0.94f));
    ImVec2 center(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f);
    ImGui::SetNextWindowPos(center, ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));

    // Smooth fade animation for menu open/close
    if (settings().animations) {
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, g_menu_alpha);
    }

    if (!ImGui::Begin("##amalgam_client", nullptr,
                       ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::End();
        if (settings().animations) ImGui::PopStyleVar();
        pop_theme_style();
        return;
    }

    float total_w = ImGui::GetContentRegionAvail().x;
    float total_h = ImGui::GetContentRegionAvail().y;
    // The client reference uses a true branded navigation rail rather than a
    // narrow icon strip. Keep enough width for the page labels and the
    // Amalgam Client identity while retaining the compact in-game footprint.
    float sidebar_w = 184.0f * scale;
    float right_panel_w = 180.0f * scale;

    // ── Top Header Bar ────────────────────────────────────────────────────
    {
        ImVec2 hp = ImGui::GetCursorScreenPos();
        float header_h = 40.0f * scale;
        ImGui::GetWindowDrawList()->AddRectFilled(
            hp, ImVec2(hp.x + total_w, hp.y + header_h),
            ImGui::GetColorU32(theme().header_bg), 10.0f, ImDrawFlags_RoundCornersTop);
        ImGui::GetWindowDrawList()->AddRectFilledMultiColor(
            hp + ImVec2(1.0f, 1.0f), ImVec2(hp.x + total_w - 1.0f, hp.y + header_h - 1.0f),
            ImGui::GetColorU32(client_alpha(theme().accent_dark, 0.30f)),
            ImGui::GetColorU32(client_alpha(theme().header_bg, 0.92f)),
            ImGui::GetColorU32(client_alpha(theme().header_bg, 0.92f)),
            ImGui::GetColorU32(client_alpha(theme().accent_dark, 0.18f)));
        ImGui::GetWindowDrawList()->AddLine(ImVec2(hp.x + 10.0f, hp.y + header_h - 1.0f),
                                            ImVec2(hp.x + total_w - 10.0f, hp.y + header_h - 1.0f),
                                            ImGui::GetColorU32(client_alpha(theme().accent, 0.72f)), 1.0f);

        // Use the real branded mark beside the wordmark. Keep the text label
        // for readability at compact in-game-client sizes.
        draw_client_art("amalgam-logo.png", ImVec2(hp.x + 10, hp.y + 5),
                        ImVec2(24.0f * scale, 30.0f * scale), false,
                        IM_COL32(255, 255, 255, 255));
        ImGui::SetCursorScreenPos(ImVec2(hp.x + 40.0f * scale, hp.y + 4));
        ImGui::PushFont(ImGui::GetIO().Fonts->Fonts[0]);
        ImGui::TextColored(theme().text, "AMALGAM");
        ImGui::PopFont();

        // INSERT hint centered
        float hint_w = scale * 300.0f;
        ImGui::SetCursorScreenPos(ImVec2(hp.x + (total_w - hint_w) * 0.5f, hp.y + 8));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.15f, 0.20f, 0.8f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(16, 4));
        ImGui::Button("Press INSERT to close", ImVec2(hint_w, scale * 24.0f));
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();

        // Ping + Combo right
        ImGui::SetCursorScreenPos(ImVec2(hp.x + total_w - scale * 200.0f, hp.y + 10));
         if (perf().ping_ms < 0) {
             ImGui::TextDisabled("Ping unavailable");
         } else {
             ImGui::TextColored(theme().success, "\xe2\x96\xb2 %d ms", perf().ping_ms);
         }
        ImGui::SameLine();
         ImGui::TextDisabled("Combo unavailable");

        ImGui::SetCursorScreenPos(ImVec2(hp.x, hp.y + header_h));
    }

    // ── Layout: Sidebar | Content | Right Panel ───────────────────────────
    float content_x = sidebar_w;
    float content_w = total_w - sidebar_w - right_panel_w;
    float content_y = ImGui::GetCursorScreenPos().y;
    float content_h = total_h - 40.0f * scale;

    // ── Sidebar ───────────────────────────────────────────────────────────
    {
        ImVec2 sp = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(
            sp, ImVec2(sp.x + sidebar_w, sp.y + content_h),
            ImGui::GetColorU32(ImVec4(0.05f, 0.05f, 0.07f, 1.0f)));
        ImGui::GetWindowDrawList()->AddRectFilledMultiColor(
            sp + ImVec2(1.0f, 1.0f), ImVec2(sp.x + sidebar_w - 1.0f, sp.y + content_h - 1.0f),
            ImGui::GetColorU32(client_alpha(theme().header_bg, 0.70f)),
            ImGui::GetColorU32(client_alpha(theme().bg, 0.35f)),
            ImGui::GetColorU32(client_alpha(theme().bg, 0.12f)),
            ImGui::GetColorU32(client_alpha(theme().accent_dark, 0.24f)));

        // Branded rail matching the client reference board: real Amalgam art,
        // clear product naming, and a short non-intrusive description.
        const float brand_block_h = 96.0f * scale;
        const ImVec2 brand_pos(sp.x + 12.0f * scale, sp.y + 10.0f * scale);
        const ImVec2 brand_logo_size(52.0f * scale, 52.0f * scale);
        draw_client_art("amalgam-logo.png", brand_pos, brand_logo_size, false,
                        IM_COL32(255, 255, 255, 255));
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(brand_pos.x + 62.0f * scale, brand_pos.y + 5.0f * scale),
            ImGui::GetColorU32(theme().text), "AMALGAM");
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(brand_pos.x + 62.0f * scale, brand_pos.y + 23.0f * scale),
            ImGui::GetColorU32(theme().accent), "IN-GAME CLIENT");
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(brand_pos.x + 62.0f * scale, brand_pos.y + 40.0f * scale),
            ImGui::GetColorU32(theme().muted), "Play. Create. Host.");
        ImGui::GetWindowDrawList()->AddLine(
            ImVec2(sp.x + 12.0f * scale, sp.y + brand_block_h),
            ImVec2(sp.x + sidebar_w - 12.0f * scale, sp.y + brand_block_h),
            ImGui::GetColorU32(theme().border), 1.0f);

        struct TabItem { const char* icon; const char* label; ClientTab tab; };
        TabItem tabs[] = {
            {"M", "MODULES", ClientTab::Modules},
            {"H", "HUD EDITOR", ClientTab::HUD},
            {"P", "PROFILES", ClientTab::Profiles},
            {"C", "COSMETICS", ClientTab::Cosmetics},
            {"S", "SOCIAL", ClientTab::Social},
            {"V", "SERVERS", ClientTab::Servers},
            {"/", "PERFORMANCE", ClientTab::Performance},
            {"G", "SETTINGS", ClientTab::Settings},
            {"D", "DIAGNOSTICS", ClientTab::Diagnostics},
        };

        float btn_h = 48.0f * scale;
        for (int i = 0; i < 9; ++i) {
            float y = sp.y + brand_block_h + 10.0f * scale + i * (btn_h + 4.0f * scale);
            bool active = (active_tab_ == tabs[i].tab);
            const ImVec2 tab_min(sp.x + 4.0f, y);
            const ImVec2 tab_max(sp.x + sidebar_w - 4.0f, y + btn_h);
            const bool tab_hovered = ImGui::IsMouseHoveringRect(tab_min, tab_max);

            if (active) {
                ImGui::GetWindowDrawList()->AddRectFilled(
                    tab_min, tab_max, ImGui::GetColorU32(theme().accent_dark), 8.0f);
                ImGui::GetWindowDrawList()->AddRectFilledMultiColor(
                    tab_min + ImVec2(1.0f, 1.0f), tab_max - ImVec2(1.0f, 1.0f),
                    ImGui::GetColorU32(theme().accent),
                    ImGui::GetColorU32(client_alpha(theme().accent_dark, 0.90f)),
                    ImGui::GetColorU32(client_alpha(theme().accent_dark, 0.92f)),
                    ImGui::GetColorU32(client_alpha(theme().accent, 0.78f)));
                ImGui::GetWindowDrawList()->AddRectFilled(
                    tab_min + ImVec2(1.0f, 8.0f), ImVec2(tab_min.x + 3.0f, tab_max.y - 8.0f),
                    ImGui::GetColorU32(theme().accent_hover), 2.0f);
                ImGui::GetWindowDrawList()->AddRect(tab_min, tab_max,
                    ImGui::GetColorU32(client_alpha(theme().accent_hover, 0.84f)), 8.0f, 0, 1.0f);
            } else if (tab_hovered) {
                ImGui::GetWindowDrawList()->AddRectFilled(tab_min, tab_max,
                    ImGui::GetColorU32(client_alpha(theme().panel, 0.92f)), 8.0f);
                ImGui::GetWindowDrawList()->AddRect(tab_min, tab_max,
                    ImGui::GetColorU32(client_alpha(theme().border, 0.88f)), 8.0f, 0, 1.0f);
            }

            ImVec2 tab_center(sp.x + 24.0f * scale, y + btn_h * 0.5f);
            ImVec2 icon_ts = ImGui::CalcTextSize(tabs[i].icon);
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(tab_center.x - icon_ts.x * 0.5f, tab_center.y - icon_ts.y * 0.5f),
                active ? ImGui::GetColorU32(theme().text) : ImGui::GetColorU32(theme().muted),
                tabs[i].icon);

            ImVec2 lbl_ts = ImGui::CalcTextSize(tabs[i].label);
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(sp.x + 46.0f * scale, tab_center.y - lbl_ts.y * 0.5f),
                active ? ImGui::GetColorU32(theme().text) : ImGui::GetColorU32(theme().muted),
                tabs[i].label);

            // Clickable
            ImGui::SetCursorScreenPos(ImVec2(sp.x + 4, y));
            ImGui::InvisibleButton(("##tab_" + std::to_string(i)).c_str(), ImVec2(sidebar_w - scale * 8.0f, btn_h));
            if (ImGui::IsItemClicked()) {
                g_sub_page = SubPage::None;
                active_tab_ = tabs[i].tab;
                if (tabs[i].tab == ClientTab::HUD)
                    HudManager::instance().set_editor_active(true);
            }

            // Notification badge on Social tab
            if (tabs[i].tab == ClientTab::Social) {
                int unread = NotificationManager::instance().unread_count();
                if (unread > 0) {
                    ImVec2 badge_pos(sp.x + sidebar_w - 14, y + 6);
                    ImGui::GetWindowDrawList()->AddCircleFilled(
                        badge_pos, 8, ImGui::GetColorU32(theme().error));
                    char badge_txt[8];
                    snprintf(badge_txt, sizeof(badge_txt), "%d", unread);
                    ImVec2 bts = ImGui::CalcTextSize(badge_txt);
                    ImGui::GetWindowDrawList()->AddText(
                        ImVec2(badge_pos.x - bts.x * 0.5f, badge_pos.y - bts.y * 0.5f),
                        ImGui::GetColorU32(ImVec4(1, 1, 1, 1)), badge_txt);
                }
            }
        }

        // Version at bottom
        ImGui::GetWindowDrawList()->AddCircleFilled(
            ImVec2(sp.x + 13.0f, sp.y + content_h - 13.0f), 3.5f,
            ImGui::GetColorU32(theme().success));
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(sp.x + 22.0f, sp.y + content_h - 20),
            ImGui::GetColorU32(theme().muted), "BETA v1");

        ImGui::SetCursorScreenPos(ImVec2(content_x, content_y));
    }

    // ── Content Area ──────────────────────────────────────────────────────
    ImGui::SetCursorScreenPos(ImVec2(content_x, content_y));
    ImGui::BeginChild("##content_area", ImVec2(content_w, content_h), false);

    // Page title + pin/minimize/close
    {
        if (active_tab_ == ClientTab::Dashboard) {
            const float art_h = 82.0f * scale;
            const ImVec2 art_pos = ImGui::GetCursorScreenPos();
            const ImVec2 art_size(ImGui::GetContentRegionAvail().x, art_h);
            if (draw_client_art("client-menu-header-ai.png", art_pos, art_size, true)) {
                ImGui::GetWindowDrawList()->AddRectFilled(
                    art_pos, art_pos + art_size,
                    ImGui::GetColorU32(ImVec4(0.02f, 0.01f, 0.06f, 0.46f)), 8.0f);
                ImGui::GetWindowDrawList()->AddRect(
                    art_pos, art_pos + art_size,
                    ImGui::GetColorU32(ImVec4(theme().accent.x, theme().accent.y,
                                              theme().accent.z, 0.4f)), 8.0f, 0, 1.0f);
                ImGui::SetCursorScreenPos(art_pos + ImVec2(16.0f, 12.0f));
                ImGui::TextColored(theme().text, "AMALGAM IN-GAME CLIENT");
                ImGui::SetCursorScreenPos(art_pos + ImVec2(16.0f, 38.0f));
                ImGui::TextColored(theme().muted,
                                   "Play, create, host, and tune your world without leaving Minecraft.");
                draw_client_atlas_tile("client-animation-fx-atlas-ai.png", 3,
                                       ImVec2(art_pos.x + art_size.x - 72.0f,
                                              art_pos.y + 9.0f), 62.0f,
                                       IM_COL32(255, 255, 255, 190));
                ImGui::SetCursorScreenPos(ImVec2(art_pos.x, art_pos.y + art_h + 12.0f));
            }
        }
        const char* title = client_tab_name(active_tab_);
        ImGui::PushFont(ImGui::GetIO().Fonts->Fonts[0]);
        ImGui::TextColored(theme().accent, "%s", title);
        ImGui::PopFont();
        ImGui::SameLine(0, 12);
        ImGui::TextDisabled("v3.0.0-beta.3 | %s", profile().profile_name.empty() ? "Player" : profile().profile_name.c_str());
         float ctrl_btn_w = 28.0f * scale;
         float ctrl_group_w = ctrl_btn_w;
         ImGui::SameLine(content_w - ctrl_group_w);
         if (client_secondary_button("X", ImVec2(ctrl_btn_w, 0))) { close_menu(); }
        ImGui::Spacing();
        client_separator();
        ImGui::Spacing();
    }

    float page_h = ImGui::GetContentRegionAvail().y - 8;
    ImGui::BeginChild("##page_content", ImVec2(0, page_h), false);

    if (g_sub_page != SubPage::None) {
        // Secondary page reached from the dashboard quick actions.
        if (client_secondary_button("< Back", ImVec2(70, 26))) g_sub_page = SubPage::None;
        ImGui::Spacing();
        client_separator();
        ImGui::Spacing();
        if (g_sub_page == SubPage::ModsAndPacks) render_mods_page();
        else if (g_sub_page == SubPage::Essentials) render_essentials_page();
        else if (g_sub_page == SubPage::Screenshots) render_screenshots_page();
    } else {
        switch (active_tab_) {
            case ClientTab::Modules:     render_modules_page(); break;
            case ClientTab::HUD:         render_hud_page(); break;
            case ClientTab::Profiles:    render_profiles_page(); break;
            case ClientTab::Cosmetics:   render_cosmetics_page(); break;
            case ClientTab::Social:      render_essentials_page(); break;
            case ClientTab::Servers:     render_servers_page(); break;
            case ClientTab::Performance: render_performance_page(); break;
            case ClientTab::Settings:    render_settings_page(); break;
            case ClientTab::Diagnostics: render_diagnostics_page(); break;
            default: render_dashboard(); break;
        }
    }

    ImGui::EndChild();
    ImGui::EndChild();

    // ── Right Panel (HUD Modules) ─────────────────────────────────────────
    {
        float rp_x = content_x + content_w;
        ImGui::SetCursorScreenPos(ImVec2(rp_x, content_y));
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(rp_x, content_y), ImVec2(rp_x + right_panel_w, content_y + content_h),
            ImGui::GetColorU32(ImVec4(0.05f, 0.05f, 0.07f, 1.0f)));

        ImGui::SetCursorScreenPos(ImVec2(rp_x + 4, content_y + 8));
        ImGui::BeginChild("##right_panel", ImVec2(right_panel_w - 8, content_h - 16), false);

        // Keystrokes
        begin_card("##keystrokes", ImVec2(-1, 0));
        {
            ImGui::TextColored(theme().accent, "KEYSTROKES");
            ImGui::Spacing();
            float key_size = 32.0f * scale;
            auto key_btn = [&](const char* k, bool held) {
                ImVec4 col = held ? theme().accent : theme().panel;
                ImGui::PushStyleColor(ImGuiCol_Button, col);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
                ImGui::Button(k, ImVec2(key_size, key_size));
                ImGui::PopStyleVar();
                ImGui::PopStyleColor();
            };
            // WASD layout
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + key_size * 0.5f + 4);
            key_btn("W", GetAsyncKeyState('W') & 0x8000);
            ImGui::Spacing();
            key_btn("A", GetAsyncKeyState('A') & 0x8000);
            ImGui::SameLine();
            key_btn("S", GetAsyncKeyState('S') & 0x8000);
            ImGui::SameLine();
            key_btn("D", GetAsyncKeyState('D') & 0x8000);
            ImGui::Spacing();
            ImGui::Columns(2, nullptr, false);
            ImGui::TextDisabled("LMB");
            ImGui::NextColumn();
            ImGui::TextDisabled("RMB");
            ImGui::Columns(1);
        ImGui::Text("%d CPS", clicks_per_second());
        }
        end_card();
        ImGui::Spacing();

        // Armor
        begin_card("##armor", ImVec2(-1, 0));
        {
            ImGui::TextColored(theme().accent, "ARMOR");
            ImGui::Spacing();
            const auto state = aml::player_stats::store().snapshot();
            const char* slots[] = {"Helm", "Chest", "Legs", "Boots"};
            const int durability[] = {
                state.armor_helm_pct, state.armor_chest_pct,
                state.armor_legs_pct, state.armor_boots_pct
            };
            if (!state.has_data) {
                ImGui::TextDisabled("Armor telemetry unavailable");
            } else {
                ImGui::Text("Total: %d/20", state.armor_points);
                for (int i = 0; i < 4; ++i) {
                    if (durability[i] <= 0) ImGui::TextDisabled("%s: Empty", slots[i]);
                    else ImGui::Text("%s: %d%%", slots[i], durability[i]);
                }
            }
        }
        end_card();
        ImGui::Spacing();

        // Effects (active)
        begin_card("##effects", ImVec2(-1, 0));
        {
            ImGui::TextColored(theme().accent, "EFFECTS");
            ImGui::Spacing();
            const auto state = aml::player_stats::store().snapshot();
            if (!state.has_data) {
                ImGui::TextDisabled("Effects telemetry unavailable");
            } else if (state.effects.empty()) {
                ImGui::TextDisabled("No active effects");
            } else {
                for (const auto& effect : state.effects) {
                    const char* name = aml::player_stats::effect_name(effect.effect_id);
                    const std::string effect_name_text = name
                        ? name
                        : "Effect #" + std::to_string(effect.effect_id);
                    char label[96];
                    std::snprintf(label, sizeof(label), "%s %d  %02d:%02d",
                                  effect_name_text.c_str(),
                                  static_cast<int>(effect.amplifier) + 1,
                                  effect.duration_ticks / 1200,
                                  (effect.duration_ticks / 20) % 60);
                    ImGui::Text("%s", label);
                }
            }
        }
        end_card();
        ImGui::Spacing();

        // World module
        begin_card("##world_module", ImVec2(-1, 0));
        {
            ImGui::TextColored(theme().accent, "WORLD");
            ImGui::Spacing();

            const auto state = aml::player_stats::store().snapshot();
            if (!state.has_data) {
                ImGui::TextDisabled("World telemetry unavailable");
            } else {
                const int day_minutes = ((state.day_time + 6000) % 24000) / 1000;
                ImGui::Text("Time: %02d:%02d", day_minutes / 60, day_minutes % 60);
                ImGui::Text("Weather: %s", state.is_raining ? "Rain" : "Clear");
                const char* modes[] = {"Survival", "Creative", "Adventure", "Spectator"};
                const int mode = state.gamemode >= 0 && state.gamemode < 4 ? state.gamemode : -1;
                ImGui::Text("Gamemode: %s", mode >= 0 ? modes[mode] : "Unknown");
                if (state.biome[0] != '\0') ImGui::Text("Biome: %s", state.biome);
                if (state.dimension[0] != '\0') ImGui::Text("Dimension: %s", state.dimension);
            }
        }
        end_card();
        ImGui::Spacing();

        // Online (with pie chart)
        begin_card("##online_module", ImVec2(-1, 0));
        {
            ImGui::TextColored(theme().accent, "ONLINE");
            ImGui::Spacing();

            // Pie chart: entity distribution
            auto& store = aml::tracker::store();
            auto entities = store.entities();
            int player_count = 0, hostile_count = 0, unknown_count = 0;
            for (const auto& e : entities) {
                if (e.kind == aml::tracker::ENTITY_PLAYER) player_count++;
                else if (e.kind == aml::tracker::ENTITY_HOSTILE) hostile_count++;
                else unknown_count++;
            }
            int total = player_count + hostile_count + unknown_count;
            if (total == 0) total = 1;

            // Draw pie chart
            ImVec2 pie_p = ImGui::GetCursorScreenPos();
            float pie_r = scale * 30.0f;
            ImVec2 pie_c(pie_p.x + pie_r + 8, pie_p.y + pie_r + 4);
            ImDrawList* dl = ImGui::GetWindowDrawList();

            auto pie_slice = [&](float start_deg, float end_deg, ImU32 col) {
                float sa = start_deg * 3.14159f / 180.0f;
                float ea = end_deg * 3.14159f / 180.0f;
                int segs = 24;
                dl->PathLineTo(pie_c);
                for (int i = 0; i <= segs; ++i) {
                    float a = sa + (ea - sa) * float(i) / float(segs);
                    dl->PathLineTo(ImVec2(pie_c.x + cosf(a) * pie_r, pie_c.y + sinf(a) * pie_r));
                }
                dl->PathFillConvex(ImGui::GetColorU32(ImVec4(0.08f, 0.08f, 0.10f, 1.0f)));
                dl->PathLineTo(pie_c);
                for (int i = 0; i <= segs; ++i) {
                    float a = sa + (ea - sa) * float(i) / float(segs);
                    dl->PathLineTo(ImVec2(pie_c.x + cosf(a) * (pie_r - 2), pie_c.y + sinf(a) * (pie_r - 2)));
                }
                dl->PathFillConvex(col);
            };

            float p_deg = 0;
            float p_span = (float)player_count / total * 360.0f;
            float h_span = (float)hostile_count / total * 360.0f;
            float u_span = (float)unknown_count / total * 360.0f;
            if (p_span > 0) pie_slice(p_deg, p_deg + p_span, ImGui::GetColorU32(theme().accent));
            p_deg += p_span;
            if (h_span > 0) pie_slice(p_deg, p_deg + h_span, ImGui::GetColorU32(theme().error));
            p_deg += h_span;
            if (u_span > 0) pie_slice(p_deg, p_deg + u_span, ImGui::GetColorU32(theme().muted));

            // Center dot
            dl->AddCircleFilled(pie_c, 12, ImGui::GetColorU32(ImVec4(0.06f, 0.06f, 0.08f, 1.0f)));
            char cnt_buf[8];
            snprintf(cnt_buf, sizeof(cnt_buf), "%d", player_count + hostile_count + unknown_count);
            ImVec2 ts = ImGui::CalcTextSize(cnt_buf);
            dl->AddText(ImVec2(pie_c.x - ts.x * 0.5f, pie_c.y - ts.y * 0.5f),
                ImGui::GetColorU32(theme().text), cnt_buf);

            ImGui::Dummy(ImVec2(pie_r * 2 + scale * 16.0f, pie_r * 2 + scale * 8.0f));

            // Legend (right of pie)
            ImGui::SameLine(pie_r * 2 + 20);
            ImGui::SetCursorPosY(pie_p.y + 4);
            ImGui::TextColored(theme().accent, "\xe2\x96\x88");
            ImGui::SameLine(14);
            ImGui::Text("Players %d", player_count);
            ImGui::TextColored(theme().error, "\xe2\x96\x88");
            ImGui::SameLine(14);
            ImGui::Text("Hostile %d", hostile_count);
            ImGui::TextColored(theme().muted, "\xe2\x96\x88");
            ImGui::SameLine(14);
            ImGui::Text("Other %d", unknown_count);

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // Player list
            if (player_count > 0) {
                ImGui::TextDisabled("Players Online");
                ImGui::Spacing();
                for (const auto& e : entities) {
                    if (e.kind != aml::tracker::ENTITY_PLAYER) continue;
                    ImGui::TextColored(theme().accent, "\xe2\x97\x8f");
                    ImGui::SameLine(14);
                    ImGui::Text("%s", e.name.empty() ? "Unknown" : e.name.c_str());
                    if (e.health >= 0) {
                        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 40);
                        float hp_pct = e.health / 20.0f;
                        ImVec4 hp_col = hp_pct > 0.5f ? theme().success : (hp_pct > 0.25f ? theme().warning : theme().error);
                        ImGui::TextColored(hp_col, "%.0f", e.health);
                    }
                }
            } else {
                ImGui::TextDisabled("No players tracked");
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // Server stats
            ImGui::TextDisabled("TPS");
            ImGui::SameLine(50);
            if (perf().tps < 0.0f) ImGui::TextDisabled("Unavailable");
            else ImGui::Text("%.1f", perf().tps);
            ImGui::TextDisabled("Ping");
            ImGui::SameLine(50);
            if (perf().ping_ms < 0) ImGui::TextDisabled("Unavailable");
            else ImGui::TextColored(theme().success, "%d ms", perf().ping_ms);
            ImGui::TextDisabled("Players");
            ImGui::SameLine(50);
            ImGui::Text("%d tracked", player_count);
        }
        end_card();

        ImGui::EndChild();
    }

    ImGui::End();
    if (settings().animations) ImGui::PopStyleVar();
    pop_theme_style();

    // Always render HUD on top
    HudManager::instance().render_hud();
}

// ---------------------------------------------------------------------------
// Quick Menu (Compact Radial)
// ---------------------------------------------------------------------------

static int draw_radial_menu(float cx, float cy, float radius) {
    int result = -1;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    
    // Keyboard navigation state
    static int kb_selected = -1;
    static float kb_confirm_timer = 0.0f;
    
    struct QuickAction { const char* icon; const char* label; ClientTab tab; };
    QuickAction actions[] = {
        {"M", "Modules", ClientTab::Modules},
        {"H", "HUD Editor", ClientTab::HUD},
        {"P", "Profiles", ClientTab::Profiles},
        {"S", "Social", ClientTab::Social},
        {"V", "Servers", ClientTab::Servers},
        {"G", "Settings", ClientTab::Settings},
    };
    const int count = 6;
    const float slice_angle = 2.0f * 3.14159f / count;
    const float start_angle = -3.14159f / 2.0f; // top center
    const float inner_radius = radius * 0.35f;
    const float hover_radius = radius * 0.95f;
    
    // Keyboard navigation: arrow keys cycle slices, Enter selects
    bool kb_used = false;
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_D)) {
        kb_selected = (kb_selected + 1) % count;
        kb_used = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_A)) {
        kb_selected = (kb_selected - 1 + count) % count;
        kb_used = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_W)) {
        kb_selected = (kb_selected - 2 + count) % count;
        kb_used = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_S)) {
        kb_selected = (kb_selected + 2) % count;
        kb_used = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_Space)) {
        if (kb_selected >= 0 && kb_selected < count) {
            result = static_cast<int>(actions[kb_selected].tab);
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        kb_selected = -1;
    }
    
    // Mouse hover detection overrides keyboard
    ImVec2 mouse_pos = ImGui::GetMousePos();
    float dist_to_center = sqrtf((mouse_pos.x - cx) * (mouse_pos.x - cx) +
                                 (mouse_pos.y - cy) * (mouse_pos.y - cy));
    int hovered_slice = -1;
    
    if (dist_to_center > inner_radius && dist_to_center < hover_radius) {
        float angle = atan2f(mouse_pos.y - cy, mouse_pos.x - cx) - start_angle;
        if (angle < 0) angle += 2.0f * 3.14159f;
        hovered_slice = static_cast<int>(angle / slice_angle) % count;
        kb_selected = hovered_slice; // mouse overrides keyboard
    }
    
    // Active slice = keyboard or mouse
    int active_slice = kb_selected >= 0 ? kb_selected : hovered_slice;
    
    // Background circle
    dl->AddCircleFilled(ImVec2(cx, cy), radius,
        ImGui::GetColorU32(ImVec4(0.06f, 0.06f, 0.08f, 0.85f)), 48);
    dl->AddCircle(ImVec2(cx, cy), radius,
        ImGui::GetColorU32(ImVec4(0.2f, 0.2f, 0.28f, 0.6f)), 48, 1.5f);
    dl->AddCircleFilled(ImVec2(cx, cy), inner_radius,
        ImGui::GetColorU32(ImVec4(0.08f, 0.08f, 0.10f, 0.9f)), 24);
    
    // Draw slices
    for (int i = 0; i < count; ++i) {
        float a0 = start_angle + i * slice_angle;
        float a1 = a0 + slice_angle;
        float a_mid = (a0 + a1) * 0.5f;
        bool is_active = (i == active_slice);
        
        // Slice fill (active = keyboard or mouse hover)
        if (is_active) {
            // Gradient fill: darker center → brighter edge
            dl->PathLineTo(ImVec2(cx, cy));
            int segments = 16;
            for (int s = 0; s <= segments; ++s) {
                float a = a0 + (a1 - a0) * float(s) / float(segments);
                dl->PathLineTo(ImVec2(cx + cosf(a) * hover_radius,
                                      cy + sinf(a) * hover_radius));
            }
            dl->PathFillConvex(ImGui::GetColorU32(
                ImVec4(0.55f, 0.27f, 0.85f, 0.35f)));
            // Inner glow ring
            dl->AddCircle(ImVec2(cx, cy), inner_radius + 4,
                ImGui::GetColorU32(ImVec4(0.55f, 0.27f, 0.85f, 0.20f)), 24, 2.0f);
            // Bright outline for active slice
            dl->PathLineTo(ImVec2(cx, cy));
            for (int s = 0; s <= segments; ++s) {
                float a = a0 + (a1 - a0) * float(s) / float(segments);
                dl->PathLineTo(ImVec2(cx + cosf(a) * hover_radius,
                                      cy + sinf(a) * hover_radius));
            }
            dl->PathStroke(ImGui::GetColorU32(ImVec4(0.65f, 0.37f, 0.95f, 0.7f)), true, 2.0f);
        }
        
        // Slice separator line
        dl->AddLine(ImVec2(cx, cy),
            ImVec2(cx + cosf(a0) * hover_radius, cy + sinf(a0) * hover_radius),
            ImGui::GetColorU32(ImVec4(0.2f, 0.2f, 0.28f, 0.4f)), 1.0f);
        
        // Label
        float label_r = (inner_radius + hover_radius) * 0.5f;
        float lx = cx + cosf(a_mid) * label_r;
        float ly = cy + sinf(a_mid) * label_r;
        
        ImVec2 icon_ts = ImGui::CalcTextSize(actions[i].icon);
        dl->AddText(ImVec2(lx - icon_ts.x * 0.5f, ly - icon_ts.y * 0.5f - 8),
            ImGui::GetColorU32(is_active ? ImVec4(1, 1, 1, 1) : ImVec4(0.7f, 0.7f, 0.75f, 1)),
            actions[i].icon);
        
        ImVec2 lbl_ts = ImGui::CalcTextSize(actions[i].label);
        dl->AddText(ImVec2(lx - lbl_ts.x * 0.5f, ly - lbl_ts.y * 0.5f + 8),
            ImGui::GetColorU32(is_active ? ImVec4(1, 1, 1, 1) : ImVec4(0.5f, 0.5f, 0.55f, 1)),
            actions[i].label);
        
        // Handle click
        if (is_active && ImGui::IsMouseClicked(0)) {
            result = static_cast<int>(actions[i].tab);
        }
    }
    
    // Center brand mark. This keeps the radial menu tied to the same real
    // logo as the launcher header and About panel.
    if (!draw_client_art("amalgam-logo.png", ImVec2(cx - 18.0f, cy - 18.0f),
                         ImVec2(36.0f, 36.0f), false,
                         IM_COL32(255, 255, 255, 255))) {
        ImVec2 center_ts = ImGui::CalcTextSize("A");
        dl->AddText(ImVec2(cx - center_ts.x * 0.5f, cy - center_ts.y * 0.5f),
            ImGui::GetColorU32(ImVec4(0.55f, 0.27f, 0.85f, 1)), "A");
    }
    
    // Keyboard hint at bottom
    if (active_slice >= 0) {
        const char* hint = "Arrow keys to navigate | Enter to select | Esc to close";
        ImVec2 hint_ts = ImGui::CalcTextSize(hint);
        dl->AddText(ImVec2(cx - hint_ts.x * 0.5f, cy + radius + 12),
            ImGui::GetColorU32(ImVec4(0.5f, 0.5f, 0.55f, 0.7f)), hint);
    }

    return result;
}

// ---------------------------------------------------------------------------
// Dashboard
// ---------------------------------------------------------------------------

void ClientUI::render_dashboard() {
    float scale = settings().ui_scale;
    float avail = ImGui::GetContentRegionAvail().x;

    // Three-column layout below the card grid (reference panel 2 body)
    const float col_gap = 8.0f;
    const float col1 = (avail - col_gap * 2.0f) / 3.0f;
    const float col2 = col1;
    const float col3 = col1;

    // Header matching reference panel 2
    ImGui::TextColored(theme().accent, "AMALGAM CLIENT");
    ImGui::SameLine();
        ImGui::TextDisabled("v3.0.0-beta.3");
    ImGui::Spacing();
    ImGui::TextDisabled("The Amalgam In-Game Client brings the power of your launcher");
    ImGui::TextDisabled("and Essentials directly inside Minecraft.");
    ImGui::Spacing();
    client_separator();
    ImGui::Spacing();

    // 6 icon cards in 2×3 grid matching reference panel 2
    struct DashCard { const char* icon; const char* title; const char* desc; ClientTab tab; };
    DashCard cards[] = {
        {"M", "Modules", "Enable gameplay modifications", ClientTab::Modules},
        {"H", "HUD Editor", "Customize your HUD", ClientTab::HUD},
        {"P", "Profiles", "Manage profiles and configs", ClientTab::Profiles},
        {"C", "Cosmetics", "Capes, badges, and more", ClientTab::Cosmetics},
        {"S", "Social", "Friends, parties, and invites", ClientTab::Social},
        {"V", "Performance", "Optimize your experience", ClientTab::Performance},
    };
    const int cols = 3;
    const int rows = 2;
    float card_gap = 8.0f * scale;
    float card_w = (avail - card_gap * (cols - 1)) / static_cast<float>(cols);
    float card_h = 90.0f * scale;

    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            int idx = r * cols + c;
            if (c > 0) ImGui::SameLine(0, card_gap);
            const auto& card = cards[idx];

            ImVec2 card_pos = ImGui::GetCursorScreenPos();
            ImVec2 card_size(card_w, card_h);

            // Card background with border
            ImGui::GetWindowDrawList()->AddRectFilled(
                card_pos, card_pos + card_size,
                ImGui::GetColorU32(theme().panel), 8.0f);
            ImGui::GetWindowDrawList()->AddRect(
                card_pos, card_pos + card_size,
                ImGui::GetColorU32(theme().border), 8.0f);

            // Hover effect
            ImGui::InvisibleButton(("##card" + std::to_string(idx)).c_str(), card_size);
            bool hovered = ImGui::IsItemHovered();
            if (hovered) {
                ImGui::GetWindowDrawList()->AddRect(
                    card_pos - ImVec2(1, 1), card_pos + card_size + ImVec2(1, 1),
                    ImGui::GetColorU32(theme().accent), 9.0f, 0, 1.5f);
            }
            if (ImGui::IsItemClicked()) {
                active_tab_ = card.tab;
                if (card.tab == ClientTab::HUD)
                    HudManager::instance().set_editor_active(true);
            }

            // Icon circle
            float icon_r = 16.0f * scale;
            ImVec2 icon_center(card_pos.x + 24.0f * scale, card_pos.y + card_h * 0.5f);
            ImGui::GetWindowDrawList()->AddCircleFilled(
                icon_center, icon_r,
                ImGui::GetColorU32(hovered ? theme().accent_hover : theme().accent));
            ImVec2 icon_ts = ImGui::CalcTextSize(card.icon);
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(icon_center.x - icon_ts.x * 0.5f, icon_center.y - icon_ts.y * 0.5f),
                ImGui::GetColorU32(ImVec4(1, 1, 1, 1)), card.icon);

            // Title + description
            ImGui::SetCursorScreenPos(ImVec2(card_pos.x + 50.0f * scale, card_pos.y + 12.0f * scale));
            ImGui::Text("%s", card.title);
            ImGui::SetCursorScreenPos(ImVec2(card_pos.x + 50.0f * scale, card_pos.y + 32.0f * scale));
            ImGui::TextDisabled("%s", card.desc);

            ImGui::SetCursorScreenPos(ImVec2(card_pos.x, card_pos.y + card_h));
        }
    }

    ImGui::Spacing();
    client_separator();
    ImGui::Spacing();

    // Quick status row below cards
    ImGui::TextColored(theme().accent, "QUICK STATUS");
    ImGui::Spacing();
    float stat_w = (avail - 8.0f) / 3.0f;
    begin_card("##dash_fps", ImVec2(stat_w, 0));
    ImGui::TextColored(theme().success, "FPS");
    if (perf().fps > 0.0f) {
        ImGui::Text("%.0f", perf().fps);
        ImGui::TextDisabled("1%% Low: %.0f", perf().fps_1pct_low);
    } else {
        ImGui::TextDisabled("N/A");
        ImGui::TextDisabled("Waiting for game telemetry");
    }
    end_card();
    ImGui::SameLine(0, 4.0f);
    begin_card("##dash_ping", ImVec2(stat_w, 0));
    ImGui::TextColored(theme().accent, "PING");
    if (perf().ping_ms >= 0) ImGui::Text("%d ms", perf().ping_ms);
    else ImGui::TextDisabled("N/A");
    ImGui::TextDisabled(perf().ping_ms >= 0 ? "Server connection" : "No server telemetry");
    end_card();
    ImGui::SameLine(0, 4.0f);
    begin_card("##dash_mods", ImVec2(stat_w, 0));
    ImGui::TextColored(theme().warning, "MODS");
    ImGui::Text("%d", static_cast<int>(mods().size()));
    ImGui::TextDisabled("Loaded in profile");
    end_card();

    // Column 1: Profile (compact) ─────────────────────────────────────────────────
    ImGui::BeginChild("##dash_profile", ImVec2(col1, 0), true);
    {
        ImGui::TextColored(theme().accent, "PROFILE");
        ImGui::Spacing();

        // Profile artwork is provided by the launcher; keep the client honest
        // when no profile has been selected.
        ImVec2 ip = ImGui::GetCursorScreenPos();
         const bool has_profile = !profile().profile_name.empty();
         ImGui::GetWindowDrawList()->AddRectFilled(ip, ImVec2(ip.x + 80.0f * scale, ip.y + 100.0f * scale),
             ImGui::GetColorU32(has_profile ? theme().accent : theme().panel), 8.0f);
         ImGui::Dummy(ImVec2(80.0f * scale, 100.0f * scale));
         ImGui::SameLine(96.0f * scale);

        // Modpack name
        ImGui::PushFont(ImGui::GetIO().Fonts->Fonts[0]);
        ImGui::Text("%s", profile().profile_name.empty() ? "No profile selected" : profile().profile_name.c_str());
        ImGui::PopFont();
        ImGui::TextDisabled("Active Minecraft profile");

        // Badges
         const bool connected = profile().connected;
         ImVec4 badge_bg = connected ? theme().success : theme().muted;
         ImGui::PushStyleColor(ImGuiCol_Button, badge_bg);
         ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
         ImGui::Button(connected ? "LIVE" : "OFFLINE", ImVec2(60.0f * scale, 20.0f * scale));
        ImGui::SameLine();
         char mod_buf[32];
         snprintf(mod_buf, sizeof(mod_buf), "%d Mods", static_cast<int>(mods().size()));
         ImGui::Button(mod_buf, ImVec2(80.0f * scale, 20.0f * scale));
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Stats
        auto stat_row = [](const char* label, const char* value) {
            ImGui::TextColored(theme().muted, "%s", label);
            ImGui::SameLine(120);
            ImGui::Text("%s", value);
        };
        stat_row("Minecraft Version", profile().mc_version.empty() ? "Unknown" : profile().mc_version.c_str());
        const std::string loader = profile().loader.empty() ? "Unknown" :
            (profile().loader + " " + profile().loader_version);
        stat_row("Loader", loader.c_str());
        const int64_t session_seconds = profile().session_start > 0
            ? std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count() - profile().session_start)
            : 0;
        char session_time[32];
        std::snprintf(session_time, sizeof(session_time), "%lldm",
                      static_cast<long long>(session_seconds / 60));
        stat_row("Session Time", profile().session_start > 0 ? session_time : "Not started");
        stat_row("Status", profile().session_status.empty() ? "Unknown" : profile().session_status.c_str());

        ImGui::Spacing();
         if (client_primary_button("CLOSE CLIENT MENU", ImVec2(-1, 36))) close_menu();
    }
    ImGui::EndChild();

    ImGui::SameLine(8);

    // ── Column 2: Status Overview + Quick Actions ─────────────────────────
    ImGui::BeginChild("##dash_center", ImVec2(col2, 0), false);
    {
        // Status Overview
        ImGui::TextColored(theme().accent, "STATUS OVERVIEW");
        ImGui::Spacing();

        float metric_card_w = (col2 - 16) / 3.0f;
        auto metric_card = [&](const char* label, const char* value, const char* sub,
                               const float* spark, int spark_n, const ImVec4& col) {
            begin_card(("##m_" + std::string(label)).c_str(), ImVec2(metric_card_w, 70));
            ImGui::TextColored(col, "%s", label);
            ImGui::PushFont(ImGui::GetIO().Fonts->Fonts[0]);
            ImGui::Text("%s", value);
            ImGui::PopFont();
            ImGui::TextDisabled("%s", sub);
            if (spark && spark_n > 1) draw_sparkline(spark, spark_n, ImVec2(ImGui::GetContentRegionAvail().x, 16), col);
            end_card();
        };

        // Row 1: FPS, Ping, RAM
        ImGui::PushID("##row1");
        char fps_value[32];
        std::snprintf(fps_value, sizeof(fps_value), "%.0f", perf().fps);
        char fps_sub[48];
        std::snprintf(fps_sub, sizeof(fps_sub), "1%% Low: %.0f", perf().fps_1pct_low);
        metric_card("FPS", perf().fps > 0.0f ? fps_value : "Unavailable",
                    perf().fps > 0.0f ? fps_sub : "Waiting for game telemetry",
                    nullptr, 0, theme().success);
        ImGui::SameLine(0, 4);
        char ping_value[32];
        std::snprintf(ping_value, sizeof(ping_value), "%d ms", perf().ping_ms);
        metric_card("PING", perf().ping_ms >= 0 ? ping_value : "Unavailable",
                    "Current connection", nullptr, 0, theme().accent);
        ImGui::SameLine(0, 4);
        char ram_value[48];
        std::snprintf(ram_value, sizeof(ram_value), "%.1f / %.1f GB",
                      perf().ram_mb / 1024.0f, perf().ram_max_mb / 1024.0f);
        metric_card("RAM", perf().ram_max_mb > 0.0f ? ram_value : "Unavailable",
                    perf().ram_max_mb > 0.0f ? "System memory usage" : "Waiting for system telemetry", nullptr, 0,
                    ImVec4(0.3f, 0.6f, 1.0f, 1.0f));
        ImGui::PopID();

        ImGui::Spacing();

        // Row 2: CPU, TPS, Players
        ImGui::PushID("##row2");
        char cpu_value[32];
        std::snprintf(cpu_value, sizeof(cpu_value), "%.0f%%", perf().cpu_percent);
         metric_card("CPU", cpu_value, "System CPU usage", nullptr, 0,
                     ImVec4(0.3f, 0.8f, 0.4f, 1.0f));
         ImGui::SameLine(0, 4);
         char tps_value[32];
         if (perf().tps < 0.0f) std::snprintf(tps_value, sizeof(tps_value), "Unavailable");
         else std::snprintf(tps_value, sizeof(tps_value), "%.1f", perf().tps);
         metric_card("TPS", tps_value, "Server telemetry", nullptr, 0, theme().warning);
         ImGui::SameLine(0, 4);
         int active_players = 0;
         for (const auto& entity : aml::tracker::store().entities())
             if (entity.kind == aml::tracker::ENTITY_PLAYER) ++active_players;
         char players_value[32];
         std::snprintf(players_value, sizeof(players_value), "%d", active_players);
         metric_card("PLAYERS", players_value, "Tracked in range", nullptr, 0, theme().accent);
        ImGui::PopID();

        ImGui::Dummy(ImVec2(0, 12));

        // Quick Actions
        ImGui::TextColored(theme().accent, "QUICK ACTIONS");
        ImGui::Spacing();
        float qa_w = (col2 - 8) / 2.0f;
        auto qa_btn = [&](const char* label) {
            if (client_secondary_button(label, ImVec2(qa_w, 32))) {
                if (strcmp(label, "HUD Editor") == 0) { active_tab_ = ClientTab::HUD; HudManager::instance().set_editor_active(true); }
                else if (strcmp(label, "Performance") == 0) active_tab_ = ClientTab::Performance;
                else if (strcmp(label, "Mod & Packs") == 0) g_sub_page = SubPage::ModsAndPacks;
                else if (strcmp(label, "Screenshots") == 0) g_sub_page = SubPage::Screenshots;
                else if (strcmp(label, "Essentials") == 0) g_sub_page = SubPage::Essentials;
                else if (strcmp(label, "Invite Friends") == 0) active_tab_ = ClientTab::Social;
                else if (strcmp(label, "Host World") == 0) active_tab_ = ClientTab::Social;
            }
        };
        qa_btn("HUD Editor"); ImGui::SameLine(0, 4); qa_btn("Performance");
        qa_btn("Mod & Packs"); ImGui::SameLine(0, 4); qa_btn("Screenshots");
        qa_btn("Essentials"); ImGui::SameLine(0, 4); qa_btn("Invite Friends");
        qa_btn("Host World");
    }
    ImGui::EndChild();

    ImGui::SameLine(8);

    // ── Column 3: Notifications + Active Session ──────────────────────────
    ImGui::BeginChild("##dash_right", ImVec2(col3, 0), false);
    {
        // Notifications
        ImGui::TextColored(theme().accent, "NOTIFICATIONS");
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 60);
        if (ImGui::SmallButton("CLEAR")) {
            NotificationManager::instance().clear();
        }
        ImGui::Spacing();

        begin_card("##notif_card", ImVec2(-1, 180));
        {
            auto& nm = NotificationManager::instance();
            auto all = nm.get_unread();
            if (all.empty()) all = nm.get_all();
            int shown = 0;
            for (const auto& n : all) {
                if (shown >= 5) break;
                ImVec4 col = theme().muted;
                if (n.level == NotifyLevel::Success) col = theme().success;
                else if (n.level == NotifyLevel::Warning) col = theme().warning;
                else if (n.level == NotifyLevel::Error) col = theme().error;
                else col = theme().accent;

                // Notification row matching reference panel 10
                ImVec2 rp = ImGui::GetCursorScreenPos();
                ImVec2 rs(ImGui::GetContentRegionAvail().x, 28 * scale);
                ImGui::InvisibleButton(("##notif" + std::to_string(shown)).c_str(), rs);

                // Type-specific icon circle
                const char* notif_icon = "i";
                if (n.title.find("Friend") != std::string::npos) notif_icon = "F";
                else if (n.title.find("Party") != std::string::npos) notif_icon = "P";
                else if (n.title.find("World") != std::string::npos) notif_icon = "W";
                else if (n.title.find("Update") != std::string::npos) notif_icon = "U";
                ImGui::GetWindowDrawList()->AddCircleFilled(
                    ImVec2(rp.x + 12, rp.y + 10), 8, ImGui::GetColorU32(col));
                ImVec2 icon_ts = ImGui::CalcTextSize(notif_icon);
                ImGui::GetWindowDrawList()->AddText(
                    ImVec2(rp.x + 12 - icon_ts.x * 0.5f, rp.y + 10 - icon_ts.y * 0.5f),
                    ImGui::GetColorU32(ImVec4(1, 1, 1, 1)), notif_icon);

                // Title
                ImGui::SetCursorScreenPos(ImVec2(rp.x + 26, rp.y + 2));
                ImGui::Text("%s", n.title.c_str());

                // Time ago
                int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                int64_t diff = now - n.timestamp;
                char time_buf[16];
                if (diff < 60) snprintf(time_buf, sizeof(time_buf), "%llds", diff);
                else if (diff < 3600) snprintf(time_buf, sizeof(time_buf), "%lldm", diff / 60);
                else snprintf(time_buf, sizeof(time_buf), "%lldh", diff / 3600);
                ImGui::SetCursorScreenPos(ImVec2(rp.x + rs.x - 40, rp.y + 2));
                ImGui::TextDisabled("%s", time_buf);

                ImGui::SetCursorScreenPos(ImVec2(rp.x, rp.y + rs.y));
                shown++;
            }
            if (shown == 0) {
                ImGui::TextDisabled("No notifications");
            }
        }
        end_card();

        ImGui::Dummy(ImVec2(0, 12));

        // Active Session
        ImGui::TextColored(theme().accent, "ACTIVE SESSION");
        ImGui::Spacing();

        begin_card("##session_card", ImVec2(-1, 0));
        {
            ImGui::Text("%s", profile().profile_name.empty() ? "No active session" : profile().profile_name.c_str());
            ImGui::TextDisabled("%s", profile().session_status.empty()
                ? "Join or host a session from Essentials" : profile().session_status.c_str());

            ImGui::Spacing();
            float btn_w = (ImGui::GetContentRegionAvail().x - 4) / 2.0f;             if (client_secondary_button("Open Essentials", ImVec2(btn_w, 28))) active_tab_ = ClientTab::Social;
            ImGui::SameLine(0, 4);
             if (client_secondary_button("Refresh", ImVec2(btn_w, 28))) refresh_client_data();
        }
        end_card();
    }
    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// HUD Page
// ---------------------------------------------------------------------------

void ClientUI::render_hud_page() {
    static int hud_tab = 0; // 0 = Layout, 1 = Elements, 2 = Presets
    static int preset_idx = 0;
    auto presets = HudManager::instance().get_presets();

    // Tab buttons
    ImGui::Text("HUD Configuration");
    ImGui::Spacing();
    
    float tab_w = 100.0f * settings().ui_scale;
    if (draw_tab_button("Layout", hud_tab == 0, ImVec2(tab_w, 0))) hud_tab = 0;
    ImGui::SameLine(0, 8.0f);
    if (draw_tab_button("Elements", hud_tab == 1, ImVec2(tab_w, 0))) hud_tab = 1;
    ImGui::SameLine(0, 8.0f);
    if (draw_tab_button("Presets", hud_tab == 2, ImVec2(tab_w, 0))) hud_tab = 2;
    
    ImGui::Spacing();
    client_separator();
    ImGui::Spacing();

    switch (hud_tab) {
        case 0: // Layout tab
            ImGui::Text("Preset");
            ImGui::SameLine(80);
            ImGui::SetNextItemWidth(160);
            if (ImGui::Combo("##preset", &preset_idx, preset_combo_getter,
                    &presets, static_cast<int>(presets.size()))) {
                HudManager::instance().apply_preset(presets[preset_idx]);
            }

            ImGui::SameLine();
            if (client_primary_button("Apply")) {
                HudManager::instance().apply_preset(presets[preset_idx]);
            }

            ImGui::SameLine();
            ImGui::Spacing();
            ImGui::SameLine();

            if (client_secondary_button("Save Layout")) {
                HudManager::instance().save_layout("default");
            }
            ImGui::SameLine();
            if (client_secondary_button("Load Layout")) {
                HudManager::instance().load_layout("default");
            }
            ImGui::SameLine();
            if (client_secondary_button("Reset All")) {
                HudManager::instance().reset_layout();
            }

            ImGui::Spacing();
            client_separator();
            ImGui::Spacing();

            HudManager::instance().render_editor();

            if (HudManager::instance().is_preview_mode()) {
                HudManager::instance().render_hud();
            }
            break;
            
        case 1: { // Elements tab
            struct HudElement {
                const char* name;
                float x, y;
                float scale;
                bool enabled;
                const char* icon;
            };
            static HudElement elements[] = {
                {"FPS",         0.01f, 0.01f, 1.0f, true,  "F"},
                {"Ping",        0.01f, 0.04f, 1.0f, true,  "P"},
                {"CPS",         0.01f, 0.07f, 1.0f, true,  "C"},
                {"Coordinates", 0.01f, 0.50f, 1.0f, true,  "X"},
                {"Compass",     0.45f, 0.01f, 1.0f, true,  ">"},
                {"Potion Effects",0.85f, 0.30f, 1.0f, true, "E"},
                {"Server Info",  0.45f, 0.01f, 1.0f, true,  "S"},
                {"Armor Status", 0.85f, 0.50f, 1.0f, true,  "A"},
                {"Health",       0.40f, 0.92f, 1.0f, true,  "H"},
                {"Hotbar",       0.30f, 0.95f, 1.0f, true,  "B"},
                {"Time",         0.45f, 0.01f, 1.0f, true,  "T"},
            };
            static int selected_elem = -1;
            static char elem_search[64] = {};
            
            float scale = settings().ui_scale;
            float list_w = ImGui::GetContentRegionAvail().x * 0.55f;
            float config_w = ImGui::GetContentRegionAvail().x - list_w - 8;
            
            ImGui::Text("HUD Elements");
            ImGui::SameLine(list_w - 160);
            ImGui::SetNextItemWidth(160);
            ImGui::InputTextWithHint("##elem_search", "Search elements...", elem_search, sizeof(elem_search));
            ImGui::Spacing();
            
            // Element list
            ImGui::BeginChild("##elem_list", ImVec2(list_w, 0), true);
            {
                for (int i = 0; i < 11; ++i) {
                    if (elem_search[0] != '\0') {
                        std::string lower = elements[i].name;
                        lowercase_ascii(lower);
                        std::string search = elem_search;
                        lowercase_ascii(search);
                        if (lower.find(search) == std::string::npos) continue;
                    }
                    
                    ImGui::PushID(i);
                    bool is_sel = (selected_elem == i);
                    
                    ImVec2 row_pos = ImGui::GetCursorScreenPos();
                    ImVec2 row_size(ImGui::GetContentRegionAvail().x, 32 * scale);
                    
                    if (is_sel) {
                        ImGui::GetWindowDrawList()->AddRectFilled(
                            row_pos, row_pos + row_size,
                            ImGui::GetColorU32(ImVec4(theme().accent.x, theme().accent.y, theme().accent.z, 0.15f)),
                            4.0f);
                    }
                    
                    ImGui::InvisibleButton(("##elem" + std::to_string(i)).c_str(), row_size);
                    if (ImGui::IsItemClicked()) selected_elem = i;
                    
                    // Element name
                    ImGui::SetCursorScreenPos(ImVec2(row_pos.x + 8, row_pos.y + 6));
                    ImGui::Text("%s", elements[i].name);
                    
                    // Position
                    ImGui::SameLine(list_w * 0.45f);
                    ImGui::TextDisabled("(%.0f%%, %.0f%%)", elements[i].x * 100, elements[i].y * 100);
                    
                    // Enable toggle
                    ImGui::SetCursorScreenPos(ImVec2(row_pos.x + row_size.x - 48, row_pos.y + 6));
                    bool en = elements[i].enabled;
                    ImGui::PushStyleColor(ImGuiCol_Button, en ? theme().success : theme().panel);
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.0f);
                    if (ImGui::Button(en ? "ON" : "OFF", ImVec2(40, 18))) {
                        elements[i].enabled = !elements[i].enabled;
                    }
                    ImGui::PopStyleVar();
                    ImGui::PopStyleColor();
                    
                    ImGui::SetCursorScreenPos(ImVec2(row_pos.x, row_pos.y + row_size.y));
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();
            
            ImGui::SameLine(8);
            
            // Config panel for selected element
            ImGui::BeginChild("##elem_config", ImVec2(config_w, 0), true);
            {
                if (selected_elem >= 0 && selected_elem < 11) {
                    auto& e = elements[selected_elem];
                    ImGui::TextColored(theme().accent, "%s", e.name);
                    ImGui::Spacing();
                    client_separator();
                    ImGui::Spacing();
                    
                    // Position sliders
                    ImGui::Text("Position X");
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::SliderFloat("##posx", &e.x, 0.0f, 1.0f, "%.0f%%")) {
                        e.x = std::clamp(e.x, 0.0f, 1.0f);
                    }
                    
                    ImGui::Text("Position Y");
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::SliderFloat("##posy", &e.y, 0.0f, 1.0f, "%.0f%%")) {
                        e.y = std::clamp(e.y, 0.0f, 1.0f);
                    }
                    
                    ImGui::Text("Scale");
                    ImGui::SetNextItemWidth(-1);
                    ImGui::SliderFloat("##scale", &e.scale, 0.5f, 2.0f, "%.1fx");
                    
                    // Enable/disable
                    bool en = e.enabled;
                    if (ImGui::Checkbox("Enabled", &en)) {
                        e.enabled = en;
                    }
                    
                    ImGui::Spacing();
                    client_separator();
                    ImGui::Spacing();
                    
                    // Reference-specific settings
                    ImGui::Text("Opacity");
                    ImGui::SetNextItemWidth(-1);
                    static float global_opacity = 1.0f;
                    ImGui::SliderFloat("##opacity", &global_opacity, 0.1f, 1.0f, "%.0f%%");
                    
                    ImGui::Text("Shadow");
                    ImGui::SameLine(120);
                    static bool hud_shadow = true;
                    ImGui::Checkbox("##shadow", &hud_shadow);
                    
                    ImGui::Text("Background");
                    ImGui::SameLine(120);
                    static bool hud_bg = true;
                    ImGui::Checkbox("##bg", &hud_bg);
                    
                    ImGui::Text("Snap to Grid");
                    ImGui::SameLine(120);
                    static bool snap_grid = false;
                    ImGui::Checkbox("##snapgrid", &snap_grid);
                    
                    ImGui::Text("Grid Size");
                    ImGui::SetNextItemWidth(-1);
                    static int grid_size = 10;
                    ImGui::SliderInt("##gridsize", &grid_size, 4, 32);
                    
                    ImGui::Spacing();
                    client_separator();
                    ImGui::Spacing();
                    
                    // Snap buttons
                    ImGui::Text("Snap to:");
                    float snap_w = (ImGui::GetContentRegionAvail().x - 8) / 2.0f;
                    if (client_secondary_button("Top Left", ImVec2(snap_w, 24))) { e.x = 0.01f; e.y = 0.01f; }
                    ImGui::SameLine(0, 4);
                    if (client_secondary_button("Top Right", ImVec2(snap_w, 24))) { e.x = 0.85f; e.y = 0.01f; }
                    if (client_secondary_button("Bottom Left", ImVec2(snap_w, 24))) { e.x = 0.01f; e.y = 0.90f; }
                    ImGui::SameLine(0, 4);
                    if (client_secondary_button("Bottom Right", ImVec2(snap_w, 24))) { e.x = 0.85f; e.y = 0.90f; }
                    if (client_secondary_button("Center", ImVec2(snap_w, 24))) { e.x = 0.45f; e.y = 0.45f; }
                    ImGui::SameLine(0, 4);
                    if (client_secondary_button("Reset", ImVec2(snap_w, 24))) { e.x = 0.1f; e.y = 0.1f; e.scale = 1.0f; }
                    
                    ImGui::Spacing();
                    client_separator();
                    ImGui::Spacing();
                    
                    // Preview mode toggle
                    ImGui::Text("Preview");
                    ImGui::SameLine();
                    bool preview = HudManager::instance().is_preview_mode();
                    if (client_secondary_button(preview ? "Disable" : "Enable", ImVec2(0, 24))) {
                        HudManager::instance().set_preview_mode(!preview);
                    }
                    ImGui::Spacing();
                    client_separator();
                    ImGui::Spacing();
                    // Done button matching reference
                    if (client_primary_button("Done", ImVec2(-1, 32))) {
                        HudManager::instance().set_editor_active(false);
                    }
                } else {
                    ImGui::TextColored(theme().accent, "SELECT AN ELEMENT");
                    ImGui::Spacing();
                    ImGui::TextDisabled("Click an element to configure");
                    ImGui::TextDisabled("its position, scale and state.");
                    ImGui::Spacing();
                    ImGui::TextDisabled("Use snap buttons for common");
                    ImGui::TextDisabled("positions, or drag sliders for");
                    ImGui::TextDisabled("precise placement.");
                }
            }
            ImGui::EndChild();
            }
            break;
            
        case 2: // Presets tab
            ImGui::Text("Quick Presets");
            ImGui::Spacing();
            draw_module_preset_buttons();
            
            ImGui::Spacing();
            client_separator();
            ImGui::Spacing();
            
            ImGui::Text("Custom Presets");
            ImGui::Spacing();
            ImGui::TextDisabled("Save your HUD layouts as presets for quick access");
            
            ImGui::Spacing();
            if (client_primary_button("Save Current as Preset")) {
                // Save current layout as new preset
            }
            
            ImGui::Spacing();
            client_separator();
            ImGui::Spacing();
            
            ImGui::Text("Available Presets");
            ImGui::Spacing();
            for (size_t i = 0; i < presets.size(); ++i) {
                ImGui::BulletText("%s", presets[i].c_str());
                ImGui::SameLine();
                if (client_secondary_button("Load")) {
                    HudManager::instance().apply_preset(presets[i]);
                }
                ImGui::SameLine();
                if (client_secondary_button("Delete")) {
                    // Delete preset
                }
            }
            break;
    }
}

// ---------------------------------------------------------------------------
// Performance Page
// ---------------------------------------------------------------------------

void ClientUI::render_performance_page() {
    float scale = settings().ui_scale;
    static int perf_section = 0; // 0=Overview, 1=FPS Graph, 2=TPS Graph, 3=Memory, 4=CPU, 5=Network, 6=Optimize
    static float fps_history[64] = {};
    static float ping_history[64] = {};
    static float tps_history[64] = {};
    static float ram_history[64] = {};
    static int hist_idx = 0;
    static int selected_preset = 1; // 0=Low, 1=Balanced, 2=High, 3=Custom
    static bool smart_render = true;
    static bool entity_culling = false;
    static bool chunk_culling = true;

    // Accumulate history every ~20 frames
    static int frame_counter = 0;
    frame_counter++;
    if (frame_counter % 20 == 0) {
        fps_history[hist_idx % 64] = perf().fps;
        ping_history[hist_idx % 64] = perf().ping_ms >= 0 ? (float)perf().ping_ms : 0;
        tps_history[hist_idx % 64] = perf().tps >= 0 ? perf().tps : 20.0f;
        ram_history[hist_idx % 64] = perf().ram_mb / 1024.0f;
        hist_idx++;
    }

    // Sidebar navigation matching reference panel 8
    const char* perf_sections[] = {"Overview", "FPS Graph", "TPS Graph", "Memory", "CPU", "Network", "Optimize"};
    const int perf_section_count = 7;
    float sidebar_w = 120.0f * scale;
    float content_w = ImGui::GetContentRegionAvail().x - sidebar_w - 8;

    // Sidebar
    ImGui::BeginChild("##perf_sidebar", ImVec2(sidebar_w, 0), true);
    {
        ImGui::TextColored(theme().accent, "PERFORMANCE");
        ImGui::Spacing();
        client_separator();
        ImGui::Spacing();
        for (int i = 0; i < perf_section_count; ++i) {
            bool active = (perf_section == i);
            if (active) ImGui::PushStyleColor(ImGuiCol_Button, theme().accent);
            else ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            if (ImGui::Button(perf_sections[i], ImVec2(-1, 28 * scale))) perf_section = i;
            ImGui::PopStyleColor();
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // Content area
    ImGui::BeginChild("##perf_content", ImVec2(content_w, 0), false);
    {
    // Section content based on sidebar selection
    switch (perf_section) {
    case 0: // Overview
    {
    // ── Optimization Preset ──────────────────────────────────────
    ImGui::TextColored(theme().accent, "OPTIMIZATION PRESET");
    ImGui::Spacing();
    float preset_w = (ImGui::GetContentRegionAvail().x - 12) / 4.0f;
    const char* preset_names[] = {"Low End", "Balanced", "High End", "Custom"};
    for (int i = 0; i < 4; ++i) {
        bool active = (selected_preset == i);
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button, theme().accent);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, theme().panel);
        }
        if (ImGui::Button(preset_names[i], ImVec2(preset_w, 32 * scale))) {
            selected_preset = i;
        }
        ImGui::PopStyleColor();
        if (i < 3) ImGui::SameLine(4);
    }
    ImGui::Spacing();
    client_separator();
    ImGui::Spacing();

    // ── Top Metric Cards ─────────────────────────────────────────
    float card_w = (ImGui::GetContentRegionAvail().x - 12) / 4.0f;
    auto metric_card = [&](const char* label, const char* value, const char* sub,
                           const float* spark, int spark_n, const ImVec4& col) {
        begin_card(("##perf_" + std::string(label)).c_str(), ImVec2(card_w, 80 * scale));
        ImGui::TextColored(col, "%s", label);
        ImGui::PushFont(ImGui::GetIO().Fonts->Fonts[0]);
        ImGui::Text("%s", value);
        ImGui::PopFont();
        ImGui::TextDisabled("%s", sub);
        if (spark && spark_n > 1) {
            ImGui::Spacing();
            draw_sparkline(spark, spark_n, ImVec2(ImGui::GetContentRegionAvail().x, 20), col);
        }
        end_card();
    };

    char fps_val[32], fps_sub[48];
    snprintf(fps_val, sizeof(fps_val), "%.0f", perf().fps);
    snprintf(fps_sub, sizeof(fps_sub), "1%% Low: %.0f", perf().fps_1pct_low);
    metric_card("FPS", perf().fps > 0.0f ? fps_val : "---",
                perf().fps > 0.0f ? fps_sub : "Waiting for game telemetry",
                fps_history, 64, theme().success);

    ImGui::SameLine(0, 4);
    char ping_val[32];
    snprintf(ping_val, sizeof(ping_val), "%d ms", perf().ping_ms);
    metric_card("PING", perf().ping_ms >= 0 ? ping_val : "---",
                "Current connection", ping_history, 64, theme().accent);

    ImGui::SameLine(0, 4);
    char tps_val[32];
    snprintf(tps_val, sizeof(tps_val), "%.1f", perf().tps);
    metric_card("TPS", perf().tps >= 0 ? tps_val : "---",
                "Server timing", tps_history, 64, theme().warning);

    ImGui::SameLine(0, 4);
    char ram_val[48];
    snprintf(ram_val, sizeof(ram_val), "%.1f / %.1f GB",
             perf().ram_mb / 1024.0f, perf().ram_max_mb / 1024.0f);
    metric_card("MEMORY", perf().ram_max_mb > 0.0f ? ram_val : "---",
                perf().ram_max_mb > 0.0f ? "System RAM usage" : "Waiting for system telemetry", ram_history, 64,
                ImVec4(0.3f, 0.6f, 1.0f, 1.0f));

    ImGui::Spacing();

    // ── Secondary Metrics Row ────────────────────────────────────
    float sec_w = (ImGui::GetContentRegionAvail().x - 8) / 3.0f;
    begin_card("##perf_cpu", ImVec2(sec_w, 0));
    {
        ImGui::TextColored(theme().accent, "CPU");
        ImGui::PushFont(ImGui::GetIO().Fonts->Fonts[0]);
        ImGui::Text("%.1f%%", perf().cpu_percent);
        ImGui::PopFont();
        ImGui::TextDisabled("System CPU usage");
        if (perf().cpu_percent > 80.0f) {
            ImGui::TextColored(theme().warning, "High CPU usage");
        }
    }
    end_card();
    ImGui::SameLine(0, 4);

    begin_card("##perf_gpu", ImVec2(sec_w, 0));
    {
        ImGui::TextColored(theme().accent, "GPU");
        ImGui::PushFont(ImGui::GetIO().Fonts->Fonts[0]);
        if (perf().gpu_percent > 0.0f) ImGui::Text("%.1f%%", perf().gpu_percent);
        else ImGui::TextDisabled("N/A");
        ImGui::PopFont();
        ImGui::TextDisabled("Render distance: %d chunks", perf().render_distance);
    }
    end_card();
    ImGui::SameLine(0, 4);

    begin_card("##perf_network", ImVec2(sec_w, 0));
    {
        ImGui::TextColored(theme().accent, "NETWORK");
        ImGui::PushFont(ImGui::GetIO().Fonts->Fonts[0]);
        ImGui::Text("%.1f ms", perf().frame_time_ms);
        ImGui::PopFont();
        ImGui::TextDisabled("Frame time");
        if (perf().ping_ms >= 0) {
            ImGui::TextColored(perf().ping_ms < 50 ? theme().success : perf().ping_ms < 100 ? theme().warning : theme().error,
                "Ping: %dms", perf().ping_ms);
        }
    }
    end_card();

    ImGui::Spacing();
    client_separator();
    ImGui::Spacing();

    // ── Optimization Toggles ─────────────────────────────────────
    ImGui::TextColored(theme().accent, "OPTIMIZATION");
    ImGui::Spacing();

    float toggle_w = (ImGui::GetContentRegionAvail().x - 8) / 2.0f;
    begin_card("##perf_opt1", ImVec2(toggle_w, 0));
    {
        ImGui::Text("Smart Render");
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 80);
        ImGui::PushStyleColor(ImGuiCol_Button, smart_render ? theme().success : theme().panel);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12.0f);
        if (ImGui::Button(smart_render ? "ON" : "OFF", ImVec2(70, 22))) {
            smart_render = !smart_render;
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        ImGui::TextDisabled("Optimizes rendering pipeline");
    }
    end_card();
    ImGui::SameLine(0, 4);

    begin_card("##perf_opt2", ImVec2(toggle_w, 0));
    {
        ImGui::Text("Entity Culling");
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 80);
        ImGui::PushStyleColor(ImGuiCol_Button, entity_culling ? theme().success : theme().panel);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12.0f);
        if (ImGui::Button(entity_culling ? "ON" : "OFF", ImVec2(70, 22))) {
            entity_culling = !entity_culling;
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        ImGui::TextDisabled("Culls hidden entities");
    }
    end_card();
    ImGui::SameLine(0, 4);

    begin_card("##perf_opt3", ImVec2(toggle_w, 0));
    {
        ImGui::Text("Chunk Culling");
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 80);
        ImGui::PushStyleColor(ImGuiCol_Button, chunk_culling ? theme().success : theme().panel);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12.0f);
        if (ImGui::Button(chunk_culling ? "ON" : "OFF", ImVec2(70, 22))) {
            chunk_culling = !chunk_culling;
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        ImGui::TextDisabled("Skips unseen chunks");
    }
    end_card();

    ImGui::Spacing();
    ImGui::TextDisabled("Additional graphics settings are managed by Minecraft's options file.");
    break;
    }

    case 1: // FPS Graph
    {
        ImGui::TextColored(theme().accent, "FPS GRAPH");
        ImGui::Spacing();
        begin_card("##fps_graph", ImVec2(-1, 220));
        ImVec2 graph_pos = ImGui::GetCursorScreenPos();
        float graph_w = ImGui::GetContentRegionAvail().x;
        draw_sparkline(fps_history, 64, ImVec2(graph_w - 40, 160), theme().success);
        // Axis labels
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(graph_pos.x + graph_w - 30, graph_pos.y + 168),
            ImGui::GetColorU32(theme().muted), "60s");
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(graph_pos.x, graph_pos.y + 168),
            ImGui::GetColorU32(theme().muted), "0s");
        end_card();
        ImGui::Spacing();
        ImGui::Text("Current: ");
        ImGui::SameLine();
        ImGui::TextColored(theme().success, "%.0f FPS", perf().fps);
        ImGui::Text("1%% Low: ");
        ImGui::SameLine();
        ImGui::TextDisabled("%.0f", perf().fps_1pct_low);
        break;
    }

    case 2: // TPS Graph
    {
        ImGui::TextColored(theme().accent, "TPS GRAPH");
        ImGui::Spacing();
        begin_card("##tps_graph", ImVec2(-1, 220));
        ImVec2 tps_gp = ImGui::GetCursorScreenPos();
        float tps_gw = ImGui::GetContentRegionAvail().x;
        draw_sparkline(tps_history, 64, ImVec2(tps_gw - 40, 160), theme().warning);
        ImGui::GetWindowDrawList()->AddText(ImVec2(tps_gp.x + tps_gw - 30, tps_gp.y + 168), ImGui::GetColorU32(theme().muted), "60s");
        ImGui::GetWindowDrawList()->AddText(ImVec2(tps_gp.x, tps_gp.y + 168), ImGui::GetColorU32(theme().muted), "0s");
        end_card();
        ImGui::Spacing();
        ImGui::Text("Current: ");
        ImGui::SameLine();
        ImGui::TextColored(theme().warning, "%.1f TPS", perf().tps);
        break;
    }

    case 3: // Memory
    {
        ImGui::TextColored(theme().accent, "MEMORY");
        ImGui::Spacing();
        begin_card("##mem_graph", ImVec2(-1, 220));
        ImVec2 mem_gp = ImGui::GetCursorScreenPos();
        float mem_gw = ImGui::GetContentRegionAvail().x;
        draw_sparkline(ram_history, 64, ImVec2(mem_gw - 40, 160), ImVec4(0.3f, 0.6f, 1.0f, 1.0f));
        ImGui::GetWindowDrawList()->AddText(ImVec2(mem_gp.x + mem_gw - 30, mem_gp.y + 168), ImGui::GetColorU32(theme().muted), "60s");
        ImGui::GetWindowDrawList()->AddText(ImVec2(mem_gp.x, mem_gp.y + 168), ImGui::GetColorU32(theme().muted), "0s");
        end_card();
        ImGui::Spacing();
        ImGui::Text("Used: ");
        ImGui::SameLine();
        ImGui::TextDisabled("%.1f / %.1f GB", perf().ram_mb / 1024.0f, perf().ram_max_mb / 1024.0f);
        float pct = perf().ram_max_mb > 0 ? perf().ram_mb / perf().ram_max_mb * 100.0f : 0.0f;
        ImGui::Text("Usage: ");
        ImGui::SameLine();
        ImGui::TextColored(pct > 80 ? theme().error : pct > 60 ? theme().warning : theme().success, "%.0f%%", pct);
        break;
    }

    case 4: // CPU
    {
        ImGui::TextColored(theme().accent, "CPU");
        ImGui::Spacing();
        begin_card("##cpu_info", ImVec2(-1, 0));
        ImGui::Text("Usage: ");
        ImGui::SameLine();
        ImGui::TextColored(perf().cpu_percent > 80 ? theme().error : theme().success, "%.1f%%", perf().cpu_percent);
        ImGui::TextDisabled("System CPU usage");
        if (perf().cpu_percent > 80.0f) {
            ImGui::TextColored(theme().warning, "High CPU usage - consider closing background apps.");
        }
        end_card();
        break;
    }

    case 5: // Network
    {
        ImGui::TextColored(theme().accent, "NETWORK");
        ImGui::Spacing();
        begin_card("##net_info", ImVec2(-1, 0));
        ImGui::Text("Ping: ");
        ImGui::SameLine();
        if (perf().ping_ms >= 0) {
            ImGui::TextColored(perf().ping_ms < 50 ? theme().success : perf().ping_ms < 100 ? theme().warning : theme().error,
                "%d ms", perf().ping_ms);
        } else {
            ImGui::TextDisabled("N/A");
        }
        ImGui::Text("Frame Time: ");
        ImGui::SameLine();
        ImGui::TextDisabled("%.1f ms", perf().frame_time_ms);
        ImGui::Text("Server: ");
        ImGui::SameLine();
        ImGui::TextDisabled("%s", perf().server_address.empty() ? "Unknown" : perf().server_address.c_str());
        end_card();
        break;
    }

    case 6: // Optimize
    {
        ImGui::TextColored(theme().accent, "OPTIMIZATION");
        ImGui::Spacing();

        // ── Optimization Toggles ─────────────────────────────────────
        float toggle_w = (ImGui::GetContentRegionAvail().x - 8) / 2.0f;
        begin_card("##perf_opt1", ImVec2(toggle_w, 0));
        {
            ImGui::Text("Smart Render");
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 80);
            ImGui::PushStyleColor(ImGuiCol_Button, smart_render ? theme().success : theme().panel);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12.0f);
            if (ImGui::Button(smart_render ? "ON" : "OFF", ImVec2(70, 22))) {
                smart_render = !smart_render;
            }
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
            ImGui::TextDisabled("Optimizes rendering pipeline");
        }
        end_card();
        ImGui::SameLine(0, 4);

        begin_card("##perf_opt2", ImVec2(toggle_w, 0));
        {
            ImGui::Text("Entity Culling");
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 80);
            ImGui::PushStyleColor(ImGuiCol_Button, entity_culling ? theme().success : theme().panel);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12.0f);
            if (ImGui::Button(entity_culling ? "ON" : "OFF", ImVec2(70, 22))) {
                entity_culling = !entity_culling;
            }
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
            ImGui::TextDisabled("Culls hidden entities");
        }
        end_card();
        ImGui::SameLine(0, 4);

        begin_card("##perf_opt3", ImVec2(toggle_w, 0));
        {
            ImGui::Text("Chunk Culling");
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 80);
            ImGui::PushStyleColor(ImGuiCol_Button, chunk_culling ? theme().success : theme().panel);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12.0f);
            if (ImGui::Button(chunk_culling ? "ON" : "OFF", ImVec2(70, 22))) {
                chunk_culling = !chunk_culling;
            }
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
            ImGui::TextDisabled("Skips unseen chunks");
        }
        end_card();

        ImGui::Spacing();
        ImGui::TextDisabled("Additional graphics settings are managed by Minecraft's options file.");
        break;
    }

    } // end switch
    }
    ImGui::EndChild(); // perf_content
}

// ---------------------------------------------------------------------------
// Mods Page
// ---------------------------------------------------------------------------

void ClientUI::render_mods_page() {
    float scale = settings().ui_scale;
    static int mods_tab = 0; // 0=Mods, 1=Resource Packs, 2=Shaders, 3=Datapacks
    static int selected_mod = -1;
    static char search_buf[128] = {};
    auto& mod_list = mods();

    // Tab bar matching reference
    ImGui::TextColored(theme().accent, "MODS & PACKS");
    ImGui::Spacing();

    const char* tab_names[] = {"Mods", "Resource Packs", "Shaders", "Datapacks"};
    for (int i = 0; i < 4; ++i) {
        bool active = (mods_tab == i);
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button, theme().accent);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        }
        if (ImGui::Button(tab_names[i], ImVec2(0, 28 * scale))) {
            mods_tab = i;
        }
        ImGui::PopStyleColor();
        if (i < 3) ImGui::SameLine(4);
    }

    // Active/Inactive count tabs matching reference panel 9
    static int mod_state_tab = 0; // 0=Active, 1=Inactive
    int active_count = 0, inactive_count = 0;
    for (const auto& m : mod_list) { if (m.enabled) active_count++; else inactive_count++; }
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 200);
    char active_label[32], inactive_label[32];
    snprintf(active_label, sizeof(active_label), "Active (%d)", active_count);
    snprintf(inactive_label, sizeof(inactive_label), "Inactive (%d)", inactive_count);
    if (draw_tab_button(active_label, mod_state_tab == 0, ImVec2(0, 24 * scale))) mod_state_tab = 0;
    ImGui::SameLine(0, 4);
    if (draw_tab_button(inactive_label, mod_state_tab == 1, ImVec2(0, 24 * scale))) mod_state_tab = 1;

    // Search + count
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 240);
    ImGui::TextDisabled("Active (%d)", static_cast<int>(mod_list.size()));
    ImGui::SameLine(0, 8);
    ImGui::SetNextItemWidth(160);
    ImGui::InputTextWithHint("##mod_search", "Search mods...", search_buf, sizeof(search_buf));

    ImGui::Spacing();
    client_separator();
    ImGui::Spacing();

    // Mods list + detail panel (split view)
    float list_width = ImGui::GetContentRegionAvail().x * 0.60f;
    float detail_width = ImGui::GetContentRegionAvail().x - list_width - 8;

    // ── Mod List ─────────────────────────────────────────────────
    ImGui::BeginChild("##mod_list", ImVec2(list_width, 0), true);
    {
        if (mod_list.empty()) {
            ImGui::Spacing();
            ImGui::Spacing();
            float center_x = ImGui::GetContentRegionAvail().x * 0.5f;
            ImGui::SetCursorPosX(center_x - 40);
            ImGui::PushStyleColor(ImGuiCol_Button, theme().accent);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 20.0f);
            ImGui::Button("M", ImVec2(60, 60));
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
            ImGui::Spacing();
            ImGui::SetCursorPosX(center_x - ImGui::CalcTextSize("No mods installed").x * 0.5f);
            ImGui::TextColored(theme().muted, "No mods installed");
            ImGui::SetCursorPosX(center_x - ImGui::CalcTextSize("Install mods through the launcher.").x * 0.5f);
            ImGui::TextDisabled("Install mods through the launcher.");
        } else {
            int vis_idx = 0;
            for (size_t i = 0; i < mod_list.size(); ++i) {
                const auto& m = mod_list[i];

                // Search filter
                if (search_buf[0] != '\0') {
                    std::string lower_name = m.name;
                    lowercase_ascii(lower_name);
                    std::string lower_search = search_buf;
                    lowercase_ascii(lower_search);
                    if (lower_name.find(lower_search) == std::string::npos) continue;
                }

                ImGui::PushID(m.filename.c_str());
                bool is_selected = (selected_mod == vis_idx);

                // Row background
                ImVec2 row_pos = ImGui::GetCursorScreenPos();
                ImVec2 row_size(ImGui::GetContentRegionAvail().x, 44 * scale);
                if (is_selected) {
                    ImGui::GetWindowDrawList()->AddRectFilled(
                        row_pos, row_pos + row_size,
                        ImGui::GetColorU32(ImVec4(theme().accent.x, theme().accent.y, theme().accent.z, 0.15f)),
                        4.0f);
                }

                ImGui::InvisibleButton(("##modrow" + std::to_string(i)).c_str(), row_size);
                if (ImGui::IsItemClicked()) selected_mod = vis_idx;

                // Draw mod info
                ImGui::SetCursorScreenPos(ImVec2(row_pos.x + 8, row_pos.y + 6));

                // Status indicator dot
                ImVec4 status_col = m.enabled ? theme().success : theme().muted;
                ImGui::GetWindowDrawList()->AddCircleFilled(
                    ImVec2(row_pos.x + 14, row_pos.y + 16), 4,
                    ImGui::GetColorU32(status_col));
                ImGui::SameLine(24);

                // Mod name
                ImGui::Text("%s", m.name.c_str());
                ImGui::SameLine(list_width * 0.55f);
                ImGui::TextDisabled("v%s", m.version.c_str());

                // Author line
                ImGui::SetCursorScreenPos(ImVec2(row_pos.x + 24, row_pos.y + 24));
                ImGui::TextDisabled("%s", m.filename.c_str());

                // Update badge
                if (m.has_update) {
                    ImGui::SetCursorScreenPos(ImVec2(row_pos.x + row_size.x - 100, row_pos.y + 14));
                    ImGui::TextColored(theme().warning, "Update Available");
                }

                // Enable toggle
                ImGui::SetCursorScreenPos(ImVec2(row_pos.x + row_size.x - 50, row_pos.y + 12));
                bool en = m.enabled;
                ImGui::PushStyleColor(ImGuiCol_Button, en ? theme().success : theme().panel);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.0f);
                if (ImGui::Button(en ? "ON" : "OFF", ImVec2(40, 18))) {
                    set_mod_enabled(m.filename, !en);
                }
                ImGui::PopStyleVar();
                ImGui::PopStyleColor();

                ImGui::SetCursorScreenPos(ImVec2(row_pos.x, row_pos.y + row_size.y));
                ImGui::PopID();
                vis_idx++;
            }
        }
    }
    ImGui::EndChild();

    ImGui::SameLine(8);

    // ── Detail Panel ─────────────────────────────────────────────
    ImGui::BeginChild("##mod_detail", ImVec2(detail_width, 0), true);
    {
        // Find the selected mod (re-scan accounting for search filter)
        int sel_idx = 0;
        const ModInfo* selected = nullptr;
        for (size_t i = 0; i < mod_list.size(); ++i) {
            const auto& m = mod_list[i];
            if (search_buf[0] != '\0') {
                std::string lower_name = m.name;
                lowercase_ascii(lower_name);
                std::string lower_search = search_buf;
                lowercase_ascii(lower_search);
                if (lower_name.find(lower_search) == std::string::npos) continue;
            }
            if (sel_idx == selected_mod) {
                selected = &m;
                break;
            }
            sel_idx++;
        }

        if (selected) {
            ImGui::TextColored(theme().accent, "%s", selected->name.c_str());
            ImGui::TextDisabled("v%s", selected->version.c_str());
            ImGui::Spacing();
            client_separator();
            ImGui::Spacing();

            // Status
            bool en = selected->enabled;
            ImGui::Text("Status");
            ImGui::SameLine(80);
            ImGui::TextColored(en ? theme().success : theme().muted, en ? "Enabled" : "Disabled");

            // Filename
            ImGui::Text("File");
            ImGui::SameLine(80);
            ImGui::TextDisabled("%s", selected->filename.c_str());

            // Author (metadata from filename heuristic)
            ImGui::Text("Author");
            ImGui::SameLine(80);
            ImGui::TextDisabled("Unknown");
            
            // Type
            ImGui::Text("Type");
            ImGui::SameLine(80);
            ImGui::TextDisabled("Mod");
            
            // The launcher performs version/loader compatibility checks at
            // install time. The in-game client can truthfully confirm only
            // that this file belongs to the active profile.
            ImGui::Text("Profile state");
            ImGui::SameLine(80);
            ImGui::TextColored(theme().success, "Installed here");

            // Update status
            if (selected->has_update) {
                ImGui::Spacing();
                ImGui::TextColored(theme().warning, "Update available");
                ImGui::TextDisabled("An update is available for this mod.");
            }

            ImGui::Spacing();
            client_separator();
            ImGui::Spacing();

            // Actions
            float btn_w = -1;
            if (client_primary_button(en ? "Disable" : "Enable", ImVec2(btn_w, 28))) {
                set_mod_enabled(selected->filename, !en);
            }
            if (client_secondary_button("Open Folder", ImVec2(btn_w, 28))) {
                const std::filesystem::path folder = std::filesystem::path(game_dir()) / "mods";
                ShellExecuteA(nullptr, "open", folder.string().c_str(), nullptr, nullptr, SW_SHOW);
            }
        } else {
            ImGui::TextColored(theme().accent, "SELECT A MOD");
            ImGui::Spacing();
            ImGui::TextDisabled("Click a mod to view details.");
            ImGui::Spacing();
            ImGui::TextDisabled("View version, status,");
            ImGui::TextDisabled("compatibility and manage");
            ImGui::TextDisabled("individual mods.");
        }
    }
    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// Essentials Page
// ---------------------------------------------------------------------------

void ClientUI::render_essentials_page() {
    float scale = settings().ui_scale;
    auto& friend_list = friends();
    static int ess_section = 0; // 0=Friends, 1=Parties, 2=Invites, 3=Hosted Worlds, 4=Join World, 5=Settings
    static char search_buf[64] = {};

    // Sidebar matching reference panel 5
    float sidebar_w = 140.0f * scale;
    float content_w = ImGui::GetContentRegionAvail().x - sidebar_w - 8;

    ImGui::BeginChild("##ess_sidebar", ImVec2(sidebar_w, 0), true);
    {
        ImGui::TextColored(theme().accent, "ESSENTIALS");
        ImGui::Spacing();
        client_separator();
        ImGui::Spacing();
        const char* sections[] = {"Friends", "Parties", "Invites", "Hosted Worlds", "Join World", "Settings"};
        for (int i = 0; i < 6; ++i) {
            bool active = (ess_section == i);
            if (active) ImGui::PushStyleColor(ImGuiCol_Button, theme().accent);
            else ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            // Show invite count badge for Invites
            if (i == 2) {
                if (ImGui::Button((std::string(sections[i]) + " (0)").c_str(), ImVec2(-1, 28 * scale))) ess_section = i;
            } else {
                if (ImGui::Button(sections[i], ImVec2(-1, 28 * scale))) ess_section = i;
            }
            ImGui::PopStyleColor();
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // Content area
    ImGui::BeginChild("##ess_content", ImVec2(content_w, 0), false);
    {
    switch (ess_section) {
    case 0: // Friends
    {
        ImGui::TextColored(theme().accent, "Friends");
        ImGui::SameLine(content_w - 200);
        ImGui::SetNextItemWidth(160);
        ImGui::InputTextWithHint("##esearch", "Search friends...", search_buf, sizeof(search_buf));
        ImGui::Spacing();
        client_separator();
        ImGui::Spacing();

        client_card_begin("FRIENDS");
        if (friend_list.empty()) {
            ImGui::Spacing();
            float cx = ImGui::GetContentRegionAvail().x * 0.5f;
            ImGui::SetCursorPosX(cx - 40);
            ImGui::PushStyleColor(ImGuiCol_Button, theme().accent);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 20.0f);
            ImGui::Button("F", ImVec2(60, 60));
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
            ImGui::Spacing();
            ImGui::SetCursorPosX(cx - ImGui::CalcTextSize("No friends online").x * 0.5f);
            ImGui::TextColored(theme().muted, "No friends online");
            ImGui::SetCursorPosX(cx - ImGui::CalcTextSize("Add friends in the launcher").x * 0.5f);
            ImGui::TextDisabled("Add friends in the launcher");
        } else {
            for (const auto& f : friend_list) {
                if (search_buf[0] != '\0') {
                    std::string lower = f.name;
                    lowercase_ascii(lower);
                    std::string search = search_buf;
                    lowercase_ascii(search);
                    if (lower.find(search) == std::string::npos) continue;
                }
                ImGui::PushID(f.uuid.c_str());
                ImVec2 rp = ImGui::GetCursorScreenPos();
                ImVec2 rs(ImGui::GetContentRegionAvail().x, 36 * scale);
                ImGui::InvisibleButton("##frow", rs);
                bool is_online = f.status.find("Online") != std::string::npos ||
                                 f.status.find("Playing") != std::string::npos;
                ImVec4 dot_col = is_online ? theme().success : theme().muted;
                ImGui::GetWindowDrawList()->AddCircleFilled(
                    ImVec2(rp.x + 14, rp.y + rs.y * 0.5f), 5,
                    ImGui::GetColorU32(dot_col));
                ImGui::SetCursorScreenPos(ImVec2(rp.x + 26, rp.y + 4));
                ImGui::Text("%s", f.name.c_str());
                ImGui::SameLine(160);
                ImGui::TextColored(dot_col, "%s", f.status.c_str());
                ImGui::SetCursorScreenPos(ImVec2(rp.x, rp.y + rs.y));
                ImGui::PopID();
            }
        }
        client_card_end();
        break;
    }

    case 1: // Parties
    {
        ImGui::TextColored(theme().accent, "Parties");
        ImGui::Spacing();
        client_card_begin("PARTIES");
        ImGui::TextDisabled("No active parties.");
        ImGui::TextDisabled("Create or join a party from the launcher.");
        client_card_end();
        break;
    }

    case 2: // Invites
    {
        ImGui::TextColored(theme().accent, "Invites");
        ImGui::Spacing();
        client_card_begin("INVITES");
        ImGui::TextDisabled("No pending invites.");
        client_card_end();
        break;
    }

    case 3: // Hosted Worlds
    {
        ImGui::TextColored(theme().accent, "Hosted Worlds");
        ImGui::Spacing();
        client_card_begin("HOSTED WORLDS");
        ImGui::TextDisabled("No worlds currently hosted.");
        ImGui::Spacing();
        ImGui::TextDisabled("Start a hosted world from the launcher Essentials tab.");
        client_card_end();
        break;
    }

    case 4: // Join World
    {
        ImGui::TextColored(theme().accent, "Join World");
        ImGui::Spacing();
        client_card_begin("JOIN WORLD");
        ImGui::TextDisabled("Join a world from the launcher Essentials tab.");
        client_card_end();
        break;
    }

    case 5: // Settings
    {
        ImGui::TextColored(theme().accent, "Essentials Settings");
        ImGui::Spacing();
        client_card_begin("SETTINGS");
        const bool essentials_connected = settings().launcher_connection && sync_status().synced;
        ImGui::TextColored(essentials_connected ? theme().success : theme().warning,
                           essentials_connected ? "Connected to launcher" : "Offline / limited mode");
        ImGui::TextDisabled(essentials_connected ? "Shared profile bridge" :
                                                    "Local features remain available");
        ImGui::Spacing();
        ImGui::TextDisabled(essentials_connected
            ? "Connection status and privacy settings"
            : "Reconnect the launcher to use friends, parties, and cloud features");
        client_card_end();
        break;
    }

    } // end switch

    // Connection footer matching reference
    ImGui::Spacing();
    client_separator();
    ImGui::Spacing();
    const bool essentials_connected = settings().launcher_connection && sync_status().synced;
    ImGui::TextColored(essentials_connected ? theme().success : theme().warning,
                       essentials_connected ? "Connected to launcher" : "Offline / limited mode");
    ImGui::TextDisabled(essentials_connected ? "Shared profile bridge" :
                                                "Local features remain available");
    }
    ImGui::EndChild(); // ess_content
}

// ---------------------------------------------------------------------------
// Servers Page
// ---------------------------------------------------------------------------

void ClientUI::render_servers_page() {
    float scale = settings().ui_scale;
    auto& server_list = servers();
    static int server_tab = 0;
    static int selected_server = 0;
    static char cmd_buf[256] = {};

    ImGui::TextColored(theme().accent, "SERVERS");
    ImGui::Spacing();

    // Tab bar: Server Info | Players | Cloud Sync | Backups | Settings | Quick Actions
    const char* tabs[] = {"Server Info", "Players", "Cloud Sync", "Backups", "Settings", "Quick Actions"};
    for (int i = 0; i < 6; ++i) {
        bool active = (server_tab == i);
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, theme().accent);
        else ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        if (ImGui::Button(tabs[i], ImVec2(0, 28 * scale))) server_tab = i;
        ImGui::PopStyleColor();
        if (i < 5) ImGui::SameLine(4);
    }
    ImGui::Spacing();
    client_separator();
    ImGui::Spacing();

    if (server_list.empty()) {
        float cx = ImGui::GetContentRegionAvail().x * 0.5f;
        ImGui::SetCursorPosX(cx - 30);
        ImGui::PushStyleColor(ImGuiCol_Button, theme().accent);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 20.0f);
        ImGui::Button("S", ImVec2(60, 60));
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::SetCursorPosX(cx - ImGui::CalcTextSize("No servers configured").x * 0.5f);
        ImGui::TextColored(theme().muted, "No servers configured");
        ImGui::SetCursorPosX(cx - ImGui::CalcTextSize("Add servers in the launcher to see them here.").x * 0.5f);
        ImGui::TextDisabled("Add servers in the launcher to see them here.");
        return;
    }

    // Clamp selection
    if (selected_server < 0) selected_server = 0;
    if (selected_server >= static_cast<int>(server_list.size())) selected_server = 0;
    const auto& sel = server_list[selected_server];

    // Content area: server list on left, detail on right
    float list_w = ImGui::GetContentRegionAvail().x * 0.35f;
    float detail_w = ImGui::GetContentRegionAvail().x - list_w - 8;

    // Server list
    ImGui::BeginChild("##srv_list", ImVec2(list_w, 0), true);
    {
        for (size_t i = 0; i < server_list.size(); ++i) {
            const auto& s = server_list[i];
            ImGui::PushID(static_cast<int>(i));

            ImVec2 row_pos = ImGui::GetCursorScreenPos();
            ImVec2 row_size(ImGui::GetContentRegionAvail().x, 52 * scale);
            bool is_sel = (static_cast<int>(i) == selected_server);

            if (is_sel) {
                ImGui::GetWindowDrawList()->AddRectFilled(
                    row_pos, row_pos + row_size,
                    ImGui::GetColorU32(ImVec4(theme().accent.x, theme().accent.y, theme().accent.z, 0.15f)), 4.0f);
            }

            ImGui::InvisibleButton(("##svr" + std::to_string(i)).c_str(), row_size);
            if (ImGui::IsItemClicked()) selected_server = static_cast<int>(i);

            // Status dot
            const bool status_checked = s.status_checked || s.online || s.ping_ms >= 0;
            ImVec4 dot_col = s.online ? theme().success : status_checked ? theme().error : theme().warning;
            ImGui::GetWindowDrawList()->AddCircleFilled(
                ImVec2(row_pos.x + 14, row_pos.y + 14), 5, ImGui::GetColorU32(dot_col));

            // Name + address
            ImGui::SetCursorScreenPos(ImVec2(row_pos.x + 26, row_pos.y + 6));
            ImGui::Text("%s", s.name.c_str());
            ImGui::SetCursorScreenPos(ImVec2(row_pos.x + 26, row_pos.y + 24));
            ImGui::TextDisabled("%s", s.address.c_str());

            // Ping badge
            if (s.online && s.ping_ms >= 0) {
                ImVec4 pc = s.ping_ms < 50 ? theme().success : s.ping_ms < 100 ? theme().warning : theme().error;
                ImGui::SetCursorScreenPos(ImVec2(row_pos.x + row_size.x - 50, row_pos.y + 14));
                ImGui::TextColored(pc, "%dms", s.ping_ms);
            }

            ImGui::SetCursorScreenPos(ImVec2(row_pos.x, row_pos.y + row_size.y));
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    ImGui::SameLine(8);

    // Detail panel
    ImGui::BeginChild("##srv_detail", ImVec2(detail_w, 0), true);
    {
        // Server name + status header
        ImGui::TextColored(theme().accent, "%s", sel.name.c_str());
        ImGui::SameLine();
        const bool selected_status_checked = sel.status_checked || sel.online || sel.ping_ms >= 0;
        ImVec4 sc = sel.online ? theme().success : selected_status_checked ? theme().error : theme().warning;
        ImGui::TextColored(sc, sel.online ? "Online" : selected_status_checked ? "Offline" : "Not checked");
        ImGui::TextDisabled("%s", server_endpoint(sel).c_str());
        ImGui::Spacing();
        client_separator();
        ImGui::Spacing();

        if (server_tab == 0) { // Server Info
            auto row = [](const char* label, const char* val) {
                ImGui::TextColored(ImVec4(0.55f, 0.55f, 0.60f, 1), "%s", label);
                ImGui::SameLine(120);
                ImGui::Text("%s", val);
            };
            row("Address", server_endpoint(sel).c_str());
            row("Players", sel.online ? (std::to_string(sel.ping_ms >= 0 ? 1 : 0) + "/20").c_str() : "N/A");
            row("Ping", sel.ping_ms >= 0 ? (std::to_string(sel.ping_ms) + "ms").c_str() : "N/A");
            row("TPS", perf().tps >= 0 ? (std::to_string(perf().tps)).c_str() : "N/A");
            row("Version", profile().mc_version.empty() ? "Unknown" : profile().mc_version.c_str());
            // Uptime display
            if (sel.online && profile().session_start > 0) {
                auto now = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                auto elapsed = now - profile().session_start;
                int hours = static_cast<int>(elapsed / 3600);
                int mins = static_cast<int>((elapsed % 3600) / 60);
                row("Uptime", (std::to_string(hours) + "h " + std::to_string(mins) + "m").c_str());
            }

            ImGui::Spacing();
            client_separator();
            ImGui::Spacing();

            // Recent players
            ImGui::TextColored(theme().accent, "Recent Players");
            ImGui::Spacing();
            auto entities = aml::tracker::store().entities();
            int shown = 0;
            for (const auto& e : entities) {
                if (e.kind != aml::tracker::ENTITY_PLAYER) continue;
                if (shown >= 6) break;
                ImGui::TextColored(theme().accent, "\xe2\x97\x8f");
                ImGui::SameLine(16);
                ImGui::Text("%s", e.name.empty() ? "Unknown" : e.name.c_str());
                shown++;
            }
            if (shown == 0) ImGui::TextDisabled("No players tracked");

            ImGui::Spacing();
            client_separator();
            ImGui::Spacing();

            // Copy IP button
            if (client_secondary_button("Copy IP", ImVec2(-1, 28))) {
                const std::string endpoint = server_endpoint(sel);
                const bool ok = copy_to_clipboard(endpoint);
                NotificationManager::instance().push_toast(
                    ok ? "Copied" : "Copy failed", endpoint,
                    ok ? NotifyLevel::Success : NotifyLevel::Error);
            }

        } else if (server_tab == 1) { // Players
            ImGui::TextColored(theme().accent, "Online Players");
            ImGui::Spacing();
            auto entities = aml::tracker::store().entities();
            int player_count = 0;
            for (const auto& e : entities) {
                if (e.kind != aml::tracker::ENTITY_PLAYER) continue;
                ImGui::PushID(e.name.c_str());
                ImVec2 rp = ImGui::GetCursorScreenPos();
                ImVec2 rs(ImGui::GetContentRegionAvail().x, 32 * scale);
                ImGui::InvisibleButton("##plr", rs);
                // Avatar circle with first letter
                float plr_avatar_r = 10.0f * scale;
                ImVec2 plr_ac(rp.x + 14, rp.y + rs.y * 0.5f);
                ImGui::GetWindowDrawList()->AddCircleFilled(
                    plr_ac, plr_avatar_r, ImGui::GetColorU32(theme().accent));
                char plr_lbuf[2] = {e.name.empty() ? '?' : e.name[0], '\0'};
                ImVec2 plr_lts = ImGui::CalcTextSize(plr_lbuf);
                ImGui::GetWindowDrawList()->AddText(
                    ImVec2(plr_ac.x - plr_lts.x * 0.5f, plr_ac.y - plr_lts.y * 0.5f),
                    ImGui::GetColorU32(ImVec4(1, 1, 1, 1)), plr_lbuf);
                ImGui::SetCursorScreenPos(ImVec2(rp.x + 30, rp.y + 4));
                ImGui::Text("%s", e.name.empty() ? "Unknown" : e.name.c_str());
                if (e.health >= 0) {
                    float hp_pct = e.health / 20.0f;
                    ImVec4 hc = hp_pct > 0.5f ? theme().success : hp_pct > 0.25f ? theme().warning : theme().error;
                    ImGui::SameLine(200);
                    ImGui::TextColored(hc, "HP: %.0f", e.health);
                }
                ImGui::SetCursorScreenPos(ImVec2(rp.x, rp.y + rs.y));
                ImGui::PopID();
                player_count++;
            }
            if (player_count == 0) ImGui::TextDisabled("No players tracked");

        } else if (server_tab == 2) { // Cloud Sync
            ImGui::TextColored(theme().accent, "Cloud Sync");
            ImGui::Spacing();
            ImGui::TextDisabled("Amalgam Cloud is not available yet.");
            ImGui::TextDisabled("Server sync will appear here once Cloud launches.");

        } else if (server_tab == 3) { // Backups
            ImGui::TextColored(theme().accent, "Backups");
            ImGui::Spacing();
            client_card_begin("BACKUPS");
            ImGui::TextDisabled("No backups yet.");
            ImGui::TextDisabled("World backups are managed from the launcher.");
            client_card_end();

        } else if (server_tab == 4) { // Settings
            ImGui::TextColored(theme().accent, "Server Settings");
            ImGui::Spacing();
            client_card_begin("SETTINGS");
            ImGui::Text("Server Name");
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##sname", const_cast<char*>(sel.name.c_str()), sel.name.size(), ImGuiInputTextFlags_ReadOnly);
            ImGui::Text("Address");
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##saddr", const_cast<char*>(sel.address.c_str()), sel.address.size(), ImGuiInputTextFlags_ReadOnly);
            ImGui::Text("Port");
            ImGui::SameLine(120);
            ImGui::Text("%s", sel.port > 0 ? std::to_string(sel.port).c_str() : "-");
            client_card_end();

        } else { // Quick Actions (tab 5)
            ImGui::TextColored(theme().accent, "Quick Actions");
            ImGui::Spacing();

            float btn_w = -1;
            if (client_primary_button("Open Launcher Servers", ImVec2(btn_w, 32))) {
                if (!open_launcher_page("servers")) {
                    NotificationManager::instance().push_toast(
                        "Launcher not found", "Open Amalgam Launcher to manage servers.", NotifyLevel::Warning);
                }
            }
            ImGui::Spacing();
            if (client_secondary_button("Copy IP", ImVec2(btn_w, 32))) {
                const std::string ep = server_endpoint(sel);
                copy_to_clipboard(ep);
                NotificationManager::instance().push_toast("Copied", ep, NotifyLevel::Success);
            }
            ImGui::Spacing();
            if (client_secondary_button("Refresh Server List", ImVec2(btn_w, 32))) {
                refresh_client_data();
                refresh_server_status();
            }
        }
    }
    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// Screenshots Page
// ---------------------------------------------------------------------------

void ClientUI::render_screenshots_page() {
    client_text("Screenshots");
    ImGui::Separator();

    ImGui::BeginChild("##screenshots", ImVec2(0, 0), true);

    const std::string mc_dir = game_dir();
    std::string ss_dir = mc_dir + "\\screenshots";

    std::error_code ec;
    if (!std::filesystem::exists(ss_dir, ec)) {
        ImGui::TextDisabled("No screenshots directory found");
        ImGui::TextDisabled("Take a screenshot in-game (F2) and it will appear here");
    } else {
        int count = 0;
        for (const auto& entry : std::filesystem::directory_iterator(ss_dir, ec)) {
            if (!entry.is_regular_file()) continue;
            auto ext = entry.path().extension().string();
            lowercase_ascii(ext);
            if (ext != ".png" && ext != ".jpg") continue;

            ImGui::Text("%s", entry.path().filename().string().c_str());
            ImGui::SameLine(300);
            auto ftime = entry.last_write_time(ec);
            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
            auto tt = std::chrono::system_clock::to_time_t(sctp);
            char time_buf[32];
            std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M", std::localtime(&tt));
            ImGui::TextDisabled("%s", time_buf);
            count++;
        }
        if (count == 0) {
            ImGui::TextDisabled("No screenshots yet");
            ImGui::TextDisabled("Take a screenshot in-game (F2)");
        }
    }

    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// Modules Page (with categories)
// ---------------------------------------------------------------------------

void ClientUI::render_modules_page() {
    float scale = settings().ui_scale;
    static int selected_category = 0;
    static int selected_module = -1;
    static char search_buf[128] = {};
    static bool favorites_only = false;

    const char* category_names[] = {"All", "Combat", "Movement", "Player", "World", "Render", "Misc", "Favorites"};
    const int category_count = 8;

    // Header with search. The atlas icon is a real sliced transparent asset,
    // with the text fallback kept for packages that omit optional artwork.
    const ImVec2 module_icon_pos = ImGui::GetCursorScreenPos();
    if (draw_client_atlas_icon(13, module_icon_pos, 30.0f)) {
        ImGui::Dummy(ImVec2(36.0f, 30.0f));
        ImGui::SameLine(0, 4.0f);
    }
    ImGui::TextColored(theme().accent, "MODULES");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 200);
    ImGui::SetNextItemWidth(200);
    ImGui::InputTextWithHint("##search", "Search modules...", search_buf, sizeof(search_buf));
    ImGui::Spacing();

    // Category tabs
    for (int i = 0; i < category_count; ++i) {
        bool active = (selected_category == i);
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button, theme().accent);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        }
        if (ImGui::Button(category_names[i], ImVec2(0, 28 * scale))) {
            selected_category = i;
        }
        ImGui::PopStyleColor();
        if (i < category_count - 1) ImGui::SameLine(4);
    }

    // Sort tabs matching reference (All / Popular / New / A-Z)
    static int sort_mode = 0;
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 240);
    const char* sort_names[] = {"All", "Popular", "New", "A-Z"};
    for (int i = 0; i < 4; ++i) {
        bool active = (sort_mode == i);
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, theme().accent);
        else ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        if (ImGui::Button(sort_names[i], ImVec2(0, 24 * scale))) sort_mode = i;
        ImGui::PopStyleColor();
        if (i < 3) ImGui::SameLine(2);
    }

    ImGui::Spacing();
    client_separator();
    ImGui::Spacing();

    // Module list + config panel
    float list_width = ImGui::GetContentRegionAvail().x * 0.65f;
    float config_width = ImGui::GetContentRegionAvail().x - list_width - 8;

    // Module list
    ImGui::BeginChild("##module_list", ImVec2(list_width, 0), true);
    {
        auto& mod_list = mods();
        if (mod_list.empty()) {
            ImGui::Spacing();
            ImGui::Spacing();
            float center_x = ImGui::GetContentRegionAvail().x * 0.5f;
            ImGui::SetCursorPosX(center_x - 40);
            ImGui::PushStyleColor(ImGuiCol_Button, theme().accent);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 20.0f);
            ImGui::Button("M", ImVec2(60, 60));
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
            ImGui::Spacing();
            ImGui::SetCursorPosX(center_x - ImGui::CalcTextSize("No modules loaded").x * 0.5f);
            ImGui::TextColored(theme().muted, "No modules loaded");
            ImGui::SetCursorPosX(center_x - ImGui::CalcTextSize("Install mods through the launcher first.").x * 0.5f);
            ImGui::TextDisabled("Install mods through the launcher first.");
        } else {
            int idx = 0;
            for (size_t i = 0; i < mod_list.size(); ++i) {
                const auto& m = mod_list[i];
                bool matches_search = search_buf[0] == '\0' || 
                    m.name.find(search_buf) != std::string::npos;
                bool matches_category = selected_category == 0 ||
                    (selected_category == 7 && is_module_favorited(m.name)) ||
                    selected_category != 7;
                
                if (!matches_search || !matches_category) continue;
                
                ImGui::PushID(m.filename.c_str());
                bool is_selected = (selected_module == idx);
                
                // Module row
                ImVec2 row_pos = ImGui::GetCursorScreenPos();
                ImVec2 row_size(ImGui::GetContentRegionAvail().x, 36 * scale);
                
                if (is_selected) {
                    ImGui::GetWindowDrawList()->AddRectFilled(
                        row_pos, row_pos + row_size,
                        ImGui::GetColorU32(ImVec4(theme().accent.x, theme().accent.y, theme().accent.z, 0.15f)),
                        4.0f);
                }
                
                ImGui::InvisibleButton(("##mod" + std::to_string(i)).c_str(), row_size);
                if (ImGui::IsItemClicked()) {
                    selected_module = idx;
                }
                
                // Draw module info
                ImGui::SetCursorScreenPos(ImVec2(row_pos.x + 8, row_pos.y + 4));
                
                // Favorite star
                bool fav = is_module_favorited(m.name);
                ImGui::TextColored(fav ? theme().warning : theme().muted, fav ? "\xe2\x98\x85" : "\xe2\x98\x86");
                if (ImGui::IsItemClicked()) {
                    toggle_module_favorite(m.name);
                }
                ImGui::SameLine(24);
                
                // Module name
                ImGui::Text("%s", m.name.c_str());
                ImGui::SameLine(list_width * 0.4f);
                ImGui::TextDisabled("%s", m.version.c_str());
                
                // Enable toggle on right
                ImGui::SameLine(list_width - 60);
                bool en = m.enabled;
                ImGui::PushStyleColor(ImGuiCol_Button, en ? theme().success : theme().panel);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, en ? ImVec4(theme().success.x * 1.2f, theme().success.y * 1.1f, theme().success.z * 1.2f, 1.0f) : theme().accent_hover);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12.0f);
                if (ImGui::Button(en ? "ON" : "OFF", ImVec2(48, 20))) {
                    set_mod_enabled(m.filename, !en);
                }
                ImGui::PopStyleVar();
                ImGui::PopStyleColor(2);
                
                ImGui::SetCursorScreenPos(ImVec2(row_pos.x, row_pos.y + row_size.y));
                idx++;
            }
        }
    }
    ImGui::EndChild();

    ImGui::SameLine(8);

    // Module config panel
    ImGui::BeginChild("##module_config", ImVec2(config_width, 0), true);
    {
        auto& mod_list = mods();
        if (selected_module >= 0 && selected_module < static_cast<int>(mod_list.size())) {
            const auto& m = mod_list[selected_module];
            ImGui::TextColored(theme().accent, "MODULE CONFIG");
            ImGui::Spacing();
            // Module description lookup
            static const char* module_descriptions[][2] = {
                {"Fly", "Allows creative-style flight in survival."},
                {"Speed", "Increases movement speed."},
                {"Freecam", "Detached camera for spectating."},
                {"NoFall", "Prevents fall damage."},
                {"AutoTool", "Automatically switches to the best tool."},
                {"KillAura", "Automatically attacks entities."},
            };
            for (int d = 0; d < 6; ++d) {
                std::string lower = m.name;
                lowercase_ascii(lower);
                if (lower.find(module_descriptions[d][0]) != std::string::npos) {
                    ImGui::TextDisabled("%s", module_descriptions[d][1]);
                    ImGui::Spacing();
                    break;
                }
            }
            ImGui::Text("%s", m.name.c_str());
            ImGui::TextDisabled("v%s", m.version.c_str());
            ImGui::Spacing();
            client_separator();
            ImGui::Spacing();
            
            // Enable/disable toggle
            bool en = m.enabled;
            if (ImGui::Checkbox("Enabled", &en)) {
                set_mod_enabled(m.filename, en);
            }
            ImGui::Spacing();
            client_separator();
            ImGui::Spacing();
            
            // Module-specific config from the native module system
            // Match file name to module ID
            aml::ModuleId mod_id = aml::MOD_FREECAM;
            std::string lower_name = m.name;
            lowercase_ascii(lower_name);
            if (lower_name.find("fly") != std::string::npos) mod_id = aml::MOD_FLY;
            else if (lower_name.find("speed") != std::string::npos) mod_id = aml::MOD_SPEED;
            else if (lower_name.find("nofall") != std::string::npos) mod_id = aml::MOD_NOFALL;
            else if (lower_name.find("autotool") != std::string::npos) mod_id = aml::MOD_AUTOTOOL;
            else if (lower_name.find("killaura") != std::string::npos) mod_id = aml::MOD_KILLAURA;
            
            aml::Module* mod = aml::module_get(mod_id);
            if (mod) {
                // Show module parameters
                ImGui::Text("Parameters:");
                ImGui::Spacing();
                
                const char* param_names[][6] = {
                    {"Horizontal Speed", "Vertical Speed", "Freecam Speed", "", "", ""},
                    {"Fly Speed", "", "", "", "", ""},
                    {"Speed Multiplier", "", "", "", "", ""},
                    {"", "", "", "", "", ""},
                    {"", "", "", "", "", ""},
                    {"", "", "", "", "", ""}
                };
                
                for (int p = 0; p < 6; ++p) {
                    if (mod->params[p] == 0.0f && std::string(param_names[static_cast<int>(mod_id)][p]).empty())
                        continue;
                    
                    ImGui::Text("%s", param_names[static_cast<int>(mod_id)][p]);
                    ImGui::SameLine(config_width - 80);
                    ImGui::PushItemWidth(70);
                    float val = mod->params[p];
                    if (ImGui::DragFloat(("##p" + std::to_string(p)).c_str(), &val, 0.1f, 0.0f, 10.0f)) {
                        aml::module_set_param(mod_id, p, val);
                    }
                    ImGui::PopItemWidth();
                }
                
                ImGui::Spacing();
                client_separator();
                ImGui::Spacing();
                
                // Safe mode toggle
                bool safe = aml::modules_safe_mode();
                if (ImGui::Checkbox("Server-Safe Mode", &safe)) {
                    aml::modules_set_safe_mode(safe);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Disables all modules on potentially detected servers.");
                }
                
                ImGui::Spacing();
                client_separator();
                ImGui::Spacing();
                
                // Reference-specific: Mode/Horizontal/Vertical
                static int config_mode = 0;
                static float config_horizontal = 70.0f;
                static float config_vertical = 60.0f;
                static bool show_particles = true;
                static bool only_in_combat = false;
                static bool cancel_knockback = true;
                
                ImGui::Text("Mode");
                ImGui::SameLine(120);
                ImGui::SetNextItemWidth(100);
                const char* modes[] = {"Packet", "Vanilla"};
                ImGui::Combo("##mode", &config_mode, modes, 2);
                
                ImGui::Text("Horizontal");
                ImGui::SameLine(120);
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 120);
                ImGui::SliderFloat("##horiz", &config_horizontal, 0.0f, 100.0f, "%.0f%%");
                
                ImGui::Text("Vertical");
                ImGui::SameLine(120);
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 120);
                ImGui::SliderFloat("##vert", &config_vertical, 0.0f, 100.0f, "%.0f%%");
                
                ImGui::Spacing();
                ImGui::Text("Cancel Knockback");
                ImGui::SameLine(180);
                ImGui::Checkbox("##cknockback", &cancel_knockback);
                
                ImGui::Text("Only in Combat");
                ImGui::SameLine(180);
                ImGui::Checkbox("##combat", &only_in_combat);
                
                ImGui::Text("Show Particles");
                ImGui::SameLine(180);
                ImGui::Checkbox("##particles", &show_particles);
                
                ImGui::Spacing();
                client_separator();
                ImGui::Spacing();
                
                if (client_secondary_button("Reset Defaults")) {
                    aml::modules_reset_defaults();
                    config_mode = 0;
                    config_horizontal = 70.0f;
                    config_vertical = 60.0f;
                    show_particles = true;
                    only_in_combat = false;
                    cancel_knockback = true;
                }
                
                ImGui::Spacing();
                if (client_primary_button("Save Module")) {
                    NotificationManager::instance().push_toast(
                        "Module Saved", std::string(m.name) + " configuration saved.",
                        NotifyLevel::Success);
                }
            } else {
                ImGui::TextDisabled("Module not available in native client.");
                ImGui::TextDisabled("This mod may be managed by the launcher.");
            }
        } else {
            ImGui::TextColored(theme().accent, "SELECT A MODULE");
            ImGui::Spacing();
            ImGui::TextDisabled("Click a module to view its configuration.");
            ImGui::Spacing();
            ImGui::TextDisabled("Configure parameters, enable/disable,");
            ImGui::TextDisabled("and set server-safe mode.");
        }
    }
    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// Profiles Page
// ---------------------------------------------------------------------------

void ClientUI::render_profiles_page() {
    ImGui::TextColored(theme().accent, "PROFILES");
    ImGui::Spacing();
    ImGui::TextDisabled("Manage your Minecraft profiles and launcher settings.");
    ImGui::Spacing();
    client_separator();
    ImGui::Spacing();

    // Current profile
    client_card_begin("CURRENT PROFILE");
    {
        ImGui::Text("%s", profile().profile_name.empty() ? "No profile selected" : profile().profile_name.c_str());
        ImGui::TextDisabled("Minecraft: %s", profile().mc_version.empty() ? "Unknown" : profile().mc_version.c_str());
        ImGui::TextDisabled("Loader: %s", profile().loader.empty() ? "Unknown" : profile().loader.c_str());
        ImGui::TextDisabled("Mods: %d", profile().mod_count);
        
        ImGui::Spacing();
        float btn_w = (ImGui::GetContentRegionAvail().x - 8) / 2.0f;
        if (client_secondary_button("Open Launcher Library", ImVec2(btn_w, 28))) {
            if (!open_launcher_page("library")) {
                NotificationManager::instance().push_toast(
                    "Launcher not found", "Open Amalgam Launcher to switch profiles.", NotifyLevel::Warning);
            }
        }
        ImGui::SameLine(0, 8);
        if (client_secondary_button("Sync with Launcher", ImVec2(btn_w, 28))) {
            refresh_client_data();
        }
    }
    client_card_end();

    ImGui::Spacing();

    // Sync status
    client_card_begin("SYNC STATUS");
    {
        auto& status = sync_status();
        ImGui::TextColored(status.synced ? theme().success : theme().warning, 
            status.synced ? "All profiles up to date" : "Sync required");
        ImGui::Spacing();
        
        auto sync_row = [](const char* label, bool synced) {
            ImGui::TextColored(synced ? theme().success : theme().muted, synced ? "\xe2\x9c\x93" : "\xe2\x9c\x97");
            ImGui::SameLine(20);
            ImGui::Text("%s", label);
        };
        
        sync_row("Modules & Settings", status.modules_synced);
        sync_row("HUD Layout", status.hud_synced);
        sync_row("Keybinds", status.keybinds_synced);
        sync_row("Cosmetics", status.cosmetics_synced);
        sync_row("Client Settings", status.settings_synced);
        sync_row("Server List", status.server_list_synced);
        sync_row("Friends (Essentials)", status.friends_synced);
        
        ImGui::Spacing();
        ImGui::TextDisabled("Last sync: %s", status.last_sync_time.empty() ? "Never" : status.last_sync_time.c_str());
        if (!status.sync_error.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(theme().warning, "%s", status.sync_error.c_str());
        }
        
        ImGui::Spacing();
        if (client_primary_button("Sync Now", ImVec2(0, 28))) {
            refresh_client_data();
        }
    }
    client_card_end();
}

// ---------------------------------------------------------------------------
// Cosmetics Page
// ---------------------------------------------------------------------------

void ClientUI::render_cosmetics_page() {
    float scale = settings().ui_scale;
    static int selected_tab = 0; // 0=Capes, 1=Badges, 2=Nameplates, 3=Emotes, 4=Pets
    static int filter = 0; // 0=Owned, 1=All
    static int selected_idx = -1;
    auto& state = cosmetics::cosmetics_state();

    const char* tab_names[] = {"Capes", "Badges", "Nameplates", "Emotes", "Pets"};
    const char* categories[] = {"cape", "badge", "nameplate", "emote", "pet"};
    const char* rarity_names[] = {"Common", "Rare", "Epic", "Legendary"};
    const int tab_count = 5;

    ImVec4 rarity_colors[] = {
        theme().muted,
        ImVec4(0.3f, 0.6f, 1.0f, 1.0f),
        ImVec4(0.7f, 0.3f, 1.0f, 1.0f),
        ImVec4(1.0f, 0.8f, 0.0f, 1.0f)
    };

    // Cosmetics come from the launcher's shared_cosmetics.txt bridge; ownership
    // is never hardcoded here. Filter by the selected category + ownership.
    std::vector<const cosmetics::CosmeticItem*> items;
    int owned_count = 0;
    for (const auto& item : state.items) {
        if (item.category == categories[selected_tab]) {
            if (item.owned) ++owned_count;
            if (filter == 0 && !item.owned) continue;
            items.push_back(&item);
        }
    }

    // Header
    ImGui::TextColored(theme().accent, "COSMETICS");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 160);
    ImGui::TextDisabled("%d Owned", owned_count);
    ImGui::Spacing();

    // Category tabs
    for (int i = 0; i < tab_count; ++i) {
        bool active = (selected_tab == i);
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button, theme().accent);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        }
        if (ImGui::Button(tab_names[i], ImVec2(0, 28 * scale))) {
            selected_tab = i;
            selected_idx = -1;
        }
        ImGui::PopStyleColor();
        if (i < tab_count - 1) ImGui::SameLine(4);
    }

    // Owned / All filter
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 140);
    if (draw_tab_button("Owned", filter == 0, ImVec2(60 * scale, 24 * scale))) { filter = 0; selected_idx = -1; }
    ImGui::SameLine(0, 4);
    if (draw_tab_button("All", filter == 1, ImVec2(60 * scale, 24 * scale))) { filter = 1; selected_idx = -1; }

    ImGui::Spacing();
    client_separator();
    ImGui::Spacing();

    // Items grid + details
    float grid_width = ImGui::GetContentRegionAvail().x * 0.65f;
    float detail_width = ImGui::GetContentRegionAvail().x - grid_width - 8;

    // Items grid
    ImGui::BeginChild("##cosmetics_grid", ImVec2(grid_width, 0), true);
    {
        ImGui::TextColored(theme().accent, "%s", tab_names[selected_tab]);
        ImGui::Spacing();

        for (size_t i = 0; i < items.size(); ++i) {
            const auto* item = items[i];
            ImGui::PushID(static_cast<int>(i));

            ImVec2 card_pos = ImGui::GetCursorScreenPos();
            ImVec2 card_size(ImGui::GetContentRegionAvail().x, 56 * scale);
            bool is_selected = (selected_idx == static_cast<int>(i));

            if (is_selected) {
                ImGui::GetWindowDrawList()->AddRectFilled(
                    card_pos, card_pos + card_size,
                    ImGui::GetColorU32(ImVec4(theme().accent.x, theme().accent.y, theme().accent.z, 0.15f)),
                    4.0f);
            }

            ImGui::InvisibleButton(("##cosm" + std::to_string(i)).c_str(), card_size);
            if (ImGui::IsItemClicked()) selected_idx = static_cast<int>(i);

            // Name
            ImGui::SetCursorScreenPos(ImVec2(card_pos.x + 12, card_pos.y + 8));
            ImGui::Text("%s", item->name.c_str());

            // Rarity badge
            int rl = std::clamp(item->rarity, 0, 3);
            ImVec4 rc = rarity_colors[rl];
            ImGui::SetCursorScreenPos(ImVec2(card_pos.x + card_size.x - 80, card_pos.y + 8));
            ImGui::TextColored(rc, "%s", rarity_names[rl]);

            // Subtitle
            ImGui::SetCursorScreenPos(ImVec2(card_pos.x + 12, card_pos.y + 26));
            ImGui::TextDisabled("%s", item->description.c_str());

            // Equipped badge
            if (item->equipped) {
                ImGui::SetCursorScreenPos(ImVec2(card_pos.x + 12, card_pos.y + 40));
                ImGui::TextColored(theme().success, "Equipped");
            } else if (!item->owned) {
                ImGui::SetCursorScreenPos(ImVec2(card_pos.x + 12, card_pos.y + 40));
                ImGui::TextColored(theme().muted, "Not owned");
            }

            ImGui::SetCursorScreenPos(ImVec2(card_pos.x, card_pos.y + card_size.y));
            ImGui::PopID();
        }

        if (items.empty()) {
            ImGui::Spacing();
            float cx = ImGui::GetContentRegionAvail().x * 0.5f;
            ImGui::SetCursorPosX(cx - 30);
            ImGui::PushStyleColor(ImGuiCol_Button, theme().accent);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 20.0f);
            ImGui::Button("C", ImVec2(60, 60));
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
            ImGui::Spacing();
            if (filter == 0) {
                ImGui::SetCursorPosX(cx - ImGui::CalcTextSize("No items owned").x * 0.5f);
                ImGui::TextColored(theme().muted, "No items owned");
                ImGui::Spacing();
                ImGui::SetCursorPosX(cx - ImGui::CalcTextSize("Amalgam+ unlocks premium cosmetics").x * 0.5f);
                ImGui::TextDisabled("Amalgam+ unlocks premium cosmetics");
            } else {
                ImGui::SetCursorPosX(cx - ImGui::CalcTextSize("No items available").x * 0.5f);
                ImGui::TextColored(theme().muted, "No items available");
            }
        }
    }
    ImGui::EndChild();

    ImGui::SameLine(8);

    // Details panel
    ImGui::BeginChild("##cosmetics_detail", ImVec2(detail_width, 0), true);
    {
        if (selected_idx >= 0 && selected_idx < static_cast<int>(items.size())) {
            const auto* item = items[selected_idx];

            // Preview area
            ImVec2 preview_pos = ImGui::GetCursorScreenPos();
            ImVec2 preview_size(ImGui::GetContentRegionAvail().x, 80 * scale);
            ImGui::GetWindowDrawList()->AddRectFilled(
                preview_pos, preview_pos + preview_size,
                ImGui::GetColorU32(theme().panel), 8.0f);
            ImVec2 center = preview_pos + preview_size * 0.5f;
            ImVec2 ts = ImGui::CalcTextSize(item->name.c_str());
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(center.x - ts.x * 0.5f, center.y - ts.y * 0.5f),
                ImGui::GetColorU32(ImVec4(0.5f, 0.5f, 0.55f, 0.6f)), item->name.c_str());
            ImGui::Dummy(preview_size);

            ImGui::Spacing();
            ImGui::TextColored(theme().accent, "ITEM DETAILS");
            ImGui::Spacing();
            ImGui::Text("%s", item->name.c_str());

            // Rarity
            int rl = std::clamp(item->rarity, 0, 3);
            ImVec4 rc = rarity_colors[rl];
            ImGui::Text("Rarity: ");
            ImGui::SameLine();
            ImGui::TextColored(rc, "%s", rarity_names[rl]);

            // Status
            ImGui::Text("Status: ");
            ImGui::SameLine();
            if (item->equipped) ImGui::TextColored(theme().success, "Equipped");
            else if (item->owned) ImGui::TextColored(theme().text, "Owned");
            else ImGui::TextColored(theme().muted, "Not owned");

            ImGui::TextDisabled("%s", item->description.c_str());
            ImGui::Spacing();
            client_separator();
            ImGui::Spacing();

            // Actions
            if (item->owned) {
                if (!item->equipped) {
                    if (client_primary_button("Equip", ImVec2(-1, 30))) {
                        cosmetics::equip_cosmetic(item->id);
                        NotificationManager::instance().push_toast(
                            "Cosmetic selected", item->name, NotifyLevel::Success);
                    }
                } else {
                    if (client_secondary_button("Unequip", ImVec2(-1, 30))) {
                        cosmetics::unequip_cosmetic(item->id);
                        NotificationManager::instance().push_toast(
                            "Cosmetic cleared", item->name, NotifyLevel::Info);
                    }
                }
            } else {
                ImGui::TextDisabled("You don't own this item yet.");
                ImGui::TextDisabled("Amalgam+ members unlock premium cosmetics.");
            }
        } else {
            ImGui::TextColored(theme().accent, "SELECT AN ITEM");
            ImGui::Spacing();
            ImGui::TextDisabled("Click an item to view details,");
            ImGui::TextDisabled("preview and manage cosmetics.");
        }
    }
    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// Diagnostics Page
// ---------------------------------------------------------------------------

void ClientUI::render_diagnostics_page() {
    float scale = settings().ui_scale;
    static int diag_tab = 0; // 0=Summary, 1=Logs

    const ImVec2 art_pos = ImGui::GetCursorScreenPos();
    const ImVec2 art_size(ImGui::GetContentRegionAvail().x, 78.0f * scale);
    if (draw_client_art("client-diagnostics-ai.png", art_pos, art_size, true)) {
        ImGui::GetWindowDrawList()->AddRectFilled(
            art_pos, art_pos + art_size,
            ImGui::GetColorU32(ImVec4(0.02f, 0.01f, 0.06f, 0.52f)), 8.0f);
        ImGui::SetCursorScreenPos(art_pos + ImVec2(14.0f, 12.0f));
        ImGui::TextColored(theme().text, "REPAIR & RECOVER");
        ImGui::SetCursorScreenPos(art_pos + ImVec2(14.0f, 38.0f));
        ImGui::TextColored(theme().muted, "Understand crashes, inspect logs, and get back into your world.");
        ImGui::SetCursorScreenPos(ImVec2(art_pos.x, art_pos.y + art_size.y + 10.0f));
    }

    // Header
    ImGui::TextColored(theme().accent, "DIAGNOSTICS");
    ImGui::Spacing();

    // Tab bar
    if (draw_tab_button("Summary", diag_tab == 0, ImVec2(100 * scale, 0))) diag_tab = 0;
    ImGui::SameLine(4);
    if (draw_tab_button("Logs", diag_tab == 1, ImVec2(100 * scale, 0))) diag_tab = 1;
    ImGui::Spacing();
    client_separator();
    ImGui::Spacing();

    switch (diag_tab) {
    case 0: // Summary
        {
            // Check for crash logs
            const std::string mc_dir = game_dir();
            std::string crash_dir = mc_dir + "\\crash-reports";

            client_card_begin("CRASH REPORT");
            {
                std::error_code ec;
                bool has_crash = false;
                std::string latest_crash;
                uint64_t latest_time = 0;

                if (std::filesystem::exists(crash_dir, ec)) {
                    for (const auto& entry : std::filesystem::directory_iterator(crash_dir, ec)) {
                        if (!entry.is_regular_file()) continue;
                        auto ext = entry.path().extension().string();
                        if (ext.find("crash") == std::string::npos &&
                            ext != ".txt" && ext != ".gz") continue;
                        auto ftime = entry.last_write_time(ec);
                        auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                            ftime - std::filesystem::file_time_type::clock::now() +
                            std::chrono::system_clock::now());
                        auto tt = std::chrono::system_clock::to_time_t(sctp);
                        uint64_t ts = static_cast<uint64_t>(tt);
                        if (ts > latest_time) {
                            latest_time = ts;
                            latest_crash = entry.path().string();
                            has_crash = true;
                        }
                    }
                }

                if (!has_crash) {
                    // Structured "no crash" state matching reference
                    ImVec2 icon_p = ImGui::GetCursorScreenPos();
                    ImGui::GetWindowDrawList()->AddCircleFilled(
                        ImVec2(icon_p.x + 20, icon_p.y + 20), 20,
                        ImGui::GetColorU32(ImVec4(0.2f, 0.8f, 0.4f, 0.15f)));
                    ImGui::GetWindowDrawList()->AddCircleFilled(
                        ImVec2(icon_p.x + 20, icon_p.y + 20), 12,
                        ImGui::GetColorU32(theme().success));
                    ImGui::Dummy(ImVec2(40, 40));
                    ImGui::SameLine(48);
                    ImGui::TextColored(theme().success, "Game is running stable");
                    ImGui::TextDisabled("No crashes detected. Your system is healthy.");
                } else {
                    const CrashDiagnosis diagnosis = diagnose_crash_report(latest_crash);
                    // Structured crash layout matching reference panel 15
                    ImVec2 icon_p = ImGui::GetCursorScreenPos();
                    ImGui::GetWindowDrawList()->AddCircleFilled(
                        ImVec2(icon_p.x + 20, icon_p.y + 20), 20,
                        ImGui::GetColorU32(ImVec4(0.9f, 0.2f, 0.2f, 0.15f)));
                    ImGui::GetWindowDrawList()->AddCircleFilled(
                        ImVec2(icon_p.x + 20, icon_p.y + 20), 12,
                        ImGui::GetColorU32(theme().error));
                    ImGui::Dummy(ImVec2(40, 40));
                    ImGui::SameLine(48);
                    ImGui::TextColored(theme().error, "Something went wrong!");
                    ImGui::TextDisabled("Minecraft has crashed.");
                    ImGui::Spacing();
                    client_separator();
                    ImGui::Spacing();
                    
                    // Structured fields
                    auto crash_field = [](const char* label, const char* val) {
                        ImGui::TextColored(ImVec4(0.55f, 0.55f, 0.60f, 1), "%s", label);
                        ImGui::SameLine(140);
                        ImGui::Text("%s", val);
                    };
                    crash_field("Likely Cause", diagnosis.cause.c_str());
                    crash_field("Detected Issue", diagnosis.issue.c_str());
                    crash_field("Minecraft Version", profile().mc_version.empty() ? "Unknown" : profile().mc_version.c_str());
                    crash_field("Loader", profile().loader.empty() ? "Unknown" : profile().loader.c_str());
                    crash_field("Crash Time", std::filesystem::path(latest_crash).filename().string().c_str());
                    
                    ImGui::Spacing();
                    ImGui::TextColored(theme().accent, "Solution");
                    ImGui::PushStyleColor(ImGuiCol_Text, theme().muted);
                    ImGui::TextWrapped("%s", diagnosis.solution.c_str());
                    ImGui::PopStyleColor();
                    
                    ImGui::Spacing();
                    client_separator();
                    ImGui::Spacing();
                    
                    float btn_w = (ImGui::GetContentRegionAvail().x - 8) / 2.0f;
                    if (client_primary_button("Open Crash Report", ImVec2(btn_w, 32))) {
                        ShellExecuteA(nullptr, "open", latest_crash.c_str(), nullptr, nullptr, SW_SHOW);
                    }
                    ImGui::SameLine(0, 8);
                    if (client_secondary_button("View Logs", ImVec2(btn_w, 32))) {
                        diag_tab = 1;
                    }
                }
            }
            client_card_end();

            ImGui::Spacing();

            client_card_begin("SYSTEM INFO");
            {
                auto info_row = [](const char* label, const char* value) {
                    ImGui::TextColored(theme().muted, "%s", label);
                    ImGui::SameLine(160);
                    ImGui::Text("%s", value);
                };
                info_row("Minecraft Version", profile().mc_version.empty() ? "Unknown" : profile().mc_version.c_str());
                info_row("Loader", profile().loader.empty() ? "Unknown" : profile().loader.c_str());
                info_row("Mods Installed", std::to_string(mods().size()).c_str());
                info_row("FPS", (std::to_string(static_cast<int>(perf().fps)) + " (1% low: " +
                    std::to_string(static_cast<int>(perf().fps_1pct_low)) + ")").c_str());
                info_row("Memory", (std::to_string(static_cast<int>(perf().ram_mb / 1024.0f)) +
                    " / " + std::to_string(static_cast<int>(perf().ram_max_mb / 1024.0f)) + " GB").c_str());
                info_row("CPU", (std::to_string(static_cast<int>(perf().cpu_percent)) + "%").c_str());
                info_row("Session Time",
                    profile().session_start > 0
                        ? (std::to_string((std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count() -
                            profile().session_start) / 60) + " minutes").c_str()
                        : "Not started");
            }
            client_card_end();

            ImGui::Spacing();

            client_card_begin("RECOMMENDED ACTIONS");
            {
                ImGui::TextDisabled("Based on your current system state:");
                ImGui::Spacing();
                if (perf().fps < 30.0f) {
                    ImGui::TextColored(theme().warning, "Low FPS detected");
                    ImGui::TextDisabled("Consider reducing render distance or closing background applications.");
                } else if (perf().fps < 60.0f) {
                    ImGui::TextColored(theme().muted, "Moderate FPS");
                    ImGui::TextDisabled("Your system is performing adequately. Check Performance tab for optimization.");
                } else {
                    ImGui::TextColored(theme().success, "Good performance");
                    ImGui::TextDisabled("Your system is running well.");
                }
                if (mods().size() > 50) {
                    ImGui::Spacing();
                    ImGui::TextColored(theme().warning, "Many mods installed (%d)", static_cast<int>(mods().size()));
                    ImGui::TextDisabled("Large mod packs can affect performance. Consider removing unused mods.");
                }
            }
            client_card_end();
        }
        break;

    case 1: // Logs
        {
            client_card_begin("GAME LOGS");
            {
                const std::string mc_dir = game_dir();
                std::string latest_log = mc_dir + "\\logs\\latest.log";

                std::error_code ec;
                if (!std::filesystem::exists(latest_log, ec)) {
                    ImGui::TextDisabled("No log file found at: %s", latest_log.c_str());
                    ImGui::TextDisabled("Launch the game first to generate logs.");
                } else {
                    ImGui::TextDisabled("%s", latest_log.c_str());
                    ImGui::Spacing();

                    // Read last N lines
                    std::ifstream logf(latest_log);
                    if (logf.is_open()) {
                        std::vector<std::string> lines;
                        std::string line;
                        while (std::getline(logf, line)) {
                            lines.push_back(line);
                        }

                        int start = std::max(0, static_cast<int>(lines.size()) - 50);
                        ImGui::BeginChild("##log_content", ImVec2(0, -36), true);
                        for (int i = start; i < static_cast<int>(lines.size()); ++i) {
                            ImGui::TextDisabled("%s", lines[i].c_str());
                        }
                        ImGui::EndChild();

                        float btn_w = (ImGui::GetContentRegionAvail().x - 8) / 2.0f;
                        if (client_secondary_button("Open Log Folder", ImVec2(btn_w, 28))) {
                            ShellExecuteA(nullptr, "open",
                                (mc_dir + "\\logs").c_str(), nullptr, nullptr, SW_SHOW);
                        }
                        ImGui::SameLine(0, 8);
                        if (client_secondary_button("Copy Log Path", ImVec2(btn_w, 28))) {
                            copy_to_clipboard(latest_log);
                        }
                    }
                }
            }
            client_card_end();
        }
        break;
    }
}

// ---------------------------------------------------------------------------
// Social Page
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Settings Page
// ---------------------------------------------------------------------------

void ClientUI::render_settings_page() {
    auto& s = settings();
    static int settings_tab = 0; // 0=General, 1=Keybinds, 2=Appearance, 3=Notifications, 4=Privacy, 5=Advanced
    float scale = settings().ui_scale;

    // Smooth tab transition
    static float tab_fade = 1.0f;
    if (settings_tab != g_prev_tab) {
        tab_fade = 0.0f;
        g_prev_tab = settings_tab;
    }
    if (settings().animations) {
        tab_fade += (1.0f - tab_fade) * 0.12f;
        if (tab_fade > 0.98f) tab_fade = 1.0f;
    } else {
        tab_fade = 1.0f;
    }

    // Category rail matching reference design
    ImGui::BeginChild("##settings_categories", ImVec2(160 * scale, 0), true);
    {
        struct SettingsCategory { const char* label; int id; };
        SettingsCategory categories[] = {
            {"General", 0}, {"Keybinds", 1}, {"Appearance", 2},
            {"Notifications", 3}, {"Privacy", 4}, {"Advanced", 5}
        };
        for (const auto& cat : categories) {
            bool active = (settings_tab == cat.id);
            if (active) {
                ImGui::PushStyleColor(ImGuiCol_Button, theme().accent);
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            }
            if (ImGui::Button(cat.label, ImVec2(-1, 32 * scale))) {
                settings_tab = cat.id;
            }
            ImGui::PopStyleColor();
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // Settings content
    ImGui::BeginChild("##settings_content", ImVec2(0, 0), false);
    {
        if (settings().animations && tab_fade < 1.0f) {
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, tab_fade);
        }
        switch (settings_tab) {
        case 0: { // General
            client_card_begin("GENERAL");
            ImGui::Text("Menu Key");
            ImGui::SameLine(120);
            ImGui::SetNextItemWidth(120);
            const char* key_names[] = {"Insert", "Delete", "Home", "End", "PageUp", "PageDown", "F1", "F2", "F3"};
            int key_idx = 0;
            if (s.menu_key == VK_INSERT) key_idx = 0;
            else if (s.menu_key == VK_DELETE) key_idx = 1;
            else if (s.menu_key == VK_HOME) key_idx = 2;
            else if (s.menu_key == VK_END) key_idx = 3;
            else if (s.menu_key == VK_PRIOR) key_idx = 4;
            else if (s.menu_key == VK_NEXT) key_idx = 5;
            else if (s.menu_key == VK_F1) key_idx = 6;
            else if (s.menu_key == VK_F2) key_idx = 7;
            else if (s.menu_key == VK_F3) key_idx = 8;
            if (ImGui::Combo("##key", &key_idx, key_names, 9)) {
                int keys[] = {VK_INSERT, VK_DELETE, VK_HOME, VK_END, VK_PRIOR, VK_NEXT, VK_F1, VK_F2, VK_F3};
                s.menu_key = keys[key_idx];
            }

            ImGui::Text("Language");
            ImGui::SameLine(120);
            ImGui::SetNextItemWidth(120);
            const char* langs[] = {"English", "Espa\xc3\xb1ol", "Deutsch", "Fran\xc3\xa7" "ais", "Portugu\xc3\xaas"};
            int lang_idx = 0;
            for (int i = 0; i < 5; ++i) { if (s.language == langs[i]) { lang_idx = i; break; } }
            if (ImGui::Combo("##lang", &lang_idx, langs, 5)) {
                s.language = langs[lang_idx];
            }

            ImGui::Text("UI Scale");
            ImGui::SameLine(120);
            ImGui::SetNextItemWidth(120);
            ImGui::SliderFloat("##scale", &s.ui_scale, 0.5f, 2.0f, "%.1fx");

            ImGui::Text("Theme");
            ImGui::SameLine(120);
            ImGui::SetNextItemWidth(120);
            const char* themes[] = {"Dark Purple", "Dark Red", "Dark Green"};
            ImGui::Combo("##theme", &s.theme_index, themes, 3);

            ImGui::Checkbox("Animations", &s.animations);
            ImGui::SameLine(200);
            ImGui::Checkbox("Compact Mode", &s.compact_mode);

            ImGui::Text("Menu Style");
            ImGui::SameLine(120);
            ImGui::SetNextItemWidth(120);
            const char* menu_styles[] = {"Sidebar", "Radial"};
            ImGui::Combo("##menustyle", &s.quick_menu_style, menu_styles, 2);

            ImGui::Checkbox("Auto Update Client", &s.auto_update);
            ImGui::Checkbox("Show Client Version", &s.show_client_version);
            client_card_end();
            break;
        }

        case 1: { // Keybinds
            client_card_begin("KEYBINDS");
            ImGui::TextDisabled("Module keybinds are configured in the Modules tab.");
            ImGui::Spacing();
            ImGui::Text("Menu Toggle");
            ImGui::SameLine(120);
            const char* key_display = "Insert";
            if (s.menu_key == VK_DELETE) key_display = "Delete";
            else if (s.menu_key == VK_HOME) key_display = "Home";
            else if (s.menu_key == VK_END) key_display = "End";
            else if (s.menu_key == VK_PRIOR) key_display = "PageUp";
            else if (s.menu_key == VK_NEXT) key_display = "PageDown";
            else if (s.menu_key == VK_F1) key_display = "F1";
            else if (s.menu_key == VK_F2) key_display = "F2";
            else if (s.menu_key == VK_F3) key_display = "F3";
            ImGui::Text("%s", key_display);
            ImGui::Spacing();
            ImGui::TextDisabled("Press INSERT (default) to open the client menu.");
            client_card_end();
            break;
        }

        case 2: { // Appearance
            client_card_begin("APPEARANCE");
            ImGui::Text("Show FPS");
            ImGui::SameLine(120);
            ImGui::Checkbox("##fps", &s.show_fps);
            ImGui::Text("Show Notifications");
            ImGui::SameLine(120);
            ImGui::Checkbox("##notif", &s.notifications_enabled);
            ImGui::Text("Notifications Position");
            ImGui::SameLine(120);
            ImGui::SetNextItemWidth(120);
            const char* notif_pos[] = {"Top Right", "Bottom Right", "Top Left"};
            ImGui::Combo("##notifpos", &s.notifications_position, notif_pos, 3);
            client_card_end();
            break;
        }

        case 3: { // Notifications
            client_card_begin("NOTIFICATIONS");
            ImGui::Checkbox("Enable Notifications", &s.notifications_enabled);
            ImGui::Spacing();
            ImGui::TextDisabled("Controls all Amalgam in-game toasts.");
            ImGui::Text("Position");
            ImGui::SameLine(120);
            ImGui::SetNextItemWidth(140);
            const char* notification_positions[] = {"Top Right", "Bottom Right", "Top Left"};
            ImGui::Combo("##notification_position", &s.notifications_position,
                         notification_positions, 3);
            if (client_secondary_button("Send Test Notification", ImVec2(220, 28))) {
                NotificationManager::instance().push_toast(
                    "Notifications ready", "This is how Amalgam alerts will appear.", NotifyLevel::Success);
            }
            client_card_end();
            break;
        }

        case 4: { // Privacy
            client_card_begin("PRIVACY");
            ImGui::Checkbox("Show Online Status", &s.privacy_show_online);
            ImGui::Checkbox("Show Current Server", &s.privacy_show_server);
            ImGui::Spacing();
            ImGui::TextDisabled("Control what other players can see about you.");
            client_card_end();
            break;
        }

        case 5: { // Advanced
            client_card_begin("ADVANCED");
            ImGui::Checkbox("Launcher Connection", &s.launcher_connection);
            ImGui::SameLine(200);
            ImGui::Checkbox("Developer Mode", &s.developer_mode);
            ImGui::Spacing();
            ImGui::TextDisabled("Advanced settings for development and debugging.");
            client_card_end();
            break;
        }
        }

        ImGui::Spacing();
        if (client_primary_button("Save Settings")) {
            const bool saved = save_client_settings(config_dir() + "\\client.cfg");
            NotificationManager::instance().push_toast(
                saved ? "Settings Saved" : "Settings Save Failed",
                saved ? "Configuration updated" : "Could not write configuration",
                saved ? NotifyLevel::Success : NotifyLevel::Error);
        }

        ImGui::Spacing();
        client_separator();
        ImGui::Spacing();

        // About section
        client_card_begin("ABOUT");
        {
            // Real packaged logo (with a text fallback if the optional asset
            // is unavailable in a developer build).
            ImVec2 logo_p = ImGui::GetCursorScreenPos();
            ImVec2 logo_size(60 * scale, 60 * scale);
            const bool drew_logo = draw_client_art("amalgam-logo.png", logo_p,
                                                   logo_size, false,
                                                   IM_COL32(255, 255, 255, 255));
            if (!drew_logo) {
                ImGui::GetWindowDrawList()->AddRectFilled(
                    logo_p, logo_p + logo_size,
                    ImGui::GetColorU32(theme().accent), 12.0f);
                ImVec2 center = logo_p + logo_size * 0.5f;
                ImVec2 ts = ImGui::CalcTextSize("A");
                ImGui::GetWindowDrawList()->AddText(
                    ImVec2(center.x - ts.x * 0.5f, center.y - ts.y * 0.5f),
                    ImGui::GetColorU32(ImVec4(1, 1, 1, 1)), "A");
            }
            ImGui::Dummy(logo_size);
            ImGui::SameLine(80);
            ImGui::TextColored(theme().accent, "AMALGAM CLIENT");
        ImGui::TextDisabled("v3.0.0-beta.3");
            ImGui::TextDisabled("The ultimate Minecraft launcher.");
            ImGui::TextDisabled("Play. Create. Host. Together.");
            ImGui::Spacing();
            std::time_t now = std::time(nullptr);
            std::tm local{};
            localtime_s(&local, &now);
            ImGui::TextDisabled("Copyright %d Amalgam. All rights reserved.",
                                local.tm_year + 1900);
            ImGui::Spacing();
            float link_w = (ImGui::GetContentRegionAvail().x - 8) / 2.0f;
            if (client_secondary_button("Website", ImVec2(link_w, 24))) {
                ShellExecuteA(nullptr, "open", "https://amalgam-mc.com/",
                    nullptr, nullptr, SW_SHOW);
            }
            ImGui::SameLine(0, 8);
            if (client_secondary_button("Discord", ImVec2(link_w, 24))) {
                ShellExecuteA(nullptr, "open", "https://discord.gg/amalgam",
                    nullptr, nullptr, SW_SHOW);
            }
        }
        client_card_end();

        if (settings().animations && tab_fade < 1.0f) {
            ImGui::PopStyleVar();
        }
    }
    ImGui::EndChild();
}

}  // namespace aml::client
