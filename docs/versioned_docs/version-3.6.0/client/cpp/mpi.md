---
sidebar_position: 5
title: "MPI Applications"
---

# Using the Client from MPI Applications

The client library starts and owns its process's Argobots and Margo runtime. An MPI application can call `MPI_Init` before it creates a `Client`, as long as MPI itself does not start Argobots.

## Which MPI

The shipped Spack environment builds MPICH 5.0.2 with `~argobots`, which uses MPICH's POSIX thread backend; Margo still uses the pinned Argobots 1.2. Build MPI applications in that environment.

An MPICH built with `+argobots` is not supported. Its `MPI_Init` starts Argobots, and its `MPI_Finalize` does not undo that, so the client finds a runtime it does not own. The first `Connect` then returns `CL_ERR_NATIVE_RUNTIME_UNSUPPORTED` (`-28`) without sending anything, and keeps returning it for the life of the process. Starting the client before `MPI_Init` does not help: MPI's reference stays live and the client cannot retire its runtime safely. Do not reorder the startup or add an `ABT_finalize` to work around it.

The MPICH version alone does not tell which thread backend it uses. Check the configure arguments that `mpichversion` prints, and the MPI library the application actually loads.

After changing the MPI backend, rebuild the Spack environment, ChronoLog and the MPI applications, and install them under a separate prefix; do not replace libraries that running processes use. No binary compatibility between MPI backends is promised. A Docker base image that still has MPICH `+argobots` must be rebuilt as well, not only the ChronoLog build inside it.

## Rules for the application

- Do not initialize or finalize Argobots or Margo yourself, before or after the client.
- Keep the client library loaded until the process exits; it runs a lifecycle thread for the life of the process.
- All `Client` objects in a process share one context. See [Connection](./api.md#connection) for what happens when the last one is deleted.

## Ranks that wait by polling

MPICH waits in `MPI_Barrier` and other blocking calls by polling. `MPIR_CVAR_POLLS_BEFORE_YIELD` only yields the CPU now and then, so a rank waiting in a barrier keeps a core busy. Over `ofi+tcp` the client and the daemons move data only when their progress threads get a CPU, so ranks that poll on the same cores as the daemons slow every RPC down. With little CPU headroom, control calls can then run out of their 5 s `control_ms` and fail with `CL_ERR_OUTCOME_UNCONFIRMED`.

Measured with 7 daemon containers and 4 `chrono-bench` ranks pinned to 4 cores, the ranks waiting in a barrier after every call:

| Setup | Runs passed |
|---|---|
| shipped configuration | 8 of 8 |
| daemons also logging at debug level and flushing every line | 4 of 7; without the barrier, 5 of 6 |
| the same, with `MPIR_CVAR_CH4_PROGRESS_THROTTLE=1` (a waiting rank sleeps) | 7 of 8 |
| the same logging, on 5, 6 or 8 cores | 18 of 18 |

Give the daemons cores of their own, or leave them headroom. Setting `MPIR_CVAR_CH4_PROGRESS_THROTTLE=1` for the ranks helps; raising `control_ms` and `attempt_ms` helps less than freeing CPU.

## Checking an installation

A Debug build includes `chronolog-test-mpi-lifecycle-probe` under `tests/integration/watermark-replay/` (installed under `chronolog/tests/` when `CHRONOLOG_INSTALL_TESTS` is on). Run it against a running deployment with its client configuration:

```bash
mpiexec -n 4 \
  /path/to/build/Debug/tests/integration/watermark-replay/chronolog-test-mpi-lifecycle-probe \
  /path/to/install/chronolog/conf/default-chrono-client-conf.json
```

Every rank initializes MPI, checks a collective and a barrier, connects, creates and acquires a story, writes an event, releases and destroys what it made, and disconnects. After the client is gone it waits for the client's Argobots runtime to retire while MPI is still live, runs another collective and finalizes MPI. It fails before reading the configuration when MPI itself started Argobots.
