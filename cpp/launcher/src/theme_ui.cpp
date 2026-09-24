#include "ui.h"
#include "ui_internal.h"
#include "ui_motion.h"
#include "config.h"
#include "ui_model.h"
#include "entitlements.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

namespace aml::ui {

// ---------------------------------------------------------------------------
// Theme UI State
// ---------------------------------------------------------------------------

struct ThemeUIState {
    int current_tab = 0; // 0=themes, 1=colors, 2=fonts, 3=accessibility, 4=localization
    
    // Themes
    std::string selected_theme;
    bool custom_theme = false;
    
    // Colors
    int selected_color_index = 0;
    ImVec4 color_picker;
    
};

static ThemeUIState& get_theme_ui_state() {
    static ThemeUIState state;
    return state;
}

// ---------------------------------------------------------------------------
// Theme Data Structures
// ---------------------------------------------------------------------------

struct ThemePreset {
    std::string id;
    std::string name;
    std::string description;
    std::string author;
    bool is_dark;
    bool premium = false;  // true = requires Amalgam+
    std::map<std::string, ImVec4> colors;
};

// ---------------------------------------------------------------------------
// Available Themes
// ---------------------------------------------------------------------------

const std::vector<ThemePreset> kAvailableThemes = {
    {
        "default_dark",
        "Default Dark",
        "The default dark theme for Amalgam launcher",
        "Amalgam Team",
        true,
        false,
        {
            {"background", ImVec4(0.017f, 0.029f, 0.051f, 1.00f)},
            {"background_secondary", ImVec4(0.032f, 0.052f, 0.086f, 1.00f)},
            {"text", ImVec4(0.93f, 0.94f, 0.97f, 1.00f)},
            {"text_muted", ImVec4(0.60f, 0.67f, 0.77f, 1.00f)},
            {"accent", ImVec4(0.48f, 0.19f, 0.90f, 1.00f)},
            {"accent_secondary", ImVec4(0.72f, 0.43f, 1.00f, 1.00f)},
            {"success", ImVec4(0.11f, 0.86f, 0.42f, 1.00f)},
            {"warning", ImVec4(0.91f, 0.80f, 0.31f, 1.00f)},
            {"error", ImVec4(0.95f, 0.33f, 0.36f, 1.00f)},
        }
    },
    {
        "default_light",
        "Default Light",
        "The default light theme for Amalgam launcher",
        "Amalgam Team",
        false,
        false,
        {
            {"background", ImVec4(0.98f, 0.98f, 0.98f, 1.00f)},
            {"background_secondary", ImVec4(0.95f, 0.95f, 0.95f, 1.00f)},
            {"text", ImVec4(0.10f, 0.10f, 0.10f, 1.00f)},
            {"text_muted", ImVec4(0.40f, 0.40f, 0.40f, 1.00f)},
            {"accent", ImVec4(0.20f, 0.40f, 0.80f, 1.00f)},
            {"accent_secondary", ImVec4(0.40f, 0.60f, 0.90f, 1.00f)},
            {"success", ImVec4(0.20f, 0.60f, 0.40f, 1.00f)},
            {"warning", ImVec4(0.80f, 0.60f, 0.20f, 1.00f)},
            {"error", ImVec4(0.80f, 0.20f, 0.20f, 1.00f)},
        }
    },
    {
        "midnight",
        "Midnight",
        "A dark theme with deep blue accents",
        "Community",
        true,
        true,
        {
            {"background", ImVec4(0.04f, 0.04f, 0.08f, 1.00f)},
            {"background_secondary", ImVec4(0.08f, 0.08f, 0.16f, 1.00f)},
            {"text", ImVec4(0.95f, 0.95f, 1.00f, 1.00f)},
            {"text_muted", ImVec4(0.50f, 0.50f, 0.70f, 1.00f)},
            {"accent", ImVec4(0.30f, 0.60f, 1.00f, 1.00f)},
            {"accent_secondary", ImVec4(0.50f, 0.80f, 1.00f, 1.00f)},
            {"success", ImVec4(0.20f, 0.80f, 0.60f, 1.00f)},
            {"warning", ImVec4(1.00f, 0.80f, 0.40f, 1.00f)},
            {"error", ImVec4(1.00f, 0.40f, 0.40f, 1.00f)},
        }
    },
    {
        "solarized_dark",
        "Solarized Dark",
        "A popular color scheme with precise color relationships",
        "Ethan Schoonover",
        true,
        true,
        {
            {"background", ImVec4(0.00f, 0.16f, 0.22f, 1.00f)},
            {"background_secondary", ImVec4(0.07f, 0.22f, 0.29f, 1.00f)},
            {"text", ImVec4(0.93f, 0.93f, 0.93f, 1.00f)},
            {"text_muted", ImVec4(0.58f, 0.58f, 0.58f, 1.00f)},
            {"accent", ImVec4(0.26f, 0.63f, 0.83f, 1.00f)},
            {"accent_secondary", ImVec4(0.42f, 0.75f, 0.92f, 1.00f)},
            {"success", ImVec4(0.40f, 0.70f, 0.30f, 1.00f)},
            {"warning", ImVec4(0.83f, 0.63f, 0.26f, 1.00f)},
            {"error", ImVec4(0.83f, 0.26f, 0.26f, 1.00f)},
        }
    },
    {
        "dracula",
        "Dracula",
        "A dark theme with a vibrant purple accent",
        "Zeno Rocha",
        true,
        true,
        {
            {"background", ImVec4(0.10f, 0.06f, 0.15f, 1.00f)},
            {"background_secondary", ImVec4(0.18f, 0.12f, 0.25f, 1.00f)},
            {"text", ImVec4(0.95f, 0.95f, 0.95f, 1.00f)},
            {"text_muted", ImVec4(0.50f, 0.50f, 0.60f, 1.00f)},
            {"accent", ImVec4(0.70f, 0.40f, 1.00f, 1.00f)},
            {"accent_secondary", ImVec4(0.80f, 0.60f, 1.00f, 1.00f)},
            {"success", ImVec4(0.40f, 0.80f, 0.40f, 1.00f)},
            {"warning", ImVec4(1.00f, 0.80f, 0.40f, 1.00f)},
            {"error", ImVec4(1.00f, 0.40f, 0.40f, 1.00f)},
        }
    }
};

// A profile is intentionally described by the color pair it separates rather
// than claiming to simulate a clinical condition.  Each profile below changes
// the actual semantic colors the launcher draws.
struct ColorVisionProfile {
    const char* id;
    const char* label;
};

const std::vector<ColorVisionProfile> kColorVisionProfiles = {
    {"red_green", "Red–green distinction"},
    {"blue_yellow", "Blue–yellow distinction"},
};

const ThemePreset& theme_for_id(const std::string& id) {
    for (const auto& theme : kAvailableThemes) {
        if (theme.id == id) return theme;
    }
    return kAvailableThemes.front();
}

int hex_value(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return 10 + value - 'a';
    if (value >= 'A' && value <= 'F') return 10 + value - 'A';
    return -1;
}

bool parse_hex_color(const std::string& value, ImVec4& out) {
    if ((value.size() != 7 && value.size() != 9) || value.front() != '#') return false;
    const auto byte_at = [&value](size_t offset) {
        const int high = hex_value(value[offset]);
        const int low = hex_value(value[offset + 1]);
        return high < 0 || low < 0 ? -1 : high * 16 + low;
    };
    const int red = byte_at(1);
    const int green = byte_at(3);
    const int blue = byte_at(5);
    const int alpha = value.size() == 9 ? byte_at(7) : 255;
    if (red < 0 || green < 0 || blue < 0 || alpha < 0) return false;
    out = ImVec4(red / 255.0f, green / 255.0f, blue / 255.0f, alpha / 255.0f);
    return true;
}

ThemePreset effective_theme(const config::Config& config) {
    ThemePreset result = theme_for_id(config.theme);
    for (auto& color : result.colors) {
        const auto custom = config.theme_custom_colors.find(color.first);
        ImVec4 parsed;
        if (custom != config.theme_custom_colors.end() && parse_hex_color(custom->second, parsed)) {
            color.second = parsed;
        }
    }
    return result;
}

void apply_theme_preset(const ThemePreset& theme);
void apply_high_contrast_palette();
void apply_color_vision_palette(const std::string& profile);

// ---------------------------------------------------------------------------
// Themes Tab
// ---------------------------------------------------------------------------

void draw_theme_themes(UiState& st) {
    auto& theme_ui = get_theme_ui_state();
    config::Config& c = *st.cfg;
    
    page_title("Themes", "Choose and customize your launcher theme");
    
    card_begin("##theme_themes_header", ImVec2(-1, 0));
    
    const float theme_header_available = ImGui::GetContentRegionAvail().x;
    const float theme_action_width = ui_px(170.0f);
    if (theme_header_available > theme_action_width + ui_px(180.0f))
        ImGui::SameLine(ImGui::GetCursorPosX() + theme_header_available - theme_action_width);
    else
        ImGui::Spacing();
    if (primary_button("+ Create Theme", ImVec2(ui_px(150.0f), ui_px(32.0f)))) {
        theme_ui.custom_theme = true;
        theme_ui.current_tab = 1;
    }
    
    card_end();
    
    ImGui::Spacing();
    
    // Theme list
    for (const auto& theme : kAvailableThemes) {
        ImGui::PushID(theme.id.c_str());
        
        card_begin(("##theme_" + theme.id).c_str(), ImVec2(-1, 0));
        
        ImGui::BeginGroup();
        
        // Theme preview
        ImGui::PushStyleColor(ImGuiCol_Button, theme.colors.at("background"));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme.colors.at("background_secondary"));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme.colors.at("background_secondary"));
        
