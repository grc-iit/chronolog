---
sidebar_position: 6
title: "On-Demand Tail Read (Playback)"
---

# On-Demand Tail Read Architecture & Design

ChronoLog 3.1 introduces **On-Demand Keeper Tail Read** (`playback`), allowing client applications to query the most recent $N$ events of a story directly from ChronoKeeper in-memory buffers with millisecond-scale latency (~0.4 ms median, 1–5 ms at p90–p99), bypassing the persistent HDF5 tier and ChronoPlayer query service.

<svg viewBox="0 0 720 380" width="100%" xmlns="http://www.w3.org/2000/svg" fontFamily="system-ui, sans-serif">
  <rect x="0" y="0" width="720" height="380" rx="10" fill="#1e2330"/>

  {/* Title */}
  <text x="360" y="20" textAnchor="middle" fill="#c3e04d" fontSize="10" fontWeight="600">ON-DEMAND TAIL READ IN-MEMORY ARCHITECTURE</text>

  {/* ChronoKeeper Memory container (left / center) */}
  <rect x="20" y="34" width="460" height="330" rx="8" fill="none" stroke="#4a90a4" strokeWidth="0.75" strokeDasharray="6,3" strokeOpacity="0.4"/>
  <text x="32" y="48" fill="#4a90a4" fontSize="8" fontWeight="600" fillOpacity="0.7">CHRONOKEEPER IN-MEMORY PIPELINE</text>

  {/* Ingestion Queue */}
  <rect x="36" y="58" width="428" height="48" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.5"/>
  <circle cx="52" cy="76" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="60" y="80" fill="#c3e04d" fontSize="9" fontWeight="600">IngestionQueue / Memory Group</text>
  <text x="60" y="94" fill="#9ca3b0" fontSize="7">{"Receives log events via record_event RPC and buffers into memory chunks"}</text>

  {/* Arrow: Ingestion -> Active Timeline */}
  <line x1="250" y1="106" x2="250" y2="124" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="250,126 246,120 254,120" fill="#c3e04d" fillOpacity="0.6"/>
  <text x="256" y="118" fill="#9ca3b0" fontSize="6.5" fontStyle="italic">insert into StoryChunk</text>

  {/* storyTimelineMap (Active Timeline) */}
  <rect x="36" y="128" width="428" height="52" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.5"/>
  <circle cx="52" cy="148" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="60" y="152" fill="#c3e04d" fontSize="9" fontWeight="600">storyTimelineMap (Active Timeline)</text>
  <text x="60" y="164" fill="#9ca3b0" fontSize="7">{"Open, active StoryChunks within [chunk_duration + acceptance_window]"}</text>
  <text x="60" y="174" fill="#4a90a4" fontSize="6.5">ActiveTailSource provides live provisional reads under sequencingMutex</text>

  {/* Arrow: Timeline -> KeeperTailStore */}
  <line x1="250" y1="180" x2="250" y2="198" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="250,200 246,194 254,194" fill="#c3e04d" fillOpacity="0.6"/>
  <text x="256" y="192" fill="#9ca3b0" fontSize="6.5" fontStyle="italic">seal on acceptance window decay</text>

  {/* KeeperTailStore (Per-Story Tail Index) */}
  <rect x="36" y="202" width="428" height="56" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.5"/>
  <circle cx="52" cy="222" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="60" y="226" fill="#c3e04d" fontSize="9" fontWeight="600">KeeperTailStore (Per-Story Tail Index)</text>
  <text x="60" y="238" fill="#9ca3b0" fontSize="7">{"In-memory index: EventSequence \u2192 StoryChunk* (zero-copy single payload)"}</text>
  <text x="60" y="248" fill="#9ca3b0" fontSize="6.5">{"Manages tail_capacity, tail_retention_secs age-out, and pin protection (kPinTicks)"}</text>

  {/* Arrow: KeeperTailStore -> Extraction Queue */}
  <line x1="250" y1="258" x2="250" y2="276" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="250,278 246,272 254,272" fill="#c3e04d" fillOpacity="0.6"/>
  <text x="256" y="270" fill="#9ca3b0" fontSize="6.5" fontStyle="italic">age out or capacity eviction</text>

  {/* StoryChunk Extraction Queue */}
  <rect x="36" y="280" width="428" height="48" rx="6" fill="#252b3b" stroke="#3a4050" strokeWidth="0.75"/>
  <circle cx="52" cy="298" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="60" y="302" fill="#e4e7ed" fontSize="9" fontWeight="600">StoryChunk Extraction Queue</text>
  <text x="60" y="316" fill="#9ca3b0" fontSize="7">Retired chunks drained via RDMA bulk transfer to ChronoGrapher / Player</text>

  {/* === CLIENT TAIL READ CONSUMER (Right Side) === */}
  <rect x="500" y="110" width="200" height="170" rx="8" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.5"/>
  <circle cx="516" cy="130" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="526" y="134" fill="#c3e04d" fontSize="9.5" fontWeight="600">Client Tail Read</text>
  <text x="516" y="148" fill="#e4e7ed" fontSize="8">{"StoryHandle::playback(N)"}</text>
  <text x="516" y="160" fill="#9ca3b0" fontSize="7">Two-Phase Scatter/Gather</text>

  <rect x="512" y="172" width="176" height="42" rx="4" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <text x="600" y="186" textAnchor="middle" fill="#c3e04d" fontSize="7" fontWeight="600">KeeperTailReader</text>
  <text x="600" y="198" textAnchor="middle" fill="#9ca3b0" fontSize="6">Phase 1: Scatter Sequence Keys</text>
  <text x="600" y="206" textAnchor="middle" fill="#9ca3b0" fontSize="6">Phase 2: Gather Winning Payloads</text>

  <rect x="512" y="222" width="176" height="46" rx="4" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <text x="600" y="236" textAnchor="middle" fill="#e4e7ed" fontSize="7">Direct RAM Access</text>
  <text x="600" y="246" textAnchor="middle" fill="#c3e04d" fontSize="6.5">~0.4 ms median · 1–5 ms p90–p99</text>
  <text x="600" y="256" textAnchor="middle" fill="#9ca3b0" fontSize="6">Bypasses Player and HDF5 disks</text>

  {/* Connecting Arrows to Client */}
  {/* KeeperTailStore -> Client (Sealed Tail Read) */}
  <line x1="464" y1="230" x2="498" y2="230" stroke="#c3e04d" strokeWidth="0.85" strokeOpacity="0.7"/>
  <polygon points="500,230 494,226 494,234" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="482" y="222" textAnchor="middle" fill="#c3e04d" fontSize="6" fontWeight="600">Sealed Tail</text>

  {/* Active Timeline -> Client (Live Tail Read) */}
  <path d="M 464,154 L 485,154 L 485,185 L 498,185" fill="none" stroke="#4a90a4" strokeWidth="0.8" strokeDasharray="4,3" strokeOpacity="0.8"/>
  <polygon points="500,185 494,181 494,189" fill="#4a90a4" fillOpacity="0.8"/>
  <text x="482" y="176" textAnchor="middle" fill="#4a90a4" fontSize="6" fontStyle="italic">live_tail_read</text>

