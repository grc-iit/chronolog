# ChronoLog MCP server

`chronolog-mcp` gives agents ChronoLog contexts as durable, time-ordered memory through twelve tools over the ChronoLog
Context API. A context is one story in the launcher's chronicle. Results are bounded JSON that lead with a one-line
`verdict`, then `answer_complete`, `has_more` and `next_cursor`; ids and nanoseconds are decimal strings, and cursors,
`at` bounds, follow tokens, ref tokens and checkpoint ids are opaque strings.

## Install

The repository root is a plugin marketplace for Claude Code and Codex. Its one plugin, `chronolog`, installs the
`chronolog` skill and this server, launched as `uvx chronolog-mcp==4.0.0`.

```sh
claude plugin marketplace add grc-iit/ChronoLog        # or a local checkout path
claude plugin install chronolog@chronolog
codex plugin marketplace add grc-iit/ChronoLog         # or a local checkout path
codex plugin add chronolog@chronolog
```

The server reads its settings from the environment: `CHRONOLOG_CATALOG` (explicit endpoint override),
`CHRONOLOG_PLAYER`, `CHRONOLOG_CHRONICLE`, `CHRONOLOG_MCP_IDENTITY` (the stable slot writable tools need),
`CHRONOLOG_HOME`, `CHRONOLOG_INSTANCE`, `CHRONOLOG_AUTOSTART`, `CHRONOLOG_BIN_DIR`, `CHRONOLOG_MCP_SESSION_ID`, `CHRONOLOG_MCP_HOST_ID`, `CHRONOLOG_MCP_LOCK_DIR` and `CHRONOLOG_MCP_STATE_CHRONICLE`.
Harness configurations should pass `CHRONOLOG_HOME` consistently and forward the settings above and `UV_FIND_LINKS`.

Until `chronolog` and `chronolog-mcp` are published on PyPI, point uv at the wheels a source checkout builds
(`bash tests/smoke/build_artifacts.sh` writes both to `build/smoke/wheels`) and the same command resolves them
locally:

```sh
export UV_FIND_LINKS=/path/to/ChronoLog/build/smoke/wheels
```

Without the plugin, register the server directly:

```sh
claude mcp add chronolog -- uvx chronolog-mcp==4.0.0 --identity team/planner
codex mcp add chronolog -- uvx chronolog-mcp==4.0.0 --identity team/planner
```

and give Codex the skill by linking `skills/chronolog` into `~/.agents/skills/` (a checkout already exposes it to
both agents through `.claude/skills` and `.agents/skills`).

## Run

```sh
chronolog-mcp --catalog 127.0.0.1:50051 --player 127.0.0.1:50054 --chronicle team --identity team/planner
```

`--identity` is the stable launcher base slot, reused across runs; writable tools need it. Each agent label on the
connection (default `self`) is its own writer slot inside that base. `--session-id` names the launcher's run and is
fresh by default. The server holds a host-local lock per slot under `--lock-dir`, defaulting to `$CHRONOLOG_HOME/locks`.
`CHRONOLOG_HOME` defaults to `${XDG_STATE_HOME:-$HOME/.local/state}/chronolog`; `XDG_RUNTIME_DIR` does not select
the shared lock directory. Explicit `--lock-dir` or `CHRONOLOG_MCP_LOCK_DIR` overrides it. Catalog endpoints are
normalized for locking (including localhost and 127.0.0.1 aliases, case and endpoint order). For this migration
release the server also holds the previous raw-endpoint lock and keeps its acquisition provenance and hints;
when using the default directory it also holds that lock in the previous runtime/cache directory. A second launcher of the same slot
on the same host serves reads only. `--host-id` (default: the hostname) and the lock identify the owner in every
acquisition record. `--transport http` serves streamable HTTP on `--host`/`--port`.

Local discovery uses the explicit Catalog first, then `CHRONOLOG_INSTANCE`, then a ready `default`. With
`CHRONOLOG_AUTOSTART=1`, the selected instance is created or booted by the LOCAL-1 detached supervisor.
The final fallback probes the legacy loopback demo. If nothing resolves, the server starts unbound and context
tools return a verdict directing the agent to `instance_control` with `action="up", create=true`.
Local creation requires installed service binaries on PATH or `CHRONOLOG_BIN_DIR`; install `chronolog-local`
alongside this server, or install `chronolog-mcp[local]` to include the launcher dependency. Its wheel can be
built with `python -m build --wheel launcher`. Explicit Catalog deployments use the base MCP package.

