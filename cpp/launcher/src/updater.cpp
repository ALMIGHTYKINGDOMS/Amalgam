#include "updater.h"

#include "extract.h"
#include "json.h"
#include "net.h"

// Generated at configure time when AMALGAM_UPDATER_PUBLIC_KEY_FILE is set;
// otherwise this header does not exist and all remote manifests fail closed.
#if __has_include("updater_public_key.h")
#include "updater_public_key.h"
#define AMALGAM_HAS_EMBEDDED_UPDATER_KEY 1
#endif

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00  // Windows 10 API surface (CryptVerifyDetachedSignature)
#endif

#include <windows.h>
#include <wincrypt.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <set>
#include <sstream>
#include <vector>

namespace aml::updater {

namespace {

// Payload-signing public key (PEM). Configured at runtime by release
// engineering; never stored in the repository.
std::mutex g_key_mu;
std::string g_signing_public_key;

// Keep the manifest policy aligned with net::download's hard ceiling.  The
// updater only ever consumes release archives, so permitting an unspecified
// or arbitrarily large size would turn the update endpoint into a disk-fill
// primitive before its hash could be checked.
constexpr int64_t kMaxUpdatePayloadBytes = 32LL * 1024 * 1024 * 1024;
constexpr int64_t kMaxReleaseComponentBytes = 4LL * 1024 * 1024 * 1024;
constexpr size_t kMaxReleaseComponents = 20000;
constexpr size_t kMaxApplyPlanComponents = kMaxReleaseComponents * 2 + 3;
constexpr uintmax_t kMaxReleaseMetadataBytes = 16ULL * 1024 * 1024;
constexpr int64_t kApplyPlanSchemaVersion = 1;

bool is_hex64(const std::string& s) {
    if (s.size() != 64) return false;
    for (char c : s) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

bool is_https_url(const std::string& url) {
    return url.rfind("https://", 0) == 0;
}

std::string lower_ascii(std::string value) {
    for (char& c : value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

bool safe_update_version(const std::string& version) {
    if (version.empty() || version.size() > 128) return false;
    for (char c : version) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-' && c != '_') {
            return false;
        }
    }
    return true;
}

bool safe_component_segment(const std::string& segment) {
    if (segment.empty() || segment == "." || segment == "..") return false;
    for (char c : segment) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-' && c != '_') {
            return false;
        }
    }
    return true;
}

// A release update may touch only product-owned files. In particular, this
// rejects launcher.json, managed Java runtimes, profiles, instances and the
// update working area even when a signed archive is malformed or a future
// release tool regresses. The narrow character policy also keeps generated
// batch paths free of cmd metacharacters.
bool normalize_component_path(const std::string& raw, const std::string& launcher_name,
                              std::string& normalized, std::string* err) {
    if (raw.empty() || raw.size() > 240 || raw.find('\\') != std::string::npos ||
        raw.front() == '/' || raw.back() == '/' || raw.find("//") != std::string::npos) {
        if (err) *err = "release component path is unsafe";
        return false;
    }

    size_t start = 0;
    while (start < raw.size()) {
        const size_t slash = raw.find('/', start);
        const std::string segment = raw.substr(start, slash == std::string::npos
                                                           ? std::string::npos
                                                           : slash - start);
        if (!safe_component_segment(segment)) {
            if (err) *err = "release component path is unsafe";
            return false;
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }

    const std::string lower = lower_ascii(raw);
    const std::string launcher_lower = lower_ascii(launcher_name);
    const size_t slash = lower.find('/');
    if (slash == std::string::npos) {
        static const std::set<std::string> kSafeRootFiles = {
            "amalgam.dll", "launcher.json.template", "prerequisites.json",
            "component-manifest.json", "sbom.cdx.json", "release.sha256",
            "license.txt", "install-info.txt", "user-guide.md", "release-notes.md",
            "beta-tester-guide.md",
        };
        if (lower != launcher_lower && kSafeRootFiles.count(lower) == 0) {
            if (err) *err = "release component targets a protected or unknown root file";
            return false;
        }
    } else {
        const bool allowed = lower.rfind("bridges/", 0) == 0 ||
                             lower.rfind("bedrock/", 0) == 0 ||
                             lower.rfind("branding/", 0) == 0 ||
                             lower.rfind("ai/", 0) == 0 ||
                             lower.rfind("legal/", 0) == 0 ||
                             lower.rfind("tools/", 0) == 0 ||
                             lower.rfind("runtimes/ai/", 0) == 0;
        if (!allowed) {
            if (err) *err = "release component targets a protected or unknown directory";
            return false;
        }
    }

    normalized = raw;
    return true;
}

std::wstring component_path(const std::wstring& root, const std::string& relative) {
    std::wstring native = net::to_wide(relative);
    std::replace(native.begin(), native.end(), L'/', L'\\');
    return (std::filesystem::path(root) / std::filesystem::path(native)).wstring();
}

bool regular_file_info(const std::wstring& path, uintmax_t* size = nullptr) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(std::filesystem::path(path), ec) || ec) return false;
    if (size) {
        const uintmax_t actual = std::filesystem::file_size(std::filesystem::path(path), ec);
        if (ec) return false;
        *size = actual;
    }
    return true;
}

