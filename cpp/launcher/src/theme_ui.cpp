#include "ui.h"
#include "ui_internal.h"
#include "ui_motion.h"
#include "config.h"
#include "ui_model.h"
#include "entitlements.h"

#include <windows.h>
#include <algorithm>
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
    
    // Fonts
    int selected_font_index = 0;
    float font_size = 14.0f;
    
    // Accessibility
    bool high_contrast_mode = false;
    bool reduced_motion = false;
    bool color_blind_mode = false;
    std::string color_blind_type;
    
    // Localization
    std::string selected_language;
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

struct Language {
    std::string code;
    std::string name;
    std::string native_name;
    bool is_rtl;
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
            {"background", ImVec4(0.06f, 0.06f, 0.06f, 1.00f)},
            {"background_secondary", ImVec4(0.12f, 0.12f, 0.12f, 1.00f)},
            {"text", ImVec4(0.90f, 0.90f, 0.90f, 1.00f)},
            {"text_muted", ImVec4(0.60f, 0.60f, 0.60f, 1.00f)},
            {"accent", ImVec4(0.20f, 0.60f, 1.00f, 1.00f)},
            {"accent_secondary", ImVec4(0.40f, 0.80f, 1.00f, 1.00f)},
            {"success", ImVec4(0.20f, 0.80f, 0.40f, 1.00f)},
            {"warning", ImVec4(1.00f, 0.80f, 0.20f, 1.00f)},
            {"error", ImVec4(1.00f, 0.30f, 0.30f, 1.00f)},
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

// ---------------------------------------------------------------------------
// Available Languages
// ---------------------------------------------------------------------------

const std::vector<Language> kAvailableLanguages = {
    {"en", "English", "English", false},
    {"es", "Spanish", "Español", false},
    {"fr", "French", "Français", false},
    {"de", "German", "Deutsch", false},
    {"it", "Italian", "Italiano", false},
    {"pt", "Portuguese", "Português", false},
    {"ru", "Russian", "Русский", false},
    {"zh", "Chinese", "中文", true},
    {"ja", "Japanese", "日本語", false},
    {"ko", "Korean", "한국어", false},
};

// ---------------------------------------------------------------------------
// Color Blind Types
// ---------------------------------------------------------------------------

const std::vector<std::string> kColorBlindTypes = {
    "Deuteranopia (Red-Green)",
    "Protanopia (Red-Green)",
    "Tritanopia (Blue-Yellow)",
    "Achromatopsia (Monochrome)"
};

void apply_theme(const ThemePreset& theme);

// ---------------------------------------------------------------------------
// Themes Tab
// ---------------------------------------------------------------------------

void draw_theme_themes(UiState& st) {
    auto& theme_ui = get_theme_ui_state();
    config::Config& c = *st.cfg;
    set_reduced_motion(theme_ui.reduced_motion);
    
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
                    apply_theme(theme);
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
                apply_theme(theme);
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
        st.settings_dirty = true;
    }
    
    card_end();
    
    ImGui::Spacing();
    
    // Build edit_theme from the current theme, then overlay custom colors from config.
    static ThemePreset edit_theme;
    static std::string loaded_theme;
    if (loaded_theme != c.theme) {
        edit_theme = kAvailableThemes[0];
        for (const auto& theme : kAvailableThemes) {
            if (theme.id == c.theme) { edit_theme = theme; break; }
        }
        // Apply saved custom colors
        for (auto& kv : edit_theme.colors) {
            auto it = c.theme_custom_colors.find(kv.first);
            if (it != c.theme_custom_colors.end() && it->second.size() >= 7) {
                auto hex = [](char c) -> unsigned char {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
                    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
                    return 0;
                };
                const std::string& h = it->second;
                kv.second = ImVec4(hex(h[1]) / 255.0f, hex(h[2]) / 255.0f,
                                   hex(h[3]) / 255.0f, h.size() >= 9 ? hex(h[5]) / 255.0f : 1.0f);
            }
        }
        loaded_theme = c.theme;
    }

    auto save_color = [&](const char* key, ImVec4& col) {
        if (ImGui::ColorEdit3(key, reinterpret_cast<float*>(&col),
                              ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "#%02x%02x%02xff",
                          static_cast<int>(col.x * 255), static_cast<int>(col.y * 255),
                          static_cast<int>(col.z * 255));
            c.theme_custom_colors[key] = buf;
            st.settings_dirty = true;
        }
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
        st.settings_dirty = true;
    }
    
    card_end();
    
    ImGui::Spacing();
    
    card_begin("##theme_fonts_size");
    ImGui::TextUnformatted("Font Size");
    ImGui::Separator();
    ImGui::Spacing();
    