</svg>

---

## 1. Motivation & Architecture Overview

Historically, retrieving logged events required querying the **ChronoPlayer** via `Client::ReplayStory`. While `ReplayStory` provides complete historical replays across persistent HDF5 files, it incurs indexing, disk I/O, and cross-tier network latencies (~1–2 seconds per query).

Many real-time use cases — such as live dashboards, monitoring alerts, stream processing, and rapid failover recovery — only need the **last $N$ events** produced by a story. 

On-Demand Tail Read provides:
- **Direct Client $\leftrightarrow$ Keeper Communication**: Clients communicate directly with the story's assigned ChronoKeepers via lightweight Thallium RPCs. No ChronoPlayer or ChronoVisor is in the read path.
- **Zero-Copy In-Memory Indexing**: Events are indexed directly by their memory pointers in sealed chunks retained before extraction.
- **Two-Phase Scatter/Gather Protocol**: Minimal network transfer — exactly $N$ payloads cross the wire across the entire keeper group.
- **Writer-Mode Compatibility**: Clients instantiated in writer-only mode (`ClientPortalServiceConf` only) can perform tail reads without configuring query endpoints.

---

## 2. In-Memory Retention & Single Payload Model

In ChronoKeeper, when a `StoryChunk` seals (past `chunk_duration + acceptance_window`), ownership is handed to `KeeperTailStore` rather than directly to the extraction queue.