bool read_limited_text_file(const std::wstring& path, std::string& out, std::string* err) {
    uintmax_t size = 0;
    if (!regular_file_info(path, &size)) {
        if (err) *err = "release metadata file is missing or not a regular file";
        return false;
    }
    if (size > kMaxReleaseMetadataBytes) {
        if (err) *err = "release metadata file is too large";
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        if (err) *err = "could not read release metadata file";
        return false;
    }
    out.assign((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (!input.good() && !input.eof()) {
        if (err) *err = "could not finish reading release metadata file";
        return false;
    }
    return true;
}

struct ReleaseComponent {
    std::string path;       // validated release-relative path, with '/'
    std::string sha256;     // lowercase SHA-256
    int64_t size = 0;
};

enum class PlanAction { Replace, Delete };

struct PlannedComponent {
    ReleaseComponent component;
    PlanAction action = PlanAction::Replace;
};

std::string component_key(const std::string& path) {
    return lower_ascii(path);
}

bool component_from_file(const std::wstring& root, const std::string& relative,
                         const std::string& launcher_name, ReleaseComponent& out,
                         std::string* err) {
    std::string normalized;
    if (!normalize_component_path(relative, launcher_name, normalized, err)) return false;
    const std::wstring file = component_path(root, normalized);
    uintmax_t size = 0;
    if (!regular_file_info(file, &size)) {
        if (err) *err = "release component is missing or not a regular file: " + normalized;
        return false;
    }
    if (size > static_cast<uintmax_t>(kMaxReleaseComponentBytes)) {
        if (err) *err = "release component is too large: " + normalized;
        return false;
    }
    const std::string hash = lower_ascii(net::sha256_file(file));
    if (!is_hex64(hash)) {
        if (err) *err = "could not hash release component: " + normalized;
        return false;
    }
    out.path = std::move(normalized);
    out.sha256 = hash;
    out.size = static_cast<int64_t>(size);
    return true;
}

bool load_component_manifest(const std::wstring& root, const std::wstring& manifest_path,
                             const std::string& launcher_name, bool verify_files,
                             std::vector<ReleaseComponent>& out, std::string* err) {
    out.clear();
    std::string text;
    if (!read_limited_text_file(manifest_path, text, err)) return false;

    std::string parse_error;
    const Json manifest = Json::parse(text, &parse_error);
    if (!parse_error.empty() || !manifest.is(Json::Type::Arr)) {
        if (err) *err = "component manifest must be a JSON array";
        return false;
    }
    if (manifest.size() == 0 || manifest.size() > kMaxReleaseComponents) {
        if (err) *err = "component manifest has an invalid component count";
        return false;
    }

    std::set<std::string> seen;
    int64_t total = 0;
    for (const Json& item : manifest.items()) {
        if (!item.is(Json::Type::Obj) || !item.isMember("name") ||
            !item.isMember("sha256") || !item.isMember("size") ||
            !item.get("name").is(Json::Type::Str) ||
            !item.get("sha256").is(Json::Type::Str) ||
            !item.get("size").is(Json::Type::Num)) {
            if (err) *err = "component manifest has a malformed component";
            return false;
        }

        std::string normalized;
        if (!normalize_component_path(item.get("name").as_str(), launcher_name, normalized, err)) {
            return false;
        }
        const std::string key = component_key(normalized);
        if (!seen.insert(key).second) {
            if (err) *err = "component manifest has duplicate component paths";
            return false;
        }
        if (key == "component-manifest.json" || key == "sbom.cdx.json" ||
            key == "release.sha256") {
            if (err) *err = "component manifest must not self-list release metadata";
            return false;
        }

        const double size_number = item.get("size").as_num(-1.0);
        const int64_t size = item.get("size").as_int(-1);
        if (size < 0 || size > kMaxReleaseComponentBytes ||
            size_number != static_cast<double>(size) || total > kMaxReleaseComponentBytes - size) {
            if (err) *err = "component manifest has an invalid component size";
            return false;
        }
        total += size;

        const std::string expected_hash = lower_ascii(item.get("sha256").as_str());
        if (!is_hex64(expected_hash)) {
            if (err) *err = "component manifest has an invalid component hash";
            return false;
        }

        ReleaseComponent component{normalized, expected_hash, size};
        if (verify_files) {
            const std::wstring file = component_path(root, normalized);
            uintmax_t actual_size = 0;
            if (!regular_file_info(file, &actual_size) ||
                actual_size != static_cast<uintmax_t>(size)) {
                if (err) *err = "component manifest size mismatch: " + normalized;
                return false;
            }
            if (lower_ascii(net::sha256_file(file)) != expected_hash) {
                if (err) *err = "component manifest hash mismatch: " + normalized;
                return false;
            }
        }
        out.push_back(std::move(component));
    }
    return true;
}

bool require_complete_release_baseline(const std::vector<ReleaseComponent>& components,
                                       const std::string& launcher_name, std::string* err) {
    std::set<std::string> paths;
    for (const ReleaseComponent& component : components) paths.insert(component_key(component.path));
    const std::vector<std::string> required = {
        lower_ascii(launcher_name), "amalgam.dll", "launcher.json.template",
        "prerequisites.json", "bedrock/amalgambedrockclient.mcaddon",
        "bedrock/package-inventory.json", "bedrock/package-sha256.txt",
    };
    for (const std::string& path : required) {
        if (paths.count(path) == 0) {
            if (err) *err = "release component manifest is missing required component: " + path;
            return false;
        }
    }
    for (const std::string& path : paths) {
        if (path.rfind("bridges/", 0) == 0) return true;
    }
    if (err) *err = "release component manifest is missing bridge components";
    return false;
}

bool validate_release_hash_inventory(const std::wstring& root,
                                     const std::vector<ReleaseComponent>& components,
                                     const std::string& launcher_name, std::string* err) {
    std::string text;
    if (!read_limited_text_file(component_path(root, "release.sha256"), text, err)) return false;

    std::set<std::string> expected;
    for (const ReleaseComponent& component : components) expected.insert(component_key(component.path));
    expected.insert("component-manifest.json");
    expected.insert("sbom.cdx.json");

    std::set<std::string> seen;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line.size() <= 65 || !is_hex64(line.substr(0, 64)) ||
            !std::isspace(static_cast<unsigned char>(line[64]))) {
            if (err) *err = "release hash inventory has a malformed line";
            return false;
        }
        size_t path_start = 64;
        while (path_start < line.size() &&
               std::isspace(static_cast<unsigned char>(line[path_start]))) {
            ++path_start;
        }
        if (path_start >= line.size()) {
            if (err) *err = "release hash inventory has an empty component path";
            return false;
        }
        std::string normalized;
        if (!normalize_component_path(line.substr(path_start), launcher_name, normalized, err)) {
            return false;
        }
        const std::string key = component_key(normalized);
        if (expected.count(key) == 0 || !seen.insert(key).second) {
            if (err) *err = "release hash inventory has an unexpected or duplicate component";
            return false;
        }
        const std::string actual_hash = lower_ascii(net::sha256_file(component_path(root, normalized)));
        if (actual_hash != lower_ascii(line.substr(0, 64))) {
            if (err) *err = "release hash inventory mismatch: " + normalized;
            return false;
        }
    }
    if (seen != expected) {
        if (err) *err = "release hash inventory does not cover every release component";
        return false;
    }
    return true;
}

bool reject_unlisted_release_files(const std::wstring& root,
                                   const std::vector<ReleaseComponent>& components,
                                   const std::string& launcher_name, std::string* err) {
    std::set<std::string> allowed;
    for (const ReleaseComponent& component : components) allowed.insert(component_key(component.path));
    allowed.insert("component-manifest.json");
    allowed.insert("sbom.cdx.json");
    allowed.insert("release.sha256");

    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(std::filesystem::path(root), ec), end;
    if (ec) {
        if (err) *err = "could not enumerate extracted release payload";
        return false;
    }
    for (; it != end; it.increment(ec)) {
        if (ec) {
            if (err) *err = "could not enumerate extracted release payload";
            return false;
        }
        const std::filesystem::file_status status = it->symlink_status(ec);
        if (ec || std::filesystem::is_symlink(status) || std::filesystem::is_other(status)) {
            if (err) *err = "release payload contains an unsafe filesystem entry";
            return false;
        }
        if (std::filesystem::is_directory(status)) continue;
        if (!std::filesystem::is_regular_file(status)) {
            if (err) *err = "release payload contains a non-file component";
            return false;
        }
        const std::filesystem::path relative = std::filesystem::relative(
            it->path(), std::filesystem::path(root), ec);
        if (ec) {
            if (err) *err = "could not resolve extracted release component path";
            return false;
        }
        std::string normalized;
        if (!normalize_component_path(net::to_utf8(relative.generic_wstring()), launcher_name,
                                      normalized, err)) {
            return false;
        }
        if (allowed.count(component_key(normalized)) == 0) {
            if (err) *err = "release payload contains an unlisted component: " + normalized;
            return false;
        }
    }
    return true;
}

