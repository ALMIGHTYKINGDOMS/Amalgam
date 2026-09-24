#include "maven_artifact.h"

#include <iostream>
#include <string>

namespace {

constexpr const char* kSha1 = "0123456789abcdef0123456789abcdef01234567";

bool expect_parse(const std::string& source, bool should_pass,
                  const std::string& expected = std::string()) {
    std::string actual;
    std::string error;
    const bool passed = aml::maven_artifact::parse_sha1_checksum(source, &actual, &error);
    if (passed != should_pass) {
        std::cerr << "unexpected parse result for '" << source << "': " << error << "\n";
        return false;
    }
    if (passed && actual != expected) {
        std::cerr << "unexpected checksum: " << actual << "\n";
        return false;
    }
    return true;
}

}  // namespace

int main() {
    bool ok = true;
    ok &= expect_parse(kSha1, true, kSha1);
    ok &= expect_parse("  0123456789ABCDEF0123456789ABCDEF01234567  installer.jar\r\n",
                       true, kSha1);
    ok &= expect_parse("0123456789abcdef0123456789abcdef0123456", false);
    ok &= expect_parse("0123456789abcdef0123456789abcdef012345678", false);
    ok &= expect_parse("0123456789abcdef0123456789abcdef0123456g", false);
    ok &= expect_parse("sha1: 0123456789abcdef0123456789abcdef01234567", false);
    if (!ok) return 1;
    std::cout << "maven artifact checksum tests passed\n";
    return 0;
}
