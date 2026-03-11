---
sidebar_position: 2
title: "Getting Started"
---

import Tabs from '@theme/Tabs';
import TabItem from '@theme/TabItem';

# Getting Started

<Tabs>
<TabItem value="build-from-source" label="Build from Source" default>

## 1.1 Checkout ChronoLog

To get started with ChronoLog, the first step involves cloning the repository to your system. To do so:

```bash
git clone https://github.com/grc-iit/ChronoLog.git
```

## 1.2 Prerequisites

### 1.2.1 Spack

ChronoLog requires various packages managed by Spack. To ensure compatibility and stability, we recommend using Spack version [v0.21.2 (2024-03-01)](https://github.com/spack/spack/releases/tag/v0.21.2). Follow the steps below to install and configure Spack:

```bash
git clone --branch v0.21.2 https://github.com/spack/spack.git
source /path-to-where-spack-was-cloned/spack/share/spack/setup-env.sh
```

## 1.3 Installing Dependencies

Currently, most of the dependencies are listed in `spack.yaml` and can be installed via Spack. `gcc` and `g++` will be needed to build ChronoLog.

### 1.3.1 Setting Up the Spack Environment

A Spack environment needs to be created and activated using the following commands.

```bash
cd ChronoLog
git switch develop
spack env activate -p .
```

To check if the environment is activated the following can be executed:

```bash
spack env status
```

### 1.3.2 Dependency Installation Commands

If the environment is properly activated, it can be installed:

```bash
spack install -v
```

:::info
Installation can take more than 30 minutes.
:::

## 1.4 Building ChronoLog

### 1.4.1 Preparation

:::caution
Ensure (by using `spack env status`) all building steps are performed within the activated Spack environment to allow CMake to locate necessary dependencies.
:::

### 1.4.2 Build Commands

For building ChronoLog the following commands must be executed.

```bash
# Build the environment
cd ChronoLog
git switch develop
mkdir build && cd build

# Build the project
cmake ..
make all
```

## 1.5 Executable Files Description

Building ChronoLog generates the following executables:

- **Servers:** `chronovisor_server` for ChronoVisor, `chrono_keeper` for ChronoKeeper and `chrono_grapher` for ChronoGrapher
- **Client Test Cases:** Located in `test/integration/Client/`.
- **Admin Tool:** `client_admin` in `Client/ChronoAdmin` serves as a workload generator.

</TabItem>
<TabItem value="tarball" label="Tarball">

## 1. Download the Tarball

Download the latest pre-built binary tarball from the [ChronoLog GitHub Releases](https://github.com/grc-iit/ChronoLog/releases) page.

```bash
# Replace <version> with the actual release version, e.g. 2.5.0
wget https://github.com/grc-iit/ChronoLog/releases/download/v<version>/chronolog-v<version>-linux-x86_64.tar.gz
```

## 2. Extract the Archive

```bash
tar -xzf chronolog-v<version>-linux-x86_64.tar.gz
cd chronolog-v<version>-linux-x86_64
```

## 3. Verify Executables

After extracting, the `bin/` directory should contain the following executables:

- `chronovisor_server` — ChronoVisor server
- `chrono_keeper` — ChronoKeeper server
- `chrono_grapher` — ChronoGrapher server
- `chrono_player` — ChronoPlayer server
- `client_admin` — Admin/workload generator tool

```bash
ls bin/
```

## 4. Set Up Configuration

The default configuration file is located at `conf/default_conf.json`. Review and adjust it for your environment before starting ChronoLog.

```bash
# View the default configuration
cat conf/default_conf.json
```

Refer to the [Configuration](./configuration-usage.md) documentation for a full description of all available options.

</TabItem>
<TabItem value="docker" label="Docker">

## 1. Prerequisites

Ensure [Docker](https://docs.docker.com/get-docker/) is installed and the Docker daemon is running on your system.

## 2. Pull the ChronoLog Image

```bash
docker pull gnosisrc/chronolog:latest
```

## 3. Run the Container

```bash
docker run -it --rm gnosisrc/chronolog:latest bash
```

This opens an interactive shell inside the container with ChronoLog pre-installed.

## 4. Deploy ChronoLog

Inside the container, run the single-node deployment script:

```bash
./local_single_user_deploy.sh --start --install-dir /home/user/chronolog
```

## 5. Verify Deployment

Check that the ChronoLog processes are running:

```bash
pgrep -la chrono
```

You should see `chronovisor_server`, `chrono_keeper`, `chrono_grapher`, and `chrono_player` listed.

## 6. Next Steps

For advanced Docker usage including multi-node and distributed deployments, see the [Single Node Deployment](./deployment-single-node.md) guide.

</TabItem>
</Tabs>