        if (ImGui::Button(("##theme_preview_" + theme.id).c_str(), ImVec2(ui_px(60.0f), ui_px(60.0f)))) {
            // Show tooltip with color swatches
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(theme.name.c_str());
            ImGui::Separator();
            const char* swatch_keys[] = {"background", "background_secondary", "text", "accent", "success", "error"};
            for (const char* key : swatch_keys) {
                auto it = theme.colors.find(key);
                if (it != theme.colors.end()) {
                    ImVec2 sp = ImGui::GetCursorScreenPos();
                    ImGui::GetWindowDrawList()->AddRectFilled(sp, ImVec2(sp.x + 14.0f, sp.y + 14.0f), c32(it->second));
                    ImGui::Dummy(ImVec2(14.0f, 14.0f));
                    ImGui::SameLine();
                    ImGui::Text("%s", key);
                }
            }
            ImGui::EndTooltip();
        }
        
        ImGui::PopStyleColor(3);
        
        ImGui::SameLine();
        
        ImGui::BeginGroup();
            ImGui::Text("%s", theme.name.c_str());
        ImGui::TextColored(k.muted, "%s", theme.description.c_str());
        ImGui::TextColored(k.muted, "By %s", theme.author.c_str());
        
        if (theme.is_dark) {
            ImGui::TextColored(k.muted, "Dark Theme");
        } else {
            ImGui::TextColored(k.muted, "Light Theme");
        }
        
