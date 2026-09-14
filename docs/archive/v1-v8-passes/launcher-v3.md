# Launcher V3 — Production Surface

V3 is the production upgrade of the V2 launcher surface. It keeps the single
unified launcher and isolated Minecraft profiles, but removes prototype-style
presentation and makes every visible action resolve to real provider, profile,
download, or runtime state.

## V3 principles

- **Real state first:** no generic “provider result” placeholders when a source,
  project, version, or profile is known.
- **One workflow:** Discover, Browse, Project Details, Install, Downloads, and
  My Modpacks share the same selected project/profile/download state.
- **Provider adapters:** Modrinth and CurseForge remain separate adapters behind
  one content model; provider-specific metadata is preserved and displayed.
- **Safe mutation:** installs, updates, deletes, repairs, and imports show the
  target profile, compatibility, progress, and recovery path before changing it.
- **No dead routes:** every navigation item either opens a real workflow or is
  removed; legacy screens cannot silently replace the current view.
- **Release evidence:** every V3 slice must build, pass tests, and have a smoke
  path that exercises the real launcher binary.

## Workstreams

1. **Content discovery** — provider badges, real project selection, pagination,
   compatible file filters, dependency summaries, attribution, and retry/error
   states.
2. **Profile lifecycle** — explicit target selection, conflict previews, update
   diffs, restore points, clean import/export, and clear modified/published state.
3. **Download engine** — durable jobs, per-file progress, cancellation, retry,
   resumable failures, and an actionable history view.
4. **Runtime readiness** — clean-room Java/loader/Bedrock validation, account
   state, prerequisite bootstrap, crash diagnostics, and launch recovery.
5. **UI system** — responsive layout, keyboard navigation, accessible feedback,
   consistent empty/loading/error states, and removal of dead prototype routes.
6. **Release** — deterministic packaging, signing/bootstrap, secrets hygiene,
   CI smoke validation, and a versioned V3 migration note.

## V3 acceptance bar

- A user can search a provider, open a real project, select a compatible file,
  understand dependencies, install it into a named profile, watch the durable
  job, and recover from failure without opening the console.
- A published pack update shows what will change before applying it and can be
  restored from the generated restore point.
- The launcher has no visible mock/demo labels or dead routes in normal mode.
- A clean build and the complete CTest suite pass for every merged V3 slice.

## Current V3 slice

The shared selected-project model, provider-aware browsing, compatible-file
selection, profile update feedback, durable download history, keyboard
navigation, and real project-detail routes are implemented. The next V3 work is
archive-level update diffs and dependency/conflict previews before an install or
profile update is confirmed.
