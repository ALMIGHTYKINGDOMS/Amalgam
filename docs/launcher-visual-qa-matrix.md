# Launcher visual-QA matrix

The comprehensive fixture manifest is [tools/launcher-visual-qa-cases.full.json](../tools/launcher-visual-qa-cases.full.json). It is the release-review inventory for the launcher UI, rather than a short gallery of representative pages.

## What it covers

The manifest is the authoritative inventory. Its `registryFixtureTokenCount`,
`cases`, `compactHighRiskCaseIds`, and `smallHighRiskCaseIds` values are read
by the capture runner at execution time; do not carry a copied route or capture
count into release evidence. `case-selection.json` records the exact dynamic
selection for the binary and manifest that were actually captured.

| Area | Examples of the covered states |
| --- | --- |
| Home, Discover, and Library | Every Discover category and each Library section |
| Profile and Project detail | Content empty/ready/filtered/loading/error, every profile and project tab |
| Core launcher, downloads, screenshots, and top-bar overlays | Job filters and empty state, screenshot layouts, alerts, quick search, account dropdown |
| Settings and account portal | Every settings section plus local-fixture account overview, profile, security, sessions, activity, and safety confirmations |
| Servers and Bedrock | Server list/create/cloud and every detail tab; installed/not-installed, profile, world, backup, add-on, and every create/edit-profile Bedrock step |
| Essentials, Mod Manager, Performance, Theme, and Admin | Every registered tab plus session dialogs and admin-gated state |
| Java, backups, logs, configuration, profile wizard, and dialogs | Every log source, every wizard source/step/resolve outcome, recovery/auth/feedback dialogs |

The route count is intentionally taken from is_visual_fixture_case in the launcher source. A manifest review should fail if the source registry changes without updating the manifest count and entries.

## Responsive and scroll evidence

| Review tier | Viewport | Deterministic selection | What it is for |
| --- | --- | --- | --- |
| Primary desktop | 1600×900 | Every entry in `cases` | Every registered surface/state, including selected middle and bottom checkpoints |
| Compact desktop | 1366×768 | `compactHighRiskCaseIds` | Navigation density, labels, layout wrapping, tab rows, side panels, and common dialogs |
| Small desktop stress test | 960×600 | `smallHighRiskCaseIds` | Dense settings, long content, server controls, Bedrock, social, managers, wizards, and dialogs |
| Explicit focused re-check | Caller-supplied viewport(s) | `-CaseId` / `-CaseGroup` with `-ViewportCasePlan All` | Regression confirmation for a changed surface without silently applying a high-risk subset |

`-ViewportCasePlan ManifestResponsive` implements the first three rows. The
runner validates every high-risk-list ID, intersects any explicit case/group
filters, and writes the resulting per-viewport IDs to `case-selection.json`.
For an unfiltered three-viewport matrix, the expected capture count is
calculated from the manifest at run time as `cases + compactHighRiskCaseIds +
smallHighRiskCaseIds`, not copied from this document.

Every base route is captured at the top position. The additional @middle and @bottom route forms select a deterministic position in the launcher shell's page-level scroll host. They are deliberately focused on long, dense, or high-risk areas such as Discover, profile content, project detail, downloads, settings, server detail, Bedrock, Essentials, management pages, and logs.

### Current evidence-host constraint

The workstation used for the September 22 evidence pass has a physical **1280×720** desktop. It cannot truthfully produce 1600×900 or 1366×768 client images: Windows safely clamps those requests. For this host, use the largest valid request, **1256×624** (currently yielding a **1242×610** client image), plus the small-screen request **960×600** (currently yielding **946×586**). The capture ledger records both the requested and actual dimensions and accepts only normal window-chrome deltas.

The harness now stops a broad viewport suite as soon as its first capture loses
more than the material-clamp threshold (96 pixels by default) and marks its
remaining planned captures `blocked`. That intentional failure is preferable
to hundreds of screenshots mislabeled as 1600×900 or 1366×768. Use the
host-valid 1256×624 request for local coverage; it is deliberately recorded as
an unmapped, all-selected-cases host viewport rather than presented as a matrix
tier.

