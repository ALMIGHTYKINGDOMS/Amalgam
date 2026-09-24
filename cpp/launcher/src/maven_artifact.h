#pragma once

#include "net.h"

#include <string>
#include <string_view>

namespace aml::maven_artifact {

// Parse the conventional Maven sidecar checksum body. Maven repositories
// normally return either "<sha1>" or "<sha1>  <artifact-name>". Reject every
// other shape instead of treating a malformed response as an optional hint.
bool parse_sha1_checksum(std::string_view response, std::string* checksum,
                         std::string* error = nullptr);

// Fetch <artifact_url>.sha1 and use it to validate both a cached artifact and
// a newly downloaded artifact. Callers must create the destination directory.
// The artifact is never considered usable unless its SHA-1 matches the
// repository's sidecar checksum.
bool download_verified(const std::wstring& artifact_url, const std::wstring& destination,
                       net::Progress progress, std::string* error = nullptr);

}  // namespace aml::maven_artifact
