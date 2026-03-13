---
sidebar_position: 0
title: "Documentation Index"
---

# Documentation Index

This page is a single-view map of the entire ChronoLog v2.5.0 documentation. Use it to navigate any page directly and track which pages are complete.

**Legend:** ✅ done &nbsp;|&nbsp; 🔲 pending

---

## Getting Started

- [Overview](./overview) — Project introduction, core features, and use cases 🔲
- [Core Concepts](./core-concepts) — Chronicle / Story / Event data model and multi-tiered storage architecture 🔲
- [Quick Start](./quick-start) — Install and run ChronoLog via source, tarball, Docker, or Spack 🔲
- [Diagrams](./diagrams) — Architecture and documentation diagram tracker organized by phase 🔲

---

## User Guide

### Configuration

- [Overview](../user-guide/configuration/overview) — ConfigurationManager usage, passing `--config`, parsing and validation 🔲
- [Client Configuration](../user-guide/configuration/client-configuration) — Connection settings, timeouts, and logging options for the client library 🔲
- [Server Configuration](../user-guide/configuration/server-configuration) — `default_conf.json` template with component-specific and RPC settings 🔲
- [Network & RPC](../user-guide/configuration/network-and-rpc) — Protocol selection and multi-node networking setup 🔲
- [Performance Tuning](../user-guide/configuration/performance-tuning) — Story chunk sizes, acceptance windows, and storage tier knobs 🔲

### Deployment

- [Single Node](../user-guide/deployment/single-node) — Single-node deployment script: build, install, start, stop, and clean 🔲
- [Multi-Node](../user-guide/deployment/multi-node) — Distributed deployment with the deployment script and Slurm integration 🔲
- [Docker Compose](../user-guide/deployment/docker-compose) — Deploying ChronoLog with Docker Compose service definitions and scaling 🔲
- [Slurm / HPC](../user-guide/deployment/slurm-hpc) — HPC cluster deployment using Slurm job scheduling and node allocation 🔲

### Operations

- [Monitoring & Logging](../user-guide/operations/monitoring-and-logging) — Monitoring running deployments, log levels, and interpreting log files 🔲
- [Troubleshooting](../user-guide/operations/troubleshooting) — Common issues and solutions: connection failures, crashes, deployment problems 🔲
- [Error Codes](../user-guide/operations/error-codes) — Comprehensive reference of all server-side error codes with recommended actions 🔲

---

## Client API

### CLI

- [Overview](../client/cli/overview) — What `client_admin` is, interactive vs scripted modes, and key capabilities 🔲
- [API Reference](../client/cli/api) — Full flag reference for all `client_admin` commands and options 🔲
- [Examples](../client/cli/examples) — Interactive session, scripted workload, and performance testing walkthroughs 🔲

### C++

- [Client Class](../client/cpp/client-class) — Main class for connecting, creating chronicles, acquiring stories, and lifecycle management 🔲
- [Event Class](../client/cpp/event-class) — Smallest data unit: timestamp, client ID, index, and log record with getters 🔲
- [StoryHandle Class](../client/cpp/story-handle) — Story-specific data access: logging events and playback queries within a time range 🔲
- [Logging](../client/cpp/logging) — Configuring log levels and output destinations in the C++ client library 🔲
- [Error Codes](../client/cpp/error-codes) — Client-side error code reference including `CL_SUCCESS` and failure conditions 🔲

### Python

- [Setup](../client/python/setup) — pybind11-based Python wrapper: installation and initial configuration 🔲
- [Writer API](../client/python/writer-api) — Creating chronicles, acquiring stories, and logging events from Python 🔲
- [Reader API](../client/python/reader-api) — Story playback and range queries using Python client bindings 🔲

### Guides

