# Local instances for agents

Use this page for discovery, MCP configuration, persistent storage and recovery. The launcher is `chronolog`; the installed server process is `chronolog-mcp`. They are separate console scripts.

## Discover and attach

`chronolog ls` returns JSON records. Select a `ready` record and pass its name to `chronolog run --up <name> -- chronolog-mcp --identity <slot> --chronicle <project>`. `run` holds an attachment lease for managed instances and exports `CHRONOLOG_CATALOG`, `CHRONOLOG_PLAYER`, `CHRONOLOG_INSTANCE`, `CHRONOLOG_HOME` and `CHRONOLOG_MCP_LOCK_DIR` to its child. An external record has no managed supervisor or lease. The demo kit registers its compose project automatically; use `chronolog-demo down` to stop that stack and remove its record, rather than the managed-instance `chronolog down`.

`CHRONOLOG_CATALOG` and optional `CHRONOLOG_PLAYER` take precedence for `run` and `env`, including when an instance name is passed. Otherwise `CHRONOLOG_INSTANCE` selects a name or id when no name is supplied, and `default` is the fallback. `--up` permits booting a stopped managed instance. Set explicit remote endpoints only when you intend to bypass local selection.

For a native child that needs the endpoints without an attachment, inspect the shell exports:

```bash
chronolog env default
```

Evaluating those exports sets the Catalog override in that shell. Unset it before selecting another managed instance; otherwise `run` treats the endpoints as an external environment override and does not hold a managed lease. Prefer `chronolog run` when an agent's lifetime should count as an attachment.

MCP offers the same discovery and lifecycle decisions: `instance_list(probe=true)` returns registry records and `bound_instance_id`; `probe=false` leaves external records `unprobed`. `instance_control(action="attach", name="default")` binds a ready record. `action="up"` boots and binds, with `create=true` required for a new record. Set `on_last_detach="keep"` or `"stop"` and `idle_grace_s` at creation; existing records keep their saved policy. Use the CLI when choosing non-default ports or storage paths.

Close writable contexts before rebinding; a refused rebind returns `changed=false`. `action="detach"` closes all of this server's sessions and drops its MCP lease. `action="down"` does the same for its bound target before requesting ordered stop; another holder, including a live launcher wrapper, prevents shutdown. Avoid `force=true` for ordinary agent cleanup. `context_status` reports the bound instance, live holders, tiers and clock. The MCP server holds its own lease as well as any surrounding `chronolog run` wrapper; leave both processes before expecting the last-detach policy.

## Registry and states

The registry is `$CHRONOLOG_HOME`, default `${XDG_STATE_HOME:-$HOME/.local/state}/chronolog`. Set the same local filesystem path in every harness. It contains `instances/<name>/`, `external/<name>.json` and shared `locks/`; directories are 0700 and files 0600. NFS is refused for the registry because liveness and leases require local flock semantics. Names match `[a-z0-9][a-z0-9-]{0,31}`.

| State | Meaning and action |
|---|---|
| `ready` | The instance is usable. A reported `probe: rpc` means Catalog and Player RPCs were checked; `probe: tcp` is the fallback when the SDK wheel is absent. |
| `degraded` | A managed probe failed, or an external endpoint accepted TCP but its RPC failed. Inspect `probe_error`, doctor and logs before using it. |
| `stopped` | The managed supervisor lock is free, or an external endpoint has no reachable listener. Boot a managed instance with `up`; restart an external deployment with its own operator command. |
| `starting`, `stopping`, `unresponsive` | Boot or shutdown is in progress, or a held supervisor lock has stale status. The supervisor lock, not a pid file, decides managed liveness. |

`chronolog status default --probe` checks an existing record. An external record always uses live endpoint probes. A managed instance is supervised outside the invoking harness's process group, so session exit does not kill the stack. Each attachment holds an flock lease; the kernel drops it even if its process is killed. `status` reports the attachment count and holders in a periodic supervisor snapshot; `down` checks the live lease locks.

The default policy is `keep`: the last detach leaves the instance running. `--ephemeral` at creation changes it to `stop`, with a 30-second idle grace by default. `chronolog down` refuses a foreign live lease. A normal stop preserves the Catalog, WAL, archive and fixed endpoints; `--purge` removes the managed instance directory and is for disposable data. Explicit WAL/archive directories outside that directory are not purged.

## Boot, restart and dragon placement

