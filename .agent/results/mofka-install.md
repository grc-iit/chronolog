# Mofka Fixed-Baseline Tooling

Status: complete.

Mofka was installed without sudo through the existing user-local Spack installation after no module-provided Mofka command was found. The installed fixed-baseline spec is:

```text
mofka@0.6.4+python~mpi~benchmark~kafka
```

Installed command paths:

```text
/mnt/common/jcernudagarcia/spack/opt/spack/linux-ubuntu22.04-skylake_avx512/gcc-11.4.0/mochi-bedrock-0.16.0-qpmaqjnfyb7tvc675nf2aa5oxscjchu7/bin/bedrock
/mnt/common/jcernudagarcia/spack/opt/spack/linux-ubuntu22.04-skylake_avx512/gcc-11.4.0/mofka-0.6.4-labml323r2hejghgbtqu3lfnha63doqo/bin/mofkactl
```

Evidence:

- Install log: `.agent/results/20260511-235742/mofka/stdout.log`
- Empty install stderr: `.agent/results/20260511-235742/mofka/stderr.log`
- Summary: `.agent/results/20260511-235742/summary.md`

Notes:

- The local `opt/mochi-spack-packages` checkout was adjusted for compatibility with the installed Spack version so Spack could resolve/build Mofka. This is packaging/exposure work only; no Mofka source code was modified for benchmarking.
- This install is sufficient to continue launch and fixed-baseline smoke work, but benchmark-generator support requires a fuller Mofka build with benchmark-related dependencies.
