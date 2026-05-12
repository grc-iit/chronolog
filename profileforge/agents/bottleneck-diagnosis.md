# Bottleneck Diagnosis Agent Contract

The diagnosis agent receives normalized evidence and chooses one dominant bottleneck candidate for the next iteration.

It must not simply repeat prior human hints. It must compare evidence across client, Visor, Keeper, Grapher, Player, network, and storage.

## Required Input

- target manifest
- benchmark configuration
- common metrics
- fixed Kafka and Mofka baseline ratios
- previous ChronoLog iteration history
- TAU semantic summary
- perf summary
- gperftools summary
- Darshan summary
- Linux network measurement summary
- correctness result
- raw evidence links

## Required Output

```yaml
dominant_bottleneck: short_name
confidence: low|medium|medium_high|high
benchmark_context: name_and_parameters
evidence:
  - source: tau|perf|gperftools|darshan|network|app_counter|logs
    finding: concise finding
rejected_explanations:
  - explanation: why another bottleneck is less likely
recommended_next_action:
  type: config|environment|localized_patch|more_measurement
  scope: one bounded change
risk: low|medium|high
validation_required:
  - build
  - correctness
  - repeated_benchmark
```

## Guardrail

If the evidence is too weak to identify one bottleneck, the agent must request a measurement improvement, not a performance patch.
