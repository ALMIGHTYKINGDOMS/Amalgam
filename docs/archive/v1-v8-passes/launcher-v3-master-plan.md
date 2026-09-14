# Amalgam Launcher V3 Master Plan

This is the execution plan for taking the launcher from a working V2/V3 beta to a
polished, CurseForge-style product surface. It covers product behavior, UI
architecture, profile/content workflows, downloads, runtime readiness, testing,
and release quality.

The goal is not to add more pages. The goal is to make every existing workflow
feel like one dependable application.

## Completion Record - 2026-08-20

The user-facing implementation path in this plan is complete to release-
candidate scope. The remaining hard gates require external authority or a real
entitled environment; they are tracked in `docs/release-gates.md` rather than
being misrepresented as unfinished source work.

| Milestone | Current closure |
|---|---|
| M0 | Deterministic fixture mode, state/request contracts, render captures, and operation/screen logging are in place. |
| M1 | Shared UI primitives, tokens, screen/request models, notices, keyboard behavior, and responsive layout are in place. Full file-level splitting of `ui.cpp` remains maintenance work. |
| M2 | Shell, Home, Library, health/status, safe create/import/duplicate flows, and responsive card layouts are complete. |
| M3 | Real Modrinth/CurseForge browsing, details, dependency/install planning, provider status, and safe install handoff are complete. |
| M4 | Isolated profile lifecycle, ownership-aware content, restore/recovery, galleries, and staged creator updates are complete. Full archive-level file diff presentation remains an enhancement. |
| M5 | Safe staged downloads, pause/cancel, durable recovery, history, retry behavior, and a visible protected queue for profile-changing work are complete. A richer centralized byte-range operation manager remains an enhancement. |
| M6 | Readiness/preflight, Java/account/provider status, local diagnostics, and recovery actions are complete locally. Live account and clean-machine launch proof are external gates. |
| M7 | Bedrock capability/addon adapter, AI handoff, servers, worlds, and screenshots are implemented. Bedrock live validation requires a machine with the UWP app. |
| M8 | Physical-pixel DPI rendering, direct OpenGL visual snapshots, 36/36 configured CTest checks, password-protected Admin settings, client module parity, and credential-free package validation are complete. Signing, installer proof, platform approval, and clean-machine evidence remain external gates. |

The sections below remain the architecture and acceptance reference. The
completion record above is the current status source of truth.

## 1. Product outcome

Amalgam V3 is complete when a user can:

1. Open the launcher and immediately understand their library, current profile,
   account state, and pending work.
2. Search Modrinth or CurseForge, compare real projects and compatible files,
   inspect dependencies, and install into a named profile.
3. Import, duplicate, repair, update, restore, export, and launch a profile
   without losing track of what will change.
4. Leave during a large operation, reopen the launcher, and resume or recover
   without using a console or manually repairing files.
5. Understand why a launch cannot proceed and receive a safe next action.
6. Use the same quality of flow for Java, Quilt, Forge, NeoForge, and the
   Bedrock adapter, with unsupported cases explained instead of hidden.

The product contract is: **real state, one source of truth, safe mutations,
visible recovery, and no prototype language in normal mode.**

## 2. Current baseline and follow-up engineering gaps

The primary V3 implementation path is complete for the local release-candidate
scope. The items below are maintainability work, deeper runtime validation, or
future enhancements; they are not missing top-level launcher pages.

### Already real

- Modrinth and CurseForge search, provider badges, pagination, compatible files,
  project details, dependency previews, and API-key testing.
- Isolated profiles with library cards, groups, favorites, import, duplicate,
  export, repair, content management, restore points, performance profiles, and
  launch actions.
- Managed installs, ownership metadata, update previews, retryable jobs,
  persistent download history, and staged published-pack updates with restore
  points and modified-profile copies.
- Java, loader, bridge, Bedrock, account, AI, and settings surfaces.
- Native build, 36/36 configured CTest checks, Java bridge build coverage,
  packaging, and a Windows CI workflow.

### Follow-up engineering gaps

1. `cpp/launcher/src/ui.cpp` is still a large render/controller file.
   Rendering, navigation, networking triggers, persistence, and mutation logic
   remain interleaved even though the shared component layer is now extracted.
