# ChronoLog Installation and Deployment Guide

This guide walks you through steps 0 to 7 for installing and deploying ChronoLog. ([github.com](https://github.com/grc-iit/ChronoLog/wiki/Tutorial-1%3A-First-Steps-with-ChronoLog))

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


Once these are installed, you're ready for the next step.

## Step 1: Clone the Repository

Open your terminal in the desired directory and run:

```bash
git clone -b develop https://github.com/grc-iit/ChronoLog.git
```

This command downloads the ChronoLog code to your local machine.

## Step 2: Install the Spack Package Manager

Spack simplifies managing software builds. To install Spack:

```bash
git clone --branch v0.21.2 https://github.com/spack/spack.git
source /path-to-where-spack-was-cloned/spack/share/spack/setup-env.sh
```

Replace `/path-to-where-spack-was-cloned/` with your actual path.

## Step 3: Move to the Deploy Folder

Navigate to ChronoLog's deploy directory:

```bash
cd /path-to-repo/deploy
```

Ensure `/path-to-repo/` is the location where you cloned ChronoLog.

## Step 4: Build the Project

Build ChronoLog in Debug mode:

```bash
./local_single_user_deploy.sh -b -t Debug
```

This compiles the project and may take a few minutes.

## Step 5: Install the Project

Install ChronoLog locally:

```bash
./local_single_user_deploy.sh -i -w /home/$USER/chronolog/Debug
```

This sets up ChronoLog in `/home/$USER/chronolog/Debug`.

## Step 6: Deploy the Project

Deploy ChronoLog:

```bash
./local_single_user_deploy.sh -d -w /home/$USER/chronolog/Debug
```

ChronoLog is now running and ready for use.

## Step 7: Check the Installation

Verify the deployment by checking running processes:

```bash
pgrep -laf "chronovisor_server|chrono_grapher|chrono_keeper|chrono_player"
```

You should see four processes corresponding to ChronoLog components.


**Ensure Python bindings by adding the following to your shell configuration (e.g., `.bashrc` or `.zshrc`):**

```bash
export LD_LIBRARY_PATH=$HOME/chronolog/Debug/lib:$LD_LIBRARY_PATH
export PYTHONPATH=$HOME/chronolog/Debug/lib:$PYTHONPATH

ln -s /path/to/chronolog/lib/py_chronolog_client.[python-version-linux-version].so /path/to/chronolog/lib/py_chronolog_client.so
```

## Step 8: Launch the Interactive Client Admin

Let’s fire up the interactive client! Open your terminal, navigate to the /bin directory on the location where ChronoLog is installed, and run:

```bash
./client_admin -i -c ../conf/visor_conf.json
```

Feel free to test and explore the chronolog operations.

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