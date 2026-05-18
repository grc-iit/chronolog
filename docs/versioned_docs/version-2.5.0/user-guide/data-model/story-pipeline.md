---
sidebar_position: 3
title: "Distributed Story Pipeline"
---

# Distributed Story Pipeline

The Distributed Story Pipeline is the data model that drives the design of **ChronoKeeper** and **ChronoGrapher**. It describes how log events travel from client applications through a multi-tier collection pipeline — being progressively grouped, merged, and archived — until they reach persistent storage. At any moment in time, segments of a Story's pipeline can be distributed across multiple processes, nodes, and storage tiers.

## Pipeline at a Glance

The diagram below shows a point-in-time snapshot of two stories (S1 and S2) as their data flows through the four tiers of the pipeline. Time flows from right (newest events being generated) to left (oldest data, already archived).

<svg viewBox="0 0 720 400" width="100%" xmlns="http://www.w3.org/2000/svg" fontFamily="system-ui, sans-serif">
  <rect x="0" y="0" width="720" height="400" rx="10" fill="#1e2330"/>

  {/* Title */}
  <text x="360" y="18" textAnchor="middle" fill="#c3e04d" fontSize="10" fontWeight="600">DISTRIBUTED PIPELINE SNAPSHOT</text>

  {/* Time axis */}
  <line x1="30" y1="34" x2="700" y2="34" stroke="#9ca3b0" strokeWidth="0.5" strokeOpacity="0.6"/>
  <polygon points="30,34 36,31 36,37" fill="#9ca3b0" fillOpacity="0.6"/>
  <text x="700" y="44" textAnchor="end" fill="#9ca3b0" fontSize="6" fontStyle="italic">now</text>
  <text x="30" y="44" fill="#9ca3b0" fontSize="6" fontStyle="italic">older</text>
  {/* Tick marks */}
  <line x1="100" y1="31" x2="100" y2="37" stroke="#9ca3b0" strokeWidth="0.5" strokeOpacity="0.4"/>
  <text x="100" y="28" textAnchor="middle" fill="#9ca3b0" fontSize="5" fillOpacity="0.5">t0</text>
  <line x1="270" y1="31" x2="270" y2="37" stroke="#9ca3b0" strokeWidth="0.5" strokeOpacity="0.4"/>
  <text x="270" y="28" textAnchor="middle" fill="#9ca3b0" fontSize="5" fillOpacity="0.5">t10</text>
  <line x1="440" y1="31" x2="440" y2="37" stroke="#9ca3b0" strokeWidth="0.5" strokeOpacity="0.4"/>
  <text x="440" y="28" textAnchor="middle" fill="#9ca3b0" fontSize="5" fillOpacity="0.5">t20</text>
  <line x1="610" y1="31" x2="610" y2="37" stroke="#9ca3b0" strokeWidth="0.5" strokeOpacity="0.4"/>
  <text x="610" y="28" textAnchor="middle" fill="#9ca3b0" fontSize="5" fillOpacity="0.5">t30</text>

  {/* === TIER LABELS (bottom) === */}
  <text x="68" y="392" textAnchor="middle" fill="#9ca3b0" fontSize="6" fontStyle="italic">Persistent Tier</text>
  <text x="230" y="392" textAnchor="middle" fill="#9ca3b0" fontSize="6" fontStyle="italic">ChronoGrapher</text>
  <text x="440" y="392" textAnchor="middle" fill="#9ca3b0" fontSize="6" fontStyle="italic">ChronoKeepers</text>
  <text x="650" y="392" textAnchor="middle" fill="#9ca3b0" fontSize="6" fontStyle="italic">Clients</text>

  {/* === PERSISTENT TIER (left) === */}
  <rect x="14" y="54" width="110" height="326" rx="6" fill="#252b3b" stroke="#3a4050" strokeWidth="0.75"/>
  <circle cx="28" cy="68" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="36" y="72" fill="#e4e7ed" fontSize="8" fontWeight="600">Persistent Tier</text>
  <text x="24" y="84" fill="#9ca3b0" fontSize="6">HDF5 archives</text>

  {/* Archived chunks */}
  <rect x="24" y="96" width="90" height="20" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <text x="69" y="109" textAnchor="middle" fill="#9ca3b0" fontSize="5">S1 Chunk t0–t5</text>

  <rect x="24" y="122" width="90" height="20" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <text x="69" y="135" textAnchor="middle" fill="#9ca3b0" fontSize="5">S1 Chunk t5–t10</text>

  <rect x="24" y="148" width="90" height="20" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <text x="69" y="161" textAnchor="middle" fill="#9ca3b0" fontSize="5">S2 Chunk t3–t9</text>

  <rect x="24" y="174" width="90" height="20" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <text x="69" y="187" textAnchor="middle" fill="#9ca3b0" fontSize="5">S2 Chunk t9–t15</text>

  {/* === ARROW: Persistent ← Grapher === */}
  <line x1="124" y1="200" x2="150" y2="200" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="124,200 130,196 130,204" fill="#c3e04d" fillOpacity="0.5"/>
  <text x="137" y="214" textAnchor="middle" fill="#9ca3b0" fontSize="5" fontStyle="italic">HDF5</text>
  <text x="137" y="221" textAnchor="middle" fill="#9ca3b0" fontSize="5" fontStyle="italic">archive</text>

  {/* === CHRONOGRAPHER (center-left) === */}
  <rect x="152" y="54" width="160" height="326" rx="6" fill="none" stroke="#4a90a4" strokeWidth="0.75" strokeDasharray="6,3" strokeOpacity="0.4"/>
  <text x="164" y="68" fill="#4a90a4" fontSize="8" fontWeight="600" fillOpacity="0.7">CHRONOGRAPHER</text>
  <text x="164" y="80" fill="#9ca3b0" fontSize="6">Merged complete chunks</text>

  {/* Story 1 section */}
  <rect x="162" y="92" width="140" height="100" rx="4" fill="#252b3b" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.4"/>
  <text x="232" y="106" textAnchor="middle" fill="#c3e04d" fontSize="7" fontWeight="600">Story 1 Pipeline</text>

  <rect x="172" y="114" width="56" height="18" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <text x="200" y="126" textAnchor="middle" fill="#9ca3b0" fontSize="5">S1 t10–t15</text>

  <rect x="236" y="114" width="56" height="18" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <text x="264" y="126" textAnchor="middle" fill="#9ca3b0" fontSize="5">S1 t15–t20</text>

  <rect x="172" y="140" width="56" height="18" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <text x="200" y="152" textAnchor="middle" fill="#9ca3b0" fontSize="5">S1 t20–t25</text>

  <text x="232" y="176" textAnchor="middle" fill="#c3e04d" fontSize="5" fillOpacity="0.5">complete (all clients merged)</text>

  {/* Story 2 section */}
  <rect x="162" y="198" width="140" height="86" rx="4" fill="#252b3b" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.4"/>
  <text x="232" y="212" textAnchor="middle" fill="#c3e04d" fontSize="7" fontWeight="600">Story 2 Pipeline</text>

  <rect x="172" y="220" width="56" height="18" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <text x="200" y="232" textAnchor="middle" fill="#9ca3b0" fontSize="5">S2 t15–t21</text>

  <rect x="236" y="220" width="56" height="18" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <text x="264" y="232" textAnchor="middle" fill="#9ca3b0" fontSize="5">S2 t21–t27</text>

  <text x="232" y="272" textAnchor="middle" fill="#c3e04d" fontSize="5" fillOpacity="0.5">complete (all clients merged)</text>

  {/* === ARROW: Grapher ← Keepers === */}
  <line x1="312" y1="200" x2="338" y2="200" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="312,200 318,196 318,204" fill="#c3e04d" fillOpacity="0.5"/>
  <text x="325" y="214" textAnchor="middle" fill="#9ca3b0" fontSize="5" fontStyle="italic">RDMA</text>
  <text x="325" y="221" textAnchor="middle" fill="#9ca3b0" fontSize="5" fontStyle="italic">bulk</text>

  {/* === CHRONOKEEPERS (center-right) === */}
  <rect x="340" y="54" width="210" height="326" rx="6" fill="none" stroke="#4a90a4" strokeWidth="0.75" strokeDasharray="6,3" strokeOpacity="0.4"/>
  <text x="352" y="68" fill="#4a90a4" fontSize="8" fontWeight="600" fillOpacity="0.7">CHRONOKEEPERS</text>
  <text x="352" y="80" fill="#9ca3b0" fontSize="6">Partial chunks (subset of events)</text>

  {/* Keeper 1 */}
  <rect x="350" y="92" width="190" height="120" rx="4" fill="#252b3b" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.4"/>
  <circle cx="362" cy="106" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="370" y="110" fill="#c3e04d" fontSize="7" fontWeight="600">ChronoKeeper 1</text>

  <rect x="360" y="120" width="50" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <text x="385" y="131" textAnchor="middle" fill="#9ca3b0" fontSize="5">S1 t25–30</text>
  <rect x="416" y="120" width="50" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <text x="441" y="131" textAnchor="middle" fill="#9ca3b0" fontSize="5">S1 t30–35</text>
  <rect x="472" y="120" width="50" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <text x="497" y="131" textAnchor="middle" fill="#9ca3b0" fontSize="5">S2 t27–33</text>

  {/* Individual events arriving into Keeper 1 */}
  <circle cx="370" y="152" r="2" fill="#c3e04d" fillOpacity="0.3"/>
  <circle cx="382" y="150" r="2" fill="#c3e04d" fillOpacity="0.3"/>
  <circle cx="394" y="154" r="2" fill="#c3e04d" fillOpacity="0.3"/>
  <text x="445" y="156" textAnchor="middle" fill="#9ca3b0" fontSize="5" fontStyle="italic">events from Client 1</text>

  <text x="445" y="200" textAnchor="middle" fill="#c3e04d" fontSize="5" fillOpacity="0.5">partial (Client 1 events only)</text>

  {/* Keeper 2 */}
  <rect x="350" y="218" width="190" height="120" rx="4" fill="#252b3b" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.4"/>
  <circle cx="362" cy="232" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="370" y="236" fill="#c3e04d" fontSize="7" fontWeight="600">ChronoKeeper 2</text>

  <rect x="360" y="246" width="50" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <text x="385" y="257" textAnchor="middle" fill="#9ca3b0" fontSize="5">S1 t25–30</text>
  <rect x="416" y="246" width="50" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <text x="441" y="257" textAnchor="middle" fill="#9ca3b0" fontSize="5">S1 t30–35</text>
  <rect x="472" y="246" width="50" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <text x="497" y="257" textAnchor="middle" fill="#9ca3b0" fontSize="5">S2 t27–33</text>

  <circle cx="370" y="278" r="2" fill="#c3e04d" fillOpacity="0.3"/>
  <circle cx="382" y="276" r="2" fill="#c3e04d" fillOpacity="0.3"/>
  <circle cx="394" y="280" r="2" fill="#c3e04d" fillOpacity="0.3"/>
  <text x="445" y="282" textAnchor="middle" fill="#9ca3b0" fontSize="5" fontStyle="italic">events from Client 2</text>

  <text x="445" y="326" textAnchor="middle" fill="#c3e04d" fontSize="5" fillOpacity="0.5">partial (Client 2 events only)</text>

  {/* === ARROW: Keepers ← Clients === */}
  <line x1="550" y1="200" x2="576" y2="200" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="550,200 556,196 556,204" fill="#c3e04d" fillOpacity="0.5"/>
  <text x="563" y="214" textAnchor="middle" fill="#9ca3b0" fontSize="5" fontStyle="italic">{"log_event()"}</text>
  <text x="563" y="221" textAnchor="middle" fill="#9ca3b0" fontSize="5" fontStyle="italic">RPC</text>

  {/* === CLIENTS (right) === */}
  <rect x="578" y="92" width="128" height="120" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.5"/>
  <circle cx="592" cy="106" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="600" y="110" fill="#c3e04d" fontSize="8" fontWeight="600">Client 1</text>
  <text x="588" y="126" fill="#9ca3b0" fontSize="6">Generating events for</text>
  <text x="588" y="135" fill="#9ca3b0" fontSize="6">Story 1 and Story 2</text>
  {/* Event dots */}
  <circle cx="598" cy="152" r="2.5" fill="#c3e04d" fillOpacity="0.6"/>
  <text x="608" y="155" fill="#9ca3b0" fontSize="4.5">S1 e1</text>
  <circle cx="598" cy="164" r="2.5" fill="#c3e04d" fillOpacity="0.6"/>
  <text x="608" y="167" fill="#9ca3b0" fontSize="4.5">S2 e2</text>
  <circle cx="598" cy="176" r="2.5" fill="#c3e04d" fillOpacity="0.6"/>
  <text x="608" y="179" fill="#9ca3b0" fontSize="4.5">S1 e3</text>
  <circle cx="598" cy="188" r="2.5" fill="#c3e04d" fillOpacity="0.6"/>
  <text x="608" y="191" fill="#9ca3b0" fontSize="4.5">S1 e4</text>

  <rect x="578" y="218" width="128" height="120" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.5"/>
  <circle cx="592" cy="232" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="600" y="236" fill="#c3e04d" fontSize="8" fontWeight="600">Client 2</text>
  <text x="588" y="252" fill="#9ca3b0" fontSize="6">Generating events for</text>
  <text x="588" y="261" fill="#9ca3b0" fontSize="6">Story 1 and Story 2</text>
  {/* Event dots */}
  <circle cx="598" cy="278" r="2.5" fill="#c3e04d" fillOpacity="0.6"/>
  <text x="608" y="281" fill="#9ca3b0" fontSize="4.5">S1 e5</text>
  <circle cx="598" cy="290" r="2.5" fill="#c3e04d" fillOpacity="0.6"/>
  <text x="608" y="293" fill="#9ca3b0" fontSize="4.5">S1 e6</text>
  <circle cx="598" cy="302" r="2.5" fill="#c3e04d" fillOpacity="0.6"/>
  <text x="608" y="305" fill="#9ca3b0" fontSize="4.5">S2 e7</text>
  <circle cx="598" cy="314" r="2.5" fill="#c3e04d" fillOpacity="0.6"/>
  <text x="608" y="317" fill="#9ca3b0" fontSize="4.5">S2 e8</text>
