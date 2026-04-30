---
sidebar_position: 2
title: "ChronoVisor"
---

# ChronoVisor

**ChronoVisor is the orchestrating component of the ChronoLog system**. There is only one ChronoVisor process in a ChronoLog deployment, typically running on its own node.

ChronoVisor acts as the client-facing portal, managing client connections and metadata management requests. It also maintains the registry of active recording processes and distributes the load of data recording across the recording process groups.

<svg viewBox="0 0 720 640" width="100%" xmlns="http://www.w3.org/2000/svg" fontFamily="system-ui, sans-serif">
  <rect x="0" y="0" width="720" height="640" rx="10" fill="#1e2330"/>

  <!-- External: Client RPC calls -->
  <text x="360" y="16" textAnchor="middle" fill="#9ca3b0" fontSize="8" fontWeight="600">CLIENT RPC CALLS</text>
  <line x1="360" y1="20" x2="360" y2="32" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="360,34 356,28 364,28" fill="#c3e04d" fillOpacity="0.6"/>

  <!-- ClientPortalService (top-level RPC entry) -->
  <rect x="130" y="36" width="460" height="34" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.6"/>
  <text x="360" y="58" textAnchor="middle" fill="#c3e04d" fontSize="10" fontWeight="600">ClientPortalService</text>

  <text x="24" y="86" fill="#9ca3b0" fontSize="7" fontWeight="600">RPC LAYER</text>
  <line x1="20" y1="90" x2="700" y2="90" stroke="#9ca3b0" strokeWidth="0.75" strokeDasharray="6,4" strokeOpacity="0.4"/>

  <!-- ChronoVisor process boundary -->
  <rect x="20" y="96" width="680" height="536" rx="8" fill="none" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.25" strokeDasharray="8,4"/>
  <text x="32" y="110" fill="#c3e04d" fontSize="8" fontWeight="600" fillOpacity="0.6">CHRONOVISOR PROCESS</text>

  <!-- VisorClientPortal (central orchestrator) -->
  <rect x="180" y="118" width="360" height="42" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.5"/>
  <circle cx="196" cy="140" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="204" y="144" fill="#c3e04d" fontSize="10" fontWeight="600">VisorClientPortal</text>
  <text x="340" y="144" fill="#9ca3b0" fontSize="7">Central orchestrator for all client operations</text>

  <!-- Arrow: ClientPortalService -> VisorClientPortal -->
  <line x1="360" y1="70" x2="360" y2="116" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="360,118 356,112 364,112" fill="#c3e04d" fillOpacity="0.6"/>

  <!-- Three downward arrows from VisorClientPortal -->

  <!-- Left arrow: to ClientRegistryManager -->
  <line x1="240" y1="160" x2="140" y2="186" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="138,188 139,181 146,185" fill="#c3e04d" fillOpacity="0.6"/>

  <!-- Center arrow: to ChronicleMetaDirectory -->
  <line x1="360" y1="160" x2="360" y2="186" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="360,188 356,182 364,182" fill="#c3e04d" fillOpacity="0.6"/>

  <!-- Right arrow: to KeeperRegistry -->
  <line x1="480" y1="160" x2="580" y2="186" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="582,188 575,185 581,181" fill="#c3e04d" fillOpacity="0.6"/>

  <!-- LEFT: ClientRegistryManager -->
  <rect x="30" y="190" width="220" height="58" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.5"/>
  <circle cx="46" cy="208" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="54" y="212" fill="#c3e04d" fontSize="10" fontWeight="600">ClientRegistryManager</text>
  <text x="46" y="228" fill="#9ca3b0" fontSize="7">Tracks active client connections</text>
  <text x="46" y="238" fill="#9ca3b0" fontSize="7">and story acquisition sessions</text>

  <!-- CENTER: ChronicleMetaDirectory -->
  <rect x="260" y="190" width="220" height="58" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.5"/>
  <circle cx="276" cy="208" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="284" y="212" fill="#c3e04d" fontSize="10" fontWeight="600">ChronicleMetaDirectory</text>
  <text x="276" y="228" fill="#9ca3b0" fontSize="7">Manages chronicle and story metadata,</text>
  <text x="276" y="238" fill="#9ca3b0" fontSize="7">coordinates data acquisition</text>

  <!-- Bidirectional arrow: ClientRegistryManager <-> ChronicleMetaDirectory -->
  <line x1="250" y1="215" x2="258" y2="215" stroke="#4a90a4" strokeWidth="0.75" strokeOpacity="0.6"/>
  <polygon points="250,215 256,211 256,219" fill="#4a90a4" fillOpacity="0.5"/>
  <polygon points="260,215 254,211 254,219" fill="#4a90a4" fillOpacity="0.5"/>

  <!-- RIGHT: KeeperRegistry -->
  <rect x="490" y="190" width="200" height="58" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.5"/>
  <circle cx="506" cy="208" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="514" y="212" fill="#c3e04d" fontSize="10" fontWeight="600">KeeperRegistry</text>
  <text x="506" y="228" fill="#9ca3b0" fontSize="7">Manages recording groups,</text>
  <text x="506" y="238" fill="#9ca3b0" fontSize="7">assigns stories to groups</text>

  <!-- Auth stub annotation on VisorClientPortal -->
  <rect x="30" y="122" width="140" height="30" rx="5" fill="#252b3b" stroke="#3a4050" strokeWidth="0.75"/>
  <text x="100" y="138" textAnchor="middle" fill="#9ca3b0" fontSize="7">Authentication check</text>
  <text x="100" y="147" textAnchor="middle" fill="#9ca3b0" fontSize="6" fontStyle="italic">(stub in v2.7.0)</text>
  <line x1="170" y1="137" x2="178" y2="137" stroke="#3a4050" strokeWidth="0.75"/>
  <polygon points="180,137 174,133 174,141" fill="#3a4050" fillOpacity="0.6"/>

  <!-- Separator: internal services -->
  <line x1="30" y1="264" x2="690" y2="264" stroke="#9ca3b0" strokeWidth="0.75" strokeDasharray="6,4" strokeOpacity="0.3"/>

  <!-- KeeperRegistryService (external-facing RPC) -->
  <text x="590" y="280" textAnchor="middle" fill="#9ca3b0" fontSize="7" fontWeight="600">REGISTRATION RPC</text>

  <rect x="490" y="286" width="200" height="48" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.5"/>
  <circle cx="506" cy="304" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="514" y="308" fill="#c3e04d" fontSize="10" fontWeight="600">KeeperRegistryService</text>
  <text x="506" y="322" fill="#9ca3b0" fontSize="7">RPC listener for process registration</text>

  <!-- Arrow: KeeperRegistryService -> KeeperRegistry -->
  <line x1="590" y1="286" x2="590" y2="250" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="590,248 586,254 594,254" fill="#c3e04d" fillOpacity="0.6"/>

  <!-- External arrow into KeeperRegistryService -->
  <line x1="698" y1="310" x2="692" y2="310" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="690,310 696,306 696,314" fill="#c3e04d" fillOpacity="0.6"/>
  <text x="714" y="302" textAnchor="end" fill="#9ca3b0" fontSize="6">Keepers</text>
  <text x="714" y="310" textAnchor="end" fill="#9ca3b0" fontSize="6">Graphers</text>
  <text x="714" y="318" textAnchor="end" fill="#9ca3b0" fontSize="6">Players</text>

  <!-- Arrow: KeeperRegistry -> Recording Groups -->
  <line x1="590" y1="248" x2="590" y2="258" stroke="none"/>
  <line x1="490" y1="248" x2="360" y2="300" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="358,302 361,294 367,299" fill="#c3e04d" fillOpacity="0.6"/>

  <!-- RECORDING GROUPS SECTION -->

  <!-- Recording Group 1 -->
  <rect x="30" y="290" width="440" height="126" rx="6" fill="none" stroke="#4a90a4" strokeWidth="0.75" strokeDasharray="6,3" strokeOpacity="0.4"/>
  <text x="42" y="304" fill="#4a90a4" fontSize="8" fontWeight="600" fillOpacity="0.7">RECORDING GROUP 1</text>

  <rect x="50" y="312" width="200" height="22" rx="4" fill="#252b3b" stroke="#3a4050" strokeWidth="0.75"/>
  <text x="150" y="327" textAnchor="middle" fill="#e4e7ed" fontSize="7">KeeperProcessEntry 1</text>

  <rect x="260" y="312" width="200" height="22" rx="4" fill="#252b3b" stroke="#3a4050" strokeWidth="0.75"/>
  <text x="360" y="327" textAnchor="middle" fill="#e4e7ed" fontSize="7">KeeperProcessEntry 2</text>

  <rect x="50" y="340" width="200" height="22" rx="4" fill="#252b3b" stroke="#3a4050" strokeWidth="0.75"/>
  <text x="150" y="355" textAnchor="middle" fill="#e4e7ed" fontSize="7">GrapherProcessEntry</text>

  <rect x="260" y="340" width="200" height="22" rx="4" fill="#252b3b" stroke="#3a4050" strokeWidth="0.75"/>
  <text x="360" y="355" textAnchor="middle" fill="#e4e7ed" fontSize="7">PlayerProcessEntry</text>

  <text x="150" y="385" textAnchor="middle" fill="#9ca3b0" fontSize="6">Each entry holds a DataStoreAdminClient for RPC to that process</text>

  <!-- Recording Group 2 -->
  <rect x="30" y="424" width="440" height="126" rx="6" fill="none" stroke="#4a90a4" strokeWidth="0.75" strokeDasharray="6,3" strokeOpacity="0.4"/>
  <text x="42" y="438" fill="#4a90a4" fontSize="8" fontWeight="600" fillOpacity="0.7">RECORDING GROUP 2</text>

  <rect x="50" y="446" width="200" height="22" rx="4" fill="#252b3b" stroke="#3a4050" strokeWidth="0.75"/>
  <text x="150" y="461" textAnchor="middle" fill="#e4e7ed" fontSize="7">KeeperProcessEntry 1</text>

  <rect x="260" y="446" width="200" height="22" rx="4" fill="#252b3b" stroke="#3a4050" strokeWidth="0.75"/>
  <text x="360" y="461" textAnchor="middle" fill="#e4e7ed" fontSize="7">KeeperProcessEntry 2</text>

  <rect x="50" y="474" width="200" height="22" rx="4" fill="#252b3b" stroke="#3a4050" strokeWidth="0.75"/>
  <text x="150" y="489" textAnchor="middle" fill="#e4e7ed" fontSize="7">GrapherProcessEntry</text>

  <rect x="260" y="474" width="200" height="22" rx="4" fill="#252b3b" stroke="#3a4050" strokeWidth="0.75"/>
  <text x="360" y="489" textAnchor="middle" fill="#e4e7ed" fontSize="7">PlayerProcessEntry</text>

  <text x="150" y="519" textAnchor="middle" fill="#9ca3b0" fontSize="6">Each entry holds a DataStoreAdminClient for RPC to that process</text>

  <!-- Outgoing arrows from Recording Groups to external processes -->
  <line x1="462" y1="350" x2="472" y2="350" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <text x="478" y="346" fill="#9ca3b0" fontSize="6">start/stop story</text>
  <text x="478" y="354" fill="#9ca3b0" fontSize="6">recording via RPC</text>
  <polygon points="474,350 468,346 468,354" fill="#c3e04d" fillOpacity="0.6"/>

  <line x1="462" y1="484" x2="472" y2="484" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <text x="478" y="480" fill="#9ca3b0" fontSize="6">start/stop story</text>
  <text x="478" y="488" fill="#9ca3b0" fontSize="6">recording via RPC</text>
  <polygon points="474,484 468,480 468,488" fill="#c3e04d" fillOpacity="0.6"/>

  <!-- Ellipsis between groups -->
  <text x="250" y="418" textAnchor="middle" fill="#9ca3b0" fontSize="9">...</text>

  <!-- Bottom annotation -->
  <text x="360" y="580" textAnchor="middle" fill="#9ca3b0" fontSize="7" fontStyle="italic">KeeperRegistry selects a recording group for each acquired story using uniform random distribution,</text>
  <text x="360" y="591" textAnchor="middle" fill="#9ca3b0" fontSize="7" fontStyle="italic">then issues start/stop recording RPCs to all processes in that group via DataStoreAdminClient</text>
