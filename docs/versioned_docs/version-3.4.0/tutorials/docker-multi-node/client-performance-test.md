---
sidebar_position: 2
title: "Client + Performance test"
---

# Client + Performance test (Multi Node)

With your multi-node ChronoLog deployment running, you can now run a client performance test to exercise the system and validate the distributed setup.

> **Prerequisite:** Make sure you have already completed the steps in [Running ChronoLog](./running-chronolog) so that the multi-node ChronoLog deployment is up and running.

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

With ChronoLog running and your performance test completed, you can:

- **Deploy workloads within containers**.
- **Integrate ChronoLog into your infrastructure**.
- **Customize setups** based on your needs.

If you haven't yet explored the single-node Docker workflow, see [Docker (single node) - Running ChronoLog](../docker-single-node/running-chronolog) for a simpler starting point.
