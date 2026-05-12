# TAU Visualization Notes

Good TAU images usually come from:

- ParaProf views for call-path/profile exploration.
- `pprof` text summaries converted into static charts for repository reports.
- Jumpshot timelines from TAU trace files when `TAU_TRACE` is enabled.
- TAU JSON exports for custom dashboards.

For this Phase 0 package, the validated distributed run is TAU profile mode with ChronoLog semantic user events. The static figure `figures/tau_semantic_events.png` shows the labels currently emitted by the instrumentation abstraction.

Instrumentation quality check:

- Present labels: `client_append`, `rpc_send`, `metadata_lookup`, `story_index_update`, `append_bytes`.
- Still needed for deeper loop guidance: service-side semantic counters in Keeper/Grapher/Player hot paths such as `keeper_ingest`, `keeper_buffer_insert`, `keeper_flush`, `storage_write`, `storage_read`, `grapher_query`, and `range_retrieval`.

The current labels are enough to validate the TAU pipeline. They are not yet extensive enough to explain every ChronoLog bottleneck without lower-level profilers.
