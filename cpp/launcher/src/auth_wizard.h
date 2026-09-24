#pragma once

#include "ui.h"
#include "ui_async_request.h"

#include <functional>
#include <string>

namespace aml::ui {

// Auth Wizard functions
void show_auth_wizard_if_needed(UiState& st);
void draw_auth_wizard(UiState& st);
void draw_microsoft_login_dialog(UiState& st);
void draw_amalgam_login_wizard(UiState& st);
void draw_password_reset_dialog(UiState& st);
void draw_account_button_with_wizard(UiState& st);

// Provider-backed account mutations and identity-scoped publisher writes use
// this one UiState-owned lane. Workers are joined during launcher shutdown;
// result application is guarded by both the action name and the monotonic
// request generation.
bool auth_async_request_is_working(const UiState& st, const std::string& action);
bool auth_async_request_lane_busy(const UiState& st);
bool start_auth_async_request(UiState& st, const std::string& action,
                              std::function<AsyncUiRequestResult()> work);
bool take_auth_async_request_result(UiState& st, const std::string& action,
                                    AsyncUiRequestSnapshot* completed);
void invalidate_auth_async_request(UiState& st, const std::string& action);

}  // namespace aml::ui
