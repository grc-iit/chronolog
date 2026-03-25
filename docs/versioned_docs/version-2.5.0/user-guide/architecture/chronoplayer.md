---
sidebar_position: 5
title: "ChronoPlayer"
---

# ChronoPlayer

**ChronoPlayer** is the component responsible for the "story reading" side of the ChronoLog services, as it reads back the recorded data and serves the client applications' queries into the recorded events.

The ChronoPlayer design is the logical continuation of the internal design of ChronoKeeper and ChronoGrapher servers, as it is also based on the Story Pipeline model.

There are two different APIs to get events back. **Playback()** retrievies Events at the end of the Story. **Replay()** requests history Events in earlier time range of a Story.

<svg viewBox="0 0 720 710" width="100%" xmlns="http://www.w3.org/2000/svg" fontFamily="system-ui, sans-serif">
  <rect x="0" y="0" width="720" height="710" rx="10" fill="#1e2330"/>

  {/* Title and outer border */}
  <text x="360" y="20" textAnchor="middle" fill="#c3e04d" fontSize="10" fontWeight="600">CHRONOPLAYER COMPONENTS</text>
  <rect x="20" y="30" width="680" height="670" rx="8" fill="none" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.25" strokeDasharray="8,4"/>
  <text x="32" y="44" fill="#c3e04d" fontSize="8" fontWeight="600" fillOpacity="0.6">CHRONOPLAYER</text>

  <g transform="translate(0, 30)">
  {/* Top annotation — centered on Player Recording Service */}
  <text x="520" y="18" textAnchor="middle" fill="#9ca3b0" fontSize="7" fontStyle="italic">Partial StoryChunks from ChronoKeepers</text>

  {/* Arrow from annotation to Player Recording Service (vertical, box center x = 520) */}
  <line x1="520" y1="24" x2="520" y2="42" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="520,48 516,42 524,42" fill="#c3e04d" fillOpacity="0.6"/>

  {/* Player Recording Service */}
  <rect x="350" y="48" width="340" height="34" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.5"/>
  <circle cx="366" cy="65" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="374" y="69" fill="#c3e04d" fontSize="10" fontWeight="600">Player Recording Service</text>

  {/* Left-side: Player Registry Client */}
  <text x="100" y="46" textAnchor="middle" fill="#9ca3b0" fontSize="6.5" fontStyle="italic">ChronoPlayer Registration</text>
  <text x="100" y="54" textAnchor="middle" fill="#9ca3b0" fontSize="6.5" fontStyle="italic">and Heartbeat Msgs</text>
  <rect x="18" y="62" width="165" height="44" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.5"/>
  <circle cx="34" cy="84" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="42" y="88" fill="#c3e04d" fontSize="9" fontWeight="600">Player Registry Client</text>
  {/* Arrow going left out */}
  <polygon points="10,84 16,80 16,88" fill="#c3e04d" fillOpacity="0.6"/>
  <line x1="16" y1="84" x2="18" y2="84" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>

  {/* Left-side: Player Data Store Admin Service */}
  <rect x="18" y="120" width="165" height="44" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.5"/>
  <circle cx="34" cy="138" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="42" y="136" fill="#c3e04d" fontSize="8" fontWeight="600">Player Data Store</text>
  <text x="42" y="148" fill="#c3e04d" fontSize="8" fontWeight="600">Admin Service</text>
  {/* Arrow coming in from left */}
  <line x1="10" y1="142" x2="18" y2="142" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="18,142 12,138 12,146" fill="#c3e04d" fillOpacity="0.6"/>
  <text x="100" y="176" textAnchor="middle" fill="#9ca3b0" fontSize="6.5" fontStyle="italic">Notifications from the ChronoVisor</text>
  <text x="100" y="184" textAnchor="middle" fill="#9ca3b0" fontSize="6.5" fontStyle="italic">{"to Start & Stop Story Recording"}</text>

  {/* Arrow from Recording Service down into Data Store */}
  <line x1="520" y1="82" x2="520" y2="98" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="520,100 516,94 524,94" fill="#c3e04d" fillOpacity="0.6"/>

  {/* Player Data Store container */}
  <rect x="250" y="100" width="440" height="236" rx="6" fill="none" stroke="#4a90a4" strokeWidth="0.75" strokeDasharray="6,3" strokeOpacity="0.4"/>
  <text x="262" y="116" fill="#4a90a4" fontSize="8" fontWeight="600" fillOpacity="0.7">PLAYER DATA STORE</text>

  {/* Vertical label bar */}
  <rect x="260" y="126" width="55" height="196" rx="4" fill="#252b3b" stroke="#3a4050" strokeWidth="0.75"/>
  <text x="288" y="230" fill="#e4e7ed" fontSize="8" transform="rotate(-90 288 230)" textAnchor="middle">Player Data Store</text>

  {/* Story 1 Pipeline */}
  <rect x="325" y="126" width="110" height="196" rx="4" fill="#252b3b" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.4"/>
  <text x="380" y="142" textAnchor="middle" fill="#c3e04d" fontSize="8" fontWeight="600">Story 1 Pipeline</text>
  <rect x="335" y="150" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="359" y="150" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="383" y="150" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="407" y="150" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="335" y="168" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="359" y="168" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="383" y="168" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="407" y="168" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="335" y="186" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="359" y="186" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="383" y="186" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="407" y="186" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="335" y="204" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="359" y="204" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="383" y="204" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="407" y="204" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="335" y="222" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="359" y="222" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="383" y="222" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="407" y="222" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="335" y="240" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="359" y="240" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="383" y="240" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="407" y="240" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="335" y="258" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="359" y="258" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="383" y="258" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="407" y="258" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="335" y="276" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="359" y="276" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="383" y="276" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="407" y="276" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>

  {/* Story 2 Pipeline */}
  <rect x="445" y="126" width="110" height="196" rx="4" fill="#252b3b" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.4"/>
  <text x="500" y="142" textAnchor="middle" fill="#c3e04d" fontSize="8" fontWeight="600">Story 2 Pipeline</text>
  <rect x="455" y="150" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="479" y="150" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="503" y="150" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="527" y="150" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="455" y="168" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="479" y="168" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="503" y="168" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="527" y="168" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="455" y="186" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="479" y="186" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="503" y="186" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="527" y="186" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="455" y="204" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="479" y="204" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="503" y="204" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="527" y="204" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="455" y="222" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="479" y="222" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="503" y="222" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="527" y="222" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="455" y="240" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="479" y="240" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="503" y="240" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="527" y="240" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="455" y="258" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="479" y="258" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="503" y="258" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="527" y="258" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="455" y="276" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="479" y="276" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="503" y="276" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="527" y="276" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>

  {/* Story 3 Pipeline */}
  <rect x="565" y="126" width="110" height="196" rx="4" fill="#252b3b" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.4"/>
  <text x="620" y="142" textAnchor="middle" fill="#c3e04d" fontSize="8" fontWeight="600">Story 3 Pipeline</text>
  <rect x="575" y="150" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="599" y="150" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="623" y="150" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="647" y="150" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="575" y="168" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="599" y="168" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="623" y="168" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="647" y="168" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="575" y="186" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="599" y="186" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="623" y="186" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="647" y="186" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="575" y="204" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="599" y="204" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="623" y="204" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="647" y="204" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="575" y="222" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="599" y="222" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="623" y="222" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="647" y="222" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="575" y="240" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="599" y="240" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="623" y="240" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="647" y="240" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="575" y="258" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="599" y="258" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="623" y="258" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="647" y="258" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="575" y="276" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="599" y="276" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="623" y="276" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="647" y="276" width="20" height="14" rx="2" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>

  {/* Playback/Replay Service */}
  <text x="18" y="356" fill="#9ca3b0" fontSize="6.5" fontStyle="italic">Playback/Replay Requests</text>
  <text x="18" y="364" fill="#9ca3b0" fontSize="6.5" fontStyle="italic">from Client Applications</text>
  {/* Arrow into Playback/Replay Service from left */}
  <line x1="10" y1="388" x2="18" y2="388" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="18,388 12,384 12,392" fill="#c3e04d" fillOpacity="0.6"/>
  <rect x="18" y="372" width="220" height="40" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.5"/>
  <circle cx="34" cy="392" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="42" y="396" fill="#c3e04d" fontSize="10" fontWeight="600">Playback/Replay Service</text>

  {/* Arrow from Data Store down to Playback/Replay Service */}
  <line x1="250" y1="280" x2="140" y2="370" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="140,372 136,366 144,366" fill="#c3e04d" fillOpacity="0.6"/>

  {/* Playback/Replay Response Transfer Agent 1 */}
  <rect x="18" y="432" width="280" height="36" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.5"/>
  <circle cx="34" cy="450" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="42" y="454" fill="#c3e04d" fontSize="9" fontWeight="600">Playback/Replay Response Transfer Agent for Story 1</text>

  {/* Arrow from Playback/Replay Service to Transfer Agent 1 */}
  <line x1="128" y1="412" x2="128" y2="430" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="128,432 124,426 132,426" fill="#c3e04d" fillOpacity="0.6"/>

  {/* Playback/Replay Response Transfer Agent 2 */}
  <rect x="18" y="484" width="280" height="36" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.5"/>
  <circle cx="34" cy="502" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="42" y="506" fill="#c3e04d" fontSize="9" fontWeight="600">Playback/Replay Response Transfer Agent for Story 2</text>

  {/* Arrow from Transfer Agent 1 to Transfer Agent 2 */}
  <line x1="128" y1="468" x2="128" y2="482" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="128,484 124,478 132,478" fill="#c3e04d" fillOpacity="0.6"/>

  {/* Arrows going left out from Transfer Agents */}
  <polygon points="10,450 16,446 16,454" fill="#c3e04d" fillOpacity="0.6"/>
  <line x1="16" y1="450" x2="18" y2="450" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="10,502 16,498 16,506" fill="#c3e04d" fillOpacity="0.6"/>
  <line x1="16" y1="502" x2="18" y2="502" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>

  <text x="18" y="536" fill="#9ca3b0" fontSize="6.5" fontStyle="italic">Playback/Replay Responses</text>
  <text x="18" y="544" fill="#9ca3b0" fontSize="6.5" fontStyle="italic">to Client Applications</text>

  {/* Archive Reading Agent */}
  <rect x="400" y="372" width="270" height="50" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.5"/>
  <circle cx="416" cy="392" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="424" y="396" fill="#c3e04d" fontSize="10" fontWeight="600">Archive Reading Agent</text>
  <text x="424" y="410" fill="#9ca3b0" fontSize="7">{"Reads from HDF5 Archives"}</text>

  {/* Arrow from Archive Reading Agent up to Player Data Store */}
  <line x1="535" y1="372" x2="535" y2="342" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="535,336 531,342 539,342" fill="#c3e04d" fillOpacity="0.6"/>

  {/* Arrow from Persistent Storage up to Archive Reading Agent */}
  <line x1="535" y1="472" x2="535" y2="428" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="535,422 531,428 539,428" fill="#c3e04d" fillOpacity="0.6"/>

  {/* Persistent Storage Tier */}
  <rect x="400" y="472" width="270" height="40" rx="6" fill="#252b3b" stroke="#3a4050" strokeWidth="0.75"/>
  <text x="535" y="496" textAnchor="middle" fill="#e4e7ed" fontSize="10">Persistent Storage Tier</text>

  {/* Small storage icon cells */}
  <rect x="610" y="478" width="14" height="10" rx="1" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="628" y="478" width="14" height="10" rx="1" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="646" y="478" width="14" height="10" rx="1" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="610" y="492" width="14" height="10" rx="1" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="628" y="492" width="14" height="10" rx="1" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>
  <rect x="646" y="492" width="14" height="10" rx="1" fill="#1e2330" stroke="#3a4050" strokeWidth="0.5"/>

  {/* Bottom annotation */}
  <text x="535" y="530" textAnchor="middle" fill="#9ca3b0" fontSize="7" fontStyle="italic">Complete StoryChunks are retrieved from the ChronoStore</text>
  </g>