`instance_list(probe=false)` reads registered instance metadata and marks this server's binding.
`instance_control(action, name="default", create=false, on_last_detach, idle_grace_s, force=false)` boots,
attaches, detaches or stops an instance. Creation policy defaults to keep with a 300 s idle grace; use
`on_last_detach="stop"` for an ephemeral instance. The server holds its own managed-instance lease, even when
launched through `chronolog run`. Detach closes sessions and persists writable close records before dropping
the lease. Rebinding requires closing writable sessions first. Down respects foreign leases unless forced.
`context_status` includes the current instance's endpoints, tiers, attach count and clock status.
Registry instances use their id for slot locks and also hold normalized and raw Catalog locks for migration.

## Tools

| Tool | Purpose |
| --- | --- |
| `context_open` | Open a context by name (`create=true` creates it) or `ref_token`, for an agent label, read_write or read_only. A writable open checks the latest acquisition and close record and returns `NEEDS_RECONCILE` or `FENCED` with `takeover_required` instead of acquiring over an unclosed record. `resume`/`checkpoint_id` restore the processed follow position. |
| `context_remember` | Store one memory. **Reuse operation_id when retrying after an error or timeout.** `stored` is `durable`, `ram_only_may_vanish`, `rejected` or `unknown`. Optional `kind` names the memory (`chronolog.` kinds are reserved and rejected) and `links` record provenance as `[{type, event_id, hlc?}]` (suggested types `caused_by`, `replies_to`, `derived_from`; at most 16); the actor is the session's `writer_identity`. |
| `context_recall` | Certified pages of a context in Replay order; `start`/`end` are `at` tokens from returned events, omitted end is a verified cut, `next_cursor` continues. `since`/`until` accept int64 nanoseconds or RFC 3339 and exclude `start`/`end`/`cursor`; `until` is exclusive. Each event carries `kind`, `actor` and `links`. |
| `context_latest` | The last n events before an optional `before` token or acceptance-time `until`, with the disclosed `as_of` and `selection_complete`. Each event carries `kind`, `actor` and `links`. |
| `context_follow` | Wait on several contexts under one deadline from `now`, `beginning` or a follow token; tokens survive restarts. |
| `context_reconcile` | Recover uncertain writes: LANDED, ABSENT or UNKNOWN per operation id. **Pass every operation_id for which you have not seen an outcome; omitted ids may duplicate.** `takeover=true` is a deliberate decision; without `session_handle` it recovers the checkpoint store. |
| `context_checkpoint` | Acknowledge processed follow tokens and persist session state durably; returns a `checkpoint_id`. |
| `context_close` | Release the writer and durably record the close of that exact incarnation. |
| `context_list` | Contexts of a chronicle with ref tokens; metadata only. |
| `context_status` | Session states, writer stamps, unresolved and permanently UNKNOWN ids, the checkpoint store. |

`compact` view (default) returns id, HLC, `at`, content type and the full payload (UTF-8 or base64) and names the
omitted fields; `full` adds attributes, trace ids, physical time, durability and a per-event follow token. Budgets
default to 50 events and 24 KiB of JSON. `--max-json-bytes 14336` sets the default for recall, latest and follow
to fit clio-coder's 16 KiB visible result; a per-call `max_json_bytes` overrides it. Budgets are soft: a single event larger than the budget is still delivered whole and
disclosed.

`since` and `until` become Hlc{t, 0} bounds on the HLC axis. They describe Keeper acceptance time: HLC physical
time is Keeper CLOCK_REALTIME at acceptance (I8.7), with lead bounded by D, 61 s by default (I8.8). HLC coverage
can be complete (I6.1). Writer-time ranges use the SDK readPhysical API. RFC 3339 bounds require a timezone and
accept up to nine fractional digits without rounding.

## Checkpoints and recovery

All labels of one base slot share one `_checkpoints:<identity>` story in `--state-chronicle`, written by the reserved
control writer `agent-context-control/v2:[identity,"checkpoints"]`. Aggregates are bounded by
`--max-checkpoint-payload-bytes`, at most `--keeper-payload-max-bytes`; operations whose tracking state could not fit
are refused before dispatch. Every start recovers the control writer and finds the newest aggregate by a bounded
backward search; when that search is incomplete every context is treated as unclosed with unknown prior state. A
foreign or unknown owner needs `context_reconcile` with `takeover=true`. If another server fences the control writer,
writable tools fail closed with `checkpoint_store_fenced` and reads continue. Idle writable sessions close after
`--idle-close-s` (default 1800, 0 disables) and every session closes when the server stops.

Deployment start and stop remain in the operational skills; closing a context never stops the stack.
