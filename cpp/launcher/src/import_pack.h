#pragma once

#include "instances.h"
#include "mods.h"

#include <functional>
#include <cstddef>
#include <string>
#include <vector>

namespace aml::import_pack {

using Progress = std::function<bool(float, const std::string&)>;

enum class ChangeKind { Added, Removed, Replaced };

struct ContentChange {
    std::string path;
    ChangeKind kind = ChangeKind::Added;
};

// A bounded, deterministic preview of how a downloaded creator archive would
// affect managed profile content. Runtime-owned data (worlds, screenshots,
// logs, launcher metadata, and Java runtime files) is deliberately excluded.
struct ArchivePreview {
    size_t added = 0;
    size_t removed = 0;
    size_t replaced = 0;
    size_t unchanged = 0;
    bool provider_file_names_resolved = true;
    std::vector<ContentChange> changes;
};

bool archive(const std::wstring& path, const std::wstring& instances_dir, const mods::ApiCfg& api,
             instances::Instance& out, Progress progress, std::string* err = nullptr);
bool archive_into(const std::wstring& path, const instances::Instance& target, const mods::ApiCfg& api,
                  instances::Instance& out, Progress progress, std::string* err = nullptr);
bool preview_archive(const std::wstring& path, const instances::Instance& target,
                     ArchivePreview& out, std::string* err = nullptr);
bool export_mrpack(const instances::Instance& instance, const std::wstring& path,
                   std::string* err = nullptr);
bool export_curseforge(const instances::Instance& instance, const std::wstring& path,
                       std::string* err = nullptr);

}  // namespace aml::import_pack