The launcher starts Visor, then Keeper and Grapher, then Player, and waits for readiness before `up` returns. The supervisor restarts a crashed service. If the supervisor dies, its services die with it; discovery reports `stopped`, and another `up` recovers the same instance and endpoints. A node reboot frees the locks; the instance remains stopped until `up`. DURABLE events survive Keeper WAL recovery with the same EventIds and HLCs. Unarchived ACCEPTED events may vanish after a crash.

By default the Catalog is `instances/<name>/visor/catalog.sqlite`, the WAL is `keeper/wal/`, the archive is `grapher/archive/`, and logs are in `logs/` below the instance directory. Paths and the local tier are returned in the record.

The PI approved dragon placement as WAL on `/home` NVMe, the local archive tier on the `/` NVMe with a 200 GB budget, and a later slow tier at `/mnt/nfs/chronolog-sprint/tiers`. On first creation, use a provisioned, owner-writable persistent directory on the `/` filesystem for `CHRONOLOG_LOCAL_ROOT`; keep the registry and WAL on `/home`:

```bash
chronolog up default --wal-dir "$CHRONOLOG_HOME/instances/default/keeper/wal" --local-root "$CHRONOLOG_LOCAL_ROOT" --budget-bytes 200000000000
```

Set both variables to the intended paths before that command. Existing instances keep their saved paths; later `up` does not relocate data. `budget_bytes` is metadata at LOCAL-3, not an enforced capacity limit. Slow-tier migration is pending the tier slices; this launcher does not configure NFS migration yet. The walkthrough's archive is temporary on `/` for proof, not a persistent deployment recommendation.

## Harness configuration

Install the launcher, SDK and MCP wheels in the venv used by the harness and install the four servers beside `chronolog`, or set `CHRONOLOG_BIN_DIR` to their build/install directory. `chronolog doctor` shows which binaries resolve. Use the full launcher path when a harness PATH differs from your shell.

Claude Code registration is shown in SKILL.md. Codex's MCP table can use:

```toml
[mcp_servers.chronolog]
command = "/path/to/venv/bin/chronolog"
args = ["run", "--up", "default", "--", "chronolog-mcp", "--identity", "agent-memory/main", "--chronicle", "agent-memory"]

[mcp_servers.chronolog.env]
CHRONOLOG_HOME = "/home/USER/.local/state/chronolog"
PATH = "/path/to/venv/bin:/usr/bin:/bin"
```

clio-coder uses the same launcher in `~/.config/clio-coder/mcp.yaml`:

```yaml
version: 1
servers:
  - id: chronolog
    command: /path/to/venv/bin/chronolog
    args: [run, --up, default, --, chronolog-mcp, --identity, agent-memory/main, --chronicle, agent-memory]
    env:
      CHRONOLOG_HOME: /home/USER/.local/state/chronolog
      PATH: /path/to/venv/bin:/usr/bin:/bin
    actionClass: execute
```

The marketplace entry chooses `chronolog run --up default -- chronolog-mcp` when the launcher is on PATH and otherwise uses `uvx chronolog-mcp==4.0.0`. The base uvx fallback connects to configured external endpoints; its instance tools require installing the optional `chronolog-mcp[local]` dependency or `chronolog-local` alongside it. It retains the existing environment allowlist. Use explicit configuration above to forward a custom `CHRONOLOG_HOME`; supply `CHRONOLOG_MCP_IDENTITY` for writable tools when using the marketplace.

## Recall by time, latest and follow

The twelve-tool interface is in [api.md](api.md). `context_recall(since=..., until=...)` accepts integer nanoseconds or RFC 3339 for a half-open Keeper acceptance-time interval; `context_latest(until=...)` selects events before an exclusive acceptance-time bound. Do not combine these conveniences with opaque bounds or recall cursors. HLC may lead Keeper CLOCK_REALTIME by D (61 seconds by default); this is not a writer physical-axis or payload-history query.

Use `context_recall` with `start` and `end` tokens from returned events' `at` fields for an HLC interval: start is inclusive and end is exclusive. Preserve `next_cursor` when paging the same range and inspect `answer_complete` and the underlying Completion before concluding that no other events occurred. HLC physical time is acceptance time at the Keeper, not an arbitrary scientific dataset's historical timestamp.

Use `context_latest` for the most recent `n` events, anchored by its returned `as_of`; check `selection_complete` before claiming those are all of the requested latest events. `before` restricts the selection using an existing `at` token.

Use `context_follow` for live work rather than repeated recalls. Save the returned follow token for each subscription and checkpoint the tokens you have processed. A live tail never certifies completeness for a fixed time range. For writer-clock ranges use the SDK's physical-axis read and inspect clock discipline and Completion; arbitrary backdated dataset import is outside the current append contract.
