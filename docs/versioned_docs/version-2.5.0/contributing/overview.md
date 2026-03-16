---
sidebar_position: 0
title: "Overview"
---

# Contributing Overview

The Contributing section is for developers who want to understand ChronoLog's internals or contribute code to the project. It covers the system architecture, data model, development environment setup, and the guidelines that keep the codebase consistent.

## What's in this section

### Architecture

A top-down look at how ChronoLog is structured, from the high-level data flow to the internals of each component:

- [System Overview](./Architecture/system-overview.md) — tiered storage, data flow, and component interactions
- **Component deep-dives** — [ChronoVisor](./Architecture/chronovisor.md), [ChronoKeeper](./Architecture/chronokeeper.md), [ChronoGrapher](./Architecture/chronographer.md), and [ChronoPlayer](./Architecture/chronoplayer.md)

### Data Model

Formal definitions of ChronoLog's core abstractions and how data moves through the system:

- [Data Model Overview](./data-model/overview.md) — Event, Story, Chronicle, and StoryChunk as data structures
- [Distributed Story Pipeline](./data-model/story-pipeline.md) — story chunks, pipelines, and event sequencing across storage tiers

### Development

Everything you need to set up a development environment and start building:

- [Development Overview](./development/overview.md) — setup-to-test workflow at a glance
- [Environment Setup](./development/environment-setup.md) — required tools, dependencies, and workspace configuration
- [Building for Development](./development/building-for-development.md) — CMake debug flags, symbols, and build targets
- [Running Tests](./development/running-tests.md) — unit, integration, and end-to-end tests via CTest

### Guidelines

Conventions and processes that keep contributions consistent:

- [Git Workflow](./guidelines/git-workflow.md) — modified GitFlow branching strategy and branch management
- [Code Style](./guidelines/code-style.md) — clang-format-18 enforcement, naming conventions, and coding patterns
- [CI/CD](./guidelines/ci-cd.md) — GitHub Actions workflows for formatting checks and build/deploy validation
- [Testing Guidelines](./guidelines/testing-guidelines.md) — what tests to write and how to organize them
- [License](./guidelines/license.md) — BSD 2-Clause license
- [Code of Conduct](./guidelines/code-of-conduct.md) — Contributor Covenant v2.0
