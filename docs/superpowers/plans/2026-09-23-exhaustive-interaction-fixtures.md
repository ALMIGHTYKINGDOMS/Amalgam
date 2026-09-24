# Exhaustive Launcher Interaction Fixtures — Execution Plan

> Execute in risk order. The user has already approved the risk-first stabilization and polishing continuation. Preserve unrelated changes, use local-only fixture data, and do not touch the official website or live Minecraft network.

**Design:** `docs/superpowers/specs/2026-09-23-exhaustive-interaction-fixtures-design.md`
**Baseline:** 221 fixture tokens, 296 manifest cases, 752 planned primary/compact/small captures. This baseline has registry parity but is not yet exhaustive interaction proof.

## Checkpoint 0 — coordination and evidence preservation

- [x] Preserve the interrupted 91-artifact broad capture as diagnostic evidence only.
- [x] Preserve the original 1×1 failure evidence and the schema-v3 retry proof; do not overwrite it.
- [x] Record the user’s website/network boundary and approved scope in the design.
- [ ] Avoid native builds while any source-editing worker is active.

## Checkpoint 1 — P1 fixture containment

**Files:** `cpp/launcher/src/server_ui.cpp`, `cpp/launcher/src/ui.cpp`, and any narrow declaration needed by fixture state.

- [ ] Disable and action-guard Server remove-entry fixture confirmation before `save_local_servers`.
- [ ] Disable and action-guard Server file-delete fixture confirmation before path lookup/removal.
- [ ] Disable and action-guard Server backup-restore fixture confirmation before service restore.
- [ ] Add uniform profile recovery fixture action guard before restore/delete/options/recovery actions.
- [ ] Reset all added transient fixture fields at the start of every fixture application.
- [ ] Inspect source diff to prove both visual and action-entry containment exist.

**Gate:** no fixture route can reach local persistence, filesystem deletion, server restore, shell launch, worker spawn, or network/service call when clicked.

## Checkpoint 2 — high-risk core-dialog inventory

**Files:** primarily `cpp/launcher/src/ui.cpp`, `cpp/launcher/src/ui_internal.h`, manifest after source routing is complete.

- [ ] Profile recovery: restore-latest, restore-point deletion, and game-option restore confirmation/error pairs.
- [ ] Project content install confirm/conflict presentation with static local project/dependencies and disabled commit action.
- [ ] Version/loader change and creator-update confirmation variants with action guards.
- [ ] Collection group create/rename/delete confirmation variants with persistence guards.
- [ ] Managed-Java install/remove and settings admin-password preview dialogs without a runtime manager, downloader, credential write, or config write.
- [ ] Add registry routes, deterministic staging, fixture disclosure, top case, and compact/small high-risk entries for each accepted route.

**Gate:** `--list-ui-fixtures` and manifest agree after a fresh build; focused compact captures show no clipped dialog body/footer.

## Checkpoint 3 — module-local dialogs and panels

**Files:** `mod_manager_ui.cpp`, `server_ui.cpp`, `admin_ui.cpp`, `essentials_ui.cpp`, plus narrow shared declarations and root routing only where needed.

- [ ] Mod Manager: static Move-to-Recovery lifecycle, details, and dependency graph presenters; no content scans, image downloads, or workers.
- [ ] Servers: restore/remove error states and static file-preview top/bottom state; no file read or Explorer launch.
- [ ] Administration: local confirmation presenter for acknowledgement, typed-target, and reason variants; no staff auth, ServerManager, or Supabase invocation.
- [ ] Essentials: local friend-profile presenter and stable lifecycle/error states; no FriendsManager, SessionManager, or Essentials request lane.
- [ ] Add all route tokens to the central registry and precise manifest cases only after local presenters are safe.

**Gate:** component routes visibly state their sample/local status and their destructive/external controls are disabled as well as action-guarded.

## Checkpoint 4 — active menus and remaining meaningful interaction states

- [ ] Reconcile the popup/combo/context/overflow inventory against fixtures and document intentionally excluded dormant/unreachable code.
- [ ] Add in-scope fixture presenters/open hooks for profile actions, Library move-to-group, server file context, Mod Manager installed-row menu, Essentials friend menu, performance cache clear, screenshot context, and other material expanded menus.
- [ ] Add missing separate page/tab/wizard/error/loading/recovery compositions that are active but not represented by a prior route.
- [ ] Make every menu opening stay within its real ImGui scope and provide a safe close path.
- [ ] Add only needed nested `@bottom` cases after component-local scrolling is real and settled.

**Gate:** every active meaningful surface has one normal composition and all stable error/recovery/completion states exposed by the product. No fake state is used to inflate coverage.

## Checkpoint 5 — build, tests, and focused visual refinement

- [ ] Check no `ninja`, `link`, `cl`, or `cmake` process is active.
- [ ] Rebuild `cpp/build-release` with the configured Visual Studio environment, then rerun it until Ninja reports no work.
- [ ] Run CTest in the two specified bounded halves (1–22 and 23–44) with output on failure.
- [ ] Run `amalgam_fixture_inventory_test` and focused module regression tests.
- [ ] Run Node manifest/runtime-agent/Supabase source checks without making live claims.
- [ ] Capture all new routes at 960×600 and 1256×624. Inspect every image, repair proven UX defects, and recapture affected surfaces.

## Checkpoint 6 — final expanded local evidence matrix

- [ ] Start a new named capture folder; never reuse partial, clamped, or diagnostic evidence as a pass.
- [ ] Run all accepted cases at 960×600 and 1256×624 under isolated fixture roots with hardened retries enabled.
- [ ] Confirm ledger schema, every attempt, dimensions, hashes, and image paths; failed/clamped attempts remain retained and failed.
- [ ] Generate all surface-grouped contact sheets and visually inspect every sheet.
- [ ] If a visual correction touches shared layout, rebuild, rerun relevant tests, recapture the changed routes, and rerun the full matrix before completion.

## Completion decision

The polishing/evidence phase is complete only when fixture containment is proven, the expanded active-surface inventory is represented honestly, current native tests and parity pass, two host-valid viewport tiers have a clean full immutable ledger, and every contact sheet has been inspected. The final report must state that the existing official website and live network were intentionally not changed or certified by this launcher-only work.
