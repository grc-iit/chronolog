---
sidebar_position: 1
title: "Overview"
---

# User Guide Overview

The User Guide covers everything you need to configure, deploy, and operate a ChronoLog system. Whether you are standing up a single-node development instance or running a multi-node cluster on an HPC system, the sections below walk you through each stage.

## What's in this section

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

### Operations

After deployment, the operations sub-section helps you keep your system healthy:

- **[Monitoring & Logging](./operations/monitoring-and-logging.md)** — log levels, log file locations, and how to interpret output from each component
- **[Troubleshooting](./operations/troubleshooting.md)** — common issues and their solutions, from connection failures to deployment problems
- **[Error Codes](./operations/error-codes.md)** — a comprehensive reference of server-side error codes with recommended actions
