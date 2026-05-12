# Mofka Local Launch Validation

Status: complete for local smoke launch mechanics.

Validation run:

```text
.agent/results/20260512-002453/
```

The fixed-baseline launch wrapper now exports the Spack dependency runtime paths needed by Bedrock provider modules and writes a per-run Mofka configuration manifest. The local `na+sm` smoke launch was then queried with `bedrock-query` while the launch shell remained active.

Evidence:

- Launch stdout/stderr: `.agent/results/20260512-002453/mofka/stdout.log`, `.agent/results/20260512-002453/mofka/stderr.log`
- Group file: `.agent/results/20260512-002453/mofka/mofka.json`
- Bedrock query output: `.agent/results/20260512-002453/mofka/bedrock-query.json`
- Mofka config manifest: `.agent/results/20260512-002453/config/mofka-config-manifest.env`
- Runtime library path capture: `.agent/results/20260512-002453/config/mofka-ld-library-path.txt`
- Python path capture: `.agent/results/20260512-002453/config/mofka-pythonpath.txt`

Observed provider layout:

- master process: Flock group manager plus Yokan controller tagged `mofka:controller`
- storage process: Flock group manager, Yokan metadata provider tagged `mofka:metadata`, and Warabi data provider tagged `mofka:data`
- transport: Mercury `na+sm` for local smoke
- execution shape: one Argobots pool and one xstream per process in the captured Bedrock configuration
- data target: Warabi memory target

Notes:

- Local runs on `ares` remain smoke validations only. Distributed bare-metal SLURM runs are still required for final Phase 0 results.
- The execution environment reaps detached child processes once the command session exits, so local benchmark workflows should launch, run, query, and stop Mofka within the same command/session. SLURM distributed scripts should keep the allocation-side launcher alive for the benchmark duration.
