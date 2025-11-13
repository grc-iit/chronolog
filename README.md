> [!IMPORTANT]
> **ChronoLog MCP is now available.**
> Integrate ChronoLog directly with LLMs through our new MCP server for real-time logging, event processing, and structured interactions.  
> 📖 [Documentation](https://iowarp.github.io/iowarp-mcps/docs/mcps/chronolog/)

<p align="center">
  <a href="https://www.chronolog.dev">
    <img src="doc/images/logos/logo-chronolog.png" alt="ChronoLog logo" width="25%">
  </a>
</p>

<h1 align="center">ChronoLog</h1>

<p align="center"><strong>Distributed Shared Tiered Log Store</strong></p>

<p align="center">A distributed and tiered shared log storage ecosystem that uses physical time to distribute log entries while providing total log ordering.</p>

<p align="center">
  <a href="LICENSE"><img alt="License" src="https://img.shields.io/github/license/grc-iit/ChronoLog.svg" /></a>
  <a href="https://github.com/grc-iit/ChronoLog"><img alt="ChronoLog" src="https://img.shields.io/badge/ChronoLog-GitHub-blue.svg" /></a>
  <a href="https://grc.iit.edu/"><img alt="GRC" src="https://img.shields.io/badge/GRC-Website-blue.svg" /></a>
  <a href="https://www.chronolog.dev"><img alt="Website" src="https://img.shields.io/badge/Website-ChronoLog-blue.svg" /></a>
  <a href="https://github.com/grc-iit/ChronoLog/issues"><img alt="GitHub Issues" src="https://img.shields.io/github/issues/grc-iit/ChronoLog.svg" /></a>
  <a href="https://github.com/grc-iit/ChronoLog/releases/latest"><img alt="GitHub Release" src="https://img.shields.io/github/release/grc-iit/ChronoLog.svg" /></a>
</p>

## Overview

**ChronoLog** is a distributed, tiered shared log storage system that provides scalable log storage with time-based data ordering and total log ordering guarantees. By leveraging physical time for data distribution and utilizing multiple storage tiers for elastic capacity scaling, ChronoLog eliminates the need for a central sequencer while maintaining high performance and scalability.

The system's modular, plugin-based architecture serves as a foundation for building scalable applications, including SQL-like query engines, streaming processors, log-based key-value stores, and machine learning integration modules.

<p align="center">
  <img src="doc/images/ChronoLogDesign.png" alt="ChronoLog Architecture" width="60%">
</p>

### Key Features

ChronoLog is built on four foundational pillars:

- **Time-Structured Ingestion** — Events are chunked and organized by physical time, enabling high-throughput parallel writes without a central sequencer.

- **Tiered & Efficient Storage** — StoryChunks flow across fast and scalable storage tiers, automatically balancing performance and capacity.

- **Concurrent Access at Scale** — Multi-writer, multi-reader support with zero coordination overhead, optimized for both RDMA and TCP networks.

- **Modular, Extensible Serving Layer** — Plugin-based architecture enables custom services to run directly on the log, supporting diverse application requirements.

### Use Cases

ChronoLog's flexible architecture supports a wide range of applications:

- **SQL-like Query Engine** — Query and analyze log data with SQL semantics, leveraging time-based data distribution for efficient processing.

- **Streaming Processor** — Real-time event processing and analytics on time-ordered log streams for monitoring, alerting, and data pipeline applications.

- **Log-based Key-Value Store** — Build distributed key-value stores on ChronoLog's ordered log abstraction with strong consistency guarantees.

- **Machine Learning Integration** — TensorFlow module for training and inference workflows using time-ordered data streams.

- **Enterprise Logging** — Enterprise-grade logging with real-time event processing and structured interaction management via the MCP server.

For more information, visit [chronolog.dev](https://www.chronolog.dev).

## Wiki:
Learn more detailed information about the project on ChronoLog's Wiki: https://github.com/grc-iit/ChronoLog/wiki/

## Main publication

<div style="border: 1px solid #555555; padding: 10px; border-radius: 5px; background-color: #888888;">
  <p style="font-size: 1.2em; margin: 0;">
    A. Kougkas, H. Devarajan, K. Bateman, J. Cernuda, N. Rajesh, X.-H. Sun. 
    <a href="http://www.cs.iit.edu/~scs/testing/scs_website/assets/files/kougkas2020chronolog.pdf" target="_blank">
      <strong>"ChronoLog: A Distributed Shared Tiered Log Store with Time-based Data Ordering"</strong>
    </a>, 
    Proceedings of the 36th International Conference on Massive Storage Systems and Technology (MSST 2020).
  </p>
</div>

## Members

<a href="https://www.iit.edu">
    <img src="doc/images/logos/IIT.png" alt="Illinois Tech" width="150">
</a>

<a href="https://www.uchicago.edu/">
    <img src="doc/images/logos/university-of-chicago.png" alt="University Of Chicago" width="150">
</a>

## Collaborators
<a href="https://www.llnl.gov/">
    <img src="doc/images/logos/llnl.jpg" alt="Lawrence Livermore National Lab" width="75">
</a>
<a href="https://www6.slac.stanford.edu/">
    <img src="doc/images/logos/slac.png" alt="SLAC National Accelerator Lab" width="75">
</a>
<a href="https://www.3redpartners.com/">
    <img src="doc/images/logos/3red.png" alt="3RedPartners" width="75">
</a>
<a href="https://www.depaul.edu/">
    <img src="doc/images/logos/depaul.png" alt="DePaul University" width="75">
</a>
<a href="https://www.wisc.edu/">
    <img src="doc/images/logos/university-of-wisconsin.jpg" alt="University of Wisconsin Madison" width="75">
</a>
<a href="https://www.paratools.com/">
    <img src="doc/images/logos/paratools.png" alt="ParaTools, Inc." width="75">
</a>
<a href="https://illinois.edu/">
    <img src="doc/images/logos/university-of-illinois.jpg" alt="University of Illinois at Urbana-Champaign" width="75">
</a>
<a href="https://www.anl.gov/">
    <img src="doc/images/logos/argonne.jpeg" alt="Argonne National Lab" width="75">
</a>
<a href="https://omnibond.com/">
    <img src="doc/images/logos/omnibond.png" alt="OmniBond Systems LLC" width="75">
</a>

## Sponsors
<a href="https://www.nsf.gov">
    <img src="doc/images/logos/nsf-fb7efe9286a9b499c5907d82af3e70fd.png" alt="NSFLOGO" width="150">
</a>

National Science Foundation (NSF CSSI-2104013)