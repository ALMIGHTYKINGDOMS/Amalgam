# V3 UI Audit Status

**Reviewed:** 2026-08-20  
**Status:** The planned page integration is complete; this document replaces the
older component-only checklist.

## Closed in the V3 integration pass

### Shared UI foundation

- Shared buttons, cards, progress bars, empty states, badges, tooltips, loading
  indicators, toasts, settings filtering, and responsive layout helpers live in
  `cpp/launcher/src/ui_components.cpp`.
- Breadcrumbs are rendered on the major pages and their parent links now route
  to the matching Home, Discover, Library, Downloads, Servers, Essentials, or
  Settings surface.
- The generic context-menu helper is safe for normal ImGui windows. Library
  profile actions use the richer page-specific context menu, which supports
  favorites, groups, recovery, and confirmation flows.

### Page integration

- Home uses real profile data, readiness information, recent activity, profile
  artwork/fallbacks, create/import actions, and responsive cards.
- Library uses real profiles, search/sort/group/favorite state, safe recovery
  actions, worlds, screenshots, profile detail, and existing context menus.
- Discover uses live Modrinth/CurseForge data, provider-aware project detail,
  compatibility and dependency review, install handoff, and empty/error states.
- Downloads uses durable job history, protected queue state, retry/pause/cancel
  controls, timeline details, transfer information, and the enhanced operation
  progress component in both the full page and compact panel.
- Settings uses a searchable category rail/selector, valid section fallback,
  actionable readiness/account/runtime/provider controls, and a clear no-match
  state. Microsoft client-ID setup routes to the actual Launcher settings area.

### Quick Search and feedback

- `Ctrl+K` searches local profiles, loaded modpacks/content, and saved/local
  servers without starting network work from the render loop.
- Search matching is case-insensitive and bounded. Click, Enter, and arrow-key
  selection all return a result to the navigation handler; recent searches are
  retained with a small cap.
- Indeterminate transfers do not display a misleading negative percentage.
  Progress values are normalized before rendering, toast windows use unique IDs,
  and retained toasts are bounded.

## Remaining code-completable engineering work

These are follow-ups, not hidden page-integration tasks:

1. Split the large `cpp/launcher/src/ui.cpp` controller/render file into feature
   translation units while preserving the existing state and fixture contracts.
2. Replace the current durable job helpers with a centralized operation manager
   that can expose cross-job dependencies, aggregate rate/ETA, and richer
   byte-range resume behavior.
3. Expand ownership migration and full archive-level update diffs for older
   manually copied content and provider manifests.
4. Add more ImGui-free state contracts and deterministic failure-injection flows
   for launch failures, restart recovery, and provider rate limits.
5. Complete clean-room runtime validation for every supported loader/bridge and
   improve Bedrock coverage on a machine with the Windows UWP package.

## External public-release gates

The source tree cannot complete these locally:

- an entitled Microsoft/Minecraft account performing a real Java launch;
- clean-machine Fabric, Quilt, Forge, and NeoForge install/launch/update/
  rollback/uninstall evidence;
- live Bedrock UWP detection, launch, and addon import;
- code-signing certificate, SmartScreen/reputation, installer upgrade, and
  uninstall verification;
- native module compatibility/policy review and provider/legal/support approval.

These remain explicitly documented in `docs/release-gates.md`; no fixture or dry
run is presented as a substitute for them.

## Verification target

The configured native project currently registers 30 CTest checks: 25 focused
native/launcher suites, two launcher smoke checks, and three Java bridge protocol
checks. Every UI or state change should keep the complete suite green and should
be reviewed through the deterministic fixture captures where the affected page
is visible.
