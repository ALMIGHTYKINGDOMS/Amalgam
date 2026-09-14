#pragma once

#include "ai_core.h"

#include <functional>
#include <string>
#include <vector>

namespace aml::ai {

// ──────────────────────────────────────────────
// Agent Tool System
// ──────────────────────────────────────────────

// Result of a tool invocation
struct ToolResult {
    bool        ok = false;
    std::string output;       // human-readable result
    std::string error;        // only set on failure
};

// A named tool the AI can request.
struct AgentTool {
    std::string name;
    std::string description;  // shown to the model for tool selection
    std::string parameters;   // JSON Schema (stringified) describing expected params
    std::function<ToolResult(const std::string& json_params)> handler;
};

// The toolset available to an AI profile.
struct ToolSet {
    std::wstring profile_root;   // sandbox root
    std::string  profile_id;
    std::vector<AgentTool> tools;

    void add_read_file();
    void add_write_file();
    void add_search_files();
    void add_get_profile_state();
    void add_search_mods();
    void add_install_mod();
    void add_read_config();
    void add_edit_config();
    void add_read_log();
    void add_read_crash();
    void add_create_checkpoint();
    void add_restore_checkpoint();
    void add_generate_art();
    void add_launch_minecraft();
};

// Generate the OpenAI-compatible function-calling tools array JSON
std::string tools_openai_json(const ToolSet& set);

// Invoke a tool by name
ToolResult invoke_tool(ToolSet& set, const std::string& name, const std::string& json_args);

}  // namespace aml::ai