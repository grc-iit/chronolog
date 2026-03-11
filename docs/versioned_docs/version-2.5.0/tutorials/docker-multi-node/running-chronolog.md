---
sidebar_position: 1
title: "Running ChronoLog"
---

# Running ChronoLog with Docker (Multi Node)

## Welcome!

This simplified tutorial will guide you through quickly setting up **ChronoLog** in a multi-node environment using our automated deployment script. By the end, you'll have a fully operational **distributed ChronoLog system**, with minimal setup steps required.

If you're looking for a simpler single-node setup instead, see [Docker (single node) - Running ChronoLog](../docker-single-node/running-chronolog).

## Overview

### What You'll Learn

- Quickly deploy ChronoLog using our automated script.
- Customize your ChronoLog deployment with flexible configuration options.
- Easily interact with and verify your ChronoLog setup.

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

## Next: Client Performance Test

Once your multi-node deployment is running, continue with the [Client + Performance test](./client-performance-test) to generate load and validate the distributed setup.
