# TAU Detection and Install

Status: complete.

Evidence directory: `.agent/results/20260511-220126/`

## Detection

No existing TAU installation was found on `PATH`.

Missing commands during initial detection:

- `tau_cc.sh`
- `tau_cxx.sh`
- `tau_exec`
- `tau_run`
- `pprof`
- `paraprof`
- `tau_treemerge`
- `tau2otf`
- `tau2slog2`

No TAU module was visible through `module avail`. Spack recognized the `tau` package but had no installed TAU package.

## Install

Installed TAU 2.34 from the official TAU release tarball into the project-local prefix:

```text
opt/tau-2.34
```

Configuration used:

```bash
./configure \
  -prefix="$PWD/../../tau-2.34" \
  -cc=gcc \
  -c++=g++ \
  -PROFILE \
  -pthread \
  -bfd=download \
  -unwind=download \
  -dwarf=download
```

The first source download attempt failed because local certificate verification could not validate the official TAU site certificate. The second attempt used `wget --no-check-certificate` against the same official TAU release URL and succeeded. Configure initially rejected `-pthreads`; the successful configure used TAU's supported `-pthread` flag.

## Validation

Installed artifacts:

| Artifact | Path |
|---|---|
| TAU header | `opt/tau-2.34/include/TAU.h` |
| C wrapper | `opt/tau-2.34/x86_64/bin/tau_cc.sh` |
| C++ wrapper | `opt/tau-2.34/x86_64/bin/tau_cxx.sh` |
| Runtime wrapper | `opt/tau-2.34/x86_64/bin/tau_exec` |
| Profile reader | `opt/tau-2.34/x86_64/bin/pprof` |
| Static profiling library | `opt/tau-2.34/x86_64/lib/libtau-pthread.a` |
| Shared profiling library | `opt/tau-2.34/x86_64/lib/libTAUsh-pthread.so` |
| TAU makefile | `opt/tau-2.34/x86_64/lib/Makefile.tau-pthread` |

Validated wrapper commands with:

```bash
TAU_MAKEFILE=opt/tau-2.34/x86_64/lib/Makefile.tau-pthread opt/tau-2.34/x86_64/bin/tau_cc.sh -tau:show
TAU_MAKEFILE=opt/tau-2.34/x86_64/lib/Makefile.tau-pthread opt/tau-2.34/x86_64/bin/tau_cxx.sh -tau:show
```

Environment snippet for later builds:

```text
.agent/config/tau-env.sh
```

The `opt/` tree is a local dependency install and is intentionally ignored instead of committed.
