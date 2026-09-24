#include "instances.h"
#include "profile_handoff.h"

#include <windows.h>

#include <filesystem>
#include <iostream>
#include <string>

namespace {

namespace fs = std::filesystem;

bool fail(const std::string& message) {
    std::cerr << "FAILED: " << message << "\n";
    return false;
}

fs::path make_temp_root() {
    wchar_t temp[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, temp) == 0) return {};
    wchar_t name[MAX_PATH]{};
    if (GetTempFileNameW(temp, L"amh", 0, name) == 0) return {};
    DeleteFileW(name);
    std::error_code ec;
    fs::create_directories(name, ec);
    return ec ? fs::path{} : fs::path(name);
}

bool test_loader_family_persistence() {
    struct Case {
        const char* loader;
        const char* minecraft_version;
        const char* resolved_version;
    };
    const Case cases[] = {
        {"fabric", "1.21.1", "0.16.14"},
        {"quilt", "1.21.1", "0.20.0-beta.9"},
        {"forge", "1.20.1", "47.4.15"},
        {"neoforge", "1.21.1", "21.1.173"},
    };

    const fs::path root = make_temp_root();
    if (root.empty()) return fail("could not create a temporary profile root");
    bool ok = true;
    for (const Case& item : cases) {
        aml::instances::Instance source;
        source.id = std::string(item.loader) + "-profile";
        source.name = source.id;
        source.minecraft_version = item.minecraft_version;
        source.loader = item.loader;

        aml::instances::Instance created;
        std::string error;
        if (!aml::instances::create(root.wstring(), source, created, &error)) {
            ok = fail(std::string(item.loader) + " profile creation: " + error);
            break;
        }

        int resolver_calls = 0;
        const bool resolved = aml::profile_handoff::ensure_loader_version(
            created,
            [&](const std::string& minecraft_version, const std::string& loader,
                std::string* loader_version, std::string* resolver_error) {
                ++resolver_calls;
                if (minecraft_version != item.minecraft_version || loader != item.loader) {
                    if (resolver_error) *resolver_error = "wrong loader family passed to resolver";
                    return false;
                }
                *loader_version = item.resolved_version;
                return true;
            },
            &error);
        if (!resolved || resolver_calls != 1 || created.loader != item.loader ||
            created.loader_version != item.resolved_version) {
            ok = fail(std::string(item.loader) + " loader version was not resolved and retained: " + error);
            break;
        }

        aml::instances::Instance persisted;
        if (!aml::instances::load(created.directory, persisted, &error) ||
            persisted.loader != item.loader || persisted.loader_version != item.resolved_version) {
            ok = fail(std::string(item.loader) + " loader version was not persisted: " + error);
            break;
        }
    }
    std::error_code ec;
    fs::remove_all(root, ec);
    return ok;
}

bool test_pinned_version_is_preserved() {
    const fs::path root = make_temp_root();
    if (root.empty()) return fail("could not create pinned-version profile root");

    aml::instances::Instance source;
    source.id = "pinned-forge";
    source.name = "Pinned Forge";
    source.minecraft_version = "1.20.1";
    source.loader = "forge";
    source.loader_version = "47.4.15";
    aml::instances::Instance created;
    std::string error;
    bool ok = aml::instances::create(root.wstring(), source, created, &error);
    bool resolver_called = false;
    if (ok) {
        ok = aml::profile_handoff::ensure_loader_version(
            created,
            [&](const std::string&, const std::string&, std::string*, std::string*) {
                resolver_called = true;
                return false;
            },
            &error);
    }
    if (!ok || resolver_called || created.loader_version != "47.4.15") {
        ok = fail("pinned loader version must be retained without a resolver call: " + error);
    }

    std::error_code ec;
    fs::remove_all(root, ec);
    return ok;
}

bool test_failed_resolution_does_not_persist_partial_metadata() {
    const fs::path root = make_temp_root();
    if (root.empty()) return fail("could not create failure-fixture profile root");

    aml::instances::Instance source;
    source.id = "offline-fabric";
    source.name = "Offline Fabric";
    source.minecraft_version = "1.21.1";
    source.loader = "fabric";
    aml::instances::Instance created;
    std::string error;
    bool ok = aml::instances::create(root.wstring(), source, created, &error);
    if (ok) {
        ok = !aml::profile_handoff::ensure_loader_version(
            created,
            [](const std::string&, const std::string&, std::string*, std::string* resolver_error) {
                if (resolver_error) *resolver_error = "metadata service unavailable";
                return false;
            },
            &error);
    }
    const std::string resolution_error = error;
    aml::instances::Instance persisted;
    if (ok) ok = aml::instances::load(created.directory, persisted, &error);
    if (!ok || !created.loader_version.empty() || !persisted.loader_version.empty() ||
        resolution_error.find("metadata service unavailable") == std::string::npos) {
        ok = fail("failed resolution must leave profile metadata unchanged: " + resolution_error);
    }

    std::error_code ec;
    fs::remove_all(root, ec);
    return ok;
}

