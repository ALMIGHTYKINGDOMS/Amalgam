#pragma once

#include "ui_state.h"

namespace aml::essentials {

void draw_essentials_tab(aml::ui::UiState& st);
void draw_profile_essentials_actions(aml::ui::UiState& st, const std::string& profile_id);
void draw_essentials_sidebar_indicator(aml::ui::UiState& st);
void draw_essentials_friend_badge(aml::ui::UiState& st);

}  // namespace aml::essentials
