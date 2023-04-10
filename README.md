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

Currently, most of the dependencies are listed in `spack.yaml` and can be installed via Spack.

A Spack environment needs to be created and activated using the following commands. When the environment is activated, a shell prompt `[ChronoLog]` will pop up.
```
cd ChronoLog
git switch develop
spack env create -d .
spack env activate -p .
spack install -v
```
The installation may take some time (> 30 minutes) to finish.

Additionally, `rapidjson` is needed to parse the JSON configuration file. You can install it using command `sudo apt install rapidjson-dev` in Ubuntu.

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

### Configuration files

All ChronoLog executables share one unified configuration file. The template file can be found in `test/default_conf.json.in`. You can modify it for your own preferences. By default, all existing mini tests expect a configuration file `default_conf.json` in the same directory it launches. The default building process will copy and rename `test/default_conf.json.in` to achieve that. If you want to change the default configurations, you can edit the template file and rebuild the targets, or directly edit the file in the target directory.

ChronoLog will support sockets/TCP/verbs protocols using ofi transport. You can run command `margo-info` to check which transports and protocols are supported on your system.

------
# Coming soon ...

For more details about the ChronoLog project, please visit our website http://www.cs.iit.edu/~scs/assets/projects/ChronoLog/ChronoLog.html.

