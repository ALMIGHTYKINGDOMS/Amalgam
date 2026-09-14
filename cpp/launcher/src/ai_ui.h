#pragma once

#include "ui_state.h"

namespace aml::ai {

// Draw the AI tab for an AI Profile (chat + tools + plan). `profile_id` is
// the instance id; the profile root is derived by the caller.
void draw_ai_profile_tab(aml::ui::UiState& st, const std::string& profile_id,
                         const std::wstring& profile_root);

// Called from the profile creation wizard when "Create AI Profile" is chosen.
void on_create_ai_profile(aml::ui::UiState& st, const std::string& profile_id,
                          const std::wstring& profile_root);

}  // namespace aml::ai