2. `UiState` still mixes durable data, async activity, transient modal state, and
   presentation state. Further separation would make stale results, duplicate
   actions, and recovery behavior easier to reason about.
3. Feature-local drawing remains in some pages. New work should continue using
   the shared cards, buttons, progress, notice, and responsive-layout contracts.
4. The durable job model now exposes phase, bytes, rate, ETA, retry, and restart
   recovery. A centralized cross-job manager with dependencies and richer
   byte-range resume behavior remains a future enhancement.
5. Ownership metadata is strong for new managed installs but incomplete for
   older manually copied files and full archive-level published-pack diffs.
6. Runtime and Bedrock support have adapters and dry-runs, but clean-room
   validation, entitled-account launch proof, and live Bedrock validation remain
   external release gates.
7. Backend tests and deterministic UI fixtures are in place; failure injection
   and real end-to-end smoke flows should continue to expand coverage.

### Important sequencing rule

Do not add another major top-level screen until the UI foundation and shared
operation model are in place. New screens built on the current shape would
increase duplication and make the final product less consistent.

## 3. Target architecture

The launcher should be organized into six layers with one-way data flow:

```text
Provider/runtime/filesystem services
              |
        domain models
              |
       operation manager
              |
      application UI state
              |
       shared ImGui views
```

### 3.1 Domain models

Introduce stable models that are independent of ImGui:

- `ProjectRef`: provider, project id/slug, title, type, source URL.
- `ProjectDetails`: description, author, license, categories, gallery, links,
  versions, hashes, dates, and provider attribution.
- `CompatibleFile`: file identity, loader/version compatibility, dependencies,
  size, hash, changelog, and download state.
- `InstallPlan`: target profile, selected file, required dependencies, optional
  dependencies, conflicts, replacements, disk estimate, and warnings.
- `ProfileHealth`: loader/runtime status, managed/unmanaged content, conflicts,
  stale metadata, available updates, and last launch result.
- `LaunchPreflight`: every check, severity, explanation, and recommended action.
- `Operation`: durable id, kind, target profile, provider context, phase,
  progress, bytes, rate, ETA, cancellation, retry, and error details.
- `Notice`: severity, title, body, timestamp, related operation, and actions.

### 3.2 Async state

Replace page-specific booleans with a reusable state shape:

```text
idle -> loading -> ready
                 \-> empty
                 \-> error (retryable or terminal)
```

Each async request must carry a request id and target key. A late response may
only update the matching request, preventing an old search or profile result
from overwriting the current screen.

### 3.3 Operation manager

Create one manager for installs, imports, updates, repairs, exports, runtime
bootstrap, and AI work. UI code submits commands and subscribes to snapshots;
workers never directly manipulate widgets.

Required capabilities:

- queued, running, paused, cancelling, completed, failed, and cancelled states;
- per-step and aggregate progress;
- persistent metadata and restart reconciliation;
- retry policy and safe idempotency keys;
- cancellation checkpoints;
- structured errors with user-facing recovery actions;
- log correlation without making the log the primary UI.

### 3.4 Persistence and migrations

Version the local data formats before adding more fields:

- `launcher.json`: settings and provider/account references;
- `downloads.json`: operation history and recovery metadata;
- profile metadata: ownership, source, version, restore points, and health;
- local content manifest: file ownership, hashes, enabled state, and provenance.

Every schema change needs a migration, a backup of the previous file, and a
fixture test. Secrets remain local and protected according to `docs/security.md`.

## 4. Target information architecture

The primary shell should have a stable, short navigation model:

- **Home**: current profile, last launch, pending operations, recommended
  content, and actionable health warnings.
- **Library**: all profiles, search, groups, favorites, sort/filter, create,
  import, duplicate, export, repair, and delete.
- **Discover**: provider search, filters, project details, dependencies, and
  install/update actions.
- **Downloads**: active queue, history, retry/resume, and operation details.
- **Servers**: saved servers and profile-aware launch targets.
- **Bedrock**: UWP detection, addon install, and capability status.
- **Settings**: account, providers, Java, storage, appearance, AI, advanced
  diagnostics, and reset/migration tools.

Profile detail remains a sub-navigation, not a second launcher. Its tabs should
be limited to Overview, Content, Worlds, Screenshots, Versions, Logs, Settings,
and Manage, with all mutation flows using the same shared dialogs and progress
views.

