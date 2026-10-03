# ChronoLog MCP server

`chronolog-mcp` gives agents ChronoLog contexts as durable, time-ordered memory through ten tools over the ChronoLog
Context API. A context is one story in the launcher's chronicle. Results are bounded JSON that lead with a one-line
`verdict`, then `answer_complete`, `has_more` and `next_cursor`; ids and nanoseconds are decimal strings, and cursors,
`at` bounds, follow tokens, ref tokens and checkpoint ids are opaque strings.

## Run

```sh
pip install chronolog chronolog-mcp
chronolog-mcp --catalog 127.0.0.1:50051 --player 127.0.0.1:50054 --chronicle team --identity team/planner
claude mcp add chronolog -- chronolog-mcp --catalog 127.0.0.1:50051 --identity team/planner
```

`--identity` is the stable launcher base slot, reused across runs; writable tools need it. Each agent label on the
connection (default `self`) is its own writer slot inside that base. `--session-id` names the launcher's run and is
fresh by default. The server holds a host-local lock per slot under `--lock-dir`; a second launcher of the same slot
on the same host serves reads only. `--host-id` (default: the hostname) and the lock identify the owner in every
acquisition record. `--transport http` serves streamable HTTP on `--host`/`--port`.

## Tools

| Tool | Purpose |
| --- | --- |
| `context_open` | Open a context by name (`create=true` creates it) or `ref_token`, for an agent label, read_write or read_only. A writable open checks the latest acquisition and close record and returns `NEEDS_RECONCILE` or `FENCED` with `takeover_required` instead of acquiring over an unclosed record. `resume`/`checkpoint_id` restore the processed follow position. |
| `context_remember` | Store one memory. **Reuse operation_id when retrying after an error or timeout.** `stored` is `durable`, `ram_only_may_vanish`, `rejected` or `unknown`. |
| `context_recall` | Certified pages of a context in Replay order; `start`/`end` are `at` tokens from returned events, omitted end is a verified cut, `next_cursor` continues. |
| `context_latest` | The last n events before an optional `at`, with the disclosed `as_of` and `selection_complete`. |
| `context_follow` | Wait on several contexts under one deadline from `now`, `beginning` or a follow token; tokens survive restarts. |
| `context_reconcile` | Recover uncertain writes: LANDED, ABSENT or UNKNOWN per operation id. **Pass every operation_id for which you have not seen an outcome; omitted ids may duplicate.** `takeover=true` is a deliberate decision; without `session_handle` it recovers the checkpoint store. |
| `context_checkpoint` | Acknowledge processed follow tokens and persist session state durably; returns a `checkpoint_id`. |
| `context_close` | Release the writer and durably record the close of that exact incarnation. |
| `context_list` | Contexts of a chronicle with ref tokens; metadata only. |
| `context_status` | Session states, writer stamps, unresolved and permanently UNKNOWN ids, the checkpoint store. |

`compact` view (default) returns id, HLC, `at`, content type and the full payload (UTF-8 or base64) and names the
omitted fields; `full` adds attributes, trace ids, physical time, durability and a per-event follow token. Budgets
default to 50 events and 24 KiB of JSON; a single event larger than the budget is still delivered whole and
disclosed.

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