- [Writing Events](../client/guides/writing-events) — End-to-end example: create a chronicle, acquire a story, log events 🔲
- [Reading Events](../client/guides/reading-events) — Using `playback_story` for range queries and ordered event retrieval 🔲
- [Multi-Threaded Usage](../client/guides/multi-threaded) — Thread safety and concurrent story access with the client API 🔲
- [MPI / Distributed](../client/guides/mpi-distributed) — Using the client API in MPI-based HPC workflows 🔲
- [Performance Testing](../client/guides/performance-testing) — Benchmarking throughput and latency with the ChronoLog client 🔲

---

## Plugins

- [Overview](../plugins/overview) — Plugin system: lightweight native interfaces on the storage layer 🔲
- [ChronoKVS](../plugins/chronokvs) — Multi-version key-value store plugin with version history and temporal queries 🔲
- [ChronoStream](../plugins/chronostream) — Streaming interface plugin for real-time event processing on top of ChronoLog 🔲

---

## Tutorials

### Docker Multi-Node

- [Running ChronoLog](../tutorials/docker-multi-node/running-chronolog) — Automated multi-node deployment using Docker and the deployment script 🔲
- [Client + Performance Test](../tutorials/docker-multi-node/client-performance-test) — Running client performance tests on a multi-node Docker deployment 🔲

### Native Linux

- [First Steps](../tutorials/native-linux/first-steps) — Setting up ChronoLog on Linux and running the first system test 🔲
- [Performance Test](../tutorials/native-linux/performance-test) — Executing scripted performance tests with the Scripted Client Admin tool 🔲

### Standalone Tutorials

- [CSV Export](../tutorials/csv-export) — Exporting story data to CSV format for external analysis 🔲
- [Distributed Telemetry](../tutorials/distributed-telemetry) — Collecting and storing distributed system telemetry data using ChronoLog 🔲

---

## Contributing

### Architecture

- [System Overview](../contributing/architecture/system-overview) — High-level data flow, tiered storage, and component interactions 🔲

### Components

- [ChronoVisor](../contributing/architecture/components/chronovisor) — Orchestrating component: client connections, metadata registry, recording groups 🔲
- [ChronoKeeper](../contributing/architecture/components/chronokeeper) — Fast event ingestion on compute nodes with story pipeline sequencing 🔲
- [ChronoGrapher](../contributing/architecture/components/chronographer) — Merges partial story chunks from keepers and archives to persistent storage 🔲
- [ChronoPlayer](../contributing/architecture/components/chronoplayer) — Serves story reading and historical replay from archived persistent storage 🔲

### Data Model

- [Definitions](../contributing/architecture/data-model/definitions) — Formal definitions of Event, Story, and Chronicle as core data structures 🔲
- [Distributed Story Pipeline](../contributing/architecture/data-model/story-pipeline) — Story chunks, story pipelines, and event sequencing across storage tiers 🔲

### Development Setup

- [Environment Setup](../contributing/development-setup/environment-setup) — Required tools, dependencies, and workspace configuration for contributors 🔲
- [Building for Development](../contributing/development-setup/building-for-development) — CMake Debug build flags, debugging symbols, and build targets 🔲
- [Running Tests](../contributing/development-setup/running-tests) — Unit, integration, and end-to-end tests via CTest and manual approaches 🔲

### Guidelines

- [Git Workflow](../contributing/guidelines/git-workflow) — Modified GitFlow: branching strategy and pull request conventions 🔲
- [Code Style](../contributing/guidelines/code-style) — IDE configuration files and style enforcement from `CodeStyleFiles/` 🔲
- [PR Process](../contributing/guidelines/pr-process) — PR creation, review guidelines, CI checks, and merge policies 🔲
- [CI/CD](../contributing/guidelines/ci-cd) — GitHub Actions pipeline and automated testing overview 🔲
- [Testing](../contributing/guidelines/testing) — Test suite categories, CTest organization, and manual testing procedures 🔲

### Plugin Development

- [Plugin Architecture](../contributing/plugin-development/plugin-architecture) — Plugin lifecycle and available extension points in ChronoLog 🔲
- [Creating a Plugin](../contributing/plugin-development/creating-a-plugin) — Step-by-step guide: template, interfaces, and testing a new plugin 🔲
