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
   The chunk keeps serving [tail reads](./on-demand-tail-read.md) while it is held.
2. **Persisted watermark.** For each story the grapher tracks how far the story is written to HDF5
   with no gaps: the *persisted watermark*. Every `watermark_report_interval_secs` it sends the
   watermarks that moved to each keeper that contributed chunks to the story.
3. **Free.** A keeper frees a chunk only when all of these hold:
   - the grapher acknowledged receiving the chunk;
   - the reported watermark has reached the end of the chunk;
   - none of the chunk's events is still in the tail index that `playback` reads from;
   - the chunk is not waiting to be sent again.
4. **Re-send.** A chunk whose send failed, or whose acknowledgment or covering watermark has not
   arrived within `watermark_resend_timeout_secs`, is sent again. Events that arrive twice are
   removed by event identity (time, client, index), in the grapher and when reading.

A keeper decides chunk by chunk. If one keeper's chunk failed to reach the grapher, that keeper keeps
it and sends it again, however far other keepers' data has moved the watermark.

---

## Replay

For a replay of `[start, end)`, the player asks each keeper of the story for the events it still
holds in the range and for the oldest event time it holds. The smallest of those times is the
boundary `B`: the archive serves `[start, B)`, the keepers serve `[B, end)`. Everything below `B` is
on disk, because a keeper frees a chunk only after it is persisted.

- A replay sees an event once its chunk has sealed on the keeper (about `story_chunk_duration_secs`
  plus `acceptance_window_secs`), without waiting for the grapher to write it.
- A keeper that does not answer within 5 seconds is left out of the boundary. The archive then
  serves more of the range; events held only by that keeper and not yet written are missing from
  that replay and appear once they are persisted.

---

## Failure behavior

| Situation | What happens |
|---|---|
| Grapher paused or unreachable for a while | Keepers keep the chunks they cannot deliver, warn once memory passes `retention_cap_mb`, and send them again when the grapher is back. No events are lost; some may be written twice and are removed when reading. |
| Grapher restarts | The watermark lives in grapher memory and starts over. Keepers send again what they still hold; the duplicates on disk are removed when reading. |
| An HDF5 write fails | The story's watermark stops advancing. Keepers keep the affected chunks and send them again until a write succeeds. |
| Keeper crashes | Chunks that keeper held and the grapher had not yet written are lost. Chunks are not replicated across keepers. |
| Story destroyed before its last chunks are written | No watermark ever covers those chunks, so the keepers hold them until the keeper restarts. |

---

## Memory

Keeper memory grows while the grapher falls behind, since nothing is freed until it is written.
`retention_cap_mb` does not limit that: it logs a warning each time retained memory crosses it, and
drops nothing. Size keeper memory for the longest grapher outage you want to ride out.
`tail_capacity` only bounds the tail index that `playback` reads; it does not free chunks.

---

## Configuration

| Key | Component | Default | Meaning |
|---|---|---|---|
| `watermark_report_interval_secs` | grapher | `1` | How often the grapher sends changed watermarks to the keepers. |
| `watermark_resend_timeout_secs` | keeper | `720` | How long a keeper waits for an acknowledgment or a covering watermark before sending a chunk again. Keep it well above the grapher's `story_chunk_duration_secs` plus `acceptance_window_secs`, or healthy chunks are sent twice. |
| `retention_cap_mb` | keeper | `512` | Retained-memory level that triggers a warning. `0` turns the warning off. |

All three live in the component's `DataStoreInternals` block; see
[Server Configuration](../configuration/server-configuration.md#datastoreinternals--story-chunk-tuning).
`tail_retention_secs` is gone: chunks are freed by the watermark, not by age.

Keepers send chunks only to the grapher by default (`single_endpoint_rdma_extractor`). The
`dual_endpoint_rdma_extractor`, which also sends each chunk to the player, still works but replay no
longer needs it.
