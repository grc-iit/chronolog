---
sidebar_position: 1
title: "Overview"
---

# ChronoLog Project Overview

ChronoLog is a scalable, high-performance distributed shared log store designed to handle the ever-growing volume, velocity, and variety of modern activity data. It is tailored for applications ranging from edge computing to high-performance computing (HPC) systems, offering a versatile solution for managing log data across diverse domains. Find out more at [chronolog.dev](https://chronolog.dev).

<svg viewBox="0 0 720 710" width="100%" xmlns="http://www.w3.org/2000/svg" fontFamily="system-ui, sans-serif">
  <rect x="0" y="0" width="720" height="710" rx="10" fill="#1e2330"/>

  <text x="355" y="16" textAnchor="middle" fill="#9ca3b0" fontSize="8" fontWeight="600">CLIENT APPLICATIONS</text>
  <rect x="30"  y="24" width="100" height="28" rx="5" fill="#252b3b" stroke="#3a4050" strokeWidth="0.75"/>
  <text x="80"  y="42" textAnchor="middle" fill="#e4e7ed" fontSize="8">GraphDB</text>
  <rect x="140" y="24" width="100" height="28" rx="5" fill="#252b3b" stroke="#3a4050" strokeWidth="0.75"/>
  <text x="190" y="42" textAnchor="middle" fill="#e4e7ed" fontSize="8">NoSQL</text>
  <rect x="250" y="24" width="100" height="28" rx="5" fill="#252b3b" stroke="#3a4050" strokeWidth="0.75"/>
  <text x="300" y="42" textAnchor="middle" fill="#e4e7ed" fontSize="8">Monitoring</text>
  <rect x="360" y="24" width="100" height="28" rx="5" fill="#252b3b" stroke="#3a4050" strokeWidth="0.75"/>
  <text x="410" y="42" textAnchor="middle" fill="#e4e7ed" fontSize="8">BigTable</text>
  <rect x="470" y="24" width="100" height="28" rx="5" fill="#252b3b" stroke="#3a4050" strokeWidth="0.75"/>
  <text x="520" y="42" textAnchor="middle" fill="#e4e7ed" fontSize="8">Telemetry</text>
  <rect x="580" y="24" width="100" height="28" rx="5" fill="#252b3b" stroke="#3a4050" strokeWidth="0.75"/>
  <text x="630" y="42" textAnchor="middle" fill="#e4e7ed" fontSize="8">Streams</text>

  <line x1="360" y1="54" x2="360" y2="68" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="360,70 356,64 364,64" fill="#c3e04d" fillOpacity="0.5"/>

  <rect x="30" y="78" width="650" height="38" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.6"/>
  <text x="355" y="102" textAnchor="middle" fill="#c3e04d" fontSize="10" fontWeight="600">ChronoLog Client API</text>

  <text x="24" y="132" fill="#9ca3b0" fontSize="7" fontWeight="600">CLIENT LAYER</text>
  <line x1="20" y1="138" x2="700" y2="138" stroke="#9ca3b0" strokeWidth="0.75" strokeDasharray="6,4" strokeOpacity="0.4"/>
  <text x="24" y="156" fill="#9ca3b0" fontSize="7" fontWeight="600">CONTROL PLANE</text>

  <rect x="30" y="168" width="650" height="440" rx="8" fill="none" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.25" strokeDasharray="8,4"/>
  <text x="42" y="182" fill="#c3e04d" fontSize="8" fontWeight="600" fillOpacity="0.6">CHRONOLOG CLUSTER</text>

  <rect x="60" y="192" width="220" height="68" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.5"/>
  <circle cx="76" cy="210" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="84" y="214" fill="#c3e04d" fontSize="10" fontWeight="600">chrono-visor</text>
  <text x="72" y="230" fill="#9ca3b0" fontSize="7">Metadata management, chronicle</text>
  <text x="72" y="240" fill="#9ca3b0" fontSize="7">registry, clock sync, client connections</text>

  <line x1="20" y1="280" x2="700" y2="280" stroke="#9ca3b0" strokeWidth="0.75" strokeDasharray="6,4" strokeOpacity="0.4"/>

  <text x="24" y="298" fill="#9ca3b0" fontSize="7" fontWeight="600">DATA PLANE</text>

  <rect x="230" y="308" width="430" height="220" rx="6" fill="none" stroke="#4a90a4" strokeWidth="0.75" strokeDasharray="6,3" strokeOpacity="0.4"/>
  <text x="242" y="322" fill="#4a90a4" fontSize="8" fontWeight="600" fillOpacity="0.7">RECORDING GROUP</text>

  <rect x="340" y="334" width="210" height="68" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.5"/>
  <circle cx="356" cy="352" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="364" y="356" fill="#c3e04d" fontSize="10" fontWeight="600">chrono-keeper</text>
  <text x="356" y="372" fill="#9ca3b0" fontSize="7">Hot-tier ingestion via RDMA, real-time</text>
  <text x="356" y="382" fill="#9ca3b0" fontSize="7">{"record() and playback() with µs latency"}</text>

  <rect x="255" y="430" width="195" height="68" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.5"/>
  <circle cx="271" cy="448" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="279" y="452" fill="#c3e04d" fontSize="10" fontWeight="600">chrono-grapher</text>
  <text x="271" y="468" fill="#9ca3b0" fontSize="7">DAG pipeline: event collection, story</text>
  <text x="271" y="478" fill="#9ca3b0" fontSize="7">building, merging, flushing to lower tiers</text>

  <rect x="478" y="430" width="165" height="68" rx="6" fill="#252b3b" stroke="#c3e04d" strokeWidth="1" strokeOpacity="0.5"/>
  <circle cx="494" cy="448" r="3" fill="#c3e04d" fillOpacity="0.8"/>
  <text x="502" y="452" fill="#c3e04d" fontSize="10" fontWeight="600">chrono-player</text>
  <text x="494" y="468" fill="#9ca3b0" fontSize="7">{"Serves replay() across hot, warm,"}</text>
  <text x="494" y="478" fill="#9ca3b0" fontSize="7">cold tiers into a time-ordered stream</text>

  <rect x="60" y="558" width="600" height="38" rx="6" fill="#252b3b" stroke="#3a4050" strokeWidth="0.75"/>
  <text x="360" y="582" textAnchor="middle" fill="#e4e7ed" fontSize="10">HDF5 on PFS</text>

  <line x1="20" y1="618" x2="700" y2="618" stroke="#9ca3b0" strokeWidth="0.75" strokeDasharray="6,4" strokeOpacity="0.4"/>

  <text x="24" y="636" fill="#9ca3b0" fontSize="7" fontWeight="600">STORAGE PLANE</text>
  <rect x="140" y="644" width="440" height="40" rx="6" fill="#252b3b" stroke="#3a4050" strokeWidth="0.75"/>
  <text x="360" y="669" textAnchor="middle" fill="#e4e7ed" fontSize="10">EXTERNAL STORAGE</text>

  <line x1="155" y1="124" x2="155" y2="190" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="155,192 151,186 159,186" fill="#c3e04d" fillOpacity="0.6"/>

  <line x1="450" y1="124" x2="450" y2="332" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="450,334 446,328 454,328" fill="#c3e04d" fillOpacity="0.6"/>

  <line x1="155" y1="262" x2="155" y2="360" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <line x1="155" y1="360" x2="228" y2="360" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="230,360 224,356 224,364" fill="#c3e04d" fillOpacity="0.6"/>

  <line x1="445" y1="404" x2="445" y2="428" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="445,430 441,424 449,424" fill="#c3e04d" fillOpacity="0.6"/>

  <line x1="352" y1="500" x2="352" y2="556" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="352,558 348,552 356,552" fill="#c3e04d" fillOpacity="0.6"/>

  <line x1="560" y1="500" x2="560" y2="556" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="560,558 556,552 564,552" fill="#c3e04d" fillOpacity="0.6"/>

  <line x1="560" y1="430" x2="560" y2="118" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="560,116 556,122 564,122" fill="#c3e04d" fillOpacity="0.6"/>

  <line x1="360" y1="598" x2="360" y2="642" stroke="#c3e04d" strokeWidth="0.75" strokeOpacity="0.5"/>
  <polygon points="360,644 356,638 364,638" fill="#c3e04d" fillOpacity="0.6"/>
</svg>

## Core Features

- **Multi-Tiered Storage:** Leverages multiple storage tiers (e.g., persistent memory, flash storage) to scale log capacity and optimize performance.
- **High Concurrency:** Supports multiple writers and multiple readers (MWMR) for efficient concurrent access to the log.
- **Partial Data Retrieval:** Enables efficient range queries for partial log processing, enhancing data exploration capabilities.
- **Total Ordering:** Guarantees strict ordering of log entries across distributed environments, eliminating the need for costly synchronization.
- **Synchronization-Free:** Employs physical time for log ordering, avoiding expensive synchronization operations.
- **Auto-Tiering:** Automatically and transparently moves log data across storage tiers based on age and access patterns.
- **Elastic I/O:** Adapts to varying I/O workloads, ensuring efficient resource utilization and performance.

## Applicability

ChronoLog is designed to address the challenges of modern applications that generate and process vast amounts of log data. It is particularly well-suited for:

- **Scientific Applications:** Astrophysics, geoscience, materials science, cosmology, and more.
- **Internet-of-Things (IoT):** Efficiently stores, indexes, queries, and analyzes data from IoT devices.
- **Financial Applications:** Supports real-time monitoring, time-series analysis, and fraud protection.
- **HPC Resource Management:** Optimizes resource utilization in large-scale HPC systems.
- **System Telemetry and Trace Recording:** Captures and analyzes telemetry data from distributed systems.
- **Application Performance Analysis:** Facilitates performance monitoring and analysis for complex applications.

## Key Innovations

- **Physical Time-Based Ordering:** A novel approach to achieving total ordering in distributed systems without synchronization overhead.
- **3D Log Distribution:** A unique model that distributes log data horizontally across nodes and vertically across storage tiers.
- **Decoupled Server-Pull Architecture:** Separates event ingestion from event persistence for enhanced performance and scalability.

## Development Team

ChronoLog is being developed by a team of researchers and engineers at Illinois Tech and the University of Chicago.

## Acknowledgements

We gratefully acknowledge the support of the National Science Foundation (NSF) for funding this project. We also thank our collaborators from various scientific and engineering domains for their valuable insights and feedback.

:::info Publication

A. Kougkas, H. Devarajan, K. Bateman, J. Cernuda, N. Rajesh, X.-H. Sun. "[ChronoLog: A Distributed Shared Tiered Log Store with Time-based Data Ordering](http://www.cs.iit.edu/~scs/testing/scs_website/assets/files/kougkas2020chronolog.pdf)," Proceedings of the 36th International Conference on Massive Storage Systems and Technology (MSST 2020).

:::
