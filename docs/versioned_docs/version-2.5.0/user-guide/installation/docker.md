---
sidebar_position: 1
title: "Docker"
---

# Docker Installation

:::info
This page is under construction. More detailed content will be added soon.
:::

## Prerequisites

Ensure [Docker](https://docs.docker.com/get-docker/) is installed and the Docker daemon is running on your system.

## Pull the ChronoLog Image

```bash
docker pull gnosisrc/chronolog:latest
```

## Run the Container

```bash
docker run -it --rm gnosisrc/chronolog:latest bash
```

This opens an interactive shell inside the container with ChronoLog pre-installed.

## Deploy ChronoLog

Inside the container, run the single-node deployment script:

```bash
./local_single_user_deploy.sh --start --install-dir /home/user/chronolog
```

## Verify Deployment

Check that the ChronoLog processes are running:

```bash
pgrep -la chrono
```

You should see `chronovisor_server`, `chrono_keeper`, `chrono_grapher`, and `chrono_player` listed.

## Next Steps

- For single-node Docker deployments, see the [Single Node Tutorial](/docs/tutorials/docker-single-node/running-chronolog).
- For multi-node Docker deployments, see the [Multi Node Tutorial](/docs/tutorials/docker-multi-node/running-chronolog).
- For Docker Compose setups, see [Docker Compose Deployment](/docs/user-guide/deployment/docker-compose).