</svg>

Reading right-to-left, each tier plays a distinct role:

- **Clients** generate individual LogEvents and stream them to ChronoKeepers via RPC.
- **ChronoKeepers** (compute nodes) group incoming events into time-windowed **partial StoryChunks**. Each Keeper only sees the events sent to it, so its chunks contain a subset of the full story.
- **ChronoGrapher** (storage node) receives partial chunks from all Keepers and **merges** them into complete, globally-ordered StoryChunks.
- **Persistent Tier** stores the final, archived StoryChunks in HDF5 format on the POSIX filesystem.

At any instant, the newest data lives on the right (still being generated), while progressively older, more complete data has migrated left through the pipeline toward persistent storage.

## Building Blocks

Three core abstractions make up the pipeline data model.

### Story

A **Story** is a time-series dataset composed of individual log events that client processes running on various HPC nodes generate over time. Every log event is identified by the `StoryId` of the story it belongs to, the `ClientId` of the application that generated it, and a timestamp assigned by the ChronoLog client library at the moment it takes ownership of the event. To handle the case where the same timestamp is generated on different threads of a client application, ChronoLog also augments the timestamp with a client-specific index.

### StoryChunk

A **StoryChunk** is a container for events belonging to the same story that fall into the time range **[start_time, end_time)** — including the start time and excluding the end time. Events within a StoryChunk are ordered by their **EventSequence = (timestamp, ClientId, index)**.

