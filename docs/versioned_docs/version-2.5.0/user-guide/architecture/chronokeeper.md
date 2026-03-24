---
sidebar_position: 3
title: "ChronoKeeper"
---

# ChronoKeeper

**ChronoKeeper** is the component responsible for fast ingestion of the log events coming from the client processes, efficiently grouping them into partial StoryChunks and moving them down to the lower tier of ChronoLog system.

Most ChronoLog deployments would have ChronoKeeper processes installed on the majority of the compute nodes and each Recording Group is expected to have multiple ChronoKeeper processes.

<svg viewBox="0 0 720 590" width="100%" xmlns="http://www.w3.org/2000/svg" fontFamily="system-ui, sans-serif">
  <rect x="0" y="0" width="720" height="590" rx="10" fill="#1e2330"/>

  {/* Title and outer border */}
  <text x="360" y="20" textAnchor="middle" fill="#c3e04d" fontSize="10" fontWeight="600">CHRONOKEEPER COMPONENTS</text>
  <rect x="20" y="30" width="680" height="550" rx="8" fill="none" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.25" strokeDasharray="8,4"/>
  <text x="32" y="44" fill="#c3e04d" fontSize="8" fontWeight="600" fillOpacity="0.6">CHRONOKEEPER</text>

  <g transform="translate(0, 30)">
  {/* Top annotation */}
  <text x="680" y="16" textAnchor="end" fill="#9ca3b0" fontSize="7" fontStyle="italic">Individual Events are streamed from the</text>
  <text x="680" y="26" textAnchor="end" fill="#9ca3b0" fontSize="7" fontStyle="italic">Client processes to the ChronoKeepers</text>

  {/* Arrow from annotation to Recording Service */}
  <line x1="580" y1="30" x2="445" y2="46" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="445,48 441,42 449,42" fill="#c3e04d" fillOpacity="0.6"/>

  {/* Keeper Recording Service */}
  <rect x="190" y="48" width="500" height="34" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.5"/>
  <circle cx="206" cy="65" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="214" y="69" fill="#c3e04d" fontSize="10" fontWeight="600">Keeper Recording Service</text>

  {/* Arrow: Recording Service -> Ingestion Queue */}
  <line x1="440" y1="82" x2="440" y2="94" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="440,96 436,90 444,90" fill="#c3e04d" fillOpacity="0.6"/>

  {/* Event Ingestion Queue */}
  <rect x="190" y="96" width="500" height="34" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.5"/>
  <circle cx="206" cy="113" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="214" y="117" fill="#c3e04d" fontSize="10" fontWeight="600">Event Ingestion Queue</text>

  {/* Arrow: Ingestion Queue -> Keeper Data Store */}
  <line x1="440" y1="130" x2="440" y2="152" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="440,154 436,148 444,148" fill="#c3e04d" fillOpacity="0.6"/>

  {/* Keeper Data Store container */}
  <rect x="190" y="154" width="500" height="236" rx="6" fill="none" stroke="#4a90a4" strokeWidth="0.75" strokeDasharray="6,3" strokeOpacity="0.4"/>
  <text x="202" y="170" fill="#4a90a4" fontSize="8" fontWeight="600" fillOpacity="0.7">KEEPER DATA STORE</text>

  {/* Vertical label bar */}
  <rect x="200" y="180" width="60" height="196" rx="4" fill="#252b3b" stroke="#3a4050" strokeWidth="0.75"/>
  <text x="230" y="285" fill="#e4e7ed" fontSize="8" transform="rotate(-90 230 285)" textAnchor="middle">Keeper Data Store</text>

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

  {/* Left-side: Keeper Registry Client */}
  <text x="92" y="155" textAnchor="middle" fill="#9ca3b0" fontSize="6.5" fontStyle="italic">ChronoKeeper Registration</text>
  <text x="92" y="163" textAnchor="middle" fill="#9ca3b0" fontSize="6.5" fontStyle="italic">and Heartbeat Msgs</text>
  <rect x="10" y="170" width="165" height="50" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.5"/>
  <circle cx="26" cy="190" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="34" y="194" fill="#c3e04d" fontSize="9" fontWeight="600">Keeper Registry Client</text>
  <text x="26" y="208" fill="#9ca3b0" fontSize="6.5">{"Registration & Heartbeat msgs"}</text>
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

  {/* Arrow: Keeper Data Store -> Extraction Queue */}
  <line x1="440" y1="390" x2="440" y2="404" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="440,406 436,400 444,400" fill="#c3e04d" fillOpacity="0.6"/>

  {/* StoryChunk Extraction Queue */}
  <rect x="190" y="406" width="500" height="34" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.5"/>
  <circle cx="206" cy="423" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="214" y="427" fill="#c3e04d" fontSize="10" fontWeight="600">StoryChunk Extraction Queue</text>

  {/* Arrows to extractors */}
  <line x1="350" y1="440" x2="310" y2="462" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="310,464 306,458 314,458" fill="#c3e04d" fillOpacity="0.6"/>
  <line x1="530" y1="440" x2="570" y2="462" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="570,464 566,458 574,458" fill="#c3e04d" fillOpacity="0.6"/>

  {/* CSV File Extractor */}
  <rect x="190" y="464" width="220" height="40" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.5"/>
  <circle cx="206" cy="484" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="214" y="488" fill="#c3e04d" fontSize="10" fontWeight="600">CSV File Extractor</text>

  {/* RDMA Extractor */}
  <rect x="430" y="464" width="260" height="40" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.5"/>
  <circle cx="446" cy="484" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="454" y="488" fill="#c3e04d" fontSize="10" fontWeight="600">RDMA Extractor</text>

  {/* Arrows down from extractors */}
  <line x1="300" y1="504" x2="300" y2="524" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="300,526 296,520 304,520" fill="#c3e04d" fillOpacity="0.6"/>
  <line x1="560" y1="504" x2="560" y2="524" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="560,526 556,520 564,520" fill="#c3e04d" fillOpacity="0.6"/>

  {/* Bottom annotation */}
  <text x="360" y="545" textAnchor="middle" fill="#9ca3b0" fontSize="7" fontStyle="italic">Partial StoryChunks are drained to the ChronoGrapher or extracted to the local files</text>
  </g>
