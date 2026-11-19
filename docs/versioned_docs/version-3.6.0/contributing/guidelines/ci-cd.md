---
sidebar_position: 4
title: "CI/CD"
---

# CI/CD Pipeline

ChronoLog uses **GitHub Actions**. Three workflows check every pull request to `develop` or `main`:

| Workflow | File | What it checks |
|---|---|---|
| Check Format | `.github/workflows/check-format.yml` | clang-format-18 on the C/C++ files the pull request changes |
| Local Pipeline | `.github/workflows/local-pipeline.yml` | Build, install, a single-host deployment and the CTest suite |
| Distributed Pipeline | `.github/workflows/distributed-pipeline.yml` | A 7-container deployment: cross-host RPC, MPI clients on several containers, replay from the archive and the keepers, and the archive on NFS |

The two pipelines share `.github/workflows/chronolog-base.yml`, which provides the base image. The integration scripts run every night instead, in the [Nightly Integration](#nightly-integration) workflow, which does not check pull requests. Other workflows publish images and releases; see [Other workflows](#other-workflows).

## Check Format

| | |
|---|---|
| **Triggers** | Pull requests that change `.cpp`, `.h`, `.hpp`, `.cc`, `.cxx` or `.c` files, the format config or this workflow; manual dispatch |
| **Runner** | `ubuntu-24.04` |
| **Timeout** | 10 minutes |

The workflow installs clang-format-18 from the Ubuntu repositories and checks every C/C++ file the pull request changes against its base, not only the files of the last commit. If a file is not formatted:

1. A comment on the pull request lists the files and the commands to fix them.
2. A `clang-format-patches` artifact holds a patch per file and a combined patch (kept for 7 days).
3. The workflow fails.

See [Code Style](./code-style.md) for formatting locally.

## Base image

`chronolog-base.yml` is a reusable workflow. It names the image `ghcr.io/grc-iit/chronolog-base:sha-<hash>`, where the hash covers `spack.yaml`, `client/spack.yaml`, the recipes under `tools/spack/` and `docker/dockerfile`, pulls it if it exists and builds it otherwise. A new image is pushed except from a fork's pull request. Changing a script under `docker/` does not change the hash and does not rebuild the image.

## Local Pipeline

| | |
|---|---|
| **Triggers** | Pull requests to `develop`/`main`; manual dispatch |
| **Runner** | `ubuntu-24.04` |
| **Timeout** | 90 minutes |

In one container from the base image:

1. **Build and install** a Release build with the test executables, through `tools/deploy/local_single_user_deploy.sh`.
2. **Verify the installation**: binaries, libraries and configuration templates are in place.
3. **Deploy** on one host: 5 keepers and 3 recording groups (`local_single_user_deploy.sh -d -k 5 -r 3`), and check that every process is running.
4. **Run CTest**, the whole set, against that deployment.
5. **Stop** the deployment, check that every process exited, and clean up.

Test failures do not fail the job: the job summary lists passed, failed and disabled tests and the names of the failed ones, and the `ctest-results-<branch>` artifact holds the JUnit file and the CTest log. The installed tree is uploaded as `chronolog-install-<branch>`, and the container is pushed as `ghcr.io/grc-iit/chronolog-ci:<branch>`.

## Distributed Pipeline

| | |
|---|---|
| **Triggers** | Pull requests to `develop`/`main`; manual dispatch |
| **Runner** | `ubuntu-24.04` |
| **Timeout** | 120 minutes |

The workflow starts 7 containers from the base image (`c1` to `c7`: 1 visor, 2 keepers, 2 graphers, 2 players), builds and installs ChronoLog in `c1`, and shares the install with the others through a volume. Its stages:

1. **Setup**: user and SSH keys in every container, the workspace copied in, the build in `c1`, and the `hosts_*` files.
2. **Deploy** with `tools/deploy/single_user_deploy.sh`, and check with `parallel-ssh` that each process runs on its container.
3. **Client performance test**: `chrono-bench` writes from MPI ranks spread over several containers.
4. **Read path test**: the distributed telemetry writer writes, `chrono-bench -l` reads the newest events back from the keepers (`playback`), and a replay reads them from the archive into a CSV file.
5. **Every recording group archived data**: each grapher wrote at least one non-empty window.
6. **Python tail read**: the writer on `c2`, the reader on `c3`.
7. **Keeper shutdown**: a keeper stopped with SIGTERM delivers the chunks it holds to the grapher before it exits.
8. **NFS pass**: the archive tests again, with the archive on an NFS export.

### The NFS pass

The runner exports a directory to `127.0.0.1` only, and every container mounts it once per *mount profile* (a name and NFS mount options). For each *pass* (a profile and the expected outcome), `tests/distributed/nfs/nfs_pass.sh` redeploys with the archive on that profile's mount and runs:

- **visibility**: two bursts written from one container and replayed from another for 190 s, a full chunk lifecycle plus the NFS cache time; every replay is compared with the events written.
- **destroy** (on one profile): destroy a story, create it again, and check that a replay returns only the new events, that the old files are gone while the story directory stays, and that the manifest holds the deletion.

The default passes:

| Pass | Mount | Expected |
|---|---|---|
| `default:clean:nowarn` | NFS 4.2, default options | Every replay complete; the destroy test passes; no mount warning from `deploy_cluster.sh` |
| `nocto:omission:warn` | `vers=3,nocto,acregmin=60,acregmax=60` | A negative control: some replay misses events while returning `CL_SUCCESS`, the last replay is complete again, and `deploy_cluster.sh` warns about the mount |

The step is `continue-on-error` for now: its result is in the job summary, not in the job status. When it fails, its logs (deploy logs, every replay, the destroy output and each pass's daemon logs) are uploaded as `nfs-pass-logs-<run>-<attempt>`. A manual dispatch can try other mount options with the inputs `nfs_profiles`, `nfs_passes`, `nfs_destroy_profile` and `nfs_cache_secs`.

The NFS stage takes about 14 minutes, most of it waiting: each test watches for 190 s, and the destroy test waits that long twice.

### Running it locally

`docker/local_distributed_ci.sh` runs the same stages against your working tree, with the same assertions:

```bash
docker/local_distributed_ci.sh -i IMAGE                 # 7 containers, as in CI
docker/local_distributed_ci.sh -i IMAGE -k 1 -g 1 -p 1  # 4 containers, faster
docker/local_distributed_ci.sh -i IMAGE --keep          # leave the containers up to debug
docker/local_distributed_ci.sh -i IMAGE --skip-build    # reuse the install in the volume
docker/local_distributed_ci.sh -i IMAGE --nfs           # also run the NFS pass
docker/local_distributed_ci.sh -i IMAGE --wal-restart   # also run stage 11c, the keeper WAL kill and restart
docker/local_distributed_ci.sh -i IMAGE --wal-restart-only  # stage 11c alone, after the setup stages
```

`IMAGE` is the base image above. `--nfs` needs sudo: it exports a directory (`/srv/chronolog-nfs-localci` by default) to `127.0.0.1`, turns file leases off while it runs, and undoes both on exit. `--nfs-only` skips stages 3 to 7 to try mount options, and `--nfs-profile NAME=OPTIONS`, `--nfs-pass PROFILE:clean|omission[:warn|nowarn]`, `--nfs-destroy-profile` and `--nfs-cache-secs` match the dispatch inputs. A full run with `--nfs` takes about 22 minutes.

Stage 11c redeploys with the keeper WAL on a volume in each keeper container, writes accepted and then durable events, kills the keeper that holds them with SIGKILL, restarts it with `--restart-keeper`, and checks that every acknowledged durable event is archived and replayed exactly once, that a replay misses at least one acknowledged accepted event (the negative control), and that the WAL segment is deleted after the grapher confirms. The Distributed Pipeline leaves it out.

Run it before a pull request that changes RPC endpoints, ports, configuration plumbing, the deploy scripts or the workflow itself, with `--nfs` for changes to the archive layout, the manifest, the player's archive reads or destroy, and with `--wal-restart` for changes to the keeper WAL, durable writes or a keeper restart.

## Nightly Integration

| | |
|---|---|
| **Triggers** | A schedule, every night at 07:00 UTC, against `develop`; manual dispatch against any branch, tag or commit; pull requests that change the workflow or `tests/integration/run_integration_tests.sh` |
| **Runner** | `ubuntu-24.04`, one per job |
| **Timeout** | 300 minutes (integration), 120 minutes (distributed WAL restart) |

`.github/workflows/nightly-integration.yml` runs the tests that take too long for every pull request. It does not gate a merge: a failure shows as a failed run on the Actions tab, and GitHub emails it to whoever last changed the schedule. GitHub runs a schedule, and offers the dispatch button, only from the copy of the workflow on the default branch, `main`, so the nightly run starts once the workflow is released.

Two jobs, each on its own runner:

1. **integration**: in one container from the base image, a Debug build, then every `tests/integration/*_test.sh` one at a time through `tests/integration/run_integration_tests.sh` (see [Running Tests](../development/running-tests.md#integration-scripts)). `keeper_restart_held_test.sh` runs over `ofi+sockets` only unless the dispatch input `protocols` says otherwise, and `keeper_wal_off_test.sh` builds its v3.3.0 keeper from the `v3.3.0` tag. The job summary has one row per script: pass, fail, error or timeout, its check count and its time. The `integration-logs-<run>-<attempt>` artifact has each script's output and the daemons' logs.
2. **distributed-wal-restart**: `docker/local_distributed_ci.sh --wal-restart-only` on the 7-container deployment of the Distributed Pipeline, which runs its stage 11c: a keeper killed with SIGKILL after durable writes, restarted, and checked to archive and replay every acknowledged durable event. Its log is the `wal-restart-log-<run>-<attempt>` artifact.

The dispatch inputs are `ref` (default `develop`), `scripts` (names separated by spaces; empty runs all) and `protocols`.

## Other workflows

| Workflow | Trigger | What it does |
|---|---|---|
| Publish Dev Image (`publish-dev-image.yml`) | Push to `develop` | Builds the current `develop` and pushes `ghcr.io/grc-iit/chronolog-dev:develop` |
| Create Release (`create-release.yml`) | A `v*.*.*` tag | Builds the Release image (`ghcr.io/grc-iit/chronolog:v<version>` and `:latest`) and the Debug image (`ghcr.io/grc-iit/chronolog-debug:v<version>`), tarballs, DEB and RPM packages, and publishes the GitHub release |
| Deploy Website (`deploy-website.yml`) | Push to `main` that changes `website/` or `docs/` | Builds the website and these docs and publishes them to GitHub Pages |
| Collect Traffic (`collect-traffic.yml`) | Weekly | Commits the repository's traffic statistics to the `repo-traffic-data` branch |
| Nightly Integration (`nightly-integration.yml`) | Nightly | The integration scripts and the distributed keeper WAL restart; see [above](#nightly-integration) |

## Troubleshooting CI Failures

### Format check

The pull-request comment lists the files and the fix commands. You can also download the `clang-format-patches` artifact and apply it with `git apply`.

### Build

- The CMake and compiler output is in the build step of either pipeline.
- If the Spack environment, a recipe under `tools/spack/` or `docker/dockerfile` changed, the `chronolog-base` job builds a new image; its log has the Spack output.

### Tests and deployment

- Local pipeline: the job summary lists the failed tests; the `ctest-results-<branch>` artifact has the log.
- Distributed pipeline: each stage's step prints its own output and the daemon logs it reads. To debug, run `docker/local_distributed_ci.sh --keep` and look inside the containers.
- Nightly integration: the job summary names each script's result; the `integration-logs-<run>-<attempt>` artifact has the script's output and the daemon logs under `<script>/monitor/`. To rerun one script, dispatch the workflow with `scripts` set to its name, or run it locally (see [Running Tests](../development/running-tests.md#integration-scripts)).
- New configuration options need defaults that work in both pipelines' deployments.
