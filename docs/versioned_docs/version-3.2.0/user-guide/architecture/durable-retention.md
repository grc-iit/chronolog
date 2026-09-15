---
sidebar_position: 7
title: "Durable Chunk Retention"
---

# Durable Chunk Retention

A ChronoKeeper keeps every sealed story chunk in memory until ChronoGrapher confirms that the chunk's
events are written to HDF5. ChronoPlayer uses the same confirmation to decide which part of a replay
comes from the HDF5 archive and which part from keeper memory.

Before this, a keeper deleted a chunk as soon as it had tried to send it, even when the send failed,
and the grapher acknowledged a chunk when it arrived rather than when it was written. A grapher
outage therefore lost every chunk sent during it, with nothing recording the loss.

---

## How chunks are kept and freed

1. **Seal and send.** When a chunk seals, the keeper sends it to the grapher and keeps it in memory.
   The grapher answers with a *receipt*, a number it gives the chunk. The chunk keeps serving
   [tail reads](./on-demand-tail-read.md) while it is held.
2. **Persisted watermark and receipts.** For each story the grapher tracks how far the story is
   written to HDF5 with no gaps: the *persisted watermark*. The watermark alone cannot confirm every
   chunk: a chunk that arrives after the watermark has passed its range goes into a reopened past
   window or a separate file. So the grapher also tracks which receipts still have events that are
   not written. Every `watermark_report_interval_secs` it sends, to each keeper that contributed to
   a story, the story's watermark and its unwritten receipts, for every story where either changed.
3. **Free.** A keeper frees a chunk only when all of these hold:
   - the grapher acknowledged receiving the chunk;
   - the reported watermark has reached the end of the chunk;
   - the grapher reports the chunk's receipt as written. A restarted grapher starts numbering
     receipts again, so its reports never confirm a receipt the previous grapher process gave out;
   - none of the chunk's events is still in the tail index that `playback` reads from;
   - the chunk is not waiting to be sent again.
4. **Re-send.** A chunk whose send failed, or that is still unconfirmed after
   `watermark_resend_timeout_secs`, is sent again.

Events leave a story's tail index when the story holds more than `tail_capacity` events there, or
when the story retires on the keeper: no client has it acquired and its acceptance window has
passed. After that, `playback` no longer returns them from this keeper, and their chunks are freed
as soon as the grapher confirms them.

A keeper decides chunk by chunk. If one keeper's chunk failed to reach the grapher, that keeper keeps
it and sends it again, however far other keepers' data has moved the watermark.

---

## Replay

For a replay of `[start, end)`, the player asks each keeper of the story for the events it still
holds in the range, the watermark it last received, and the oldest event time it holds. The boundary
`B` is the highest of those watermarks: the archive serves `[start, B)`, the keepers serve
`[B, end)`. Everything below `B` is on disk, and a keeper never frees a chunk its own watermark does
not cover, so every event at or above `B` is still in some keeper's memory. If no keeper has received
a watermark yet, no keeper has freed anything, and `B` is the oldest event time any keeper holds.

- Events in a chunk the grapher has not yet confirmed written come from the keeper even below `B`,
  since the archive may not have them.
- A replay sees an event once its chunk has sealed on the keeper (about `story_chunk_duration_secs`
  plus `acceptance_window_secs`), without waiting for the grapher to write it.
- A keeper that does not answer within 5 seconds is left out. Events held only by that keeper and not
  yet written are missing from that replay and appear once they are persisted.
- Nothing removes duplicates between the archive and the keepers, or between archive files. An event
  can appear twice in a replay when the grapher has written its chunk but the keeper has not received
  the report yet (up to `watermark_report_interval_secs`), or when a re-sent chunk was written to a
  second file.

---

## Failure behavior

| Situation | What happens |
|---|---|
| Grapher paused or unreachable for a while | Keepers keep the chunks they cannot deliver, warn once memory passes `retention_cap_mb`, and send them again when the grapher is back. No events are lost; a chunk that reached the grapher before the outage can be written twice, and replays then return its events twice. |
| Grapher restarts | The watermark and the receipts live in grapher memory and start over. Keepers send again the chunks they still hold, since the new process's reports confirm none of the old receipts; events both processes wrote appear twice in replays. |
| An HDF5 write fails | The story's watermark stops advancing, and the receipts of the chunks in that write stay unconfirmed. Keepers keep the affected chunks and send them again until a write succeeds. |
| Keeper crashes | Chunks that keeper held and the grapher had not yet written are lost. Chunks are not replicated across keepers. |
| Story destroyed before its last chunks are written | Nothing ever confirms those chunks, so the keepers hold them until the keeper restarts. |

---

## Memory

Keeper memory grows while the grapher falls behind, since nothing is freed until it is written.
`retention_cap_mb` does not limit that: it logs a warning each time retained memory crosses it, and
drops nothing. Size keeper memory for the longest grapher outage you want to ride out.

While a story is recorded, up to `tail_capacity` of its events stay in memory for tail reads even
after they are written, together with the chunks holding them. They are released when the story
retires.

---

## Configuration

| Key | Component | Default | Meaning |
|---|---|---|---|
| `watermark_report_interval_secs` | grapher | `1` | How often the grapher sends changed watermarks and receipts to the keepers. |
| `watermark_resend_timeout_secs` | keeper | `720` | How long a keeper waits for a chunk to be confirmed written before sending it again. Keep it well above the grapher's `story_chunk_duration_secs` plus `acceptance_window_secs`, or healthy chunks are sent twice. |
| `retention_cap_mb` | keeper | `512` | Retained-memory level that triggers a warning. `0` turns the warning off. |

All three live in the component's `DataStoreInternals` block; see
[Server Configuration](../configuration/server-configuration.md#datastoreinternals--story-chunk-tuning).
`tail_retention_secs` is gone: chunks are freed by the watermark, not by age.

Keepers send chunks only to the grapher by default (`single_endpoint_rdma_extractor`). The
`dual_endpoint_rdma_extractor`, which also sends each chunk to the player, still works but replay no
longer needs it.
