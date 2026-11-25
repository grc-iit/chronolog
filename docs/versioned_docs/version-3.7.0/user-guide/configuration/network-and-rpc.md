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
    "protocol_conf": "ofi+tcp",
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
| `"ofi+tcp"` | TCP through libfabric's tcp provider. Works everywhere, no special hardware required. The template's value since 3.6.0, and what CI uses. |
| `"ofi+sockets"` | TCP through libfabric's sockets provider, the template's value up to 3.5.0. It still works and the tests run on it, but libfabric deprecates this provider, and on it a reply that arrives after its call's deadline keeps the receiving process's progress thread at 100% CPU for the rest of its life. Prefer `ofi+tcp`. |
| `"ofi+verbs"` | RDMA through libfabric's verbs provider, on InfiniBand or RoCE NICs. |

:::note
Each service has its own `protocol_conf`. A service and every component that connects to it must use the same value. The template uses `ofi+tcp` everywhere; running different transports on different services is not covered by CI.
:::

### Choosing the interface for `ofi+tcp`

libfabric's tcp provider binds the network interface named by the `FI_TCP_IFACE` environment variable, or one it picks itself when the variable is unset. Its own pick is not always the interface that carries the configured `service_ip`:

- **One host, every `service_ip` is `127.0.0.1`:** set `FI_TCP_IFACE=lo` for the daemons, the deploy scripts' stop controller and the clients. `deploy_local.sh` exports it for the processes it starts when the conf uses `ofi+tcp`, every `service_ip` is loopback and the variable is not already set. Without it, a reply can be sent from another interface and time out; in testing this made a stop wait its whole budget.
- **Several hosts or containers:** set it to the interface that carries the configured addresses, for example `FI_TCP_IFACE=eth0` in the distributed CI containers. Never carry `lo` into a distributed deployment.

## RPC Deadlines

Since 3.6.0 every RPC that a client or a server component makes has a deadline. Each component reads its budgets from an `RpcPolicy` block at the top of its section (`chrono_client`, `chrono_visor`, `chrono_keeper`, `chrono_grapher`, `chrono_player`); the shipped templates set the same values everywhere:

```json
"RpcPolicy": {
  "append_ms": 5000,
  "control_ms": 5000,
  "tail_ms": 5000,
  "replay_ms": 180000,
  "attempt_ms": 5000,
  "startup_ms": 30000,
  "shutdown_ms": 30000,
  "transfer_ms": 30000
}
```

| Key | Default (ms) | Bounds |
|---|---|---|
| `append_ms` | 5000 | One accepted `log_event` / `append_event` |
| `control_ms` | 5000 | One metadata call: connect, disconnect, chronicle and story create, acquire, release and destroy, show; and ChronoVisor's start and stop of a story on its components |
| `tail_ms` | 5000 | One tail read (`StoryHandle::playback`), both its phases |
| `replay_ms` | 180000 | One `ReplayStory`, including the player's answer |
| `attempt_ms` | 5000 | The cap on any single RPC attempt inside an operation; a component's answer to another also uses it |
| `startup_ms` | 30000 | A server component's startup: its RPC services, its registration with ChronoVisor and, on a keeper, the WAL replay |
| `shutdown_ms` | 30000 | One stop attempt of a component |
| `transfer_ms` | 30000 | One chunk or replay transfer (the bulk pull and its answer) |

The block and each key are optional; a missing key takes the default above. A value must be a positive integer no larger than the steady clock can hold, or the component refuses its configuration. An operation's budget starts when the call is made and covers lookup, serialization, waiting for a free RPC slot and the wait for the answer; each attempt within it is also capped at `attempt_ms`. Durable writes keep their own budget, `attempts × timeout_ms` from `LogEventOptions`.

Each component runs a fixed number of RPC slots (the client two, ChronoVisor's admin calls 32). A call that finds every slot held, and no slot that frees within 50 µs, fails at once with `CL_ERR_RPC_CAPACITY` and sends nothing. A slot whose call got no answer stays held until Mercury is done with it. See [Error Codes](../../client/cpp/error-codes.md) for what each new result means to a caller.

:::tip
On busy hosts, for example with MPI ranks that busy-poll on the same cores as the daemons, a control call can run out of its 5 s budget and fail with `CL_ERR_OUTCOME_UNCONFIRMED`. Give the daemons cores of their own, or leave them headroom; raising `control_ms` and `attempt_ms` helps less than freeing CPU.
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

**Firewall**: ensure all ports listed in the table above are open between the relevant hosts. For `ofi+tcp` and `ofi+sockets`, all communication is TCP; the port numbers are exactly the `service_base_port` values.
