#include "ui_internal.h"
#include "ui_motion.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace aml::ui {

namespace {

// Dear ImGui uses the text before "##" as the visible label and the full
// string as the ID.  The premium buttons below draw their own label after the
// invisible interaction surface has been created, so retain that convention.
std::string visible_button_label(const char* label) {
    if (!label) return {};
    const char* marker = std::strstr(label, "##");
    return marker ? std::string(label, marker) : std::string(label);
}

ImVec4 with_alpha(ImVec4 color, float alpha) {
    color.w = alpha;
    return color;
}

} // namespace

float auto_item_width(float preferred, float minimum, float reserved) {
    const float available = ImGui::GetContentRegionAvail().x - ui_px(reserved);
    if (available <= 0.0f) return 0.0f;
    const float wanted = std::max(ui_px(minimum), ui_px(preferred));
    return std::min(wanted, available);
}

bool input_text(const char* label, std::string* value) {
    char buf[4096];
    strncpy_s(buf, value->c_str(), _TRUNCATE);
    bool changed = ImGui::InputText(label, buf, sizeof(buf));
    if (changed) *value = buf;
    return changed;
}

bool input_text_hint(const char* label, const char* hint, std::string* value) {
    char buf[4096];
    strncpy_s(buf, value->c_str(), _TRUNCATE);
    bool changed = ImGui::InputTextWithHint(label, hint, buf, sizeof(buf));
    if (changed) *value = buf;
    return changed;
}

bool input_secret(const char* label, std::string* value) {
    char buf[4096];
    strncpy_s(buf, value->c_str(), _TRUNCATE);
    bool changed = ImGui::InputText(label, buf, sizeof(buf), ImGuiInputTextFlags_Password);
    if (changed) *value = buf;
    return changed;
}

void draw_badge(ImDrawList* dl, const ImVec2& pos, const char* text, const ImVec4& fg,
                 const ImVec4& bg) {
    ImGui::PushFont(f_small);
    ImVec2 t = ImGui::CalcTextSize(text);
    ImVec2 sz(t.x + ui_px(14.0f), ui_px(20.0f));
    dl->AddRectFilled(pos, pos + sz, c32(bg), ui_px(5.0f));
    dl->AddText(ImVec2(pos.x + ui_px(7.0f), pos.y + ui_px(4.0f)), c32(fg), text);
    ImGui::PopFont();
}

void draw_badge_vert(ImDrawList* dl, const ImVec2& pos, const char* text, const ImVec4& fg,
                     const ImVec4& bg) {
    ImGui::PushFont(f_small);
    ImVec2 t = ImGui::CalcTextSize(text);
    ImVec2 sz(t.x + ui_px(14.0f), ui_px(20.0f));
    dl->AddRectFilled(pos, pos + sz, c32(bg), ui_px(5.0f));
    dl->AddText(ImVec2(pos.x + ui_px(7.0f), pos.y + ui_px(4.0f)), c32(fg), text);
    ImGui::PopFont();
}

void page_title(const char* title, const char* subtitle) {
    draw_page_header(title, subtitle);
}

void draw_page_emblem(UiState& st, const char* asset_name) {
    if (!asset_name || !asset_name[0]) return;
    const float size = ui_px(56.0f);
    const ImVec2 content = ImGui::GetContentRegionAvail();
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const ImVec2 pos(cursor.x + std::max(0.0f, content.x - size - ui_px(16.0f)),
                     cursor.y - ui_px(6.0f));
    std::wstring wide_name;
    for (const unsigned char ch : std::string(asset_name)) wide_name.push_back(static_cast<wchar_t>(ch));
    draw_local_image(st, st.exe_dir + L"\\branding\\ai\\" + wide_name,
                     pos, ImVec2(size, size), c32(with_alpha(k.surface2, 0.18f)),
                     ui_model::ImageFit::Contain);
}

void draw_page_header(const char* title, const char* subtitle) {
    // Keep a consistent editorial header across every launcher page.  The
    // violet rail gives wide, information-dense screens a clear first anchor
    // without taking vertical room away from the actual content.
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float title_height = ImGui::GetFontSize() * 1.55f;
    const float header_height = title_height + (subtitle && subtitle[0] ? ui_px(20.0f) : ui_px(4.0f));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(origin, origin + ImVec2(ui_px(3.0f), header_height),
                      c32(k.brand), ui_px(2.0f));
    dl->AddCircleFilled(origin + ImVec2(ui_px(1.5f), ui_px(5.5f)), ui_px(2.2f),
                        c32(k.brand_hov));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ui_px(10.0f));

    // Enforced hierarchy: PAGE TITLE (f_title) → subtitle (small, muted).
    // The subtitle is kept readable (never the old tiny TextDisabled gray).
    ImGui::PushFont(f_title);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    if (subtitle && subtitle[0]) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ui_px(0.0f));
        ImGui::PushFont(f_small);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
        ImGui::TextColored(k.muted, "%s", subtitle);
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
    }
    const ImVec2 line = ImGui::GetCursorScreenPos();
    dl->AddLine(ImVec2(line.x + ui_px(10.0f), line.y + ui_px(2.0f)),
                ImVec2(line.x + ImGui::GetContentRegionAvail().x, line.y + ui_px(3.0f)),
                c32(with_alpha(k.border, 0.72f)), ui_px(1.0f));
    ImGui::Dummy(ImVec2(0.0f, ui_px(5.0f)));
}

bool primary_button(const char* label, const ImVec2& size, bool loading, bool disabled) {
    // Use a transparent Dear ImGui button solely for input/focus handling, then
    // draw the layered surface ourselves. This keeps every action button crisp
    // and consistent with the reference's dense dark-purple desktop language.
    if (disabled) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 0));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 0));
    }
    ImGui::PushFont(f_bold);
    const std::string display_label = visible_button_label(label);
    const ImVec2 text_size = ImGui::CalcTextSize(display_label.c_str());
    const ImGuiStyle& style = ImGui::GetStyle();
    ImVec2 fitted = size;
    if (fitted.x > 0.0f)
        fitted.x = std::max(fitted.x, text_size.x + style.FramePadding.x * 2.0f + ui_px(8.0f));
    if (fitted.y > 0.0f)
        fitted.y = std::max(fitted.y, ImGui::GetTextLineHeight() + style.FramePadding.y * 2.0f);
    
    if (loading) {
        fitted.x += ui_px(24.0f); // Extra space for spinner
    }
    
    bool r = ImGui::Button(label, fitted);

    const ImVec2 bmin = ImGui::GetItemRectMin();
    const ImVec2 bmax = ImGui::GetItemRectMax();
    const bool hovered = ImGui::IsItemHovered();
    const bool pressed = ImGui::IsItemActive() && hovered;
    const bool focused = ImGui::IsItemFocused();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rounding = ui_px(8.0f);

    ImVec4 base = disabled ? k.surface : k.brand_dk;
    ImVec4 top = disabled ? k.surface2 : (hovered ? k.brand_hov : k.brand);
    ImVec4 bottom = disabled ? k.surface : (pressed ? k.brand_dk : k.brand_dk);
    dl->AddRectFilled(bmin, bmax, c32(base), rounding);
    const ImVec2 inset = ImVec2(ui_px(1.0f), ui_px(1.0f));
    dl->AddRectFilledMultiColor(bmin + inset, bmax - inset,
                                c32(top), c32(with_alpha(top, 0.90f)),
                                c32(bottom), c32(bottom));
    dl->AddLine(bmin + ImVec2(ui_px(8.0f), ui_px(1.0f)),
                ImVec2(bmax.x - ui_px(8.0f), bmin.y + ui_px(1.0f)),
                c32(with_alpha(k.brand_hov, disabled ? 0.10f : 0.78f)), ui_px(1.0f));
    dl->AddRect(bmin, bmax, c32(disabled ? k.border : with_alpha(k.brand_hov, hovered ? 0.95f : 0.70f)),
                rounding, 0, ui_px(1.0f));
    if (focused) {
        dl->AddRect(bmin - ImVec2(ui_px(2.0f), ui_px(2.0f)),
                    bmax + ImVec2(ui_px(2.0f), ui_px(2.0f)), c32(k.brand_hov),
                    ui_px(10.0f), 0, ui_px(1.25f));
    }
    const ImVec2 label_pos(bmin.x + (bmax.x - bmin.x - text_size.x) * 0.5f,
                           bmin.y + (bmax.y - bmin.y - text_size.y) * 0.5f);
    dl->AddText(label_pos, c32(disabled ? k.muted : ImVec4(1, 1, 1, 1)), display_label.c_str());

    // Pressed feedback + hover glow (animated, honors reduced motion).
    if (!disabled) {
        static std::unordered_map<std::string, AnimFloat> g_btn_press;
        static std::unordered_map<std::string, AnimFloat> g_btn_glow;
        const std::string key(label);
        AnimFloat& press = g_btn_press[key];
        AnimFloat& glow = g_btn_glow[key];
        press.duration = 0.10f;
        glow.duration = 0.16f;
        press.target(pressed ? 1.0f : 0.0f);
        glow.target(hovered ? 1.0f : 0.0f);
        const float p = press.update(ImGui::GetIO().DeltaTime);
        const float g = glow.update(ImGui::GetIO().DeltaTime);

        // Pressed dip: darken + slight inset shadow.
        if (p > 0.02f) {
            ImVec4 dip = ImVec4(0, 0, 0, 0.18f * p);
            dl->AddRectFilled(bmin, bmax, c32(dip), ui_px(8.0f));
        }
        // Hover glow: soft brand halo around the button.
        if (g > 0.02f) {
            ImVec4 halo = k.brand;
            halo.w = 0.16f * g;
            dl->AddRect(bmin - ImVec2(ui_px(2.0f), ui_px(2.0f)),
                        bmax + ImVec2(ui_px(2.0f), ui_px(2.0f)),
                        c32(halo), ui_px(10.0f), 0, ui_px(2.0f));
        }
    }
    
    if (loading && !disabled) {
        ImVec2 center(bmax.x - ui_px(15.0f), (bmin.y + bmax.y) * 0.5f);
        float radius = ui_px(6.0f);
        float time = static_cast<float>(ImGui::GetTime());
        float start_angle = time * 3.0f;
        
        dl->PathArcTo(center, radius, start_angle, start_angle + 2.5f, 32);
        dl->PathStroke(c32(ImVec4(1, 1, 1, 0.8f)), false, ui_px(2.0f));
    }
    
    ImGui::PopFont();
    ImGui::PopStyleColor(4);
    return r && !disabled && !loading;
}

