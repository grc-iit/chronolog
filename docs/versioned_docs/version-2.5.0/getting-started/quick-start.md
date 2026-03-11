---
sidebar_position: 3
title: "Quick Start"
---

import Tabs from '@theme/Tabs';
import TabItem from '@theme/TabItem';

# Quick Start

Get ChronoLog running on your machine in minutes. Choose the method that best fits your environment:

<Tabs>
<TabItem value="docker" label="Docker" default>

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
<TabItem value="tarball" label="Tarball">

## Download the Tarball

Download the latest pre-built binary tarball from the [ChronoLog GitHub Releases](https://github.com/grc-iit/ChronoLog/releases) page.

```bash
# Replace <version> with the actual release version, e.g. 2.5.0
wget https://github.com/grc-iit/ChronoLog/releases/download/v<version>/chronolog-v<version>-linux-x86_64.tar.gz
```

## Extract the Archive

```bash
tar -xzf chronolog-v<version>-linux-x86_64.tar.gz
cd chronolog-v<version>-linux-x86_64
```

## Verify Executables

After extracting, the `bin/` directory should contain the following executables:

- `chronovisor_server` — ChronoVisor server
- `chrono_keeper` — ChronoKeeper server
- `chrono_grapher` — ChronoGrapher server
- `chrono_player` — ChronoPlayer server
- `client_admin` — Admin/workload generator tool

```bash
ls bin/
```

## Set Up Configuration

The default configuration file is located at `conf/default_conf.json`. Review and adjust it for your environment before starting ChronoLog.

```bash
# View the default configuration
cat conf/default_conf.json
```

Refer to the [Configuration](/docs/user-guide/configuration/overview) documentation for a full description of all available options.

</TabItem>
<TabItem value="source" label="Build from Source">

## Checkout ChronoLog

```bash
git clone https://github.com/grc-iit/ChronoLog.git
```

## Prerequisites

### Spack

ChronoLog requires various packages managed by Spack. We recommend using Spack version [v0.21.2 (2024-03-01)](https://github.com/spack/spack/releases/tag/v0.21.2):

```bash
git clone --branch v0.21.2 https://github.com/spack/spack.git
source /path-to-where-spack-was-cloned/spack/share/spack/setup-env.sh
```

## Installing Dependencies

```bash
cd ChronoLog
git switch develop
spack env activate -p .
spack install -v
```

:::info
Installation can take more than 30 minutes.
:::

## Building ChronoLog

:::caution
Ensure (by using `spack env status`) all building steps are performed within the activated Spack environment.
:::

```bash
cd ChronoLog
git switch develop
mkdir build && cd build
cmake ..
make all
```

## Install

```bash
make install
```

This installs executables and dependencies into the default directory (`~/chronolog`).

</TabItem>
<TabItem value="native-linux" label="Native Linux">

For a full native Linux setup tutorial (without Docker), see the [Native Linux Tutorial](/docs/tutorials/native-linux/first-steps).

</TabItem>
</Tabs>
