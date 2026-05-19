# Patch / Tuning Agent Contract

The patch agent applies one bounded change selected from a bottleneck diagnosis.

It must obey:

- `profileforge/targets/chronolog/allowed_edits.yaml`
- `profileforge/controller/acceptance-policy.yaml`
- benchmark oracle and fixed-baseline immutability

## Allowed Change Shape

One iteration may do one of:

- one configuration change
- one environment or placement change
- one localized source patch
- one instrumentation/counter addition if evidence is insufficient

## Required Output

```yaml
change_summary: concise description
files_changed:
  - path
expected_effect: what should improve and why
risk: low|medium|high
validation_commands:
  - command
rollback_plan: how to revert if rejected
```

## Forbidden

- Editing Kafka or Mofka source.
- Editing metric validators to make a change pass.
- Hand-editing generated benchmark scores.
- Combining unrelated optimizations in one iteration.
- Broad architectural rewrites before Level 0 and Level 1 space has been explored.