<svg viewBox="0 0 720 260" width="100%" xmlns="http://www.w3.org/2000/svg" fontFamily="system-ui, sans-serif">
  <rect x="0" y="0" width="720" height="260" rx="10" fill="#1e2330"/>

  {/* Title */}
  <text x="360" y="20" textAnchor="middle" fill="#c3e04d" fontSize="10" fontWeight="600">KEEPERTAILSTORE SINGLE-PAYLOAD INDEXING MODEL</text>

  {/* Top: KeeperTailStore (Index Map) */}
  <rect x="30" y="36" width="660" height="106" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.5"/>
  <circle cx="46" cy="52" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="54" y="56" fill="#c3e04d" fontSize="9" fontWeight="600">KeeperTailStore (Per Story Index)</text>
  <text x="280" y="56" fill="#9ca3b0" fontSize="7">{"Sorted Index: std::map<EventSequence, StoryChunk*> (Key lookup in O(log K))"}</text>

  {/* Table Headers */}
  <rect x="46" y="66" width="310" height="20" rx="3" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <text x="201" y="79" textAnchor="middle" fill="#c3e04d" fontSize="7" fontWeight="600">{"EventSequence Key (time, client_id, index)"}</text>

  <rect x="364" y="66" width="310" height="20" rx="3" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <text x="519" y="79" textAnchor="middle" fill="#4a90a4" fontSize="7" fontWeight="600">{"Retained Chunk Pointer (StoryChunk*)"}</text>

  {/* Table Row 1 */}
  <rect x="46" y="90" width="310" height="20" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <text x="201" y="103" textAnchor="middle" fill="#e4e7ed" fontSize="6.5">{"(1700000000100, 101, 1)"}</text>

  <rect x="364" y="90" width="310" height="20" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <text x="519" y="103" textAnchor="middle" fill="#4a90a4" fontSize="6.5">{"Chunk A (0x7f8a1000)"}</text>

  {/* Table Row 2 */}
  <rect x="46" y="114" width="310" height="20" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <text x="201" y="127" textAnchor="middle" fill="#e4e7ed" fontSize="6.5">{"(1700000000300, 102, 1)"}</text>

  <rect x="364" y="114" width="310" height="20" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <text x="519" y="127" textAnchor="middle" fill="#4a90a4" fontSize="6.5">{"Chunk B (0x7f8a2000)"}</text>

  {/* Pointers Down to Chunks */}
  <line x1="450" y1="134" x2="220" y2="168" stroke="#4a90a4" strokeWidth="0.75" strokeDasharray="3,2" strokeOpacity="0.7"/>
  <polygon points="218,170 226,166 222,160" fill="#4a90a4" fillOpacity="0.7"/>

  <line x1="580" y1="134" x2="520" y2="168" stroke="#4a90a4" strokeWidth="0.75" strokeDasharray="3,2" strokeOpacity="0.7"/>
  <polygon points="518,170 526,166 522,160" fill="#4a90a4" fillOpacity="0.7"/>

  {/* Bottom: Retained StoryChunks with Event Payloads */}
  {/* Chunk A */}
  <rect x="30" y="172" width="320" height="74" rx="6" fill="#252b3b" stroke="#4a90a4" strokeWidth="1" strokeOpacity="0.5"/>
  <circle cx="46" cy="188" r="3" fill="#4a90a4" fillOpacity="0.8"/>
  <text x="54" y="192" fill="#4a90a4" fontSize="8" fontWeight="600">{"Retained StoryChunk A (0x7f8a1000)"}</text>
  <text x="210" y="192" fill="#9ca3b0" fontSize="6.5">pinned / unarchived</text>

  <rect x="42" y="202" width="140" height="32" rx="3" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <text x="112" y="216" textAnchor="middle" fill="#e4e7ed" fontSize="6.5">LogEvent 1</text>
  <text x="112" y="226" textAnchor="middle" fill="#9ca3b0" fontSize="5.5">"payload byte sequence"</text>

  <rect x="194" y="202" width="140" height="32" rx="3" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <text x="264" y="216" textAnchor="middle" fill="#e4e7ed" fontSize="6.5">LogEvent 2</text>
  <text x="264" y="226" textAnchor="middle" fill="#9ca3b0" fontSize="5.5">"payload byte sequence"</text>

  {/* Chunk B */}
  <rect x="370" y="172" width="320" height="74" rx="6" fill="#252b3b" stroke="#4a90a4" strokeWidth="1" strokeOpacity="0.5"/>
  <circle cx="386" cy="188" r="3" fill="#4a90a4" fillOpacity="0.8"/>
  <text x="394" y="192" fill="#4a90a4" fontSize="8" fontWeight="600">{"Retained StoryChunk B (0x7f8a2000)"}</text>
  <text x="550" y="192" fill="#9ca3b0" fontSize="6.5">pinned / unarchived</text>

  <rect x="382" y="202" width="140" height="32" rx="3" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <text x="452" y="216" textAnchor="middle" fill="#e4e7ed" fontSize="6.5">LogEvent 3</text>
  <text x="452" y="226" textAnchor="middle" fill="#9ca3b0" fontSize="5.5">"payload byte sequence"</text>

  <rect x="534" y="202" width="140" height="32" rx="3" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <text x="604" y="216" textAnchor="middle" fill="#e4e7ed" fontSize="6.5">LogEvent 4</text>
  <text x="604" y="226" textAnchor="middle" fill="#9ca3b0" fontSize="5.5">"payload byte sequence"</text>

