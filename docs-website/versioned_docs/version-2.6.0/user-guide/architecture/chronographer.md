---
sidebar_position: 4
title: "ChronoGrapher"
---

# ChronoGrapher

**ChronoGrapher** is the component that continues the job of sequencing and merging of the partial StoryChunks created by the ChronoKeepers on the first tier, assembling the complete time-range bound StoryChunks out of them, and archiving the complete StoryChunks into the persistent storage tier.

The ChronoGrapher design follows the StoryPipeline Data Model and mirrors the design of the ChronoKeeper. The ChronoLog deployment allows for only one ChronoGrapher process within the RecordingGroup, and it's usually deployed on the storage node.

<svg viewBox="0 0 720 650" width="100%" xmlns="http://www.w3.org/2000/svg" fontFamily="system-ui, sans-serif">
  <rect x="0" y="0" width="720" height="650" rx="10" fill="#1e2330"/>

  {/* Title and outer border */}
  <text x="360" y="20" textAnchor="middle" fill="#c3e04d" fontSize="10" fontWeight="600">CHRONOGRAPHER COMPONENTS</text>
  <rect x="20" y="30" width="680" height="610" rx="8" fill="none" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.25" strokeDasharray="8,4"/>
  <text x="32" y="44" fill="#c3e04d" fontSize="8" fontWeight="600" fillOpacity="0.6">CHRONOGRAPHER</text>

  <g transform="translate(0, 30)">
  {/* Top annotation — centered on Grapher Recording Service (center x = 190 + 500/2) */}
  <text x="440" y="18" textAnchor="middle" fill="#9ca3b0" fontSize="7" fontStyle="italic">Partial StoryChunks from ChronoKeepers</text>

  {/* Arrow from annotation to Grapher Recording Service (vertical, box center x = 440) */}
  <line x1="440" y1="24" x2="440" y2="42" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="440,48 436,42 444,42" fill="#c3e04d" fillOpacity="0.6"/>

  {/* Grapher Recording Service */}
  <rect x="190" y="48" width="500" height="34" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.5"/>
  <circle cx="206" cy="65" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="214" y="69" fill="#c3e04d" fontSize="10" fontWeight="600">Grapher Recording Service</text>

  {/* Arrow: Recording Service -> Ingestion Queue */}
  <line x1="440" y1="82" x2="440" y2="94" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="440,96 436,90 444,90" fill="#c3e04d" fillOpacity="0.6"/>

  {/* StoryChunk Ingestion Queue */}
  <rect x="190" y="96" width="500" height="34" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.5"/>
  <circle cx="206" cy="113" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="214" y="117" fill="#c3e04d" fontSize="10" fontWeight="600">StoryChunk Ingestion Queue</text>

  {/* Arrow: Ingestion Queue -> Grapher Data Store */}
  <line x1="440" y1="130" x2="440" y2="152" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="440,154 436,148 444,148" fill="#c3e04d" fillOpacity="0.6"/>

  {/* Grapher Data Store container */}
  <rect x="190" y="154" width="500" height="236" rx="6" fill="none" stroke="#4a90a4" strokeWidth="0.75" strokeDasharray="6,3" strokeOpacity="0.4"/>
  <text x="202" y="170" fill="#4a90a4" fontSize="8" fontWeight="600" fillOpacity="0.7">GRAPHER DATA STORE</text>

  {/* Vertical label bar */}
  <rect x="200" y="180" width="60" height="196" rx="4" fill="#252b3b" stroke="#3a4050" strokeWidth="0.75"/>
  <text x="230" y="285" fill="#e4e7ed" fontSize="8" transform="rotate(-90 230 285)" textAnchor="middle">Grapher Data Store</text>

  {/* Story 1 Pipeline */}
  <rect x="275" y="180" width="125" height="196" rx="4" fill="#252b3b" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.4"/>
  <text x="337" y="196" textAnchor="middle" fill="#c3e04d" fontSize="8" fontWeight="600">Story 1 Pipeline</text>
  <rect x="285" y="204" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="311" y="204" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="337" y="204" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="363" y="204" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="285" y="224" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="311" y="224" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="337" y="224" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="363" y="224" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="285" y="244" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="311" y="244" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="337" y="244" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="363" y="244" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="285" y="264" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="311" y="264" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="337" y="264" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="363" y="264" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="285" y="284" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="311" y="284" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="337" y="284" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="363" y="284" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="285" y="304" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="311" y="304" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="337" y="304" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="363" y="304" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="285" y="324" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="311" y="324" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="337" y="324" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="363" y="324" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="285" y="344" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="311" y="344" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="337" y="344" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="363" y="344" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>

  {/* Story 2 Pipeline */}
  <rect x="410" y="180" width="125" height="196" rx="4" fill="#252b3b" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.4"/>
  <text x="472" y="196" textAnchor="middle" fill="#c3e04d" fontSize="8" fontWeight="600">Story 2 Pipeline</text>
  <rect x="420" y="204" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="446" y="204" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="472" y="204" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="498" y="204" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="420" y="224" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="446" y="224" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="472" y="224" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="498" y="224" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="420" y="244" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="446" y="244" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="472" y="244" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="498" y="244" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="420" y="264" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="446" y="264" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="472" y="264" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="498" y="264" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="420" y="284" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="446" y="284" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="472" y="284" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="498" y="284" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="420" y="304" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="446" y="304" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="472" y="304" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="498" y="304" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="420" y="324" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="446" y="324" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="472" y="324" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="498" y="324" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="420" y="344" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="446" y="344" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="472" y="344" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="498" y="344" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>

  {/* Story 3 Pipeline */}
  <rect x="545" y="180" width="125" height="196" rx="4" fill="#252b3b" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.4"/>
  <text x="607" y="196" textAnchor="middle" fill="#c3e04d" fontSize="8" fontWeight="600">Story 3 Pipeline</text>
  <rect x="555" y="204" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="581" y="204" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="607" y="204" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="633" y="204" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="555" y="224" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="581" y="224" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="607" y="224" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="633" y="224" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="555" y="244" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="581" y="244" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="607" y="244" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="633" y="244" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="555" y="264" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="581" y="264" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="607" y="264" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="633" y="264" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="555" y="284" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="581" y="284" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="607" y="284" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="633" y="284" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="555" y="304" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="581" y="304" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="607" y="304" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="633" y="304" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="555" y="324" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="581" y="324" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="607" y="324" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="633" y="324" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="555" y="344" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="581" y="344" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="607" y="344" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="633" y="344" width="22" height="16" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>

  {/* Left-side: RecordingProcess Registry Client */}
  <text x="92" y="155" textAnchor="middle" fill="#9ca3b0" fontSize="6.5" fontStyle="italic">ChronoGrapher Registration</text>
  <text x="92" y="163" textAnchor="middle" fill="#9ca3b0" fontSize="6.5" fontStyle="italic">and Heartbeat Msgs</text>
  <rect x="10" y="170" width="165" height="50" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.5"/>
  <circle cx="26" cy="186" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="34" y="190" fill="#c3e04d" fontSize="8" fontWeight="600">RecordingProcess</text>
  <text x="34" y="202" fill="#c3e04d" fontSize="8" fontWeight="600">Registry Client</text>
  {/* Arrow going left out */}
  <polygon points="2,190 8,186 8,194" fill="#c3e04d" fillOpacity="0.6"/>
  <line x1="8" y1="190" x2="10" y2="190" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>

  {/* Left-side: Data Store Admin Service */}
  <rect x="10" y="240" width="165" height="50" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.5"/>
  <circle cx="26" cy="260" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="34" y="264" fill="#c3e04d" fontSize="9" fontWeight="600">DataStore Admin Service</text>
  <text x="26" y="278" fill="#9ca3b0" fontSize="6.5">{"Start/Stop Story notifications"}</text>
  {/* Arrow coming in from left */}
  <line x1="2" y1="260" x2="10" y2="260" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="10,260 4,256 4,264" fill="#c3e04d" fillOpacity="0.6"/>
  <text x="92" y="302" textAnchor="middle" fill="#9ca3b0" fontSize="6.5" fontStyle="italic">Notifications from the ChronoVisor</text>
  <text x="92" y="310" textAnchor="middle" fill="#9ca3b0" fontSize="6.5" fontStyle="italic">{"to Start & Stop Story Recording"}</text>
  {/* Arrow from Admin Service to Data Store */}
  <line x1="175" y1="265" x2="188" y2="265" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="190,265 184,261 184,269" fill="#c3e04d" fillOpacity="0.6"/>

  {/* Arrow: Grapher Data Store -> Extraction Queue */}
  <line x1="440" y1="390" x2="440" y2="404" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="440,406 436,400 444,400" fill="#c3e04d" fillOpacity="0.6"/>

  {/* StoryChunk Extraction Queue */}
  <rect x="190" y="406" width="500" height="34" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.5"/>
  <circle cx="206" cy="423" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="214" y="427" fill="#c3e04d" fontSize="10" fontWeight="600">StoryChunk Extraction Queue</text>

  {/* Arrow: Extraction Queue -> Archiving Extractor */}
  <line x1="440" y1="440" x2="440" y2="458" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="440,460 436,454 444,454" fill="#c3e04d" fillOpacity="0.6"/>

  {/* StoryChunk Archiving Extractor */}
  <rect x="190" y="460" width="500" height="34" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.5"/>
  <circle cx="206" cy="477" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="214" y="481" fill="#c3e04d" fontSize="10" fontWeight="600">StoryChunk Archiving Extractor</text>

  {/* Arrow down to Persistent Storage */}
  <line x1="440" y1="494" x2="440" y2="530" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="440,532 436,526 444,526" fill="#c3e04d" fillOpacity="0.6"/>

  {/* Persistent Storage Tier */}
  <rect x="340" y="532" width="200" height="36" rx="6" fill="#252b3b" stroke="#3a4050" strokeWidth="0.75"/>
  <text x="440" y="554" textAnchor="middle" fill="#e4e7ed" fontSize="10">Persistent Storage Tier</text>

  {/* Bottom annotation */}
  <text x="440" y="586" textAnchor="middle" fill="#9ca3b0" fontSize="7" fontStyle="italic">Complete StoryChunks are archived in the ChronoStore</text>

  {/* Small storage icon cells next to Persistent Storage */}
  <rect x="556" y="538" width="14" height="10" rx="1" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="574" y="538" width="14" height="10" rx="1" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="592" y="538" width="14" height="10" rx="1" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="556" y="552" width="14" height="10" rx="1" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="574" y="552" width="14" height="10" rx="1" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="592" y="552" width="14" height="10" rx="1" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  </g>
