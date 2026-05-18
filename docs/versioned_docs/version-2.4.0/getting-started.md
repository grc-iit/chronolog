---
sidebar_position: 2
title: "Getting Started"
---

# Getting Started

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
