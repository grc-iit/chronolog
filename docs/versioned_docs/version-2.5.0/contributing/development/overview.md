---
sidebar_position: 0
title: "Overview"
---

# Development Overview

The Development section covers everything you need to set up a working development environment, build ChronoLog from source, and run its test suite. Whether you are fixing a bug, adding a feature, or exploring the codebase, start here to get your toolchain in place.

## What's in this section

### Environment Setup

ChronoLog builds on Linux with a C++17 compiler and manages its dependency tree through Spack. This page walks through two setup paths: building from source with Spack v0.21.2 or using the pre-built Docker image with all dependencies included. It also covers optional IDE configuration for CLion and VS Code.

See [Environment Setup](./environment-setup.md) for the full setup instructions.

### Building for Development

Once your environment is ready, the developer deployment scripts handle the build-install-run cycle. This page documents the single-node (`local_single_user_deploy.sh`) and cluster (`single_user_deploy.sh`) scripts, their execution modes (build, install, start, stop, clean), and all available options including build type, directory paths, and process counts.

See [Building for Development](./building-for-development.md) for build workflows and script reference.

### Running Tests

ChronoLog's test suite spans unit, integration, communication, overhead, and system tests, with ~133 CTest entries in a debug build. This page catalogs every test, explains how to run subsets with CTest, and documents which tests require a live deployment (manual tests) or are temporarily disabled.

See [Running Tests](./running-tests.md) for the full test catalog and execution instructions.

## Typical workflow

1. **Set up your environment** — install dependencies via Spack or pull the Docker image
2. **Build in Debug mode** — use `local_single_user_deploy.sh --build --build-type Debug`
3. **Install** — use `local_single_user_deploy.sh --install`
4. **Run tests** — `ctest --output-on-failure -j$(nproc)` from the build directory
5. **Start ChronoLog** — use `local_single_user_deploy.sh --start` for manual/integration tests that need a live deployment
