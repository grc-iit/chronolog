# ChronoLog

ChronoLog: A High-Performance Storage Infrastructure for Activity and Log Workloads (NSF CSSI 2104013)

## ChronoLog Project Synopsis

This project will design and implement ChronoLog, a distributed and tiered shared log storage ecosystem. ChronoLog uses physical time to distribute log entries while providing total log ordering. It also utilizes multiple storage tiers to elastically scale the log capacity (i.e., auto-tiering). ChronoLog will serve as a foundation for developing scalable new plugins, including a SQL-like query engine for log data, a streaming processor leveraging the time-based data distribution, a log-based key-value store, and a log-based TensorFlow module.

## Workloads and Applications

Modern applications spanning from Edge to High Performance Computing (HPC) systems, produce and process log data and create a plethora of workload characteristics that rely on a common storage model: **the distributed shared log**.

![Log centric paradigm](/doc/images/log_centric_paradigm.svg)

## Features

![Feature matrix](/doc/images/feature-matrix.png)

## Checkout ChronoLog

ChronoLog uses HCL internally. It is added to this repository as a submodule. Thus, you need to clone the submodules as well. You can do it using `git clone --recursive git@github.com:scs-lab/ChronoLog.git` to clone ChronoLog. Or you can run `git submodule update --init --recursive` once in `ChronoLog` directory after you clone the repository without `--recursive`. For following pulls, you can update the submodule using command `git pull --recurse-submodules`.

## Building

ChronoLog has a list of dependencies which can be solved by Spack packages. Thus, Spack needs to be installed and configured as the first step to build ChronoLog.

### Install Spack

Spack can be checked out with `git clone https://github.com/spack/spack.git`. It is assumed that Spack is stored at `~/Spack` for the following step. Spack needs to activated by running `source ~/Spack/spack/share/spack/setup-env.sh`.

### Install ChronoLog dependencies

Currently, most of the dependencies are listed in `spack.yaml` and can be installed via Spack. `gcc` and `g++` will be needed to build ChronoLog.

A Spack environment needs to be created and activated using the following commands. When the environment is activated, a shell prompt `[ChronoLog]` will pop up.
```
cd ChronoLog
git switch develop
spack env create -d .
spack env activate -p .
spack install -v
```
The installation may take some time (> 30 minutes) to finish.

### Build ChronoLog

**Please make sure all the building is carried out in the activated Spack environment.** Otherwise, CMake will not able to find the dependencies.

Two executables, `chronovisor_server` and `chrono_keeper`, will be built for ChronoVisor and ChronoKeeper, respectively. Multiple client test cases will be built in the `test/integration/Client/` directory. An additional command line client admin tool `client_admin` will be built in the `Client/ChronoAdmin` directory as a workload generator.
```
cd ChronoLog
git switch develop
mkdir build
cd build
cmake ..
make all
```

### Install ChronoLog

You can run `make install` in the build directory to install all generated executables along with their dependencies to the install directory (`~/chronolog` by default).

### Configuration files

All ChronoLog executables need a configuration file to run properly. The template file can be found in `default_conf.json.in`. You can modify it for your own preferences. The default installing process will copy and rename `default_conf.json.in` into `conf` under the install directory. You can pass it to the executables via command line argument `--config default_conf.json`.

ChronoLog will support sockets/TCP/verbs protocols using ofi transport. You can run command `margo-info` to check which transports and protocols are supported on your system.

------
# Coming soon ...

For more details about the ChronoLog project, please visit our website http://chronolog.dev.