bool ghost_button(const char* label, const ImVec2& size, bool disabled) {
    if (disabled) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 0));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 0));
    }
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    const std::string display_label = visible_button_label(label);
    const ImVec2 text_size = ImGui::CalcTextSize(display_label.c_str());
    const ImGuiStyle& style = ImGui::GetStyle();
    ImVec2 fitted = size;
    if (fitted.x > 0.0f)
        fitted.x = std::max(fitted.x, text_size.x + style.FramePadding.x * 2.0f + ui_px(8.0f));
    if (fitted.y > 0.0f)
        fitted.y = std::max(fitted.y, ImGui::GetTextLineHeight() + style.FramePadding.y * 2.0f);
    bool r = ImGui::Button(label, fitted);
    const ImVec2 bmin = ImGui::GetItemRectMin();
    const ImVec2 bmax = ImGui::GetItemRectMax();
    const bool hovered = ImGui::IsItemHovered();
    const bool focused = ImGui::IsItemFocused();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rounding = ui_px(8.0f);
    const ImVec4 fill = disabled ? k.surface : (hovered ? k.hover : k.surface2);
    dl->AddRectFilled(bmin, bmax, c32(fill), rounding);
    dl->AddLine(bmin + ImVec2(ui_px(8.0f), ui_px(1.0f)),
                ImVec2(bmax.x - ui_px(8.0f), bmin.y + ui_px(1.0f)),
                c32(with_alpha(k.text, hovered ? 0.15f : 0.06f)), ui_px(1.0f));
    dl->AddRect(bmin, bmax, c32(disabled ? k.border : (hovered ? k.brand : k.border)),
                rounding, 0, ui_px(1.0f));
    if (focused) {
        dl->AddRect(bmin - ImVec2(ui_px(2.0f), ui_px(2.0f)),
                    bmax + ImVec2(ui_px(2.0f), ui_px(2.0f)), c32(k.brand_hov),
                    ui_px(10.0f), 0, ui_px(1.25f));
    }
    dl->AddText(ImVec2(bmin.x + (bmax.x - bmin.x - text_size.x) * 0.5f,
                       bmin.y + (bmax.y - bmin.y - text_size.y) * 0.5f),
                c32(disabled ? k.muted : k.text), display_label.c_str());
    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar();
    return r && !disabled;
}

// Per-card hover animation state, keyed by the child id. Kept small; cards
// are few per view. The map is bounded by callers reusing stable ids each
// frame.
static std::unordered_map<std::string, AnimFloat> g_card_hover;

void card_begin(const char* id, const ImVec2& size, bool hoverable) {
    const ImGuiChildFlags flags = ImGuiChildFlags_Borders |
                                  (size.y == 0.0f ? ImGuiChildFlags_AutoResizeY : 0);
    // Fixed-height cards are layout surfaces, not independent documents. Let
    // the route's page scroll carry them instead of showing a scrollbar inside
    // a row or compact panel when a line barely exceeds its measured height.
    const ImGuiWindowFlags window_flags = size.y > 0.0f
        ? ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
        : ImGuiWindowFlags_None;
    // Do not push styles across BeginChild/EndChild. Dear ImGui keeps the
    // parent and child stack backups separately, so a style pushed before a
    // child belongs to the parent while the child still validates it at
    // EndChild. Draw the card surface directly instead and keep both windows'
    // style stacks balanced.
    ImGui::BeginChild(id, size, flags, window_flags);
    const ImVec2 min = ImGui::GetWindowPos();
    const ImVec2 max = min + ImGui::GetWindowSize();
    const bool hovered = ImGui::IsWindowHovered();

    AnimFloat& anim = g_card_hover[std::string(id)];
    anim.duration = 0.14f;
    anim.target(hoverable && hovered ? 1.0f : 0.0f);
    const float h = anim.update(ImGui::GetIO().DeltaTime);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rounding = ui_px(k.radius_md);
    // Layered surfaces make cards feel like deliberate desktop panels instead
    // of default ImGui children. Content such as banner imagery is still drawn
    // afterwards and therefore remains fully visible.
    dl->AddRectFilled(min, max, c32(k.surface), rounding);
    // Keep the surface tint as a restrained depth cue. A 42px cap made short
    // rows look like header bars instead of cards, especially in dense lists.
    const float top_h = std::min(ui_px(28.0f), std::max(ui_px(10.0f), max.y - min.y - ui_px(2.0f)));
    dl->AddRectFilled(ImVec2(min.x + ui_px(1.0f), min.y + ui_px(1.0f)),
                      ImVec2(max.x - ui_px(1.0f), min.y + top_h),
                      c32(with_alpha(k.surface2, 0.58f)), rounding,
                      ImDrawFlags_RoundCornersTop);
    dl->AddLine(ImVec2(min.x + ui_px(9.0f), min.y + ui_px(1.0f)),
                ImVec2(max.x - ui_px(9.0f), min.y + ui_px(1.0f)),
                c32(with_alpha(k.brand_hov, hoverable ? 0.24f : 0.12f)), ui_px(1.0f));

    // Elevation: border glows toward brand and gains a soft outer glow as the
    // card is hovered. Subtle: border width 1 -> 1.5, plus a faint brand halo.
    const ImVec4 border_color = ImGui::ColorConvertU32ToFloat4(
        ImGui::ColorConvertFloat4ToU32(k.border));
    const ImVec4 glow_color = ImGui::ColorConvertU32ToFloat4(
        ImGui::ColorConvertFloat4ToU32(k.brand));
    ImVec4 blended;
    blended.x = border_color.x + (glow_color.x - border_color.x) * h;
    blended.y = border_color.y + (glow_color.y - border_color.y) * h;
    blended.z = border_color.z + (glow_color.z - border_color.z) * h;
    blended.w = border_color.w + (glow_color.w - border_color.w) * h;
    dl->AddRect(min, max, c32(blended), rounding,
                0, ui_px(1.0f + 0.5f * h));

    // Soft outer halo only while elevated.
    if (h > 0.02f) {
        ImVec4 halo = k.brand;
        halo.w = 0.10f * h;
        dl->AddRect(min - ImVec2(ui_px(2.0f), ui_px(2.0f)),
                    max + ImVec2(ui_px(2.0f), ui_px(2.0f)),
                    c32(halo), ui_px(10.0f), 0, ui_px(1.5f));
    }
}

void card_end(bool was_hoverable) {
    (void)was_hoverable;
    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// Empty state display
// ---------------------------------------------------------------------------

void empty_state(const char* title, const char* message, const char* icon, const char* action_label) {
    const ImVec2 content = ImGui::GetContentRegionAvail();
    const float width = std::min(ui_px(460.0f), std::max(ui_px(220.0f), content.x - ui_px(32.0f)));
    const float height = icon ? ui_px(198.0f) : ui_px(104.0f);
    const ImVec2 origin = ImGui::GetCursorPos() + ImVec2(
        std::max(0.0f, (content.x - width) * 0.5f),
        std::max(0.0f, (content.y - height) * 0.5f));
    ImGui::SetCursorPos(origin);
    ImGui::BeginGroup();
    if (icon) {
        // Build a small branded illustration rather than leaving a lone
        // placeholder letter in the middle of an otherwise premium screen.
        // The word-style legacy symbols ("backup", "cloud", etc.) collapse to
        // a neutral sparkle while single-character glyphs remain informative.
        const float icon_box = ui_px(56.0f);
        const ImVec2 icon_pos = ImGui::GetCursorScreenPos() +
                                ImVec2((width - icon_box) * 0.5f, ui_px(12.0f));
        const ImVec2 center = icon_pos + ImVec2(icon_box * 0.5f, icon_box * 0.5f);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec4 halo = k.brand;
        halo.w = 0.08f;
        dl->AddCircleFilled(center, icon_box * 0.72f, c32(halo), 48);
        ImVec4 ring = k.brand_hov;
        ring.w = 0.30f;
        dl->AddCircle(center, icon_box * 0.62f, c32(ring), 48, ui_px(1.0f));
        ImVec4 inner = k.brand;
        inner.w = 0.18f;
        dl->AddCircleFilled(center, icon_box * 0.45f, c32(inner), 40);
        dl->AddRectFilled(icon_pos, icon_pos + ImVec2(icon_box, icon_box),
                          c32(k.brand_dk), ui_px(16.0f));
        dl->AddRect(icon_pos, icon_pos + ImVec2(icon_box, icon_box),
                    c32(k.brand_hov), ui_px(16.0f), 0, ui_px(1.2f));

        const std::string glyph = std::strlen(icon) <= 2 ? icon : "•";
        ImGui::PushFont(f_h2);
        const float glyph_width = ImGui::CalcTextSize(glyph.c_str()).x;
        dl->AddText(ImGui::GetFont(), f_h2->LegacySize,
                    icon_pos + ImVec2((icon_box - glyph_width) * 0.5f,
                                     (icon_box - f_h2->LegacySize) * 0.5f - ui_px(1.0f)),
                    c32(k.brand_hov), glyph.c_str());
        ImGui::PopFont();

        // Three small orbiting lights give empty screens a finished visual
        // rhythm without distracting from the next action.
        for (const ImVec2 offset : {ImVec2(-icon_box * 0.58f, -icon_box * 0.28f),
                                    ImVec2(icon_box * 0.58f, -icon_box * 0.12f),
                                    ImVec2(icon_box * 0.46f, icon_box * 0.52f)}) {
            ImVec4 mote = k.brand_hov;
            mote.w = 0.72f;
            dl->AddCircleFilled(center + offset, ui_px(2.2f), c32(mote), 12);
        }
        ImGui::Dummy(ImVec2(0, icon_box + ui_px(30.0f)));
    }
    const float title_width = ImGui::CalcTextSize(title).x;
    ImGui::SetCursorPosX(origin.x + (width - title_width) * 0.5f);
    ImGui::PushFont(f_bold);
    ImGui::TextColored(k.text, "%s", title);
    ImGui::PopFont();
    ImGui::PushTextWrapPos(origin.x + width);
    ImGui::TextColored(k.muted, "%s", message);
    ImGui::PopTextWrapPos();
    
    if (action_label) {
        ImGui::Spacing();
        ImGui::Spacing();
        const float btn_width = ImGui::CalcTextSize(action_label).x + ui_px(32.0f);
        ImGui::SetCursorPosX(origin.x + (width - btn_width) * 0.5f);
        if (primary_button(action_label)) {
            // Action callback would be handled by caller
        }
    }
    
    ImGui::EndGroup();
}

void illustrated_empty_state(IconId icon, const char* title, const char* message,
                             const char* action_label,
                             void (*on_action)(UiState&),
                             UiState* action_state) {
    const ImVec2 content = ImGui::GetContentRegionAvail();
    const float width = std::min(ui_px(460.0f), std::max(ui_px(240.0f), content.x - ui_px(32.0f)));
    const float height = ui_px(168.0f);
    const ImVec2 origin = ImGui::GetCursorPos() + ImVec2(
        std::max(0.0f, (content.x - width) * 0.5f),
        std::max(0.0f, (content.y - height) * 0.5f));
    ImGui::SetCursorPos(origin);
    ImGui::BeginGroup();

    // Layered geometric illustration: outer glow ring + brand disc + icon.
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 center = ImGui::GetCursorScreenPos() +
            ImVec2(width * 0.5f, ui_px(40.0f));
        const float outer_r = ui_px(34.0f);
        const float inner_r = ui_px(24.0f);
        // Outer faint ring
        ImVec4 ring = k.brand;
        ring.w = 0.10f;
        dl->AddCircleFilled(center, outer_r, c32(ring), 40);
        // Inner disc
        ImVec4 disc = k.brand_dk;
        disc.w = 0.55f;
        dl->AddCircleFilled(center, inner_r, c32(disc), 40);
        // Accent arc (premium flourish)
        dl->PathArcTo(center, outer_r - ui_px(2.0f), 0.3f, 2.1f, 24);
        dl->PathStroke(c32(k.brand), false, ui_px(2.0f));
        // Icon
        draw_icon(icon, center, ui_px(13.0f), c32(k.brand_hov));
        ImGui::Dummy(ImVec2(0, ui_px(80.0f)));
    }

    const float title_width = ImGui::CalcTextSize(title).x;
    ImGui::SetCursorPosX(origin.x + (width - title_width) * 0.5f);
    ImGui::PushFont(f_bold);
    ImGui::TextColored(k.text, "%s", title);
    ImGui::PopFont();
    ImGui::PushTextWrapPos(origin.x + width);
    ImGui::TextColored(k.muted, "%s", message);
    ImGui::PopTextWrapPos();

    if (action_label) {
        ImGui::Spacing();
        ImGui::Spacing();
        const float btn_width = ImGui::CalcTextSize(action_label).x + ui_px(32.0f);
        ImGui::SetCursorPosX(origin.x + (width - btn_width) * 0.5f);
        if (primary_button(action_label)) {
            if (on_action && action_state) on_action(*action_state);
        }
    }

    ImGui::EndGroup();
}

