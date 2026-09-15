#include "java.h"

#include "extract.h"
#include "json.h"
#include "net.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <functional>
#include <regex>
#include <set>
#include <sstream>

namespace aml::java {

namespace {

std::wstring bin_dir(const std::wstring& home) {
    return home + L"\\bin\\java.exe";
}

bool scan_dir(const std::wstring& root, std::vector<std::wstring>* found) {
    std::wstring pattern = root + L"\\*";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            std::wstring name = fd.cFileName;
            if (name != L"." && name != L"..") {
                std::wstring candidate = root + L"\\" + name;
                std::wstring java = candidate + L"\\bin\\java.exe";
                if (net::file_exists(java)) {
                    found->push_back(candidate);
                } else {
                    WIN32_FIND_DATAW fd2{};
                    HANDLE h2 = FindFirstFileW((candidate + L"\\*").c_str(), &fd2);
                    if (h2 != INVALID_HANDLE_VALUE) {
                        do {
                            if ((fd2.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                                wcscmp(fd2.cFileName, L".") != 0 &&
                                wcscmp(fd2.cFileName, L"..") != 0) {
                                std::wstring c2 = candidate + L"\\" + fd2.cFileName;
                                if (net::file_exists(c2 + L"\\bin\\java.exe")) {
                                    found->push_back(c2);
                                }
                            }
                        } while (FindNextFileW(h2, &fd2));
                        FindClose(h2);
                    }
                }
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return true;
}

}  // namespace

int java_major_from_version_text(const std::string& text) {
    std::smatch m;
    if (std::regex_search(text, m, std::regex(R"("?([0-9]+)\.([0-9]+)\.[0-9]+)"))) {
        int first = std::atoi(m[1].str().c_str());
        int second = std::atoi(m[2].str().c_str());
        return first == 1 ? second : first;
    }
    if (std::regex_search(text, m, std::regex(R"(version "?([0-9]+))"))) {
        return std::atoi(m[1].str().c_str());
    }
    return 0;
}

std::vector<Install> scan_installed() {
    std::vector<Install> out;
    std::set<std::wstring> seen;

    auto probe = [&](const std::wstring& exe) {
        if (seen.count(exe)) return;
        seen.insert(exe);
        std::string cap;
        std::string err;
        std::wstring ver_arg = L"-version";
        if (extract::run_capture(exe, ver_arg, &cap, &err)) {
            int major = java_major_from_version_text(cap);
            if (major > 0) {
                Install inst;
                inst.major = major;
                inst.exe = exe;
                inst.home = extract::parent_of(extract::parent_of(exe));
                out.push_back(std::move(inst));
            }
        }
    };

    wchar_t env_buf[32768];
    if (GetEnvironmentVariableW(L"JAVA_HOME", env_buf, 32768) > 0) {
        std::wstring home = env_buf;
        if (!home.empty() && home.back() == L'\\') home.pop_back();
        probe(bin_dir(home));
    }

    wchar_t path_buf[65536];
    if (GetEnvironmentVariableW(L"PATH", path_buf, 65536) > 0) {
        std::wstring path = path_buf;
        size_t start = 0;
        while (start < path.size()) {
            size_t sep = path.find(L';', start);
            std::wstring dir = path.substr(start, sep == std::wstring::npos ? std::wstring::npos
                                                                            : sep - start);
            if (!dir.empty()) {
                std::wstring exe = dir + L"\\java.exe";
                if (net::file_exists(exe)) probe(exe);
            }
            if (sep == std::wstring::npos) break;
            start = sep + 1;
        }
    }

    std::vector<std::wstring> roots;
    wchar_t pf[MAX_PATH];
    if (SHGetFolderPathW(nullptr, CSIDL_PROGRAM_FILES, nullptr, 0, pf) == S_OK) {
        roots.push_back(std::wstring(pf) + L"\\Eclipse Adoptium");
        roots.push_back(std::wstring(pf) + L"\\Microsoft");
        roots.push_back(std::wstring(pf) + L"\\Java");
        roots.push_back(std::wstring(pf) + L"\\Zulu");
        roots.push_back(std::wstring(pf) + L"\\Amazon Corretto");
    }
    wchar_t pf32[MAX_PATH];
    if (SHGetFolderPathW(nullptr, CSIDL_PROGRAM_FILESX86, nullptr, 0, pf32) == S_OK) {
        roots.push_back(std::wstring(pf32) + L"\\Java");
    }
    wchar_t local[MAX_PATH];
    if (SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local) == S_OK) {
        roots.push_back(std::wstring(local) + L"\\Programs\\Eclipse Adoptium");
    }
    std::vector<std::wstring> found;
    for (const std::wstring& root : roots) {
        scan_dir(root, &found);
    }
    for (const std::wstring& home : found) probe(bin_dir(home));

    std::sort(out.begin(), out.end(),
              [](const Install& a, const Install& b) { return a.major < b.major; });
    return out;
}

std::wstring resolve(int major, const std::vector<Install>& installed,
                     const std::wstring& cache_dir,
                     const std::function<bool(uint64_t, uint64_t)>& progress, std::string* err) {
    JavaRuntimeManager manager(cache_dir);
    JavaRuntime managed = manager.GetRuntime(major);
    if (managed.major == major && !managed.home.empty()) return managed.home;
    for (const Install& inst : installed) {
        if (inst.major == major) return inst.home;
    }
    DownloadResult result = manager.DownloadJava(major, Architecture::X64, progress);
    if (!result.success) {
        if (err) *err = result.error;
        return std::wstring();
    }
    return result.runtime.home;
}

namespace {

std::string architecture_name(Architecture architecture) {
    switch (architecture) {
        case Architecture::Arm64: return "aarch64";
        case Architecture::X86: return "x32";
        case Architecture::X64: default: return "x64";
    }
}

std::string architecture_label(Architecture architecture) {
    switch (architecture) {
        case Architecture::Arm64: return "arm64";
        case Architecture::X86: return "x86";
        case Architecture::X64: default: return "x64";
    }
}

std::wstring runtime_home_from_root(const std::wstring& root) {
    std::wstring direct = root + L"\\bin\\java.exe";
    if (net::file_exists(direct)) return root;
    std::wstring nested = root + L"\\jdk\\bin\\java.exe";
    if (net::file_exists(nested)) return root + L"\\jdk";
    std::error_code ec;
    if (std::filesystem::exists(root, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
            if (ec || !entry.is_directory(ec)) continue;
            const std::wstring candidate = entry.path().wstring();
            if (net::file_exists(candidate + L"\\bin\\java.exe")) return candidate;
            if (net::file_exists(candidate + L"\\jdk\\bin\\java.exe"))
                return candidate + L"\\jdk";
        }
    }
    return std::wstring();
}

JavaRuntime runtime_from_home(const std::wstring& home, int expected_major,
                              bool managed, std::string* error) {
    JavaRuntime runtime;
    runtime.major = expected_major;
    runtime.home = home;
    runtime.executable = home + L"\\bin\\java.exe";
    runtime.javaw = home + L"\\bin\\javaw.exe";
    runtime.managed_by_amalgam = managed;
    std::string output;
    std::string run_error;
    if (!extract::run_capture(runtime.executable, L"-version", &output, &run_error)) {
        if (error) *error = run_error.empty() ? "java -version failed" : run_error;
        return JavaRuntime();
    }
    runtime.major = java_major_from_version_text(output);
    if (runtime.major <= 0 || (expected_major > 0 && runtime.major != expected_major)) {
        if (error) *error = "runtime major version does not match the requested version";
        return JavaRuntime();
    }
    std::smatch match;
    if (std::regex_search(output, match, std::regex(R"(version\s+"?([^"\s]+))")))
        runtime.version = match[1].str();
    return runtime;
}

bool valid_major(int major) {
    return major == 8 || major == 11 || major == 17 || major == 21 || major == 25;
}

bool is_safe_runtime_root(const std::wstring& root) {
    return !root.empty() && root.find(L"..") == std::wstring::npos;
}

}  // namespace

bool JavaRuntimeValidator::ValidateJava(const JavaRuntime& runtime, std::string* error) {
    if (runtime.major <= 0 || runtime.home.empty()) {
        if (error) *error = "runtime metadata is incomplete";
        return false;
    }
    if (!net::file_exists(runtime.home + L"\\bin\\java.exe")) {
        if (error) *error = "bin\\java.exe is missing";
        return false;
    }
    JavaRuntime checked = runtime_from_home(runtime.home, runtime.major,
                                            runtime.managed_by_amalgam, error);
    return checked.major == runtime.major && !checked.home.empty();
}

JavaRuntimeRegistry::JavaRuntimeRegistry(std::wstring root) : root_(std::move(root)) {}

JavaRuntime JavaRuntimeRegistry::GetRuntime(int major) const {
    if (!valid_major(major) || !is_safe_runtime_root(root_)) return JavaRuntime();
    const std::wstring dir = root_ + L"\\" + std::to_wstring(major);
    JavaRuntime runtime;
    std::string error;
    Json metadata;
    if (!json_parse_file(dir + L"\\runtime.json", metadata, &error) || !metadata.isObject())
        return JavaRuntime();
    runtime.major = static_cast<int>(metadata.get("major").as_int(0));
    if (runtime.major != major) return JavaRuntime();
    runtime.vendor = metadata.get("vendor").as_str("Temurin");
    runtime.architecture = metadata.get("architecture").as_str("x64");
    runtime.version = metadata.get("version").as_str();
    runtime.managed_by_amalgam = metadata.get("managedByAmalgam").as_bool(true);
    const std::wstring home = runtime_home_from_root(dir);
    if (home.empty()) return JavaRuntime();
    JavaRuntime checked = runtime_from_home(home, major, true, &error);
    if (checked.home.empty()) return JavaRuntime();
    if (!runtime.version.empty()) checked.version = runtime.version;
    if (!runtime.vendor.empty()) checked.vendor = runtime.vendor;
    if (!runtime.architecture.empty()) checked.architecture = runtime.architecture;
    return checked;
}

std::vector<JavaRuntime> JavaRuntimeRegistry::GetInstalledRuntimes() const {
    std::vector<JavaRuntime> runtimes;
    if (!is_safe_runtime_root(root_)) return runtimes;
    std::error_code ec;
    if (!std::filesystem::exists(root_, ec)) return runtimes;
    for (const auto& entry : std::filesystem::directory_iterator(root_, ec)) {
        if (ec || !entry.is_directory(ec)) continue;
        const std::string name = entry.path().filename().string();
        if (name.empty() || !std::all_of(name.begin(), name.end(),
                                         [](unsigned char c) { return std::isdigit(c) != 0; })) continue;
        const int major = std::atoi(name.c_str());
        JavaRuntime runtime = GetRuntime(major);
        if (runtime.major > 0) runtimes.push_back(std::move(runtime));
    }
    std::sort(runtimes.begin(), runtimes.end(),
              [](const JavaRuntime& a, const JavaRuntime& b) { return a.major < b.major; });
    return runtimes;
}

bool JavaRuntimeRegistry::RegisterRuntime(const JavaRuntime& runtime, std::string* error) const {
    if (!valid_major(runtime.major) || !is_safe_runtime_root(root_) ||
        !JavaRuntimeValidator::ValidateJava(runtime, error)) return false;
    const std::wstring dir = root_ + L"\\" + std::to_wstring(runtime.major);
    if (!net::mkdirs(dir)) {
        if (error) *error = "cannot create managed runtime directory";
        return false;
    }
    Json metadata = Json::obj();
    metadata.set("major", Json::num(runtime.major));
    metadata.set("vendor", Json::str(runtime.vendor.empty() ? "Temurin" : runtime.vendor));
    metadata.set("architecture", Json::str(runtime.architecture.empty() ? "x64" : runtime.architecture));
    metadata.set("version", Json::str(runtime.version));
    metadata.set("path", Json::str(net::to_utf8(runtime.home)));
    metadata.set("managedByAmalgam", Json::boolean(true));
    return json_write_file(dir + L"\\runtime.json", metadata, error);
}

bool JavaRuntimeRegistry::RemoveManagedRuntime(int major, std::string* error) const {
    if (!valid_major(major) || !is_safe_runtime_root(root_)) {
        if (error) *error = "invalid managed runtime version or root";
        return false;
    }
    const std::wstring dir = root_ + L"\\" + std::to_wstring(major);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    if (ec) {
        if (error) *error = "cannot remove managed runtime: " + ec.message();
        return false;
    }
    return true;
}

JavaRuntimeDownloader::JavaRuntimeDownloader(std::wstring root) : root_(std::move(root)) {}

DownloadResult JavaRuntimeDownloader::DownloadJava(
    int major, Architecture architecture,
    const std::function<bool(uint64_t, uint64_t)>& progress) const {
    DownloadResult result;
    if (!valid_major(major) || !is_safe_runtime_root(root_)) {
        result.error = "unsupported Java version or invalid runtime root";
        return result;
    }
    JavaRuntime existing = JavaRuntimeRegistry(root_).GetRuntime(major);
    if (existing.major == major && !existing.home.empty()) {
        result.success = true;
        result.runtime = std::move(existing);
        return result;
    }
    net::mkdirs(root_);
    const std::wstring target = root_ + L"\\" + std::to_wstring(major);
    const std::wstring zip = root_ + L"\\.java-" + std::to_wstring(major) + L".zip";
    const std::wstring staging = root_ + L"\\.staging-" + std::to_wstring(major) +
                                 L"-" + std::to_wstring(GetCurrentProcessId());
    const std::wstring api = L"https://api.adoptium.net/v3/assets/latest/" +
        std::to_wstring(major) + L"/hotspot?architecture=" + net::to_wide(architecture_name(architecture)) +
        L"&image_type=jdk&jvm_impl=hotspot&os=windows&vendor=eclipse";
    std::vector<uint8_t> bytes;
    if (!net::get(api, bytes, &result.error)) return result;
    std::string parse_error;
    Json metadata = Json::parse(std::string(bytes.begin(), bytes.end()), &parse_error);
    if (!metadata.isArray() || metadata.size() == 0) {
        result.error = parse_error.empty() ? "Adoptium returned no matching runtime" : parse_error;
        return result;
    }
    const Json& asset = metadata.at(0);
    const Json& package = asset.get("binary").get("package");
    const std::string link = package.get("link").as_str();
    std::string checksum = package.get("checksum").as_str();
    const int64_t size = package.get("size").as_int(-1);
    if (link.empty() || checksum.size() != 64 || size <= 0) {
        result.error = "Adoptium metadata is missing package integrity information";
        return result;
    }
    for (char& c : checksum)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (!net::download(net::to_wide(link), zip, progress, &result.error,
                       std::string(), size, checksum)) return result;
    std::error_code ec;
    std::filesystem::remove_all(staging, ec);
    std::filesystem::create_directories(staging, ec);
    if (ec || !extract::zip(zip, staging, &result.error)) {
        std::filesystem::remove(zip, ec);
        std::filesystem::remove_all(staging, ec);
        return result;
    }
    std::filesystem::remove(zip, ec);
    const std::wstring home = runtime_home_from_root(staging);
    if (home.empty()) {
        result.error = "downloaded archive does not contain a valid Java runtime";
        std::filesystem::remove_all(staging, ec);
        return result;
    }
    JavaRuntime runtime = runtime_from_home(home, major, true, &result.error);
    if (runtime.home.empty()) {
        std::filesystem::remove_all(staging, ec);
        return result;
    }
    runtime.vendor = "Temurin";
    runtime.architecture = architecture_label(architecture);
    runtime.version = asset.get("version").get("semver").as_str(runtime.version);
    const std::wstring backup = root_ + L"\\.backup-" + std::to_wstring(major) +
                                L"-" + std::to_wstring(GetCurrentProcessId());
    std::filesystem::remove_all(backup, ec);
    const bool had_target = std::filesystem::exists(target, ec);
    if (had_target) std::filesystem::rename(target, backup, ec);
    if (!ec) std::filesystem::rename(staging, target, ec);
    if (ec) {
        result.error = "cannot install managed runtime: " + ec.message();
        std::filesystem::remove_all(staging, ec);
        if (had_target) {
            std::error_code restore_error;
            std::filesystem::rename(backup, target, restore_error);
        }
        return result;
    }
    std::filesystem::remove_all(backup, ec);
    runtime.home = runtime_home_from_root(target);
    runtime.executable = runtime.home + L"\\bin\\java.exe";
    runtime.javaw = runtime.home + L"\\bin\\javaw.exe";
    JavaRuntimeRegistry registry(root_);
    if (!registry.RegisterRuntime(runtime, &result.error)) {
        registry.RemoveManagedRuntime(major);
        return result;
    }
    result.success = true;
    result.runtime = std::move(runtime);
    return result;
}

int JavaVersionResolver::ResolveRequiredJava(const MinecraftVersion& minecraft,
                                             LoaderType /*loader*/, ServerType /*server_type*/) {
    int major = 1;
    int minor = 0;
    int patch = 0;
    if (std::sscanf(minecraft.id.c_str(), "%d.%d.%d", &major, &minor, &patch) < 2) return 17;
    if (major == 1 && (minor > 20 || (minor == 20 && patch >= 5))) return 21;
    if (major == 1 && minor >= 18) return 17;
    if (major == 1 && minor == 17) return 17;
    return 8;
}

JavaRuntimeManager::JavaRuntimeManager(std::wstring root)
    : registry_(root), downloader_(std::move(root)) {}

bool JavaRuntimeManager::IsJavaInstalled(int major) const {
    return GetRuntime(major).major == major;
}

JavaRuntime JavaRuntimeManager::GetRuntime(int major) const {
    return registry_.GetRuntime(major);
}

int JavaRuntimeManager::ResolveRequiredJava(const MinecraftVersion& minecraft,
                                            LoaderType loader, ServerType server_type) const {
    return JavaVersionResolver::ResolveRequiredJava(minecraft, loader, server_type);
}

DownloadResult JavaRuntimeManager::DownloadJava(
    int major, Architecture architecture,
    const std::function<bool(uint64_t, uint64_t)>& progress) const {
    return downloader_.DownloadJava(major, architecture, progress);
}

bool JavaRuntimeManager::ValidateJava(const JavaRuntime& runtime, std::string* error) const {
    return JavaRuntimeValidator::ValidateJava(runtime, error);
}

std::vector<JavaRuntime> JavaRuntimeManager::GetInstalledRuntimes() const {
    return registry_.GetInstalledRuntimes();
}

bool JavaRuntimeManager::RemoveManagedRuntime(int major, std::string* error) const {
    return registry_.RemoveManagedRuntime(major, error);
}

std::wstring JavaRuntimeManager::Resolve(int major, const std::vector<Install>& installed,
                                         const std::function<bool(uint64_t, uint64_t)>& progress,
                                         std::string* error) const {
    JavaRuntime managed = GetRuntime(major);
    if (managed.major == major && !managed.home.empty()) return managed.home;
    for (const Install& install : installed) {
        if (install.major == major && !install.home.empty()) return install.home;
    }
    DownloadResult result = DownloadJava(major, Architecture::X64, progress);
    if (!result.success) {
        if (error) *error = result.error;
        return std::wstring();
    }
    return result.runtime.home;
}

std::wstring managed_runtime_root(const std::wstring& base_dir) {
    if (base_dir.empty()) return L"";
    const std::wstring suffix = L"\\runtimes\\java";
    if (base_dir.size() >= suffix.size() &&
        _wcsicmp(base_dir.c_str() + base_dir.size() - suffix.size(), suffix.c_str()) == 0)
        return base_dir;
    return base_dir + suffix;
}

namespace {

// The directory the running launcher lives in. Managed runtimes sit beside it
// because that is where the packaged launcher.json points java_cache_dir.
std::wstring launcher_dir() {
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring path = self;
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : path.substr(0, slash);
}

}  // namespace

std::wstring managed_root() {
    return managed_runtime_root(launcher_dir());
}

std::wstring managed_root(const std::wstring& configured) {
    if (configured.empty()) return managed_root();
    const std::filesystem::path root(configured);
    return root.is_relative() ? (std::filesystem::path(launcher_dir()) / root).wstring()
                              : root.wstring();
}

}  // namespace aml::java