</svg>

| Component                              | Description                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                  |
| ----------------------------------------| ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| **Client Portal RPC Service**          | Manages communication sessions between client applications and ChronoVisor using a Thallium RPC engine configured for efficient concurrent connections.                                                                                                                                                                                                                                                                                                                                                                                      |
| **Client Authentication Module**       | Defines the authentication and authorization interface for client access control. In v2.7.0 all checks are implemented as stubs that grant access unconditionally; full role-based authentication is planned for a future release.                                                                                                                                                                                                                                                                                                           |
| **Client Registry**                    | Keeps track of all active client connections and clients' data acquisition requests for Chronicles and Stories.                                                                                                                                                                                                                                                                                                                                                                                                                              |
| **RecordingProcessRegistryService**    | Listens to RPC Registration and heartbeat/statistics messages coming from ChronoKeeper, ChronoGrapher, and ChronoPlayer processes and passes these messages to the Recording Group Registry to act upon.                                                                                                                                                                                                                                                                                                                                     |
| **Recording Group Registry**           | Monitors and manages the Recording Groups process composition and activity status. ChronoVisor uses this information to distribute the load of data recording by assigning each newly acquired story to a specific recording group using a uniform random distribution. Recording Group Registry maintains **DataStoreAdminClient** connections to all registered ChronoKeepers, ChronoGraphers, and ChronoPlayers and uses them to issue RPC notifications about the start and stop of story recording to all recording processes involved. |
| **Chronicle and Story Meta Directory** | Manages the Chronicle and Story metadata for the ChronoLog system, coordinating with Client Registry to track client data acquisition sessions for specific Chronicles and Stories, and with the Recording Group Registry for distributing data access tasks between the recording process groups.                                                                                                                                                                                                                                           |
