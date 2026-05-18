---
sidebar_position: 3
title: "Building for Development"
---

# Building for Development

The developer deployment scripts combine build, install, and runtime management into a single workflow. They wrap `build.sh` and `install.sh` for build/install steps, then delegate start/stop/clean to the user-facing `deploy_local.sh` or `deploy_cluster.sh` scripts.

Both scripts live in `tools/deploy/ChronoLog/` alongside the other deployment scripts.

## Single-Node: `local_single_user_deploy.sh`

Use this script for development and testing on a single machine.

### Execution Modes

| Mode | Description |
|------|-------------|
| `--build` | Compile ChronoLog from source using `build.sh` |
| `--install` | Copy compiled artifacts to the install directory using `install.sh` |
| `--start` | Start ChronoLog (delegates to `deploy_local.sh`) |
| `--stop` | Stop ChronoLog (delegates to `deploy_local.sh`) |
| `--clean` | Clean generated files (delegates to `deploy_local.sh`) |

### Options

| Option | Default | Description |
|--------|---------|-------------|
| `-b, --build` | — | Build mode |
| `-i, --install` | — | Install mode |
| `-d, --start` | — | Start mode |
| `-s, --stop` | — | Stop mode |
| `-c, --clean` | — | Clean mode |
| `-t, --build-type <Debug\|Release>` | `Release` | CMake build type |
| `-B, --build-dir <path>` | `$HOME/chronolog-build` | Directory for CMake build output |
| `-I, --install-dir <path>` | `$HOME/chronolog-install` | Installation root; binaries land in `<install-dir>/chronolog/bin/` |
| `-w, --work-dir <path>` | `<install-dir>/chronolog` | ChronoLog tree root (for start/stop/clean) |
| `-k, --keepers <n>` | `1` | Number of ChronoKeeper processes |
| `-r, --record-groups <n>` | `1` | Number of recording groups |
| `-m, --monitor-dir <path>` | `<work-dir>/monitor` | Log output directory |
| `-u, --output-dir <path>` | `<work-dir>/output` | Story file output directory |
| `-v, --visor-bin <path>` | `<work-dir>/bin/chrono-visor` | ChronoVisor binary path |
| `-g, --grapher-bin <path>` | `<work-dir>/bin/chrono-grapher` | ChronoGrapher binary path |
| `-p, --keeper-bin <path>` | `<work-dir>/bin/chrono-keeper` | ChronoKeeper binary path |
| `-a, --player-bin <path>` | `<work-dir>/bin/chrono-player` | ChronoPlayer binary path |
| `-f, --conf-file <path>` | `<work-dir>/conf/default-chrono-conf.json` | Main configuration template |
| `-n, --client-conf-file <path>` | `<work-dir>/conf/default-chrono-client-conf.json` | Client configuration template |

### Typical Developer Workflow

```bash
cd tools/deploy/ChronoLog

# 1. Build in Debug mode
./local_single_user_deploy.sh --build --build-type Debug --install-dir ~/chronolog-install

# 2. Install to the target directory
./local_single_user_deploy.sh --install --build-type Debug --install-dir ~/chronolog-install

# 3. Start ChronoLog
./local_single_user_deploy.sh --start --work-dir ~/chronolog-install/chronolog

# 4. Run your tests / client code
#    (clients use ~/chronolog-install/chronolog/conf/chrono-client-conf.json)

# 5. Stop ChronoLog
./local_single_user_deploy.sh --stop --work-dir ~/chronolog-install/chronolog

# 6. Clean generated files before the next run
./local_single_user_deploy.sh --clean --work-dir ~/chronolog-install/chronolog
```

To start with multiple keepers and recording groups:

```bash
./local_single_user_deploy.sh --start \
    --work-dir ~/chronolog-install/chronolog \
    --keepers 5 \
    --record-groups 2
```

---

## Cluster: `single_user_deploy.sh`

Use this script for development on a multi-node cluster. It follows the same mode structure as `local_single_user_deploy.sh` but delegates start/stop/clean to `deploy_cluster.sh`.

### Execution Modes

| Mode | Description |
|------|-------------|
| `--build` | Compile ChronoLog from source |
| `--install` | Install compiled artifacts |
| `--start` | Start cluster deployment (delegates to `deploy_cluster.sh`) |
| `--stop` | Stop cluster deployment (delegates to `deploy_cluster.sh`) |
| `--clean` | Clean generated files (delegates to `deploy_cluster.sh`) |

### Options

All options from `local_single_user_deploy.sh` apply, plus:

| Option | Default | Description |
|--------|---------|-------------|
| `-j, --job-id <id>` | _(none)_ | Slurm job ID; derives host list from active job |
| `-q, --visor-hosts <path>` | `<work-dir>/conf/hosts_visor` | Visor host file |
| `-k, --grapher-hosts <path>` | `<work-dir>/conf/hosts_grapher` | Grapher host file |
| `-o, --keeper-hosts <path>` | `<work-dir>/conf/hosts_keeper` | Keeper host file |
| `-e, --verbose` | `false` | Enable verbose output |

### Typical Developer Workflow (Cluster)

```bash
cd tools/deploy/ChronoLog

# 1. Build and install (same as local)
./single_user_deploy.sh --build --build-type Debug --install-dir /shared/chronolog-install
./single_user_deploy.sh --install --build-type Debug --install-dir /shared/chronolog-install

# 2. Start using a Slurm job
./single_user_deploy.sh --start \
    --work-dir /shared/chronolog-install/chronolog \
    --job-id $SLURM_JOB_ID \
    --record-groups 2

# 3. Stop and clean
./single_user_deploy.sh --stop --work-dir /shared/chronolog-install/chronolog --job-id $SLURM_JOB_ID
./single_user_deploy.sh --clean --work-dir /shared/chronolog-install/chronolog
```

For cluster deployment prerequisites (passwordless SSH, shared filesystem, `parallel-ssh`), see [Multi-Node Deployment](../../user-guide/deployment/multi-node.md).
