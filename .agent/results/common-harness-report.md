# Common Phase 0 Harness Artifacts

Status: complete for local-smoke result validation.

Artifacts:

- Shared workload config: `.agent/config/workloads.json`
- Metrics schema: `.agent/config/metrics-schema.json`
- Metrics validator: `.agent/scripts/phase0_validate_metrics.py`
- ChronoLog local smoke config manifest: `.agent/results/20260511-222650/config/chronolog-config-manifest.env`
- ChronoLog TAU local smoke config manifest: `.agent/results/20260511-231340/config/chronolog-config-manifest.env`
- Kafka local smoke config manifest: `.agent/results/20260511-234455/config/kafka-config-manifest.env`
- Mofka local smoke config manifest: `.agent/results/20260512-003314/config/mofka-config-manifest.env`

Validation command:

```text
.agent/scripts/phase0_validate_metrics.py \
  .agent/results/20260511-222650/chronolog/metrics.json \
  .agent/results/20260511-234455/kafka/metrics.json \
  .agent/results/20260512-003314/mofka/metrics.json
```

Validation evidence:

- `.agent/results/20260512-003711/harness/stdout.log`
- `.agent/results/20260512-003711/harness/stderr.log`

Scope:

- The validator checks the required common `metrics.json` fields and allowed system names.
- The manifests identify deployment mode, workload parameters, config files, logs, metrics, and comparison notes for existing local smoke results.
- These artifacts do not make the existing local smoke results final distributed results. They make the current evidence explicit and machine-checkable enough to continue toward the distributed Phase 0 comparison.
