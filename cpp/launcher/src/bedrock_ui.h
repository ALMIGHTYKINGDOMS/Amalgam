#pragma once

#include "ui.h"
#include "bedrock.h"

namespace aml::ui {

// Bedrock UI functions
void draw_bedrock_tab(UiState& st);
void draw_bedrock_overview(UiState& st);
void draw_bedrock_profiles(UiState& st);
void draw_bedrock_worlds(UiState& st);
void draw_bedrock_backups(UiState& st);
void draw_bedrock_addons(UiState& st);

// Internal functions
void draw_bedrock_create_profile(UiState& st);
void draw_bedrock_edit_profile(UiState& st);
void draw_bedrock_addons_installed(UiState& st);
void draw_bedrock_addons_discover(UiState& st);
void draw_bedrock_addons_import(UiState& st);

// Helper functions
std::string generate_bedrock_id();
std::string format_current_timestamp();
std::string format_bytes(uint64_t bytes);

// Bedrock Stats
struct BedrockStats {
    int total_profiles = 0;
    int favorite_profiles = 0;
    int total_worlds = 0;
    uint64_t total_world_size = 0;
    int total_backups = 0;
    uint64_t total_backup_size = 0;
};

BedrockStats get_bedrock_stats(UiState& st, const std::vector<aml::bedrock::BedrockProfile>& profiles);

}  // namespace aml::ui
