#pragma once

#include "ui.h"

namespace aml::ui {

// Auth Wizard functions
void show_auth_wizard_if_needed(UiState& st);
void draw_auth_wizard(UiState& st);
void draw_microsoft_login_dialog(UiState& st);
void draw_amalgam_login_wizard(UiState& st);
void draw_password_reset_dialog(UiState& st);
void draw_account_button_with_wizard(UiState& st);

}  // namespace aml::ui