bool test_profile_identity_snapshot_rejects_stale_selection() {
    const fs::path root = make_temp_root();
    if (root.empty()) return fail("could not create identity snapshot profile root");

    aml::instances::Instance source;
    source.id = "identity-profile";
    source.name = "Identity Profile";
    source.minecraft_version = "1.21.1";
    source.loader = "fabric";
    aml::instances::Instance created;
    std::string error;
    bool ok = aml::instances::create(root.wstring(), source, created, &error);
    aml::instances::ProfileIdentitySnapshot snapshot;
    if (ok) ok = aml::instances::capture_profile_identity(created, snapshot, &error);

    // Simulate a profile edit or replacement after a destructive/recovery
    // confirmation was opened. The old selection must not remain valid.
    aml::instances::Instance changed = created;
    changed.name = "Identity Profile Changed While Dialog Was Open";
    if (ok) ok = aml::instances::save(changed, &error);
    error.clear();
    if (ok && (aml::instances::profile_identity_matches(created, snapshot, &error) || error.empty())) {
        ok = fail("stale profile identity snapshot was accepted");
    }

    std::error_code ec;
    fs::remove_all(root, ec);
    return ok;
}

bool test_recovery_move_rejects_stale_profile_selection() {
    const fs::path root = make_temp_root();
    if (root.empty()) return fail("could not create recovery move profile root");

    aml::instances::Instance source;
    source.id = "recovery-race";
    source.name = "Recovery Race";
    source.minecraft_version = "1.21.1";
    source.loader = "fabric";
    aml::instances::Instance created;
    std::string error;
    bool ok = aml::instances::create(root.wstring(), source, created, &error);

    // The UI retains this old value while a confirmation is open. Replacing
    // its metadata must make the commit fail before the folder can be moved.
    aml::instances::Instance changed = created;
    changed.name = "Replacement selected while recovery confirmation was open";
    if (ok) ok = aml::instances::save(changed, &error);
    error.clear();
    const bool moved = ok && aml::instances::remove(created, &error);
    const fs::path recovery_root = root / L".amalgam-profile-recovery";
    if (moved || !fs::exists(created.directory) || fs::exists(recovery_root) || error.empty()) {
        ok = fail("stale profile selection was moved to recovery: " + error);
    }

    std::error_code ec;
    fs::remove_all(root, ec);
    return ok;
}

bool test_handoff_does_not_overwrite_profile_changed_during_resolution() {
    const fs::path root = make_temp_root();
    if (root.empty()) return fail("could not create handoff race profile root");

    aml::instances::Instance source;
    source.id = "handoff-race";
    source.name = "Before metadata resolution";
    source.minecraft_version = "1.21.1";
    source.loader = "fabric";
    aml::instances::Instance created;
    std::string error;
    bool ok = aml::instances::create(root.wstring(), source, created, &error);
    if (ok) {
        ok = !aml::profile_handoff::ensure_loader_version(
            created,
            [&](const std::string&, const std::string&, std::string* loader_version,
                std::string*) {
                aml::instances::Instance replacement;
                std::string replacement_error;
                if (!aml::instances::load(created.directory, replacement, &replacement_error)) return false;
                replacement.name = "Profile changed while loader metadata was resolving";
                if (!aml::instances::save(replacement, &replacement_error)) return false;
                *loader_version = "0.16.14";
                return true;
            },
            &error);
    }

    aml::instances::Instance persisted;
    std::string load_error;
    if (ok) ok = aml::instances::load(created.directory, persisted, &load_error);
    if (!ok || !created.loader_version.empty() || !persisted.loader_version.empty() ||
        persisted.name != "Profile changed while loader metadata was resolving" || error.empty()) {
        ok = fail("handoff overwrote a profile changed during resolution: " + error + "; " + load_error);
    }

    std::error_code ec;
    fs::remove_all(root, ec);
    return ok;
}

}  // namespace

int main() {
    const bool ok = test_loader_family_persistence() &&
                    test_pinned_version_is_preserved() &&
                    test_failed_resolution_does_not_persist_partial_metadata() &&
                    test_profile_identity_snapshot_rejects_stale_selection() &&
                    test_recovery_move_rejects_stale_profile_selection() &&
                    test_handoff_does_not_overwrite_profile_changed_during_resolution();
    if (ok) std::cout << "profile handoff tests passed\n";
    return ok ? 0 : 1;
}