bool validate_release_payload(const std::wstring& root, const std::string& launcher_name,
                              std::vector<ReleaseComponent>& components, std::string* err) {
    const std::wstring manifest_path = component_path(root, "component-manifest.json");
    if (!load_component_manifest(root, manifest_path, launcher_name, true, components, err) ||
        !require_complete_release_baseline(components, launcher_name, err) ||
        !validate_release_hash_inventory(root, components, launcher_name, err) ||
        !reject_unlisted_release_files(root, components, launcher_name, err)) {
        return false;
    }

    for (const char* metadata : {"component-manifest.json", "sbom.cdx.json", "release.sha256"}) {
        ReleaseComponent component;
        if (!component_from_file(root, metadata, launcher_name, component, err)) return false;
        components.push_back(std::move(component));
    }
    return true;
}

bool component_less(const PlannedComponent& a, const PlannedComponent& b) {
    const std::string a_key = component_key(a.component.path);
    const std::string b_key = component_key(b.component.path);
    if (a_key != b_key) return a_key < b_key;
    return static_cast<int>(a.action) < static_cast<int>(b.action);
}

bool build_apply_plan(const std::wstring& exe_dir, const std::string& launcher_name,
                      const std::vector<ReleaseComponent>& release_components,
                      std::vector<PlannedComponent>& plan, std::string* err) {
    plan.clear();
    std::set<std::string> new_paths;
    for (const ReleaseComponent& component : release_components) {
        const std::string key = component_key(component.path);
        if (!new_paths.insert(key).second) {
            if (err) *err = "release component plan has duplicate paths";
            return false;
        }
        plan.push_back({component, PlanAction::Replace});
    }

    // A prior manifest is used only to remove old product components no longer
    // present in the new full payload. If it is absent/corrupt, replacing the
    // authenticated new set is safe; deletion is simply withheld.
    const std::wstring prior_manifest = component_path(exe_dir, "component-manifest.json");
    if (regular_file_info(prior_manifest)) {
        std::vector<ReleaseComponent> prior_components;
        std::string ignored_prior_error;
        if (load_component_manifest(exe_dir, prior_manifest, launcher_name, false,
                                    prior_components, &ignored_prior_error)) {
            for (const ReleaseComponent& component : prior_components) {
                if (new_paths.count(component_key(component.path)) == 0) {
                    plan.push_back({component, PlanAction::Delete});
                }
            }
        }
    }

    std::sort(plan.begin(), plan.end(), component_less);
    if (plan.empty() || plan.size() > kMaxApplyPlanComponents) {
        if (err) *err = "release component plan has an invalid component count";
        return false;
    }
    return true;
}

const char* action_name(PlanAction action) {
    return action == PlanAction::Replace ? "replace" : "delete";
}

bool parse_action(const std::string& value, PlanAction& out) {
    if (value == "replace") {
        out = PlanAction::Replace;
        return true;
    }
    if (value == "delete") {
        out = PlanAction::Delete;
        return true;
    }
    return false;
}

bool write_apply_plan(const std::wstring& path, const UpdateInfo& info,
                      const std::vector<PlannedComponent>& plan, std::string* err) {
    Json root = Json::obj();
    root.set("schema_version", Json(kApplyPlanSchemaVersion));
    root.set("version", Json::str(info.version));
    Json components = Json::arr();
    for (const PlannedComponent& planned : plan) {
        Json item = Json::obj();
        item.set("path", Json::str(planned.component.path));
        item.set("action", Json::str(action_name(planned.action)));
        item.set("sha256", Json::str(planned.component.sha256));
        item.set("size", Json(planned.component.size));
        components.push(std::move(item));
    }
    root.set("components", std::move(components));
    return json_write_file(path, root, err);
}

bool read_apply_plan(const std::wstring& path, const std::string& launcher_name,
                     std::vector<PlannedComponent>& plan, std::string* err) {
    plan.clear();
    std::string text;
    if (!read_limited_text_file(path, text, err)) return false;
    std::string parse_error;
    const Json root = Json::parse(text, &parse_error);
    if (!parse_error.empty() || !root.is(Json::Type::Obj) ||
        !root.isMember("schema_version") || !root.get("schema_version").is(Json::Type::Num) ||
        root.get("schema_version").as_int(-1) != kApplyPlanSchemaVersion ||
        !root.isMember("version") || !root.get("version").is(Json::Type::Str) ||
        !safe_update_version(root.get("version").as_str()) ||
        !root.isMember("components") || !root.get("components").is(Json::Type::Arr) ||
        root.get("components").size() == 0 || root.get("components").size() > kMaxApplyPlanComponents) {
        if (err) *err = "last-known-good update plan is malformed";
        return false;
    }

    std::set<std::string> seen;
    std::string previous;
    for (const Json& item : root.get("components").items()) {
        if (!item.is(Json::Type::Obj) || !item.isMember("path") ||
            !item.isMember("action") || !item.isMember("sha256") || !item.isMember("size") ||
            !item.get("path").is(Json::Type::Str) || !item.get("action").is(Json::Type::Str) ||
            !item.get("sha256").is(Json::Type::Str) || !item.get("size").is(Json::Type::Num)) {
            if (err) *err = "last-known-good update plan has a malformed component";
            return false;
        }
        std::string normalized;
        if (!normalize_component_path(item.get("path").as_str(), launcher_name, normalized, err)) {
            return false;
        }
        PlanAction action;
        if (!parse_action(item.get("action").as_str(), action)) {
            if (err) *err = "last-known-good update plan has an invalid action";
            return false;
        }
        const std::string hash = lower_ascii(item.get("sha256").as_str());
        const double size_number = item.get("size").as_num(-1.0);
        const int64_t size = item.get("size").as_int(-1);
        if (!is_hex64(hash) || size < 0 || size > kMaxReleaseComponentBytes ||
            size_number != static_cast<double>(size)) {
            if (err) *err = "last-known-good update plan has invalid component metadata";
            return false;
        }
        const std::string key = component_key(normalized);
        if (!seen.insert(key).second || (!previous.empty() && key <= previous)) {
            if (err) *err = "last-known-good update plan is not a deterministic component list";
            return false;
        }
        previous = key;
        plan.push_back({{normalized, hash, size}, action});
    }
    return true;
}

