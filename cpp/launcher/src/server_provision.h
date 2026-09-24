#pragma once

// Getting a local server's runtime onto the disk.
//
// The Servers page owns the job, the progress bar and the buttons; this owns
// what an install actually does, so the same steps serve a brand-new server and
// a server whose files were deleted.

#include "server_providers.h"
#include "server_types.h"

#include <functional>
#include <string>

namespace aml::server_provision {

// Reads a Java server's persisted Minecraft EULA acknowledgement. Native
// Bedrock Dedicated Server has no EULA and is therefore always eligible.
bool has_accepted_eula(const server::ServerConfig& server);

// Creates eula.txt and server.properties for a server when the files are
// missing. Existing server.properties is user-owned and is deliberately
// preserved during a repair/Prepare flow. Java-server callers must explicitly
// pass the EULA acknowledgement that their UX collected; this boundary refuses
// to write eula=true otherwise. Bedrock Dedicated Server has no EULA, so it
// does not get one (its server.properties is still initialized when absent).
bool write_server_config_files(const server::ServerConfig& server, bool eula_accepted,
                               std::string* error);

// Confirms the exact file the local-server manager can launch exists and is a
// regular file. This is the provisioning postcondition required before a
// server can be marked Ready.
bool verify_runnable_runtime(const server::ServerConfig& server, std::string* error);

// Resolves the provider's artifact for this software and version, downloads it,
// and puts it where the start command expects it: a server jar, a loader's
// installer run, a loader launcher jar plus the vanilla server it wraps, or an
// unpacked archive. Returns false with a reason the page can show verbatim.
//
// `java_exe` runs loader installers and may be empty for software that does not
// need Java. `progress` reports transfer bytes and is asked to cancel (return
// false). `phase` receives the sentence the page shows while it waits.
bool provision_runtime(const server::ServerConfig& server, const std::wstring& java_exe,
                       bool eula_accepted, server_providers::Progress progress,
                       const std::function<void(const std::string&)>& phase,
                       std::string* error);

}  // namespace aml::server_provision
