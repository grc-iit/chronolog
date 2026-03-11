---
sidebar_position: 2
title: "Running ChronoLog with Docker (Multi Node)"
---

# Running ChronoLog with Docker (Multi Node)

## Welcome!

This simplified tutorial will guide you through quickly setting up **ChronoLog** in a multi-node environment using our automated deployment script. By the end, you'll have a fully operational **distributed ChronoLog system**, with minimal setup steps required.

## Overview

### What You'll Learn

- Quickly deploy ChronoLog using our automated script.
- Customize your ChronoLog deployment with flexible configuration options.
- Easily interact with and verify your ChronoLog setup.

For a **simpler single-node setup**, see our guide: [Running ChronoLog with Docker (Single Node)](/docs/tutorials/docker-single-node).

## Prerequisites: Docker & Docker Compose

ChronoLog runs inside **Docker containers**, orchestrated by **Docker Compose**, for easy multi-node management.

### Install Docker & Docker Compose

- [Docker Installation Guide](https://docs.docker.com/get-docker/)
- [Docker Compose Installation Guide](https://docs.docker.com/compose/install/)

### Verify Installation

```bash
docker --version
```

```bash
docker compose version
```

## Setting Up ChronoLog Using Automated Deployment

### Step 1: Pull ChronoLog Docker Image

```bash
docker pull gnosisrc/chronolog:latest
```

### Step 2: Download the Deployment Script

Retrieve the dynamic deployment script and make it executable:

```bash
wget https://raw.githubusercontent.com/grc-iit/ChronoLog/refs/heads/develop/CI/docker/dynamic_deploy.sh
```

```bash
chmod +x dynamic_deploy.sh
```

### Step 3: Run the Deployment Script

**Default deployment (1 of each component):**

```bash
./dynamic_deploy.sh
```

**Customized deployment:**

Use flags for a custom setup:

- `-n` total number of components (ChronoVisor + sum of the rest)
- `-k` number of ChronoKeepers
- `-g` number of ChronoGraphers
- `-p` number of ChronoPlayers

Example:

```bash
./dynamic_deploy.sh -n 7 -k 2 -g 2 -p 2
```

This example will deploy a docker instance for the ChronoVisor, 2 instances for ChronoKeepers, 2 for ChronoGraphers and 2 for ChronoPlayers.

Check your deployment:

```bash
docker ps
```

## Running a Client Performance Test

### Step 1: Generate Client Hosts File

Run the following command directly from your host machine:

```bash
docker exec chronolog-c1 bash -c 'echo "chronolog-c1" > ~/chronolog_install/Release/conf/hosts_client'
```

### Step 2: Run Client Performance Test

Execute the performance test directly:

```bash
docker exec chronolog-c1 bash -c 'LD_LIBRARY_PATH=~/chronolog_install/Release/lib ~/chronolog_repo/.spack-env/view/bin/mpiexec -n 4 -f ~/chronolog_install/Release/conf/hosts_client ~/chronolog_install/Release/bin/client_admin --config ~/chronolog_install/Release/conf/default_client_conf.json -a 4096 -b 4096 -s 4096 -n 4096 -t 1 -h 1 -p -r'
```

## Stopping ChronoLog

Stop all containers:

```bash
docker compose -f dynamic-compose.yaml down
```

Remove volumes (optional):

```bash
docker compose -f dynamic-compose.yaml down -v
```

## What's Next?

With ChronoLog running, you can:

- **Deploy workloads within containers**.
- **Integrate ChronoLog into your infrastructure**.
- **Customize setups** based on your needs.

Stay tuned for further advanced tutorials!