StoryChunks are the fundamental unit of data movement through the pipeline. They travel from ChronoKeepers to ChronoGrapher as partial batches, get merged into complete batches, and are eventually archived to persistent storage.

### StoryPipeline

A **StoryPipeline** is the ordered set of StoryChunks for a given story, sorted by their `start_time` values. For any story with active writers, segments of the StoryPipeline can be distributed over several processes, nodes, and storage tiers simultaneously — as illustrated in the snapshot above.

## Parallel Ingestion

A key design goal of ChronoLog is to distribute the ingestion workload across many compute nodes. Rather than funneling all events through a single process, client applications spread their events across multiple ChronoKeepers using round-robin selection.

<svg viewBox="0 0 720 380" width="100%" xmlns="http://www.w3.org/2000/svg" fontFamily="system-ui, sans-serif">
  <rect x="0" y="0" width="720" height="380" rx="10" fill="#1e2330"/>

  {/* Title */}
  <text x="360" y="18" textAnchor="middle" fill="#c3e04d" fontSize="10" fontWeight="600">PARALLEL INGESTION AND MERGING</text>

  {/* === CLIENT ROW === */}
  <rect x="80" y="30" width="110" height="42" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.5"/>
  <circle cx="94" cy="46" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="102" y="50" fill="#c3e04d" fontSize="8" fontWeight="600">Client 1</text>
  <text x="94" y="64" fill="#9ca3b0" fontSize="5">events: e1, e4, e7</text>

  <rect x="305" y="30" width="110" height="42" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.5"/>
  <circle cx="319" cy="46" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="327" y="50" fill="#c3e04d" fontSize="8" fontWeight="600">Client 2</text>
  <text x="319" y="64" fill="#9ca3b0" fontSize="5">events: e2, e5, e8</text>

  <rect x="530" y="30" width="110" height="42" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.5"/>
  <circle cx="544" cy="46" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="552" y="50" fill="#c3e04d" fontSize="8" fontWeight="600">Client 3</text>
  <text x="544" y="64" fill="#9ca3b0" fontSize="5">events: e3, e6, e9</text>

  {/* === FAN-OUT ARROWS (clients → keepers) === */}
  {/* Client 1 → Keeper 1 */}
  <line x1="135" y1="72" x2="200" y2="118" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.4"/>
  <polygon points="200,120 194,116 198,110" fill="#c3e04d" fillOpacity="0.4"/>
  {/* Client 1 → Keeper 2 */}
  <line x1="135" y1="72" x2="470" y2="118" stroke="#c3e04d" strokeWidth="0.5" strokeOpacity="0.25" strokeDasharray="4,2"/>

  {/* Client 2 → Keeper 1 */}
  <line x1="360" y1="72" x2="250" y2="118" stroke="#c3e04d" strokeWidth="0.5" strokeOpacity="0.25" strokeDasharray="4,2"/>
  {/* Client 2 → Keeper 2 */}
  <line x1="360" y1="72" x2="520" y2="118" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.4"/>
  <polygon points="520,120 514,116 518,110" fill="#c3e04d" fillOpacity="0.4"/>

  {/* Client 3 → Keeper 1 */}
  <line x1="585" y1="72" x2="280" y2="118" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.4"/>
  <polygon points="280,120 274,116 278,110" fill="#c3e04d" fillOpacity="0.4"/>
  {/* Client 3 → Keeper 2 */}
  <line x1="585" y1="72" x2="550" y2="118" stroke="#c3e04d" strokeWidth="0.5" strokeOpacity="0.25" strokeDasharray="4,2"/>

  {/* Round-robin label */}
  <text x="360" y="98" textAnchor="middle" fill="#9ca3b0" fontSize="6" fontStyle="italic">round-robin distribution (solid = current, dashed = next rotation)</text>

  {/* === KEEPER ROW === */}
  {/* Keeper 1 */}
  <rect x="60" y="120" width="290" height="100" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.5"/>
  <circle cx="76" cy="136" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="84" y="140" fill="#c3e04d" fontSize="8" fontWeight="600">ChronoKeeper 1</text>

  {/* Story 1 Pipeline in Keeper 1 */}
  <rect x="76" y="150" width="260" height="42" rx="4" fill="none" stroke="#4a90a4" strokeWidth="0.5" strokeDasharray="4,2" strokeOpacity="0.3"/>
  <text x="86" y="162" fill="#4a90a4" fontSize="6" fontWeight="600" fillOpacity="0.6">Story 1 Pipeline</text>

  <rect x="86" y="168" width="70" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <text x="121" y="179" textAnchor="middle" fill="#9ca3b0" fontSize="5">S1 t0–30 (partial)</text>

  <rect x="164" y="168" width="70" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <text x="199" y="179" textAnchor="middle" fill="#9ca3b0" fontSize="5">S1 t30–60 (partial)</text>

  <text x="205" y="206" textAnchor="middle" fill="#9ca3b0" fontSize="5" fontStyle="italic">contains events from Client 1 + Client 3 only</text>

  {/* Keeper 2 */}
  <rect x="370" y="120" width="290" height="100" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.5"/>
  <circle cx="386" cy="136" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="394" y="140" fill="#c3e04d" fontSize="8" fontWeight="600">ChronoKeeper 2</text>

  {/* Story 1 Pipeline in Keeper 2 */}
  <rect x="386" y="150" width="260" height="42" rx="4" fill="none" stroke="#4a90a4" strokeWidth="0.5" strokeDasharray="4,2" strokeOpacity="0.3"/>
  <text x="396" y="162" fill="#4a90a4" fontSize="6" fontWeight="600" fillOpacity="0.6">Story 1 Pipeline</text>

  <rect x="396" y="168" width="70" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <text x="431" y="179" textAnchor="middle" fill="#9ca3b0" fontSize="5">S1 t0–30 (partial)</text>

  <rect x="474" y="168" width="70" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <text x="509" y="179" textAnchor="middle" fill="#9ca3b0" fontSize="5">S1 t30–60 (partial)</text>

  <text x="515" y="206" textAnchor="middle" fill="#9ca3b0" fontSize="5" fontStyle="italic">contains events from Client 2 + Client 3 only</text>

  {/* === FAN-IN ARROWS (keepers → grapher) === */}
  <line x1="205" y1="220" x2="280" y2="248" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="280,250 274,246 278,240" fill="#c3e04d" fillOpacity="0.5"/>

  <line x1="515" y1="220" x2="440" y2="248" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="440,250 436,240 444,246" fill="#c3e04d" fillOpacity="0.5"/>

  <text x="360" y="240" textAnchor="middle" fill="#9ca3b0" fontSize="6" fontStyle="italic">RDMA bulk transfer</text>

  {/* === GRAPHER ROW === */}
  <rect x="60" y="252" width="600" height="90" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.5"/>
  <circle cx="76" cy="268" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="84" y="272" fill="#c3e04d" fontSize="8" fontWeight="600">ChronoGrapher</text>
  <text x="240" y="272" fill="#9ca3b0" fontSize="7">merges partial chunks into complete, globally-ordered StoryChunks</text>

  {/* Merge visualization: partial chunks → complete chunks */}
  {/* Left side: partial inputs */}
  <rect x="80" y="284" width="80" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5" strokeOpacity="0.5"/>
  <text x="120" y="295" textAnchor="middle" fill="#9ca3b0" fontSize="5" fillOpacity="0.6">K1: S1 t0–30</text>

  <rect x="80" y="304" width="80" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5" strokeOpacity="0.5"/>
  <text x="120" y="315" textAnchor="middle" fill="#9ca3b0" fontSize="5" fillOpacity="0.6">K2: S1 t0–30</text>

  {/* Merge arrow */}
  <line x1="160" y1="300" x2="200" y2="300" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="202,300 196,296 196,304" fill="#c3e04d" fillOpacity="0.5"/>
  <text x="180" y="330" textAnchor="middle" fill="#c3e04d" fontSize="5" fillOpacity="0.6">merge</text>

  {/* Complete chunk result */}
  <rect x="204" y="290" width="100" height="20" rx="2" fill="#1e2330" stroke="#c3e04d" strokeWidth="0.5" strokeOpacity="0.4"/>
  <text x="254" y="303" textAnchor="middle" fill="#c3e04d" fontSize="5">S1 t0–30 (complete)</text>

  {/* Second merge pair */}
  <rect x="340" y="284" width="80" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5" strokeOpacity="0.5"/>
  <text x="380" y="295" textAnchor="middle" fill="#9ca3b0" fontSize="5" fillOpacity="0.6">K1: S1 t30–60</text>

  <rect x="340" y="304" width="80" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5" strokeOpacity="0.5"/>
  <text x="380" y="315" textAnchor="middle" fill="#9ca3b0" fontSize="5" fillOpacity="0.6">K2: S1 t30–60</text>

  {/* Merge arrow */}
  <line x1="420" y1="300" x2="460" y2="300" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="462,300 456,296 456,304" fill="#c3e04d" fillOpacity="0.5"/>
  <text x="440" y="330" textAnchor="middle" fill="#c3e04d" fontSize="5" fillOpacity="0.6">merge</text>

  {/* Complete chunk result */}
  <rect x="464" y="290" width="100" height="20" rx="2" fill="#1e2330" stroke="#c3e04d" strokeWidth="0.5" strokeOpacity="0.4"/>
  <text x="514" y="303" textAnchor="middle" fill="#c3e04d" fontSize="5">S1 t30–60 (complete)</text>

  {/* === ARCHIVE ARROW === */}
  <line x1="360" y1="342" x2="360" y2="356" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="360,358 356,352 364,352" fill="#c3e04d" fillOpacity="0.5"/>

  {/* HDF5 box */}
  <rect x="290" y="358" width="140" height="18" rx="4" fill="#252b3b" stroke="#3a4050" strokeWidth="0.75"/>
  <text x="360" y="370" textAnchor="middle" fill="#e4e7ed" fontSize="7" fontWeight="600">HDF5 Persistent Storage</text>
