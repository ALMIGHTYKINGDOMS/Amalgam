# Exhaustive Launcher Interaction Fixtures and Visual-Quality Design

**Date:** 2026-09-23
**Status:** Approved for implementation by the user's earlier explicit "Approach A — Risk-first stabilization, then polish" approval and their instruction to keep going.
**Scope:** The native Amalgam launcher only. Existing website and live network operations are explicitly excluded.

## Problem statement

The launcher has already received its risk-first stability and visible-layout repairs, and the current deterministic fixture inventory is internally synchronized: 221 registered fixture tokens correspond to 296 manifest cases. That is an important baseline, but it is not the same thing as the user's requested evidence for every active page, tab, wizard, dialog, recovery state, contextual menu, and meaningful overflow surface.

The current audit also found a more urgent issue: a small number of existing local fixture overlays reuse live destructive confirmation handlers. Their rendered buttons can be clicked and can reach a local server-list save or filesystem removal path. Fixture mode must be a hard safety boundary, not merely a convention observed by the capture runner.

## Goals

- Make every existing and newly introduced fixture-mode action fail closed at both the rendered-control and action-entry boundaries.
- Cover every meaningful, reachable launcher composition with deterministic, local-only fixture evidence. This includes normal/error/recovery variants where the product actually has a stable corresponding UI state.
- Preserve a truthful distinction between local presentation proof and live-provider, filesystem, server, account, or website verification.
- Improve any compact-layout, focus, affordance, or error-treatment defect that fresh evidence exposes before counting the visual matrix as complete.
- Produce an immutable, hash-verified two-tier visual ledger and inspect its contact sheets after source and fixture changes settle.

## Boundaries and non-goals

