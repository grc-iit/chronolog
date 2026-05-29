---
sidebar_position: 3
title: "Client Configuration"
---

# Client Configuration

The ChronoLog client uses a separate JSON configuration file that is independent of the server-side `default_conf.json`. This file is passed to client applications via the `--config` flag and tells the client how to locate and connect to a running ChronoVisor.

## Passing the Config File

```bash
./my_chronolog_app --config /path/to/default_client_conf.json
```

## File Structure

The entire client configuration lives under a top-level `chrono_client` key:

```json
{
  "chrono_client": {
    "VisorClientPortalService": { ... },
    "ClientQueryService": { ... },
    "Monitoring": { ... }
  }
}
```

## Full Default Configuration

```json
{
  "chrono_client": {
    "VisorClientPortalService": {
      "rpc": {
        "protocol_conf": "ofi+sockets",
        "service_ip": "127.0.0.1",
        "service_base_port": 5555,
        "service_provider_id": 55
      }
    },
    "ClientQueryService": {
      "rpc": {
        "protocol_conf": "ofi+sockets",
        "service_ip": "127.0.0.1",
        "service_base_port": 5557,
        "service_provider_id": 57
      }
    },
    "Monitoring": {
      "monitor": {
        "type": "file",
        "file": "chrono-client.log",
        "level": "debug",
        "name": "ChronoClient",
        "filesize": 1048576,
        "filenum": 3,
        "flushlevel": "warning"
      }
    }
  }
}
```

## Field Reference

### `VisorClientPortalService`

Defines how the client connects to ChronoVisor's client-facing portal. **These values must match the `VisorClientPortalService` block in the ChronoVisor's server configuration.**

| Field | Default | Description |
|---|---|---|
| `protocol_conf` | `"ofi+sockets"` | Thallium transport protocol. See [Network & RPC](./network-and-rpc.md) for options. |
| `service_ip` | `"127.0.0.1"` | IP address of the host running ChronoVisor. Change to the visor's actual IP for remote connections. |
| `service_base_port` | `5555` | Port on which ChronoVisor listens for client connections. |
| `service_provider_id` | `55` | Thallium provider ID. Must match the visor's configured value. |

### `ClientQueryService`

Configures the client's local endpoint used for query responses from ChronoPlayer.

| Field | Default | Description |
|---|---|---|
| `protocol_conf` | `"ofi+sockets"` | Transport protocol (must match the rest of the deployment). |
| `service_ip` | `"127.0.0.1"` | IP address the client binds for receiving query responses. |
| `service_base_port` | `5557` | Port the client listens on for playback query responses. |
| `service_provider_id` | `57` | Thallium provider ID for this service endpoint. |

### `Monitoring`

Controls client-side logging. Uses the same structure as the server-side monitoring blocks.

| Field | Default | Description |
|---|---|---|
| `type` | `"file"` | Log sink type. Currently only `"file"` is supported. |
| `file` | `"chrono-client.log"` | Log file name (written to the working directory). |
| `level` | `"debug"` | Minimum log level to record: `"debug"`, `"info"`, `"warning"`, `"error"`. |
| `name` | `"ChronoClient"` | Logger name that appears in log entries. |
| `filesize` | `1048576` | Maximum log file size in bytes (1 MB) before rotation. |
| `filenum` | `3` | Number of rotated log files to retain. |
| `flushlevel` | `"warning"` | Minimum level at which log entries are flushed to disk immediately. |

## Connecting to a Remote ChronoVisor

For deployments where the client and ChronoVisor run on different hosts, update `service_ip` in `VisorClientPortalService` to point at the visor's host:

```json
"VisorClientPortalService": {
  "rpc": {
    "protocol_conf": "ofi+sockets",
    "service_ip": "192.168.1.10",
    "service_base_port": 5555,
    "service_provider_id": 55
  }
}
```

Ensure the `protocol_conf`, `service_base_port`, and `service_provider_id` match exactly what is set in the server's `chrono_visor.VisorClientPortalService.rpc` block. A mismatch will cause the client to fail to connect.
