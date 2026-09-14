#pragma once

#include "json.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace aml::model {

struct ManifestEntry {
    std::string id;
    std::string type;
    std::string url;
    std::string sha1;
    int64_t size = -1;
};

std::vector<ManifestEntry> fetch_manifest(std::string* err);
bool rules_allow(const Json& j);
int rank(const std::string& id);
int default_java_major(int game_rank);
std::string first_segment(const std::string& id);

struct Download {
    std::string url;
    std::string sha1;
    std::string path;
    int64_t size = -1;
};

struct Lib {
    std::string name;
    std::string maven_url;
    Download artifact;
    std::vector<std::pair<std::string, Download>> natives;
    std::vector<std::string> extract_exclude;
};

struct VersionJson {
    std::string id;
    std::string main_class;
    std::string assets_name;
    Download assets_index;
    Download client;
    std::vector<std::string> jvm_args;
    std::vector<std::string> game_args;
    std::vector<Lib> libraries;
    int java_major = 0;
};

bool parse_version(const Json& j, VersionJson& out);

bool merge_inherited(const std::string& mc_id, const Json& profile, VersionJson* out,
                     std::string* err);

bool resolve_loader_version(const std::string& mc_id, const std::string& loader,
                            std::string* loader_version, std::string* err);

std::string loader_profile_url(const std::string& mc_id, const std::string& loader,
                               const std::string& loader_version);

std::string loader_installer_url(const std::string& loader, const std::string& loader_version);

bool is_installer_loader(const std::string& loader);

}  // namespace aml::model
