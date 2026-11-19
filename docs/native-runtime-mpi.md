# MPI build and native-runtime requirements

ChronoLog owns its process Argobots runtime. MPI applications can call
`MPI_Init` before creating a Client when the MPI backend does not initialize
Argobots. The shipped Spack environment selects MPICH 5.0.2 `~argobots`, which
uses MPICH's POSIX thread backend. Margo still uses the pinned Argobots 1.2.

MPICH `+argobots` is unsupported. With MPICH 4.0.2, where this was measured,
its `MPI_Init` initializes Argobots, while its `MPI_Finalize` does not balance
that initialization reference.
ChronoLog rejects an existing runtime with
`CL_ERR_NATIVE_RUNTIME_UNSUPPORTED (-28)` before sending Connect. Starting
ChronoLog before MPI leaves the MPI reference live and prevents safe physical
retirement. Do not reorder startup or add an extra `ABT_finalize` to work
around the backend requirement.

Rebuild the Spack environment, ChronoLog and MPI applications after changing
this backend. Use a separate dependency view and build/install prefix; do not
replace libraries used by running processes. The MPI implementation/version
alone does not identify the thread backend. Inspect `mpichversion` configure
arguments and the actual loaded MPI library. No MPI binary compatibility is
promised by this change.

Build and install ChronoLog through `tools/deploy/local_single_user_deploy.sh`
with dedicated `-B` and `-I` paths. Rebuilding a product inside a cached Docker
base that still contains MPICH `+argobots` is insufficient: rebuild the base
image's Spack environment too. The CI base-image key includes `spack.yaml`.
Retain other pinned dependency versions when reconcretizing.

A Debug build includes `chronolog-test-mpi-lifecycle-probe` under
`tests/integration/watermark-replay/` and installs it under `chronolog/tests/`
when `CHRONOLOG_INSTALL_TESTS` is enabled. Run it against a live deployment with
its real client configuration:

```bash
.spack-env/view/bin/mpiexec -n 4 \
  /path/to/build/Debug/tests/integration/watermark-replay/chronolog-test-mpi-lifecycle-probe \
  /path/to/install/chronolog/conf/default-chrono-client-conf.json
```

Every rank initializes MPI first, checks a collective and barrier, connects,
creates/acquires a story, writes an event, releases/destroys its resources and
disconnects. After Client destruction it waits for actual Argobots retirement
while MPI remains live, then performs another collective and finalizes MPI.
All observed boundary states must be `ABT_ERR_UNINITIALIZED`. The probe fails
before reading the config when MPI itself initializes Argobots. It does not
call compensating Argobots initialization/finalization.

The shipped transport is `ofi+tcp`. For a deployment whose endpoints all use
`127.0.0.1`, `FI_TCP_IFACE=lo` must be set for the daemons, the deploy
script's stop controller and the clients; `deploy_local.sh` exports it for its
own processes when every `service_ip` is loopback and the variable is not
already set, and the native deadline fixtures set it themselves. Unset
interface selection is not proof of a working loopback route. Distributed TCP
must use a reachable container or host interface (the distributed pipeline
sets `FI_TCP_IFACE=eth0`); never carry the loopback setting into a distributed
deployment.

`ofi+sockets` still works, but a reply that arrives after its call's deadline
stays in the sockets provider's receive buffer with no posted receive, and the
provider's progress thread then polls at 100% CPU for the rest of the process's
life (libfabric 1.22 to 2.7). Prefer `ofi+tcp`.

Every control call (story acquire, release, destroy) has a budget of
`control_ms`, and each attempt at most `attempt_ms` (5 s by default). Over
`ofi+tcp`, Mercury moves data only when each process's progress thread runs, so
on oversubscribed cores a control call can run out of its budget and fail with
`CL_ERR_OUTCOME_UNCONFIRMED`. MPI ranks add to this while they wait: MPICH
waits in `MPI_Barrier` by polling (`MPIR_CVAR_POLLS_BEFORE_YIELD` only yields
the CPU), so ranks held in a barrier take whole cores. Measured with 7 daemon
containers and 4 `chrono-bench` ranks pinned to 4 cores:

- with the shipped configuration, 8 of 8 runs passed with a barrier after
  every call (`-y`);
- with the daemons also logging at debug level and flushing every line, 3 of
  7 runs with `-y` failed an acquire, and 1 of 6 without `-y` failed;
  `MPIR_CVAR_CH4_PROGRESS_THROTTLE=1`, which makes a waiting rank sleep, left
  1 failure in 8 runs with `-y`;
- with the same logging on 5, 6 or 8 cores, 18 of 18 runs with `-y` passed.

Give the daemons cores of their own, or leave them headroom. The distributed
pipeline's `chrono-bench` write runs without `-y`, since its 4-CPU runner
leaves little.

Applications must still avoid independently initialized Argobots/Margo work
and keep the client library loaded until process exit. Existing foreign-runtime
rejection and incomplete-cleanup ownership rules remain in force.
