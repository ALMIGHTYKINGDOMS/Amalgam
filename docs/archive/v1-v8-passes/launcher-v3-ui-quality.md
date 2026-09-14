# Launcher V3 UI / GUI Quality Plan

This plan upgrades the existing V2/V3 launcher surface from “functional beta”
to a coherent production GUI. The goal is not more screens; it is consistent,
real, understandable workflows across every existing screen.

## Current baseline

### User-base polish slice — 2026-08-21

The latest focused pass improves the high-traffic account and social surfaces
without introducing a second design language:

- Account statistics now use shared DPI-aware breakpoints (4 columns, 2
  columns, or a single column) instead of overflowing narrow account cards.
- The account header stacks avatar, identity, and actions when the content
  region is narrow, keeping sign-in and security actions reachable.
- Essentials friends uses a stacked, scrollable layout below the three-column
  breakpoint; activity, invites, status, and connection cards remain visible
  rather than collapsing into narrow unreadable columns.
- Social messages stack the friend list above the conversation at narrow widths,
  preserving usable message bubbles and composer space.
- Friend and party overflow actions now use shared vector icon buttons with
  explicit tooltips; Bedrock profile favorite/menu actions use the same pattern.
- Breadcrumbs, theme switching, and Essentials quick-action tiles now use the
  shared interaction/accessibility affordances instead of emoji or unlabeled
  raw controls.
- New breakpoint decisions live in the ImGui-free UI model and are covered by
  the UI model regression test.

The full native rebuild remains an external verification gate when the Visual
Studio/CMake environment is not available to the current shell.

Already upgraded:

- Modrinth + CurseForge provider-aware browsing
- Pagination and retryable provider searches
- Project detail pages with compatible files and dependency previews
- Install confirmation with target/conflict context
- Settings save/reload and CurseForge connection testing
- Published Settings credential boundary with password-protected Admin tabs
- Native client module controls with per-profile persistence, safe mode, bulk
  toggles, bounded parameters, and immediate queue clearing
- Managed-content update preview and restore-backed apply flow
- Persistent retry metadata for core install/update jobs
- Keyboard navigation enabled

Implemented in the foundation slice after this plan was written:

- ImGui-free `ScreenState<T>` contracts with request-id guarded provider results.
- Structured alert/recovery center for success, warning, and failure outcomes.
- Durable operation metadata for phase, target, provider, bytes, rate, ETA,
  cancellation, and restart reconciliation.
- Launch preflight review with profile, bridge, account, Java, and disk checks.
- Collapsible/automatic compact sidebar for narrow windows and DPI-aware layout.
- Provider ownership shown in installed-content and install-conflict previews.
- Per-file managed update selection and resumable `.part` downloads.

## Quality rules for every V3 change

- No mock/demo/provider-placeholder labels in normal mode.
- No network request runs on the render thread.
- Every async action has loading, success, empty, failure, cancel, and retry states.
- Every mutation names the target profile and has a recovery path.
- Every provider result keeps its source, project id, version, and compatibility data.
- Every merged slice must build and pass all tests.

## Work package 1 — UI foundation

1. Consolidate repeated ImGui patterns into shared components:
   page headers, tabs, cards, badges, empty states, error panels, progress rows,
   confirmation modals, and toast notifications.
2. Add a single screen-state model for `idle/loading/ready/empty/error` rather
   than page-specific booleans and ad-hoc status strings.
3. Add a notification center with success, warning, failure, and action buttons.
4. Add responsive layout rules: compact sidebar, narrow project detail layout,
   wrapped filter bars, and DPI-aware spacing.
5. Add tooltips, keyboard focus visibility, and consistent disabled-button text.

**Done when:** every existing page uses the same loading/error/empty patterns,
the UI remains usable at the minimum window size, and no operation requires the
console to understand what happened.

## Work package 2 — Discover / provider quality

1. Finish project pages with author, license, website, issues, categories,
   screenshots, dependency tree, file hashes, and file dates.
2. Add provider-specific filters for loader, Minecraft version, category, sort,
   and source while preserving pagination state.
3. Add a real selected-project model shared by Home, Discover, Browse, and the
   project page so selection never falls back to the first result.
4. Add a dependency graph preview with required/optional nodes and compatibility
   warnings before installation.
5. Add stale-cache indicators, refresh controls, rate-limit messages, and retry
   backoff instead of generic empty results.

**Done when:** a user can discover a real project, understand its compatibility
and dependencies, choose a release, and begin an install without ambiguity.

## Work package 3 — My Modpacks / profile quality

1. Replace filename-only conflict checks with ownership-aware conflict analysis.
2. Add update diff views for added, removed, replaced, and unchanged content.
3. Add per-file update selection and a final review modal before applying updates.
4. Add screenshot and world thumbnail galleries with safe delete/backup actions.
5. Improve profile cards with last played, current activity, update state, health,
   and clear published/modified/custom labels.
6. Make duplicate, import, export, repair, restore, and delete flows use the same
   confirmation and progress components.

**Done when:** profile management feels like one coherent library instead of a
   collection of separate tools.

## Work package 4 — Downloads / operations quality

1. Upgrade retry-from-start to resumable downloads using `.part` files and
   verified byte ranges where the server supports them.
2. Show per-file and aggregate progress, transfer rate, remaining time, and the
   current dependency/file name.
3. Add pause/cancel/retry/clear actions with safe job state transitions.
4. Persist enough job metadata to recover after restart without losing the target
   profile or provider context.
5. Add an operation history filter for active, completed, failed, and cancelled.

**Done when:** a user can leave the launcher during a large install, reopen it,
understand what happened, and resume or retry safely.

## Work package 5 — Settings / runtime quality

1. Add connection tests and clear status for CurseForge, Modrinth, AI providers,
   Microsoft account, Java, and Bedrock.
2. Add Java compatibility explanations and guided runtime/bootstrap actions.
3. Add launch preflight with missing Java, loader, bridge, account, disk-space,
   and audio-device checks before the launch button runs.
4. Add a readable crash/result screen with copyable diagnostics and next actions.
5. Keep advanced settings behind an explicit advanced-mode boundary.

**Done when:** the launcher explains why a launch cannot proceed and provides a
   safe next action instead of leaving the user in a log panel.

## Work package 6 — Visual and release QA

1. Establish a small visual test matrix: minimum size, 125/150% DPI, light/dark
   Windows text scaling, empty library, long project names, provider errors, and
   large download queues.
2. Add UI smoke flows for search → project → preview → install, profile update,
   failed-job retry, and settings save/reload.
3. Remove dead code/routes and audit all user-facing copy for prototype language.
4. Validate clean-room launches for supported loaders and Bedrock.
5. Finish packaging, bootstrap, signing, and release evidence.

**Definition of V3 quality:** the launcher is understandable without the console,
safe to use on a real profile, visually consistent at supported sizes, and every
major workflow has a tested recovery path.

## Recommended execution order

1. UI foundation + notification/state model
2. Discover/project/dependency experience
3. Profile conflict/update/recovery experience
4. Durable downloads and operation history
5. Runtime preflight and diagnostics
6. Visual QA, clean-room validation, packaging, and signing
