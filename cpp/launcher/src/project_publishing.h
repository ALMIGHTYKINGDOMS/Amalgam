#pragma once

#include "supabase.h"

#include <cstdint>
#include <string>
#include <vector>

namespace aml::publishing {

using supabase::SupabaseClient;

struct ProjectDraft {
    std::string name;
    std::string slug;
    std::string description;
    std::string type = "modpack";
    std::string license = "All Rights Reserved";
    std::vector<std::string> tags;
};

struct Project {
    std::string id;
    std::string owner_id;
    std::string name;
    std::string slug;
    std::string description;
    std::string type;
    std::string status;
    bool approved = false;
    std::string current_version_id;
};

struct Version {
    std::string id;
    std::string project_id;
    std::string version;
    std::string changelog;
    std::string artifact_path;
    std::string artifact_sha256;
    int64_t artifact_size = 0;
    std::string status;
};

struct Media {
    std::string project_id;
    std::string version_id;
    std::string kind; // icon, banner, gallery, video
    std::string storage_path;
    std::string mime_type;
    int64_t size_bytes = 0;
    std::string sha256;
    std::string title;
};

bool create_project(SupabaseClient& client, const ProjectDraft& draft,
                    Project& out, std::string* error = nullptr);
bool list_owned_projects(SupabaseClient& client, std::vector<Project>& out,
                         std::string* error = nullptr);
bool submit_for_review(SupabaseClient& client, const std::string& project_id,
                       std::string* error = nullptr);
bool create_version(SupabaseClient& client, const Version& version,
                    Version& out, std::string* error = nullptr);
bool publish_version(SupabaseClient& client, const std::string& project_id,
                     const std::string& version_id, std::string* error = nullptr);
bool add_media(SupabaseClient& client, const Media& media, std::string* error = nullptr);

bool list_moderation_queue(SupabaseClient& client, std::vector<Project>& out,
                           std::string* error = nullptr);
bool review_project(SupabaseClient& client, const std::string& project_id,
                    const std::string& decision, const std::string& reason,
                    std::string* error = nullptr);

} // namespace aml::publishing
