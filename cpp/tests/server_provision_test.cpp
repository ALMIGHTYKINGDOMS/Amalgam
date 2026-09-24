// What provisioning promises the Servers page, without touching the network:
// a manual runtime explains itself instead of failing silently, and the files
// that make a server startable are written where the start command looks.
#include "server_provision.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

namespace prov = aml::server_provision;
using aml::server::ServerConfig;
using aml::server::ServerSoftware;

int failures = 0;
int temporary_index = 0;

void expect(bool condition, const char* what) {
    if (condition) return;
    std::printf("FAIL: %s\n", what);
    ++failures;
}

std::filesystem::path temporary_dir() {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("amalgam-provision-" + std::to_string(++temporary_index));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return dir;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return text;
}

void test_manual_runtime_explains_itself() {
    const std::filesystem::path dir = temporary_dir();
    ServerConfig server;
    server.name = "manual";
    server.software = ServerSoftware::Spigot;
    server.minecraft_version = "1.20.1";
    server.server_directory = dir.string();

    std::string error;
    const bool ok = prov::provision_runtime(server, std::wstring(), true, nullptr, nullptr,
                                            &error);
    expect(!ok, "a manual runtime must not report success");
    expect(error.find("BuildTools") != std::string::npos,
           "a manual runtime must say how it is obtained");
    std::filesystem::remove_all(dir);
}

void test_config_files() {
    const std::filesystem::path dir = temporary_dir();
    ServerConfig server;
    server.name = "Survival Realm";
    server.software = ServerSoftware::Paper;
    server.minecraft_version = "1.20.1";
    server.port = 25599;
    server.max_players = 12;
    server.server_directory = dir.string();

    std::string error;
    expect(prov::write_server_config_files(server, true, &error),
           "writing a server's config files must succeed");
    expect(read_file(dir / "eula.txt").find("eula=true") != std::string::npos,
           "eula.txt must record the accepted EULA");
    const std::string properties = read_file(dir / "server.properties");
    expect(properties.find("server-port=25599") != std::string::npos,
           "server.properties must carry the chosen port");
    expect(properties.find("max-players=12") != std::string::npos,
           "server.properties must carry the chosen player limit");
    expect(properties.find("server-name=Survival Realm") != std::string::npos,
           "server.properties must carry the server name");

    // Bedrock reads server.properties but has no EULA to accept.
    const std::filesystem::path bedrock_dir = temporary_dir();
    ServerConfig bedrock;
    bedrock.name = "bedrock";
    bedrock.software = ServerSoftware::BedrockDedicatedServer;
    bedrock.port = 19132;
    bedrock.server_directory = bedrock_dir.string();
    expect(prov::write_server_config_files(bedrock, false, &error),
           "Bedrock's config files must be writable");
    expect(!std::filesystem::exists(bedrock_dir / "eula.txt"),
           "Bedrock must not be given a EULA it does not have");
    expect(read_file(bedrock_dir / "server.properties").find("server-port=19132") !=
               std::string::npos,
           "Bedrock's port must reach server.properties");

    std::filesystem::remove_all(dir);
    std::filesystem::remove_all(bedrock_dir);
}

// The create dialog's checkbox is useful UX, but the provisioning boundary
// must independently refuse to write eula=true unless that confirmation made
// it through the API. That keeps an imported/incomplete server from silently
// accepting Minecraft's EULA during a repair download.
void test_java_config_requires_explicit_eula_acceptance() {
    const std::filesystem::path dir = temporary_dir();
    ServerConfig server;
    server.name = "EULA Guard";
    server.software = ServerSoftware::Paper;
    server.minecraft_version = "1.20.1";
    server.server_directory = dir.string();

    std::string error;
    expect(!prov::write_server_config_files(server, false, &error),
           "a Java server config must refuse an unaccepted EULA");
    expect(error.find("EULA") != std::string::npos,
           "an unaccepted EULA must have an actionable explanation");
    expect(!std::filesystem::exists(dir),
           "an unaccepted EULA must not create a partial server directory");

    error.clear();
    expect(prov::write_server_config_files(server, true, &error),
           "an explicitly accepted Java EULA must permit config creation");
    expect(prov::has_accepted_eula(server),
           "the written EULA marker must be recognizable by later provisioning");

    std::filesystem::remove_all(dir);
}

// Prepare/repair can run on an imported server. It may acquire a runtime, but
// it must not replace an existing owner's properties with launcher's defaults.
void test_existing_server_properties_are_preserved() {
    const std::filesystem::path dir = temporary_dir();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    ServerConfig server;
    server.name = "Imported Realm";
    server.software = ServerSoftware::Paper;
    server.minecraft_version = "1.20.1";
    server.port = 25565;
    server.max_players = 20;
    server.server_directory = dir.string();

    const std::string original_properties =
        "# owner-managed settings\nserver-port=25577\nmax-players=7\nview-distance=18\n";
    const std::string original_eula = "# owner acknowledgement\neula=true\n";
    std::ofstream(dir / "server.properties") << original_properties;
    std::ofstream(dir / "eula.txt") << original_eula;

    std::string error;
    expect(prov::write_server_config_files(server, true, &error),
           "preparing an imported server with accepted EULA must succeed");
    expect(read_file(dir / "server.properties") == original_properties,
           "preparing an imported server must preserve its existing server.properties");
    expect(read_file(dir / "eula.txt") == original_eula,
           "preparing an imported server must preserve its existing eula acknowledgement");

    std::filesystem::remove_all(dir, ec);
}

