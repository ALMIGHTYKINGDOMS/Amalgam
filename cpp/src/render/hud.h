#pragma once

#include "core/protocol.h"

#include <string>
#include <vector>

namespace aml::hud {

struct Element {
    std::string id;
    std::string name;
    bool enabled = true;
    float x = 20.0f;
    float y = 20.0f;
    float scale = 1.0f;
};

void init(const std::string& path);
void draw(const Snapshot& snapshot);
void draw_editor();
void save();
std::vector<Element> snapshot_elements();
int panel_profile();
void set_panel_profile(int profile);
const char* panel_profile_name(int profile);
bool server_safe();
void set_server_safe(bool enabled);

}  // namespace aml::hud
