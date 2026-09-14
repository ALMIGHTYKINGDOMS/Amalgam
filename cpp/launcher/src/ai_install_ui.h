#pragma once

namespace aml::ui { struct UiState; }

namespace aml::ai_install_ui {

// Draw the AI model install/verify panel inside the Settings page.
// Replaces the old ShellExecute CMD window approach.
void draw_ai_install_panel(aml::ui::UiState& st);

}  // namespace aml::ai_install_ui
