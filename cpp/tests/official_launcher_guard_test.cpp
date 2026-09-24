#include "official_launcher_guard.h"

#include <iostream>
#include <string>
#include <vector>

namespace {

bool fail(const std::string& message) {
    std::cerr << "FAILED: " << message << "\n";
    return false;
}

bool test_only_java_minecraft_game_windows_block_handoff() {
    using aml::official_launcher::HandoffBlockReasonForProcesses;
    using aml::official_launcher::HandoffProcessObservation;

    if (!HandoffBlockReasonForProcesses({}).empty()) {
        return fail("no relevant process should block a handoff");
    }
    if (!HandoffBlockReasonForProcesses({{L"javaw.exe", L"Gradle Build"}}).empty()) {
        return fail("an unrelated Java window should not block a handoff");
    }
    const std::wstring block = HandoffBlockReasonForProcesses({
        {L"javaw.exe", L"Minecraft 1.21.1 - Multiplayer"},
    });
    if (block.find(L"Java Minecraft game") == std::wstring::npos) {
        return fail("a Java Minecraft game window must block a handoff");
    }
    return true;
}

bool test_official_launcher_processes_block_without_window_titles() {
    using aml::official_launcher::HandoffBlockReasonForProcesses;

    const std::wstring launcher = HandoffBlockReasonForProcesses({
        {L"MinecraftLauncher.exe", L""},
    });
    if (launcher.find(L"official Minecraft Launcher") == std::wstring::npos) {
        return fail("the desktop launcher process must block a handoff");
    }
    const std::wstring store_helper = HandoffBlockReasonForProcesses({
        {L"GameLaunchHelper.exe", L""},
    });
    if (store_helper.find(L"official Minecraft Launcher") == std::wstring::npos) {
        return fail("the Store launcher helper must block a handoff");
    }
    return true;
}

bool test_only_explicit_java_edition_processes_are_classified() {
    using aml::official_launcher::HandoffBlockReasonForProcesses;

    // A title alone cannot block the operation.  The Java Edition game rule
    // requires a Java runtime image, and only the two official-launcher image
    // names are otherwise considered by the guard.
    if (!HandoffBlockReasonForProcesses({{L"unrelated-minecraft-window.exe", L"Minecraft"}}).empty()) {
        return fail("an unrecognized native process must not be classified as a Java Edition handoff blocker");
    }
    return true;
}

}  // namespace

int main() {
    const bool ok = test_only_java_minecraft_game_windows_block_handoff() &&
                    test_official_launcher_processes_block_without_window_titles() &&
                    test_only_explicit_java_edition_processes_are_classified();
    if (ok) std::cout << "official launcher guard tests passed\n";
    return ok ? 0 : 1;
}
