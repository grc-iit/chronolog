---
sidebar_position: 1
title: "Running ChronoLog"
---

# Running ChronoLog with Docker (Single Node)

## Welcome!

If you're looking for the **easiest way to run ChronoLog**, this is it. **ChronoLog's Docker image** provides a ready-to-use installation, so you can skip the manual setup and jump straight into using it.

Let's get started!

## Before we start: What is Docker?

Docker is a tool that allows you to run applications inside **lightweight, isolated environments** called **containers**. This means you don't have to worry about dependencies or system configurations—everything is pre-packaged and ready to go.

For a deeper dive, check out:

- [Docker Overview](https://docs.docker.com/get-started/overview/)
- [Install Docker](https://docs.docker.com/get-docker/)

Before continuing, let's verify that Docker is properly installed on your system:

```bash
docker --version
```

If you see a version number, you're good to go!

## Setting Up ChronoLog Docker Image

### Step 1: Pull the ChronoLog Docker Image

First, pull the pre-built ChronoLog image from GitHub Container Registry:

```bash
docker pull ghcr.io/grc-iit/chronolog:v2.6.0
```

This downloads the ChronoLog v2.6.0 image to your local machine, with a pre-installed release build ready to use.

---

### Step 2: Run the ChronoLog Container

Next, launch the ChronoLog container:

```bash
docker run -it --rm --name chronolog-instance ghcr.io/grc-iit/chronolog:v2.6.0
```

This command:

- **Starts** a new ChronoLog container.
- **Runs it in interactive mode** (`-it`), opening a terminal session inside the container.
- **Removes** the container (`--rm`) when it stops, so it doesn't persist unnecessarily.
- **Assigns a name** (`--name chronolog-instance`) for easier reference.

Once executed, you should see a welcome message. At this point, you are **inside** the ChronoLog container. The working directory is automatically set to the ChronoLog installation (`$CHRONOLOG_HOME`).

---

### Step 3: Verify That ChronoLog Is Running

Open a **new terminal window** (outside the Docker container) and run:

```bash
docker ps
```

You should see a container named `chronolog-instance`. For example:

```bash
CONTAINER ID   IMAGE                                    COMMAND       CREATED          STATUS          PORTS     NAMES
bfbc93af1d50   ghcr.io/grc-iit/chronolog:v2.6.0        "/bin/bash"   X seconds ago    Up X seconds             chronolog-instance
```

This indicates your ChronoLog container is up and running successfully!

## Deploy ChronoLog Components

With the ChronoLog environment set up, it's time to deploy the system. We'll use the ChronoLog single-node deployment script to start all necessary components.

### Step 1: Run the Deployment Script

Inside the container, execute the deployment script to start the ChronoLog system:

```bash
tools/deploy_local.sh --start
```

No `cd` is needed — the container's working directory is already set to the ChronoLog installation.

This command will deploy a **basic ChronoLog architecture**, including:

- One **ChronoVisor** (log controller)
- One **ChronoKeeper** (log store manager)
- One **ChronoGrapher** (query and indexing service)
- One **ChronoPlayer** (stream processor)

### Step 2: Verify That ChronoLog Components Are Running

To ensure all components are properly deployed, run the following command:

```bash
pgrep -fla 'chrono-visor|chrono-keeper|chrono-grapher|chrono-player'
```

You should see an active process for each ChronoLog component:

```bash
/home/grc-iit/chronolog-release-install/chronolog/bin/chrono-visor ...
/home/grc-iit/chronolog-release-install/chronolog/bin/chrono-keeper ...
/home/grc-iit/chronolog-release-install/chronolog/bin/chrono-grapher ...
/home/grc-iit/chronolog-release-install/chronolog/bin/chrono-player ...
```

If all four processes appear, your ChronoLog deployment is successfully running!

## Stop ChronoLog

To stop all ChronoLog services:

```bash
tools/deploy_local.sh --stop
```

To also remove generated logs, configuration artifacts and stored data:

```bash
tools/deploy_local.sh --clean
```

## Next: Client + Performance test

Once ChronoLog is up and running, continue with the [Client + Performance test](./client-performance-test) to generate load and evaluate performance.