std::wstring backup_marker_path(const std::wstring& backup_root, const wchar_t* category,
                                size_t index) {
    return backup_root + L"\\" + category + L"\\entry-" + std::to_wstring(index) + L".marker";
}

std::wstring quote_batch_path(const std::wstring& path) {
    return L"\"" + path + L"\"";
}

void append_ensure_directory(std::wstring& cmd, const std::wstring& directory,
                             const wchar_t* failure_label) {
    cmd += L"if not exist " + quote_batch_path(directory) + L" mkdir " +
           quote_batch_path(directory) + L"\r\n";
    cmd += L"if not exist " + quote_batch_path(directory) + L" goto " + failure_label + L"\r\n";
}

void append_backup_component(std::wstring& cmd, const std::wstring& exe_dir,
                             const std::wstring& backup_root,
                             const PlannedComponent& component, size_t index) {
    const std::wstring relative = net::to_wide(component.component.path);
    const std::wstring target = component_path(exe_dir, component.component.path);
    const std::wstring backup = component_path(backup_root, component.component.path);
    const std::wstring absent = backup_marker_path(backup_root, L".absent", index);
    const std::wstring ready = backup_marker_path(backup_root, L".ready", index);
    cmd += L"rem backup component " + relative + L"\r\n";
    cmd += L"if exist " + quote_batch_path(target) + L" (\r\n";
    append_ensure_directory(cmd, std::filesystem::path(backup).parent_path().wstring(), L"fail");
    cmd += L"copy /Y " + quote_batch_path(target) + L" " + quote_batch_path(backup) + L" >nul\r\n";
    cmd += L"if errorlevel 1 goto fail\r\n";
    cmd += L") else (\r\n";
    cmd += L">" + quote_batch_path(absent) + L" echo absent\r\n";
    cmd += L"if errorlevel 1 goto fail\r\n";
    cmd += L")\r\n";
    cmd += L">" + quote_batch_path(ready) + L" echo ready\r\n";
    cmd += L"if errorlevel 1 goto fail\r\n";
}

void append_apply_component(std::wstring& cmd, const std::wstring& exe_dir,
                            const std::wstring& stage_dir,
                            const PlannedComponent& component) {
    const std::wstring target = component_path(exe_dir, component.component.path);
    if (component.action == PlanAction::Replace) {
        const std::wstring staged = component_path(stage_dir, component.component.path);
        append_ensure_directory(cmd, std::filesystem::path(target).parent_path().wstring(), L"fail");
        cmd += L"copy /Y " + quote_batch_path(staged) + L" " + quote_batch_path(target) + L" >nul\r\n";
        cmd += L"if errorlevel 1 goto fail\r\n";
    } else {
        cmd += L"if exist " + quote_batch_path(target) + L" del /F /Q " +
               quote_batch_path(target) + L" >nul\r\n";
        cmd += L"if exist " + quote_batch_path(target) + L" goto fail\r\n";
    }
}

void append_rollback_component(std::wstring& cmd, const std::wstring& exe_dir,
                               const std::wstring& backup_root,
                               const PlannedComponent& component, size_t index) {
    const std::wstring target = component_path(exe_dir, component.component.path);
    const std::wstring backup = component_path(backup_root, component.component.path);
    const std::wstring absent = backup_marker_path(backup_root, L".absent", index);
    const std::wstring ready = backup_marker_path(backup_root, L".ready", index);
    cmd += L"if exist " + quote_batch_path(ready) + L" (\r\n";
    cmd += L"  if exist " + quote_batch_path(absent) + L" (\r\n";
    cmd += L"    if exist " + quote_batch_path(target) + L" del /F /Q " +
           quote_batch_path(target) + L" >nul\r\n";
    cmd += L"    if exist " + quote_batch_path(target) + L" set \"ROLLBACK_FAILED=1\"\r\n";
    cmd += L"  ) else (\r\n";
    const std::wstring parent = std::filesystem::path(target).parent_path().wstring();
    cmd += L"    if not exist " + quote_batch_path(parent) + L" mkdir " + quote_batch_path(parent) + L"\r\n";
    cmd += L"    if not exist " + quote_batch_path(parent) + L" set \"ROLLBACK_FAILED=1\"\r\n";
    cmd += L"    copy /Y " + quote_batch_path(backup) + L" " + quote_batch_path(target) + L" >nul\r\n";
    cmd += L"    if errorlevel 1 set \"ROLLBACK_FAILED=1\"\r\n";
    cmd += L"  )\r\n";
    cmd += L")\r\n";
}

bool write_update_helper(const std::wstring& batch, const std::wstring& exe_dir,
                         const std::wstring& exe_name, const std::wstring& stage_dir,
                         const std::wstring& apply_plan,
                         const std::vector<PlannedComponent>& plan, std::string* err) {
    const std::wstring backup_root = exe_dir + L"\\updates\\last-known-good";
    const std::wstring backup_plan = backup_root + L"\\component-plan.json";
    const std::wstring live_exe = exe_dir + L"\\" + exe_name;
    const std::wstring rollback_failure = exe_dir + L"\\updates\\rollback-failed.txt";
    std::wstring cmd;
    cmd += L"@echo off\r\n";
    cmd += L"setlocal DisableDelayedExpansion\r\n";
    cmd += L"rem Amalgam transactional full-release update helper\r\n";
    cmd += L":wait\r\n";
    cmd += L"tasklist /FI \"IMAGENAME eq " + exe_name + L"\" 2>nul | find /I \"" +
           exe_name + L"\" >nul\r\n";
    cmd += L"if %errorlevel%==0 ( timeout /t 1 /nobreak >nul & goto wait )\r\n";
    cmd += L"if exist " + quote_batch_path(rollback_failure) + L" del /F /Q " +
           quote_batch_path(rollback_failure) + L" >nul\r\n";
    cmd += L"if exist " + quote_batch_path(backup_root) + L" rmdir /S /Q " +
           quote_batch_path(backup_root) + L"\r\n";
    cmd += L"if exist " + quote_batch_path(backup_root) + L" goto preflight_fail\r\n";
    append_ensure_directory(cmd, backup_root, L"preflight_fail");
    append_ensure_directory(cmd, backup_root + L"\\.ready", L"preflight_fail");
    append_ensure_directory(cmd, backup_root + L"\\.absent", L"preflight_fail");
    cmd += L"copy /Y " + quote_batch_path(apply_plan) + L" " + quote_batch_path(backup_plan) + L" >nul\r\n";
    cmd += L"if errorlevel 1 goto preflight_fail\r\n";
    cmd += L">" + quote_batch_path(backup_root + L"\\.transaction-started.marker") +
           L" echo started\r\n";
    cmd += L"if errorlevel 1 goto preflight_fail\r\n";

    for (size_t i = 0; i < plan.size(); ++i) {
        append_backup_component(cmd, exe_dir, backup_root, plan[i], i);
    }
    for (const PlannedComponent& component : plan) {
        append_apply_component(cmd, exe_dir, stage_dir, component);
    }
    cmd += L"start \"\" " + quote_batch_path(live_exe) + L"\r\n";
    cmd += L"endlocal\r\n";
    cmd += L"exit /b 0\r\n";
    cmd += L":preflight_fail\r\n";
    cmd += L"start \"\" " + quote_batch_path(live_exe) + L"\r\n";
    cmd += L"endlocal\r\n";
    cmd += L"exit /b 1\r\n";
    cmd += L":fail\r\n";
    cmd += L"set \"ROLLBACK_FAILED=\"\r\n";
    for (size_t i = plan.size(); i > 0; --i) {
        append_rollback_component(cmd, exe_dir, backup_root, plan[i - 1], i - 1);
    }
    cmd += L"if defined ROLLBACK_FAILED >" + quote_batch_path(rollback_failure) +
           L" echo One or more product components could not be restored.\r\n";
    cmd += L"start \"\" " + quote_batch_path(live_exe) + L"\r\n";
    cmd += L"endlocal\r\n";
    cmd += L"exit /b 1\r\n";

    std::ofstream out(batch, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        if (err) *err = "could not write update helper batch";
        return false;
    }
    out << net::to_utf8(cmd);
    out.flush();
    out.close();
    if (!out) {
        if (err) *err = "could not finish writing update helper batch";
        return false;
    }
    return true;
}

