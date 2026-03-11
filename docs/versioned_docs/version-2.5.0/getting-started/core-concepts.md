---
sidebar_position: 2
title: "Core Concepts"
---

# Core Concepts

:::info
This page is under construction. More detailed content will be added soon.
:::

ChronoLog organizes data using a layered hierarchy of **Chronicles**, **Stories**, and **Events**. Understanding these abstractions is key to working with the system.

## Chronicle

A **Chronicle** is the top-level organizational unit in ChronoLog. It groups related Stories together for easier data management. Think of it as a namespace or a project container.

## Story

A **Story** is a time-series data stream within a Chronicle. It represents a logical sequence of events that share a common context (e.g., a sensor feed, a job trace, or an application log). Stories provide ordering guarantees across their events.

## Event

An **Event** is the smallest data unit in ChronoLog. Each event contains:

- **Timestamp** — physical time assigned at ingestion
- **Client ID** — identifier of the producing client
- **Index** — per-client counter for disambiguation
- **Log Record** — the actual payload (arbitrary string data)

## Storage Tiers

ChronoLog uses a multi-tiered architecture where data flows through three layers:

1. **ChronoKeeper (Tier 1)** — fast ingestion on compute nodes
2. **ChronoGrapher (Tier 2)** — merging and sequencing on storage nodes
3. **Persistent Storage (Tier 3)** — long-term archival (e.g., HDF5)

Data automatically moves from higher tiers to lower tiers over time, optimizing for both write throughput and long-term storage efficiency.
