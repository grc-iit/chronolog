# ChronoLog Coarse Semantic Regions

Checkpoint: add coarse semantic regions
Status: complete
Time: 2026-05-11 22:45 CT

## Regions Added

The code now uses `CL_PROFILE_REGION` and `CL_PROFILE_COUNTER` at coarse call-site boundaries for:

```text
client_append
client_query
rpc_send
rpc_receive
serialization
deserialization
keeper_ingest
keeper_buffer_insert
keeper_flush
storage_write
storage_read
metadata_lookup
story_index_update
grapher_query
range_retrieval
```

## Validation

- Reconfigured the default Release build with `CHRONOLOG_ENABLE_TAU_PROFILING=OFF`.
- Rebuilt and installed ChronoLog successfully.
- Wrote the detected semantic call sites to `.agent/results/20260511-224300/chronolog/semantic-region-sites.txt`.

Evidence:

- `.agent/results/20260511-224300/chronolog/stdout.log`
- `.agent/results/20260511-224300/chronolog/stderr.log`
- `.agent/results/20260511-224300/chronolog/semantic-region-sites.txt`

## Note

The first build attempt caught one misplaced macro in `KeeperDataStore.cpp`; the retry after moving it into the intended function body completed successfully.
