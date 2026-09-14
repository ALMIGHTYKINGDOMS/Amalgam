# AMALGAM V3 BETA 1 — COMPREHENSIVE VERIFICATION REPORT

Version: 3.0.0-beta.1 · Date: 2026-08-21

## Build Status

| Metric | Result |
|---|---|
| Fresh build | 333/333 steps, 0 errors |
| Build directory | cpp/build-beta (fresh, no stale artifacts) |
| Compiler | MSVC 19.51, Ninja, Debug mode |
| Launcher | amalgam_launcher.exe (23 MB) |
| Client DLL | amalgam.dll (3.6 MB) |
| Bridges | 19 Java bridge jars staged |

## Test Results

| Suite | Result |
|---|---|
| CTest (native) | 33/33 PASS |
| Runtime agent | 24/24 PASS |
| Schema verifier | 65/65 PASS |
| Supabase source | PASS (18 migrations, 23 functions) |
| Node tooling | 38/38 PASS |
| **Total** | **160 PASS / 0 FAIL / 0 SKIP** |

## Visual Testing

### Page Screenshots Captured

| Page | Status | File Size |
|---|---|---|
| Home | ✅ Valid PNG, 1242×610 | 455K |
| Discover | ✅ Valid PNG, 1242×610 | 244K |
| Library | ✅ Valid PNG, 1242×610 | 444K |
| Settings | ✅ Valid PNG, 1242×610 | 100K |
| Account | ✅ Valid PNG, 1242×610 | 43K |
| Essentials | ✅ Valid PNG, 1242×610 | 116K |
| Servers | ✅ Valid PNG, 1242×610 | 84K |
| Bedrock | ✅ Valid PNG, 1242×610 | 78K |
| Mods | ✅ Valid PNG, 1242×610 | 69K |
| Downloads | ✅ Valid PNG, 1242×610 | 72K |

### DPI/Resolution Testing

| Resolution | Status |
|---|---|
| 1280×720 | ✅ Renders correctly |
| 1366×768 | ✅ Renders correctly |
| 1920×1080 | ✅ Renders correctly |
| 2560×1440 | ✅ Renders correctly |
| 3840×2160 | ✅ Renders correctly |

## Installer Testing

### Build

| Metric | Result |
|---|---|
| Installer compiler | Inno Setup 6.7.3 |
| Installer name | AmalgamLauncher-3.0.0-beta.1-Setup.exe |
| Installer size | 20.6 MB |
| Compile time | 6.5 seconds |

### Install Test

| Metric | Result |
|---|---|
| Install directory | C:\Users\David\AppData\Local\Temp\amalgam-test |
| Install mode | Silent (/VERYSILENT) |
| Files installed | ✅ launcher, DLL, 19 bridges, branding, docs |
| launcher.json created | ✅ From template |
| Prerequisites check | ✅ Native bridge, Java bridges, Java runtime, Bedrock |

### Launcher Smoke Test

| Metric | Result |
|---|---|
| --doctor output | ✅ All checks pass |
| Native bridge | READY |
| Java bridges | 19 staged |
| Java runtime | 1 system Java detected |
| Bedrock | READY (detected) |
| Microsoft account | ACTION needed (expected) |

### UI Snapshot Test

| Metric | Result |
|---|---|
| Snapshot created | ✅ |
| No crashes | ✅ |
| No ImGui assertions | ✅ (fixed) |

### Uninstall Test

| Metric | Result |
|---|---|
| Uninstaller | ✅ Works |
| User data preserved | ✅ (launcher.json, logs) |
| No errors | ✅ |

## Edge Case Testing

### Corrupt Config Recovery

| Test | Result |
|---|---|
| launcher.json = "CORRUPT_JSON" | ✅ Launcher recovers gracefully |
| No crash | ✅ |
| Default config created | ✅ |

### Missing Components

| Test | Result |
|---|---|
| No bridges | ✅ Doctor shows ACTION needed |
| No Java | ✅ Doctor shows ACTION needed |
| No Microsoft account | ✅ Doctor shows ACTION needed |

## Bug Fixes Verified

### ImGui Assertion (Critical)

| Metric | Result |
|---|---|
| Issue | IsPopupOpen() with string ID + ImGuiPopupFlags_AnyPopupLevel |
| Fix | Removed ImGuiPopupFlags_AnyPopupLevel from 6 calls |
| Verification | ✅ No assertion failures in Debug mode |
| Affected lines | 4584-4586, 10613-10615 |

### Release Naming

| Metric | Result |
|---|---|
| Old name | AmalgamLauncher-V2-Setup.exe |
| New name | AmalgamLauncher-3.0.0-beta.1-Setup.exe |
| Verification | ✅ Zero remaining V2 references |

## Security Verification

| Check | Result |
|---|---|
| Secret scan (repo) | ✅ 0 unexpected findings |
| Secret scan (package) | ✅ 0 findings |
| No private keys | ✅ |
| No credentials | ✅ |
| No debug dumps | ✅ |

## Documentation

| Document | Status |
|---|---|
| Release notes | ✅ AMALGAM-V3-BETA-1-RELEASE-NOTES.md |
| Tester guide | ✅ BETA-TESTER-GUIDE.md |
| Known issues | ✅ Included in release notes |
| Setup notes | ✅ BETA-SETUP-NOTES.txt |
| Go-live report | ✅ go-live-report-2026-08-21.md |

## Package Contents

| File | Status |
|---|---|
| amalgam_launcher.exe | ✅ |
| amalgam.dll | ✅ |
| 19 bridge jars | ✅ |
| Branding assets | ✅ |
| launcher.json.template | ✅ |
| prerequisites.json | ✅ |
| component-manifest.json | ✅ |
| release.sha256 | ✅ |
| sbom.cdx.json | ✅ |
| beta-tester-guide.md | ✅ |
| SHA256SUMS.txt | ✅ |
| inventory.json | ✅ |

## Remaining External Gates

| Gate | Status | Required |
|---|---|---|
| Supabase live deploy | BLOCKED | Access token + DB password |
| Microsoft auth | BLOCKED | Client ID + test account |
| Real Vanilla launch | BLOCKED | Gates above |
| CI workflow | BLOCKED | GitHub Actions |

## Final Verdict

**LOCAL VERIFICATION: COMPLETE**

All local gates pass:
- ✅ Fresh build 333/333
- ✅ All tests 160/160
- ✅ Visual testing: 10 pages, 5 DPI sizes
- ✅ Installer: build, install, run, uninstall
- ✅ Edge cases: corrupt config, missing components
- ✅ Bug fixes verified (ImGui assertion, naming)
- ✅ Security scan clean
- ✅ Documentation complete

The beta is ready for external testing once the three mandatory gates (Supabase, Microsoft auth, real launch) are unlocked with owner-supplied credentials.