// ---------------------------------------------------------------------------
// Progress bar with animation
// ---------------------------------------------------------------------------

void progress_bar(float progress, const ImVec2& size, const char* overlay_text,
                  const ImVec4* color) {
    const float normalized_progress = ui_model::normalize_progress(progress);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, k.surface2);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, color ? *color : k.brand);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ui_px(4.0f));
    
    ImVec2 bar_size = size;
    if (bar_size.x == 0.0f) bar_size.x = ImGui::GetContentRegionAvail().x;
    if (bar_size.y == 0.0f) bar_size.y = ui_px(8.0f);
    
    ImGui::ProgressBar(normalized_progress, bar_size, overlay_text ? overlay_text : "");
    
    // Add striped pattern for indeterminate progress
    if (normalized_progress < 0.0f) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p_min = ImGui::GetItemRectMin();
        ImVec2 p_max = ImGui::GetItemRectMax();
        float time = static_cast<float>(ImGui::GetTime());
        float offset = fmod(time * 20.0f, bar_size.x * 2.0f);
        
        dl->PushClipRect(p_min, p_max, true);
        
        for (float x = -bar_size.x + offset; x < bar_size.x; x += ui_px(20.0f)) {
            ImVec2 stripe_start = p_min + ImVec2(x, 0);
            ImVec2 stripe_end = stripe_start + ImVec2(ui_px(10.0f), bar_size.y);
            dl->AddRectFilled(stripe_start, stripe_end, c32(ImVec4(1, 1, 1, 0.1f)));
        }
        
        dl->PopClipRect();
    }
    
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
}

// ---------------------------------------------------------------------------
// Breadcrumb navigation
// ---------------------------------------------------------------------------

static void navigate_breadcrumb(const std::string& crumb) {
    if (!app) return;
    if (crumb == "Home") navigate_to(*app, 0, 0);
    else if (crumb == "Discover") navigate_to(*app, 2, 16, 0);
    else if (crumb == "Library") navigate_to(*app, 3, 6);
    else if (crumb == "Downloads") navigate_to(*app, 4, 17);
    else if (crumb == "Servers") navigate_to(*app, 8, 8);
    else if (crumb == "Essentials") navigate_to(*app, 23, 23);
    else if (crumb == "Settings") navigate_to(*app, 12, 4);
}

void draw_breadcrumbs(const std::vector<std::string>& crumbs) {
    if (crumbs.empty()) return;
    
    ImGui::PushFont(f_small);
    ImGui::PushStyleColor(ImGuiCol_Text, k.muted);
    
    for (size_t i = 0; i < crumbs.size(); ++i) {
        if (i > 0) {
            ImGui::SameLine(0, ui_px(4.0f));
            ImGui::TextDisabled("/");
            ImGui::SameLine(0, ui_px(4.0f));
        }
        
        if (i < crumbs.size() - 1) {
            ImGui::PushStyleColor(ImGuiCol_Text, k.brand);
            if (ghost_button(crumbs[i].c_str(), ImVec2(0, ui_px(24.0f)))) {
                navigate_breadcrumb(crumbs[i]);
            }
            ImGui::PopStyleColor();
        } else {
            ImGui::TextUnformatted(crumbs[i].c_str());
        }
        
        if (i < crumbs.size() - 1) {
            ImGui::SameLine(0, ui_px(4.0f));
        }
    }
    
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::Spacing();
}

// ---------------------------------------------------------------------------
// Health badge
// ---------------------------------------------------------------------------

void draw_health_badge(ImDrawList* dl, const ImVec2& pos, const char* status, const ImVec4& color) {
    ImGui::PushFont(f_small);
    ImVec2 t = ImGui::CalcTextSize(status);
    ImVec2 sz(t.x + ui_px(12.0f), ui_px(18.0f));
    
    // Background
    dl->AddRectFilled(pos, pos + sz, c32(color), ui_px(4.0f));
    
    // Status text
    dl->AddText(ImVec2(pos.x + ui_px(6.0f), pos.y + ui_px(3.0f)), c32(ImVec4(1, 1, 1, 1)), status);
    ImGui::PopFont();
}

// ---------------------------------------------------------------------------
// Quick search dialog
// ---------------------------------------------------------------------------

struct QuickSearchState {
    bool open = false;
    bool focus_input = true;
    char query[256] = "";
    int selected_index = 0;
    std::vector<std::string> recent_searches;
    std::vector<std::string> results;
};

static QuickSearchState g_quick_search;

bool g_quick_search_open() { return g_quick_search.open; }
std::string g_quick_search_query() { return g_quick_search.query; }
void g_quick_search_set_results(const std::vector<std::string>& results) {
    constexpr size_t kMaxResults = 64;
    if (g_quick_search.results == results) {
        g_quick_search.selected_index =
            ui_model::clamp_selection(g_quick_search.selected_index, g_quick_search.results.size());
        return;
    }
    g_quick_search.results.assign(results.begin(),
                                  results.begin() + std::min(results.size(), kMaxResults));
    g_quick_search.selected_index =
        ui_model::clamp_selection(g_quick_search.selected_index, g_quick_search.results.size());
}

bool quick_search_dialog(std::string* selected_result) {
    if (!g_quick_search.open) return false;

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ui_px(600.0f), ui_px(400.0f)), ImGuiCond_Always);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_Modal |
                             ImGuiWindowFlags_NoSavedSettings;
    bool open = true;
    bool activated = false;

    auto close_search = [&]() {
        g_quick_search.open = false;
        g_quick_search.query[0] = '\0';
        g_quick_search.selected_index = 0;
        g_quick_search.results.clear();
        g_quick_search.focus_input = true;
    };
    auto activate_result = [&](int index) {
        if (g_quick_search.results.empty()) return;
        const int safe_index = ui_model::clamp_selection(index, g_quick_search.results.size());
        if (selected_result) *selected_result = g_quick_search.results[static_cast<size_t>(safe_index)];
        const std::string query = g_quick_search.query;
        if (!query.empty()) {
            auto existing = std::find(g_quick_search.recent_searches.begin(),
                                      g_quick_search.recent_searches.end(), query);
            if (existing != g_quick_search.recent_searches.end())
                g_quick_search.recent_searches.erase(existing);
            g_quick_search.recent_searches.insert(g_quick_search.recent_searches.begin(), query);
            if (g_quick_search.recent_searches.size() > 8)
                g_quick_search.recent_searches.resize(8);
        }
        activated = true;
        close_search();
    };

    if (ImGui::Begin("Quick Search", &open, flags)) {
        ImGui::PushFont(f_bold);
        ImGui::TextUnformatted("Search");
        ImGui::PopFont();
        ImGui::Spacing();

        if (g_quick_search.focus_input) {
            ImGui::SetKeyboardFocusHere(0);
            g_quick_search.focus_input = false;
        }
        const bool submitted = ImGui::InputText("##search", g_quick_search.query,
                                                sizeof(g_quick_search.query),
                                                ImGuiInputTextFlags_EnterReturnsTrue);

        if (strlen(g_quick_search.query) == 0 && !g_quick_search.recent_searches.empty()) {
            ImGui::Spacing();
            ImGui::PushFont(f_small);
            ImGui::TextDisabled("Recent Searches");
            ImGui::PopFont();
            ImGui::Spacing();
            for (size_t i = 0; i < g_quick_search.recent_searches.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::Selectable(g_quick_search.recent_searches[i].c_str(), false)) {
                    strncpy_s(g_quick_search.query, g_quick_search.recent_searches[i].c_str(), _TRUNCATE);
                    g_quick_search.results.clear();
                    g_quick_search.selected_index = 0;
                    g_quick_search.focus_input = true;
                }
                ImGui::PopID();
            }
        }

        if (!activated && !g_quick_search.results.empty()) {
            ImGui::Spacing();
            ImGui::PushFont(f_small);
            ImGui::TextDisabled("Results");
            ImGui::PopFont();
            ImGui::Spacing();
            for (size_t i = 0; i < g_quick_search.results.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::Selectable(g_quick_search.results[i].c_str(),
                                      g_quick_search.selected_index == static_cast<int>(i)))
                    activate_result(static_cast<int>(i));
                ImGui::PopID();
                if (activated) break;
            }
        }

        if (!activated && (submitted || ImGui::IsKeyPressed(ImGuiKey_Enter)))
            activate_result(g_quick_search.selected_index);

        if (!activated && ImGui::IsKeyPressed(ImGuiKey_Escape)) close_search();
        if (!activated && !g_quick_search.results.empty()) {
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
                g_quick_search.selected_index = ui_model::clamp_selection(
                    g_quick_search.selected_index + 1, g_quick_search.results.size());
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
                g_quick_search.selected_index = ui_model::clamp_selection(
                    g_quick_search.selected_index - 1, g_quick_search.results.size());
        }
    }
    ImGui::End();

    if (!open && !activated) close_search();
    return activated;
}

void open_quick_search() {
    g_quick_search.open = true;
    g_quick_search.focus_input = true;
    g_quick_search.query[0] = '\0';
    g_quick_search.selected_index = 0;
    g_quick_search.results.clear();
}

// ---------------------------------------------------------------------------
// Enhanced operation progress display
// ---------------------------------------------------------------------------

void draw_operation_progress(const UiState::DownloadJob& job, const ImVec4* color) {
    const float progress = ui_model::normalize_progress(job.progress);
    std::string display_phase = job.phase;
    if (display_phase.find("http status") != std::string::npos ||
        display_phase.find("HTTP ") != std::string::npos) {
        display_phase = job.failed ? "Download blocked by provider" : "Provider request failed";
    }
    const char* current_item = job.current_item.c_str();
    double rate = job.bytes_per_second;
    double eta = job.eta_seconds;
    
    // Negative progress is intentionally indeterminate; never show a
    // misleading negative percentage to the player.
    char overlay[32]{};
    const char* overlay_text = nullptr;
    if (progress >= 0.0f) {
        snprintf(overlay, sizeof(overlay), "%.0f%%", progress * 100.0f);
        overlay_text = overlay;
    }
    progress_bar(progress, ImVec2(ImGui::GetContentRegionAvail().x, 0), overlay_text, color);
    
    // Phase and current item
    ImGui::Spacing();
    ImGui::PushFont(f_small);
    if (!display_phase.empty())
        ImGui::TextColored(k.muted, "%s", display_phase.c_str());
    if (current_item[0]) {
        ImGui::SameLine(0, ui_px(8.0f));
        ImGui::TextDisabled("• %s", current_item);
    }
    ImGui::PopFont();
    
    // Rate and ETA
    if (rate > 0.0 || eta >= 0.0) {
        ImGui::Spacing();
        ImGui::PushFont(f_small);
        if (rate > 0.0) {
            ImGui::TextColored(k.muted, "%s/s", format_rate(rate).c_str());
            ImGui::SameLine(0, ui_px(16.0f));
        }
        if (eta >= 0.0) {
            ImGui::TextColored(k.muted, "ETA: %s", format_eta(eta).c_str());
        }
        ImGui::PopFont();
    }
    
    // Bytes progress
    if (job.bytes_total > 0) {
        ImGui::Spacing();
        ImGui::PushFont(f_small);
        ImGui::TextColored(k.muted, "%s / %s", 
                          format_bytes(job.bytes_done).c_str(),
                          format_bytes(job.bytes_total).c_str());
        ImGui::PopFont();
    }
}

