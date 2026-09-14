#pragma once

#include "ai_core.h"

#include <filesystem>
#include <string>
#include <vector>

namespace aml::ai {

// ──────────────────────────────────────────────
// Checkpoint Manager — safe undo for AI work
// ──────────────────────────────────────────────

struct Checkpoint {
    std::string id;
    std::string label;
    int64_t     created_at = 0;
    std::wstring dir;        // checkpoint directory
    std::vector<std::string> changed_files;
    std::string profile_id;
};

// Root directory for checkpoints of a profile.
std::wstring checkpoint_dir(const std::wstring& profile_root);

// Create a snapshot of the current profile state. Copies files (not worlds,
// not large mods unless they are small) into a versioned checkpoint dir.
bool create_checkpoint(const std::wstring& profile_root, const std::string& label,
                       Checkpoint& out, std::string* err);

// List existing checkpoints for a profile, newest first.
std::vector<Checkpoint> list_checkpoints(const std::wstring& profile_root);

// Restore a checkpoint: copies its files back over the profile. Does not
// delete files that were added after the checkpoint unless they are also in
// the checkpoint (that is a safe partial-undo policy).
bool restore_checkpoint(const std::wstring& profile_root, const Checkpoint& cp,
                        std::string* err);

// Prune old checkpoints, keeping the newest `keep` ones.
bool prune_checkpoints(const std::wstring& profile_root, int keep, std::string* err);

}  // namespace aml::ai