</svg>

Because each client can send events to any ChronoKeeper, each Keeper involved in the same story recording can only have a **subset** of the story's events for a given time range. These are called **partial StoryChunks**.

When the ChronoGrapher receives partial StoryChunks for the same story and overlapping time ranges from different Keepers, it **merges** them into complete StoryChunks that contain events from all clients, sorted by EventSequence. StoryChunks in the ChronoGrapher's pipeline typically use longer time ranges (coarser granularity) than those in the ChronoKeepers, since the Grapher operates at a slower, batch-oriented cadence.

## Acceptance Windows

Each tier in the pipeline holds onto its StoryChunks for a configurable **acceptance window** before forwarding them downstream. This window provides tolerance for network delays and late-arriving events — an event that arrives slightly after its chunk's time range has ended can still be inserted into the correct chunk, as long as the acceptance window has not yet expired.

<svg viewBox="0 0 720 260" width="100%" xmlns="http://www.w3.org/2000/svg" fontFamily="system-ui, sans-serif">
  <rect x="0" y="0" width="720" height="260" rx="10" fill="#1e2330"/>

  {/* Title */}
  <text x="360" y="18" textAnchor="middle" fill="#c3e04d" fontSize="10" fontWeight="600">ACCEPTANCE WINDOW AND CHUNK LIFECYCLE</text>

  {/* Timeline axis */}
  <line x1="140" y1="38" x2="700" y2="38" stroke="#9ca3b0" strokeWidth="0.5" strokeOpacity="0.6"/>
  <polygon points="700,38 694,35 694,41" fill="#9ca3b0" fillOpacity="0.6"/>
  <text x="700" y="32" textAnchor="end" fill="#9ca3b0" fontSize="6" fontStyle="italic">time</text>

  {/* Tick marks */}
  <line x1="160" y1="35" x2="160" y2="41" stroke="#9ca3b0" strokeWidth="0.5" strokeOpacity="0.5"/>
  <text x="160" y="50" textAnchor="middle" fill="#9ca3b0" fontSize="5">t=0s</text>

  <line x1="280" y1="35" x2="280" y2="41" stroke="#9ca3b0" strokeWidth="0.5" strokeOpacity="0.5"/>
  <text x="280" y="50" textAnchor="middle" fill="#9ca3b0" fontSize="5">t=30s</text>

  <line x1="400" y1="35" x2="400" y2="41" stroke="#9ca3b0" strokeWidth="0.5" strokeOpacity="0.5"/>
  <text x="400" y="50" textAnchor="middle" fill="#9ca3b0" fontSize="5">t=60s</text>

  <line x1="520" y1="35" x2="520" y2="41" stroke="#9ca3b0" strokeWidth="0.5" strokeOpacity="0.5"/>
  <text x="520" y="50" textAnchor="middle" fill="#9ca3b0" fontSize="5">t=120s</text>

  <line x1="640" y1="35" x2="640" y2="41" stroke="#9ca3b0" strokeWidth="0.5" strokeOpacity="0.5"/>
  <text x="640" y="50" textAnchor="middle" fill="#9ca3b0" fontSize="5">t=240s</text>

  {/* Vertical phase transition lines */}
  <line x1="280" y1="55" x2="280" y2="110" stroke="#3a4050" strokeWidth="0.5" strokeDasharray="3,2"/>
  <line x1="400" y1="55" x2="400" y2="160" stroke="#3a4050" strokeWidth="0.5" strokeDasharray="3,2"/>
  <line x1="640" y1="55" x2="640" y2="230" stroke="#3a4050" strokeWidth="0.5" strokeDasharray="3,2"/>

  {/* === Row 1: Chunk Duration === */}
  <text x="130" y="72" textAnchor="end" fill="#c3e04d" fontSize="7" fontWeight="600">Chunk</text>
  <text x="130" y="81" textAnchor="end" fill="#c3e04d" fontSize="7" fontWeight="600">Duration</text>
  <rect x="160" y="60" width="120" height="28" rx="4" fill="#252b3b" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  {/* Event dots arriving over time */}
  <circle cx="172" cy="74" r="2" fill="#c3e04d" fillOpacity="0.5"/>
  <circle cx="185" cy="74" r="2" fill="#c3e04d" fillOpacity="0.5"/>
  <circle cx="196" cy="74" r="2" fill="#c3e04d" fillOpacity="0.5"/>
  <circle cx="210" cy="74" r="2" fill="#c3e04d" fillOpacity="0.5"/>
  <circle cx="222" cy="74" r="2" fill="#c3e04d" fillOpacity="0.5"/>
  <circle cx="237" cy="74" r="2" fill="#c3e04d" fillOpacity="0.5"/>
  <circle cx="250" cy="74" r="2" fill="#c3e04d" fillOpacity="0.5"/>
  <circle cx="265" cy="74" r="2" fill="#c3e04d" fillOpacity="0.5"/>
  <text x="220" y="84" textAnchor="middle" fill="#9ca3b0" fontSize="4.5" fontStyle="italic">events grouped into chunk</text>

  {/* === Row 2: Keeper Acceptance Window === */}
  <text x="130" y="108" textAnchor="end" fill="#c3e04d" fontSize="7" fontWeight="600">Keeper</text>
  <text x="130" y="117" textAnchor="end" fill="#c3e04d" fontSize="7" fontWeight="600">Window</text>
  <rect x="160" y="96" width="240" height="28" rx="4" fill="none" stroke="#4a90a4" strokeWidth="0.75" strokeDasharray="6,3" strokeOpacity="0.4"/>
  <text x="340" y="114" textAnchor="middle" fill="#9ca3b0" fontSize="5" fontStyle="italic">late events still accepted (network delay tolerance)</text>
  {/* Late event dots */}
  <circle cx="290" cy="106" r="2" fill="#c3e04d" fillOpacity="0.3"/>
  <circle cx="310" cy="106" r="2" fill="#c3e04d" fillOpacity="0.3"/>
  <circle cx="375" cy="106" r="2" fill="#c3e04d" fillOpacity="0.3"/>

  {/* === Row 3: Extract & Transfer === */}
  <text x="130" y="146" textAnchor="end" fill="#c3e04d" fontSize="7" fontWeight="600">Extract</text>
  <text x="130" y="155" textAnchor="end" fill="#c3e04d" fontSize="7" fontWeight="600">{"& Transfer"}</text>
  <rect x="400" y="134" width="50" height="28" rx="4" fill="#252b3b" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <text x="425" y="152" textAnchor="middle" fill="#9ca3b0" fontSize="5" fontStyle="italic">RDMA</text>
  {/* Arrow from window end to extract */}
  <line x1="400" y1="124" x2="400" y2="132" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="400,134 396,128 404,128" fill="#c3e04d" fillOpacity="0.5"/>

  {/* === Row 4: Grapher Acceptance Window === */}
  <text x="130" y="184" textAnchor="end" fill="#c3e04d" fontSize="7" fontWeight="600">Grapher</text>
  <text x="130" y="193" textAnchor="end" fill="#c3e04d" fontSize="7" fontWeight="600">Window</text>
  <rect x="450" y="172" width="190" height="28" rx="4" fill="none" stroke="#4a90a4" strokeWidth="0.75" strokeDasharray="6,3" strokeOpacity="0.4"/>
  <text x="545" y="190" textAnchor="middle" fill="#9ca3b0" fontSize="5" fontStyle="italic">merge partial chunks from all Keepers</text>
  {/* Arrow from extract to grapher */}
  <line x1="425" y1="162" x2="450" y2="180" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="450,182 444,178 448,174" fill="#c3e04d" fillOpacity="0.5"/>

  {/* === Row 5: Archive to HDF5 === */}
  <text x="130" y="224" textAnchor="end" fill="#c3e04d" fontSize="7" fontWeight="600">Archive</text>
  <text x="130" y="233" textAnchor="end" fill="#c3e04d" fontSize="7" fontWeight="600">to HDF5</text>
  <rect x="640" y="210" width="55" height="28" rx="4" fill="#252b3b" stroke="#3a4050" strokeWidth="0.75"/>
  <text x="668" y="228" textAnchor="middle" fill="#9ca3b0" fontSize="5" fontStyle="italic">HDF5</text>
  {/* Arrow from grapher to archive */}
  <line x1="640" y1="200" x2="665" y2="208" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="665,210 659,206 661,200" fill="#c3e04d" fillOpacity="0.5"/>

  {/* Bracket annotation: total time from event to archive */}
  <text x="430" y="252" textAnchor="middle" fill="#9ca3b0" fontSize="6" fontStyle="italic">{"Total time from event ingestion to persistent archive: ~240s (configurable per tier)"}</text>