</svg>

- **Single Payload Footprint**: `KeeperTailStore` maintains a sorted map `EventSequence -> StoryChunk*`. The actual `LogEvent` payload resides solely in the `StoryChunk`'s map node accessed via `StoryChunk::findEvent()`.
- **Deferred Archival**: A retained chunk is forwarded to the `StoryChunkExtractionQueue` for RDMA transfer to ChronoGrapher only after its events age out of the tail window, are evicted by capacity limits, or are handed over by the shutdown flush (see §4).

---

## 3. Two-Phase Scatter/Gather Protocol

A story's events are striped across its assigned ChronoKeepers. To gather the global last $N$ events without transferring redundant payloads, the client executes a two-phase protocol:

<svg viewBox="0 0 720 520" width="100%" xmlns="http://www.w3.org/2000/svg" fontFamily="system-ui, sans-serif">
  <rect x="0" y="0" width="720" height="520" rx="10" fill="#1e2330"/>

  {/* Title */}
  <text x="360" y="20" textAnchor="middle" fill="#c3e04d" fontSize="10" fontWeight="600">TWO-PHASE SCATTER / GATHER TAIL READ PROTOCOL</text>

  {/* Actor Columns */}
  {/* Lifelines */}
  <line x1="80"  y1="64" x2="80"  y2="490" stroke="#3a4050" strokeWidth="0.75" strokeDasharray="4,3"/>
  <line x1="270" y1="64" x2="270" y2="490" stroke="#3a4050" strokeWidth="0.75" strokeDasharray="4,3"/>
  <line x1="470" y1="64" x2="470" y2="490" stroke="#3a4050" strokeWidth="0.75" strokeDasharray="4,3"/>
  <line x1="630" y1="64" x2="630" y2="490" stroke="#3a4050" strokeWidth="0.75" strokeDasharray="4,3"/>

  {/* Actor Boxes */}
  {/* Client App */}
  <rect x="25" y="34" width="110" height="30" rx="5" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.6"/>
  <text x="80" y="53" textAnchor="middle" fill="#c3e04d" fontSize="8" fontWeight="600">Client App</text>

  {/* KeeperTailReader */}
  <rect x="205" y="34" width="130" height="30" rx="5" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.6"/>
  <text x="270" y="53" textAnchor="middle" fill="#c3e04d" fontSize="8" fontWeight="600">KeeperTailReader</text>

  {/* ChronoKeeper 1 */}
  <rect x="410" y="34" width="120" height="30" rx="5" fill="#252b3b" stroke="#4a90a4" strokeWidth="1" strokeOpacity="0.6"/>
  <text x="470" y="53" textAnchor="middle" fill="#4a90a4" fontSize="8" fontWeight="600">ChronoKeeper 1</text>

  {/* ChronoKeeper 2 */}
  <rect x="570" y="34" width="120" height="30" rx="5" fill="#252b3b" stroke="#4a90a4" strokeWidth="1" strokeOpacity="0.6"/>
  <text x="630" y="53" textAnchor="middle" fill="#4a90a4" fontSize="8" fontWeight="600">ChronoKeeper 2</text>

  {/* 1. App calls playback(N) */}
  <line x1="80" y1="84" x2="266" y2="84" stroke="#c3e04d" strokeWidth="0.8"/>
  <polygon points="268,84 262,80 262,88" fill="#c3e04d"/>
  <text x="175" y="78" textAnchor="middle" fill="#e4e7ed" fontSize="7">{"playback(N, events)"}</text>

  {/* Phase 1 Box (Scatter: Concurrent Key Lookup) */}
  <rect x="180" y="98" width="510" height="114" rx="6" fill="#1e2330" fillOpacity="0.6" stroke="#4a90a4" strokeWidth="0.75" strokeDasharray="5,3" strokeOpacity="0.4"/>
  <text x="195" y="112" fill="#4a90a4" fontSize="7" fontWeight="600">{"Phase 1 (Scatter): Concurrent Key Lookup (RPC Timeout: 5000ms)"}</text>

  {/* TR -> K1 tail_get_sequences */}
  <line x1="270" y1="126" x2="466" y2="126" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.8"/>
  <polygon points="468,126 462,122 462,130" fill="#c3e04d"/>
  <text x="365" y="121" textAnchor="middle" fill="#c3e04d" fontSize="6.5">tail_get_sequences(story_id, N)</text>

  {/* TR -> K2 tail_get_sequences */}
  <line x1="270" y1="146" x2="626" y2="146" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.8"/>
  <polygon points="628,146 622,142 622,150" fill="#c3e04d"/>
  <text x="445" y="141" textAnchor="middle" fill="#c3e04d" fontSize="6.5">tail_get_sequences(story_id, N)</text>

  {/* K1 -> TR response */}
  <line x1="470" y1="168" x2="274" y2="168" stroke="#9ca3b0" strokeWidth="0.75" strokeDasharray="3,2"/>
  <polygon points="272,168 278,164 278,172" fill="#9ca3b0"/>
  <text x="370" y="163" textAnchor="middle" fill="#9ca3b0" fontSize="6.5">{"up to N EventSequence keys (pinned)"}</text>

  {/* K2 -> TR response */}
  <line x1="630" y1="190" x2="274" y2="190" stroke="#9ca3b0" strokeWidth="0.75" strokeDasharray="3,2"/>
  <polygon points="272,190 278,186 278,194" fill="#9ca3b0"/>
  <text x="450" y="185" textAnchor="middle" fill="#9ca3b0" fontSize="6.5">{"up to N EventSequence keys (pinned)"}</text>

  {/* Global Selection Box on KeeperTailReader */}
  <rect x="200" y="222" width="140" height="42" rx="4" fill="#252b3b" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.7"/>
  <text x="270" y="236" textAnchor="middle" fill="#c3e04d" fontSize="6.5" fontWeight="600">Client Key Selection</text>
  <text x="270" y="247" textAnchor="middle" fill="#e4e7ed" fontSize="6">Sort all keys globally</text>
  <text x="270" y="256" textAnchor="middle" fill="#9ca3b0" fontSize="6">Select top N; group by keeper</text>

  {/* Phase 2 Box (Gather: Fetch Winning Payloads) */}
  <rect x="180" y="272" width="510" height="116" rx="6" fill="#1e2330" fillOpacity="0.6" stroke="#4a90a4" strokeWidth="0.75" strokeDasharray="5,3" strokeOpacity="0.4"/>
  <text x="195" y="286" fill="#4a90a4" fontSize="7" fontWeight="600">{"Phase 2 (Gather): Fetch Winning Payloads (Exact N Payloads Across Group)"}</text>

  {/* TR -> K1 tail_get_events */}
  <line x1="270" y1="302" x2="466" y2="302" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.8"/>
  <polygon points="468,302 462,298 462,306" fill="#c3e04d"/>
  <text x="365" y="297" textAnchor="middle" fill="#c3e04d" fontSize="6.5">tail_get_events(story_id, seqs_K1)</text>

  {/* TR -> K2 tail_get_events */}
  <line x1="270" y1="324" x2="626" y2="324" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.8"/>
  <polygon points="628,324 622,320 622,328" fill="#c3e04d"/>
  <text x="445" y="319" textAnchor="middle" fill="#c3e04d" fontSize="6.5">tail_get_events(story_id, seqs_K2)</text>

  {/* K1 -> TR response */}
  <line x1="470" y1="348" x2="274" y2="348" stroke="#9ca3b0" strokeWidth="0.75" strokeDasharray="3,2"/>
  <polygon points="272,348 278,344 278,352" fill="#9ca3b0"/>
  <text x="370" y="343" textAnchor="middle" fill="#9ca3b0" fontSize="6.5">LogEvent payloads for winning keys</text>

  {/* K2 -> TR response */}
  <line x1="630" y1="370" x2="274" y2="370" stroke="#9ca3b0" strokeWidth="0.75" strokeDasharray="3,2"/>
  <polygon points="272,370 278,366 278,374" fill="#9ca3b0"/>
  <text x="450" y="365" textAnchor="middle" fill="#9ca3b0" fontSize="6.5">LogEvent payloads for winning keys</text>

  {/* Deduplicate & Assemble Box on KeeperTailReader */}
  <rect x="200" y="398" width="140" height="38" rx="4" fill="#252b3b" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.7"/>
  <text x="270" y="412" textAnchor="middle" fill="#c3e04d" fontSize="6.5" fontWeight="600">Assemble &amp; Deduplicate</text>
  <text x="270" y="423" textAnchor="middle" fill="#9ca3b0" fontSize="6">{"std::map<EventSequence, Event>"}</text>

  {/* Return std::vector<Event> to App */}
  <line x1="270" y1="450" x2="84" y2="450" stroke="#c3e04d" strokeWidth="0.85"/>
  <polygon points="82,450 88,446 88,454" fill="#c3e04d"/>
  <text x="175" y="444" textAnchor="middle" fill="#e4e7ed" fontSize="7">{"std::vector<Event> (ascending order)"}</text>

  {/* Bottom Summary Annotation */}
  <text x="360" y="482" textAnchor="middle" fill="#9ca3b0" fontSize="6.5" fontStyle="italic">{"Bounded latency (max keeper time) \u2022 Exactly N payloads over wire \u2022 No redundant payload transfer"}</text>
