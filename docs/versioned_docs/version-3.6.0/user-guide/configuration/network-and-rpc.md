---
sidebar_position: 4
title: "Network & RPC"
---

# Network & RPC Configuration

ChronoLog includes built-in scripts installed under `tools/` under the installation directory to help deploy ChronoLog in both local and distributed setups. The remaining sections on this page describe manual network and RPC configuration.

ChronoLog uses [Thallium](https://mochi.readthedocs.io/en/latest/thallium.html) (a Mochi RPC framework built on Mercury) for all inter-component communication. Every service endpoint in the configuration follows the same `rpc` block structure.

## RPC Block Structure

Each service in `default_conf.json` contains an `rpc` sub-block:

```json
"SomeService": {
  "rpc": {
    "protocol_conf": "ofi+sockets",
    "service_ip": "127.0.0.1",
    "service_base_port": 5555,
    "service_provider_id": 55
  }
}
```

| Field | Description |
|---|---|
| `protocol_conf` | Thallium/Mercury transport string. Controls the network layer used for RPC. |
| `service_ip` | IP address this service binds to (servers) or connects to (clients). |
| `service_base_port` | TCP/UDP port number for the service. |
| `service_provider_id` | Thallium provider ID, used to multiplex multiple services on the same address. Must be unique per address. |

## Protocol Options

The transport is set via `protocol_conf`. ChronoLog passes the string to Mercury as the address prefix (`<protocol_conf>://<service_ip>:<service_base_port>`), so any transport that the installed Mercury and libfabric support can be named. These are the ones ChronoLog is run with:

| `protocol_conf` value | Use case |
|---|---|
| `"ofi+sockets"` | TCP through libfabric's sockets provider. Works everywhere, no special hardware required. The template's value, and what CI uses. |
| `"ofi+tcp"` | TCP through libfabric's tcp provider. Known problem: when a keeper dies, an accepted `log_event` to it and ChronoVisor's start-recording call to it wait with no deadline, so `keeper_restart_held_test.sh` passes 8 of its 11 checks on `ofi+tcp` and the nightly run leaves it out. |
| `"ofi+verbs"` | RDMA through libfabric's verbs provider, on InfiniBand or RoCE NICs. |

:::note
Each service has its own `protocol_conf`. A service and every component that connects to it must use the same value. The template uses `ofi+sockets` everywhere; running different transports on different services is not covered by CI.
:::

## Default Port Assignments

The table below lists the default ports for every service across all ChronoLog components. These must be consistent between the component that **listens** on a port and the component that **connects** to it.

| Component | Service | Default Port | Provider ID | Connects from |
|---|---|---|---|---|
| **ChronoVisor** | `VisorClientPortalService` | 5555 | 55 | ChronoClient |
| **ChronoVisor** | `VisorKeeperRegistryService` | 8888 | 88 | ChronoKeeper, ChronoGrapher, ChronoPlayer |
| **ChronoKeeper** | `KeeperRecordingService` | 6666 | 66 | ChronoClient (event logging & tail reads) |
| **ChronoKeeper** | `KeeperDataStoreAdminService` | 7777 | 77 | ChronoVisor (admin) |
| **ChronoKeeper** | `KeeperGrapherDrainService` | 3333 | 33 | ChronoGrapher |
| **ChronoGrapher** | `KeeperGrapherDrainService` | 3333 | 33 | ChronoKeeper |
| **ChronoGrapher** | `DataStoreAdminService` | 4444 | 44 | ChronoVisor (admin) |
| **ChronoPlayer** | `PlayerStoreAdminService` | 2222 | 22 | ChronoVisor (admin) |
| **ChronoPlayer** | `PlaybackQueryService` | 2525 | 25 | ChronoClient (queries) |
| **ChronoPlayer** | `PlayerRecordingService` | 7878 | 78 | ChronoKeeper (sealed chunks) |
| **ChronoClient** | `VisorClientPortalService` | 5555 | 55 | → ChronoVisor |
| **ChronoClient** | `ClientQueryService` | 5557 | 57 | ← ChronoPlayer (responses) |

## Multi-Node Setup

In single-node deployments, every `service_ip` defaults to `127.0.0.1`. For distributed deployments, each component's `service_ip` must be set to the actual IP address of the host it runs on.

**Example: three-node deployment**

| Host | IP | Components |
|---|---|---|
| `node01` | `10.0.0.1` | ChronoVisor |
| `node02` | `10.0.0.2` | ChronoKeeper + ChronoGrapher |
| `node03` | `10.0.0.3` | ChronoPlayer |

On `node01` (`chrono_visor` block):
```json
"VisorClientPortalService": {
  "rpc": { "service_ip": "10.0.0.1", "service_base_port": 5555, ... }
},
"VisorKeeperRegistryService": {
  "rpc": { "service_ip": "10.0.0.1", "service_base_port": 8888, ... }
}
```

On `node02` (`chrono_keeper` and `chrono_grapher` blocks), the `VisorKeeperRegistryService` entry points **to** the visor:
```json
"VisorKeeperRegistryService": {
  "rpc": { "service_ip": "10.0.0.1", "service_base_port": 8888, ... }
},
"KeeperRecordingService": {
  "rpc": { "service_ip": "10.0.0.2", "service_base_port": 6666, ... }
}
```

On the client host, `VisorClientPortalService.service_ip` must point to `node01`:
```json
"VisorClientPortalService": {
  "rpc": { "service_ip": "10.0.0.1", "service_base_port": 5555, ... }
}
```

**Firewall**: ensure all ports listed in the table above are open between the relevant hosts. For `ofi+sockets`, all communication is TCP; the port numbers are exactly the `service_base_port` values.