        ImGui::EndGroup();
        
        ImGui::EndGroup();
        
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - ui_px(150.0f));
        
        ImGui::BeginGroup();
        
        // Select button with premium gating
        if (c.theme == theme.id) {
            ImGui::TextColored(k.green, "Selected");
        } else if (theme.premium) {
            auto& theme_ents = aml::entitlements::EntitlementManager::instance();
            if (theme_ents.is_plus()) {
                if (ghost_button("Select", ImVec2(ui_px(100.0f), ui_px(28.0f)))) {
                    c.theme = theme.id;
                    theme_ui.selected_theme = theme.id;
                    apply_configured_theme(c);
                    save_ui_config(st);
                }
            } else {
                ImVec4 plus_bg = k.brand; plus_bg.w = 0.15f;
                ImVec2 bp = ImGui::GetCursorScreenPos();
                draw_badge(ImGui::GetWindowDrawList(), bp, "AMALGAM+", k.brand, plus_bg);
                ImGui::Dummy(ImVec2(
                    ImGui::CalcTextSize("AMALGAM+").x + ui_px(14.0f), ui_px(20.0f)));
            }
        } else {
            if (ghost_button("Select", ImVec2(ui_px(100.0f), ui_px(28.0f)))) {
                c.theme = theme.id;
                theme_ui.selected_theme = theme.id;
                apply_configured_theme(c);
                save_ui_config(st);
            }
        }
        
        ImGui::EndGroup();
        
        card_end();
        ImGui::PopID();
    }
}

// ---------------------------------------------------------------------------
// Colors Tab
// ---------------------------------------------------------------------------

