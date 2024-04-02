# ChronoLog

ChronoLog: A High-Performance Storage Infrastructure for Activity and Log Workloads (NSF CSSI 2104013)

## ChronoLog Project Synopsis

This project will design and implement ChronoLog, a distributed and tiered shared log storage ecosystem. ChronoLog uses
physical time to distribute log entries while providing total log ordering. It also utilizes multiple storage tiers to
elastically scale the log capacity (i.e., auto-tiering). ChronoLog will serve as a foundation for developing scalable
new plugins, including a SQL-like query engine for log data, a streaming processor leveraging the time-based data
distribution, a log-based key-value store, and a log-based TensorFlow module.

## Workloads and Applications

Modern applications spanning from Edge to High Performance Computing (HPC) systems, produce and process log data and
create a plethora of workload characteristics that rely on a common storage model: **the distributed shared log**.

![Log centric paradigm](/doc/images/log_centric_paradigm.svg)

## Features

![Feature matrix](/doc/images/feature-matrix.png)

## Checkout ChronoLog

To get started with ChronoLog, the first step involves cloning the repository to your system. To do so: 

```
git clone https://github.com/grc-iit/ChronoLog.git
```

## Installation and configuration

### Prerequisites: Spack

ChronoLog requires various packages managed by Spack. To ensure compatibility and stability, we recommend using Spack
version [`v0.21.2 (2024-03-01)`](https://github.com/spack/spack/releases/tag/v0.21.2). Follow the steps below to install
and configure Spack:

```
git clone --branch v0.21.2 https://github.com/spack/spack.git
source /path-to-where-spack-was-cloned/spack/share/spack/setup-env.sh
```

### Installing Dependencies

Currently, most of the dependencies are listed in `spack.yaml` and can be installed via Spack. `gcc` and `g++` will be
needed to build ChronoLog.

A Spack environment needs to be created and activated using the following commands. When the environment is activated, a
shell prompt `[ChronoLog]` will pop up.

```
cd ChronoLog
git switch develop
spack env activate -p .
spack install -v
```

:information_source: Installation can take > 30 minutes.

### Building ChronoLog

:exclamation: Ensure all build steps are performed within the activated Spack environment to allow CMake to locate
necessary dependencies:

```
// Build the environment
cd ChronoLog
git switch develop
mkdir build && cd build

// Build the project 
cmake ..
make all
```

Building ChronoLog generates the following executables:

- **Servers:** `chronovisor_server` for ChronoVisor and `chrono_keeper` for ChronoKeeper.
- **Client Test Cases:** Located in `test/integration/Client/`.
- **Admin Tool:** `client_admin` in `Client/ChronoAdmin` serves as a workload generator.

### Installing ChronoLog

From the build directory, execute

```
make install
```

to install executables and dependencies into the default install directory (**`~/chronolog`**).

## Configuration files

- **Default Configuration:** ChronoLog executables require a configuration file. Modify this template (`default_conf.json.in`) according to your preferences.
- **Installation Process:** The installation copies and renames default_conf.json.in to conf/default_conf.json in the
  installation directory by default. You can pass -DCMAKE_INSTALL_PREFIX=/new/installation/directory to CMake to change it.
- **Using Configuration:** Pass the configuration file to executables with `--config default_conf.json`.

------

# Coming soon ...

For more details about the ChronoLog project, please visit
our [website](https://www.chronolog.dev)

