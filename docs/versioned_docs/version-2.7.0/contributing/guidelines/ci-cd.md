---
sidebar_position: 4
title: "CI/CD"
---

# CI/CD Pipeline

ChronoLog uses **GitHub Actions** with two workflows that must pass before a pull request can be merged.

## Clang Format Check

**File:** `.github/workflows/clang-format-check.yml`

| | |
|---|---|
| **Triggers** | Pull requests that modify C/C++ files (`.cpp`, `.h`, `.hpp`, `.cc`, `.cxx`, `.c`) or the format config |
| **Runner** | `ubuntu-24.04` |
| **Timeout** | 10 minutes |

This workflow installs clang-format-18 from the default Ubuntu repositories and checks only the files changed in the PR. If any file doesn't match the expected formatting:

1. A comment is posted on the PR listing affected files with fix instructions
2. A `clang-format-patches` artifact is uploaded containing individual and combined patch files (retained for 7 days)
3. The workflow exits with a failure status

See [Code Style](./code-style.md) for how to format code locally and set up the pre-commit hook.

## ChronoLog CI

**File:** `.github/workflows/ci.yml`

| | |
|---|---|
| **Triggers** | Pull requests to `develop`/`main`; pushes to `develop`/`main` (when Spack or Docker files change); manual dispatch |
| **Runner** | `ubuntu-24.04` |

This workflow has three sequential jobs:

### 1. `chronolog-base` (90 min timeout)

Builds a Docker base image containing all Spack dependencies. The image is tagged with a content hash of the Spack environment files and Docker configuration, so it is only rebuilt when dependencies actually change.

- Computes a hash of `spack.yaml`, `Client/spack.yaml`, and `.github/docker/**`
- Checks `ghcr.io` for an existing image with that tag
- If no cached image exists, builds and pushes a new one
- Outputs the full image name for downstream jobs

### 2. `local-deployment` (90 min timeout)

Builds ChronoLog from source inside the base image, installs it, and runs a single-node deployment test:

1. **Build & install** — CMake configure, build, and install into the container
2. **Deploy** — starts a single-node ChronoLog instance (ChronoVisor, ChronoKeeper, ChronoGrapher)
3. **Verify** — checks that all expected processes are running
4. **Stop & clean** — gracefully shuts down all services

The resulting image (with ChronoLog installed) is pushed to `ghcr.io` for the next job.

### 3. `distributed-deployment` (120 min timeout)

Runs a multi-node deployment test using Docker Compose:

1. **Network setup** — creates a Docker network with 7 containers (visor, keepers, graphers)
2. **SSH configuration** — sets up passwordless SSH between containers for MPI
3. **Distributed deploy** — starts ChronoLog services across multiple containers
4. **Integration test** — runs an MPI-based client performance test against the distributed cluster
5. **Teardown** — stops services and removes containers

## Build System

ChronoLog uses **CMake** as its build system and **Spack** for dependency management. The CI Docker images pre-install all Spack dependencies so that CI builds only need to compile ChronoLog itself. See [Building for Development](../development/building-for-development.md) for local build instructions.

## Troubleshooting CI Failures

### Format check failures

The PR comment includes the exact files and fix commands. You can also:

1. Download the `clang-format-patches` artifact from the workflow run
2. Apply it: `git apply format_fixes.patch`
3. Commit and push

### Build failures

- Check the CMake configure and build logs in the `local-deployment` job
- Verify your changes compile locally with the same CMake flags used in CI
- If Spack dependencies changed, the `chronolog-base` job logs show the full Spack install output

### Deployment failures

- The `local-deployment` job logs show process start/stop output — look for missing processes or early exits
- The `distributed-deployment` job logs include SSH setup, container networking, and MPI output
- Check that any new configuration options have valid defaults
