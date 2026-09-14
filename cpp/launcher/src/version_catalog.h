#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

// The launcher may inspect/import any manifest version. This catalogue keeps
// the common stable targets immediately available in creation dialogs while
// preserving the distinction between vanilla/server coverage and the smaller
// set of loader versions with a maintained Amalgam bridge.
namespace aml::version_catalog {

struct Entry {
    const char* id;
    bool fabric;
    bool quilt;
    bool forge;
    bool neoforge;
    bool server;
};

inline const std::vector<Entry>& entries() {
    static const std::vector<Entry> kEntries = {
        {"1.21.11", true,  true,  false, true,  true },
        {"1.21.10", false, false, false, false, true },
        {"1.21.9",  false, false, false, false, true },
        {"1.21.8",  true,  true,  false, true,  true },
        {"1.21.7",  false, false, false, false, true },
        {"1.21.6",  true,  true,  false, true,  true },
        {"1.21.5",  true,  true,  false, true,  true },
        {"1.21.4",  true,  true,  false, true,  true },
        {"1.21.3",  false, false, false, false, true },
        {"1.21.2",  false, false, false, false, true },
        {"1.21.1",  true,  true,  false, true,  true },
        {"1.21",    false, false, false, false, true },
        {"1.20.6",  false, false, false, false, true },
        {"1.20.5",  false, false, false, false, true },
        {"1.20.4",  false, false, false, false, true },
        {"1.20.3",  false, false, false, false, true },
        {"1.20.2",  false, false, false, false, true },
        {"1.20.1",  true,  true,  true,  false, true },
        {"1.19.4",  false, false, false, false, true },
        {"1.19.3",  false, false, false, false, true },
        {"1.19.2",  true,  true,  true,  false, true },
        {"1.18.2",  true,  true,  true,  false, true },
        {"1.18.1",  false, false, false, false, true },
        {"1.17.1",  false, false, false, false, true },
        {"1.16.5",  false, false, false, false, true },
        {"1.15.2",  false, false, false, false, true },
        {"1.14.4",  false, false, false, false, true },
        {"1.13.2",  false, false, false, false, true },
        {"1.12.2",  false, false, true,  false, false },
    };
    return kEntries;
}

inline std::string normalized_loader(std::string loader) {
    std::transform(loader.begin(), loader.end(), loader.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return loader;
}

inline bool supports(const std::string& loader, const std::string& version) {
    const std::string normalized = normalized_loader(loader);
    for (const Entry& entry : entries()) {
        if (version != entry.id) continue;
        if (normalized.empty() || normalized == "auto" || normalized == "vanilla") return true;
        if (normalized == "fabric") return entry.fabric;
        if (normalized == "quilt") return entry.quilt;
        if (normalized == "forge") return entry.forge;
        if (normalized == "neoforge") return entry.neoforge;
        return false;
    }
    return false;
}

inline std::vector<std::string> profile_versions(const std::string& loader = "auto") {
    std::vector<std::string> result;
    for (const Entry& entry : entries()) {
        if (supports(loader, entry.id)) result.emplace_back(entry.id);
    }
    return result;
}

// Merge released IDs from Mojang's live manifest into a selector without
// claiming that an Amalgam bridge exists for every loader/version pair. The
// vanilla/auto path can launch any released vanilla target; specific modded
// loaders remain limited to the maintained bridge matrix above.
inline void merge_live_releases(std::vector<std::string>& result,
                                const std::vector<std::string>& live_ids,
                                const std::string& loader = "auto") {
    const std::string normalized = normalized_loader(loader);
    for (const std::string& id : live_ids) {
        if (id.empty()) continue;
        const bool vanilla_target = normalized.empty() || normalized == "auto" ||
                                    normalized == "vanilla";
        if (!vanilla_target && !supports(normalized, id)) continue;
        if (std::find(result.begin(), result.end(), id) == result.end())
            result.push_back(id);
    }
}

inline std::vector<std::string> server_versions() {
    std::vector<std::string> result;
    for (const Entry& entry : entries()) {
        if (entry.server) result.emplace_back(entry.id);
    }
    return result;
}

inline bool supports_server(const std::string& version) {
    for (const Entry& entry : entries()) {
        if (version == entry.id) return entry.server;
    }
    // Mojang's manifest is the authority for newly released vanilla versions.
    // Accept a numeric release id here so the server UI can use a fresh
    // manifest entry without requiring a launcher binary update first.
    if (version.empty()) return false;
    int components = 0;
    bool digit = false;
    for (size_t i = 0; i <= version.size(); ++i) {
        const char c = i < version.size() ? version[i] : '.';
        if (c == '.') {
            if (!digit) return false;
            ++components;
            digit = false;
        } else if (std::isdigit(static_cast<unsigned char>(c))) {
            digit = true;
        } else {
            return false;
        }
    }
    return components >= 2;
}

inline std::string default_version(const std::string& loader = "auto") {
    const auto choices = profile_versions(loader);
    return choices.empty() ? std::string{} : choices.front();
}

inline std::string default_server_version() {
    const auto choices = server_versions();
    const auto it = std::find(choices.begin(), choices.end(), "1.20.1");
    return it == choices.end() ? (choices.empty() ? std::string{} : choices.front()) : *it;
}

}  // namespace aml::version_catalog