// ---------------------------------------------------------------------------
// Settings search filter
// ---------------------------------------------------------------------------

bool settings_search_filter(const char* search_query, const char* setting_name,
                           const char* setting_description) {
    if (!search_query || search_query[0] == '\0') return true;
    const std::string query = search_query;
    return ui_model::contains_case_insensitive(setting_name ? setting_name : "", query) ||
           ui_model::contains_case_insensitive(setting_description ? setting_description : "", query);
}

// ---------------------------------------------------------------------------
// Keyboard shortcut display
// ---------------------------------------------------------------------------

void draw_keyboard_shortcut(const char* shortcut, const char* description) {
    ImGui::PushFont(f_small);
    
    // Shortcut key
    ImGui::PushStyleColor(ImGuiCol_Button, k.surface2);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, k.hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, k.surface2);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ui_px(4.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ui_px(8.0f), ui_px(4.0f)));
    
    ImGui::Button(shortcut);
    
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(3);
    
    ImGui::SameLine(0, ui_px(12.0f));
    ImGui::TextUnformatted(description);
    
    ImGui::PopFont();
}

// ---------------------------------------------------------------------------
// Theme toggle
// ---------------------------------------------------------------------------

struct ThemeState {
    bool dark_mode = true;
};

static ThemeState g_theme_state;

bool draw_theme_toggle() {
    // Use text instead of emoji glyphs here. Font fallback is not guaranteed
    // on clean Windows installs, while the label remains legible at every DPI.
    const char* label = g_theme_state.dark_mode ? "Dark mode" : "Light mode";
    const bool clicked = ghost_button(label, ImVec2(ui_px(122.0f), ui_px(32.0f)));
    draw_tooltip(g_theme_state.dark_mode
        ? "Switch to the light appearance"
        : "Switch to the dark appearance");
    if (clicked) g_theme_state.dark_mode = !g_theme_state.dark_mode;
    return clicked;
}

bool is_dark_mode() {
    return g_theme_state.dark_mode;
}

void set_dark_mode(bool dark) {
    g_theme_state.dark_mode = dark;
}

// ---------------------------------------------------------------------------
// Tooltip helper
// ---------------------------------------------------------------------------

void draw_tooltip(const char* text) {
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// ---------------------------------------------------------------------------
// Status indicator
// ---------------------------------------------------------------------------

void draw_status_indicator(const ImVec4& color, const char* label) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    
    // Draw colored circle
    dl->AddCircleFilled(p + ImVec2(ui_px(6.0f), ui_px(6.0f)), ui_px(4.0f), c32(color));
    
    ImGui::Dummy(ImVec2(ui_px(16.0f), ui_px(12.0f)));
    
    if (label) {
        ImGui::SameLine(0, 0);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ui_px(12.0f));
        ImGui::PushFont(f_small);
        ImGui::TextUnformatted(label);
        ImGui::PopFont();
    }
}

// ---------------------------------------------------------------------------
// Loading spinner
// ---------------------------------------------------------------------------

void draw_loading_spinner(float size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 center = ImGui::GetCursorScreenPos() + ImVec2(size * 0.5f, size * 0.5f);
    float radius = size * 0.4f;
    float time = static_cast<float>(ImGui::GetTime());
    float start_angle = time * 3.0f;
    
    dl->PathArcTo(center, radius, start_angle, start_angle + 2.5f, 32);
    dl->PathStroke(c32(ImVec4(1, 1, 1, 0.8f)), false, ui_px(2.0f));
    
    ImGui::Dummy(ImVec2(size, size));
}

// ---------------------------------------------------------------------------
// Context menu for quick actions
// ---------------------------------------------------------------------------

struct ContextMenuState {
    bool open = false;
    ImVec2 position;
    std::vector<std::pair<std::string, int>> items; // label, id
};

static ContextMenuState g_context_menu;

bool draw_context_menu(int* selected_id) {
    if (!g_context_menu.open) return false;
    
    ImGui::SetNextWindowPos(g_context_menu.position, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui_px(8.0f), ui_px(8.0f)));
    
    bool open = true;
    bool selected = false;
    if (ImGui::Begin("##context_menu", &open, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize)) {
        for (const auto& item : g_context_menu.items) {
            if (ImGui::MenuItem(item.first.c_str())) {
                if (selected_id) *selected_id = item.second;
                g_context_menu.open = false;
                selected = true;
                break;
            }
        }
        
        if (!selected && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            g_context_menu.open = false;
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
    
    if (!open) {
        g_context_menu.open = false;
    }
    
    return selected;
}

void open_context_menu(const ImVec2& position, const std::vector<std::pair<std::string, int>>& items) {
    g_context_menu.open = true;
    g_context_menu.position = position;
    g_context_menu.items = items;
}

// ---------------------------------------------------------------------------
// Enhanced sidebar item
// ---------------------------------------------------------------------------

void draw_sidebar_item(const char* label, const char* icon, bool active, bool available, const char* tooltip) {
    ImVec2 size = ImVec2(ui_px(48.0f), ui_px(48.0f));
    
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, k.brand);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, k.brand_hov);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, k.brand);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    } else if (!available) {
        ImGui::PushStyleColor(ImGuiCol_Button, k.surface);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, k.surface);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, k.surface);
        ImGui::PushStyleColor(ImGuiCol_Text, k.muted);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, k.hover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, k.surface2);
        ImGui::PushStyleColor(ImGuiCol_Text, k.text);
    }
    
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ui_px(8.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    
    ImGui::Button(icon && icon[0] ? icon : label, size);
    
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(4);
    
    if (tooltip) {
        draw_tooltip(tooltip);
    }
    
    if (active) {
        // Draw active indicator
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p_min = ImGui::GetItemRectMin();
        ImVec2 p_max = ImGui::GetItemRectMax();
        dl->AddRectFilled(ImVec2(p_min.x, p_max.y - ui_px(3.0f)), 
                        ImVec2(p_max.x, p_max.y), c32(k.brand), 0);
    }
}

// ---------------------------------------------------------------------------
// Enhanced profile card
// ---------------------------------------------------------------------------

void draw_profile_card_enhanced(const char* name, const char* loader, const char* version, 
                                const char* last_played, int mod_count, bool has_update,
                                const ImVec4& health_color, const char* health_status) {
    card_begin("profile_card", ImVec2(0, 0), true);
    
    ImGui::BeginGroup();
    
    // Profile name
    ImGui::PushFont(f_bold);
    ImGui::TextUnformatted(name);
    ImGui::PopFont();
    
    // Loader and version
    ImGui::PushFont(f_small);
    ImGui::TextColored(k.muted, "%s %s", loader, version);
    ImGui::PopFont();
    
    ImGui::Spacing();
    
    // Stats row
    ImGui::PushFont(f_small);
    ImGui::TextColored(k.muted, "%d mods", mod_count);
    if (last_played && last_played[0]) {
        ImGui::SameLine(0, ui_px(12.0f));
        ImGui::TextColored(k.muted, "• %s", last_played);
    }
    ImGui::PopFont();
    
    // Health badge
    if (health_status) {
        ImGui::Spacing();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 badge_pos = ImGui::GetCursorScreenPos();
        draw_health_badge(dl, badge_pos, health_status, health_color);
        ImGui::Dummy(ImVec2(0, ui_px(24.0f)));
    }
    
    // Update indicator
    if (has_update) {
        ImGui::Spacing();
        ImGui::PushFont(f_small);
        ImGui::TextColored(k.brand, "Update available");
        ImGui::PopFont();
    }
    
    ImGui::EndGroup();
    
    card_end(true);
}

// ---------------------------------------------------------------------------
// Compact sidebar mode
// ---------------------------------------------------------------------------

void draw_sidebar_compact_item(const char* icon, bool active, const char* tooltip) {
    ImVec2 size = ImVec2(ui_px(32.0f), ui_px(32.0f));
    
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, k.brand);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, k.brand_hov);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, k.brand);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, k.hover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, k.surface2);
        ImGui::PushStyleColor(ImGuiCol_Text, k.text);
    }
    
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ui_px(6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
    
    ImGui::Button(icon, size);
    
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    
    if (tooltip) {
        draw_tooltip(tooltip);
    }
}

// ---------------------------------------------------------------------------
// Notification toast
// ---------------------------------------------------------------------------

struct ToastState {
    struct Toast {
        uint64_t id = 0;
        std::string title;
        std::string message;
        ImVec4 color;
        uint64_t created_at = 0;
        float duration = 3.0f;
    };
    uint64_t next_id = 1;
    std::vector<Toast> toasts;
};

static ToastState g_toast_state;

void show_toast(const char* title, const char* message, const ImVec4& color, float duration) {
    ToastState::Toast toast;
    toast.id = g_toast_state.next_id++;
    toast.title = title ? title : "";
    toast.message = message ? message : "";
    toast.color = color;
    toast.created_at = static_cast<uint64_t>(ImGui::GetTime() * 1000);
    toast.duration = std::max(0.1f, duration);
    g_toast_state.toasts.push_back(std::move(toast));
    constexpr size_t kMaxVisibleToasts = 8;
    if (g_toast_state.toasts.size() > kMaxVisibleToasts) {
        g_toast_state.toasts.erase(
            g_toast_state.toasts.begin(),
            g_toast_state.toasts.begin() +
                static_cast<ptrdiff_t>(g_toast_state.toasts.size() - kMaxVisibleToasts));
    }
}

void draw_toasts() {
    if (g_toast_state.toasts.empty()) return;
    
    uint64_t now_ms = static_cast<uint64_t>(ImGui::GetTime() * 1000);
    
    // Remove expired toasts with a fade-out in the last 300ms
    g_toast_state.toasts.erase(
        std::remove_if(g_toast_state.toasts.begin(), g_toast_state.toasts.end(),
            [now_ms](const ToastState::Toast& toast) {
                return now_ms >= toast.created_at &&
                       now_ms - toast.created_at > static_cast<uint64_t>((toast.duration + 0.3f) * 1000);
            }),
        g_toast_state.toasts.end()
    );
    
    if (g_toast_state.toasts.empty()) return;
    
    // Draw toasts with entrance animation and accent strip
    float y_offset = ui_px(16.0f);
    for (const auto& toast : g_toast_state.toasts) {
        const float age_s = static_cast<float>(now_ms - toast.created_at) / 1000.0f;
        // Entrance slide-in (150ms) and fade-out (300ms at end)
        const float entrance_t = std::min(age_s / 0.15f, 1.0f);
        const float remaining = toast.duration - age_s;
        const float fade_t = remaining > 0.0f ? std::min(remaining / 0.3f, 1.0f) : 0.0f;
        const float alpha = std::clamp(entrance_t * fade_t, 0.0f, 1.0f);
        const float slide_x = (1.0f - entrance_t) * ui_px(40.0f);
        
        const float toast_w = ui_px(320.0f);
        const float toast_x = ImGui::GetMainViewport()->WorkPos.x +
                              ImGui::GetMainViewport()->WorkSize.x - toast_w - ui_px(16.0f) + slide_x;
        ImGui::SetNextWindowPos(ImVec2(toast_x,
                                      ImGui::GetMainViewport()->WorkPos.y + y_offset),
                                ImGuiCond_Always);
        
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, ui_px(10.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui_px(14.0f), ui_px(12.0f)));
        // Semi-transparent dark surface with accent overlay
        ImVec4 bg = ImVec4(k.surface2.x, k.surface2.y, k.surface2.z, alpha * 0.97f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, bg);
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(toast.color.x, toast.color.y, toast.color.z, alpha * 0.4f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, ui_px(1.0f));
        
        ImGui::SetNextWindowSizeConstraints(ImVec2(toast_w - ui_px(20.0f), 0),
                                            ImVec2(toast_w, ui_px(200.0f)));
        
        if (ImGui::Begin(("##toast_" + std::to_string(toast.id)).c_str(), nullptr,
                        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | 
                        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize)) {
            // Accent strip on the left
            const ImVec2 tost_pos = ImGui::GetWindowPos();
            const ImVec2 tost_size = ImGui::GetWindowSize();
            ImGui::GetWindowDrawList()->AddRectFilled(
                tost_pos, ImVec2(tost_pos.x + ui_px(3.0f), tost_pos.y + tost_size.y),
                c32(ImVec4(toast.color.x, toast.color.y, toast.color.z, alpha)),
                ui_px(1.5f));
            
            // Title
            ImGui::PushFont(f_bold);
            ImGui::TextColored(ImVec4(toast.color.x, toast.color.y, toast.color.z, alpha),
                              "%s", toast.title.c_str());
            ImGui::PopFont();
            
            if (!toast.message.empty()) {
                ImGui::Spacing();
                ImGui::PushFont(f_small);
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + toast_w - ui_px(46.0f));
                ImGui::TextColored(ImVec4(k.text.x, k.text.y, k.text.z, alpha * 0.85f),
                                  "%s", toast.message.c_str());
                ImGui::PopTextWrapPos();
                ImGui::PopFont();
            }
            
            y_offset += ImGui::GetWindowHeight() + ui_px(10.0f);
        }
        ImGui::End();
        
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(2);
    }
}