Those host-valid runs are valuable coverage, but they do **not** replace a genuine 1600×900 or 1366×768 review on a suitably sized host. Do not rename or present clamped images as those larger viewports.

## Capture checklist

1. Build the intended launcher binary before capturing. Do not use a stale binary just because it already produced images.

2. Run the responsive matrix on a host that supports every target viewport. The runner creates a fresh timestamped evidence directory, refuses to overwrite it, validates the high-risk lists, and saves the actual selection and hashes.

       .\tools\capture-launcher-visual-qa.ps1 -LauncherPath ".\cpp\build-release\amalgam_launcher.exe" -OutputDir ".\artifacts\visual-qa-2026-09-22\launcher" -ManifestPath ".\tools\launcher-visual-qa-cases.full.json" -ViewportCasePlan ManifestResponsive -Viewport 1600x900,1366x768,960x600

3. On the current 1280×720 evidence host, do not request a fabricated wide viewport. Run the host-valid primary substitute plus the manifest small-screen high-risk set. The ledger will identify 1256×624 as an unmapped all-cases viewport and retain its actual PNG IHDR dimensions.

       .\tools\capture-launcher-visual-qa.ps1 -LauncherPath ".\cpp\build-release\amalgam_launcher.exe" -OutputDir ".\artifacts\visual-qa-2026-09-22\launcher" -ManifestPath ".\tools\launcher-visual-qa-cases.full.json" -ViewportCasePlan ManifestResponsive -Viewport 1256x624,960x600

4. Treat `visual-capture-ledger.json`, `visual-capture-ledger.md`, and `case-selection.json` as required evidence. Every planned row must have status `passed`, a zero exit code, non-zero PNG length, a valid PNG IHDR, expected dimensions under the configured policy, a SHA-256 digest, and a successful configuration/isolation outcome. A `blocked` row means the matrix was not completed.

5. Review images by surface and by state, not only as a file-count exercise. Check clipped or overlapping text, hierarchy of primary and destructive actions, panel balance, tab/pill wrapping, empty/loading/error explanations, dialog fit, contrast, and the top/middle/bottom transition on selected long pages.

6. Run separate live, interactive smoke tests after fixture review. Exercise pointer and keyboard interaction, focus order, real account sign-in, Microsoft hand-off, provider data, live server operations, real Bedrock installation, and actual file mutation only with safe test accounts, profiles, and servers.

## Fixture labels and limits

Every image produced through --ui-snapshot must be labelled **deterministic local fixture** in a release review. It is strong visual evidence, but it is not live end-to-end evidence.

- Fixture mode starts in safe mode and avoids provider/update/session calls. It writes fixture files beside the evidence output instead of into a reviewer-owned profile. The harness also gives each snapshot a fresh child `LOCALAPPDATA`, `APPDATA`, `TEMP`, and `TMP` root under the evidence run, then records that application/restoration outcome.
- Home is intentionally signed out. The account sub-tabs use a clearly captioned local sample-data facade, so no signed-in profile, entitlement, session, or activity data is read.
- Servers are seeded sample records; a running-looking fixture server is not a supervised process. Cloud-hosting visuals are likewise not a cloud API verification.
- Bedrock fixture pages force availability except for the explicit bedrock-not-installed route. They validate composition, not a real Microsoft Store/UWP installation or an add-on operation.
- Admin content uses fixture authorization. It proves layout and state presentation, not real authorization or role enforcement.
- @middle and @bottom move only the shell's outer page-level scroll host. They do not prove the scroll behavior of nested consoles, tables, image grids, or virtualized lists.
- PNG snapshots do not prove hover, keyboard navigation, focus semantics, screen-reader/UI Automation exposure, network error recovery, or mutation workflows. Those require an interactive accessibility and real-user pass.

This distinction is deliberate: visual completeness should be evidence-backed without claiming that synthetic screenshots certify production integrations.
