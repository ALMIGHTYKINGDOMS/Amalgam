#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace aml::ai {

// ──────────────────────────────────────────────
// Platform information
// ──────────────────────────────────────────────

struct HardwareInfo {
    std::string gpu_name;
    int64_t vram_mb    = 0;
    int64_t system_ram_mb = 0;
    int     cpu_cores   = 0;
    bool    vulkan_ok   = false;
};

enum class MemoryMode { GPU = 0, Balanced, LowMemory, CPU };

HardwareInfo detect_hardware();
MemoryMode   best_memory_mode(const HardwareInfo& hw);

// ──────────────────────────────────────────────
// AI Model Manifest
// ──────────────────────────────────────────────

struct ModelEntry {
    std::string filename;       // e.g. "Qwen3VL-8B-Instruct-Q4_K_M.gguf"
    std::string sha256;         // expected hex hash
    int64_t     size_bytes = 0;
    std::string hf_repo;        // HuggingFace repo, e.g. "Qwen/Qwen3-VL-8B-Instruct-GGUF"
    std::string hf_file;        // filename in repo
    std::string destination;     // manifest-relative component directory
};

struct AIManifest {
    int schema_version = 1;
    std::string ai_package;     // e.g. "1.0.0"
    std::string min_launcher;
    ModelEntry  brain;
    ModelEntry  vision_projector;
    ModelEntry  art_model;
    ModelEntry  art_encoder;
    ModelEntry  art_decoder;
    std::vector<std::string> licenses;
};

bool            load_manifest(AIManifest& out, std::string* err);
bool            validate_models(const AIManifest& m, const std::wstring& models_dir, std::string* err);

// ──────────────────────────────────────────────
// AI Profile
// ──────────────────────────────────────────────

enum class AIMode { Ask = 0, Build, Agent, Auto };

struct AIProfileState {
    bool    is_ai_profile = false;
    AIMode  mode          = AIMode::Ask;
    bool    live_vision   = false;     // enable window capture
    int64_t last_index_scan = 0;       // timestamp of last profile file scan
};

// ──────────────────────────────────────────────
// Conversation store (per profile)
// ──────────────────────────────────────────────

struct Turn {
    std::string role;      // "user" | "assistant" | "system"
    std::string text;
    int64_t     timestamp = 0;
    // metadata
    std::string plan_id;    // if this turn is part of an AI plan
    bool        checkpointed = false;
};

std::vector<Turn>  load_conversation(const std::wstring& profile_dir);
void               save_conversation(const std::wstring& profile_dir, const std::vector<Turn>& turns);
void               append_turn(const std::wstring& profile_dir, const Turn& turn);

// ──────────────────────────────────────────────
// AI plan — structured modpack plan
// ──────────────────────────────────────────────

struct PlanStep {
    std::string description;
    std::string category;    // "mod", "config", "kubejs", "java", "texture", "quest", etc.
    bool        completed = false;
    std::string artifact_path; // file created
};

struct AIPlan {
    std::string id;
    std::string goal;
    int64_t     created_at = 0;
    std::vector<PlanStep> steps;
};

}  // namespace aml::ai