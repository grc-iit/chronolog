# ChronoLog Performance Baselines

This branch holds per-release performance baselines for the manual regression
workflow defined in `test/performance/perf_regression_design.md` (on `main`).

Layout:

    <version>/ares/ofi+sockets/4groups_4x20clients/record_event.json

Each baseline JSON conforms to `schema.json` at the branch root.

Baselines are committed manually after each release run on Ares using
`test/performance/save_baseline.sh`. If you must amend an existing baseline,
prefer adding a corrected file alongside the original rather than rewriting
it so the history reflects what each release actually measured.
