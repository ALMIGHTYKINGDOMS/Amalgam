# Launcher visual-capture runner

`tools/capture-launcher-visual-qa.ps1` captures the launcher's deterministic
`--ui-snapshot` routes and records evidence that can be audited later. It is a
capture harness, not a substitute for interaction testing: the case manifest
should explicitly cover every supported page, tab, wizard/dialog state,
empty/loading/error state, and each scroll position being claimed.

The runner never removes evidence or writes directly into a previous run. It
creates a new uniquely named directory below the supplied output root, copies
the manifest used for that run, writes a deterministic `case-selection.json`,
and writes both `visual-capture-ledger.json` and
`visual-capture-ledger.md`. A capture passes only when the launcher exits zero
and the requested file exists, is non-empty, has a valid PNG/IHDR header, has
dimensions within the declared policy, and has a SHA-256 digest recorded.
Every snapshot child receives a fresh `LOCALAPPDATA`, `APPDATA`, `TEMP`, and
`TMP` tree below that evidence run; the runner restores its own process
environment after each child exits. It also fingerprints the launcher binary,
source manifest, copied manifest, selection artifact, image, and stdout/stderr
files so a later review can tell exactly what was captured.
The default `ClientArea` policy reflects the launcher's current behavior: it
requests an outer window size but captures its client area, so each image may
be smaller by at most 64 pixels in either dimension. Use `-DimensionPolicy
Exact` only for a launcher build that produces a fixed-size bitmap.

For a **broad** viewport plan (more than one selected case at that viewport),
the runner treats a loss greater than `-MaterialViewportClampPixels` (96 by
default) as a host clamp even if someone has raised the ordinary chrome-delta
tolerance. It keeps the first failed probe, marks all remaining cases at that
viewport `blocked`, writes the ledger, and exits unsuccessfully. This prevents
a 1600×900 suite from quietly turning into a collection of 1242×610 images on
a smaller evidence host. A single targeted capture still follows its declared
dimension policy normally.

## Case manifest

The example at `tools/launcher-visual-qa-cases.example.json` uses this minimal
schema:

```json
{
  "schemaVersion": 1,
  "suite": "Human-readable suite name",
  "cases": [
    {
      "id": "unique-safe-file-name",
      "route": "launcher-snapshot-route",
      "surface": "Optional review group",
      "state": "Optional named state",
      "annotation": "What visual state is under review",
      "scroll": "top | middle | bottom | modal | wizard-step-N"
    }
  ]
}
```

`id` is required, must be unique (case-insensitively), and is deliberately
restricted to filename-safe letters, digits, `.`, `_`, and `-`. `route` is
required. When `scroll` is `top`, `middle`, or `bottom`, the runner passes that
position to the deterministic fixture route and records it in the ledger.
Values such as `modal` and `wizard-step-N` remain reviewer annotations. The
runner labels omitted annotation/scroll fields conservatively. `surface` and
`state` are evidence metadata; neither is passed to the launcher.

The comprehensive manifest also declares its responsive plan: a primary
viewport with all cases and compact/small viewports with named high-risk case
lists. The runner validates every listed case ID against the manifest before it
starts a launcher process, so a stale high-risk list cannot silently shrink a
review.

## Run it

From the repository root, use an absolute launcher path and a fresh evidence
root. The following records three sample routes at both an ordinary desktop
viewport and a compact viewport:

```powershell
& .\tools\capture-launcher-visual-qa.ps1 `
  -LauncherPath "C:\absolute\path\to\amalgam_launcher.exe" `
  -OutputDir "C:\absolute\path\to\artifacts\visual-qa" `
  -ManifestPath ".\tools\launcher-visual-qa-cases.example.json" `
  -Viewport @("1600x900", "960x600")
```

Use `-ViewportCasePlan ManifestResponsive` with the comprehensive manifest to
apply its all-cases primary tier and its per-viewport high-risk tiers in one
deterministic run. The exact case IDs for every selected viewport are saved in
`case-selection.json` rather than inferred later from a command line:

```powershell
& .\tools\capture-launcher-visual-qa.ps1 `
  -LauncherPath "C:\absolute\path\to\amalgam_launcher.exe" `
  -OutputDir "C:\absolute\path\to\artifacts\visual-qa" `
  -ManifestPath ".\tools\launcher-visual-qa-cases.full.json" `
  -ViewportCasePlan ManifestResponsive `
  -Viewport @("1600x900", "1366x768", "960x600")
```

For a focused re-check, `-CaseId` selects exact case IDs and `-CaseGroup`
selects exact manifest `surface` values, case-insensitively. When both are
provided they are intersected. Use `-ViewportCasePlan All` for a focused case
at every supplied viewport, even if it is not in a matrix high-risk list:

```powershell
& .\tools\capture-launcher-visual-qa.ps1 `
  -LauncherPath "C:\absolute\path\to\amalgam_launcher.exe" `
  -OutputDir "C:\absolute\path\to\artifacts\visual-qa" `
  -ManifestPath ".\tools\launcher-visual-qa-cases.full.json" `
  -CaseGroup "Account portal / Overview" `
  -ViewportCasePlan All `
  -Viewport 960x600
```

The launcher receives one correctly quoted argument string for every snapshot,
so output paths containing spaces remain a single path argument. The runner
keeps the per-capture standard-output and standard-error files beside the PNG
and makes a non-zero PowerShell failure after it has written the ledgers if any
case fails or is blocked. The Markdown ledger includes actual PNG IHDR sizes,
exact commands, exit outcomes, image/log hashes, and isolation results; the
JSON ledger keeps the same data structurally. Review the PNGs visually;
integrity checks alone do not certify layout or interaction quality.

## Build readable review batches

`tools/review-launcher-visual-qa.ps1` is a non-destructive companion for a
completed capture run. It reads `visual-capture-ledger.json`, independently
checks that every referenced image is present, decodable as a PNG, has the
recorded dimensions, and matches the ledger SHA-256 when one was recorded. It
also checks the recorded launcher-binary hash against the currently available
binary, plus the preserved manifest-copy and case-selection hashes inside the
evidence run. A binary rebuilt after capture is reported as a provenance
mismatch; it does not rewrite or invalidate the preserved capture files. The
review index summarizes the recorded fixture/configuration-isolation outcome.
It then creates a **new sibling review directory**; it never replaces a capture,
ledger, or log in the source run.

The review directory contains:

- `contact-sheets/` — high-resolution sheets grouped by the manifest's
  `surface` value (or a conservative route-prefix fallback for legacy runs);
- `visual-review-index.md` — a compact audit index with links to every source
  PNG and batch;
- `visual-review-index.html` — a gallery of the generated contact sheets; and
- `visual-review-ledger.json` — per-capture integrity findings and the sheet
  that contains each capture.

Run it from the repository root after the capture runner completes:

```powershell
& .\tools\review-launcher-visual-qa.ps1 `
  -RunDirectory "C:\absolute\path\to\launcher-visual-qa-20260923T..."
```

The default two-column, three-row layout keeps each source view at 600 pixels
wide before it is placed on a sheet. Tune the batch size without changing the
source evidence, for example:

```powershell
& .\tools\review-launcher-visual-qa.ps1 `
  -LedgerPath "C:\absolute\path\to\visual-capture-ledger.json" `
  -OutputRoot "C:\absolute\path\to\visual-review-artifacts" `
  -GroupBy Surface `
  -ThumbnailWidth 720 `
  -Columns 2 `
  -RowsPerSheet 2
```

Use the contact sheets to spot clipping, density, alignment, unexpected blank
states, and responsive outliers quickly. For any candidate issue, open the
source PNG linked from `visual-review-index.md`; contact sheets do not replace
text-level visual inspection or interaction testing.