// ---------------------------------------------------------------------------
// Search highlight helper
// ---------------------------------------------------------------------------

void draw_search_highlight(const char* text, const char* search_query) {
    if (!search_query || search_query[0] == '\0') {
        ImGui::TextUnformatted(text);
        return;
    }
    
    std::string text_str = text;
    std::string query_lower = search_query;
    const auto lower = [](char value) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
    };
    std::transform(query_lower.begin(), query_lower.end(), query_lower.begin(), lower);
    
    std::string text_lower = text_str;
    std::transform(text_lower.begin(), text_lower.end(), text_lower.begin(), lower);
    
    size_t pos = text_lower.find(query_lower);
    if (pos == std::string::npos) {
        ImGui::TextUnformatted(text);
        return;
    }
    
    // Draw text before match
    ImGui::TextUnformatted(text_str.substr(0, pos).c_str());
    ImGui::SameLine(0, 0);
    
    // Draw highlighted match
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    ImGui::PushStyleColor(ImGuiCol_Button, k.brand);
    ImGui::Button(text_str.substr(pos, query_lower.length()).c_str());
    ImGui::PopStyleColor(2);
    ImGui::SameLine(0, 0);
    
    // Draw text after match
    ImGui::TextUnformatted(text_str.substr(pos + query_lower.length()).c_str());
}

// ---------------------------------------------------------------------------
// V3 Design System Components
// ---------------------------------------------------------------------------

void draw_skeleton_rect(const ImVec2& pos, const ImVec2& size, float rounding) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Subtle pulsing skeleton effect
    const float time = static_cast<float>(ImGui::GetTime());
    const float pulse = 0.5f + 0.3f * std::sin(time * 3.0f);
    dl->AddRectFilled(pos, pos + size, c32(ImVec4(k.surface2.x, k.surface2.y, k.surface2.z, 1.0f)),
                      rounding > 0 ? rounding : ui_px(6.0f));
    // Animated shimmer overlay
    const float shimmer_width = size.x * 0.4f;
    const float shimmer_offset = fmod(time * 60.0f, size.x + shimmer_width * 2) - shimmer_width;
    dl->PushClipRect(pos, pos + size, true);
    dl->AddRectFilled(
        ImVec2(pos.x + shimmer_offset, pos.y),
        ImVec2(pos.x + shimmer_offset + shimmer_width, pos.y + size.y),
        c32(ImVec4(1.0f, 1.0f, 1.0f, 0.03f * pulse)),
        rounding > 0 ? rounding : ui_px(6.0f));
    dl->PopClipRect();
}

void draw_skeleton_circle(const ImVec2& center, float radius) {
    draw_skeleton_rect(ImVec2(center.x - radius, center.y - radius),
                       ImVec2(radius * 2, radius * 2), radius);
}

void draw_skeleton_text(const ImVec2& pos, float width, float height) {
    float h = height > 0.0f ? height : ui_px(14.0f);
    draw_skeleton_rect(pos, ImVec2(width, h), ui_px(4.0f));
}

void draw_section_header(const char* title, const char* subtitle) {
    ImGui::PushFont(f_h2);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    if (subtitle && subtitle[0]) {
        ImGui::PushFont(f_small);
        ImGui::TextColored(k.muted, "%s", subtitle);
        ImGui::PopFont();
    }
    ImGui::Spacing();
}

