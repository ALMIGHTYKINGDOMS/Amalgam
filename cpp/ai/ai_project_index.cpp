#include "ai_project_index.h"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>

namespace aml::ai {

namespace {
std::string to_lower_copy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string rel_path(const std::filesystem::path& root, const std::filesystem::path& p) {
    std::error_code ec;
    auto rel = std::filesystem::relative(p, root, ec);
    if (ec) return p.string();
    std::string s = rel.string();
    for (char& c : s) if (c == '\\') c = '/';
    return s;
}

std::string categorize(const std::filesystem::path& p, const std::string& rel) {
    std::string r = to_lower_copy(rel);
    if (r.find("kubejs") != std::string::npos) return "kubejs";
    if (r.find("datapack") != std::string::npos) return "datapack";
    if (r.find("resourcepack") != std::string::npos || r.find("resource_pack") != std::string::npos)
        return "resourcepack";
    if (r.find("config") != std::string::npos) return "config";
    if (r.find("fancymenu") != std::string::npos) return "fancymenu";
    if (r.find("crash-reports") != std::string::npos || r.find("crash") != std::string::npos)
        return "crash";
    if (r.find("logs") != std::string::npos) return "log";
    if (r.find("mods") != std::string::npos) {
        std::string ext = to_lower_copy(p.extension().string());
        if (ext == ".jar") return "mod";
    }
    if (r.find("quest") != std::string::npos) return "quest";
    if (r.find("java") != std::string::npos || r.find(".java") != std::string::npos)
        return "java";
    return "other";
}
}  // namespace

ProfileSnapshot build_profile_index(const std::wstring& profile_root, std::string* err) {
    ProfileSnapshot snap;
    std::error_code ec;
    std::filesystem::path root(profile_root);
    if (!std::filesystem::exists(root, ec)) {
        if (err) *err = "profile root does not exist";
        return snap;
    }

    // Walk the profile tree, skipping huge/irrelevant dirs
    auto skip = [](const std::filesystem::path& p) {
        std::string n = to_lower_copy(p.filename().string());
        return n == ".git" || n == ".cache" || n == "saves" || n == "world" ||
               n == "screenshots" || n == "assets" || n == "cache" ||
               n == "libraries" || n == "versions";
    };

    int file_count = 0;
    for (auto it = std::filesystem::recursive_directory_iterator(
             root, std::filesystem::directory_options::skip_permission_denied, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        const auto& p = it->path();
        if (it->is_directory(ec)) {
            if (skip(p)) it.disable_recursion_pending();
            continue;
        }
        std::string rel = rel_path(root, p);
        auto sz = it->file_size(ec);
        auto mt = it->last_write_time(ec).time_since_epoch().count();
        IndexedFile f;
        f.path = p.wstring();
        f.rel = rel;
        f.category = categorize(p, rel);
        f.size = sz;
        f.mtime = mt;
        snap.files.push_back(std::move(f));
        ++file_count;
        if (file_count > 4000) break;  // safety bound
    }

    // mod detection
    for (auto& f : snap.files) {
        if (f.category == "mod") {
            snap.mod_ids.push_back(f.rel);
            snap.mod_count++;
        }
    }
    return snap;
}

std::vector<IndexedFile> search_index(const ProfileSnapshot& snap, const std::string& needle, int limit) {
    std::vector<IndexedFile> out;
    std::string q = to_lower_copy(needle);
    for (const auto& f : snap.files) {
        if (out.size() >= static_cast<size_t>(limit)) break;
        std::string rel = to_lower_copy(f.rel);
        if (rel.find(q) != std::string::npos) out.push_back(f);
    }
    return out;
}

std::string profile_summary(const ProfileSnapshot& snap, int max_files) {
    std::ostringstream ss;
    ss << "Profile: MC " << snap.minecraft_version
       << " | Loader: " << snap.loader
       << (snap.loader_version.empty() ? "" : " " + snap.loader_version)
       << " | Java: " << (snap.java_version.empty() ? "auto" : snap.java_version)
       << " | Mods: " << snap.mod_count << "\n";
    ss << "Files indexed: " << snap.files.size() << "\n";
    // group counts by category
    std::map<std::string, int> cats;
    for (auto& f : snap.files) cats[f.category]++;
    for (auto& [cat, n] : cats) {
        ss << "  " << cat << ": " << n << "\n";
    }
    ss << "Notable files:\n";
    int shown = 0;
    for (auto& f : snap.files) {
        if (shown >= max_files) break;
        if (f.category == "other" || f.size > 1024 * 1024) continue;
        ss << "  [" << f.category << "] " << f.rel << "\n";
        ++shown;
    }
    return ss.str();
}

}  // namespace aml::ai