</svg>

### Phase 1 — Scatter (Keys Only)
1. The client issues asynchronous `tail_get_sequences(story_id, n)` RPCs concurrently to all keepers assigned to the story.
2. Each keeper queries `KeeperTailStore::getTailSequences` and returns up to $n$ newest `EventSequence` keys (each a tuple of `(chrono_time, clientId, index)`).
3. Total Phase 1 latency is bounded by the slowest keeper rather than the sum, and per-RPC timeouts (`kTailReadRpcTimeoutMs = 5000ms`) isolate failed keepers.

### Selection
The client merges all returned keys into a single globally-ordered collection, picks the largest $N$ sequences, and groups them by their owning keeper.

### Phase 2 — Gather (Payloads for Winners Only)
1. The client issues concurrent `tail_get_events(story_id, selected_seqs)` RPCs to each keeper that holds winning keys.
2. Each keeper retrieves the specific `LogEvent` payloads using `KeeperTailStore::getTailEvents`.
3. The client inserts returned events into a `std::map<EventSequence, Event>` to ensure cross-keeper deduplication and ascending ordering, returning a `std::vector<Event>`.

**Network Guarantee**: Exactly $N$ event payloads are transmitted across the network, independent of the number of keepers in the recording group.

---

## 4. Retention Lifecycle & Starvation Prevention

