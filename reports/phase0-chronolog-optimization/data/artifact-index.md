# Artifact Index

These are the original local artifact paths from the completed Phase 0 run. They were generated on the experiment branch and are listed here so reviewers can map this report back to the raw evidence.

## Final reports

| Artifact | Path |
|---|---|
| Final report | `.agent/results/20260518-211500-phase0-final-report/summary.md` |
| Completion audit | `.agent/results/20260518-212000-completion-audit/analysis.md` |
| Latest manifest/report refresh | `.agent/results/20260518-210507-mofka-pmdk-per-event-64k-report-refresh/` |
| Post-Mofka coverage audit | `.agent/results/20260518-211000-phase0-post-mofka-coverage-audit/analysis.md` |
| Progress log | `.agent/results/progress.md` |
| Iteration log | `.agent/results/profileforge-iteration-log.csv` |
| Current state | `.agent/state/current.md` |

## Latest manifest refresh contents

| Artifact | Path |
|---|---|
| Manifest JSON | `.agent/results/20260518-210507-mofka-pmdk-per-event-64k-report-refresh/manifest/manifest.json` |
| Manifest summary | `.agent/results/20260518-210507-mofka-pmdk-per-event-64k-report-refresh/manifest/summary.md` |
| Evolution summary | `.agent/results/20260518-210507-mofka-pmdk-per-event-64k-report-refresh/evolution/summary.md` |
| Iteration evolution CSV | `.agent/results/20260518-210507-mofka-pmdk-per-event-64k-report-refresh/evolution/iteration-evolution.csv` |
| ChronoLog accepted evolution CSV | `.agent/results/20260518-210507-mofka-pmdk-per-event-64k-report-refresh/evolution/chronolog-accepted-evolution.csv` |
| Latest semantic matrix CSV | `.agent/results/20260518-210507-mofka-pmdk-per-event-64k-report-refresh/evolution/latest-semantic-matrix.csv` |

## Branches

- Experiment branch with full local history: `opt/phase0-profileforge`
- Shareable report branch: `report/phase0-chronolog-optimization`

## Notes for reviewers

The shareable report branch is intentionally smaller than the experiment branch. It is meant for engineering review and discussion. Raw logs, profiles, generated manifests, and intermediate run artifacts should be pulled from the experiment branch or the local result directories when deeper forensic review is needed.