bool ensure_target_parent(const std::wstring& path, std::string* error) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    if (ec) {
        if (error) *error = "could not create a rollback target directory: " + ec.message();
        return false;
    }
    return true;
}

bool restore_component_from_backup(const std::wstring& exe_dir, const std::wstring& backup_root,
                                   const PlannedComponent& component, size_t index,
                                   bool& restored, std::string* error) {
    restored = false;
    const std::wstring ready = backup_marker_path(backup_root, L".ready", index);
    if (!regular_file_info(ready)) return true;
    restored = true;

    const std::wstring target = component_path(exe_dir, component.component.path);
    const std::wstring absent = backup_marker_path(backup_root, L".absent", index);
    if (regular_file_info(absent)) {
        std::error_code ec;
        const std::filesystem::file_status status = std::filesystem::symlink_status(target, ec);
        if (ec) {
            if (error) *error = "could not inspect rollback target: " + component.component.path;
            return false;
        }
        if (!std::filesystem::exists(status)) return true;
        if (!std::filesystem::is_regular_file(status) || !DeleteFileW(target.c_str())) {
            if (error) *error = "could not remove a component absent before the update: " +
                                component.component.path;
            return false;
        }
        return true;
    }

    const std::wstring backup = component_path(backup_root, component.component.path);
    if (!regular_file_info(backup)) {
        if (error) *error = "last-known-good component is missing: " + component.component.path;
        return false;
    }
    if (!ensure_target_parent(target, error) || !CopyFileW(backup.c_str(), target.c_str(), FALSE)) {
        if (error && error->empty()) {
            *error = "could not restore last-known-good component: " + component.component.path +
                     " (error " + std::to_string(GetLastError()) + ")";
        }
        return false;
    }
    return true;
}

bool legacy_rollback_executable(const std::wstring& exe_dir, std::string& error) {
    wchar_t self[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, self, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        error = "cannot determine current executable path";
        return false;
    }
    const std::wstring exe_name = std::filesystem::path(self).filename().wstring();
    const std::wstring backup = exe_dir + L"\\updates\\" + exe_name + L".last-good";
    const std::wstring target = exe_dir + L"\\" + exe_name;
    if (!regular_file_info(backup)) {
        error = "no last-known-good update plan or legacy executable backup is available";
        return false;
    }
    if (!CopyFileW(backup.c_str(), target.c_str(), FALSE)) {
        error = "could not restore last-known-good executable (error " +
                std::to_string(GetLastError()) + ")";
        return false;
    }
    return true;
}

bool get_signing_public_key(std::string& out) {
    std::lock_guard<std::mutex> lock(g_key_mu);
    out = g_signing_public_key;
    return !out.empty();
}

// Validate the full trust boundary, not just the JSON shape.  This is used by
// both parsing and staging so an in-process caller cannot construct an
// UpdateInfo that bypasses the remote-manifest policy.
bool validate_authenticated_manifest(const UpdateInfo& info, std::string* err) {
    if (err) err->clear();

    if (info.version.empty() || info.download_url.empty()) {
        if (err) *err = "update manifest is missing version or download_url";
        return false;
    }
    if (!is_https_url(info.download_url)) {
        if (err) *err = "update payload URL must be HTTPS";
        return false;
    }
    if (!is_hex64(info.sha256)) {
        if (err) *err = "update manifest sha256 must be a 64-character hex digest";
        return false;
    }
    if (info.size <= 0 || info.size > kMaxUpdatePayloadBytes) {
        if (err) {
            *err = "update manifest size must be greater than zero and no larger than " +
                   std::to_string(kMaxUpdatePayloadBytes) + " bytes";
        }
        return false;
    }
    if (info.manifest_signature.empty()) {
        if (err) *err = "update manifest is missing its required signature";
        return false;
    }

    std::string key;
    if (!get_signing_public_key(key)) {
        if (err) {
            *err =
                "update manifest cannot be verified because no update-signing public key "
                "is configured";
        }
        return false;
    }
    return verify_manifest_signature(key, canonical_manifest_json(info),
                                     info.manifest_signature, err);
}

// Split "3.0.1-beta.2" into leading numeric segments and a prerelease tag.
struct ParsedVersion {
    std::vector<int> nums;
    std::string prerelease;  // e.g. "beta.2" or "" for a release build
};

bool parse_version(const std::string& text, ParsedVersion& out) {
    std::string s = text;
    const size_t dash = s.find('-');
    if (dash != std::string::npos) {
        out.prerelease = s.substr(dash + 1);
        s = s.substr(0, dash);
    }
    std::istringstream stream(s);
    std::string part;
    while (std::getline(stream, part, '.')) {
        if (part.empty()) return false;
        int value = 0;
        for (char c : part) {
            if (!std::isdigit(static_cast<unsigned char>(c))) return false;
            value = value * 10 + (c - '0');
            if (value > 1000000) return false;  // sane bound
        }
        out.nums.push_back(value);
    }
    if (out.nums.empty()) return false;
    return true;
}

}  // namespace

