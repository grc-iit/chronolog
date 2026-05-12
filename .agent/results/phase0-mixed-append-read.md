# Phase 0 Mixed Append/Read Support

Status: unsupported for the selected comparable Phase 0 suite.

The provisional `mixed_append_read` workflow was defined as concurrent append plus read/range activity when all three systems expose compatible APIs. It is not selected for final Phase 0 comparison because the currently validated ChronoLog read path is archived story playback, not a live concurrent read path.

## Evidence

- ChronoLog append validation uses `StoryHandle.log_event`.
- ChronoLog read validation uses `Client.ReplayStory` through `ClientQueryService` and ChronoPlayer archive reading.
- The successful distributed ChronoLog range run waits for an HDF5 archive file before issuing `ReplayStory`: `.agent/scripts/chronolog_range_retrieval.py`.
- The archive evidence for the successful run is `.agent/results/20260512-015243/chronolog/output/phase0_range_chronicle_1778568791663.phase0_range_story_1778568791663.1778568780.vlen.h5`.

Kafka and Mofka can run producer/consumer style workloads, but that would not be comparable to ChronoLog unless ChronoLog exposes a supported live read/query path over in-flight or unarchived events.

## Decision

`mixed_append_read` is documented as unsupported for the selected Phase 0 comparable suite. It should be revisited after ChronoLog has a clearly supported live read/query workload or after the intended mixed workload semantics are redefined by the project owner.