// This is deliberately a no-provider/no-installer test. The post-installer
// readiness predicate is the same one provisioning invokes after an installer
// returns success, so an exit code alone can never make an empty Forge folder
// look Ready.
void test_installer_requires_runnable_launch_target() {
    const std::filesystem::path dir = temporary_dir();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    ServerConfig server;
    server.name = "Forge Guard";
    server.software = ServerSoftware::Forge;
    server.minecraft_version = "1.20.1";
    server.server_directory = dir.string();

    std::string error;
    expect(!prov::verify_runnable_runtime(server, &error),
           "an installer result without a launch target must not be ready");
    expect(error.find("runnable launch target") != std::string::npos,
           "a missing installer target must explain how to recover");

    const std::filesystem::path args =
        dir / "libraries/net/minecraftforge/forge/1.20.1-47.4.23" / "win_args.txt";
    std::filesystem::create_directories(args.parent_path(), ec);
    std::ofstream(args) << "-cp .";
    error.clear();
    expect(prov::verify_runnable_runtime(server, &error),
           "a regular loader argument file must satisfy the runnable target contract");

    std::filesystem::remove_all(dir, ec);
}

// A missing EULA must stop automatic provisioning before it resolves a
// provider artifact or creates a server/runtime workspace. BungeeCord is used
// only as a representative automatic runtime; the early guard keeps this test
// fully local and deterministic.
void test_provision_rejects_missing_eula_before_provider_or_files() {
    const std::filesystem::path dir = temporary_dir();
    ServerConfig server;
    server.name = "Provision EULA Guard";
    server.software = ServerSoftware::BungeeCord;
    server.minecraft_version = "latest";
    server.server_directory = dir.string();

    std::string error;
    expect(!prov::provision_runtime(server, std::wstring(), false, nullptr, nullptr, &error),
           "automatic Java provisioning must require an accepted EULA");
    expect(error.find("EULA") != std::string::npos,
           "blocked provisioning must tell the user to accept the EULA");
    expect(!std::filesystem::exists(dir),
           "blocked provisioning must not create a server or runtime workspace");

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// A directory that cannot be created has to come back as a reason, not as a
// half-written server the page would badge Ready.
void test_unwritable_directory_is_reported() {
    const std::filesystem::path dir = temporary_dir();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    ServerConfig server;
    server.name = "blocked";
    server.software = ServerSoftware::Paper;
    // A path through an existing *file* cannot be a directory.
    const std::filesystem::path file = dir / "note.txt";
    std::ofstream(file) << "x";
    server.server_directory = (file / "child").string();

    std::string error;
    expect(!prov::write_server_config_files(server, true, &error),
           "an unusable server directory must be reported");
    expect(!error.empty(), "an unusable server directory must explain itself");
    std::filesystem::remove_all(dir);
}

// The provisioning worker promises a boolean/error result. A failed base or
// scratch directory must not throw through the worker and terminate the app.
void test_runtime_directory_failures_are_reported() {
    std::error_code ec;
    const std::filesystem::path blocked_base = temporary_dir();
    std::filesystem::create_directories(blocked_base, ec);
    const std::filesystem::path base_file = blocked_base / "not-a-directory.txt";
    std::ofstream(base_file) << "x";

    ServerConfig server;
    server.name = "blocked runtime";
    // BungeeCord resolves locally without a network request, so a filesystem
    // failure is deterministic and reaches the exact provisioning boundary.
    server.software = ServerSoftware::BungeeCord;
    server.minecraft_version = "latest";
    server.server_directory = (base_file / "child").string();

    std::string error;
    expect(!prov::provision_runtime(server, std::wstring(), true, nullptr, nullptr, &error),
           "a file-as-parent runtime path must report failure instead of throwing");
    expect(!error.empty(), "a base runtime directory failure must explain itself");
    std::filesystem::remove_all(blocked_base, ec);

    const std::filesystem::path blocked_scratch = temporary_dir();
    std::filesystem::create_directories(blocked_scratch, ec);
    std::ofstream(blocked_scratch / ".amalgam") << "x";
    server.server_directory = blocked_scratch.string();
    error.clear();
    expect(!prov::provision_runtime(server, std::wstring(), true, nullptr, nullptr, &error),
           "a file-as-scratch-parent must report failure instead of throwing");
    expect(!error.empty(), "a scratch runtime directory failure must explain itself");
    std::filesystem::remove_all(blocked_scratch, ec);
}

}  // namespace

int main() {
    test_manual_runtime_explains_itself();
    test_config_files();
    test_java_config_requires_explicit_eula_acceptance();
    test_existing_server_properties_are_preserved();
    test_installer_requires_runnable_launch_target();
    test_provision_rejects_missing_eula_before_provider_or_files();
    test_unwritable_directory_is_reported();
    test_runtime_directory_failures_are_reported();
    if (failures > 0) return 1;
    std::printf("server provision: contract verified\n");
    return 0;
}