void draw_status_dot(const ImVec2& center, const ImVec4& color, float radius) {
    if (radius <= 0.0f) radius = ui_px(4.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddCircleFilled(center, radius, c32(color));
    // Subtle glow effect
    dl->AddCircleFilled(center, radius * 1.6f, c32(ImVec4(color.x, color.y, color.z, 0.15f)));
}

void draw_divider(float padding) {
    const float p = padding > 0.0f ? padding : ui_px(4.0f);
    ImGui::Dummy(ImVec2(0, p));
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    ImGui::GetWindowDrawList()->AddLine(pos, ImVec2(pos.x + width, pos.y), c32(k.border));
    ImGui::Dummy(ImVec2(0, p));
}

void draw_meta_line(const char* label, const char* value) {
    ImGui::PushFont(f_small);
    ImGui::TextColored(k.muted, "%s", label);
    ImGui::SameLine(ui_px(100.0f));
    ImGui::TextColored(k.text, "%s", value ? value : "");
    ImGui::PopFont();
}

void draw_meta_line_colored(const char* label, const char* value, const ImVec4& value_color) {
    ImGui::PushFont(f_small);
    ImGui::TextColored(k.muted, "%s", label);
    ImGui::SameLine(ui_px(100.0f));
    ImGui::TextColored(value_color, "%s", value ? value : "");
    ImGui::PopFont();
}

// ---------------------------------------------------------------------------
// Circular progress indicator
// ---------------------------------------------------------------------------

void draw_circle_progress(const ImVec2& center, float radius, float progress,
                          const ImVec4& track_color, const ImVec4& fill_color) {
    static constexpr float kPi = 3.14159265358979323846f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const int segments = 48;
    const float start = -kPi * 0.5f;  // start from top
    const float end = start + std::clamp(progress, 0.0f, 1.0f) * kPi * 2.0f;
    const float thickness = radius * 0.22f;

    // Track
    dl->PathClear();
    dl->PathArcTo(center, radius, 0, kPi * 2.0f, segments);
    dl->PathStroke(c32(track_color), false, thickness);

    // Fill arc
    if (progress > 0.001f) {
        dl->PathClear();
        dl->PathArcTo(center, radius, start, end, segments);
        dl->PathStroke(c32(fill_color), false, thickness);
    }
}

// ---------------------------------------------------------------------------
// Stat card: metric label + large value + optional progress bar
// ---------------------------------------------------------------------------

float draw_stat_card(const char* label, const char* value, float progress,
                     const ImVec4& accent, float width) {
    const float card_w = width > 0.0f ? width : ui_px(160.0f);
    const float pad = ui_px(14.0f);
    const float card_h = progress >= 0.0f ? ui_px(80.0f) : ui_px(56.0f);

    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 sz(card_w, card_h);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Card background
    dl->AddRectFilled(pos, pos + sz, c32(k.surface), ui_px(8.0f));
    dl->AddRect(pos, pos + sz, c32(k.border), ui_px(8.0f));

    // Top accent line
    ImVec4 ac = (accent.x < 0.0f) ? k.brand : accent;
    dl->AddRectFilled(
        ImVec2(pos.x + pad, pos.y),
        ImVec2(pos.x + sz.x - pad, pos.y + ui_px(3.0f)),
        c32(ac), ui_px(1.5f));

    // Label
    ImGui::PushFont(f_small);
    dl->AddText(pos + ImVec2(pad, ui_px(10.0f)), c32(k.muted), label);
    ImGui::PopFont();

    // Value (large)
    ImGui::PushFont(f_bold);
    ImVec2 text_sz = ImGui::CalcTextSize(value);
    dl->AddText(pos + ImVec2(pad, ui_px(28.0f)), c32(k.text), value);
    ImGui::PopFont();

    // Progress bar
    if (progress >= 0.0f) {
        float bar_y = pos.y + sz.y - ui_px(16.0f);
        float bar_x = pos.x + pad;
        float bar_w = sz.x - pad * 2.0f;
        float bar_h = ui_px(5.0f);

        // Track
        dl->AddRectFilled(
            ImVec2(bar_x, bar_y),
            ImVec2(bar_x + bar_w, bar_y + bar_h),
            c32(k.surface2), bar_h * 0.5f);
        // Fill
        float fill_w = bar_w * std::clamp(progress, 0.0f, 1.0f);
        ImVec4 bar_color = progress > 0.9f ? k.red :
                           progress > 0.7f ? k.orange : ac;
        dl->AddRectFilled(
            ImVec2(bar_x, bar_y),
            ImVec2(bar_x + fill_w, bar_y + bar_h),
            c32(bar_color), bar_h * 0.5f);
    }

    ImGui::Dummy(sz);
    return sz.y;
}

// ---------------------------------------------------------------------------
// Vector icon system
// ---------------------------------------------------------------------------

void draw_icon(IconId icon, const ImVec2& center, float radius, ImU32 color) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float line = std::max(1.4f, radius * 0.18f);
    const ImVec2 c = center;
    const float r = radius;

    auto arc = [&](const ImVec2& p, float radius_arc, float a0, float a1, int seg) {
        dl->PathClear();
        dl->PathArcTo(p, radius_arc, a0, a1, seg);
        dl->PathStroke(color, false, line);
    };

    switch (icon) {
        case IconId::Play:
            dl->AddTriangleFilled(
                ImVec2(c.x - r * 0.55f, c.y - r),
                ImVec2(c.x - r * 0.55f, c.y + r),
                ImVec2(c.x + r * 0.9f, c.y),
                color);
            break;
        case IconId::Pause:
            dl->AddRectFilled(ImVec2(c.x - r * 0.6f, c.y - r), ImVec2(c.x - r * 0.15f, c.y + r), color, r * 0.15f);
            dl->AddRectFilled(ImVec2(c.x + r * 0.15f, c.y - r), ImVec2(c.x + r * 0.6f, c.y + r), color, r * 0.15f);
            break;
        case IconId::Stop:
            dl->AddRectFilled(ImVec2(c.x - r * 0.6f, c.y - r * 0.6f), ImVec2(c.x + r * 0.6f, c.y + r * 0.6f), color, r * 0.15f);
            break;
        case IconId::Restart:
            arc(c, r * 0.75f, -1.2f, 4.7f, 24);
            dl->AddTriangleFilled(
                ImVec2(c.x + r * 0.72f, c.y - r * 0.62f),
                ImVec2(c.x + r * 0.98f, c.y - r * 0.22f),
                ImVec2(c.x + r * 0.42f, c.y - r * 0.28f),
                color);
            break;
        case IconId::Open:
            dl->AddRect(ImVec2(c.x - r * 0.6f, c.y - r * 0.65f), ImVec2(c.x + r * 0.6f, c.y + r * 0.65f), color, r * 0.12f, 0, line);
            dl->AddLine(ImVec2(c.x + r * 0.6f, c.y + r * 0.65f), ImVec2(c.x + r * 1.0f, c.y + r * 1.0f), color, line);
            dl->AddLine(ImVec2(c.x + r * 0.1f, c.y + r * 0.65f), ImVec2(c.x + r * 1.0f, c.y + r * 1.0f), color, line);
            dl->AddLine(ImVec2(c.x + r * 1.0f, c.y + r * 0.5f), ImVec2(c.x + r * 1.0f, c.y + r * 1.0f), color, line);
            break;
        case IconId::Folder:
            dl->PathArcTo(ImVec2(c.x - r * 0.6f, c.y - r * 0.2f), r * 0.3f, 3.14f, 4.7f, 8);
            dl->PathLineTo(ImVec2(c.x - r * 0.6f, c.y + r * 0.55f));
            dl->PathLineTo(ImVec2(c.x + r * 0.6f, c.y + r * 0.55f));
            dl->PathLineTo(ImVec2(c.x + r * 0.6f, c.y - r * 0.55f));
            dl->PathLineTo(ImVec2(c.x - r * 0.6f, c.y - r * 0.55f));
            dl->PathStroke(color, false, line);
            break;
        case IconId::Trash:
            dl->AddRect(ImVec2(c.x - r * 0.6f, c.y - r * 0.3f), ImVec2(c.x + r * 0.6f, c.y + r * 0.75f), color, r * 0.1f, 0, line);
            dl->AddLine(ImVec2(c.x - r * 0.8f, c.y - r * 0.6f), ImVec2(c.x + r * 0.8f, c.y - r * 0.6f), color, line);
            dl->AddLine(ImVec2(c.x - r * 0.3f, c.y - r * 0.9f), ImVec2(c.x + r * 0.3f, c.y - r * 0.9f), color, line);
            dl->AddLine(ImVec2(c.x - r * 0.35f, c.y), ImVec2(c.x - r * 0.35f, c.y + r * 0.55f), color, line);
            dl->AddLine(ImVec2(c.x, c.y), ImVec2(c.x, c.y + r * 0.55f), color, line);
            dl->AddLine(ImVec2(c.x + r * 0.35f, c.y), ImVec2(c.x + r * 0.35f, c.y + r * 0.55f), color, line);
            break;
        case IconId::Refresh: {
            arc(c, r * 0.7f, 0.3f, 5.0f, 24);
            dl->AddTriangleFilled(
                ImVec2(c.x + r * 0.75f, c.y - r * 0.55f),
                ImVec2(c.x + r * 1.02f, c.y - r * 0.2f),
                ImVec2(c.x + r * 0.45f, c.y - r * 0.2f),
                color);
            break;
        }
        case IconId::Check:
            dl->AddLine(ImVec2(c.x - r * 0.6f, c.y), ImVec2(c.x - r * 0.15f, c.y + r * 0.6f), color, line * 1.15f);
            dl->AddLine(ImVec2(c.x - r * 0.15f, c.y + r * 0.6f), ImVec2(c.x + r * 0.75f, c.y - r * 0.6f), color, line * 1.15f);
            break;
        case IconId::Close:
            dl->AddLine(ImVec2(c.x - r * 0.6f, c.y - r * 0.6f), ImVec2(c.x + r * 0.6f, c.y + r * 0.6f), color, line);
            dl->AddLine(ImVec2(c.x - r * 0.6f, c.y + r * 0.6f), ImVec2(c.x + r * 0.6f, c.y - r * 0.6f), color, line);
            break;
        case IconId::Search:
            dl->AddCircle(ImVec2(c.x - r * 0.2f, c.y - r * 0.2f), r * 0.55f, color, 24, line);
            dl->AddLine(ImVec2(c.x + r * 0.3f, c.y + r * 0.3f), ImVec2(c.x + r * 0.95f, c.y + r * 0.95f), color, line * 1.2f);
            break;
        case IconId::Download:
            dl->AddLine(ImVec2(c.x, c.y - r * 0.8f), ImVec2(c.x, c.y + r * 0.4f), color, line);
            dl->AddLine(ImVec2(c.x - r * 0.55f, c.y + r * 0.05f), ImVec2(c.x, c.y + r * 0.5f), color, line);
            dl->AddLine(ImVec2(c.x + r * 0.55f, c.y + r * 0.05f), ImVec2(c.x, c.y + r * 0.5f), color, line);
            dl->AddLine(ImVec2(c.x - r * 0.8f, c.y + r * 0.85f), ImVec2(c.x + r * 0.8f, c.y + r * 0.85f), color, line);
            break;
        case IconId::Upload:
            dl->AddLine(ImVec2(c.x, c.y + r * 0.8f), ImVec2(c.x, c.y - r * 0.4f), color, line);
            dl->AddLine(ImVec2(c.x - r * 0.55f, c.y - r * 0.05f), ImVec2(c.x, c.y - r * 0.5f), color, line);
            dl->AddLine(ImVec2(c.x + r * 0.55f, c.y - r * 0.05f), ImVec2(c.x, c.y - r * 0.5f), color, line);
            dl->AddLine(ImVec2(c.x - r * 0.8f, c.y - r * 0.85f), ImVec2(c.x + r * 0.8f, c.y - r * 0.85f), color, line);
            break;
        case IconId::Pin:
            dl->AddCircle(ImVec2(c.x, c.y - r * 0.5f), r * 0.42f, color, 16, line);
            dl->AddLine(ImVec2(c.x, c.y - r * 0.08f), ImVec2(c.x, c.y + r * 0.7f), color, line);
            dl->AddLine(ImVec2(c.x - r * 0.4f, c.y + r * 0.7f), ImVec2(c.x + r * 0.4f, c.y + r * 0.7f), color, line);
            dl->AddCircleFilled(ImVec2(c.x, c.y - r * 0.5f), r * 0.22f, color);
            break;
        case IconId::Heart:
            dl->AddBezierCubic(
                ImVec2(c.x, c.y + r * 0.85f),
                ImVec2(c.x - r * 1.1f, c.y + r * 0.05f),
                ImVec2(c.x - r * 0.5f, c.y - r * 0.85f),
                ImVec2(c.x, c.y - r * 0.35f),
                color, line);
            dl->AddBezierCubic(
                ImVec2(c.x, c.y + r * 0.85f),
                ImVec2(c.x + r * 1.1f, c.y + r * 0.05f),
                ImVec2(c.x + r * 0.5f, c.y - r * 0.85f),
                ImVec2(c.x, c.y - r * 0.35f),
                color, line);
            break;
        case IconId::Users: {
            const float kPi = 3.14159265358979323846f;
            dl->AddCircle(ImVec2(c.x - r * 0.35f, c.y - r * 0.35f), r * 0.35f, color, 16, line);
            dl->PathArcTo(ImVec2(c.x - r * 0.35f, c.y + r * 0.1f), r * 0.55f, 0.35f, kPi - 0.35f, 16);
            dl->PathStroke(color, false, line);
            dl->AddCircle(ImVec2(c.x + r * 0.4f, c.y - r * 0.2f), r * 0.28f, color, 16, line);
            dl->PathArcTo(ImVec2(c.x + r * 0.4f, c.y + r * 0.1f), r * 0.45f, 0.45f, kPi - 0.45f, 16);
            dl->PathStroke(color, false, line);
            break;
        }
        case IconId::User: {
            const float kPi = 3.14159265358979323846f;
            dl->AddCircle(ImVec2(c.x, c.y - r * 0.35f), r * 0.38f, color, 16, line);
            dl->PathArcTo(ImVec2(c.x, c.y + r * 0.15f), r * 0.62f, 0.3f, kPi - 0.3f, 16);
            dl->PathStroke(color, false, line);
            break;
        }
        case IconId::Server:
            for (int i = -1; i <= 1; ++i) {
                const float y = c.y + static_cast<float>(i) * r * 0.55f;
                dl->AddRectFilled(ImVec2(c.x - r, y - r * 0.22f), ImVec2(c.x + r, y + r * 0.22f), color, r * 0.2f);
                dl->AddCircleFilled(ImVec2(c.x + r * 0.4f, y), r * 0.12f, IM_COL32(0,0,0,160));
            }
            break;
        case IconId::Shield:
            dl->AddLine(ImVec2(c.x - r * 0.7f, c.y - r * 0.5f), ImVec2(c.x - r * 0.7f, c.y + r * 0.3f), color, line);
            dl->AddLine(ImVec2(c.x - r * 0.7f, c.y - r * 0.5f), ImVec2(c.x + r * 0.7f, c.y - r * 0.5f), color, line);
            dl->AddLine(ImVec2(c.x + r * 0.7f, c.y - r * 0.5f), ImVec2(c.x + r * 0.7f, c.y + r * 0.3f), color, line);
            dl->AddLine(ImVec2(c.x - r * 0.7f, c.y + r * 0.3f), ImVec2(c.x, c.y + r * 0.8f), color, line);
            dl->AddLine(ImVec2(c.x + r * 0.7f, c.y + r * 0.3f), ImVec2(c.x, c.y + r * 0.8f), color, line);
            break;
        case IconId::Edit:
            dl->AddLine(ImVec2(c.x - r * 0.7f, c.y + r * 0.7f), ImVec2(c.x + r * 0.4f, c.y - r * 0.4f), color, line);
            dl->AddLine(ImVec2(c.x + r * 0.4f, c.y - r * 0.4f), ImVec2(c.x + r * 0.75f, c.y - r * 0.05f), color, line);
            dl->AddLine(ImVec2(c.x + r * 0.4f, c.y - r * 0.4f), ImVec2(c.x + r * 0.05f, c.y - r * 0.75f), color, line);
            dl->AddLine(ImVec2(c.x - r * 0.7f, c.y + r * 0.7f), ImVec2(c.x - r * 0.95f, c.y + r * 0.95f), color, line);
            break;
        case IconId::Duplicate:
            dl->AddRect(ImVec2(c.x - r * 0.65f, c.y - r * 0.65f), ImVec2(c.x + r * 0.35f, c.y + r * 0.35f), color, r * 0.1f, 0, line);
            dl->AddRect(ImVec2(c.x - r * 0.15f, c.y - r * 0.15f), ImVec2(c.x + r * 0.85f, c.y + r * 0.85f), color, r * 0.1f, 0, line);
            break;
        case IconId::Export:
            dl->AddLine(ImVec2(c.x, c.y + r * 0.7f), ImVec2(c.x, c.y - r * 0.35f), color, line);
            dl->AddLine(ImVec2(c.x - r * 0.55f, c.y - r * 0.05f), ImVec2(c.x, c.y - r * 0.5f), color, line);
            dl->AddLine(ImVec2(c.x + r * 0.55f, c.y - r * 0.05f), ImVec2(c.x, c.y - r * 0.5f), color, line);
            dl->AddLine(ImVec2(c.x - r * 0.8f, c.y + r * 0.85f), ImVec2(c.x + r * 0.8f, c.y + r * 0.85f), color, line);
            break;
        case IconId::Import:
            dl->AddLine(ImVec2(c.x, c.y - r * 0.7f), ImVec2(c.x, c.y + r * 0.35f), color, line);
            dl->AddLine(ImVec2(c.x - r * 0.55f, c.y + r * 0.05f), ImVec2(c.x, c.y + r * 0.5f), color, line);
            dl->AddLine(ImVec2(c.x + r * 0.55f, c.y + r * 0.05f), ImVec2(c.x, c.y + r * 0.5f), color, line);
            dl->AddLine(ImVec2(c.x - r * 0.8f, c.y - r * 0.85f), ImVec2(c.x + r * 0.8f, c.y - r * 0.85f), color, line);
            break;
        case IconId::Settings:
            dl->AddCircle(ImVec2(c.x, c.y), r * 0.4f, color, 20, line);
            dl->AddCircle(ImVec2(c.x, c.y), r * 0.85f, color, 20, line * 0.6f);
            for (int i = 0; i < 8; ++i) {
                const float a = i * 3.14159265358979323846f * 0.25f;
                dl->AddCircleFilled(ImVec2(c.x + std::cos(a) * r * 0.62f, c.y + std::sin(a) * r * 0.62f),
                                    r * 0.14f, color);
            }
            break;
        case IconId::Bell:
            dl->AddCircle(ImVec2(c.x, c.y - r * 0.1f), r * 0.45f, color, 16, line);
            dl->AddRectFilled(ImVec2(c.x - r * 0.5f, c.y + r * 0.25f), ImVec2(c.x + r * 0.5f, c.y + r * 0.5f), color, r * 0.1f);
            dl->AddCircleFilled(ImVec2(c.x, c.y + r * 0.72f), r * 0.13f, color);
            break;
        case IconId::Info:
            dl->AddCircle(ImVec2(c.x, c.y), r * 0.85f, color, 24, line);
            dl->AddCircleFilled(ImVec2(c.x, c.y - r * 0.25f), r * 0.1f, color);
            dl->AddLine(ImVec2(c.x, c.y - r * 0.1f), ImVec2(c.x, c.y + r * 0.35f), color, line);
            break;
        case IconId::Warning:
            dl->AddTriangle(ImVec2(c.x, c.y - r * 0.9f), ImVec2(c.x - r * 0.85f, c.y + r * 0.7f), ImVec2(c.x + r * 0.85f, c.y + r * 0.7f), color);
            dl->AddLine(ImVec2(c.x, c.y - r * 0.35f), ImVec2(c.x, c.y + r * 0.3f), color, line);
            dl->AddCircleFilled(ImVec2(c.x, c.y + r * 0.55f), r * 0.09f, color);
            break;
        case IconId::Error:
            dl->AddCircle(ImVec2(c.x, c.y), r * 0.85f, color, 24, line);
            dl->AddLine(ImVec2(c.x - r * 0.4f, c.y - r * 0.4f), ImVec2(c.x + r * 0.4f, c.y + r * 0.4f), color, line);
            dl->AddLine(ImVec2(c.x - r * 0.4f, c.y + r * 0.4f), ImVec2(c.x + r * 0.4f, c.y - r * 0.4f), color, line);
            break;
        case IconId::Globe:
            dl->AddCircle(ImVec2(c.x, c.y), r * 0.85f, color, 24, line);
            dl->AddEllipse(ImVec2(c.x, c.y), ImVec2(r * 0.85f, r * 0.35f), color,
                           0.0f, 24, line);
            dl->AddLine(ImVec2(c.x, c.y - r * 0.85f), ImVec2(c.x, c.y + r * 0.85f), color, line);
            break;
        case IconId::Cube:
            dl->AddLine(ImVec2(c.x, c.y - r * 0.75f), ImVec2(c.x + r * 0.7f, c.y - r * 0.4f), color, line);
            dl->AddLine(ImVec2(c.x + r * 0.7f, c.y - r * 0.4f), ImVec2(c.x + r * 0.7f, c.y + r * 0.35f), color, line);
            dl->AddLine(ImVec2(c.x + r * 0.7f, c.y + r * 0.35f), ImVec2(c.x, c.y + r * 0.7f), color, line);
            dl->AddLine(ImVec2(c.x, c.y + r * 0.7f), ImVec2(c.x - r * 0.7f, c.y + r * 0.35f), color, line);
            dl->AddLine(ImVec2(c.x - r * 0.7f, c.y + r * 0.35f), ImVec2(c.x - r * 0.7f, c.y - r * 0.4f), color, line);
            dl->AddLine(ImVec2(c.x - r * 0.7f, c.y - r * 0.4f), ImVec2(c.x, c.y - r * 0.75f), color, line);
            dl->AddLine(ImVec2(c.x - r * 0.7f, c.y - r * 0.4f), ImVec2(c.x + r * 0.7f, c.y - r * 0.4f), color, line);
            dl->AddLine(ImVec2(c.x, c.y - r * 0.75f), ImVec2(c.x, c.y - r * 0.1f), color, line);
            break;
        case IconId::Image:
            dl->AddRect(ImVec2(c.x - r * 0.85f, c.y - r * 0.7f), ImVec2(c.x + r * 0.85f, c.y + r * 0.7f), color, r * 0.12f, 0, line);
            dl->AddCircleFilled(ImVec2(c.x - r * 0.3f, c.y - r * 0.25f), r * 0.12f, color);
            dl->AddLine(ImVec2(c.x - r * 0.75f, c.y + r * 0.6f), ImVec2(c.x - r * 0.1f, c.y), color, line);
            dl->AddLine(ImVec2(c.x - r * 0.1f, c.y), ImVec2(c.x + r * 0.3f, c.y + r * 0.4f), color, line);
            dl->AddLine(ImVec2(c.x + r * 0.3f, c.y + r * 0.4f), ImVec2(c.x + r * 0.75f, c.y - r * 0.1f), color, line);
            break;
        case IconId::Log:
            dl->AddRect(ImVec2(c.x - r * 0.8f, c.y - r * 0.7f), ImVec2(c.x + r * 0.8f, c.y + r * 0.7f), color, r * 0.12f, 0, line);
            for (int i = -1; i <= 1; ++i) {
                const float y = c.y + static_cast<float>(i) * r * 0.4f;
                dl->AddLine(ImVec2(c.x - r * 0.5f, y), ImVec2(c.x + r * 0.2f, y), color, line * 0.7f);
                dl->AddCircleFilled(ImVec2(c.x + r * 0.45f, y), r * 0.1f, color);
            }
            break;
        case IconId::File: {
            dl->PathLineTo(ImVec2(c.x - r * 0.55f, c.y + r * 0.8f));
            dl->PathLineTo(ImVec2(c.x - r * 0.55f, c.y - r * 0.8f));
            dl->PathLineTo(ImVec2(c.x + r * 0.05f, c.y - r * 0.8f));
            dl->PathLineTo(ImVec2(c.x + r * 0.6f, c.y - r * 0.25f));
            dl->PathLineTo(ImVec2(c.x + r * 0.6f, c.y + r * 0.8f));
            dl->PathStroke(color, ImDrawFlags_Closed, line);
            dl->AddLine(ImVec2(c.x + r * 0.05f, c.y - r * 0.8f), ImVec2(c.x + r * 0.05f, c.y - r * 0.25f), color, line);
            dl->AddLine(ImVec2(c.x + r * 0.05f, c.y - r * 0.25f), ImVec2(c.x + r * 0.6f, c.y - r * 0.25f), color, line);
            for (int i = -1; i <= 1; ++i) {
                const float y = c.y + static_cast<float>(i) * r * 0.35f;
                dl->AddLine(ImVec2(c.x - r * 0.35f, y), ImVec2(c.x + r * 0.35f, y), color, line * 0.7f);
            }
            break;
        }
        case IconId::Cloud:
            dl->AddCircle(ImVec2(c.x - r * 0.35f, c.y - r * 0.1f), r * 0.45f, color, 20, line);
            dl->AddCircle(ImVec2(c.x + r * 0.2f, c.y - r * 0.35f), r * 0.4f, color, 20, line);
            dl->AddCircle(ImVec2(c.x + r * 0.45f, c.y + r * 0.05f), r * 0.35f, color, 20, line);
            dl->AddRectFilled(ImVec2(c.x - r * 0.8f, c.y + r * 0.05f), ImVec2(c.x + r * 0.8f, c.y + r * 0.55f), color, r * 0.25f);
            break;
        case IconId::Lock:
            dl->AddRect(ImVec2(c.x - r * 0.6f, c.y - r * 0.15f), ImVec2(c.x + r * 0.6f, c.y + r * 0.8f), color, r * 0.12f, 0, line);
            dl->PathArcTo(ImVec2(c.x, c.y - r * 0.1f), r * 0.4f, 3.14159265358979323846f, 6.283185307179586f, 12);
            dl->PathStroke(color, false, line);
            dl->AddCircleFilled(ImVec2(c.x, c.y + r * 0.3f), r * 0.12f, color);
            break;
        case IconId::Star:
            for (int i = 0; i < 5; ++i) {
                const float a0 = -3.14159265358979323846f * 0.5f + i * 3.14159265358979323846f * 0.4f;
                const float a1 = a0 + 3.14159265358979323846f * 0.2f;
                const float x0 = c.x + std::cos(a0) * r;
                const float y0 = c.y + std::sin(a0) * r;
                const float x1 = c.x + std::cos(a1) * r * 0.45f;
                const float y1 = c.y + std::sin(a1) * r * 0.45f;
                const float x2 = c.x + std::cos(a0 + 3.14159265358979323846f * 0.4f) * r;
                const float y2 = c.y + std::sin(a0 + 3.14159265358979323846f * 0.4f) * r;
                dl->AddTriangleFilled(ImVec2(x0, y0), ImVec2(x1, y1), ImVec2(x2, y2), color);
            }
            break;
        case IconId::Plus:
            dl->AddLine(ImVec2(c.x - r * 0.7f, c.y), ImVec2(c.x + r * 0.7f, c.y), color, line * 1.3f);
            dl->AddLine(ImVec2(c.x, c.y - r * 0.7f), ImVec2(c.x, c.y + r * 0.7f), color, line * 1.3f);
            break;
        case IconId::ChevronRight:
            dl->AddLine(ImVec2(c.x - r * 0.35f, c.y - r * 0.7f), ImVec2(c.x + r * 0.35f, c.y), color, line * 1.2f);
            dl->AddLine(ImVec2(c.x - r * 0.35f, c.y + r * 0.7f), ImVec2(c.x + r * 0.35f, c.y), color, line * 1.2f);
            break;
        case IconId::ChevronDown:
            dl->AddLine(ImVec2(c.x - r * 0.7f, c.y - r * 0.35f), ImVec2(c.x, c.y + r * 0.35f), color, line * 1.2f);
            dl->AddLine(ImVec2(c.x + r * 0.7f, c.y - r * 0.35f), ImVec2(c.x, c.y + r * 0.35f), color, line * 1.2f);
            break;
        case IconId::More:
            for (int i = -1; i <= 1; ++i)
                dl->AddCircleFilled(ImVec2(c.x + static_cast<float>(i) * r * 0.6f, c.y), r * 0.16f, color);
            break;
        case IconId::Link:
            dl->AddLine(ImVec2(c.x - r * 0.75f, c.y), ImVec2(c.x + r * 0.75f, c.y), color, line);
            dl->AddLine(ImVec2(c.x - r * 0.75f, c.y), ImVec2(c.x - r * 0.3f, c.y - r * 0.55f), color, line);
            dl->AddLine(ImVec2(c.x - r * 0.75f, c.y), ImVec2(c.x - r * 0.3f, c.y + r * 0.55f), color, line);
            dl->AddLine(ImVec2(c.x + r * 0.75f, c.y), ImVec2(c.x + r * 0.3f, c.y - r * 0.55f), color, line);
            dl->AddLine(ImVec2(c.x + r * 0.75f, c.y), ImVec2(c.x + r * 0.3f, c.y + r * 0.55f), color, line);
            break;
        case IconId::Copy:
            dl->AddRect(ImVec2(c.x - r * 0.3f, c.y - r * 0.75f), ImVec2(c.x + r * 0.7f, c.y + r * 0.25f), color, r * 0.1f, 0, line);
            dl->AddRect(ImVec2(c.x - r * 0.7f, c.y - r * 0.25f), ImVec2(c.x + r * 0.3f, c.y + r * 0.75f), color, r * 0.1f, 0, line);
            break;
        case IconId::Wrench:
            dl->AddCircle(ImVec2(c.x - r * 0.2f, c.y + r * 0.3f), r * 0.45f, color, 20, line);
            dl->AddLine(ImVec2(c.x + r * 0.15f, c.y - r * 0.05f), ImVec2(c.x + r * 0.9f, c.y - r * 0.8f), color, line * 1.4f);
            dl->AddLine(ImVec2(c.x + r * 0.9f, c.y - r * 0.8f), ImVec2(c.x + r * 1.1f, c.y - r * 0.6f), color, line * 1.4f);
            break;
        case IconId::Rocket:
            dl->AddTriangleFilled(
                ImVec2(c.x, c.y - r * 0.95f),
                ImVec2(c.x - r * 0.5f, c.y + r * 0.3f),
                ImVec2(c.x + r * 0.5f, c.y + r * 0.3f),
                color);
            dl->AddCircleFilled(ImVec2(c.x, c.y + r * 0.15f), r * 0.22f, IM_COL32(0,0,0,140));
            dl->AddCircleFilled(ImVec2(c.x, c.y + r * 0.15f), r * 0.13f, color);
            dl->AddLine(ImVec2(c.x - r * 0.25f, c.y + r * 0.4f), ImVec2(c.x - r * 0.5f, c.y + r * 0.95f), color, line);
            dl->AddLine(ImVec2(c.x + r * 0.25f, c.y + r * 0.4f), ImVec2(c.x + r * 0.5f, c.y + r * 0.95f), color, line);
            break;
        case IconId::Power:
            dl->AddLine(ImVec2(c.x, c.y - r * 0.8f), ImVec2(c.x, c.y - r * 0.1f), color, line * 1.3f);
            dl->PathArcTo(ImVec2(c.x, c.y + r * 0.15f), r * 0.65f, 0.5f, 5.8f, 20);
            dl->PathStroke(color, false, line);
            break;
        case IconId::Book:
            dl->AddRectFilled(ImVec2(c.x - r * 0.75f, c.y - r * 0.7f), ImVec2(c.x + r * 0.75f, c.y + r * 0.7f), color, r * 0.1f);
            dl->AddRectFilled(ImVec2(c.x - r * 0.5f, c.y - r * 0.5f), ImVec2(c.x, c.y + r * 0.5f), IM_COL32(0,0,0,120));
            dl->AddLine(ImVec2(c.x + r * 0.2f, c.y - r * 0.4f), ImVec2(c.x + r * 0.55f, c.y - r * 0.4f), IM_COL32(0,0,0,120), line * 0.6f);
            dl->AddLine(ImVec2(c.x + r * 0.2f, c.y), ImVec2(c.x + r * 0.55f, c.y), IM_COL32(0,0,0,120), line * 0.6f);
            break;
        case IconId::Gift:
            dl->AddRectFilled(ImVec2(c.x - r * 0.8f, c.y - r * 0.2f), ImVec2(c.x + r * 0.8f, c.y + r * 0.75f), color, r * 0.1f);
            dl->AddRectFilled(ImVec2(c.x - r * 0.35f, c.y - r * 0.75f), ImVec2(c.x + r * 0.35f, c.y + r * 0.75f), color, r * 0.1f);
            dl->AddLine(ImVec2(c.x - r * 0.8f, c.y - r * 0.75f), ImVec2(c.x + r * 0.8f, c.y - r * 0.75f), IM_COL32(0,0,0,120), line * 0.7f);
            break;
        case IconId::Sparkle:
            for (int i = 0; i < 4; ++i) {
                const float a = i * 3.14159265358979323846f * 0.5f;
                const float x = c.x + std::cos(a) * r * 0.7f;
                const float y = c.y + std::sin(a) * r * 0.7f;
                dl->AddTriangleFilled(
                    ImVec2(x, y - r * 0.28f),
                    ImVec2(x - r * 0.14f, y),
                    ImVec2(x + r * 0.14f, y),
                    color);
            }
            dl->AddCircleFilled(c, r * 0.22f, color);
            break;
        default:
            dl->AddCircle(ImVec2(c.x, c.y), r, color, 16, line);
            break;
    }
}

