---
sidebar_position: 6
title: "Single Node Deployment"
---

# Single Node Deployment

This document provides a technical reference for the ChronoLog deployment script. It describes the script's purpose, functionalities, command-line arguments, and usage.

### Overview

The ChronoLog deployment script is a Bash utility that streamlines building, installing, starting, stopping, and cleaning a ChronoLog deployment environment. It automates tasks such as generating configuration files, managing processes, and organizing output and log directories.

Key functionalities include:

1. **Building** (Compile the ChronoLog project)
2. **Installing** (Copy the compiled binaries and libraries to a target location)
3. **Starting** (Start Visor, Grapher, Player, and Keeper processes in the ChronoLog environment)
4. **Stopping** (Stop all ChronoLog processes gracefully)
5. **Cleaning** (Remove generated configuration and log files)

### Execution Modes (choose only one)

- **-b | --build**: Build ChronoLog with the specified build type (Debug or Release).
- **-i | --install**: Install ChronoLog binaries and libraries into target directories. Requires a --work-dir to define where it installs by default.
- **-d | --start**: Start the ChronoLog services. Requires a --work-dir, which houses the lib, bin, and conf directories, as well as output and monitoring directories.
- **-s | --stop**: Stop all ChronoLog services (Visor, Grapher, Player, Keeper). Requires --work-dir.
- **-c | --clean**: Remove generated configuration files, logs, and output artifacts. Requires --work-dir and ensures no ChronoLog processes are active.

### Options

| **Option** | **Description** |
|------------|-----------------|
| `-h, --help` | Display script usage help and exit. |
| `-b, --build` | **(Mode)** Build ChronoLog. |
| `-i, --install` | **(Mode)** Install ChronoLog. |
| `-d, --start` | **(Mode)** Start ChronoLog services. |
| `-s, --stop` | **(Mode)** Stop ChronoLog services. |
| `-c, --clean` | **(Mode)** Clean log and config artifacts. |
| `-w, --install-dir` | **(Mandatory)** Sets the working directory containing `bin/`, `lib/`, `conf/`, etc. |
| `-t, --build-type` | **(Optional)** Define the build type: **Debug** or **Release** (default: **Release**). |
| `-k, --keepers` | **(Optional)** Number of **Keeper** processes to launch (default: **1**). |
| `-r, --record-groups` | **(Optional)** Number of **Recording Groups**/**Grapher** processes (default: **1**). |
| `-m, --monitor-dir` | **(Optional)** Directory for log output (default: `$INSTALL_DIR/monitor`). |
| `-u, --output-dir` | **(Optional)** Directory for generated output (default: `$INSTALL_DIR/output`). |
| `-v, --visor-bin` | **(Optional)** Path to the **ChronoVisor** binary (default: `$INSTALL_DIR/bin/chronovisor_server`). |
| `-g, --grapher-bin` | **(Optional)** Path to the **ChronoGrapher** binary (default: `$INSTALL_DIR/bin/chrono_grapher`). |
| `-p, --keeper-bin` | **(Optional)** Path to the **ChronoKeeper** binary (default: `$INSTALL_DIR/bin/chrono_keeper`). |
| `-a, --player-bin` | **(Optional)** Path to the **ChronoPlayer** binary (default: `$INSTALL_DIR/bin/chrono_player`). |
| `-f, --conf-file` | **(Optional)** Path to the **default_conf.json** (default: `$INSTALL_DIR/conf/default_conf.json`). |

### Examples

1. Build the project in Debug mode:

```bash
./local_single_user_deploy.sh --build --build-type Debug --install-dir /home/user/chronolog/Debug
```

2. Install into the default directories (requires a built project and a valid work-dir):

```bash
./local_single_user_deploy.sh --install --install-dir /home/user/chronolog/Debug
```

3. Start ChronoLog with 5 Keeper processes and 2 Recording Groups:

```bash
./local_single_user_deploy.sh --start --keepers 5 --record-groups 2 --install-dir /home/user/chronolog/Debug
```

4. Stop ChronoLog deployment:

```bash
./local_single_user_deploy.sh --stop --install-dir /home/user/chronolog/Debug
```

5. Clean logs and generated config files:

```bash
./local_single_user_deploy.sh --clean --install-dir /home/user/chronolog/Debug
```

### Important Notes

- Ensure only one mode (--build, --install, --start, --stop, --clean) is used per execution.
- `INSTALL_DIR` is mandatory.
- This script relies on certain dependencies: `jq`, `ldd`, `nohup`, `pkill`, `readlink`. Make sure they are installed.

---
