# MPI build and native-runtime requirements

ChronoLog owns its process Argobots runtime. MPI applications can call
`MPI_Init` before creating a Client when the MPI backend does not initialize
Argobots. The shipped Spack environment selects MPICH 4.0.2 `~argobots`, which
uses MPICH's POSIX thread backend. Margo still uses the pinned Argobots 1.1.

MPICH 4.0.2 `+argobots` is unsupported. Its `MPI_Init` initializes Argobots,
while its `MPI_Finalize` does not balance that initialization reference.
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

For a native TCP fixture whose endpoints all use `127.0.0.1`, explicitly set
`FI_TCP_IFACE=lo` for both the daemons and MPI ranks, as the existing native
deadline fixtures do. Unset interface selection is not proof of a working
loopback route. Distributed TCP must use a reachable container or host
interface; never carry the loopback setting into a distributed deployment.

Applications must still avoid independently initialized Argobots/Margo work
and keep the client library loaded until process exit. Existing foreign-runtime
rejection and incomplete-cleanup ownership rules remain in force.
