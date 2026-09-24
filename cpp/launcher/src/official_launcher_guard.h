#pragma once

#include <string>
#include <vector>

namespace aml::official_launcher {

// A minimal process/window observation used by the handoff guard.  Keeping the
// matching rule separate from Windows enumeration makes the safety policy
// deterministic and directly testable without examining a real machine.
struct HandoffProcessObservation {
    std::wstring executable_name;
    std::wstring window_title;
};

// Returns an explanatory reason when an official-launcher handoff must not
// change profiles yet, or an empty string when these observations are safe.
// The policy intentionally considers only the Java Edition's official launcher
// and Java game windows; it does not inspect other Minecraft editions.
std::wstring HandoffBlockReasonForProcesses(
    const std::vector<HandoffProcessObservation>& processes);

// Safely enumerates only the Java Edition processes relevant to a profile
// handoff.  A failed enumeration is treated as unsafe: the caller must not
// change the official launcher's profile file until Windows can be checked.
bool CanPrepareOfficialLauncherHandoff(std::wstring* reason = nullptr);

}  // namespace aml::official_launcher
