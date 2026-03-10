---
sidebar_position: 5
title: "Deployment"
---

# Deployment

## 4.1 Single Node Deployment

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

## 4.2 Distributed Deployment

ChronoLog is designed to work in a distributed environment. We prepare a script at `deploy/single_user_deploy.sh` to help deploying it in a distributed environment.

[mpssh](https://github.com/ndenev/mpssh) and [jq](https://jqlang.github.io/jq/) are needed to run the script. Passwordless SSH needs to be configured for the script to work.

By default, the script reads hosts files, `hosts_visor`, `hosts_keeper`, `hosts_grapher`, and `hosts_client` to be specific, under `~/chronolog/conf` directory for the hosts to run the ChronoLog server daemons.

If the cluster uses **Slurm** for job scheduling, you can deploy ChronoLog in your active job using the following command:

```bash
./deploy/single_user_deploy.sh -d -n NUM_RECORDING_GROUP -j JOB_ID
```

The script will fetch the node list from Slurm, assign the first node to run the ChronoVisor, all the nodes to run ChronoKeepers, and the last `NUM_RECORDING_GROUP` nodes to run ChronoGraphers. The `client_lib_multi_storytellers` use case will be launched at last to test if the deployment is successful.

```
Usage:
-d|--deploy Start ChronoLog deployment (default: false)
-s|--stop Stop ChronoLog deployment (default: false)
-k|--kill Terminate ChronoLog deployment (default: false)
-r|--reset Reset/cleanup ChronoLog deployment (default: false)
-w|--work_dir WORK_DIR (default: ~/chronolog)
-u|--output_dir OUTPUT_DIR (default: work_dir/output)
-v|--visor VISOR_BIN (default: work_dir/bin/chronovisor_server)
-g|--grapher GRAPHER_BIN (default: work_dir/bin/chrono_grapher)
-p|--keeper KEEPER_BIN (default: work_dir/bin/chrono_keeper)
-c|--client CLIENT_BIN (default: work_dir/bin/client_lib_multi_storytellers)
-i|--visor_hosts VISOR_HOSTS (default: work_dir/conf/hosts_visor)
-a|--grapher_hosts GRAPHER_HOSTS (default: work_dir/conf/hosts_grapher)
-o|--keeper_hosts KEEPER_HOSTS (default: work_dir/conf/hosts_keeper)
-t|--client_hosts CLIENT_HOSTS (default: work_dir/conf/hosts_client)
-f|--conf_file CONF_FILE (default: work_dir/conf/default_conf.json)
-j|--job_id JOB_ID (default: "", overwrites hosts files if set)
-n|--num_recording_group NUM_RECORDING_GROUP (default: #hosts in GRAPHER_HOSTS if exists, 1 otherwise, overwrites hosts files if set)
-e|--verbose Enable verbose output (default: false)
-h|--help Print this page
```
