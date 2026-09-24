#pragma once

#include "instances.h"

#include <functional>
#include <string>

namespace aml::profile_handoff {

// Resolves one exact loader version for a concrete Minecraft/loader pair.  It
// is injectable so profile persistence can be verified without contacting a
// loader metadata service.
using LoaderVersionResolver = std::function<bool(const std::string& minecraft_version,
                                                 const std::string& loader,
                                                 std::string* loader_version,
                                                 std::string* error)>;

// Ensure a profile that will be handed to the official Minecraft Launcher has
// an exact, persisted loader version. Vanilla profiles require no loader;
// Fabric, Quilt, Forge, and NeoForge profiles retain a valid existing pin or
// resolve one through `resolver` and atomically persist it before handoff.
// Unsupported/automatic loader selections, invalid pins, resolver failures,
// and persistence failures are reported without mutating the caller's profile.
bool ensure_loader_version(instances::Instance& instance,
                           const LoaderVersionResolver& resolver,
                           std::string* error = nullptr);

// Production overload using the launcher's normal loader metadata resolver.
bool ensure_loader_version(instances::Instance& instance, std::string* error = nullptr);

}  // namespace aml::profile_handoff
