---
sidebar_position: 1
title: "Overview"
---

# User Guide Overview

The User Guide covers everything you need to understand, configure, deploy, and operate a ChronoLog system. Whether you are standing up a single-node development instance or running a multi-node cluster on an HPC system, the sections below walk you through each stage.

## What's in this section

### Architecture

A top-down look at how ChronoLog is structured, from the high-level data flow to the internals of each component:

- [System Overview](./architecture/system-overview.md) — tiered storage, data flow, and component interactions
- **Component deep-dives** — [ChronoVisor](./architecture/chronovisor.md), [ChronoKeeper](./architecture/chronokeeper.md), [ChronoGrapher](./architecture/chronographer.md), and [ChronoPlayer](./architecture/chronoplayer.md)

### Data Model

Formal definitions of ChronoLog's core abstractions and how data moves through the system:

- [Data Model Overview](./data-model/overview.md) — Event, Story, Chronicle, and StoryChunk as data structures
- [Distributed Story Pipeline](./data-model/story-pipeline.md) — story chunks, pipelines, and event sequencing across storage tiers

### Configuration

ChronoLog is configured through a single JSON file that controls every component in the system. The configuration sub-section explains:

- How the **ConfigurationManager** parses and validates settings at startup
- **Client configuration** — connection endpoints, timeouts, and logging
- **Server configuration** — per-component settings for ChronoVisor, ChronoKeeper, ChronoGrapher, and ChronoPlayer
- **Network & RPC** — protocol selection and multi-node networking
- **Performance tuning** — story chunk sizes, acceptance windows, and storage tier knobs

See [Configuration Overview](./configuration/overview.md) to get started.

### Deployment

Once configured, ChronoLog can be deployed in several ways depending on your environment:

- **[Single Node](./deployment/single-node.md)** — the quickest path; a single script builds, installs, starts, and stops all services on one machine
- **[Multi-Node](./deployment/multi-node.md)** — distributed deployment across multiple hosts using the deployment script and Slurm integration
- **[Docker Compose](./deployment/docker-compose.md)** — containerized deployment with service definitions and scaling

