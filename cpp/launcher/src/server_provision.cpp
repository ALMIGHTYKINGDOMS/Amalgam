#include "server_provision.h"

#include "extract.h"
#include "net.h"

#include <cctype>
#include <filesystem>
#include <fstream>

namespace aml::server_provision {

namespace {

namespace sp = server_providers;

// Where a runtime's intermediate files live inside a server directory, so a
// half-finished install never looks like a finished one next to server.jar.
std::wstring runtime_scratch_dir(const server::ServerConfig& server) {
    return net::to_wide(server.server_directory) + L"\\.amalgam\\runtime";
}

bool ensure_directory(const std::filesystem::path& path, const char* purpose,
                      std::string* error) {
    if (path.empty()) {
        if (error) *error = "Amalgam could not create the " + std::string(purpose) +
                            ": the server folder is empty.";
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (!ec) return true;
    if (error) {
        *error = "Amalgam could not create the " + std::string(purpose) + " " +
                 net::to_utf8(path.wstring()) + ": " + ec.message();
    }
    return false;
}

void remove_runtime_temp(const std::filesystem::path& path) {
    // A stale cache file is best-effort cleanup. Use the error-code overload
    // because cleanup must never throw through a provisioning worker.
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

void substitute(std::wstring& text, const std::wstring& token, const std::wstring& value) {
    for (size_t at = text.find(token); at != std::wstring::npos; at = text.find(token, at)) {
        text.replace(at, token.size(), value);
        at += value.size();
    }
}

// The last lines of an installer's output name the real reason it stopped
// (offline, a blocked artifact, an incompatible Java), so they are kept instead
// of a generic failure line.
std::string installer_failure_tail(const std::string& output) {
    std::string tail = output;
    if (tail.size() > 400) tail = tail.substr(tail.size() - 400);
    for (char& c : tail) {
        if (c == '\r' || c == '\n') c = ' ';
    }
    return tail;
}

std::string trim_ascii(std::string value) {
    size_t first = 0;
    while (first < value.size() &&
           std::isspace(static_cast<unsigned char>(value[first])) != 0) {
        ++first;
    }
    size_t last = value.size();
    while (last > first &&
           std::isspace(static_cast<unsigned char>(value[last - 1])) != 0) {
        --last;
    }
    return value.substr(first, last - first);
}

void lowercase_ascii(std::string& value) {
    for (char& c : value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
}

std::string eula_required_message(const server::ServerConfig& server) {
    return std::string("Accept the Minecraft EULA before preparing ") +
           server::server_software_name(server.software) +
           ". Create the server through Amalgam's setup dialog, or confirm eula=true "
           "only after you have read the Minecraft EULA.";
}

}  // namespace

bool has_accepted_eula(const server::ServerConfig& server) {
    if (server::is_native_software(server.software)) return true;
    if (server.server_directory.empty()) return false;

    std::ifstream eula(std::filesystem::path(net::to_wide(server.server_directory)) /
                       "eula.txt");
    std::string line;
    while (std::getline(eula, line)) {
        const size_t separator = line.find('=');
        if (separator == std::string::npos) continue;
        std::string key = trim_ascii(line.substr(0, separator));
        std::string value = trim_ascii(line.substr(separator + 1));
        lowercase_ascii(key);
        lowercase_ascii(value);
        if (key == "eula") return value == "true";
    }
    return false;
}

bool write_server_config_files(const server::ServerConfig& server, bool eula_accepted,
                               std::string* error) {
    // The checkbox in the creation dialog is not the security boundary: a
    // repair/import path can reach this API later. Refuse before creating any
    // directory or partial config if an automatic call lacks acknowledgement.
    if (!server::is_native_software(server.software) && !eula_accepted) {
        if (error) *error = eula_required_message(server);
        return false;
    }
    const std::wstring dir = net::to_wide(server.server_directory);
    if (!ensure_directory(std::filesystem::path(dir), "server directory", error)) return false;
    // Bedrock has no EULA to accept; it does read server.properties, so the
    // port and player limit still have to reach the file.
    if (!server::is_native_software(server.software) && !has_accepted_eula(server)) {
        std::ofstream eula(std::filesystem::path(dir) / "eula.txt", std::ios::trunc);
        eula << "eula=true\n";
        if (!eula) {
            if (error) *error = "Amalgam could not write eula.txt into " + server.server_directory;
            return false;
        }
    }
    const std::filesystem::path properties_path = std::filesystem::path(dir) /
                                                   "server.properties";
    std::error_code properties_ec;
    const bool properties_exist = std::filesystem::exists(properties_path, properties_ec);
    if (properties_ec) {
        if (error) {
            *error = "Amalgam could not inspect server.properties in " +
                     server.server_directory + ": " + properties_ec.message();
        }
        return false;
    }
    if (properties_exist) {
        if (!std::filesystem::is_regular_file(properties_path, properties_ec) || properties_ec) {
            if (error) {
                *error = "Amalgam expected server.properties to be a regular file in " +
                         server.server_directory;
            }
            return false;
        }
        return true;
    }

    std::ofstream properties(properties_path, std::ios::trunc);
    properties << "server-name=" << server.name << "\n"
               << "motd=" << server.name << " - managed by Amalgam\n"
               << "server-port=" << server.port << "\n"
               << "max-players=" << server.max_players << "\n"
               << "online-mode=true\n"
               << "view-distance=10\n"
               << "simulation-distance=10\n";
    if (!properties) {
        if (error) {
            *error = "Amalgam could not write server.properties into " + server.server_directory;
        }
        return false;
    }
    return true;
}

bool verify_runnable_runtime(const server::ServerConfig& server, std::string* error) {
    const server::LaunchTarget target = server::resolve_launch_target(server);
    if (target.kind == server::LaunchTarget::Kind::Missing) {
        if (error) {
            *error = std::string(server::server_software_name(server.software)) +
                     " did not create a runnable launch target. Review the installer output "
                     "and server folder, then choose Prepare again.";
        }
        return false;
    }

    std::error_code ec;
    if (!std::filesystem::is_regular_file(target.path, ec) || ec) {
        if (error) {
            *error = std::string(server::server_software_name(server.software)) +
                     " created an invalid runnable launch target. Review the installer output "
                     "and server folder, then choose Prepare again.";
        }
        return false;
    }
    return true;
}

bool provision_runtime(const server::ServerConfig& server, const std::wstring& java_exe,
                       bool eula_accepted, server_providers::Progress progress,
                       const std::function<void(const std::string&)>& phase,
                       std::string* error) {
    const std::wstring dir = net::to_wide(server.server_directory);

    // The provisioning worker is independently responsible for consent. This
    // check deliberately occurs before resolve_runtime, directory creation, or
    // any download, so a missing acknowledgement cannot create partial files
    // or make a provider request.
    if (!server::is_native_software(server.software) &&
        server::server_software_kind(server.software) != server::RuntimeKind::Manual &&
        !eula_accepted) {
        if (error) *error = eula_required_message(server);
        return false;
    }

    if (phase) phase("Resolving the runtime");
    sp::RuntimeArtifact artifact;
    if (!sp::resolve_runtime(server.software, server.minecraft_version, artifact, error))
        return false;
    if (artifact.kind == server::RuntimeKind::Manual) {
        if (error) {
            *error = artifact.manual_hint.empty()
                ? std::string(server::server_software_name(server.software)) +
                      " has no automatic download."
                : artifact.manual_hint;
        }
        return false;
    }

    const std::wstring scratch = runtime_scratch_dir(server);
    if (!ensure_directory(std::filesystem::path(dir), "server directory", error) ||
        !ensure_directory(std::filesystem::path(scratch), "runtime workspace", error)) {
        return false;
    }

    if (artifact.kind == server::RuntimeKind::Jar) {
        if (phase) phase("Downloading " + artifact.label);
        return sp::download_runtime(artifact, dir + L"\\server.jar", progress, error);
    }

    if (artifact.kind == server::RuntimeKind::LoaderJar) {
        // The loader's jar is a launcher that starts the vanilla server beside
        // it, so the server jar has to be there too: naming the loader jar
        // `server.jar` would make the launcher try to launch itself.
        if (phase) phase("Downloading the Minecraft server " + server.minecraft_version);
        const std::wstring companion = dir + L"\\" + net::to_wide(artifact.companion_name);
        sp::RuntimeArtifact companion_artifact;
        companion_artifact.kind = server::RuntimeKind::Jar;
        companion_artifact.url = artifact.companion_url;
        companion_artifact.sha1 = artifact.companion_sha1;
        companion_artifact.size = artifact.companion_size;
        companion_artifact.label = "Minecraft server " + server.minecraft_version;
        if (!sp::download_runtime(companion_artifact, companion, progress, error)) {
            return false;
        }
        const char* launcher = server::loader_launcher_jar(server.software);
        if (!launcher) {
            if (error) {
                *error = std::string(server::server_software_name(server.software)) +
                         " has no known server launcher file.";
            }
            return false;
        }
        if (phase) phase("Downloading " + artifact.label);
        return sp::download_runtime(artifact, dir + L"\\" + net::to_wide(launcher), progress,
                                    error);
    }

    if (artifact.kind == server::RuntimeKind::Archive) {
        if (phase) phase("Downloading " + artifact.label);
        const std::wstring zip = scratch + L"\\runtime.zip";
        remove_runtime_temp(std::filesystem::path(zip));
        if (!sp::download_runtime(artifact, zip, progress, error))
            return false;
        if (phase) phase("Unpacking " + artifact.label);
        std::string extract_error;
        if (!extract::zip(zip, dir, &extract_error)) {
            if (error) {
                *error = extract_error.empty() ? "Could not unpack the server archive"
                                               : extract_error;
            }
            return false;
        }
        remove_runtime_temp(std::filesystem::path(zip));
        return true;
    }

    // Installer kind: the loader's installer jar builds the server (and
    // downloads the Minecraft files it needs) inside the server directory.
    if (java_exe.empty()) {
        if (error) {
            *error = std::string(server::server_software_name(server.software)) +
                     " needs a Java runtime, and the launcher could not resolve one for " +
                     server.minecraft_version + ".";
        }
        return false;
    }
    const std::string installer_name = std::filesystem::path(artifact.url).filename().string();
    const std::wstring installer = scratch + L"\\" + net::to_wide(installer_name);
    if (phase) phase("Downloading " + artifact.label);
    remove_runtime_temp(std::filesystem::path(installer));
    if (!sp::download_runtime(artifact, installer, progress, error))
        return false;

    if (phase) phase("Installing " + artifact.label);
    // Each provider states its own installer command; {installer} and {dir} are
    // the only parts this code knows about.
    std::wstring installer_args = net::to_wide(artifact.installer_args);
    substitute(installer_args, L"{installer}", extract::quote(installer));
    substitute(installer_args, L"{dir}", extract::quote(dir));

    int exit_code = 0;
    std::string output;
    std::string run_error;
    if (!extract::run_command(java_exe, installer_args, dir, 30 * 60 * 1000, &exit_code,
                              &run_error, &output)) {
        if (error) {
            *error = run_error.empty() ? "Could not run " + artifact.label + "'s installer"
                                       : run_error;
        }
        return false;
    }
    if (exit_code != 0) {
        if (error) {
            const std::string tail = installer_failure_tail(output);
            *error = artifact.label + "'s installer exited with code " +
                     std::to_string(exit_code) + (tail.empty() ? "" : ": " + tail);
        }
        return false;
    }
    if (phase) phase("Verifying runnable server files");
    if (!verify_runnable_runtime(server, error)) return false;
    remove_runtime_temp(std::filesystem::path(installer));
    return true;
}

}  // namespace aml::server_provision