int compare_versions(const std::string& a, const std::string& b) {
    ParsedVersion pa, pb;
    if (!parse_version(a, pa) || !parse_version(b, pb)) {
        // Fall back to plain string comparison for unparseable versions so a
        // malformed manifest can never be treated as "up to date".
        const int cmp = a.compare(b);
        return cmp < 0 ? -1 : cmp > 0 ? 1 : 0;
    }
    const size_t common = pa.nums.size() < pb.nums.size() ? pa.nums.size() : pb.nums.size();
    for (size_t i = 0; i < common; ++i) {
        if (pa.nums[i] != pb.nums[i]) return pa.nums[i] < pb.nums[i] ? -1 : 1;
    }
    if (pa.nums.size() != pb.nums.size()) return pa.nums.size() < pb.nums.size() ? -1 : 1;
    // Same numeric version: a release build beats a prerelease build.
    const bool a_pre = !pa.prerelease.empty();
    const bool b_pre = !pb.prerelease.empty();
    if (a_pre != b_pre) return a_pre ? -1 : 1;
    if (a_pre) {
        // Semver-style prerelease ordering: "beta.2" < "beta.10" because the
        // trailing identifiers are compared numerically.
        std::istringstream as(pa.prerelease);
        std::istringstream bs(pb.prerelease);
        std::string aseg, bseg;
        while (std::getline(as, aseg, '.')) {
            if (!std::getline(bs, bseg, '.')) return 1;  // a has more segments
            const bool a_num = !aseg.empty() &&
                std::all_of(aseg.begin(), aseg.end(), [](char c) {
                    return std::isdigit(static_cast<unsigned char>(c));
                });
            const bool b_num = !bseg.empty() &&
                std::all_of(bseg.begin(), bseg.end(), [](char c) {
                    return std::isdigit(static_cast<unsigned char>(c));
                });
            if (a_num && b_num) {
                const long av = std::strtol(aseg.c_str(), nullptr, 10);
                const long bv = std::strtol(bseg.c_str(), nullptr, 10);
                if (av != bv) return av < bv ? -1 : 1;
            } else {
                const int cmp = aseg.compare(bseg);
                if (cmp != 0) return cmp < 0 ? -1 : 1;
            }
        }
        if (std::getline(bs, bseg, '.')) return -1;  // b has more segments
        return 0;
    }
    return 0;
}

bool should_apply(const UpdateInfo& info, const std::string& current_version,
                  const std::string& current_channel, std::string* err) {
    if (err) err->clear();
    // Anti-rollback: never apply a version that is not strictly newer.
    if (compare_versions(info.version, current_version) <= 0) {
        if (err) {
            *err = "refusing an update that is not newer than the running version";
        }
        return false;
    }
    if (!info.min_version.empty() &&
        compare_versions(current_version, info.min_version) < 0) {
        if (err) {
            *err = "the running launcher version is too old for this update";
        }
        return false;
    }
    // Channel crossing guard: a stable build must not silently move to a
    // prerelease channel via an update.
    if (current_channel == "stable" && info.channel == "beta") {
        if (err) {
            *err = "refusing to move from the stable channel to beta";
        }
        return false;
    }
    return true;
}

std::string canonical_manifest_json(const UpdateInfo& info) {
    // Fixed key order shared with the release tooling (tools/update-manifest.mjs).
    // Json::dump is compact and preserves insertion order, matching
    // JSON.stringify for the same object; this must stay byte-identical.
    Json j = Json::obj();
    j.set("channel", Json::str(info.channel));
    j.set("download_url", Json::str(info.download_url));
    j.set("mandatory", Json::boolean(info.mandatory));
    j.set("min_version", Json::str(info.min_version));
    j.set("notes", Json::str(info.notes));
    j.set("sha256", Json::str(info.sha256));
    j.set("size", Json(static_cast<int64_t>(info.size)));
    j.set("version", Json::str(info.version));
    return j.dump();
}

std::wstring default_manifest_url() {
    // Backend-owned release manifest; the schema below must be preserved.
    return L"https://amalgam-mc.com/releases/launcher/manifest.json";
}

bool parse_manifest_text(const std::string& text, UpdateInfo& out, std::string* err) {
    if (err) err->clear();

    // A host that answers with its own web page (unpublished feed, error page,
    // captive portal) is the common case of a non-JSON body. Naming that says
    // why no update can be verified; the JSON parser's "unexpected char '<'"
    // does not. Still fails closed either way.
    size_t first = text.find_first_not_of(" \t\r\n\xEF\xBB\xBF");
    if (first != std::string::npos && text[first] == '<') {
        if (err)
            *err =
                "release server returned a web page instead of a signed update "
                "manifest";
        return false;
    }

    Json root;
    std::string parse_error;
    root = Json::parse(text, &parse_error);
    if (!parse_error.empty()) {
        if (err) *err = "update manifest is malformed: " + parse_error;
        return false;
    }
    if (!root.is(Json::Type::Obj)) {
        if (err) *err = "update manifest is not an object";
        return false;
    }

    UpdateInfo info;
    info.version = root.get("version").as_str();
    info.channel = root.get("channel").as_str();
    info.download_url = root.get("download_url").as_str();
    info.sha256 = root.get("sha256").as_str();
    info.signature = root.get("signature").as_str();
    info.manifest_signature = root.get("manifest_signature").as_str();
    info.min_version = root.get("min_version").as_str();
    info.notes = root.get("notes").as_str();
    info.size = root.get("size").as_int();
    info.mandatory = root.get("mandatory").as_bool();

    if (!validate_authenticated_manifest(info, err)) return false;

    out = std::move(info);
    return true;
}

bool fetch_manifest(const std::wstring& url, UpdateInfo& out, std::string* err) {
    if (err) err->clear();
    if (!net::validate_url(url, err)) return false;

    std::vector<uint8_t> body;
    if (!net::get(url, body, err)) {
        if (err && err->empty()) *err = "update manifest request failed";
        return false;
    }

    return parse_manifest_text(std::string(body.begin(), body.end()), out, err);
}

void set_signing_public_key(const std::string& pem) {
    std::lock_guard<std::mutex> lock(g_key_mu);
    g_signing_public_key = pem;
}

void init_from_embedded_key() {
#ifdef AMALGAM_HAS_EMBEDDED_UPDATER_KEY
    if constexpr (sizeof(aml::updater::detail::kUpdaterPublicKeyPem) > 8) {
        set_signing_public_key(aml::updater::detail::kUpdaterPublicKeyPem);
    }
#endif
}

bool has_signing_key() {
    std::string key;
    return get_signing_public_key(key);
}

