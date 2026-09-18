# ChronoKeeper — chunk durability lifecycle under the watermark protocol (sequence)

The `watermark-feedback` branch replaces ack-less chunk deletion with **durability-gated retention**. `KeeperTailStore` becomes `KeeperChunkRetentionStore`, which owns every sealed chunk. A chunk is **ship-on-seal** (its pointer enters the extraction queue the instant it seals) but is freed only once it is durable: `shipped ∧ chunk.endTime ≤ W ∧ tail-released` (invariant `E ≤ W`). Drain outcomes route back through the extraction module's **disposal seam** (`markShipped` / `markSendFailed`); the grapher's persisted watermark `W` arrives via `report_story_watermarks` → `confirmPersisted`; un-acked/un-covered chunks are re-shipped by the stall timer (`requeueStalled`). Fonts enlarged ~50%. See also [watermark_feedback_sequence](../Watermark/watermark_feedback_sequence.md).

```mermaid
%%{init: {"htmlLabels": false, "themeCSS": ".messageText{font-size:18px !important;} .loopText,.loopText tspan{font-size:18px !important;} .labelText,.labelText tspan{font-size:18px !important;} text.actor tspan,.actor{font-size:18px !important;} .noteText,.noteText tspan{font-size:24px !important;} .sectionTitle{font-size:18px !important;} .sequenceNumber{font-size:16px !important;}"}}%%
%% ChronoKeeper — chunk durability lifecycle under the watermark protocol: ship-on-seal, retention store ownership, free only when E <= W
sequenceDiagram
  autonumber
  actor C as Client (storyteller)
  participant V as ChronoVisor
  box rgb(235,242,250) ChronoKeeper process
    participant KRS as KeeperRecordingService
    participant AS as DataStoreAdminService
    participant IQ as IngestionQueue
    participant KDS as KeeperDataStore (collection ULTs)
    participant KSP as KeeperStoryPipeline
    participant RS as KeeperChunkRetentionStore
    participant XQ as StoryChunkExtractionQueue
    participant XM as ExtractionModule (extraction ULTs)
    participant CH as ChronoKeeperExtractionChain
    participant DX as DualEndpointChunkExtractorRDMA
  end
  participant GR as ChronoGrapher
  participant PL as ChronoPlayer

  Note over V,RS: 0) ACQUIRE  —  Visor starts the story, the pipeline is built and wired to the retention store
  V->>AS: start_story_recording(chronicle, story, storyId, t0)   [RPC]
  AS->>KDS: startStoryRecording(...)
  KDS->>KSP: construct KeeperStoryPipeline(..., theRetentionStore)
  KDS->>IQ: addStoryIngestionHandle(storyId, handle)
  AS-->>V: CL_SUCCESS

  Note over C,IQ: 1) RECORD  —  hot path (returns immediately)
  C->>KRS: record_event(LogEvent)   [RPC]
  KRS->>IQ: ingestLogEvent(event)
  KRS-->>C: CL_SUCCESS

  Note over KDS,KSP: 2) COLLECT + CHUNK  —  background data-collection ULT
  loop dataCollectionTask, per tick
    KDS->>KSP: collectIngestedEvents() -> mergeEvents() -> storyTimelineMap
  end

  Note over KDS,XQ: 3) SEAL (ship-on-seal)  —  a decayed chunk is OWNED by the retention store AND queued for shipping at once
  KDS->>KSP: extractDecayedStoryChunks(now)
  KSP->>RS: ingestSealedChunk(storyId, sealed chunk)
  RS->>RS: index the last-N tail (EventSequence -> chunk), mark state.in_queue = true
  RS->>XQ: stashStoryChunk(chunk)  (pointer only — payload stays retained)
  Note right of RS: chunk is RETAINED — never freed until it is durable (E ≤ W)

  Note over XM,PL: 4) SHIP  —  extraction ULT drains, the RDMA fan-out carries the keeper's reporter id, and the outcome is routed back through the disposal seam
  loop drainExtractionQueue (extraction ULT)
    XM->>XQ: ejectStoryChunk()
    XM->>CH: process_chunk(chunk)
    CH->>DX: process_chunk(chunk)
    par to Grapher (archival)
      DX->>GR: receive_story_chunk(bulk, reporter = keeper AdminService)   [RDMA]
      GR-->>DX: ack (bytes)
    and to Player (legacy mirror)
      DX->>PL: receive_story_chunk(bulk, reporter)   [RDMA]
      PL-->>DX: ack
    end
    XM->>CH: dispose_chunk(chunk, status)
    alt every extractor acked (status == CL_SUCCESS)
      CH->>RS: markShipped(chunk)  (shipped = true, in_queue = false)
    else transfer failed
      CH->>RS: markSendFailed(chunk)  (stays retained and readable, re-sent later)
    end
  end

  Note over GR,RS: 5) CONFIRM PERSISTED  —  the grapher reports its persisted watermark W, and only now may covered chunks free
  GR->>AS: report_story_watermarks(map StoryId -> W)   [RPC, one-way]
  AS->>KDS: applyWatermarkReport(storyId, W)
  KDS->>RS: confirmPersisted(storyId, W)
  RS->>RS: free every chunk where shipped AND endTime ≤ W AND tail-released (frees oldest-first, keeps E ≤ W)

  Note over KDS,XQ: 6) STALL RE-SEND  —  un-acked or un-covered chunks are re-shipped after the resend timeout (idempotent, grapher dedups)
  loop each tick
    KDS->>RS: requeueStalled(watermark_resend_timeout_secs)
    RS->>XQ: re-stash stale, un-covered chunks
  end

  Note over RS,PL: the retention store also serves reads from keeper memory — playback tail reads (tail_get_sequences / tail_get_events) and the player's story_range_fetch — see watermark_feedback_sequence
```
