---
sidebar_position: 1
title: "Running ChronoLog with Docker (Single Node)"
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

First, pull the pre-built ChronoLog image from Docker Hub:

```bash
docker pull gnosisrc/chronolog:latest
```

This downloads the latest ChronoLog image to your local machine, ensuring you have the most up-to-date version.

---

### Step 2: Run the ChronoLog Container

Next, launch the ChronoLog container:

```bash
docker run -it --rm --name chronolog-instance gnosisrc/chronolog:latest
```

This command:

- **Starts** a new ChronoLog container.
- **Runs it in interactive mode** (`-it`), opening a terminal session inside the container.
- **Removes** the container (`--rm`) when it stops, so it doesn't persist unnecessarily.
- **Assigns a name** (`--name chronolog-instance`) for easier reference.

Once executed, you should see a welcome message. At this point, you are **inside** the ChronoLog container.

---

### Step 3: Verify That ChronoLog Is Running

Open a **new terminal window** (outside the Docker container) and run:

```bash
docker ps
```

You should see a container named `chronolog-instance`. For example:

```
CONTAINER ID   IMAGE                        COMMAND       CREATED          STATUS          PORTS     NAMES
bfbc93af1d50   gnosisrc/chronolog:latest    "/bin/bash"   X seconds ago    Up X seconds             chronolog-instance
```

This indicates your ChronoLog container is up and running successfully!

## Deploy ChronoLog Components

With the ChronoLog environment set up, it's time to deploy the system. We'll use the ChronoLog single-node deployment script to start all necessary components.

### Step 1: Navigate to the Deployment Scripts Folder

Inside the container, navigate to the deployment scripts directory:

```bash
cd /home/grc-iit/chronolog_repo/deploy
```

### Step 2: Run the Deployment Script

Execute the deployment script to start the ChronoLog system:

```bash
./local_single_user_deploy.sh -d -w /home/grc-iit/chronolog_install/Release
```

This command will deploy a **basic ChronoLog architecture**, including:

- One **ChronoVisor** (log controller)
- One **ChronoKeeper** (log store manager)
- One **ChronoGrapher** (query and indexing service)
- One **ChronoPlayer** (stream processor)

### Step 3: Verify That ChronoLog Components Are Running

To ensure all components are properly deployed, run the following command:

```bash
pgrep -la chrono
```

You should see an active process for each ChronoLog component:

```bash
/home/grc-iit/chronolog_install/Release/bin/chronovisor_server --config /home/grc-iit/chronolog_install/Release/conf/visor_conf.json
/home/grc-iit/chronolog_install/Release/bin/chrono_grapher --config /home/grc-iit/chronolog_install/Release/conf/grapher_conf_1.json
/home/grc-iit/chronolog_install/Release/bin/chrono_player --config /home/grc-iit/chronolog_install/Release/conf/player_conf_1.json
/home/grc-iit/chronolog_install/Release/bin/chrono_keeper --config /home/grc-iit/chronolog_install/Release/conf/keeper_conf_1.json
```

If all four processes appear, your ChronoLog deployment is successfully running!

## Running a ChronoLog Client + Performance Test

With the ChronoLog environment set up and all components deployed, it's time to test the system by running a client and analyzing its performance.

### Step 1: Execute the ChronoLog Client

Run the following command to start the **client admin** with multiple MPI processes:

```bash
LD_LIBRARY_PATH=~/chronolog_install/Release/lib \
/home/grc-iit/chronolog_repo/.spack-env/view/bin/mpiexec -n 4 \
~/chronolog_install/Release/bin/client_admin \
--config ~/chronolog_install/Release/conf/default_conf.json \
-h 2 -t 2 -a 100 -s 100 -b 100 -n 100 -p
```

This command:

- Launches the **ChronoLog client**.
- Uses **4 MPI processes** for parallel execution (`-n 4`).
- Runs a **performance test** with specific parameters (`-h 2 -t 2 -a 100 -s 100 -b 100 -n 100 -p`).

Once executed, the client will create **Chronicles**, **Stories**, and **Events**, simulating real workloads to evaluate system performance.

## Stopping the ChronoLog Container

Once you're done using ChronoLog, **exit the container**:

```bash
exit
```

Then **stop the running instance** (if it wasn't started with `--rm`):

```bash
docker stop chronolog-instance
```

And if you want to **remove** it completely:

```bash
docker rm chronolog-instance
```

## What's Next?

Now that you have ChronoLog running in Docker, you're ready to:

- Explore different **ChronoLog deployment options** by checking the [Deployment](../../deployment) guide.
- Discover the potential of **ChronoLog Clients**: [Client Examples](../../client/client-examples/basic-client-api) (Basic Client, Interactive Client, Scripted Client & Python Client).
- Start **building your own applications** on top of ChronoLog.

In our next tutorial, we'll show you how to **run ChronoLog in a multi-node setup**, so stay tuned!
