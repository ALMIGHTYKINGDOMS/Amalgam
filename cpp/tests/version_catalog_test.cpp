#include "version_catalog.h"

#include <cassert>
#include <iostream>

int main() {
    using namespace aml::version_catalog;

    assert(supports("fabric", "1.21.11"));
    assert(supports("forge", "1.20.1"));
    assert(!supports("forge", "1.21.11"));
    assert(supports("neoforge", "1.21.8"));
    assert(!supports("neoforge", "1.20.1"));
    assert(supports("vanilla", "1.20.4"));
    assert(!supports("fabric", "1.20.4"));
    assert(!supports("fabric", "1.17.1"));
    std::vector<std::string> live = {"26.2", "1.20.1", "26.2"};
    std::vector<std::string> vanilla = profile_versions("auto");
    merge_live_releases(vanilla, live, "auto");
    assert(std::find(vanilla.begin(), vanilla.end(), "26.2") != vanilla.end());
    std::vector<std::string> forge = profile_versions("forge");
    merge_live_releases(forge, {"26.2", "1.20.1"}, "forge");
    assert(std::find(forge.begin(), forge.end(), "26.2") == forge.end());
    assert(std::find(forge.begin(), forge.end(), "1.20.1") != forge.end());
    assert(default_version("forge") == "1.20.1");
    assert(default_server_version() == "1.20.1");
    assert(supports_server("1.21.11"));
    assert(!supports_server("1.12.2"));

    std::cout << "version catalog passed\n";
    return 0;
}
