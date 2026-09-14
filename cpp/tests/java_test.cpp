#include "java.h"

#include <iostream>
#include <string>

int main() {
    // java_major_from_version_text(): standard JDK 21 output
    {
        int r = aml::java::java_major_from_version_text(
            "openjdk version \"21.0.12\" 2024-07-16");
        if (r != 21) {
            std::cerr << "java_major_from_version_text() failed for JDK 21, got " << r << "\n";
            return 1;
        }
    }
    // java_major_from_version_text(): Java 8 format
    {
        int r = aml::java::java_major_from_version_text(
            "java version \"1.8.0_392\"");
        if (r != 8) {
            std::cerr << "java_major_from_version_text() failed for Java 8, got " << r << "\n";
            return 1;
        }
    }
    // java_major_from_version_text(): simple version string
    {
        int r = aml::java::java_major_from_version_text("version \"17\"");
        if (r != 17) {
            std::cerr << "java_major_from_version_text() failed for version 17, got " << r << "\n";
            return 1;
        }
    }
    // java_major_from_version_text(): empty string returns 0
    {
        int r = aml::java::java_major_from_version_text("");
        if (r != 0) {
            std::cerr << "java_major_from_version_text() should return 0 for empty string\n";
            return 1;
        }
    }
    // java_major_from_version_text(): garbage text returns 0
    {
        int r = aml::java::java_major_from_version_text("not a version string");
        if (r != 0) {
            std::cerr << "java_major_from_version_text() should return 0 for garbage\n";
            return 1;
        }
    }
    // java_major_from_version_text(): JDK 11
    {
        int r = aml::java::java_major_from_version_text(
            "openjdk version \"11.0.22\" 2024-01-16");
        if (r != 11) {
            std::cerr << "java_major_from_version_text() failed for JDK 11, got " << r << "\n";
            return 1;
        }
    }
    // java_major_from_version_text(): JDK 17 with build info
    {
        int r = aml::java::java_major_from_version_text(
            "OpenJDK Runtime Environment (build 17.0.10+7-107)");
        if (r != 17) {
            std::cerr << "java_major_from_version_text() failed for JDK 17 with build, got " << r << "\n";
            return 1;
        }
    }
    // scan_installed(): doesn't crash
    {
        std::vector<aml::java::Install> installs = aml::java::scan_installed();
        // We can't assert specific results since it depends on the machine,
        // but we verify it doesn't crash and returns a sorted list.
        for (size_t i = 1; i < installs.size(); ++i) {
            if (installs[i].major < installs[i - 1].major) {
                std::cerr << "scan_installed() results are not sorted\n";
                return 1;
            }
        }
    }
    // Java version resolution follows Minecraft's supported runtime bands.
    {
        if (aml::java::JavaVersionResolver::ResolveRequiredJava(
                {"1.16.5"}, aml::java::LoaderType::Forge,
                aml::java::ServerType::Client) != 8 ||
            aml::java::JavaVersionResolver::ResolveRequiredJava(
                {"1.20.1"}, aml::java::LoaderType::Fabric,
                aml::java::ServerType::Client) != 17 ||
            aml::java::JavaVersionResolver::ResolveRequiredJava(
                {"1.21.1"}, aml::java::LoaderType::NeoForge,
                aml::java::ServerType::Client) != 21) {
            std::cerr << "JavaVersionResolver returned an unexpected runtime\n";
            return 1;
        }
    }
    // Managed runtime roots are deterministic and do not embed runtimes in the installer.
    {
        const std::wstring root = aml::java::managed_runtime_root(L"C:\\Amalgam");
        if (root != L"C:\\Amalgam\\runtimes\\java" ||
            aml::java::managed_runtime_root(root) != root) {
            std::cerr << "managed_runtime_root() returned an unexpected path\n";
            return 1;
        }
    }
    std::cout << "java_test passed\n";
    return 0;
}
