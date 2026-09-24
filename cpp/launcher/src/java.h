#pragma once

#include <functional>
#include <string>
#include <vector>

namespace aml::java {

struct Install {
    int major = 0;
    std::wstring home;
    std::wstring exe;
};

enum class Architecture {
    X64,
    Arm64,
    X86,
};

struct MinecraftVersion {
    std::string id;
};

enum class LoaderType {
    Vanilla,
    Fabric,
    Quilt,
    Forge,
    NeoForge,
};

enum class ServerType {
    Client,
    Dedicated,
    Local,
};

struct JavaRuntime {
    int major = 0;
    std::string vendor = "Temurin";
    std::string architecture = "x64";
    std::string version;
    std::wstring home;
    std::wstring executable;
    std::wstring javaw;
    bool managed_by_amalgam = false;
};

struct DownloadResult {
    bool success = false;
    JavaRuntime runtime;
    std::string error;
};

class JavaRuntimeValidator {
public:
    static bool ValidateJava(const JavaRuntime& runtime, std::string* error = nullptr);
};

// Normalizes the documented per-profile forms of a Java runtime location:
// a Java home, its bin directory, or bin\java.exe / bin\javaw.exe.  This is
// lexical only; callers that will execute Java must use
// ValidateConfiguredRuntime() as well.
std::wstring normalize_configured_runtime_home(const std::wstring& configured_path);

// Resolves and validates a user-configured Java location.  The validator runs
// the sibling java.exe and requires its actual major version to match
// expected_major, so a profile cannot accidentally launch with a different
// JRE merely because the configured path exists.
bool ValidateConfiguredRuntime(const std::wstring& configured_path, int expected_major,
                              JavaRuntime* runtime, std::string* error = nullptr);

class JavaRuntimeRegistry {
public:
    explicit JavaRuntimeRegistry(std::wstring root);

    std::vector<JavaRuntime> GetInstalledRuntimes() const;
    JavaRuntime GetRuntime(int major) const;
    bool RegisterRuntime(const JavaRuntime& runtime, std::string* error = nullptr) const;
    bool RemoveManagedRuntime(int major, std::string* error = nullptr) const;
    const std::wstring& root() const { return root_; }

private:
    std::wstring root_;
};

class JavaRuntimeDownloader {
public:
    explicit JavaRuntimeDownloader(std::wstring root);

    DownloadResult DownloadJava(
        int major, Architecture architecture = Architecture::X64,
        const std::function<bool(uint64_t, uint64_t)>& progress = {}) const;

private:
    std::wstring root_;
};

class JavaVersionResolver {
public:
    static int ResolveRequiredJava(const MinecraftVersion& minecraft,
                                   LoaderType loader, ServerType server_type);
};

class JavaRuntimeManager {
public:
    explicit JavaRuntimeManager(std::wstring root);

    bool IsJavaInstalled(int major) const;
    JavaRuntime GetRuntime(int major) const;
    int ResolveRequiredJava(const MinecraftVersion& minecraft,
                            LoaderType loader, ServerType server_type) const;
    DownloadResult DownloadJava(
        int major, Architecture architecture = Architecture::X64,
        const std::function<bool(uint64_t, uint64_t)>& progress = {}) const;
    bool ValidateJava(const JavaRuntime& runtime, std::string* error = nullptr) const;
    std::vector<JavaRuntime> GetInstalledRuntimes() const;
    bool RemoveManagedRuntime(int major, std::string* error = nullptr) const;
    std::wstring Resolve(int major, const std::vector<Install>& installed,
                         const std::function<bool(uint64_t, uint64_t)>& progress,
                         std::string* error) const;

private:
    JavaRuntimeRegistry registry_;
    JavaRuntimeDownloader downloader_;
};

// Appends \runtimes\java to a launcher base directory unless the directory is
// already a managed-runtime root.
std::wstring managed_runtime_root(const std::wstring& base_dir);

// The one directory that holds launcher-managed Java runtimes. Game launch,
// server start, --check-java/--java-install and every download that populates
// the directory resolve through here, so the Java the launcher reports is the
// Java it runs and nothing looks in a directory the launcher never creates.
// `configured` is config::Config::java_cache_dir; empty falls back to the
// launcher's own <launcher>\runtimes\java, and a relative value is resolved
// against the launcher directory.
std::wstring managed_root(const std::wstring& configured);
std::wstring managed_root();

std::vector<Install> scan_installed();

int java_major_from_version_text(const std::string& text);

std::wstring resolve(int major, const std::vector<Install>& installed,
                     const std::wstring& cache_dir, const std::function<bool(uint64_t, uint64_t)>& progress,
                     std::string* err);

}  // namespace aml::java