</svg>

| Component                                   | Description                                                                                                                                                                                                                                                                                                                   |
| ---------------------------------------------| -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| **Player Registry Client**                  | The client side of RPC communication between the ChronoPlayer process and ChronoVisor's ProcessRegistryService. Used to send register/unregister and periodic heartbeat/statistics messages to ChronoVisor.                                                                                                                   |
| **Player Data Store Admin Service**         | Listens to Start/Stop Story recording notifications from ChronoVisor. These notifications trigger instantiation or dismantling of the appropriate active StoryPipelines based on client data access requests.                                                                                                                 |
| **Player Recording Service**                | The RPC service that receives partial StoryChunks from ChronoKeeper processes — the same partial story chunks sent to ChronoGrapher to be merged and committed to the Persistent Storage layer.                                                                                                                               |
| **Player Data Store**                       | Maintains the most recent segment of StoryPipeline for every active story, applying the same chunk merging logic as ChronoGrapher. This allows ChronoPlayer to have the most recent story events sorted, merged, and available for playback/replay requests before they are available in the slower Persistent Storage layer. |
| **Playback/Replay Service**                 | Listens to Story playback/replay queries from client applications and serves responses. Returns all sorted events for a specific story within the requested time range. The most recent events may come from the active Player Data Store; the majority of requests are satisfied from the Persistent Storage layer.          |
| **Playback/Replay Response Transfer Agent** | The RPC client responsible for bulk transfer of the range query response event series back to the requesting client application. Instantiated by the Playback Service for each client query, using the client's response-receiving service credentials provided in the query request.                                         |
| **Archive Reading Agent**                   | Reads Chronicle and Story data persisted in HDF5 Archives, extracts the subset of events relevant to the requested range query as StoryChunks for use by the Playback Service.                                                                                                                                                |
