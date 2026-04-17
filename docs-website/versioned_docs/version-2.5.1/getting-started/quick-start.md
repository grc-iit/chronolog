---
sidebar_position: 3
title: "Quick Start"
---

import Tabs from '@theme/Tabs';
import TabItem from '@theme/TabItem';

# Quick Start

Get ChronoLog running on your machine in minutes. Choose the method that best fits your environment:
- ChronoLog can be installed via five primary methods:
  - Release archive (tarball) — no build tools required, no source code either
  - RPM package (for RHEL, Fedora, and compatible distributions)
  - DEB package (for Debian, Ubuntu, and compatible distributions)
  - Source build using CMake and Make (for advanced users and developers)
  - Official Docker image (for containerized deployments with a pre-installed release build)
- Each method fully supports single-node or multi-node local and distributed development.
- See below for detailed, step-by-step instructions for each option.


<Tabs>
<TabItem value="tarball" label="Release Archive (tarball)" default>

## Download the Tarball

Download the pre-built binary tarball for v2.5.1 from the [ChronoLog GitHub Releases](https://github.com/grc-iit/ChronoLog/releases) page:

```bash
wget https://github.com/grc-iit/ChronoLog/releases/download/v2.5.1/chronolog-2.5.1-linux-x86_64.tar.gz
```

## Extract the Archive

Extract the tarball:

```bash
tar -xzf chronolog-2.5.1-linux-x86_64.tar.gz
```

Enter the extracted directory:

```bash
cd chronolog-2.5.1
```

## Verify Executables

After extracting, the `bin/` directory should contain the following executables:

- `chrono-visor` — ChronoVisor server
- `chrono-keeper` — ChronoKeeper server
- `chrono-grapher` — ChronoGrapher server
- `chrono-player` — ChronoPlayer server
- `chrono-client-admin` — CLI tool

List the contents of the `bin/` directory to confirm:

```bash
ls bin/
```

## Set Up Configuration

The default configuration file is located at `conf/default-chrono-conf.json`. Review and adjust it for your environment before starting ChronoLog.

View the default configuration:

```bash
cat conf/default-chrono-conf.json
```

Refer to the [Configuration](/docs/user-guide/configuration/overview) documentation for a full description of all available options.

## Start ChronoLog

From inside the extracted directory (`chronolog-2.5.1`), run the deployment script:

```bash
tools/deploy_local.sh --start
```

## Verify Deployment

Check that the ChronoLog processes are running:

```bash
pgrep -fla 'chrono-visor|chrono-keeper|chrono-grapher|chrono-player'
```

You should see `chrono-visor`, `chrono-keeper`, `chrono-grapher`, and `chrono-player` listed.

## Stop ChronoLog

To stop all ChronoLog services:

```bash
tools/deploy_local.sh --stop
```

To also remove generated logs, configuration artifacts and stored data:

```bash
tools/deploy_local.sh --clean
```

## Next Steps

For full deployment options and configuration, see the [Single Node Deployment](/docs/user-guide/deployment/single-node) guide.

</TabItem>

<TabItem value="rpm" label="RPM Package">

## Prerequisites

Ensure you have `rpm` and either `yum` or `dnf` available (RHEL, Fedora, Rocky Linux, AlmaLinux, and compatible distributions).

## Download the RPM Package

Download the RPM package for v2.5.1 from the [ChronoLog GitHub Releases](https://github.com/grc-iit/ChronoLog/releases/tag/v2.5.1) page:

```bash
wget https://github.com/grc-iit/ChronoLog/releases/download/v2.5.1/chronolog-2.5.1-linux-x86_64.rpm
```

## Install the Package

Install ChronoLog using your package manager:

```bash
# With dnf (Fedora, RHEL 8+, Rocky, Alma)
sudo dnf install ./chronolog-2.5.1-linux-x86_64.rpm

# Or with yum (RHEL 7, CentOS)
sudo yum install ./chronolog-2.5.1-linux-x86_64.rpm
```

## Verify Executables

After installation, the ChronoLog executables are available system-wide:

- `chrono-visor` — ChronoVisor server
- `chrono-keeper` — ChronoKeeper server
- `chrono-grapher` — ChronoGrapher server
- `chrono-player` — ChronoPlayer server
- `chrono-client-admin` — CLI tool

Confirm the installation:

```bash
which chrono-visor
```

## Set Up Configuration

The default configuration file is installed at `/etc/chronolog/default-chrono-conf.json`. Review and adjust it for your environment before starting ChronoLog.

View the default configuration:

```bash
cat /etc/chronolog/default-chrono-conf.json
```

Refer to the [Configuration](/docs/user-guide/configuration/overview) documentation for a full description of all available options.

## Start ChronoLog

Run the deployment script:

```bash
/usr/share/chronolog/tools/deploy_local.sh --start
```

## Verify Deployment

Check that the ChronoLog processes are running:

```bash
pgrep -fla 'chrono-visor|chrono-keeper|chrono-grapher|chrono-player'
```

You should see `chrono-visor`, `chrono-keeper`, `chrono-grapher`, and `chrono-player` listed.

## Stop ChronoLog

To stop all ChronoLog services:

```bash
/usr/share/chronolog/tools/deploy_local.sh --stop
```

To also remove generated logs, configuration artifacts and stored data:

```bash
/usr/share/chronolog/tools/deploy_local.sh --clean
```

## Next Steps

For full deployment options and configuration, see the [Single Node Deployment](/docs/user-guide/deployment/single-node) guide.

</TabItem>

<TabItem value="deb" label="DEB Package">

## Prerequisites

Ensure you have `apt` available (Debian, Ubuntu, and compatible distributions).

## Download the DEB Package

Download the DEB package for v2.5.1 from the [ChronoLog GitHub Releases](https://github.com/grc-iit/ChronoLog/releases/tag/v2.5.1) page:

```bash
wget https://github.com/grc-iit/ChronoLog/releases/download/v2.5.1/chronolog-2.5.1-linux-x86_64.deb
```

## Install the Package

Install ChronoLog using `apt`:

```bash
sudo apt install ./chronolog-2.5.1-linux-x86_64.deb
```

## Verify Executables

After installation, the ChronoLog executables are available system-wide:

- `chrono-visor` — ChronoVisor server
- `chrono-keeper` — ChronoKeeper server
- `chrono-grapher` — ChronoGrapher server
- `chrono-player` — ChronoPlayer server
- `chrono-client-admin` — CLI tool

Confirm the installation:

```bash
which chrono-visor
```

## Set Up Configuration

The default configuration file is installed at `/etc/chronolog/default-chrono-conf.json`. Review and adjust it for your environment before starting ChronoLog.

View the default configuration:

```bash
cat /etc/chronolog/default-chrono-conf.json
```

Refer to the [Configuration](/docs/user-guide/configuration/overview) documentation for a full description of all available options.

## Start ChronoLog

Run the deployment script:

```bash
/usr/share/chronolog/tools/deploy_local.sh --start
```

## Verify Deployment

Check that the ChronoLog processes are running:

```bash
pgrep -fla 'chrono-visor|chrono-keeper|chrono-grapher|chrono-player'
```

You should see `chrono-visor`, `chrono-keeper`, `chrono-grapher`, and `chrono-player` listed.

## Stop ChronoLog

To stop all ChronoLog services:

```bash
/usr/share/chronolog/tools/deploy_local.sh --stop
```

To also remove generated logs, configuration artifacts and stored data:

```bash
/usr/share/chronolog/tools/deploy_local.sh --clean
```

## Next Steps

For full deployment options and configuration, see the [Single Node Deployment](/docs/user-guide/deployment/single-node) guide.

</TabItem>

<TabItem value="source" label="Build from Source">

## Prerequisites

### Spack

ChronoLog requires various packages managed by Spack:

:::info
Spack v0.21.2 is required due to an incompatibility between newer Spack versions and spdlog. This will be resolved in a future release.
:::

Clone Spack at the required version:

```bash
git clone --branch v0.21.2 https://github.com/spack/spack.git
```

Load Spack into your shell environment:

```bash
source /path-to-where-spack-was-cloned/spack/share/spack/setup-env.sh
```

## Checkout ChronoLog

Clone the ChronoLog repository:

```bash
git clone https://github.com/grc-iit/ChronoLog.git
```

## Installing Dependencies

Enter the repository and check out the stable release:

```bash
cd ChronoLog
git checkout v2.5.1
```

Activate the Spack environment defined in the repository:

```bash
spack env activate -p .
```

Install all required dependencies:

```bash
spack install -v
```

:::info
Installation can take more than 30 minutes.
:::

## Building ChronoLog

Create and enter the build directory:

```bash
mkdir build && cd build
```

Configure the build with CMake:

```bash
# Use Debug instead of Release for a debug build
cmake -DCMAKE_BUILD_TYPE=Release ..
```

Compile ChronoLog:

```bash
make all
```

## Install

Install ChronoLog to the default directory:

```bash
make install
```

This installs executables, libraries, configuration, and deploy scripts into `~/chronolog-install/chronolog/`.

## Start ChronoLog

Run the deployment script from the install tree:

```bash
~/chronolog-install/chronolog/tools/deploy_local.sh --start
```

## Verify Deployment

Check that the ChronoLog processes are running:

```bash
pgrep -fla 'chrono-visor|chrono-keeper|chrono-grapher|chrono-player'
```

You should see `chrono-visor`, `chrono-keeper`, `chrono-grapher`, and `chrono-player` listed.

## Stop ChronoLog

To stop all ChronoLog services:

```bash
~/chronolog-install/chronolog/tools/deploy_local.sh --stop
```

To also remove generated logs, configuration artifacts and stored data:

```bash
~/chronolog-install/chronolog/tools/deploy_local.sh --clean
```

## Next Steps

For full deployment options and configuration, see the [Single Node Deployment](/docs/user-guide/deployment/single-node) guide.

</TabItem>

<TabItem value="docker" label="Docker">

## Prerequisites

Ensure [Docker](https://docs.docker.com/get-docker/) is installed and the Docker daemon is running on your system.

## Pull the ChronoLog Image

Pull the official ChronoLog image from GitHub Container Registry:

```bash
docker pull ghcr.io/grc-iit/chronolog:v2.5.1
```

## Run the Container

```bash
docker run -it --rm ghcr.io/grc-iit/chronolog:v2.5.1 bash
```

This opens an interactive shell inside the container with ChronoLog pre-installed. The working directory is automatically set to the ChronoLog installation (`$CHRONOLOG_HOME`).

## Verify Executables

After entering the container, the `bin/` directory should contain the following executables:

- `chrono-visor` — ChronoVisor server
- `chrono-keeper` — ChronoKeeper server
- `chrono-grapher` — ChronoGrapher server
- `chrono-player` — ChronoPlayer server
- `chrono-client-admin` — CLI tool

List the contents of the `bin/` directory to confirm:

```bash
ls bin/
```

## Set Up Configuration

The default configuration file is located at `conf/default-chrono-conf.json`. Review and adjust it for your environment before starting ChronoLog.

View the default configuration:

```bash
cat conf/default-chrono-conf.json
```

Refer to the [Configuration](/docs/user-guide/configuration/overview) documentation for a full description of all available options.

## Start ChronoLog

Inside the container, run the deployment script:

```bash
tools/deploy_local.sh --start
```

## Verify Deployment

Check that the ChronoLog processes are running:

```bash
pgrep -fla 'chrono-visor|chrono-keeper|chrono-grapher|chrono-player'
```

You should see `chrono-visor`, `chrono-keeper`, `chrono-grapher`, and `chrono-player` listed.

## Stop ChronoLog

To stop all ChronoLog services:

```bash
tools/deploy_local.sh --stop
```

To also remove generated logs, configuration artifacts and stored data:

```bash
tools/deploy_local.sh --clean
```

## Next Steps

For step-by-step Docker tutorials, see the [Single Node Tutorial](/docs/tutorials/docker-single-node/running-chronolog) and [Multi Node Tutorial](/docs/tutorials/docker-multi-node/running-chronolog).

</TabItem>
</Tabs>
