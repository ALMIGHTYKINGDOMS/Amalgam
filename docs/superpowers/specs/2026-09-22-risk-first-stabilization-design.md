# Risk-First Stabilization and Product Polish Design

**Date:** 2026-09-22
**Status:** Approved for implementation by the user
**Scope:** The existing Amalgam working tree. Preserve unrelated, pre-existing work.

## Problem statement

The September audit established that the current native build and automated suite are healthy, but four repository-resolvable P1 defects can make the launcher unreliable or give a release gate false assurance:

1. Forge and NeoForge JVM argument-file paths are not safely quoted when a server directory contains spaces.
2. Server-name validation accepts Windows device aliases and trailing-dot/space aliases, while the UI uses a throwing directory-creation API on the render thread.
3. Runtime provisioning uses throwing directory creation despite an error-return API contract.
4. Provider artifact metadata can include a digest and exact size, but the download wrapper discards the SHA-1 and size values.
5. The release secret scanner accepts one positional root even though its release gate gives it three.

There are also verified security, truthfulness, UX, and release-certification gaps. They must be addressed only after the failure-prone launcher paths above have deterministic behavior and regression coverage.

## Goals

- Prevent ordinary Windows paths, invalid server names, permissions failures, and artifact corruption from crashing or silently breaking a local-server workflow.
- Turn published provider integrity metadata into one consistent download contract without falsely claiming verification when a provider supplies no digest.
- Make the release gate scan every requested source root and prove that behavior with an automated test.
- Harden Supabase policy/function verification using least privilege and safe function search paths, without changing live state until migration review and local/linked verification are available.
- Refine visible empty/error/cancellation states so they accurately explain what the product can and cannot do.
- Re-run proportional native, Node, Bedrock, Java, release-package, visual, and live checks; document external prerequisites instead of implying a release certification that has not happened.

## Non-goals and boundaries

- Do not embed, print, or invent Microsoft, Supabase, CurseForge, signing, or provider credentials.
- Do not change packet behavior, bridge wire formats, supported-version policy, or gameplay functionality as part of stabilization.
- Do not modify the deployed React-style marketing site without its actual source/deployment project. The tracked static website can only be made truthful for the artifact it represents.
- Do not remove untracked QA assets or user-owned changes until their provenance is classified. Cleanup must be recoverable and narrowly targeted.
- Do not declare live Supabase, Microsoft/Xbox, CurseForge, signing, clean-machine, or authenticated gameplay verification passed when their external prerequisites are unavailable.

## Architecture and delivery order

### Batch A — P1 launcher reliability and integrity

Create small, testable helpers at existing ownership boundaries rather than adding broad abstraction layers.

- Centralize Windows-safe server-name validation near ServerConfig/ServerRecord types. A valid name must be non-empty after trimming, free of invalid path/control characters, not a dot alias, not end with a dot or space, and not be a case-insensitive Windows device name (including an extension such as NUL.txt). Human-readable error text belongs at the UI boundary.
- Replace throwing directory creation in server creation and provisioning with std::error_code overloads. The UI reports an actionable error; provisioning returns false with its documented error string. No filesystem exception may cross a render or worker boundary.
- Treat JVM argfiles as a single Windows command-line argument. The command must pass the at-prefixed path to Java without splitting an embedded space. Reuse the project’s established Windows quoting helper if its behavior supports an at-prefixed argument; otherwise add one narrowly scoped command-argument helper with unit coverage.
- Change the provisioning download wrapper to accept the complete RuntimeArtifact. Pass SHA-1, SHA-256, and expected byte count to the existing network download contract. Preserve an honest “no provider checksum” path rather than manufacturing one.
- Make downloader cancel semantics explicit: transfer cancellation remains cooperative, while a running external installer is shown as non-cancellable unless the process implementation can actually stop it safely.

### Batch B — release/security gate correctness