bool icon_button(IconId icon, const ImVec2& size, const char* tooltip,
                 const ImVec4& color, bool disabled) {
    ImGui::PushID((int)icon);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, k.hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, k.sel);
    ImGui::PushStyleColor(ImGuiCol_Text, color.x < 0.0f ? k.muted : color);
    bool clicked = false;
    if (disabled) ImGui::BeginDisabled();
    if (ImGui::Button("", size)) clicked = true;
    if (disabled) ImGui::EndDisabled();
    const ImVec2 center = ImGui::GetItemRectMin() + ImVec2(size.x * 0.5f, size.y * 0.5f);
    draw_icon(icon, center, std::min(size.x, size.y) * 0.32f,
              c32(disabled ? k.muted : (color.x < 0.0f ? k.text : color)));
    if (tooltip && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tooltip);
    }
    ImGui::PopStyleColor(4);
    ImGui::PopID();
    return clicked && !disabled;
}

// ---------------------------------------------------------------------------
// Server card component
// ---------------------------------------------------------------------------
void draw_server_card(const char* name, const char* address, const char* software,
                      const char* version, int players, int max_players,
                      const ImVec4& status_color, const char* status_text,
                      bool selected, float width) {
    const float h = ui_px(64.0f);
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImVec2 p1(p0.x + width, p0.y + h);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Card background
    dl->AddRectFilled(p0, p1, c32(selected ? k.surface2 : k.surface), ui_px(8.0f));
    if (selected)
        dl->AddRect(p0, p1, c32(k.brand_hov), ui_px(8.0f), 0, ui_px(1.5f));

    // Status strip
    dl->AddRectFilled(p0, ImVec2(p0.x + ui_px(4.0f), p1.y), c32(status_color), ui_px(2.0f));

    // Name
    ImGui::PushFont(f_bold);
    dl->AddText(ImVec2(p0.x + ui_px(12.0f), p0.y + ui_px(6.0f)), c32(k.text),
                elide_to_width(name, width - ui_px(120.0f)).c_str());
    ImGui::PopFont();

    // Address
    dl->AddText(ImVec2(p0.x + ui_px(12.0f), p0.y + ui_px(24.0f)), c32(k.muted), address);

    // Software badge
    if (software && *software) {
        ImVec2 badge_pos(p0.x + ui_px(12.0f), p0.y + ui_px(42.0f));
        draw_badge(dl, badge_pos, software, k.brand, ImVec4(k.brand.x, k.brand.y, k.brand.z, 0.12f));
    }

    // Version
    if (version && *version) {
        ImVec2 badge_pos(p0.x + ui_px(80.0f), p0.y + ui_px(42.0f));
        draw_badge(dl, badge_pos, version, k.blue, ImVec4(k.blue.x, k.blue.y, k.blue.z, 0.12f));
    }

    // Players
    char players_buf[32];
    std::snprintf(players_buf, sizeof(players_buf), "%d/%d", players, max_players);
    ImVec2 pl_pos(p1.x - ui_px(60.0f), p0.y + ui_px(8.0f));
    dl->AddText(f_small, f_small->LegacySize, pl_pos, c32(k.muted), players_buf);

    // Status
    dl->AddText(f_small, f_small->LegacySize,
                ImVec2(p1.x - ui_px(60.0f), p0.y + ui_px(24.0f)),
                c32(status_color), status_text);

    // Clickable
    ImGui::SetCursorScreenPos(p0);
    ImGui::InvisibleButton((std::string("##sv_") + name).c_str(), ImVec2(width, h));
}

