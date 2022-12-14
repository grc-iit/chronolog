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

### Install ChronoLog dependencies using Spack environment

Currently, ChronoLog uses the same dependencies as HCL. Thus, we can follow the dependency description file at `external_libs/hcl/Spack.yaml` to install dependecies for ChronoLog.

A Spack environment needs to be created and activated using the following commands. When the environment is activated, a shell prompt `[dependencies]` wil l pop up.
```
cd external_libs/hcl
mkdir dependencies
cd dependencies
cp ../Spack.yaml .
vim Spack.yaml # Update the version of gcc and cmake to the versions on your testbed
spack env create -d .
spack env activate -p .
spack install -v
```
The installation may take some time (> 30 minutes) to finish.

### Build ChronoLog

**Please make sure all the building is carried out in the activated Spack environment.** Otherwise, CMake will not able to find the dependencies.

Three tests can be built for not to have a mini testbed. `chronovisor_server_test` is for the ChronoVisor. `chronolog_client_lib_connect_rpc_test` and `chronolog_client_lib_metadata_rpc_test` are two client apps to test the connection/disconnection and metadata operations (e.g., Chronicle and Story management) functionalities, respectively.
```
cd ChronoLog
git switch develop
mkdir build
cd build
cmake ..
make chronovisor_server_test chronolog_client_lib_connect_rpc_test chronolog_client_lib_metadata_rpc_test
```

### Run mini tests

All tests require a `server_list` file to be in the same directory of the executable to run which should be generated after building automatically. Just in case the automated generation failed, a simple line of `localhost` in the file should suffice.

------
# Coming soon ...

For more details about the ChronoLog project, please visit our website http://www.cs.iit.edu/~scs/assets/projects/ChronoLog/ChronoLog.html.

