# ChronoLog Reading Gap

Current validated read workflow:

1. Append events through the ChronoLog client.
2. Wait for the Grapher to archive data into HDF5.
3. Read via `ReplayStory` through the ChronoPlayer/archive path.

This should remain in Phase 0 as the comparable read/range workflow because it is the currently validated ChronoLog read behavior.

The missing future workflow is live or tail reading: a consumer reads newly appended data directly from the active service path instead of waiting for archive playback. That would make Mofka's simultaneous producer/consumer benchmark mode a fair comparison target. Until ChronoLog exposes and validates that path, mixed append/read should be marked future work rather than used as a Phase 0 fairness claim.
