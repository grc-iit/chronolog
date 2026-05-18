---
sidebar_position: 1
title: "Overview"
---

# ChronoLog Project Overview

ChronoLog is a scalable, high-performance distributed shared log store designed to handle the ever-growing volume, velocity, and variety of modern activity data. It is tailored for applications ranging from edge computing to high-performance computing (HPC) systems, offering a versatile solution for managing log data across diverse domains. Find out more at [chronolog.dev](https://chronolog.dev).

![ChronoLog Design](/img/ChronoLogDesign.png)

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

### Publication

A. Kougkas, H. Devarajan, K. Bateman, J. Cernuda, N. Rajesh, X.-H. Sun. "[ChronoLog: A Distributed Shared Tiered Log Store with Time-based Data Ordering](http://www.cs.iit.edu/~scs/testing/scs_website/assets/files/kougkas2020chronolog.pdf)," Proceedings of the 36th International Conference on Massive Storage Systems and Technology (MSST 2020).