</svg>

| Component | Description |
|-----------|-------------|
| **Keeper Data Store** | The main ChronoKeeper module. A collection of Story Pipelines for all actively recorded stories in the Recording Group. Instantiates a Story Pipeline when ChronoKeeper receives a StartStoryRecording notification and dismantles it after StopStoryRecording. Sequencing threads are responsible for ordering ingested log events and grouping them into time-range bound StoryChunks. (See ChronoLog Story Pipeline Data Model for reference.) |
| **Keeper Recording Service** | Listens to incoming streams of log events from client applications and passes the events to the Ingestion Queue module for processing. |
| **Ingestion Queue** | A collection of IngestionHandles for actively recorded stories the ChronoKeeper is expecting events for. Receives log events from the Keeper Recording Service and attributes them to the appropriate Story Ingestion Handles. Story Ingestion Handle is part of the Story Pipeline object exposed to the Ingestion Queue at instantiation time. |
| **StoryChunk Extraction Queue** | A mutex-protected deque of StoryChunk pointers, serving as the communication boundary between the Keeper Data Store and StoryChunk Extractor modules. |
| **StoryChunk Extractors** | ChronoKeeper has two extractor types: **CSVFileStoryChunkExtractor** writes retired StoryChunks to a configurable local POSIX directory; **StoryChunkExtractorRDMA** drains retired StoryChunks to ChronoGrapher over an RDMA communication channel. Extractors can be chained if needed. |
| **KeeperRegistryClient** | The client side of RPC communication between the ChronoKeeper process and ChronoVisor's Recording Process Registry Service. Sends Register/Unregister and Heartbeat/Statistics messages to ChronoVisor. |
| **DataStoreAdminService** | Listens to Start/Stop Story recording notifications from ChronoVisor. These notifications trigger instantiation or dismantling of the appropriate StoryPipelines based on client data access requests. |
