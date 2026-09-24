#include "maven_artifact.h"

#include <algorithm>
#include <cctype>
#include <utility>
#include <vector>

namespace aml::maven_artifact {

namespace {

void set_error(std::string* error, const std::string& message) {
    if (error) *error = message;
}

}  // namespace

bool parse_sha1_checksum(std::string_view response, std::string* checksum,
                         std::string* error) {
    if (!checksum) {
        set_error(error, "checksum output is required");
        return false;
    }

    size_t start = 0;
    while (start < response.size() &&
           std::isspace(static_cast<unsigned char>(response[start]))) {
        ++start;
    }
    if (response.size() - start < 40) {
        set_error(error, "Maven checksum is missing or truncated");
        return false;
    }

    std::string value(response.substr(start, 40));
    if (!std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return std::isxdigit(c) != 0;
        })) {
        set_error(error, "Maven checksum is not hexadecimal");
        return false;
    }

    // A sidecar may contain whitespace followed by the artifact name, but a
    // 41st non-whitespace character means this is not an exact SHA-1 token.
    const size_t after = start + value.size();
    if (after < response.size() &&
        !std::isspace(static_cast<unsigned char>(response[after]))) {
        set_error(error, "Maven checksum has an invalid length");
        return false;
    }

    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    *checksum = std::move(value);
    return true;
}

bool download_verified(const std::wstring& artifact_url, const std::wstring& destination,
                       net::Progress progress, std::string* error) {
    std::vector<uint8_t> response;
    std::string checksum_error;
    if (!net::get(artifact_url + L".sha1", response, &checksum_error)) {
        set_error(error, "failed to retrieve Maven checksum: " + checksum_error);
        return false;
    }

    const std::string response_text(response.begin(), response.end());
    std::string checksum;
    if (!parse_sha1_checksum(response_text, &checksum, &checksum_error)) {
        set_error(error, checksum_error.empty() ? "Maven checksum is invalid" : checksum_error);
        return false;
    }

    if (net::verify_file(destination, checksum, -1)) return true;
    return net::download(artifact_url, destination, std::move(progress), error, checksum, -1);
}

}  // namespace aml::maven_artifact