// ---------------------------------------------------------------------------
// Confirmation dialog
// ---------------------------------------------------------------------------
bool draw_confirmation_dialog(const char* title, const char* message,
                               const char* confirm_label, const char* cancel_label,
                               const ImVec4& confirm_color, bool* open) {
    if (!*open) return false;

    ImGui::OpenPopup(title);
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ui_px(400), 0), ImGuiCond_Appearing);

    bool confirmed = false;
    if (ImGui::BeginPopupModal(title, open,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextWrapped("%s", message);
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        float btn_w = (ImGui::GetContentRegionAvail().x - ui_px(12.0f)) / 2.0f;
        ImGui::PushStyleColor(ImGuiCol_Button, confirm_color);
        if (ImGui::Button(confirm_label, ImVec2(btn_w, ui_px(32.0f)))) {
            confirmed = true;
            *open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor();
        ImGui::SameLine(0, ui_px(12.0f));
        if (ImGui::Button(cancel_label, ImVec2(btn_w, ui_px(32.0f)))) {
            *open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    return confirmed;
}

// ---------------------------------------------------------------------------
// Error state
// ---------------------------------------------------------------------------
void draw_error_state(const char* title, const char* message,
                      const char* retry_label, bool* retry_flag) {
    const float center_x = ImGui::GetContentRegionAvail().x * 0.5f;
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ui_px(40.0f));

    // Error icon (triangle)
    ImVec2 icon_pos(center_x - ui_px(20.0f), ImGui::GetCursorScreenPos().y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p1(icon_pos.x + ui_px(20.0f), icon_pos.y);
    ImVec2 p2(icon_pos.x - ui_px(16.0f), icon_pos.y + ui_px(30.0f));
    ImVec2 p3(icon_pos.x + ui_px(56.0f), icon_pos.y + ui_px(30.0f));
    dl->AddTriangleFilled(p1, p2, p3, c32(k.red));
    dl->AddText(ImVec2(icon_pos.x + ui_px(14.0f), icon_pos.y + ui_px(6.0f)),
                c32(k.text), "!");
    ImGui::Dummy(ImVec2(ui_px(56.0f), ui_px(36.0f)));

    // Title
    ImGui::SetCursorPosX(center_x - ImGui::CalcTextSize(title).x * 0.5f);
    ImGui::PushFont(f_h2);
    ImGui::TextColored(k.red, "%s", title);
    ImGui::PopFont();
    ImGui::Spacing();

    // Message
    ImVec2 msg_sz = ImGui::CalcTextSize(message);
    float max_w = std::min(ImGui::GetContentRegionAvail().x - ui_px(40.0f), ui_px(400.0f));
    ImGui::SetCursorPosX(center_x - max_w * 0.5f);
    ImGui::PushTextWrapPos(center_x + max_w * 0.5f);
    ImGui::TextColored(k.muted, "%s", message);
    ImGui::PopTextWrapPos();
    ImGui::Spacing();

    // Retry button
    if (retry_label && retry_flag) {
        float btn_w = ui_px(140.0f);
        ImGui::SetCursorPosX(center_x - btn_w * 0.5f);
        ImGui::PushStyleColor(ImGuiCol_Button, k.brand);
        if (ImGui::Button(retry_label, ImVec2(btn_w, ui_px(32.0f)))) {
            *retry_flag = true;
        }
        ImGui::PopStyleColor();
    }
}

}  // namespace aml::ui
