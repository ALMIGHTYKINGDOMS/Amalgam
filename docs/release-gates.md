# Release Candidate Gates

Status: 2026-09-22.

The current launcher code, automated tests, renderer captures, and
credential-free package can be completed locally. The gates below require a
real account, a separate machine, a signing identity, a platform decision, or
other authority outside the source tree. They must be completed before calling
the product a public CurseForge-quality release.

## Complete locally

- Native launcher and DLL build successfully.
- All 36 configured CTest checks pass, including the Admin password verifier,
  config persistence, server-provider parsers, UI model contracts, launcher
  smoke checks, and native camera projection coverage.
- The release archive contains the launcher, DLL, supplied branding, SBOM,
  component hashes, a credential-free configuration template, and exactly 19
  runtime bridge jars.
- Provider installs use staging, hash verification, backups, rollback, and
  restart-safe recovery metadata.
- Published-pack updates inspect the downloaded creator archive before live
  changes, recording added/replaced/removed/unchanged managed-content counts in
  the durable operation timeline. The change classifier is covered for Modrinth
  and CurseForge manifests and for replaced/unchanged classification; CurseForge
  manifests carry project/file ids, so their final provider filenames are
  resolved during staging and flagged as such in review. Worlds, screenshots,
  logs, launcher metadata, and runtime files are excluded from both review and
  update payload.
- Microsoft sign-in guidance, profile/preflight UI, local diagnostics, and
  Bedrock capability detection are implemented without collecting passwords.
- Managed Temurin Java 8/11/17/21/25 downloads use Adoptium metadata, checksum
  and size verification, safe extraction, runtime metadata, and post-install
  `java -version` validation. A real Java 21 install and Fabric dry run pass.
- Published Settings keeps provider credentials and publisher/runtime controls
  behind a local Admin session. Admin passwords use a salted PBKDF2 verifier;
  provider credentials remain Windows DPAPI-protected.
- Direct renderer snapshots cover reference and minimum-size layouts without
  desktop-capture blur.
- A clean extracted-package runtime smoke test (`tools/validate-package-runtime.ps1`)
  validates each packaged candidate from a fresh extraction: Java detection,
  prerequisite checks, and a secret scan of the extracted files.
- `tools/release-gate.ps1` chains build, tests, packaging, installer, package
  integrity, runtime validation, installer inputs, the cached launch matrix,
  launcher smoke, the Node suites, the secret scan, the embedded-updater-key
  check, and Authenticode verification in one command with an honest summary.
- The gate fails closed when the package would ship without its public backend
  and sign-in configuration (`AMALGAM_MICROSOFT_CLIENT_ID`,
  `AMALGAM_SUPABASE_URL`, `AMALGAM_SUPABASE_PUBLISHABLE_KEY`). Pass
  `-AllowInertConfig` and `-AllowInertUpdates` only for a deliberately
  non-releasable local test package; the summary labels that result accordingly.
- The production gate (`-RequireSigned`) has an explicit byte-safe ordering:
  it signs the staged launcher/DLL, regenerates the component manifest, SBOM,
  release hashes, and ZIP from those signed bytes, builds the installer from
  that finalized stage, signs the installer, and then verifies all artifacts
  use one signing certificate. It requires an explicit signing identity,
  update private-key path, and publisher-approved HTTPS payload URL; it does
  not guess an endpoint or publish anything.
- The production gate generates a fresh manifest only from the final ZIP,
  signs both the manifest and payload, and verifies its hash, size, and both
  signatures with the public key embedded in the shipped launcher. The
  checked-in feed is not proof of a current candidate until this step has run
  for that candidate.
- A real install/uninstall cycle against the production Setup.exe passes:
  silent install verifies files and prereq checks; the data-preserving
  uninstall removes application files while profiles, worlds, and backups
  survive.

## Required before a public release

1. **Minecraft publisher approval and account validation.** Enable direct
   device-code sign-in only after the publisher's Minecraft application is
   approved, then sign in with an entitled Microsoft/Minecraft Java account and
   perform an actual Java launch. Until then, keep the official Launcher
   fallback enabled.
2. **CurseForge production approval.** Use a production API key, verify the
   intended distribution and attribution terms, and run a live install/update
   against the production service.
3. **Clean-machine launch matrix.** Test supported Fabric, Quilt, Forge, and
   NeoForge profiles from a fresh Windows account or VM. Record install,
   launch, update, rollback, and uninstall outcomes for each supported loader.
   `tools/qa-install-uninstall.ps1` automates the silent install and the
   data-preserving uninstall against the real Setup.exe.
4. **Bedrock live validation.** Install the Windows Bedrock UWP package on a
   test machine with its proper Store/Microsoft entitlement, then verify detect,
   launch, and addon import behavior.
5. **Code signing and installer.** Obtain a Windows code-signing certificate.
   The required order is: sign the staged launcher/DLL; run
   `tools/package-release.ps1 -FinalizeExistingStage -RequireSignedStagedBinaries`;
   build the installer from that refreshed stage; then sign the installer.
   `tools/release-gate.ps1 -RequireSigned` automates and verifies those phases
   when supplied the signing identity and update-manifest inputs. Verify
   SmartScreen/reputation behavior and upgrade/uninstall on a clean machine.
6. **Update feed deployment.** Supply the production gate with the
   publisher-approved HTTPS ZIP URL; it creates and locally verifies
   `dist/update-feed/manifest.json` against that exact final ZIP. Upload that
   manifest and ZIP only to the approved endpoint, then fetch both back and
   compare them verbatim. The official website URL alone is not authorization
   to assume a release-feed path.
7. **Native module compatibility and policy review.** Perform controlled
   single-player and permitted-server checks for the native bridge/modules,
   then make the distribution policy explicit for advanced functionality.
8. **Support and legal review.** Approve third-party branding/content handling,
   provider attribution, privacy copy, EULA/support workflow, and the final
   release notes.

## Known non-blocking engineering follow-ups

- `ui.cpp` is still a large file. The shared UI model and components are tested,
  but a full file-level split is maintainability work, not a reason to delay
  the verified release candidate.
- CurseForge manifests identify remote files by project/file id rather than
  their final provider filename. Archive review reports the safe local content
  changes it can determine and clearly marks provider filenames as resolved
  during staging instead of inventing names.
- The operation center persists/reconciles jobs and supports safe staged
  recovery. A future centralized operation manager can add richer cross-job
  rate/ETA and byte-range resumptions without changing the user-facing safety
  contract.