Global search, downloads, account status, and settings remain available from the
top bar. A single selected project/profile object must be shared across Home,
Discover, and Library so selection never silently falls back to the first card.

## 5. Design system and GUI quality bar

### 5.1 Component inventory

Build and reuse these components before polishing individual pages:

- page header with breadcrumb, title, subtitle, and primary action;
- responsive two-column and three-column layouts;
- project/profile cards with image fallback, status, metadata, and actions;
- provider, loader, compatibility, ownership, and health badges;
- filter bar with wrapped compact mode;
- loading skeleton, empty state, error panel, and stale-cache banner;
- progress row with phase, bytes, rate, ETA, pause/cancel/retry;
- notification/toast center and persistent operation drawer;
- confirmation, install-plan, update-review, restore, delete, and conflict
  dialogs;
- key/value rows, copyable diagnostics, changelog blocks, dependency tree, and
  gallery tiles.

### 5.2 Visual rules

- One spacing, radius, border, typography, and color token set.
- Use real icons/assets with text fallbacks; avoid improvised glyphs for primary
  navigation.
- All text wraps or truncates intentionally; no clipped project names or errors.
- Buttons state what they do and why they are disabled.
- Destructive actions show the exact profile/file target and recovery option.
- Loading, empty, failure, success, and cancelled states are visually distinct.
- The console is a diagnostic detail view, never the only explanation.

### 5.3 Window and input behavior

Support the minimum window size, 125% and 150% DPI, keyboard-only navigation,
visible focus, tab order, escape-to-close for overlays, and safe resizing. The
layout must remain usable with long translations/metadata even though the
initial language is English.

## 6. Execution milestones

Each milestone ends with a build, test, and review gate. The order is deliberate.

### M0 - Baseline freeze and instrumentation

**Deliverables**

- Inventory every route, modal, button, background worker, and persistence write.
- Mark each path as real, partial, unsupported, or dead; remove/disable dead
  routes in normal mode.
- Add a stable UI test fixture mode with fake provider/profile/job data.
- Add operation and screen-state logging with correlation ids.
- Capture the current visual baseline at minimum size and common DPI scales.

**Exit gate**

Every visible action has an owner, a target state, a failure state, and a test
fixture. No new surface is accepted without this inventory.

### M1 - UI foundation and state separation

**Deliverables**

- Split the monolithic UI into shell, components, navigation, and feature views.
- Add design tokens and shared components from section 5.
- Introduce typed screen states and request ids.
- Add notification center, global operation drawer, modal coordinator, and
  centralized keyboard/focus behavior.
- Move network/filesystem triggers behind service commands and result snapshots.

**Exit gate**

Every existing page uses the same loading/empty/error/progress/confirmation
components. A failed operation is understandable without opening the console.

### M2 - Shell, Home, and Library

**Deliverables**

- Finalize the navigation model and remove duplicate/legacy entry paths.
- Redesign Home around current profile, last launch, pending jobs, and health.
- Make Library the authoritative profile list with stable selection, groups,
  favorites, filters, sorting, and keyboard navigation.
- Add profile health badges and a consistent create/import/duplicate flow.

**Exit gate**

Cold start, empty library, populated library, stale profile, and failed launch
all lead to clear next actions in under one screen of information.

### M3 - Discover and provider parity

**Deliverables**

- Unify Modrinth and CurseForge behind `ProjectRef`, `ProjectDetails`, and
  `CompatibleFile`.
- Add author/license/categories, screenshots, source links, hashes, dates,
  changelogs, provider attribution, and rate-limit messaging.
- Add loader, game version, type, category, source, sort, and compatibility
  filters with preserved pagination.
- Add dependency graph preview with required/optional nodes, unresolved nodes,
  incompatible nodes, and disk estimate.
- Add cache freshness indicators, refresh, retry backoff, and offline behavior.

**Exit gate**

Search -> project -> compatible file -> dependency plan -> target profile is a
single coherent flow for both providers, including provider failure and missing
API-key states.

### M4 - Profile lifecycle and managed content

**Deliverables**

- Ownership-aware conflict analysis for managed, local, imported, and modified
  files.