void draw_theme_colors(UiState& st) {
    config::Config& c = *st.cfg;
    
    page_title("Colors", "Customize the color scheme of your launcher");
    
    card_begin("##theme_colors_header");
    
    ImGui::TextUnformatted("Color Customization");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - ui_px(200.0f));
    
    if (ghost_button("Reset to Theme", ImVec2(ui_px(150.0f), ui_px(32.0f)))) {
        c.theme_custom_colors.clear();
        apply_configured_theme(c);
        save_ui_config(st);
    }
    
    card_end();
    
    ImGui::Spacing();
    
    // Rebuild the editing model from persisted data every frame.  This keeps
    // the swatches synchronized with Reset, Reload saved, and the actual
    // palette used by the rest of the launcher.
    ThemePreset edit_theme = effective_theme(c);

    auto save_color = [&](const char* key, ImVec4& col) {
        const bool changed = ImGui::ColorEdit3(key, reinterpret_cast<float*>(&col),
                                                ImGuiColorEditFlags_NoInputs |
                                                    ImGuiColorEditFlags_NoLabel);
        if (changed) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "#%02X%02X%02X",
                          static_cast<int>(std::round(col.x * 255.0f)),
                          static_cast<int>(std::round(col.y * 255.0f)),
                          static_cast<int>(std::round(col.z * 255.0f)));
            c.theme_custom_colors[key] = buf;
            apply_configured_theme(c);
            st.settings_dirty = true;
        }
        // Color pickers emit changes while dragged.  Apply every intermediate
        // shade for a truthful live preview, but persist once the edit ends.
        if (ImGui::IsItemDeactivatedAfterEdit()) save_ui_config(st);
    };
    
    // Color categories
    card_begin("##theme_colors_primary");
    ImGui::TextUnformatted("Primary Colors");
    ImGui::Separator();
    ImGui::Spacing();
    
    ImGui::TextUnformatted("Background");
    ImGui::SameLine();
    save_color("background", edit_theme.colors["background"]);
    
    ImGui::TextUnformatted("Background Secondary");
    ImGui::SameLine();
    save_color("background_secondary", edit_theme.colors["background_secondary"]);
    
    ImGui::Spacing();
    
    ImGui::TextUnformatted("Text");
    ImGui::SameLine();
    save_color("text", edit_theme.colors["text"]);
    
    ImGui::TextUnformatted("Text Muted");
    ImGui::SameLine();
    save_color("text_muted", edit_theme.colors["text_muted"]);
    
    card_end();
    
    ImGui::Spacing();
    
    card_begin("##theme_colors_accent");
    ImGui::TextUnformatted("Accent Colors");
    ImGui::Separator();
    ImGui::Spacing();
    
    ImGui::TextUnformatted("Accent");
    ImGui::SameLine();
    save_color("accent", edit_theme.colors["accent"]);
    
    ImGui::TextUnformatted("Accent Secondary");
    ImGui::SameLine();
    save_color("accent_secondary", edit_theme.colors["accent_secondary"]);
    
    ImGui::Spacing();
    
    ImGui::TextUnformatted("Success");
    ImGui::SameLine();
    save_color("success", edit_theme.colors["success"]);
    
    ImGui::TextUnformatted("Warning");
    ImGui::SameLine();
    save_color("warning", edit_theme.colors["warning"]);
    
    ImGui::TextUnformatted("Error");
    ImGui::SameLine();
    save_color("error", edit_theme.colors["error"]);
    
    card_end();
}

// ---------------------------------------------------------------------------
// Fonts Tab
// ---------------------------------------------------------------------------

void draw_theme_fonts(UiState& st) {
    config::Config& c = *st.cfg;
    
    page_title("Fonts", "Customize the fonts used in the launcher");
    
    card_begin("##theme_fonts_header");
    
    ImGui::TextUnformatted("Font Settings");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - ui_px(200.0f));
    
    if (ghost_button("Reset to Default", ImVec2(ui_px(150.0f), ui_px(32.0f)))) {
        c.theme_font_size = 14.0f;
        apply_configured_theme(c);
        save_ui_config(st);
    }
    
    card_end();
    
    ImGui::Spacing();
    
    card_begin("##theme_fonts_size");
    ImGui::TextUnformatted("Font Size");
    ImGui::Separator();
    ImGui::Spacing();
    
    ImGui::TextUnformatted("Base Font Size");
    ImGui::SetNextItemWidth(ui_px(100.0f));
    const bool font_size_changed = ImGui::SliderFloat("##theme_font_size", &c.theme_font_size,
                                                       12.0f, 20.0f, "%.1f px");
    if (font_size_changed) {
        config::normalize_presentation_preferences(c);
        apply_configured_theme(c);
        st.settings_dirty = true;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) save_ui_config(st);
    
    ImGui::Spacing();
    
    ImGui::TextUnformatted("Preview");
    ImGui::TextColored(k.muted, "This text changes at once and stays selected after a restart.");
    ImGui::TextColored(k.muted, "The quick brown fox jumps over the lazy dog.");
    
    card_end();
    
    ImGui::Spacing();
    
    card_begin("##theme_fonts_family");
    ImGui::TextUnformatted("Font Family");
    ImGui::Separator();
    ImGui::Spacing();
    
    ImGui::TextColored(k.muted, "Amalgam uses the bundled Segoe UI family and CJK fallback for consistent rendering.");
    ImGui::TextColored(k.muted, "Font family switching is not offered because no alternate family is installed or applied by the launcher.");
    
    card_end();
}

// ---------------------------------------------------------------------------
// Accessibility Tab
// ---------------------------------------------------------------------------

