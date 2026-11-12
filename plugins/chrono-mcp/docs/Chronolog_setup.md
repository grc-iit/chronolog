# ChronoLog Installation and Deployment Guide

This guide installs and starts a single-node ChronoLog for the MCP server: Steps 0 to 7 install and deploy it, and Step 8 tries it out by hand. The full documentation, including the multi-node and Docker setups, is at [chronolog.dev](https://chronolog.dev/docs/).

## Step 0: Prerequisites

Before you begin, ensure you have the following installed on your system:

* **Git** – Required for cloning the ChronoLog repository. Install with:

  ```bash
  sudo apt install git  # Debian/Ubuntu
  ```
* **g++** – The GNU C++ compiler is needed to build ChronoLog. Install with:

  ```bash
  sudo apt install g++  # Debian/Ubuntu
  ```

* **cmake** – The cmake is needed to build ChronoLog. Install with:

  ```bash
  sudo apt install cmake  # Debian/Ubuntu
  ```

* **gfortran** – Spack builds one of ChronoLog's build dependencies (py-scikit-build-core) only when a Fortran compiler is present. Install with:

  ```bash
  sudo apt install gfortran  # Debian/Ubuntu
  ```


Once these are installed, you're ready for the next step.

## Step 1: Clone the Repository

Open your terminal in the desired directory and run:

```bash
git clone -b develop https://github.com/grc-iit/ChronoLog.git
```

This command downloads the ChronoLog code to your local machine.

## Step 2: Install the Spack Package Manager

Spack builds ChronoLog's dependencies. ChronoLog 3.5.0 and later, and `develop`, need Spack v1.2.2:

```bash
git clone --depth 1 --branch v1.2.2 https://github.com/spack/spack.git
source /path-to-where-spack-was-cloned/spack/share/spack/setup-env.sh
```

Replace `/path-to-where-spack-was-cloned/` with your actual path.

ChronoLog 3.4.0 and older need Spack v0.21.2 instead: Spack v1.2.2 has no recipes for the versions their `spack.yaml` pins (it stops at `cmake@3.25.1 ^openssl@3.0`), and v0.21.2 cannot read the `spack.yaml` of 3.5.0. To build such a version, clone `--branch v0.21.2` into another directory and source its `setup-env.sh` in place of the v1.2.2 one. A 3.5.0 client cannot talk to 3.4.0 or older servers, or the other way round, so build every component and client from the same version.

## Step 3: Move to the Repository

The deploy scripts are in `tools/deploy/` and run from the repository root:

```bash
cd /path-to-repo
```

Ensure `/path-to-repo/` is the location where you cloned ChronoLog.

## Step 4: Build the Project

Build ChronoLog in Debug mode:

```bash
tools/deploy/local_single_user_deploy.sh -b -t Debug
```

The first build also builds the Spack environment in `.spack-env/`, which takes a while. The build tree is `~/chronolog-build/Debug/`.

## Step 5: Install the Project

Install ChronoLog locally:

```bash
tools/deploy/local_single_user_deploy.sh -i -t Debug
```

This installs ChronoLog in `~/chronolog-install/chronolog`. Pass `-I <dir>` to both the build and the install command to use another directory.

## Step 6: Deploy the Project

Start a visor, a keeper, a grapher and a player:

```bash
tools/deploy/local_single_user_deploy.sh -d -w ~/chronolog-install/chronolog
```

The script returns before the keeper has registered with the visor. Wait about 10 seconds before the first client connects, or `AcquireStory` can return `CL_ERR_NO_KEEPERS` (issue #742).

## Step 7: Check the Installation

Verify the deployment by checking running processes:

```bash
pgrep -af "chrono-(visor|keeper|grapher|player) --config"
```

You should see four processes, one for each component. Stop them with `tools/deploy/local_single_user_deploy.sh -s -w ~/chronolog-install/chronolog`.

**Python bindings.** The MCP server imports `py_chronolog_client`, which the install puts in `~/chronolog-install/chronolog/lib`. It is built for the Python of ChronoLog's Spack environment, `/path-to-repo/.spack-env/view/bin/python3` (Python 3.14 on the 3.5.0 stack, 3.11 on 3.4.0 and older), and no other Python can import it. Run the MCP server with that interpreter and put the library directory on `PYTHONPATH`:

```bash
export PYTHONPATH=$HOME/chronolog-install/chronolog/lib:$PYTHONPATH
/path-to-repo/.spack-env/view/bin/python3 -c "import py_chronolog_client"  # no output: it imports
```

With uv, pass the same interpreter with `--python /path-to-repo/.spack-env/view/bin/python3`.

## Step 8: Try the Client CLI

`chrono-client-cli` reads commands from stdin until `-disconnect`:

```bash
cd ~/chronolog-install/chronolog
tools/cli/chrono-client-cli -c conf/default-chrono-client-conf.json
```

For example, `-c MyChronicle`, then `-a -s MyChronicle MyStory`, `-w hello`, `-q -s MyChronicle MyStory` and `-disconnect`. The CLI prints the full list of commands when it starts. A tail read with `-p <n>` sees an event only once its chunk is sealed, about 30 seconds after it was written.

### Retrieving interactions

`retrieve_interaction` replays a story through ChronoPlayer with the Python client, so it needs no
separate reader. For that the plugin's client, made at the first tool call, listens for
ChronoPlayer's answers on this machine. The client library keeps one client per process, so start,
record and stop use the same one:

| Variable | Default | What |
|---|---|---|
| `CHRONO_QUERY_HOST` | `127.0.0.1` | This machine's address as ChronoPlayer reaches it. Set it when the player runs on another node. |
| `CHRONO_QUERY_PORT` | `5557` | The port to listen on. Another client on the same machine, a second MCP server included, needs a different one; a client whose port is taken crashes (issue #719). |
| `CHRONO_QUERY_PROVIDER_ID` | `57` | The provider id of that listener. |

These are the `ClientQueryService` settings of a client configuration. ChronoVisor keeps chronicles
and stories in memory only, so after it restarts `retrieve_interaction` creates the chronicle and
story again to read their archived events; retrieving a name nothing was written to leaves an empty
story behind.

Please follow all the steps carefully, feel free to make an issue if there's any problem setting up the chronolog.
