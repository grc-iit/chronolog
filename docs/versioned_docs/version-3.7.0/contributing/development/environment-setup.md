---
sidebar_position: 2
title: "Environment Setup"
---

import Tabs from '@theme/Tabs';
import TabItem from '@theme/TabItem';

# Environment Setup

This guide walks you through preparing your machine to build and develop ChronoLog. Choose the method that best fits your workflow:

<Tabs>
<TabItem value="source" label="Build from Source" default>

## Prerequisites

- **Operating System:** Linux (Ubuntu 20.04+, CentOS 7+, or equivalent)
- **C++17 Compiler:** GCC 9+ or Clang 10+
- **Git:** 2.x+
- **Python:** 3.6+ (required by Spack)
- **Fortran compiler:** `gfortran` (see below)

### Spack

ChronoLog requires various packages managed by Spack. Use Spack [v1.2.2 (2026-07-20)](https://github.com/spack/spack/releases/tag/v1.2.2).

Spack needs a few system packages, among them a Fortran compiler: Spack builds `py-scikit-build-core`, a build dependency of `py-pybind11`, only when one is installed. On Debian or Ubuntu:

```bash
sudo apt install file bzip2 ca-certificates g++ gcc gfortran git gzip lsb-release patch python3 tar unzip xz-utils zstd
```

Clone Spack v1.2.2:

```bash
git clone --depth 1 --branch v1.2.2 https://github.com/spack/spack.git
```

Load Spack into your shell environment:

```bash
source /path-to-where-spack-was-cloned/spack/share/spack/setup-env.sh
```

:::tip
Add the `source` command to your `~/.bashrc` (or `~/.zshrc`) so Spack is available in every new shell session.
:::

:::note ChronoLog 3.4.0 and older
ChronoLog 3.4.0 and older build with Spack [v0.21.2](https://github.com/spack/spack/releases/tag/v0.21.2), not v1.2.2: their `spack.yaml` pins versions that the recipes of v1.2.2 no longer have (concretizing fails on its `cmake@3.25.1 ^openssl@3.0`), and v0.21.2 cannot read the `spack.yaml` of 3.5.0. To build an older version, clone `--branch v0.21.2` into another directory and source its `setup-env.sh` instead, including in `~/.bashrc` if you put it there. 3.5.0 and older components cannot exchange RPCs, so do not mix the builds.
:::

## Checkout ChronoLog

Clone the ChronoLog repository:

```bash
git clone https://github.com/grc-iit/ChronoLog.git
```

## Installing Dependencies

Enter the repository and check out the development branch:

```bash
cd ChronoLog
```

```bash
git checkout develop
```

:::note
The `develop` branch contains the latest in-progress work. Use `main` for the current stable release.
:::

Activate the Spack environment defined in the repository:

```bash
spack env activate -p .
```

:::info
The repository's `spack.yaml` pins the package recipes to a commit of the `releases/v2026.10` branch of [spack-packages](https://github.com/spack/spack-packages) and takes Margo 0.24.4 from its own recipe in `tools/spack/`. Spack clones the pinned recipes into `.spack-env/` the first time the environment is used, so that step needs network access.
:::

Install all required dependencies:

```bash
spack install -v
```

:::info
Installation can take more than 30 minutes. All dependencies are defined in the repository's `spack.yaml`.
:::

## Verify the Environment

Confirm that Spack installed the dependencies and that CMake is available:

```bash
spack find
which cmake
```

`spack find` should list all installed packages. `which cmake` should point to the Spack-installed CMake inside the environment view.

## IDE Setup (Optional)

### CLion

1. Open the `ChronoLog` directory as a CMake project.
2. Go to **Settings → Build, Execution, Deployment → CMake**.
3. Set **CMake options** to:
   ```
   -DCMAKE_PREFIX_PATH=$(spack location -i cmake)/..
   ```
   Alternatively, launch CLion from a terminal where the Spack environment is already activated — CLion will inherit the environment variables and find all dependencies automatically.

### VS Code

1. Install the **CMake Tools** extension.
2. Open the `ChronoLog` directory.
3. In `.vscode/settings.json`, add:
   ```json
   {
     "cmake.configureArgs": [
       "-DCMAKE_PREFIX_PATH=${env:SPACK_ENV}/.spack-env/view"
     ]
   }
   ```
   Or simply open VS Code from a terminal with the Spack environment activated.

</TabItem>
<TabItem value="docker" label="Docker">

## Prerequisites

Ensure [Docker](https://docs.docker.com/get-docker/) is installed and the Docker daemon is running:

```bash
docker --version
```

## Pull the ChronoLog Dev Image

```bash
docker pull ghcr.io/grc-iit/chronolog-dev:develop
```

The image is automatically published on every push to the `develop` branch by the CI pipeline. It comes with:
- All Spack dependencies pre-installed
- A **Debug build** of ChronoLog compiled and installed at `/home/grc-iit/chronolog-install/chronolog`
- The full source tree at `/home/grc-iit/chronolog-src`
- `PATH`, `LD_LIBRARY_PATH`, and `CHRONOLOG_HOME` already configured

No additional setup is required to start building or running.

## Run the Container

```bash
docker run -it --rm ghcr.io/grc-iit/chronolog-dev:develop bash
```

This opens an interactive shell as the `grc-iit` user, starting in `/home/grc-iit/chronolog-src` with ChronoLog already built and installed.

## Mount Local Source (Recommended for Active Development)

To develop with your own local edits — edit on the host, build inside the container — bind-mount your local ChronoLog checkout over the baked-in source:

```bash
docker run -it --rm \
  -v $(pwd):/home/grc-iit/chronolog-src \
  ghcr.io/grc-iit/chronolog-dev:develop bash
```

Run this from the root of your cloned `ChronoLog` repository. Your local changes are immediately visible inside the container. After editing, run the build and install scripts to apply them (see [Building for Development](./building-for-development.md)).

</TabItem>
</Tabs>

## Next Steps

Your environment is ready. Head to [Building for Development](./building-for-development.md) for build, install, and run instructions.