void draw_theme_accessibility(UiState& st) {
    config::Config& c = *st.cfg;
    
    page_title("Accessibility", "Configure accessibility options for better usability");
    
    card_begin("##theme_accessibility_vision");
    ImGui::TextUnformatted("Vision");
    ImGui::Separator();
    ImGui::Spacing();
    
    if (ImGui::Checkbox("High-contrast palette", &c.high_contrast_mode)) {
        apply_configured_theme(c);
        save_ui_config(st);
    }
    ImGui::TextColored(k.muted, "Raises text, border, focus, and surface contrast across the launcher.");
    
    ImGui::Spacing();
    
    if (ImGui::Checkbox("Color-vision palette", &c.color_vision_palette)) {
        apply_configured_theme(c);
        save_ui_config(st);
    }
    
    if (c.color_vision_palette) {
        ImGui::SetNextItemWidth(ui_px(200.0f));
        const auto selected = std::find_if(kColorVisionProfiles.begin(), kColorVisionProfiles.end(),
                                           [&c](const ColorVisionProfile& profile) {
                                               return profile.id == c.color_vision_profile;
                                           });
        const char* label = selected != kColorVisionProfiles.end()
            ? selected->label : kColorVisionProfiles.front().label;
        if (ImGui::BeginCombo("##theme_color_vision_profile", label)) {
            for (const auto& profile : kColorVisionProfiles) {
                if (ImGui::Selectable(profile.label, c.color_vision_profile == profile.id)) {
                    c.color_vision_profile = profile.id;
                    apply_configured_theme(c);
                    save_ui_config(st);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::TextColored(k.muted, "Changes semantic status and accent hues; labels and icons remain the primary status signal.");
    }

    if (c.high_contrast_mode || c.color_vision_palette) {
        ImGui::Spacing();
        ImGui::TextColored(k.muted, "Applied semantic palette");
        ImGui::TextColored(k.green, "Success / ready");
        ImGui::SameLine();
        ImGui::TextColored(k.yellow, "Warning / attention");
        ImGui::TextColored(k.red, "Error / blocked");
        ImGui::SameLine();
        ImGui::TextColored(k.blue, "Information");
    }
    
    card_end();
    
    ImGui::Spacing();
    
    card_begin("##theme_accessibility_motion");
    ImGui::TextUnformatted("Motion");
    ImGui::Separator();
    ImGui::Spacing();
    
    if (ImGui::Checkbox("Reduced motion", &c.reduced_motion)) {
        apply_configured_theme(c);
        save_ui_config(st);
    }
    ImGui::TextColored(k.muted, "Snaps launcher transitions and popups to their final state.");
    
    card_end();
    
    ImGui::Spacing();
    
    card_begin("##theme_accessibility_general");
    ImGui::TextUnformatted("General");
    ImGui::Separator();
    ImGui::Spacing();
    
    if (ImGui::Checkbox("Launcher navigation shortcuts", &c.keyboard_navigation)) {
        save_ui_config(st);
    }
    ImGui::TextColored(k.muted, "Enables Ctrl+1–6 and Ctrl+, page shortcuts. Tab and arrow-key focus remain available.");
    
    ImGui::Spacing();
    
    // Dear ImGui currently exposes this launcher as a single native window;
    // advertising an on/off screen-reader switch before control-level
    // semantics exist would be misleading. Keep the limitation explicit while
    // the accessible native control tree is completed.
    ImGui::TextColored(k.yellow, "Screen-reader semantics are not available in this build.");
    ImGui::TextWrapped("The launcher is a single native window and does not yet expose reliable control-by-control screen-reader information. No inactive screen-reader switch is shown as a result.");
    
    card_end();
}

// ---------------------------------------------------------------------------
// Localization Tab
// ---------------------------------------------------------------------------

void draw_theme_localization(UiState& st) {
    config::Config& c = *st.cfg;
    
    page_title("Localization", "Configure language and regional settings");
    
    card_begin("##theme_localization_language");
    ImGui::TextUnformatted("Language");
    ImGui::Separator();
    ImGui::Spacing();
    
    ImGui::TextUnformatted("Launcher interface");
    ImGui::TextColored(k.text, "English");
    ImGui::Spacing();
    ImGui::TextWrapped("The launcher interface currently ships in English. A language selector would not translate this build, so it is intentionally not shown. Account-profile language and optional content translation are managed in their dedicated pages.");
    
    card_end();
    
    ImGui::Spacing();
    
    card_begin("##theme_localization_regional");
    ImGui::TextUnformatted("Regional Settings");
    ImGui::Separator();
    ImGui::Spacing();
    
    ImGui::TextUnformatted("Date Format");
    ImGui::SetNextItemWidth(ui_px(200.0f));
    if (ImGui::BeginCombo("##theme_date_format", c.date_format.c_str())) {
        if (ImGui::Selectable("MM/DD/YYYY", c.date_format == "MM/DD/YYYY")) {
            c.date_format = "MM/DD/YYYY";
            save_ui_config(st);
        }
        if (ImGui::Selectable("DD/MM/YYYY", c.date_format == "DD/MM/YYYY")) {
            c.date_format = "DD/MM/YYYY";
            save_ui_config(st);
        }
        if (ImGui::Selectable("YYYY-MM-DD", c.date_format == "YYYY-MM-DD")) {
            c.date_format = "YYYY-MM-DD";
            save_ui_config(st);
        }
        ImGui::EndCombo();
    }
    
    ImGui::Spacing();
    
    ImGui::TextUnformatted("Time Format");
    ImGui::SetNextItemWidth(ui_px(200.0f));
    if (ImGui::BeginCombo("##theme_time_format", c.time_format.c_str())) {
        if (ImGui::Selectable("12-hour", c.time_format == "12-hour")) {
            c.time_format = "12-hour";
            save_ui_config(st);
        }
        if (ImGui::Selectable("24-hour", c.time_format == "24-hour")) {
            c.time_format = "24-hour";
            save_ui_config(st);
        }
        ImGui::EndCombo();
    }

    ImGui::Spacing();
    std::tm example{};
    example.tm_year = 126; // 2026
    example.tm_mon = 8;    // September
    example.tm_mday = 22;
    example.tm_hour = 15;
    example.tm_min = 7;
    ImGui::TextColored(k.muted, "Applied example: %s",
                       config::format_local_date_time(example, c).c_str());
    
    card_end();
}

// ---------------------------------------------------------------------------
// Theme Helper Functions
// ---------------------------------------------------------------------------

void apply_theme_preset(const ThemePreset& theme) {
    const auto color = [&theme](const char* name) { return theme.colors.at(name); };
    k.bg = color("background");
    k.sidebar = color("background_secondary");
    k.surface = color("background_secondary");
    k.surface2 = color("background_secondary");
    k.border = ImVec4(color("text_muted").x, color("text_muted").y,
                      color("text_muted").z, 0.35f);
    k.text = color("text");
    k.muted = color("text_muted");
    k.brand = color("accent");
    k.brand_hov = color("accent_secondary");
    k.brand_dk = ImVec4(k.brand.x * 0.65f, k.brand.y * 0.65f, k.brand.z * 0.65f, 1.0f);
    k.sel = ImVec4(k.brand.x, k.brand.y, k.brand.z, 0.35f);
    k.hover = color("background_secondary");
    k.red = color("error");
    k.blue = color("accent_secondary");
    k.orange = color("warning");
    k.yellow = color("warning");
    k.green = color("success");

}

void apply_high_contrast_palette() {
    const float brightness = k.bg.x * 0.2126f + k.bg.y * 0.7152f + k.bg.z * 0.0722f;
    const bool light_base = brightness > 0.55f;
    if (light_base) {
        k.bg = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
        k.sidebar = ImVec4(0.94f, 0.94f, 0.94f, 1.0f);
        k.surface = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
        k.surface2 = ImVec4(0.92f, 0.92f, 0.92f, 1.0f);
        k.text = ImVec4(0.03f, 0.03f, 0.03f, 1.0f);
        k.muted = ImVec4(0.18f, 0.18f, 0.18f, 1.0f);
        k.border = ImVec4(0.03f, 0.03f, 0.03f, 1.0f);
        k.hover = ImVec4(0.82f, 0.89f, 1.0f, 1.0f);
        k.sel = ImVec4(0.10f, 0.36f, 0.75f, 0.35f);
        k.brand = ImVec4(0.03f, 0.25f, 0.67f, 1.0f);
        k.brand_hov = ImVec4(0.00f, 0.18f, 0.55f, 1.0f);
    } else {
        k.bg = ImVec4(0.01f, 0.01f, 0.01f, 1.0f);
        k.sidebar = ImVec4(0.035f, 0.035f, 0.035f, 1.0f);
        k.surface = ImVec4(0.055f, 0.055f, 0.055f, 1.0f);
        k.surface2 = ImVec4(0.10f, 0.10f, 0.10f, 1.0f);
        k.text = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
        k.muted = ImVec4(0.88f, 0.88f, 0.88f, 1.0f);
        k.border = ImVec4(0.92f, 0.92f, 0.92f, 1.0f);
        k.hover = ImVec4(0.16f, 0.23f, 0.36f, 1.0f);
        k.sel = ImVec4(0.38f, 0.65f, 1.0f, 0.42f);
        k.brand = ImVec4(0.48f, 0.75f, 1.0f, 1.0f);
        k.brand_hov = ImVec4(0.73f, 0.87f, 1.0f, 1.0f);
    }
    k.brand_dk = ImVec4(k.brand.x * 0.55f, k.brand.y * 0.55f, k.brand.z * 0.55f, 1.0f);
    k.red = ImVec4(0.88f, 0.16f, 0.18f, 1.0f);
    k.orange = ImVec4(0.86f, 0.48f, 0.08f, 1.0f);
    k.yellow = ImVec4(0.75f, 0.63f, 0.02f, 1.0f);
    k.green = ImVec4(0.00f, 0.52f, 0.25f, 1.0f);
    k.blue = k.brand_hov;
}

void apply_color_vision_palette(const std::string& profile) {
    if (profile == "blue_yellow") {
        // Avoid a blue/yellow-only error signal; use violet, teal, and coral
        // semantic hues while preserving explicit labels and icons.
        k.blue = ImVec4(0.67f, 0.46f, 0.94f, 1.0f);
        k.green = ImVec4(0.12f, 0.67f, 0.56f, 1.0f);
        k.yellow = ImVec4(0.91f, 0.40f, 0.29f, 1.0f);
        k.orange = ImVec4(0.91f, 0.40f, 0.29f, 1.0f);
        k.red = ImVec4(0.84f, 0.22f, 0.36f, 1.0f);
    } else {
        // Separate success and error away from a red/green pair: cyan for
        // success, orange for error, and amber for warning.
        k.green = ImVec4(0.12f, 0.70f, 0.84f, 1.0f);
        k.red = ImVec4(0.94f, 0.42f, 0.16f, 1.0f);
        k.orange = ImVec4(0.94f, 0.64f, 0.20f, 1.0f);
        k.yellow = ImVec4(0.88f, 0.72f, 0.22f, 1.0f);
        k.blue = ImVec4(0.38f, 0.62f, 0.95f, 1.0f);
    }
}

void apply_configured_theme(const config::Config& config) {
    config::Config normalized = config;
    config::normalize_presentation_preferences(normalized);
    apply_theme_preset(effective_theme(normalized));
    if (normalized.high_contrast_mode) apply_high_contrast_palette();
    if (normalized.color_vision_palette) apply_color_vision_palette(normalized.color_vision_profile);
    set_reduced_motion(normalized.reduced_motion);
    set_theme_font_size(normalized.theme_font_size);
    // ui.cpp owns the common ImGui style baseline; call it after every palette
    // transformation so startup, reload, and live changes produce identical
    // actual colors, focus outlines, scrollbars, and widget states.
    apply_theme();
}

// ---------------------------------------------------------------------------
// Main Theme Page
// ---------------------------------------------------------------------------

void draw_theme_page(UiState& st) {
    auto& theme_ui = get_theme_ui_state();

    // Snapshot fixtures select a tab explicitly so each visual surface can be
    // reviewed without brittle coordinate clicks. Normal launcher sessions
    // retain Dear ImGui's regular tab-selection behaviour.
    if (st.fixture_mode) {
        // Fixture preferences are applied only to the isolated in-memory
        // fixture config; save_ui_config refuses fixture writes.  These named
        // states make the real palette/motion/localization behavior visible
        // without reading a player's launcher.json.
        config::Config& c = *st.cfg;
        c.high_contrast_mode = st.fixture_case == "theme-accessibility-high-contrast";
        c.color_vision_palette = st.fixture_case == "theme-accessibility-color-vision";
        c.reduced_motion = st.fixture_case == "theme-accessibility-reduced-motion";
        if (c.color_vision_palette) c.color_vision_profile = "red_green";
        if (st.fixture_case == "theme-localization-12-hour") {
            c.date_format = "MM/DD/YYYY";
            c.time_format = "12-hour";
        }
        apply_configured_theme(c);
        if (st.fixture_case == "theme-colors") theme_ui.current_tab = 1;
        else if (st.fixture_case == "theme-fonts") theme_ui.current_tab = 2;
        else if (st.fixture_case == "theme-accessibility" ||
                 st.fixture_case.rfind("theme-accessibility-", 0) == 0)
            theme_ui.current_tab = 3;
        else if (st.fixture_case == "theme-localization" ||
                 st.fixture_case.rfind("theme-localization-", 0) == 0)
            theme_ui.current_tab = 4;
        else if (st.fixture_case == "theme" || st.fixture_case == "theme-themes")
            theme_ui.current_tab = 0;
    }
    
    // Keep the requested fixture tab stable while Dear ImGui walks every tab
    // header below.  Mutating `current_tab` from the first visible header used
    // to erase the requested Accessibility/Localization selection before its
    // later header could receive SetSelected, producing misleading evidence.
    const int requested_fixture_tab = st.fixture_mode ? theme_ui.current_tab : -1;
    const auto fixture_tab_flags = [requested_fixture_tab](int tab) {
        return requested_fixture_tab == tab
            ? ImGuiTabItemFlags_SetSelected
            : ImGuiTabItemFlags_None;
    };

    page_title("Theme & Accessibility", "Customize the appearance and accessibility of your launcher");
    
    // Theme tabs
    if (ImGui::BeginTabBar("##theme_tabs")) {
    
    if (ImGui::BeginTabItem("Themes", nullptr,
                            st.fixture_mode ? fixture_tab_flags(0)
                                            : ImGuiTabItemFlags_None)) {
        if (!st.fixture_mode) theme_ui.current_tab = 0;
        ImGui::EndTabItem();
    }
    
    if (ImGui::BeginTabItem("Colors", nullptr,
                            st.fixture_mode ? fixture_tab_flags(1)
                                            : ImGuiTabItemFlags_None)) {
        if (!st.fixture_mode) theme_ui.current_tab = 1;
        ImGui::EndTabItem();
    }
    
    if (ImGui::BeginTabItem("Fonts", nullptr,
                            st.fixture_mode ? fixture_tab_flags(2)
                                            : ImGuiTabItemFlags_None)) {
        if (!st.fixture_mode) theme_ui.current_tab = 2;
        ImGui::EndTabItem();
    }
    
    if (ImGui::BeginTabItem("Accessibility", nullptr,
                            st.fixture_mode ? fixture_tab_flags(3)
                                            : ImGuiTabItemFlags_None)) {
        if (!st.fixture_mode) theme_ui.current_tab = 3;
        ImGui::EndTabItem();
    }
    
    if (ImGui::BeginTabItem("Localization", nullptr,
                            st.fixture_mode ? fixture_tab_flags(4)
                                            : ImGuiTabItemFlags_None)) {
        if (!st.fixture_mode) theme_ui.current_tab = 4;
        ImGui::EndTabItem();
    }
    
        ImGui::EndTabBar();
    }
    
    ImGui::Spacing();
    
    // Draw current tab
    switch (st.fixture_mode ? requested_fixture_tab : theme_ui.current_tab) {
        case 0:
        default:
            draw_theme_themes(st);
            break;
        case 1:
            draw_theme_colors(st);
            break;
        case 2:
            draw_theme_fonts(st);
            break;
        case 3:
            draw_theme_accessibility(st);
            break;
        case 4:
            draw_theme_localization(st);
            break;
    }
}

void draw_theme_selector_inline(UiState& st, config::Config& c) {
    // Keep this compact control subject to the same entitlement policy as the
    // full Themes page.  It is used in more than one settings surface, so an
    // unrestricted Combo here would otherwise provide a second path around
    // the Amalgam+ selection gate.
    const bool has_plus = aml::entitlements::EntitlementManager::instance().is_plus();
    const ThemePreset* current = &kAvailableThemes.front();
    for (const auto& theme : kAvailableThemes) {
        if (c.theme == theme.id) {
            current = &theme;
            break;
        }
    }

    std::string preview = current->name;
    if (current->premium) preview += "  (Amalgam+)";

    const ImVec2 combo_min = ImGui::GetCursorScreenPos();
    if (st.fixture_mode && st.fixture_theme_selector_open) {
        // Keep the inert evidence route open for the full capture window so
        // the popup's compact geometry is visible without a synthetic click.
        ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    }
    if (ImGui::BeginCombo("##settings_theme", preview.c_str())) {
        for (const auto& theme : kAvailableThemes) {
            const bool locked = theme.premium && !has_plus;
            const bool selected = c.theme == theme.id;
            std::string label = theme.name;
            if (theme.premium) label += "  (Amalgam+)";

            if (locked) ImGui::BeginDisabled();
            if (ImGui::Selectable(label.c_str(), selected) && !locked && !st.fixture_mode) {
                c.theme = theme.id;
                apply_configured_theme(c);
                st.settings_dirty = true;
            }
            if (locked) ImGui::EndDisabled();
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    if (st.fixture_mode && st.fixture_theme_selector_open) {
        // Dear ImGui only opens a combo in response to an item activation. A
        // screenshot runner has no pointer activation, so present the same
        // inert option surface as a tooltip-style popup at the combo's exact
        // location. This keeps evidence deterministic while preserving the
        // real control's labels, premium lock states, and spacing.
        ImGui::SetNextWindowPos(combo_min + ImVec2(0.0f, ui_px(30.0f)), ImGuiCond_Always);
        ImGui::SetNextWindowSizeConstraints(ImVec2(ui_px(210.0f), 0.0f),
                                            ImVec2(ui_px(310.0f), ui_px(260.0f)));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, ui_px(8.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                            ImVec2(ui_px(10.0f), ui_px(8.0f)));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, k.surface2);
        ImGui::PushStyleColor(ImGuiCol_Border, k.border);
        ImGui::Begin("##fixture_theme_picker", nullptr,
                     ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::TextColored(k.muted, "Theme options");
        ImGui::Separator();
        for (const auto& theme : kAvailableThemes) {
            const bool locked = theme.premium && !has_plus;
            const bool selected = c.theme == theme.id;
            if (locked) ImGui::BeginDisabled();
            ImGui::TextColored(selected ? k.brand_hov : k.text, "%s%s",
                              theme.name.c_str(), theme.premium ? "  (Amalgam+)" : "");
            if (locked) ImGui::EndDisabled();
        }
        ImGui::End();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
    }

    if (!has_plus) {
        ImGui::PushStyleColor(ImGuiCol_Text, k.muted);
        ImGui::TextWrapped("Amalgam+ unlocks Midnight, Solarized Dark, and Dracula.");
        ImGui::PopStyleColor();
    }
}

}  // namespace aml::ui
