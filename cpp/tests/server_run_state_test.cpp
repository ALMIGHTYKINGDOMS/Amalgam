// The supervised process owns local run state. A stage restored from
// servers.json is only a hint, and one no process backs must never read as a
// server that is actually up (otherwise a restart shows a RUNNING badge, live
// metrics chrome and a Stop button for a server that is not there).
#include "server_types.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using aml::server::reconciled_stage;
using aml::server::runtime_files_present;
using aml::server::ServerConfig;
using aml::server::ServerSoftware;
using aml::server::ServerStage;

namespace {

int failures = 0;

void expect(ServerStage persisted, bool supervised, ServerStage expected) {
    ServerConfig server;
    server.stage = persisted;
    const ServerStage actual = reconciled_stage(server, supervised);
    if (actual != expected) {
        std::printf("FAIL: persisted stage %d supervised=%d read as %d, expected %d\n",
                    static_cast<int>(persisted), supervised ? 1 : 0,
                    static_cast<int>(actual), static_cast<int>(expected));
        ++failures;
    }
}

// A "Ready" badge has to mean the files are there. Otherwise Start is offered
// on a server whose runtime was deleted, and the click can only fail.
void expect_present(ServerSoftware software, bool write_runtime, bool expected,
                    bool directory_instead_of_file = false) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        (std::string("amalgam-runtime-__") + std::to_string(static_cast<int>(software)));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);

    if (write_runtime) {
        switch (software) {
            case ServerSoftware::BedrockDedicatedServer:
                if (directory_instead_of_file) {
                    std::filesystem::create_directories(dir / "bedrock_server.exe", ec);
                } else {
                    std::ofstream(dir / "bedrock_server.exe") << "stub";
                }
                break;
            case ServerSoftware::Forge:
            case ServerSoftware::NeoForge: {
                const std::filesystem::path args =
                    dir / (software == ServerSoftware::NeoForge
                               ? "libraries/net/neoforged/neoforge/21.1.66"
                               : "libraries/net/minecraftforge/forge/1.20.1-47.4.23");
                std::filesystem::create_directories(args, ec);
                if (directory_instead_of_file) {
                    std::filesystem::create_directories(args / "win_args.txt", ec);
                } else {
                    std::ofstream(args / "win_args.txt") << "-cp .";
                }
                break;
            }
            default:
                if (directory_instead_of_file) {
                    std::filesystem::create_directories(dir / "server.jar", ec);
                } else {
                    std::ofstream(dir / "server.jar") << "stub";
                }
                break;
        }
    }

    ServerConfig server;
    server.software = software;
    server.server_directory = dir.string();
    const bool actual = runtime_files_present(server);
    if (actual != expected) {
        std::printf("FAIL: software %d present=%d read as %d, expected %d\n",
                    static_cast<int>(software), write_runtime ? 1 : 0, actual ? 1 : 0,
                    expected ? 1 : 0);
        ++failures;
    }
    std::filesystem::remove_all(dir, ec);
}

void expect_catalog() {
    // Every software the create dialog offers must describe itself, and the
    // catalogue must not list the same software twice (the dialog indexes it).
    const auto& catalog = aml::server::software_catalog();
    if (catalog.empty()) {
        std::printf("FAIL: the server software catalogue is empty\n");
        ++failures;
    }
    for (size_t i = 0; i < catalog.size(); ++i) {
        if (!catalog[i].label || !*catalog[i].label || !catalog[i].note ||
            !*catalog[i].note) {
            std::printf("FAIL: catalogue entry %zu has no label or note\n", i);
            ++failures;
        }
        for (size_t j = i + 1; j < catalog.size(); ++j) {
            if (catalog[i].software == catalog[j].software) {
                std::printf("FAIL: catalogue lists software %d twice\n",
                            static_cast<int>(catalog[i].software));
                ++failures;
            }
        }
        if (aml::server::server_software_kind(catalog[i].software) != catalog[i].kind) {
            std::printf("FAIL: catalogue kind disagrees with server_software_kind for %d\n",
                        static_cast<int>(catalog[i].software));
            ++failures;
        }
    }
}