</svg>

| Component                             | Description                                                                                                                                                                                                                                                                       |
| ---------------------------------------| -----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| **Recording Process Registry Client** | The client side of RPC communication between the ChronoGrapher process and ChronoVisor's RecordingProcessRegistryService. Used to send register/unregister and periodic heartbeat/statistics messages to ChronoVisor.                                                             |
| **DataStore Admin Service**           | Listens to Start/Stop Story recording notifications from ChronoVisor. These notifications trigger instantiation or dismantling of the appropriate StoryPipelines based on client data access requests.                                                                            |
| **Grapher Recording Service**         | The RPC service that receives partial StoryChunks from ChronoKeeper processes using Thallium RDMA communication.                                                                                                                                                                  |
| **StoryChunk Ingestion Queue**        | Ensures thread-safe communication between the ingestion threads of the Recording Service and the sequencing threads of the Grapher Data Store. Each active StoryPipeline has a pair of active and passive StoryChunkIngestionHandles to manage StoryChunk processing efficiently. |
| **Grapher Data Store**                | Maintains a StoryPipeline for each active story, merging ingested partial StoryChunks into complete time-range bound StoryChunks and retiring the complete StoryChunks to the Story Chunk Extraction Queue.                                                                       |
| **StoryChunk Extraction Queue**       | Provides a thread-safe communication boundary between the Grapher Data Store and the Extractor modules as retired StoryChunks leave their StoryPipelines and wait to be picked up by the extractor.                                                                               |
| **StoryChunk Archiving Extractor**    | Handles the serialization and archiving of StoryChunks from the Story Chunk Extraction Queue to persistent storage.                                                                                                                                                               |
