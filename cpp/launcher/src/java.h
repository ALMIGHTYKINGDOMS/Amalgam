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

std::wstring managed_runtime_root(const std::wstring& base_dir);

std::vector<Install> scan_installed();

int java_major_from_version_text(const std::string& text);

std::wstring resolve(int major, const std::vector<Install>& installed,
                     const std::wstring& cache_dir, const std::function<bool(uint64_t, uint64_t)>& progress,
                     std::string* err);

}  // namespace aml::java