void expect_server_name(const std::string& name, bool expected, const char* description) {
    std::string reason;
    const bool actual = aml::server::validate_server_name(name, &reason);
    if (actual != expected) {
        std::printf("FAIL: %s was %s, expected %s\\n", description,
                    actual ? "accepted" : "rejected", expected ? "accepted" : "rejected");
        ++failures;
    }
    if (!expected && reason.empty()) {
        std::printf("FAIL: %s was rejected without an explanation\\n", description);
        ++failures;
    }
    if (expected && !reason.empty()) {
        std::printf("FAIL: %s was accepted with a stale explanation\\n", description);
        ++failures;
    }
}

void expect_safe_server_names() {
    // Normal display names may include spaces, but cannot map to a Windows
    // device/path alias when the launcher creates the managed server folder.
    expect_server_name("Forsaken World SMP", true, "a normal spaced server name");
    expect_server_name(std::string(u8"世界 Realm"), true, "a Unicode display name");
    expect_server_name("", false, "an empty server name");
    expect_server_name("   ", false, "a whitespace-only server name");
    expect_server_name(".", false, "the current-directory alias");
    expect_server_name("..", false, "the parent-directory alias");
    expect_server_name("Realm/One", false, "a path separator");
    expect_server_name("Realm:One", false, "a path punctuation character");
    expect_server_name(std::string("Realm") + static_cast<char>(0x1f), false,
                       "a control character");
    expect_server_name("Realm.", false, "a trailing dot");
    expect_server_name("Realm ", false, "a trailing space");
    expect_server_name("CON", false, "the CON device name");
    expect_server_name("nul.txt", false, "a device name with an extension");
    expect_server_name("con .txt", false, "a device name disguised before an extension");
    expect_server_name("Com9", false, "a serial device name");
    expect_server_name("lpt1.log", false, "a parallel device name with an extension");
    expect_server_name("COM10", true, "a non-reserved serial-like name");
}

}  // namespace

int main() {
    // The restart case: a server that was running when the launcher closed.
    expect(ServerStage::Running, false, ServerStage::Stopped);
    // A supervised process that is still up keeps reading as running.
    expect(ServerStage::Running, true, ServerStage::Running);
    // Every other stage passes through untouched, supervised or not.
    for (const bool supervised : {false, true}) {
        expect(ServerStage::NotInstalled, supervised, ServerStage::NotInstalled);
        expect(ServerStage::Installing, supervised, ServerStage::Installing);
        expect(ServerStage::Ready, supervised, ServerStage::Ready);
        expect(ServerStage::Stopped, supervised, ServerStage::Stopped);
        expect(ServerStage::Error, supervised, ServerStage::Error);
        expect(ServerStage::Crashed, supervised, ServerStage::Crashed);
    }

    // "Ready" only means ready when the files a start needs are on disk.
    expect_present(ServerSoftware::Vanilla, true, true);
    expect_present(ServerSoftware::Vanilla, false, false);
    expect_present(ServerSoftware::Paper, true, true);
    expect_present(ServerSoftware::Paper, true, false, true);
    expect_present(ServerSoftware::Velocity, false, false);
    expect_present(ServerSoftware::Forge, true, true);
    expect_present(ServerSoftware::Forge, true, false, true);
    expect_present(ServerSoftware::Forge, false, false);
    expect_present(ServerSoftware::NeoForge, true, true);
    expect_present(ServerSoftware::NeoForge, false, false);
    expect_present(ServerSoftware::BedrockDedicatedServer, true, true);
    expect_present(ServerSoftware::BedrockDedicatedServer, true, false, true);
    expect_present(ServerSoftware::BedrockDedicatedServer, false, false);
    expect_present(ServerSoftware::Spigot, true, true);
    expect_present(ServerSoftware::Spigot, false, false);
    expect_catalog();
    expect_safe_server_names();

    if (failures > 0) return 1;
    std::printf("server run state: reconciled stage and runtime contract verified\n");
    return 0;
}