- Installed-content table with type, provider, version, enabled state, hash,
  last update, and safe actions.
- Update review showing added, removed, replaced, unchanged, and conflicted
  files, with per-file selection.
- Restore-point creation, restore preview, rollback result, and repair flow.
- Published/custom/modified state that is understandable and persistent.
- Galleries for screenshots/worlds with backup-before-delete behavior.

**Exit gate**

A user can update a published or custom profile, review every change, apply or
cancel it, and restore the previous state after a simulated failure.

### M5 - Durable downloads and operation center

**Deliverables**

- Replace start-over retries with resumable `.part` downloads and verified byte
  ranges where supported.
- Track per-file and aggregate bytes, rate, ETA, dependency/file name, and
  current phase.
- Implement pause, cancel, retry, resume, clear-history, and restart recovery.
- Persist target profile, provider, selected file, hashes, and operation phase.
- Add operation details with logs, error classification, and copyable support
  information.

**Exit gate**

Interrupting a large install, closing the launcher, reopening it, and resuming
produces the same verified final profile as an uninterrupted install.

### M6 - Runtime readiness and diagnostics

**Deliverables**

- Launch preflight for Java, loader, bridge, account, disk space, paths,
  permissions, audio device, and profile health.
- Guided Java/bootstrap actions with plain-language explanations.
- Runtime compatibility matrix for Fabric, Quilt, Forge, NeoForge, and Bedrock.
- Readable launch result/crash screen with classification, relevant logs,
  copy/export diagnostics, and next actions.
- Account/provider status cards with connection test and privacy explanation.

**Exit gate**

Every known pre-launch failure points to a specific repair or settings action;
unexpected failures still produce a useful diagnostic bundle.

### M7 - Secondary surfaces: Bedrock, AI, servers, screenshots

**Deliverables**

- Bedrock capability card, detection/install status, addon validation, and clear
  UWP limitations.
- AI assistant with provider/model status, progress, cancellation, structured
  plan review, and safe install handoff.
- Server list with profile association, launch target, and connection status.
- Screenshot/world browsing with real thumbnails, sorting, safe file actions,
  and empty/error states.

**Exit gate**

Secondary surfaces use the same components, operation model, settings status,
and recovery language as Java modpack workflows.

### M8 - Visual QA, performance, security, and release

**Deliverables**

- Visual test matrix: minimum size, 125/150% DPI, long metadata, empty states,
  provider errors, stale cache, large queues, and modals near screen edges.
- UI smoke flows using deterministic fixtures plus live provider smoke checks.
- Render/performance pass: no network on the render thread, bounded image/cache
  memory, no unbounded logs, and no duplicate background jobs.
- Security pass: secrets, URL policy, archive extraction, path handling,
  diagnostic redaction, and provider attribution.
- Clean-room launch matrix, bridge staging, package validation, installer,
  signing/bootstrap, upgrade migration, and uninstall verification.
- User-facing V3 migration and support documentation.

**Exit gate**

The release candidate builds cleanly, passes all native/bridge tests, passes the
UI smoke matrix, survives restart/recovery scenarios, and contains no visible
prototype/dead-route behavior.

## 7. First implementation backlog

These are the first tickets to execute, in order:

| ID | Work | Depends on |
|---|---|---|
| UI-001 | Create the route/action inventory and mark dead/partial flows | none |
| UI-002 | Extract theme tokens and shared layout primitives | UI-001 |
| UI-003 | Add `ScreenState<T>` and request-id guarded async results | UI-001 |
| UI-004 | Add `Notice` and `Operation` snapshots plus the global operation drawer | UI-003 |
| UI-005 | Move provider/profile/job commands out of render functions | UI-003, UI-004 |
| UI-006 | Split `ui.cpp` into shell, components, and feature view translation units | UI-002, UI-005 |
| UI-007 | Implement fixture data and UI smoke harness | UI-003, UI-006 |
| UI-008 | Redesign Home and Library on the new state/components | UI-002..UI-007 |
| UI-009 | Unify project selection and provider detail models | UI-005, UI-006 |
| UI-010 | Build the dependency/install-plan review surface | UI-009 |
| UI-011 | Replace update/install logs with structured operation details | UI-004, UI-005 |
| UI-012 | Add update diff, conflict ownership, and restore review | UI-011 |
| UI-013 | Add resumable download state and restart reconciliation | UI-004, UI-011 |
| UI-014 | Add launch preflight and readable crash/result screen | UI-004, UI-008 |
| UI-015 | Run visual matrix, clean-room matrix, package, and release gate | UI-007..UI-014 |

