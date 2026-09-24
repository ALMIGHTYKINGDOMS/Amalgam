#include "java.h"

#include <windows.h>

#include <filesystem>
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
    std::vector<aml::java::Install> installs;
    {
        installs = aml::java::scan_installed();
        // We can't assert specific results since it depends on the machine,
        // but we verify it doesn't crash and returns a sorted list.
        for (size_t i = 1; i < installs.size(); ++i) {
            if (installs[i].major < installs[i - 1].major) {
                std::cerr << "scan_installed() results are not sorted\n";
                return 1;
            }
        }
    }
    // Explicit runtime locations are documented as either a Java home or a
    // java.exe/javaw.exe path.  Normalization must not turn a concrete
    // executable into a bogus ...\\bin\\bin home.
    {
        if (aml::java::normalize_configured_runtime_home(L"C:\\SDK") != L"C:\\SDK" ||
            aml::java::normalize_configured_runtime_home(L"C:\\SDK\\bin") != L"C:\\SDK" ||
            aml::java::normalize_configured_runtime_home(L"C:\\SDK\\bin\\java.exe") != L"C:\\SDK" ||
            aml::java::normalize_configured_runtime_home(L"C:\\SDK\\bin\\javaw.exe") != L"C:\\SDK") {
            std::cerr << "configured Java path normalization returned the wrong home\n";
            return 1;
        }
    }
    // When an installed JRE is available, exercise the real validator against
    // both documented input forms.  The test remains portable to machines
    // with no Java by treating this as an optional live probe.
    if (!installs.empty()) {
        const auto& install = installs.front();
        aml::java::JavaRuntime runtime;
        std::string error;
        if (!aml::java::ValidateConfiguredRuntime(install.exe, install.major, &runtime, &error) ||
            std::filesystem::path(runtime.home) != std::filesystem::path(install.home) ||
            runtime.executable != install.home + L"\\bin\\java.exe") {
            std::cerr << "configured java.exe validation failed: " << error << "\n";
            return 1;
        }
        const int wrong_major = install.major == 25 ? 24 : install.major + 1;
        if (aml::java::ValidateConfiguredRuntime(install.home + L"\\bin", wrong_major,
                                                 &runtime, &error)) {
            std::cerr << "configured Java accepted a mismatched major version\n";
            return 1;
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
    // managed_root() is the launcher's single managed-runtime directory: game
    // launch, server start, --check-java and every runtime download resolve
    // through it. It must stay the directory the launcher creates beside itself,
    // never a second location no part of the launcher populates.
    {
        wchar_t self[MAX_PATH]{};
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        std::wstring dir = self;
        const size_t slash = dir.find_last_of(L"\\/");
        dir = slash == std::wstring::npos ? L"." : dir.substr(0, slash);
        const std::wstring expected = aml::java::managed_runtime_root(dir);
        if (aml::java::managed_root() != expected) {
            std::cerr << "managed_root() is not the launcher's own runtimes/java\n";
            return 1;
        }
        // No config value means the launcher's own directory, so a server start
        // and --check-java cannot disagree about where managed Java lives.
        if (aml::java::managed_root(L"") != expected) {
            std::cerr << "managed_root(empty) diverged from managed_root()\n";
            return 1;
        }
        // A configured directory is honored verbatim, as an absolute path...
        if (aml::java::managed_root(L"C:\\Custom\\java") != L"C:\\Custom\\java") {
            std::cerr << "managed_root(configured) did not keep the configured path\n";
            return 1;
        }
        // ...and a relative one resolves against the launcher directory.
        const std::wstring relative = aml::java::managed_root(L"runtimes\\java");
        if (std::filesystem::path(relative) != std::filesystem::path(expected)) {
            std::cerr << "managed_root(relative) did not resolve under the launcher\n";
            return 1;
        }
    }
    std::cout << "java_test passed\n";
    return 0;
}
