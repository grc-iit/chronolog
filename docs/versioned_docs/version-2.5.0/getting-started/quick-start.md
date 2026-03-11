---
sidebar_position: 3
title: "Quick Start"
---

import Tabs from '@theme/Tabs';
import TabItem from '@theme/TabItem';

# Quick Start

Get ChronoLog running on your machine in minutes. Choose the method that best fits your environment:

<Tabs>
<TabItem value="source" label="Build from Source" default>

## Prerequisites

### Spack

ChronoLog requires various packages managed by Spack. We recommend using Spack version [v0.21.2 (2024-03-01)](https://github.com/spack/spack/releases/tag/v0.21.2):

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
```

```bash
git checkout v2.5.0
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
pgrep -la chrono
```

You should see `chrono-visor`, `chrono-keeper`, `chrono-grapher`, and `chrono-player` listed.

## Stop ChronoLog

To stop all ChronoLog services:

```bash
~/chronolog-install/chronolog/tools/deploy_local.sh --stop
```

To also remove generated logs and configuration artifacts:

```bash
~/chronolog-install/chronolog/tools/deploy_local.sh --clean
```

## Next Steps

For full deployment options and configuration, see the [Single Node Deployment](/docs/user-guide/deployment/single-node) guide.

</TabItem>
<TabItem value="tarball" label="Release Archive">

## Download the Tarball

Download the pre-built binary tarball for v2.5.0 from the [ChronoLog GitHub Releases](https://github.com/grc-iit/ChronoLog/releases) page:

```bash
wget https://github.com/grc-iit/ChronoLog/releases/download/v2.5.0/chronolog-2.5.0-linux-x86_64.tar.gz
```

## Extract the Archive

Extract the tarball:

```bash
tar -xzf chronolog-2.5.0-linux-x86_64.tar.gz
```

Enter the extracted directory:

```bash
cd chronolog-2.5.0
```

## Verify Executables

After extracting, the `bin/` directory should contain the following executables:

- `chrono-visor` — ChronoVisor server
- `chrono-keeper` — ChronoKeeper server
- `chrono-grapher` — ChronoGrapher server
- `chrono-player` — ChronoPlayer server
- `chrono-client-admin` — Admin/workload generator tool

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

From inside the extracted directory (`chronolog-2.5.0`), run the deployment script:

```bash
tools/deploy_local.sh --start
```

## Verify Deployment

Check that the ChronoLog processes are running:

```bash
pgrep -la chrono
```

You should see `chrono-visor`, `chrono-keeper`, `chrono-grapher`, and `chrono-player` listed.

## Stop ChronoLog

To stop all ChronoLog services:

```bash
tools/deploy_local.sh --stop
```

To also remove generated logs and configuration artifacts:

```bash
tools/deploy_local.sh --clean
```

## Next Steps

For full deployment options and configuration, see the [Single Node Deployment](/docs/user-guide/deployment/single-node) guide.

</TabItem>
<TabItem value="docker" label="Docker">

## Prerequisites

Ensure [Docker](https://docs.docker.com/get-docker/) is installed and the Docker daemon is running on your system.

## Pull the ChronoLog Image

```bash
docker pull gnosisrc/chronolog:latest
```

## Run the Container

```bash
docker run -it --rm gnosisrc/chronolog:latest bash
```

This opens an interactive shell inside the container with ChronoLog pre-installed.

## Deploy ChronoLog

Inside the container, run the single-node deployment script:

```bash
./local_single_user_deploy.sh --start --install-dir /home/user/chronolog
```

## Verify Deployment

Check that the ChronoLog processes are running:

```bash
pgrep -la chrono
```

You should see `chronovisor_server`, `chrono_keeper`, `chrono_grapher`, and `chrono_player` listed.

## Next Steps

For step-by-step Docker tutorials, see the [Single Node Tutorial](/docs/tutorials/docker-single-node/running-chronolog) and [Multi Node Tutorial](/docs/tutorials/docker-multi-node/running-chronolog).

</TabItem>
<TabItem value="spack" label="Spack">

:::info
Spack installation support for ChronoLog is coming soon. Once available, you will be able to install ChronoLog directly via:

```bash
spack install chronolog
```

In the meantime, use the **Build from Source** tab to install ChronoLog using Spack for dependency management.
:::

</TabItem>
</Tabs>
