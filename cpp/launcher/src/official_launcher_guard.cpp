#include "official_launcher_guard.h"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cwctype>
#include <unordered_map>
#include <utility>

namespace aml::official_launcher {

namespace {

std::wstring lower_case(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });
    return value;
}

bool contains_case_insensitive(const std::wstring& value, const wchar_t* needle) {
    return lower_case(value).find(needle) != std::wstring::npos;
}

struct JavaWindowCollector {
    const std::unordered_map<DWORD, std::wstring>* java_processes = nullptr;
    std::vector<HandoffProcessObservation>* observations = nullptr;
};

BOOL CALLBACK collect_java_window(HWND window, LPARAM parameter) {
    auto* collector = reinterpret_cast<JavaWindowCollector*>(parameter);
    if (!collector || !collector->java_processes || !collector->observations) return TRUE;

    DWORD process_id = 0;
    GetWindowThreadProcessId(window, &process_id);
    const auto process = collector->java_processes->find(process_id);
    if (process == collector->java_processes->end()) return TRUE;

    const int length = GetWindowTextLengthW(window);
    if (length <= 0) return TRUE;
    std::wstring title(static_cast<size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(window, title.data(), length + 1);
    if (copied <= 0) return TRUE;
    title.resize(static_cast<size_t>(copied));
    collector->observations->push_back({process->second, std::move(title)});
    return TRUE;
}

bool enumerate_relevant_processes(std::vector<HandoffProcessObservation>* observations,
                                  std::wstring* error) {
    if (!observations) return false;
    observations->clear();

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        if (error) {
            *error = L"Amalgam could not confirm that the Java Minecraft game and the "
                     L"official Minecraft Launcher are closed. Close them, then try again.";
        }
        return false;
    }

    std::unordered_map<DWORD, std::wstring> java_processes;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    BOOL more = Process32FirstW(snapshot, &entry);
    if (!more) {
        CloseHandle(snapshot);
        if (error) {
            *error = L"Amalgam could not confirm that the Java Minecraft game and the "
                     L"official Minecraft Launcher are closed. Close them, then try again.";
        }
        return false;
    }
    do {
        const std::wstring executable = lower_case(entry.szExeFile);
        if (executable == L"minecraftlauncher.exe" || executable == L"gamelaunchhelper.exe") {
            observations->push_back({executable, L""});
        } else if (executable == L"java.exe" || executable == L"javaw.exe") {
            java_processes.emplace(entry.th32ProcessID, executable);
        }
        entry.dwSize = sizeof(entry);
        more = Process32NextW(snapshot, &entry);
    } while (more);
    CloseHandle(snapshot);

    // Java itself is intentionally not enough to block a handoff: Gradle,
    // development tools, and unrelated apps use it too.  Match only a Java
    // process that owns a window whose title identifies a Minecraft game.
    if (!java_processes.empty()) {
        JavaWindowCollector collector{&java_processes, observations};
        EnumWindows(collect_java_window, reinterpret_cast<LPARAM>(&collector));
    }
    return true;
}

}  // namespace

std::wstring HandoffBlockReasonForProcesses(
    const std::vector<HandoffProcessObservation>& processes) {
    for (const auto& process : processes) {
        const std::wstring executable = lower_case(process.executable_name);
        if (executable == L"minecraftlauncher.exe" || executable == L"gamelaunchhelper.exe") {
            return L"The official Minecraft Launcher is still open. Close it before Amalgam changes or selects a profile.";
        }
    }
    for (const auto& process : processes) {
        const std::wstring executable = lower_case(process.executable_name);
        if ((executable == L"java.exe" || executable == L"javaw.exe") &&
            contains_case_insensitive(process.window_title, L"minecraft")) {
            return L"A Java Minecraft game window is still open. Close the game before Amalgam changes or selects a profile.";
        }
    }
    return {};
}

bool CanPrepareOfficialLauncherHandoff(std::wstring* reason) {
    std::vector<HandoffProcessObservation> processes;
    std::wstring enumeration_error;
    if (!enumerate_relevant_processes(&processes, &enumeration_error)) {
        if (reason) *reason = std::move(enumeration_error);
        return false;
    }
    const std::wstring block_reason = HandoffBlockReasonForProcesses(processes);
    if (!block_reason.empty()) {
        if (reason) *reason = block_reason;
        return false;
    }
    if (reason) reason->clear();
    return true;
}

}  // namespace aml::official_launcher
