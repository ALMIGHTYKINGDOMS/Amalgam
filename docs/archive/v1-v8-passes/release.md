# Release Checklist

## Product scope

The public product scope keeps the complete Amalgam utility client, including
the native injection path and all advanced utility modules. Release hardening
must therefore improve reliability, transparency, safety controls, and support
documentation without removing those capabilities.

CurseForge-quality is the release quality target, not a guarantee of platform
approval. Before submission, perform a separate moderation, EULA/ToS, license,
and distribution review for the complete feature set. Do not describe the
artifact as CurseForge-approved until that review has actually occurred.

## Build

After building and copying runtime bridge jars, create a clean archive without caches or local credentials:

```
powershell -ExecutionPolicy Bypass -File tools\package-release.ps1 -Version 1.0.0 -MicrosoftClientId <publisher-app-client-id>
powershell -ExecutionPolicy Bypass -File tools\validate-package.ps1 -Package dist\amalgam-1.0.0.zip
```

If Inno Setup 7 is installed, compile the per-user installer with `ISCC.exe /DSourceDir=..\dist\amalgam-<version> /DMyAppVersion=<version> installer\AmalgamLauncher.iss`. The staging directory must be the one just validated, and the installer never includes `launcher.json` with real credentials.

1. Build the native launcher and DLL with the pinned MSVC toolchain:

   ```
   call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
   cmake --build cpp\build
   ctest --test-dir cpp\build --output-on-failure
   ```

2. Build Fabric and NeoForge bridges with JDK 21 and Gradle 9.5.0:

   ```
   gradle.bat -p java build
   gradle.bat -p java-neoforge build
   ```

3. Copy only non-`-sources.jar` bridge artifacts into `cpp/build/bridges/`.

The Forge 1.20.1 bridge is built separately with ForgeGradle 6.0.53, Gradle 8.8, and JDK 17. Fabric/NeoForge continue to use the pinned Gradle 9.5/JDK 21 builds.

## Verify

- All CTest suites pass, including native telemetry, SQLite outbox, launcher `--check-java`, and the Java protocol/codec test.
- Run `amalgam_launcher.exe --inspect <mc_id> <loader>` for each release target that will be distributed.
- Run `amalgam_launcher.exe --check-prereqs` on the target machine before distributing; diagnostics persistence is opt-in, but the Windows 10+ `winsqlite3.dll` component is required by the native DLL.
- With the required metadata/cache available, run `powershell -ExecutionPolicy Bypass -File tools\validate-cached-launches.ps1` to validate sequential Fabric, Quilt, NeoForge, and Forge 1.12.2/1.18.2/1.19.2/1.20.1 dry-runs. Do not run those cases concurrently because they share launcher caches.
- Run representative `--launch <mc_id> <loader> --dry-run` checks for Fabric, Quilt, NeoForge, and Forge.
- Run the clean extracted-package smoke check before any clean-machine work:
  `powershell -ExecutionPolicy Bypass -File tools\validate-package-runtime.ps1 -Package dist\amalgam-<version>.zip`.
- Confirm the native DLL and launcher are x64 and load from a clean staging directory.
- Confirm the target instance has the expected bridge jar, native directory, classpath separator, and per-instance Java/memory settings.

## Secret and artifact review

- Do not package `launcher.json` with real keys. Ship a documented template or let the launcher create local configuration.
- Do not package `%LOCALAPPDATA%\Amalgam\diagnostics.sqlite3` or any exported diagnostics CSV unless the user explicitly requested the export.
- Search the staging directory for API keys, tokens, `.hprof`, `.dmp`, `.log`, `.part`, `imgui.ini`, and Gradle caches.
- Run `powershell -ExecutionPolicy Bypass -File tools\verify-signing.ps1 -PackageDir dist\amalgam-<version> -InstallerPath dist\installer\AmalgamLauncher-3.0.0-beta.1-Setup.exe`; public release tags require valid signatures after configuring the signing certificate.
- Do not include build directories, source jars, debug symbols, test fixtures, or crash dumps in a normal release.
- Record exact Minecraft, loader, bridge, Java, and launcher versions in the release notes.
- The package script emits a SHA-256 `component-manifest.json`, `release.sha256`, and `sbom.cdx.json`; the validation script checks artifact hashes, bridge coverage, forbidden secrets/source jars, prerequisites, and archive integrity.

## Distribution notes

The launcher supports online metadata and downloads, so a release must include a clear offline failure message and must not silently substitute an unverified archive. Users should back up their instance directories before installer migrations and keep provider credentials outside shared archives.