namespace {

using Feed = std::function<bool(HCRYPTHASH)>;

// Core RSA-SHA256 verification shared by the payload and manifest paths:
// decode the base64 signature, import the PEM public key, create an SHA-256
// hash, feed it through `feed`, then verify the signature over it.
bool verify_rsa_sha256(const std::string& pem_public_key,
                       const std::string& base64_signature,
                       const Feed& feed, const char* what, std::string* err) {
    if (err) err->clear();

    // Decode the base64 signature.
    std::vector<uint8_t> sig;
    DWORD decoded_len = 0;
    if (!CryptStringToBinaryA(base64_signature.c_str(),
                              static_cast<DWORD>(base64_signature.size()),
                              CRYPT_STRING_BASE64, nullptr, &decoded_len,
                              nullptr, nullptr) ||
        decoded_len == 0) {
        if (err) *err = std::string(what) + " signature is not valid base64";
        return false;
    }
    sig.resize(decoded_len);
    if (!CryptStringToBinaryA(base64_signature.c_str(),
                              static_cast<DWORD>(base64_signature.size()),
                              CRYPT_STRING_BASE64, sig.data(), &decoded_len,
                              nullptr, nullptr)) {
        if (err) *err = std::string(what) + " signature is not valid base64";
        return false;
    }
    sig.resize(decoded_len);

    // Decode the PEM public key into a CERT_PUBLIC_KEY_INFO.
    std::vector<uint8_t> der;
    DWORD der_len = 0;
    if (!CryptStringToBinaryA(pem_public_key.c_str(),
                              static_cast<DWORD>(pem_public_key.size()),
                              CRYPT_STRING_BASE64HEADER, nullptr, &der_len,
                              nullptr, nullptr) ||
        der_len == 0) {
        if (err) *err = "update signing key is not a valid PEM public key";
        return false;
    }
    der.resize(der_len);
    if (!CryptStringToBinaryA(pem_public_key.c_str(),
                              static_cast<DWORD>(pem_public_key.size()),
                              CRYPT_STRING_BASE64HEADER, der.data(), &der_len,
                              nullptr, nullptr)) {
        if (err) *err = "update signing key is not a valid PEM public key";
        return false;
    }
    der.resize(der_len);

    DWORD pub_len = 0;
    if (!CryptDecodeObjectEx(X509_ASN_ENCODING, X509_PUBLIC_KEY_INFO,
                             der.data(), der_len, 0, nullptr, nullptr,
                             &pub_len) ||
        pub_len == 0) {
        if (err) *err = "update signing key is not a valid RSA public key";
        return false;
    }
    std::vector<uint8_t> pub_buf(pub_len);
    if (!CryptDecodeObjectEx(X509_ASN_ENCODING, X509_PUBLIC_KEY_INFO,
                             der.data(), der_len, 0, nullptr, pub_buf.data(),
                             &pub_len)) {
        if (err) *err = "update signing key is not a valid RSA public key";
        return false;
    }
    CERT_PUBLIC_KEY_INFO* pub_info =
        reinterpret_cast<CERT_PUBLIC_KEY_INFO*>(pub_buf.data());

    HCRYPTPROV h_prov = 0;
    HCRYPTKEY h_key = 0;
    HCRYPTHASH h_hash = 0;
    bool ok = false;
    if (!CryptAcquireContextW(&h_prov, nullptr, MS_ENH_RSA_AES_PROV, PROV_RSA_AES,
                              CRYPT_VERIFYCONTEXT)) {
        if (err && err->empty()) {
            *err = "signature verify: could not acquire provider (error " +
                   std::to_string(GetLastError()) + ")";
        }
    } else if (!CryptImportPublicKeyInfo(h_prov, X509_ASN_ENCODING, pub_info,
                                         &h_key)) {
        if (err && err->empty()) {
            *err = "signature verify: could not import public key (error " +
                   std::to_string(GetLastError()) + ")";
        }
    } else if (!CryptCreateHash(h_prov, CALG_SHA_256, 0, 0, &h_hash)) {
        if (err && err->empty()) {
            *err = "signature verify: could not create hash (error " +
                   std::to_string(GetLastError()) + ")";
        }
    } else if (!feed(h_hash)) {
        if (err && err->empty()) {
            *err = "could not feed data for signature verification";
        }
    } else {
        // CryptVerifySignature expects the historic little-endian signature
        // byte order, while OpenSSL/Node (the release tooling) emits the
        // standard big-endian PKCS#1 order. Both encodings prove possession
        // of the same private key, so accept either: verify as-is first,
        // then retry with the byte order flipped.
        if (CryptVerifySignature(h_hash, sig.data(),
                                 static_cast<DWORD>(sig.size()),
                                 h_key, nullptr, 0)) {
            ok = true;
        } else {
            std::vector<uint8_t> flipped(sig.rbegin(), sig.rend());
            if (CryptVerifySignature(h_hash, flipped.data(),
                                     static_cast<DWORD>(flipped.size()),
                                     h_key, nullptr, 0)) {
                ok = true;
            }
        }
        if (!ok && err && err->empty()) {
            *err = std::string(what) + " signature verification failed";
        }
    }

    if (h_hash) CryptDestroyHash(h_hash);
    if (h_key) CryptDestroyKey(h_key);
    if (h_prov) CryptReleaseContext(h_prov, 0);
    return ok;
}

}  // namespace

bool verify_payload_signature(const std::string& pem_public_key,
                              const std::string& base64_signature,
                              const std::wstring& payload_path,
                              std::string* err) {
    return verify_rsa_sha256(
        pem_public_key, base64_signature,
        [&](HCRYPTHASH h) {
            // Hash the payload file content in bounded chunks.
            std::ifstream file(payload_path, std::ios::binary);
            if (!file.is_open()) return false;
            char buffer[64 * 1024];
            bool ok = true;
            while (file.good() && !file.eof()) {
                file.read(buffer, sizeof(buffer));
                const std::streamsize got = file.gcount();
                if (got > 0 &&
                    !CryptHashData(h, reinterpret_cast<const BYTE*>(buffer),
                                   static_cast<DWORD>(got), 0)) {
                    ok = false;
                    break;
                }
            }
            return ok && (file.eof() || file.good());
        },
        "update payload", err);
}

bool verify_manifest_signature(const std::string& pem_public_key,
                               const std::string& canonical_json,
                               const std::string& base64_signature,
                               std::string* err) {
    return verify_rsa_sha256(
        pem_public_key, base64_signature,
        [&](HCRYPTHASH h) {
            return CryptHashData(h,
                                 reinterpret_cast<const BYTE*>(canonical_json.data()),
                                 static_cast<DWORD>(canonical_json.size()), 0) != 0;
        },
        "update manifest", err);
}

namespace {

// The manifest signature is mandatory and has already authenticated the
// hash, size, URL, and version before staging starts.  A payload signature is
// optional defense in depth; when one is present it must verify with the same
// configured public key.
bool signature_policy_ok(const UpdateInfo& info, const std::wstring& payload_path,
                         std::string* err) {
    if (info.signature.empty()) return true;
    std::string key;
    if (!get_signing_public_key(key)) {
        if (err) *err = "update payload cannot be verified because no update-signing "
                        "public key is configured";
        return false;
    }
    return verify_payload_signature(key, info.signature, payload_path, err);
}

}  // namespace