</svg>

The lifecycle of a single StoryChunk follows these phases:

1. **Chunk Duration** — During the chunk's time window (default: 30 seconds), the ChronoKeeper groups arriving events into the chunk by StoryId and timestamp.
2. **Keeper Acceptance Window** — After the chunk's time range ends, the Keeper continues accepting late events for a configurable window (default: 60 seconds from chunk start) to tolerate network delays.
3. **Extract and Transfer** — Once the acceptance window expires, the Keeper serializes the partial StoryChunk and sends it to the ChronoGrapher via RDMA bulk transfer.
4. **Grapher Acceptance Window** — The ChronoGrapher holds the chunk in its own pipeline (default: 180 seconds), during which it merges partial chunks from different Keepers into complete StoryChunks.
5. **Archive** — When the Grapher's acceptance window expires, the complete StoryChunk is extracted and written to HDF5 persistent storage.

All timing parameters are configurable per deployment to balance between latency (shorter windows) and completeness (longer windows that tolerate more delay).

## Design Benefits

The distributed Story Pipeline model provides several key advantages for high-throughput event collection in HPC environments:

- **Parallelized ingestion** — Using a group of ChronoKeepers running on different HPC nodes to record individual events for the same story distributes the immediate ingestion workload and parallelizes early sequencing of events into partial StoryChunks across compute nodes.
- **Batch data movement** — Moving events in presorted batches (StoryChunks) between compute and storage nodes, rather than forwarding individual events, provides significantly higher overall throughput across the network.
- **Progressive ordering** — Events are first locally sorted within each Keeper, then globally merged at the Grapher. This two-phase approach avoids the bottleneck of a centralized sequencer.
- **Network delay tolerance** — Configurable acceptance windows at each tier ensure that late-arriving events are correctly placed, without sacrificing pipeline throughput.
- **Decoupled processing** — Each tier operates independently: Keepers can continue ingesting while the Grapher merges and archives, preventing back-pressure from slowing down client applications.