## 8. Test and verification strategy

### Unit and contract tests

- provider normalization, compatibility, pagination, dependencies, hashes, and
  rate-limit errors;
- install/update plan construction and conflict ownership;
- operation state transitions, cancellation, retry, resume, and migrations;
- profile health, restore points, launch preflight, and crash classification;
- rendering-independent navigation and selection rules.

### Deterministic UI fixtures

Fixture scenarios must cover:

- first run with no account, no Java, and empty library;
- populated library with favorites, groups, stale profiles, and long names;
- provider success, empty, unauthorized, rate-limited, offline, and malformed
  responses;
- required/optional/unresolved dependencies and file conflicts;
- active, paused, resumed, failed, cancelled, and completed operations;
- successful launch, missing prerequisite, audio failure, mod conflict, and
  unexpected crash.

### End-to-end smoke flows

1. Search -> project -> compatible file -> install -> launch.
2. Import -> inspect -> repair -> update preview -> restore.
3. Start large install -> cancel -> close -> reopen -> resume.
4. Save settings -> restart -> verify persistence -> test provider connection.
5. Launch each supported loader family in a clean profile where practical.
6. Validate Bedrock detection and addon install on a clean Windows account.

### Quality thresholds

- zero render-thread network calls;
- zero unbounded worker growth or duplicate operations for one command;
- no clipped primary action or unreadable error at supported sizes/scales;
- no silent destructive mutation;
- all 30 configured CTest checks remain green, plus the deterministic UI/domain fixtures;
- every release artifact passes package, manifest, installer, upgrade, and
  uninstall validation.

## 9. Risk register and decisions

| Risk | Mitigation |
|---|---|
| Splitting the monolithic UI causes regressions | Extract behavior in small slices; preserve smoke fixtures and build after each slice |
| Provider schemas diverge | Normalize into shared models while retaining provider-specific metadata |
| Resume corrupts a profile | `.part` files, hash/size verification, staging, atomic rename, and restore points |
| Late async response overwrites current view | Request ids and target keys on every result |
| UI becomes visually inconsistent again | Component-only rule for new surfaces and visual baseline review |
| Runtime validation is unavailable on one machine | Layer cached dry-runs, clean-room fixtures, and targeted real launches |
| Secrets leak into diagnostics or fixtures | Redaction tests, credential-free fixtures, and security review before packaging |
| Scope expands indefinitely | M1-M6 are the completion path; M7 enhancements cannot block the core release gate |

Decisions to keep fixed:

- one launcher and one profile model;
- Modrinth and CurseForge remain provider adapters, not separate UIs;
- no custom Minecraft packets;
- no remote diagnostics upload by default;
- no hidden automation or destructive action without review and recovery;
- advanced controls stay behind an explicit advanced boundary.

## 10. Definition of done

V3 is complete when all of the following are true:

- the primary navigation is coherent and has no dead routes;
- Home, Library, Discover, Downloads, profile detail, Settings, Bedrock, AI,
  and Servers use the shared visual/state/operation system;
- a real provider project can be inspected, planned, installed, updated,
  repaired, restored, and launched from the UI;
- operations survive errors and restart with visible recovery;
- launch failures are classified with actionable next steps;
- the launcher is usable at supported window sizes and DPI scales with keyboard
  navigation and intentional text wrapping;
- the UI contains no mock/demo/placeholder copy in normal mode;
- native, Java bridge, package, and UI smoke gates pass locally; clean-room runtime and entitled-account gates are completed separately;
- docs, migration behavior, support diagnostics, and release artifacts are
  complete.

The previous immediate build slice—**M1 / UI-001 through UI-005**—is now landed:
shared visual/state primitives, operation/notification handling, page wiring,
documentation, and fixture verification are complete. The next sensible slice is
optional maintainability work: split `ui.cpp`, deepen operation-manager
contracts, and expand failure-injection/clean-room coverage.
