# Official Launch Readiness

Status: 2026-08-22 — release candidate engineering complete; external release
approvals remain.

## Completed in the product

- The launcher and DLL build successfully with native hardening enabled.
- All 36 configured CTest checks pass, including the launcher smoke tests,
  Java/Fabric/NeoForge/Forge bridge checks, server transport, Bedrock package,
  archive safety, updater, authentication, and content/install logic tests.
- Profile creation, server creation, Discover, and the installed-content
  browser use a shared supported-version catalogue. New profiles cannot be
  created with a random, unsupported target.
- The release archive validates its 19 Java bridge jars, Bedrock add-on,
  branding, hashes, SBOM, and credential-free configuration template.
- Supabase backend-only tables have RLS enabled, no public or authenticated
  Data API privileges, and are accessed through authenticated RPCs. Helper and
  trigger functions have pinned search paths and are not anonymous endpoints.

## Required before broad public release

1. **Minecraft publisher approval** — enable and verify direct Microsoft/
   Xbox/Minecraft device-code sign-in with a real entitled account. Until then,
   keep the official Minecraft Launcher fallback enabled.
2. **Production provider verification** — validate live Modrinth and
   CurseForge search, install, update, attribution, and creator-license flows
   against approved production credentials.
3. **Code signing** — obtain a Windows code-signing certificate; sign the
   launcher, DLL, and Inno Setup installer; then verify the signatures and
   SmartScreen behavior from a clean Windows account.
4. **Clean-machine matrix** — run create/install/launch/update/rollback/
   uninstall tests for every supported Java loader, plus official Windows
   Bedrock detect/launch/add-on-import validation.
5. **Support and legal** — publish the final privacy notice, terms, support
   contact, provider attribution, and distribution policy.
6. **Supabase Auth setting** — enable leaked-password protection in the
   Supabase Auth dashboard before accepting production account registrations.

## Deliberate boundaries

- No service-role database credential ships in the launcher.
- No Microsoft password is requested by Amalgam.
- Bedrock is integrated through the official Windows app and approved add-on
  import; it does not use injected DLLs or bypass platform ownership,
  multiplayer, or marketplace controls.
- Existing imported/legacy profiles are preserved. The version picker limits
  new profiles to release-supported targets rather than deleting old profiles.
