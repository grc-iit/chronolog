---
sidebar_position: 3
title: "Docker Compose"
---

# Docker Compose Deployment

`dynamic_deploy.sh` provides a containerized multi-node environment for developing and testing ChronoLog without physical cluster hardware. It generates a Docker Compose file at runtime, starts the containers, configures SSH between them, and deploys the pre-built ChronoLog services across the cluster — following the same SSH-based approach as the [multi-node deployment](./multi-node.md). Unlike the [single-node](./single-node.md) and [multi-node](./multi-node.md) bare-metal scripts, this workflow handles the full lifecycle from image launch through service startup in one step. The script is located at `CI/docker/dynamic_deploy.sh` in the repository.

## Prerequisites

| Requirement | Notes |
|-------------|-------|
| Docker Engine | Must be running and reachable |
| Docker Compose v2 | `docker compose` (not `docker-compose`) must be on `PATH` |
| `docker` group membership | Current user must be able to run `docker` without `sudo` |

## Docker Image

All containers use `ghcr.io/grc-iit/chronolog:v2.6.0` (or `ghcr.io/grc-iit/chronolog:latest`). The image provides:

- Ubuntu 24.04 base
- Spack package manager with pre-installed dependencies (MPICH, Mochi/Thallium, etc.)
- `grc-iit` user account
- ChronoLog pre-installed at `$CHRONOLOG_HOME` (`/home/grc-iit/chronolog-release-install/chronolog/`)
- OpenSSH server for inter-container SSH (started automatically by the container entrypoint)

ChronoLog is **pre-built and ready to deploy** — no compilation is needed inside the containers.

## Container Layout

The script assigns each container a fixed role based on its index. For a deployment with K keepers, G graphers, and P players the total container count is `N = 1 + K + G + P`.

| Container | Hostname | Role |
|-----------|----------|------|
| `chronolog-c1` | `c1` | ChronoVisor |
| `chronolog-c2` ... `chronolog-c(K+1)` | `c2` ... `c(K+1)` | ChronoKeeper x K |
| `chronolog-c(K+2)` ... `chronolog-c(K+G+1)` | `c(K+2)` ... `c(K+G+1)` | ChronoGrapher x G |
| `chronolog-c(K+G+2)` ... `chronolog-c(N)` | `c(K+G+2)` ... `c(N)` | ChronoPlayer x P |

All containers are connected to the `chronolog_net` bridge network and mount the same `shared_home` Docker volume at `/home/grc-iit`. This shared volume is automatically populated with the image contents (including the ChronoLog installation) on first use, making the installed binaries and libraries visible to all containers.

## Script Options

| Flag | Default | Constraint | Description |
|------|---------|------------|-------------|
| `-n` | `4` | >= 2; must equal 1 + K + G + P | Total number of containers |
| `-k` | `1` | >= 1 | Number of ChronoKeeper containers |
| `-g` | `1` | >= 1 | Number of ChronoGrapher containers |
| `-p` | `1` | >= 1 | Number of ChronoPlayer containers |

The script exits with an error if `-n` does not equal `1 + k + g + p`.

## Deployment Steps

Running `dynamic_deploy.sh` performs the following steps in order:

1. **Generate `dynamic-compose.yaml`** — writes a Compose file that defines one service per container (`c1`...`cN`), all sharing `shared_home` and `chronolog_net`.
2. **Start containers** — runs `docker compose -f dynamic-compose.yaml up -d`; containers `c2`...`cN` depend on `c1`.
3. **SSH key setup on `c1`** — generates an RSA 4096-bit key pair under `/home/grc-iit/.ssh/`, copies the public key to `authorized_keys`, and scans host keys for all containers into `known_hosts`. Sets correct permissions on the `.ssh` directory.
4. **Restart SSH on all containers** — ensures the SSH daemon is running and accepts the newly configured keys.
5. **Generate host files** — writes one hostname per line into `~/chronolog-release-install/chronolog/conf/`:
   - `hosts_visor` — `c1`
   - `hosts_keeper` — `c2` ... `c(K+1)`
   - `hosts_grapher` — `c(K+2)` ... `c(K+G+1)`
   - `hosts_player` — `c(K+G+2)` ... `c(N)`
   - `hosts_clients` and `hosts_all` — all containers
6. **Deploy** — runs `deploy_cluster.sh` with the host files generated in step 5. ChronoLog processes are started across all containers via SSH, matching the multi-node deployment flow.

## Examples

Default 4-container deployment (1 ChronoVisor, 1 ChronoKeeper, 1 ChronoGrapher, 1 ChronoPlayer):

```bash
cd CI/docker
./dynamic_deploy.sh
```

8-container deployment with 3 keepers, 2 graphers, and 2 players:

```bash
cd CI/docker
./dynamic_deploy.sh -n 8 -k 3 -g 2 -p 2
```

## Stopping

Bring down the containers:

```bash
docker compose -f CI/docker/dynamic-compose.yaml down
```

To also remove the `shared_home` volume (required when upgrading to a new ChronoLog image version):

```bash
docker compose -f CI/docker/dynamic-compose.yaml down -v
```

:::note
`dynamic-compose.yaml` is generated at runtime in the `CI/docker/` directory. A static 4-node reference file is available at `CI/docker/distributed.docker-compose.yaml` if you need a fixed, pre-defined topology without running the generation script.
:::