- Do not modify, rebuild, browse, automate, publish, or otherwise change [the official Amalgam site](https://amalgam-mc.com/) or the live Minecraft network (`play.amalgam-network.com`). The launcher may only retain its existing truthful handoff behavior.
- Do not call live providers, hydrate a real account, read user content, mutate user profiles, start services, launch Explorer, write a server registry, or spawn a worker from a visual fixture.
- Do not invent a route for a compiled but unreachable popup. In particular, `Switch Account` has no production opener and remains excluded unless a separate product decision restores a real reachable entry point first.
- Do not represent a fictional asynchronous or in-dialog error state when production only closes the dialog and emits a later notice. Improve the real behavior first if that evidence is needed.
- Preserve unrelated working-tree changes and do not reset, clean, broadly format, or delete artifacts.

## Safety architecture

Fixture state is owned by `UiState::fixture_mode` and the canonical registry/router in `cpp/launcher/src/ui.cpp`. Every route follows the same containment contract:

1. It is registered in `visual_fixture_cases()` and staged deterministically by `apply_visual_fixture_case`.
2. It uses representative in-memory presentation data only, marked visibly as a local visual fixture.
3. Its primary/mutating control is visibly disabled or rendered as an explicit `Preview only` control.
4. The corresponding mutation, filesystem, worker, shell, account, service, and provider entry point independently returns before doing work whenever fixture mode is active.
5. The route is added to the manifest only after the compiled fixture inventory accepts it, with both compact and small responsive high-risk coverage for modal/menu surfaces.

This two-boundary rule protects both automated capture and a human reviewer who clicks within a fixture window.

## Delivery sequence

### Gate 1 — repair existing fixture containment (P1)

- Server saved-entry removal: disable the fixture button and return before local-registry persistence.
- Server file deletion: disable the fixture button and return before path lookup or `remove`/`remove_all`.
- Server backup restore: retain an explicit local-only explanation, disable the action, and return before the restore service.
- Profile recovery confirmations: guard restore-latest, delete-restore-point, restore-options, and recovery-move execution branches as fixture-safe no-ops before registering their dialogs.
- Reset every newly staged transient field at the router boundary so reused fixture processes never inherit modal or form state.

### Gate 2 — highest-risk active dialogs and recovery states

Add deterministic, action-locked routes for:

| Area | Planned evidence |
|---|---|
| Profile recovery | restore-latest confirm/error; delete restore-point confirm/error; restore game options confirm/error |
| Content install | content-install confirm and conflict/replacement warning |
| Profile migration/update | version with backup; version without backup; loader change; creator update and creator-update-copy confirmation |
| Collections | create-and-assign group; rename group; delete group |
| Managed Java / Settings | install runtime; remove runtime confirmation; admin-password-change preview |

### Gate 3 — module-local dialogs and secondary panels

Add route-local, read-only fixture presenters for:

| Area | Planned evidence |
|---|---|
| Mod Manager | Move to Recovery confirm/working/error; details; dependency graph |
| Servers | backup restore error; saved-entry remove error; static file preview (top and fixture-owned bottom scroll) |
| Administration | delete-server confirmation; project-reject confirmation; delete-server error |
| Essentials | friend profile; where production exposes stable lifecycle UI, removal/block working and error variants |

### Gate 4 — reachable menu, overflow, context, combo, and tooltip evidence

After action containment is demonstrably correct, inventory and add only active/reachable surface forms, including:

- Profile action overflow menu.
- Library profile overflow and nested move-to-group menu.
- Server file context menu and static preview controls.
- Mod Manager installed-row overflow and relevant filter/sort combos.
- Essentials friend context menu.
- Performance cache-clear confirmation.
- Screenshot context menu.
- Existing visible account, search, profile/version, content, Java, server, Bedrock, admin, and settings menus whose expanded state changes the composition materially.

Each real menu is opened inside the same ImGui ID scope as its production trigger. Fixture menu entries are non-interactive/disabled and independently action-guarded. Tooltip coverage is limited to meaningful reachable tooltips with substantive product guidance; cosmetic hover labels are not misrepresented as distinct functional pages.

### Gate 5 — evidence, refinement, and certification

1. Rebuild the native Release launcher only after all concurrent source edits stop.
2. Run the complete 44-test native suite and focused fixture/content/profile/server/admin/Essentials regression targets.
3. Verify compiled registry ↔ manifest parity using `amalgam_fixture_inventory_test`.
4. Capture every new route first at the two physically host-valid requested viewport tiers (960×600 and 1256×624); fix actual layout defects, then recapture affected surfaces.
5. Run a fresh all-case × two-tier immutable capture, retaining all retry attempts and clamp evidence under the hardened schema-v3 ledger.
6. Generate and inspect every contact sheet. A failed, blocked, clamped, missing, or partial record is never promoted to visual completion.

## Implementation notes

- Page-owned popups use `request_popup` only inside the correct window/ID scope. Nested menus may need a fixture-only in-scope open hook rather than a global request.
- Global page `@top`, `@middle`, and `@bottom` capture checkpoints do not scroll a modal/child window. A nested visual state gets a bottom checkpoint only after it supports a settled-frame local scroll position.
- Error copy names the affected local sample and accurately says no fixture action changed it. Errors use the same visual severity treatment across profile recovery variants.
- Fixture disclosure, disabled state, escape/close affordance, focus order, keyboard behavior, contrast, compact footer visibility, and destructive hierarchy are review criteria, not merely screenshot metadata.

## Acceptance criteria

1. No visible fixture control can change a profile, file, server record, account, provider state, external service, or user environment, even if clicked.
2. Every registered fixture route is active/reachable in normal product behavior (or explicitly documented as a fixture-only error/recovery representation of an active flow); dormant unreachable code is excluded rather than staged artificially.
3. The manifest, binary registry, responsive high-risk lists, and capture ledger are exactly synchronized.
4. Each modal/menu has a clear local-fixture disclosure, compact responsive evidence, an accessible close route, and no clipped primary/footer at 960×600.
5. Fresh visual evidence contains no debug overlay, placeholder 1×1 image, material clamp relabeling, or uninspected contact-sheet failure.
6. Final reporting distinguishes completed launcher work from the deliberately untouched official site and live network, and names any remaining external prerequisite precisely.
