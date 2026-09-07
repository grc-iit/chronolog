---
sidebar_position: 2
title: "Client + Performance test"
---

# Client + Performance test (Single Node)

With the ChronoLog environment set up and all components deployed, it's time to test the system by running a client and analyzing its performance.

> **Prerequisite:** Make sure you have already completed the steps in [Running ChronoLog](./running-chronolog) so that ChronoLog is up and running inside the Docker container.

## Running a ChronoLog Client + Performance Test

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

Now that you have ChronoLog running in Docker and have executed a performance test, you're ready to:

- Explore different **ChronoLog deployment options** by checking the [Deployment](../../user-guide/deployment/single-node) guide.
- Discover the potential of **ChronoLog Clients**: [Client Examples](../../client/cpp/examples) (Basic Client, Interactive Client, Scripted Client & Python Client).
- Start **building your own applications** on top of ChronoLog.

If you're interested in scaling out, you can move on to the multi-node setup in [Docker (multi node)](../docker-multi-node/running-chronolog).