Retaining sealed chunks in keeper memory introduces the need for robust memory bounds and archival guarantees:

### Archival Safety Mechanisms
1. **Time-Based Age-Out (`tail_retention_secs`, default 60s)**:
   Sealed chunks age out after `tail_retention_secs` beyond their end time and are forwarded to the extraction queue. This ensures low-volume stories that never fill `tail_capacity` are still archived to persistent storage.
2. **Capacity Eviction (`tail_capacity`, default 65536)**:
   When total retained events for a story exceed `tail_capacity`, oldest events are evicted immediately. Once a chunk has zero remaining indexed events, it is stashed to the extraction queue.
3. **Pin Protection (`kPinTicks`) with Anti-Starvation (`pinDeferredTicks`)**:
   - When Phase 1 returns sequences from a chunk, that chunk is **pinned** for `kPinTicks` (~3 maintenance ticks / ~3 seconds) so a concurrent age-out does not delete the payload before Phase 2 arrives.
   - To prevent rapid polling from permanently deferring archival, pins defer age-out for at most `kPinTicks` consecutive ticks per story (`pinDeferredTicks`). Archival proceeds regardless of active reads.
4. **Shutdown Flush (`flushRetainedChunks`)**:
   During clean shutdown, `flushRetainedChunks()` explicitly hands all retained chunks to the extraction queue before extraction threads are stopped, preventing data loss.