- Make secret-scan process every supplied root, normalize/deduplicate roots safely, aggregate findings, and retain a sensible default when no root is supplied. Add an isolated Node test proving a clean first root cannot hide a fixture finding in a later root.
- Audit the effective final Supabase migration state rather than merely searching individual migrations. Produce a deterministic repository verifier for exposed table grants/RLS and SECURITY DEFINER function ownership, search_path, and executable roles. Any migration must be additive, reversible in concept, follow the project migration naming convention, revoke PUBLIC execution where appropriate, and explicitly grant only intended roles.
- Add policy/function tests if a local or linked Supabase database route is available. If it is not, leave a precise blocked gate with the exact credentials/tooling required; source verification alone remains labeled as source verification.

### Batch C — product truth and visual polish

- Replace the profile Content route’s generic empty result with distinct no-content and filtered-empty states. Give the search field an in-field prompt or clear nearby ownership label, and offer only actions that are available from that profile context.
- Update README test counts and static-site capability claims only from current, verifiable artifact/catalog data. Avoid unsupported size, AI, privacy, or provider claims.
- Record concrete remediation requirements for the live marketing site: remove/reduce splash gating, preserve mobile zoom, correct contrast and heading order, reserve hero layout space, and repair mobile hero density. Implement only when the actual source is in scope.

### Batch D — verification and release decision

- Rebuild the native release configuration from the edited sources and run targeted tests before the full CTest suite.
- Run all Node tool tests, explicit multi-root secret scan, Bedrock validation, and Supabase source verification. Build relevant Java bridge projects when the installed toolchain is available.
- Exercise the safe server-creation error path and a loader launch dry-run with a server path containing spaces. Run visual snapshots at 960x600 plus desktop/wide sizes where the display permits.
- Run package/installer checks only after the edited build is green, and produce a fact-based PASS / BLOCKED / NOT TESTED release decision.

## Error and user-experience rules

| Situation | Required product behavior |
|---|---|
| Invalid or reserved server name | Reject before writing files; explain the specific naming rule. |
| Directory cannot be created | Remain running; present the path operation failure and a remediation hint. |
| Loader argument-file path contains spaces | Start command carries each argfile as one argument and reaches Java’s at-file parser intact. |
| Digest/size supplied by provider | Download verifies every supplied value before accepting the artifact. |
| Provider supplies no digest | Download may proceed through transport trust, but diagnostics never label it checksum-verified. |
| User clicks cancel during download | Transfer cancels cooperatively and reports a cancelled terminal state. |
| External installer is running | UI states that the installation step cannot be cancelled safely; do not report a cancelled job if installation completes. |
| Profile has no content | Explain it has no installed content and show appropriate acquisition/import choices. |
| Filters produce zero results | Preserve filters and say that no installed content matches them. |

## Test strategy

Tests lead the behavior change whenever the code boundary is pure or fixture-driven:

- Native tests cover valid spaced names, all forbidden naming categories, non-throwing provisioning directory failure, loader command/argfile quoting, metadata propagation into a download request, and truthful job phases.
- The secret-scanner test invokes the command against two temporary roots, placing a benign detection fixture only in the second root; it must fail only when both roots are passed.
- Existing full CTest, Node, Bedrock, and Supabase source suites act as regression gates. A fresh build is required, rather than accepting an already-current Ninja graph.
- Visual verification compares no-content versus filtered-empty routes at minimum and desktop dimensions. Runtime/server checks must not require a private credential unless the user supplies one.

## Acceptance criteria

1. Every P1 finding above has an automated regression test and a passing product-level verification step.
2. The launcher converts all expected filesystem/name/path failure modes in this scope into user-facing errors rather than uncaught exceptions or broken child-process arguments.
3. Published provider integrity metadata is preserved through the download call and verified by the network layer.
4. The release secret scan cannot silently ignore any root passed by its caller.
5. Supabase hardening has a deterministic evidence path; any unavailable live DB route remains explicitly blocked rather than assumed safe.
6. The polished UI distinguishes empty inventory, filtered results, unavailable configuration, cancellation, and installer execution accurately.
7. Final reporting separates completed repository work, fresh automated/visual evidence, and external blockers.

## Working-tree policy

The workspace began with a large uncommitted September change set. This work will edit only the listed stabilization, test, release-tooling, documentation, and narrowly related UI files. It will not reset, discard, or mass-format existing user work. A standalone commit is deferred unless the current working tree can be safely partitioned without absorbing unrelated changes.
