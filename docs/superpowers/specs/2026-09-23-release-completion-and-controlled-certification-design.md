# Release Completion and Controlled Certification Design

**Date:** 2026-09-23
**Status:** Approved continuation of the user's risk-first stabilization approach
**Scope:** The local Amalgam repository, its local build/package tooling, and disposable local test state only.

## Current factual baseline

The repository-resolvable launcher safety and visual-polish work is complete at the current source revision:

- The current Release launcher was rebuilt successfully.
- Native CTest passed in two bounded batches: 22/22 and 22/22 (44/44 total).
- The update-manifest Node suite passed 12/12.
- The immutable local visual matrix contains 792 captures, with 792 passed, zero failed, zero blocked, matching binary/manifest/fixture provenance, and no configuration mutation.
- The final review generated 257 contact sheets and found no missing, invalid, mismatched, or provenance-invalid image.

That establishes a strong local UI/UX and source-quality baseline. It does **not** establish that a distributed installer has been tested on a clean customer machine, that a real Minecraft entitlement can launch a game, or that production services and signing are available. Those are different verification levels and must remain distinct.

## Problem statement

The remaining work is no longer “make the launcher look better.” It is release-readiness work:

1. Turn the current source/build evidence into one traceable local release candidate.
2. Prove that candidate does not silently depend on the source tree, developer PATH, existing launcher configuration, or hidden assets.
3. Exercise all safe local recovery and dependency checks with disposable state.
4. Separate real external certification gates from software defects so the release decision is honest.

## Goals

- Preserve the user's existing working tree, credentials, launcher configuration, profiles, worlds, and server data.
- Produce a single evidence-backed, locally staged candidate whose package contents, hashes, and installed behavior are checked.
- Test the installed candidate using an isolated data root and a clean environment where safe.
- Re-run all local suites that can prove source, package, and installed-copy behavior.
- Create one consolidated release ledger with exact PASS, BLOCKED, or NOT TESTED status for each major system.
- Repair any repository-resolvable P0/P1 or high-value P2 defect discovered during that process, then rebuild, repackage, and recertify the affected path.

## Explicit boundaries

- Do not browse, edit, publish to, or test the official Amalgam website.
- Do not connect to, probe, administer, or otherwise touch the live Amalgam Minecraft network.
- Do not use or print production credentials, private signing keys, account tokens, or personal configuration.
- Do not sign in to a real account, launch a real game, import to a real Bedrock world, or use a real production backend without a later controlled-test authorization.
- Do not remove, reset, clean, or overwrite user-owned worktree changes or runtime state.
- Do not call a source-only or fixture-only result “live tested.”

## Selected approach — risk-first local certification

The execution order deliberately prevents cosmetic or external work from hiding a package, safety, or dependency defect:

1. **Provenance and defect ledger.** Reconcile the final binary, evidence ledger, fixture inventory, source tree, package scripts, and existing artifact paths. Old packages are treated as stale if they do not contain the current binary.
2. **Operational dependency audit.** Trace the launcher, Java/bridge, Bedrock package, local server, content-provider, Essentials, Cloud, updater, and installer requirements. Classify each as self-contained, auto-resolving, externally configured, or intentionally gated.
3. **Fresh local candidate.** Build from current source; run current native, Node, Bedrock, Java/bridge, Supabase-source, and tooling checks that are available without private credentials.
4. **Package and install proof.** Stage a candidate from the fresh binaries, inspect its inventory and hashes, scan it for secrets/debug leftovers, then exercise it through a disposable installed-copy state.
5. **Controlled journey proof.** Test first run, restart, configuration isolation, diagnostics, safe Java preflight/dry-run, Bedrock absent/validator behavior, local fixture UI, and non-destructive server/provider paths.
6. **External certification packet.** List only the smallest set of remaining tests that genuinely need a disposable account, signing authority, clean Windows environment, installed Minecraft/Bedrock, configured backend, or a second real participant.

## Requirement matrix

| System | Local evidence already available | Work that starts now | External gate, if any |
|---|---|---|---|
| Launcher core/UI | Current Release build, 44 native tests, 792/792 fixture evidence | Reconfirm binary/package provenance and installed-copy startup | None for local certification |
| Installer/package | Historical staged installer checks | Produce a candidate only from current binary, inspect inventory/hash, run isolated install lifecycle | Publisher code signing is separate |
| Java/runtime/bridges | Native route/preflight coverage and bridge contracts | Recheck packaged bridge inventory and dry-run/preflight without developer PATH | Real Minecraft login/entitlement for actual game launch |
| Bedrock add-on | Current package validator and local UI evidence | Validate final add-on contents/version/secret hygiene, verify honest absent state | Minecraft for Windows plus disposable world for import/runtime |
| Local servers | Safe transport/provisioning coverage and fixture matrix | Recheck disposable local transport/startup and error recovery | Real provider acquisition/start proof where a controlled server runtime is permitted |
| Modrinth/CurseForge | Local contracts/tooling | Verify client contains no private credentials and error states are honest | Live provider credentials/approval, especially CurseForge |
| Supabase/auth/Essentials | Source verifier and local UI fixtures | Audit package/client configuration shape and source contracts | Disposable backend/test account, deployed functions, second user/device for real social/P2P |
| Cloud | Fixture UI truthfully gated | Verify package preserves the gate | Actual control plane, node, entitlement, and billing authority |
| Updater | Manifest/integrity test suite | Verify staged candidate fails closed without production signing/feed | Signing identity, updater key authority, hosted endpoint, upload authority |
| Website/live network | Explicitly out of scope | No action | Owner-controlled production assets remain untouched |

## State isolation model

Every certification action must use one of these modes:

| Mode | Permitted state | Prohibited state |
|---|---|---|
| Fixture visual QA | Per-capture temporary artifact root | Real launcher configuration, accounts, providers, files, games, Bedrock, servers |
| Source/build QA | Repository build directories and test sandboxes | Real user configuration and credentials |
| Package/installer QA | Newly created disposable staging/install/data directories | Existing installation, profiles, worlds, server folders, secrets |
| Controlled integration QA | Explicitly approved disposable account/device/environment | Production website and live Amalgam Network |

## Completion criteria for this pass

This pass is complete when:

1. The current source, binary, visual evidence, and candidate package have a traceable provenance chain.
2. Every locally testable release check has a current result, and any discovered repository-resolvable P0/P1 is fixed and retested.
3. The final installed-copy proof uses a disposable state and does not consume real launcher data.
4. Package content, hashes, and secret/debug scans are clean.
5. The final report distinguishes source reviewed, build verified, automated tested, visually tested, installed-copy tested, live tested, and externally blocked.
6. The only open items are concrete external prerequisites, not unidentified local work.

## Decision standard

The result may be:

- **Locally release-ready for controlled private-beta packaging:** all repository-resolvable safety, UI, package, and installed-copy checks pass; production-only capabilities are truthfully gated.
- **Private-beta NO-GO:** a current package/install/source defect, secret leak, destructive behavior, or repository-fixable P0/P1 remains.
- **Production certification blocked:** local work is complete but actual sign-in, Minecraft/Bedrock runtime, deployed backend, signing, hosted updater, or clean-machine proof cannot be performed without the named external resource.