---

## 5. Live Tail from Unsealed Chunks (`live_tail_read`)

By default, events become visible in `playback()` once their chunk seals (`chunk_duration + acceptance_window`, ~25–30s). For latency-critical streaming applications, ChronoKeeper provides the optional **`live_tail_read`** setting.

- **Mechanism**: `KeeperStoryPipeline` implements the `ActiveTailSource` interface, registering with `KeeperTailStore`. When enabled, `getTailSequences` unions the sealed tail with the unsealed active timeline (`storyTimelineMap`), and `getTailEvents` falls back to `findActiveEvent()` under `sequencingMutex`.
- **Visibility Latency**: Send-to-visible latency drops from ~20 seconds to **~0.5 s on average, bounded by ~1 s**. An event becomes readable only once `collectIngestedEvents()` merges it into `storyTimelineMap`, and that maintenance tick runs once per second, so the tick interval — not the RPC — sets this floor.
- **Provisional Ordering**: Because events inside the active acceptance window arrive out of order from distributed clients, active window reads are provisional (a late event may sort behind an already-seen event until sealed).

---

## 6. Orphan Event & Chunk Recovery

To prevent data loss when stories are released, retired, or unhooked:
- **Keeper Orphan Recovery (`sealOrphanedEvents`)**: Late client events arriving after a story pipeline has decayed are rescued from `IngestionQueue`'s orphan queue, sealed into a recovery `StoryChunk` with retained chronicle/story names, and sent for archival.
- **Grapher Orphan Adoption (`adoptOrphanChunks`)**: If ChronoGrapher receives an orphan chunk for an unregistered story, it adopts the chunk, instantiates a temporary pipeline using the chunk's embedded metadata, archives it to HDF5, and schedules graceful retirement.

---

## 7. Performance Characteristics

| Metric | Persistent Replay (`ReplayStory`) | Sealed Tail Read (`playback`) | Live Tail Read (`live_tail_read = true`) |
|---|---|---|---|
| **Query Latency** | ~1000–2500 ms (disk / HDF5) | **~0.4 ms median, 1–5 ms p90–p99** (keeper RAM) | **~0.4 ms median, 1–5 ms p90–p99** (keeper RAM) |
| **Visibility Delay** | After HDF5 archival (~60–240 s) | After chunk seal (~25–30 s) | **~0.5 s mean, ~1 s max** (one ingestion tick) |
| **Network Overhead** | Full range transfer via Player | Exactly $N$ payloads | Exactly $N$ payloads |
| **Consistency** | Strictly ordered / final | Strictly ordered / final | Provisional within acceptance window |
