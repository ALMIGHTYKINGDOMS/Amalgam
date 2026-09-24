# AMALGAM V9 — OPERATIONAL READINESS REPORT

**Version:** 1.0.0
**Channel:** stable
**Date:** September 24, 2026
**Status:** TECHNICALLY READY — EXTERNAL OWNER GATE

> This is the authoritative operational-readiness record. Earlier V8/V9
> reports are historical evidence and must not be used as the current product
> or release status.

---

## EXECUTIVE SUMMARY

The repository-resolvable launch work is complete: a clean native Release
build, all current Java bridge outputs, the statically validated Bedrock input,
package validation, installer validation, smoke checks, visual QA, and an
isolated installer install/uninstall pass all succeed. The release is not yet a
public distribution because owner-controlled gates remain: a trusted Windows
code-signing identity, Microsoft/Xbox publisher approval for direct Minecraft
sign-in, publication of the signed update feed/release asset, and one clean-
machine acceptance pass.

Bedrock is deliberately **Coming Soon** in 1.0.0. Its package and manifest are
validated as static release inputs, but the launcher does not detect, launch, or
import Bedrock content in this release. No Bedrock runtime test was performed.

---

## OPERATIONAL MATRIX

### CORE RUNTIME
| Requirement | Status |
|-------------|--------|
| Launcher EXE | PRESENT |
| Native DLL | PRESENT |
| Windows Dependencies (tar, winsqlite3, msvc-runtime) | VERIFIED |
| Java Bridges (19 jars) | PRESENT |
| Bedrock Package | Static package validated; runtime Coming Soon |
| Config Template | PRESENT |

### JAVA RUNTIME
| Requirement | Status |
|-------------|--------|
| Java Runtime Manager | OPERATIONAL |
| Adoptium API Integration | VERIFIED |
| Auto-Selection Logic | VERIFIED |
| Download/Extract/Validate | VERIFIED |
| Supported Versions (8, 11, 17, 21) | VERIFIED |

### SERVER RUNTIME RESOLVER
| Software | Status | Source |
|----------|--------|--------|
| Vanilla | OPERATIONAL | Mojang Official |
| Paper | OPERATIONAL | Fill API v3 |
| Purpur | OPERATIONAL | Fill API v2 |
| Fabric | OPERATIONAL | FabricMeta API v2 |
| Quilt | OPERATIONAL | Quilt Meta API v3 |
| Folia | OPERATIONAL | Fill API v3 |
| Velocity | OPERATIONAL | Fill API v3 |
| Forge | EXPERIMENTAL | Manual workflow |
| NeoForge | EXPERIMENTAL | Manual workflow |
| Spigot | GATED | BuildTools required |

### BEDROCK
| Requirement | Status |
|-------------|--------|
| Minecraft Detection | Coming Soon — runtime intentionally gated |
| Package Validation | VERIFIED |
| Manifest/UUIDs | CORRECT |
| Version 1.0.0 | ALIGNED |

### PROVIDERS
| Provider | Status | Notes |
|----------|--------|-------|
| Modrinth | OPERATIONAL | Public API |
| CurseForge | Configured | Live production key/endpoint verification remains owner-controlled |

### ESSENTIALS
| Requirement | Status |
|-------------|--------|
| Friends | OPERATIONAL |
| Presence | OPERATIONAL |
| Invites | OPERATIONAL |
| Sessions | OPERATIONAL |
| P2P (libdatachannel) | OPERATIONAL |
| TURN Relay | OPERATIONAL |

### UPDATER
| Requirement | Status |
|-------------|--------|
| Manifest Parsing | VERIFIED |
| SHA-256 Validation | VERIFIED |
| Signature Verification | VERIFIED |
| Anti-Rollback | VERIFIED |
| Tests (12/12) | PASS |
| Authenticode | BLOCKED — no trusted release certificate is available here |

---

## PACKAGE CONTENTS VERIFIED

- Launcher EXE (9.8 MB)
- Native DLL (931 KB)
- 19 Java Bridges
- Bedrock Package (9.4 MB)
- Documentation (Release Notes, User Guide)
- Component Manifest
- SBOM
- SHA-256 Hash
- Branding Assets

---

## VERIFICATION EVIDENCE

| Check | Result |
|-------|--------|
| Native CTest | 45/45 passed |
| Runtime agent | 38/38 passed |
| Bedrock static validator | 4/4 passed; no runtime activity |
| Visual audit | 8/8 viewport checks passed |
| Installer sandbox | Silent install, prerequisite smoke, and uninstall passed |
| Secret scan | 0 findings in candidate/package scope |

The current exact ZIP candidate is
`artifacts/release-candidate-2026-09-24/rc-20260924T044334Z/amalgam-1.0.0.zip`
(SHA-256 `18AF3E98E0A456C833DF845D5FC91E67F4EB5A362B2622890514432F026AF911`,
113,006,167 bytes). The locally signed feed manifest has SHA-256
`DE58572EB7CCF2394951822C9F28961EB2D4D596E56456D83F467837E8BC7075`.
The previously validated installer remains in the prior immutable candidate;
the matching new installer is intentionally not fabricated while the local
Inno compiler is unavailable without elevation. The corrected CI workflow is
ready to produce it on a build host with Inno Setup installed.

---

## EXTERNAL BLOCKERS

1. **Microsoft/Xbox approval** — direct device-code sign-in remains safely held
   behind the publisher approval; the official-launcher fallback is available.
2. **Trusted code signing** — sign the launcher EXE, DLL, installer, and update
   artifacts with a certificate trusted by target Windows users.
3. **Update/website publication** — publish the signed ZIP and manifest at the
   owner-controlled URL, then verify them from a clean machine.
4. **Supabase Auth dashboard** — enable leaked-password protection before public
   account launch; the live advisor still reports it disabled.
5. **Clean-machine Java handoff** — validate the signed installer and real Java
   profile handoff outside the development machine. Bedrock remains excluded.
6. **Hosted build execution** — the GitHub Actions Windows job is ready but is
   currently rejected before startup because the repository account has a
   billing lock. Restore Actions billing or run the same workflow on a Windows
   host with Inno Setup installed before publishing the matching installer.

---

## FINAL DECISION

# TECHNICALLY READY — EXTERNAL OWNER GATE

No repository-resolvable P0/P1 release blocker remains. The candidate is ready
for owner-controlled signing, publication, and approval completion. It is not
honest to call the public launch complete until the external gates above are
closed and re-verified.

---

**Document Version:** 2.0
**Last Updated:** September 24, 2026
