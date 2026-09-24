#include "profile_handoff.h"

#include "model.h"

#include <cctype>

namespace aml::profile_handoff {

namespace {

std::string normalize_loader(std::string loader) {
    for (char& ch : loader)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return loader;
}

bool is_supported_modded_loader(const std::string& loader) {
    return loader == "fabric" || loader == "quilt" || loader == "forge" ||
           loader == "neoforge";
}

bool is_exact_loader_version(const std::string& value) {
    if (value.empty()) return false;
    for (unsigned char ch : value) {
        if (!std::isalnum(ch) && ch != '.' && ch != '-' && ch != '_' && ch != '+')
            return false;
    }
    return true;
}

bool persist_resolved_profile(instances::Instance& instance,
                              const instances::Instance& original,
                              const instances::ProfileIdentitySnapshot& identity,
                              std::string* error) {
    if (instance.directory.empty()) {
        instance = original;
        if (error) *error = "profile has no directory in which to save its loader version";
        return false;
    }
    std::string identity_error;
    if (!instances::profile_identity_matches(instance, identity, &identity_error)) {
        instance = original;
        if (error) {
            *error = identity_error.empty()
                ? "the selected profile changed before its loader version could be saved"
                : identity_error;
        }
        return false;
    }
    std::string save_error;
    if (!instances::save(instance, &save_error)) {
        instance = original;
        if (error) {
            *error = save_error.empty() ? "could not save the resolved loader version"
                                        : save_error;
        }
        return false;
    }
    return true;
}

}  // namespace

bool ensure_loader_version(instances::Instance& instance,
                           const LoaderVersionResolver& resolver,
                           std::string* error) {
    if (error) error->clear();

    const std::string loader = normalize_loader(instance.loader);
    if (loader.empty() || loader == "vanilla") return true;
    if (loader == "auto") {
        if (error) {
            *error = "the profile must select a concrete mod loader before official-launcher handoff";
        }
        return false;
    }
    if (!is_supported_modded_loader(loader)) {
        if (error) *error = "unsupported client loader: " + instance.loader;
        return false;
    }
    if (instance.minecraft_version.empty()) {
        if (error) *error = "the profile is missing its Minecraft version";
        return false;
    }
    if (!instance.loader_version.empty() && !is_exact_loader_version(instance.loader_version)) {
        if (error) *error = "the profile has an invalid pinned loader version";
        return false;
    }

    const instances::Instance original = instance;
    instances::ProfileIdentitySnapshot identity;
    if (!instances::capture_profile_identity(instance, identity, error)) return false;
    if (!instance.loader_version.empty()) {
        // A pinned version is already exact. Persist only normalisation needed
        // by the official launcher's strict lowercase loader identifiers.
        instance.loader = loader;
        return instance.loader == original.loader ||
                   persist_resolved_profile(instance, original, identity, error);
    }
    if (!resolver) {
        if (error) *error = "no loader-version resolver is available";
        return false;
    }

    std::string resolved;
    std::string resolve_error;
    if (!resolver(instance.minecraft_version, loader, &resolved, &resolve_error)) {
        if (error) {
            *error = resolve_error.empty()
                ? "could not resolve an exact " + loader + " loader version"
                : resolve_error;
        }
        return false;
    }
    if (!is_exact_loader_version(resolved)) {
        if (error) *error = "the loader resolver returned an invalid exact version";
        return false;
    }

    instance.loader = loader;
    instance.loader_version = resolved;
    return persist_resolved_profile(instance, original, identity, error);
}

bool ensure_loader_version(instances::Instance& instance, std::string* error) {
    return ensure_loader_version(
        instance,
        [](const std::string& minecraft_version, const std::string& loader,
           std::string* loader_version, std::string* resolve_error) {
            return model::resolve_loader_version(minecraft_version, loader, loader_version,
                                                 resolve_error);
        },
        error);
}

}  // namespace aml::profile_handoff
