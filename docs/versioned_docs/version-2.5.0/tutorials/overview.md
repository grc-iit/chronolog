---
sidebar_position: 0
title: "Overview"
---

# Tutorials Overview

The Tutorials section provides hands-on walkthroughs for common ChronoLog tasks. Each tutorial is self-contained and guides you from setup through execution, so you can follow along on your own machine or cluster.

## Available tutorials

### Native Linux

Get started with ChronoLog on bare-metal Linux without containers. These tutorials cover building from source, running your first system test, and executing performance benchmarks.

- [First Steps](./native-linux/first-steps.md) — set up ChronoLog on Linux and run the first system test
- [Performance Test](./native-linux/performance-test.md) — execute scripted performance tests with the client admin tool

### Docker (Single Node)

A quick local setup using Docker for experimenting with ChronoLog on a single machine without installing dependencies on the host.

### Docker (Multi Node)

A distributed deployment walkthrough using Docker across multiple nodes, including automated deployment and client performance testing.

- [Running ChronoLog](./docker-multi-node/running-chronolog.md) — automated multi-node deployment with Docker and the deployment script
- [Client + Performance Test](./docker-multi-node/client-performance-test.md) — running client performance tests on the multi-node deployment

### CSV Export

- [CSV Export](./csv-export.md) — export story data to CSV format for external analysis and visualization

### Distributed Telemetry

- [Distributed Telemetry](./distributed-telemetry.md) — collect and store distributed system telemetry data using ChronoLog
