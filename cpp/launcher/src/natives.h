#pragma once

#include <string>
#include <vector>

namespace aml::natives {

// A verified native classifier archive and the archive-relative paths its
// version metadata says to omit while extracting.
struct Archive {
    std::wstring path;
    std::vector<std::string> exclusions;
};

// Computes a content-addressed native-layout identifier.  Archive order is
// deliberately retained because two archives can contain the same filename
// and later archives therefore have observable overlay precedence.
bool layout_fingerprint(const std::vector<Archive>& archives, std::string* fingerprint,
                        std::string* error = nullptr);

// Builds a fresh, immutable native directory below native_root and atomically
// promotes it only after every archive extracted successfully.  Existing
// legacy native directories and incomplete prior attempts are never removed.
// The returned directory is the only path that should be supplied to
// -Djava.library.path for this launch.
bool prepare_layout(const std::wstring& native_root, const std::vector<Archive>& archives,
                    std::wstring* active_directory, std::string* error = nullptr);

}  // namespace aml::natives
