---
sidebar_position: 2
title: "Troubleshooting"
---

# Troubleshooting

## Services Fail to Start

**Check the launch logs first.** Each component's stdout/stderr is captured to `<work-dir>/monitor/chrono-<component>.launch.log` before the structured logger is initialised. These logs contain errors about missing libraries, bad config paths, or argument parsing failures.

**Verify startup order.** The deploy script starts components in this sequence:

1. ChronoVisor (wait 2 s)
2. ChronoGrapher instances (wait 2 s)
3. ChronoPlayer instances (wait 2 s)
4. ChronoKeeper instances

Keepers, Graphers, and Players all register with ChronoVisor on startup. If ChronoVisor is not already running they will fail to register.

**Check for port conflicts.** Default ports used by ChronoLog:

| Service | Default port |
|---|---|
| ChronoVisor — client portal | 5555 |
| ChronoVisor — keeper registry | 8888 |
| ChronoKeeper — recording service | 6666 |
| ChronoKeeper — datastore admin | 7777 |
| ChronoKeeper — grapher drain | 3333 |
| ChronoGrapher — datastore admin | 4444 |
| ChronoPlayer — store admin | 2222 |
| ChronoPlayer — playback query | 2225 |

```bash
ss -tlnp | grep -E '5555|8888|6666|7777|3333|4444|2222|2225'
```

If any port is already in use, either reconfigure it in `default_conf.json` or stop the conflicting process.

**Verify config files exist.** The deploy script generates per-instance config files under `<work-dir>/conf/` before starting any process. If config generation failed (e.g., `jq` not installed, malformed base config), the conf files will be missing or empty.

---

## Client Cannot Connect

### `CL_ERR_NO_CONNECTION` (-8)

The client cannot reach ChronoVisor.

- Confirm ChronoVisor is running: `pgrep -a chrono-visor`
- Check the `service_ip` and `service_base_port` in the client config (`chrono_client.VisorClientPortalService.rpc`) match the running Visor.
- Verify the protocol (`protocol_conf`) is the same on both sides (e.g., both use `"ofi+sockets"`).

### `CL_ERR_NO_KEEPERS` (-7)

ChronoVisor is reachable but no Keepers have registered.

- Check that at least one `chrono-keeper` process is running: `pgrep -a chrono-keeper`
- Inspect `<work-dir>/monitor/chrono-keeper-1.launch.log` for startup errors.
- Confirm Keeper's registry port (`VisorKeeperRegistryService.rpc.service_base_port`, default `8888`) matches Visor's `VisorKeeperRegistryService` port.

---

## Events Rejected / High Drop Rate

Symptoms: `LOG_WARNING` messages about rejected events, or `acceptance_window_secs` appearing in Keeper logs.

**Cause:** The event timestamp falls outside the Keeper's `acceptance_window_secs` window (default 15 s). This typically means clock skew between the client host and the Keeper node.

**Fix:**
- Synchronise clocks across all nodes with NTP or PTP.
- Increase `acceptance_window_secs` in `chrono_keeper.DataStoreInternals` to tolerate larger skew:

```json
"DataStoreInternals": {
  "acceptance_window_secs": 60
}
```

:::caution
Setting `acceptance_window_secs` too high allows late events to land in already-sealed chunks, which can cause out-of-order data in the archive.
:::

---

## Data Not Queryable After Writing

Data written by a client is not immediately visible in query results.

- **Chunk flush latency:** Events accumulate in a story chunk for `story_chunk_duration_secs` seconds (default 10 s on Keeper, 60 s on Grapher) before being drained downstream. Query results reflect only sealed, drained chunks.
- **ChronoPlayer not running:** Queries require a running Player. If no Player is registered you will receive `CL_ERR_NO_PLAYERS` (-10). Check `pgrep -a chrono-player` and inspect the Player launch log.

---

## Process Crashed / Cannot Stop

**Normal stop** (`--stop` flag) sends `SIGTERM` to each component and waits up to 100 seconds for a graceful shutdown, then force-kills with `SIGKILL` if the process does not exit.

**After a forced kill or crash**, stale config files and lock state may prevent a clean restart. Run `--clean` before restarting:

```bash
./deploy_local.sh --stop
./deploy_local.sh --clean
./deploy_local.sh --start
```

`--clean` removes generated config files in `<work-dir>/conf/`, all log files in `<work-dir>/monitor/`, and output data in `<work-dir>/output/`. It aborts if any ChronoLog process is still running.

---

## Chronicle or Story Operation Fails

Consult the [Error Codes](./error-codes.md) page for the specific error value returned by the API call.

Common cases:

| Symptom | Likely cause | Fix |
|---|---|---|
| Create chronicle returns `CL_ERR_CHRONICLE_EXISTS` (-6) | A chronicle with that name already exists | Use the existing chronicle, or destroy it first |
| Create story returns `CL_ERR_STORY_EXISTS` (-101) | Story already exists in this chronicle | Use the existing story |
| Acquire story returns `CL_ERR_NOT_EXIST` (-3) | Chronicle or story was not created | Create chronicle and story before acquiring |
| Destroy chronicle/story returns `CL_ERR_ACQUIRED` (-4) | Resource is still acquired by a client | Release all acquisitions before destroying |
| Add property returns `CL_ERR_CHRONICLE_PROPERTY_FULL` (-103) or `CL_ERR_STORY_PROPERTY_FULL` (-104) | Property list is at capacity | Remove unused properties before adding new ones |
| Replay query returns `CL_ERR_CHRONICLE_DIR_NOT_EXIST` (-109) | `story_files_dir` in Grapher/Player config points to a non-existent path | Verify the path exists and is writable |
