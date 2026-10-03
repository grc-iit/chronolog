# Operating ChronoLog

For agent discovery, local boot, leases, storage placement and recovery, start with [local-instances.md](local-instances.md). Containers below remain the demo path.

## Build from source

Requirements: Linux x86_64 (developed on Ubuntu 24.04), GCC 13, CMake with presets, Ninja, Python 3.12 with venv, Node 22 for the TypeScript binding, and vcpkg at the baseline pinned in `vcpkg.json` (`VCPKG_ROOT` set). The first configure builds gRPC, protobuf, HDF5, SQLite, NuRaft and curl through vcpkg; that takes a long time and a lot of memory, so build on a server rather than a laptop.

```bash
cmake --preset dev && cmake --build --preset dev     # binaries under build/dev
ctest --preset dev                                   # unit, contract and acceptance tests
bash tests/smoke/build_artifacts.sh                  # wheels, viz assets, TypeScript package, images' inputs
```

Presets: `dev` (debug), `release`, `python` (the wheel's own build directory), `tsan` and `asan` (instrumented dependency triplets in `triplets/`).

Server binaries: `build/dev/src/chrono-visor/chrono_visor`, `.../chrono-keeper/chrono_keeper`, `.../chrono-grapher/server/chrono_grapher`, `.../chrono-player/chrono_player`. Each takes `--config <file.json>`; in containers the entrypoint builds the config from `CHRONOLOG_*` environment variables and `CHRONOLOG_ROLE`.

Container-only build (no host toolchain; slow the first time): `deploy/containers/builder.Containerfile` prebuilds the vcpkg dependencies and `deploy/containers/runtime.Containerfile` builds a release runtime image from it. The demo kit uses the faster `runtime-local.Containerfile`, which packages binaries built natively.

## Single host with containers

`deploy/compose/compose.yaml` runs one of each service on a private network, rootless under Docker or Podman, read-only file systems, all capabilities dropped. Volumes: `visor-catalog` (SQLite Catalog), `keeper-wal` (write-ahead log), `archive` (HDF5 chunks and manifest; the Player mounts it read-only).

| Override | Adds |
|---|---|
| `demo.override.yaml` | host-reachable 127.0.0.1 endpoints and fast seal and chunk timings for demos |
| `smoke.override.yaml` | the fast timings alone (1 s chunks, 200 ms seal, 1 s archive visibility) |
| `stream.override.yaml` | InfluxDB 2.7, Grafana 11.6, the telemetry collector and exporter |
| `viz.override.yaml` | the viz backend (8087) and Grafana with the ChronoLog datasource |
| `mcp.override.yaml` | chronolog-mcp over HTTP on 8000 |

Advertised endpoints matter: a Keeper advertises one address, and clients connect to whatever it advertises. Use 127.0.0.1 endpoints (the demo override) for clients on the host, and service names (the base file) for clients inside the compose network.

## Several machines

`deploy/cluster/` runs the services natively across hosts over SSH: one Visor, two Graphers, two Keepers, a Player, and the archive on a shared NFS mount. `deploy/cluster/run_dragon.sh --preflight-only` checks prerequisites (prebuilt binaries, passwordless SSH to the other hosts, the NFS mount, an empty archive directory) before a full run. Role configs are the JSON files beside it; edit addresses for your hosts.

## Dynamic membership

Default is static membership: a fixed Keeper list, every story at epoch 1. For high availability set `membership_mode` to dynamic and give each of three Visors a `raft` block (its server id, Raft endpoint and the peer list). The Catalog then commits through NuRaft with an fsynced log; any replica serves reads and watches, followers forward writes to the leader. Keepers, Graphers and Players take the Visor address as a comma-separated list of replicas and fail over between them. Operate the cluster with `chronolog_admin <visor-internal-endpoint> list|drain|join|abandon [process_id]`. `tests/integration/dynamic/` holds a working three-replica setup and seven failover scenarios (Visor leader kill, Keeper partition, successor admitted above the cut, predecessor drain, transition budget, join with a stale Player, abandonment); its ctest `dynamic_failover_integration` runs them on one host, and `tests/integration/dynamic/run.py --homelab` runs them across machines. `src/chrono-visor/VisorConfig.cpp` lists every configuration key.

## Health and debugging

- `chronolog-demo status` and `chronolog-demo logs <service>`; in raw compose, `<engine> compose -p <project> ps` and `logs`.
- Each service logs one readiness line at start: Visor `catalog ready`, Keeper `journal ready`, Player `player ready`, Grapher `grapher registered`.
- A read that stays incomplete names its laggards: the writer ids and the Keeper holding the frontier back.
- A Keeper that restarts replays its WAL; DURABLE events keep their ids and HLCs, and ACCEPTED events that were not archived are gone by design.
- `OutOfRange` on append means a supplied physical reading was outside the acceptance window; drop `physical=` and let the SDK stamp it.
- Port conflicts: only one stack per host uses 50051 to 50054; `down` the other one or change the published ports in an override.

## Tests

`ctest --preset dev` runs everything that does not need containers, including the contract suites every implementation must pass. `tests/smoke/run_dragon.sh` brings the full stack up under Docker and Podman and runs the Python, MCP, TypeScript and plugin suites against it (`ENGINES=docker` or `ENGINES=podman` to pick one; `SKIP_NATIVE_BUILD=1 SKIP_BINDING_BUILD=1` to reuse artifacts from `build_artifacts.sh`).
