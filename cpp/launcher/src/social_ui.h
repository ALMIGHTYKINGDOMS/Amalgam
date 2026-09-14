#pragma once

#include "config.h"
#include "ui_model.h"

namespace aml::ui {

// Social UI functions (surfaces merged into the Essentials page)
void draw_social_messages(UiState& st);
void draw_social_parties(UiState& st);

// Deprecated standalone social page; kept for source compatibility only.
void draw_social_page(UiState& st);

// Writes the launcher-owned friends, servers and entitlement-derived cosmetic
// catalog into a selected profile's private client bridge directory.
bool write_client_bridge(const config::Config& cfg, const std::wstring& target_dir,
                         std::string* error = nullptr);

}  // namespace aml::ui