bool stage_update(const UpdateInfo& info, const std::wstring& exe_dir,
                  std::wstring& staged_path, std::string* err) {
    if (err) err->clear();

    // The staged filename embeds the manifest version. Defense in depth:
    // a hostile version string must never be able to steer the payload
    // outside the staging directory.
    if (!safe_update_version(info.version)) {
        if (err) *err = "manifest version contains unsafe characters";
        return false;
    }

    // Keep this check at the staging boundary as well as parse time.  All
    // normal callers receive UpdateInfo from fetch_manifest(), but rechecking
    // protects against future in-process call sites or accidental mutation
    // between fetch and apply.
    if (!validate_authenticated_manifest(info, err)) return false;

    const std::wstring updates_dir = exe_dir + L"\\updates";
    if (!net::mkdirs(updates_dir)) {
        if (err) *err = "could not create update staging directory";
        return false;
    }

    const std::wstring target = updates_dir + L"\\amalgam-" +
                                net::to_wide(info.version) + L".zip";

    // Reuse a previously staged payload when it already passes verification.
    if (net::file_exists(target)) {
        const bool size_ok = net::file_size(target) == static_cast<uint64_t>(info.size);
        const bool hash_ok = net::sha256_file(target) == info.sha256;
        if (size_ok && hash_ok) {
            std::string sig_err;
            if (!signature_policy_ok(info, target, &sig_err)) {
                if (err) *err = sig_err;
                return false;
            }
            staged_path = target;
            return true;
        }
        // Stale or tampered staging: remove it and re-download.
        DeleteFileW(target.c_str());
    }

    // Verify size + SHA-256 during download; a mismatch deletes the payload.
    if (!net::download(net::to_wide(info.download_url), target, nullptr, err,
                       std::string(), info.size, info.sha256)) {
        return false;
    }

    // Enforce the signature policy over the freshly downloaded payload.
    {
        std::string sig_err;
        if (!signature_policy_ok(info, target, &sig_err)) {
            if (err) *err = sig_err;
            DeleteFileW(target.c_str());
            return false;
        }
    }
    staged_path = target;
    return true;
}

bool prepare_apply(const UpdateInfo& info, const std::wstring& exe_dir,
                   const std::wstring& staged_path, std::wstring& helper_path,
                   std::string* err) {
    if (err) err->clear();

    // Revalidate both trust boundaries here. stage_update normally performed
    // these checks already, but prepare_apply is public and must not let a
    // future in-process caller turn an arbitrary ZIP into a replacement plan.
    if (!safe_update_version(info.version)) {
        if (err) *err = "manifest version contains unsafe characters";
        return false;
    }
    if (!validate_authenticated_manifest(info, err)) return false;
    uintmax_t staged_size = 0;
    if (!regular_file_info(staged_path, &staged_size) ||
        staged_size != static_cast<uintmax_t>(info.size) ||
        lower_ascii(net::sha256_file(staged_path)) != lower_ascii(info.sha256)) {
        if (err) *err = "staged update payload no longer matches its signed manifest";
        return false;
    }
    if (!signature_policy_ok(info, staged_path, err)) return false;

    std::error_code ec;
    if (!std::filesystem::is_directory(std::filesystem::path(exe_dir), ec) || ec) {
        if (err) *err = "launcher installation directory is unavailable";
        return false;
    }

    wchar_t self[MAX_PATH]{};
    const DWORD self_length = GetModuleFileNameW(nullptr, self, MAX_PATH);
    if (self_length == 0 || self_length >= MAX_PATH) {
        if (err) *err = "cannot determine current executable path";
        return false;
    }
    const std::wstring exe_name = std::filesystem::path(self).filename().wstring();
    const std::string launcher_name = net::to_utf8(exe_name);

    // Extract the authenticated archive only underneath the protected update
    // working directory. Every extracted file is later checked against the
    // release component manifest and inventory before a helper is generated.
    const std::wstring stage_dir = exe_dir + L"\\updates\\staged-" +
                                   net::to_wide(info.version);
    std::filesystem::remove_all(stage_dir, ec);
    if (ec || !net::mkdirs(stage_dir)) {
        if (err) *err = "could not create extraction directory";
        return false;
    }
    if (!extract::zip(staged_path, stage_dir, err)) return false;

    std::vector<ReleaseComponent> release_components;
    if (!validate_release_payload(stage_dir, launcher_name, release_components, err)) {
        return false;
    }

    std::vector<PlannedComponent> plan;
    if (!build_apply_plan(exe_dir, launcher_name, release_components, plan, err)) return false;

    const std::wstring updates_dir = exe_dir + L"\\updates";
    const std::wstring apply_plan = updates_dir + L"\\apply-plan.json";
    if (!write_apply_plan(apply_plan, info, plan, err)) return false;

    const std::wstring batch = updates_dir + L"\\apply-update.bat";
    if (!write_update_helper(batch, exe_dir, exe_name, stage_dir, apply_plan, plan, err)) {
        return false;
    }
    helper_path = batch;
    return true;
}

bool rollback_update(const std::wstring& exe_dir, std::string& error) {
    error.clear();

    wchar_t self[MAX_PATH]{};
    const DWORD self_length = GetModuleFileNameW(nullptr, self, MAX_PATH);
    if (self_length == 0 || self_length >= MAX_PATH) {
        error = "cannot determine current executable path";
        return false;
    }
    const std::string launcher_name = net::to_utf8(std::filesystem::path(self).filename().wstring());
    const std::wstring backup_root = exe_dir + L"\\updates\\last-known-good";
    const std::wstring plan_path = backup_root + L"\\component-plan.json";
    if (!regular_file_info(plan_path)) return legacy_rollback_executable(exe_dir, error);

    std::vector<PlannedComponent> plan;
    if (!read_apply_plan(plan_path, launcher_name, plan, &error)) return false;

    bool saw_ready_component = false;
    bool all_restored = true;
    std::string failures;
    for (size_t i = plan.size(); i > 0; --i) {
        bool restored = false;
        std::string component_error;
        if (!restore_component_from_backup(exe_dir, backup_root, plan[i - 1], i - 1,
                                           restored, &component_error)) {
            all_restored = false;
            if (!failures.empty()) failures += "; ";
            failures += component_error.empty()
                ? "could not restore " + plan[i - 1].component.path
                : component_error;
        }
        saw_ready_component = saw_ready_component || restored;
    }
    if (!saw_ready_component) {
        error = "last-known-good update backup has no completed component snapshots";
        return false;
    }
    if (!all_restored) {
        error = failures.empty() ? "some last-known-good components could not be restored" : failures;
        return false;
    }
    return true;
}

}  // namespace aml::updater