    ImGui::TextUnformatted("Base Font Size");
    ImGui::SetNextItemWidth(ui_px(100.0f));
    if (ImGui::SliderFloat("##theme_font_size", &c.theme_font_size, 8.0f, 24.0f, "%.1f px")) {
        st.settings_dirty = true;
    }
    
    ImGui::Spacing();
    
    ImGui::TextUnformatted("Preview");
    ImGui::TextColored(k.muted, "This is a sample text with the current font settings.");
    ImGui::TextColored(k.muted, "The quick brown fox jumps over the lazy dog.");
    
    card_end();
    
    ImGui::Spacing();
    
    card_begin("##theme_fonts_family");
    ImGui::TextUnformatted("Font Family");
    ImGui::Separator();
    ImGui::Spacing();
    
    ImGui::TextColored(k.muted, "The beta uses the bundled launcher font family for consistent rendering.");
    ImGui::TextColored(k.muted, "Font family switching is intentionally disabled in this build.");
    
    card_end();
}

// ---------------------------------------------------------------------------
// Accessibility Tab
// ---------------------------------------------------------------------------

void draw_theme_accessibility(UiState& st) {
    auto& theme_ui = get_theme_ui_state();
    config::Config& c = *st.cfg;
    
    page_title("Accessibility", "Configure accessibility options for better usability");
    
    card_begin("##theme_accessibility_vision");
    ImGui::TextUnformatted("Vision");
    ImGui::Separator();
    ImGui::Spacing();
    
    ImGui::Checkbox("High Contrast Mode", &theme_ui.high_contrast_mode);
    ImGui::TextColored(k.muted, "Increase contrast for better visibility");
    
    ImGui::Spacing();
    
    ImGui::Checkbox("Color Blind Mode", &theme_ui.color_blind_mode);
    
    if (theme_ui.color_blind_mode) {
        ImGui::SetNextItemWidth(ui_px(200.0f));
        if (ImGui::BeginCombo("##theme_color_blind_type", 
                            theme_ui.color_blind_type.empty() ? 
                            "Select type" : theme_ui.color_blind_type.c_str())) {
            for (const auto& type : kColorBlindTypes) {
                if (ImGui::Selectable(type.c_str(), theme_ui.color_blind_type == type)) {
                    theme_ui.color_blind_type = type;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::TextColored(k.muted, "Adjust colors for color blindness");
    }
    
    card_end();
    
    ImGui::Spacing();
    
    card_begin("##theme_accessibility_motion");
    ImGui::TextUnformatted("Motion");
    ImGui::Separator();
    ImGui::Spacing();
    
    if (ImGui::Checkbox("Reduced Motion", &theme_ui.reduced_motion)) {
        set_reduced_motion(theme_ui.reduced_motion);
    }
    ImGui::TextColored(k.muted, "Reduce animations and transitions");
    
    card_end();
    
    ImGui::Spacing();
    
    card_begin("##theme_accessibility_general");
    ImGui::TextUnformatted("General");
    ImGui::Separator();
    ImGui::Spacing();
    
    ImGui::Checkbox("Keyboard Navigation", &c.keyboard_navigation);
    ImGui::TextColored(k.muted, "Enable enhanced keyboard navigation");
    
    ImGui::Spacing();
    
    ImGui::Checkbox("Screen Reader Support", &c.screen_reader_support);
    ImGui::TextColored(k.muted, "Enable screen reader compatibility features");
    
    card_end();
}

// ---------------------------------------------------------------------------
// Localization Tab
// ---------------------------------------------------------------------------

void draw_theme_localization(UiState& st) {
    auto& theme_ui = get_theme_ui_state();
    config::Config& c = *st.cfg;
    
    page_title("Localization", "Configure language and regional settings");
    
    card_begin("##theme_localization_language");
    ImGui::TextUnformatted("Language");
    ImGui::Separator();
    ImGui::Spacing();
    
    ImGui::TextUnformatted("Select Language");
    ImGui::SetNextItemWidth(ui_px(200.0f));
    
    // Find current language
    std::string current_lang_name = "English";
    for (const auto& lang : kAvailableLanguages) {
        if (lang.code == c.language) {
            current_lang_name = lang.native_name;
            break;
        }
    }
    
    if (ImGui::BeginCombo("##theme_language", current_lang_name.c_str())) {
        for (const auto& lang : kAvailableLanguages) {
            if (ImGui::Selectable(lang.native_name.c_str(), c.language == lang.code)) {
                c.language = lang.code;
                theme_ui.selected_language = lang.code;
                save_ui_config(st);
            }
        }
        ImGui::EndCombo();
    }
    
    ImGui::Spacing();
    ImGui::TextColored(k.muted, "Language changes will take effect after restart");
    
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
        }
        if (ImGui::Selectable("DD/MM/YYYY", c.date_format == "DD/MM/YYYY")) {
            c.date_format = "DD/MM/YYYY";
        }
        if (ImGui::Selectable("YYYY-MM-DD", c.date_format == "YYYY-MM-DD")) {
            c.date_format = "YYYY-MM-DD";
        }
        ImGui::EndCombo();
    }
    
    ImGui::Spacing();
    
    ImGui::TextUnformatted("Time Format");
    ImGui::SetNextItemWidth(ui_px(200.0f));
    if (ImGui::BeginCombo("##theme_time_format", c.time_format.c_str())) {
        if (ImGui::Selectable("12-hour", c.time_format == "12-hour")) {
            c.time_format = "12-hour";
        }
        if (ImGui::Selectable("24-hour", c.time_format == "24-hour")) {
            c.time_format = "24-hour";
        }
        ImGui::EndCombo();
    }
    
    card_end();
}

// ---------------------------------------------------------------------------
// Theme Helper Functions
// ---------------------------------------------------------------------------

void apply_theme(const ThemePreset& theme) {
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

    ImGuiStyle& style = ImGui::GetStyle();
    style.Colors[ImGuiCol_WindowBg] = k.bg;
    style.Colors[ImGuiCol_PopupBg] = k.surface;
    style.Colors[ImGuiCol_Border] = k.border;
    style.Colors[ImGuiCol_FrameBg] = k.surface2;
    style.Colors[ImGuiCol_FrameBgActive] = k.brand_dk;
    style.Colors[ImGuiCol_TitleBg] = k.sidebar;
    style.Colors[ImGuiCol_TitleBgActive] = k.sidebar;
    style.Colors[ImGuiCol_Header] = k.sel;
    style.Colors[ImGuiCol_HeaderHovered] = k.hover;
    style.Colors[ImGuiCol_HeaderActive] = k.brand_dk;
    style.Colors[ImGuiCol_Button] = k.surface2;
    style.Colors[ImGuiCol_ButtonHovered] = k.hover;
    style.Colors[ImGuiCol_ButtonActive] = k.brand_dk;
    style.Colors[ImGuiCol_CheckMark] = k.brand;
    style.Colors[ImGuiCol_SliderGrab] = k.brand;
    style.Colors[ImGuiCol_SliderGrabActive] = k.brand_hov;
    style.Colors[ImGuiCol_TextSelectedBg] = k.sel;
    apply_scrollbar_style();
    style.Colors[ImGuiCol_Separator] = k.border;
    style.Colors[ImGuiCol_Tab] = k.surface;
    style.Colors[ImGuiCol_TabHovered] = k.hover;
    style.Colors[ImGuiCol_TabActive] = k.sel;
    style.Colors[ImGuiCol_Text] = k.text;
    style.Colors[ImGuiCol_TextDisabled] = k.muted;
}

// ---------------------------------------------------------------------------
// Main Theme Page
// ---------------------------------------------------------------------------

void draw_theme_page(UiState& st) {
    auto& theme_ui = get_theme_ui_state();
    
    page_title("Theme & Accessibility", "Customize the appearance and accessibility of your launcher");
    
    // Theme tabs
    if (ImGui::BeginTabBar("##theme_tabs")) {
    
    if (ImGui::BeginTabItem("Themes")) {
        theme_ui.current_tab = 0;
        ImGui::EndTabItem();
    }
    
    if (ImGui::BeginTabItem("Colors")) {
        theme_ui.current_tab = 1;
        ImGui::EndTabItem();
    }
    
    if (ImGui::BeginTabItem("Fonts")) {
        theme_ui.current_tab = 2;
        ImGui::EndTabItem();
    }
    
    if (ImGui::BeginTabItem("Accessibility")) {
        theme_ui.current_tab = 3;
        ImGui::EndTabItem();
    }
    
    if (ImGui::BeginTabItem("Localization")) {
        theme_ui.current_tab = 4;
        ImGui::EndTabItem();
    }
    
        ImGui::EndTabBar();
    }
    
    ImGui::Spacing();
    
    // Draw current tab
    switch (theme_ui.current_tab) {
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
    const char* theme_ids[] = {"default_dark", "default_light", "midnight", "solarized_dark", "dracula"};
    const char* theme_items = "Default Dark\0Default Light\0Midnight\0Solarized Dark\0Dracula\0";
    int theme_index = 0;
    for (int i = 0; i < 5; ++i) {
        if (c.theme == theme_ids[i]) theme_index = i;
    }
    if (ImGui::Combo("##settings_theme", &theme_index, theme_items, 5)) {
        c.theme = theme_ids[theme_index];
        st.settings_dirty = true;
    }
}

}  // namespace aml::ui
