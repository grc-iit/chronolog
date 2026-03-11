---
sidebar_position: 2
title: "Pre-Built Binaries"
---

# Pre-Built Binaries

Download and install ChronoLog from pre-built binary tarballs.

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

Refer to the [Configuration Overview](/docs/user-guide/configuration/overview) documentation for a full description of all available options.
