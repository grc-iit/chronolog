# Mofka Benchmark Tooling Exploration

Status: explored; usable path requires additional Mofka benchmark dependencies.

Mofka exposes bundled benchmark tooling through:

```text
mofkactl benchmark generate
```

The generator is relevant to Phase 0 because it covers service shape, topic/data shape, producer behavior, consumer behavior, and mixed producer/consumer behavior. Configuration axes exposed by the CLI include:

- service shape: server count, metadata databases per process, data storage targets per process, database/storage path prefixes, persistence flags, pools, and execution streams
- topic/data shape: partition count, metadata field count, metadata key/value sizes, data block count, and total data size
- producer behavior: producer count, batch size, adaptive batching, ordering, thread count, burst size, inter-event/inter-burst waits, and flush policy
- consumer behavior: consumer count, batch size, adaptive batching, data validation, thread count, data-selector selectivity/proportions, and data-broker block counts
- mixed mode: simultaneous producer and consumer generation

Validation attempted:

```text
mofkactl benchmark generate -a na+sm -n 10 --num-servers 1 --num-producers 1 --num-consumers 0
```

Result: failed because the current fixed-baseline install is `~benchmark` and does not include the ConfigSpace-backed benchmark generation dependency path.

Evidence:

- CLI help captured during exploration: command output from `mofkactl benchmark generate --help`
- Failed generator run: `.agent/results/20260512-001403/mofka/stderr.log`
- Failure root cause: `ModuleNotFoundError: No module named 'ConfigSpace'`

Implication for Phase 0:

- Short term: continue fixed-baseline launch and smoke work with the minimal Mofka install.
- Benchmark harness option: build a second Mofka toolchain spec with `+benchmark` if the bundled generator will be used as a reference workload source.
- Comparison option: use the generator output model to inform the common ChronoLog/Kafka/Mofka workload schema, even if the first smoke benchmark uses a smaller custom harness.
