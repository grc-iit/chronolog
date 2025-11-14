> [!IMPORTANT]
> **ChronoLog MCP is now available.**
> Integrate ChronoLog directly with LLMs through our new MCP server for real-time logging, event processing, and structured interactions.  
> [Code](https://github.com/iowarp/agent-toolkit/tree/main/agent-toolkit-mcp-servers/chronolog) - [Documentation](https://www.iowarp.ai/docs/agent-toolkit/mcp#system-monitoring-2-servers-14-tools)

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


<p align="center">
  Created by:
</p>

<p align="center">
  <a href="https://www.iit.edu">
    <img src="doc/images/logos/IIT.png" alt="Illinois Tech" width="80" ">
  </a>
  <a href="https://www.uchicago.edu">
    <img src="doc/images/logos/university-of-chicago.png" alt="UChicago" width="80" ">
  </a>
</p>

<p align="center">
  <strong>Illinois Tech</strong> &nbsp;&nbsp;&nbsp; <strong>UChicago</strong>
</p>

<p align="center">
  Supported by:
</p>

<p align="center">
  <a href="https://www.nsf.gov">
    <img src="doc/images/logos/nsf-fb7efe9286a9b499c5907d82af3e70fd.png" alt="National Science Foundation" width="100" ">
  </a>
</p>

<p align="center">
  <strong>National Science Foundation (NSF CSSI-2104013)</strong>
</p>

## Overview

**ChronoLog** is a distributed, tiered shared log storage system that provides scalable log storage with time-based data ordering and total log ordering guarantees. By leveraging physical time for data distribution and utilizing multiple storage tiers for elastic capacity scaling, ChronoLog eliminates the need for a central sequencer while maintaining high performance and scalability.

The system's modular, plugin-based architecture serves as a foundation for building scalable applications, including SQL-like query engines, streaming processors, log-based key-value stores, and machine learning integration modules.

<!--
<p align="center">
  <img src="doc/images/ChronoLogDesign.png" alt="ChronoLog Architecture" width="60%">
</p>
-->

### Key Features

ChronoLog is built on four foundational pillars:

- **Time-Structured Ingestion** — Events are chunked and organized by physical time, enabling high-throughput parallel writes without a central sequencer.

- **Tiered & Efficient Storage** — StoryChunks flow across fast and scalable storage tiers, automatically balancing performance and capacity.

- **Concurrent Access at Scale** — Multi-writer, multi-reader support with zero coordination overhead, optimized for both RDMA and TCP networks.

- **Modular, Extensible Serving Layer** — Plugin-based architecture enables custom services to run directly on the log, supporting diverse application requirements.

### Use Cases

ChronoLog's flexible architecture supports a wide range of applications:

- **AI & LLM Integration** — MCP server for seamless LLM integration with enterprise logging and real-time event processing.

- **Machine Learning & Training** — TensorFlow module for training and inference workflows using time-ordered data streams.

- **SQL-like Query Engine** — Query and analyze log data with SQL semantics and time-based distribution.

- **Streaming Processor** — Real-time event processing and analytics for monitoring, alerting, and data pipelines.

- **Log-based Key-Value Store** — Distributed key-value stores with strong consistency guarantees.

For more information, visit [chronolog.dev](https://www.chronolog.dev).

<!--
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
-->

## Collaborators

We are grateful for the collaboration and support from our research and industry partners.

| Organization | Description |
|-------------|-------------|
| <img src="doc/images/logos/llnl.jpg" alt="Lawrence Livermore National Laboratory" width="30" style="vertical-align: middle;"> [Lawrence Livermore National Laboratory](https://www.llnl.gov) | A premier national security laboratory advancing science and technology for national security, energy, and environmental challenges. |
| <img src="doc/images/logos/slac.png" alt="SLAC National Accelerator Laboratory" width="30" style="vertical-align: middle;"> [SLAC National Accelerator Laboratory](https://www6.slac.stanford.edu) | A leading research facility advancing particle physics, accelerator science, and photon science for discovery and innovation. |
| <img src="doc/images/logos/argonne.jpeg" alt="Argonne National Laboratory" width="30" style="vertical-align: middle;"> [Argonne National Laboratory](https://www.anl.gov) | A multidisciplinary research center addressing energy, environmental, and national security challenges through cutting-edge science. |
| <img src="doc/images/logos/university-of-wisconsin.jpg" alt="University of Wisconsin-Madison" width="30" style="vertical-align: middle;"> [University of Wisconsin-Madison](https://www.wisc.edu) | A leading public research university advancing knowledge in computing, engineering, and interdisciplinary sciences. |
| <img src="doc/images/logos/university-of-illinois.jpg" alt="University of Illinois at Urbana-Champaign" width="30" style="vertical-align: middle;"> [University of Illinois at Urbana-Champaign](https://illinois.edu) | A world-class research institution renowned for excellence in computer science, engineering, and high-performance computing. |
| <img src="doc/images/logos/depaul.png" alt="DePaul University" width="30" style="vertical-align: middle;"> [DePaul University](https://www.depaul.edu) | A private research university contributing to computing and data science research and education. |
| <img src="doc/images/logos/paratools.png" alt="ParaTools, Inc." width="30" style="vertical-align: middle;"> [ParaTools, Inc.](https://www.paratools.com) | A software company specializing in parallel computing tools and high-performance computing solutions. |
| <img src="doc/images/logos/3red.png" alt="3RedPartners" width="30" style="vertical-align: middle;"> [3RedPartners](https://www.3redpartners.com) | A technology consulting firm providing strategic partnerships and solutions for innovative computing systems. |
| <img src="doc/images/logos/omnibond.png" alt="OmniBond Systems LLC" width="30" style="vertical-align: middle;"> [OmniBond Systems LLC](https://omnibond.com) | A systems software company developing advanced storage and computing solutions for enterprise and research applications. |

## Resources

- **Documentation**: Visit [chronolog.dev](https://www.chronolog.dev) for comprehensive documentation and guides
- **GitHub Repository**: [github.com/grc-iit/ChronoLog](https://github.com/grc-iit/ChronoLog)
- **Issues & Support**: Report issues or request features on [GitHub Issues](https://github.com/grc-iit/ChronoLog/issues)
- **Releases**: Check out the latest releases on [GitHub Releases](https://github.com/grc-iit/ChronoLog/releases)

<br>

---
<br>
<div align="center">

<img src="https://grc.iit.edu/img/logo.png" alt="Gnosis Research Center" width="60">

**Gnosis Research Center**  
Illinois Institute of Technology  
*Advancing the Future of Scalable Computing and Data-Driven Discovery*

**Connect with us:**  
🌐 [Website](https://grc.iit.edu) • 🐦 [X (Twitter)](https://twitter.com/grc_iit) • 💼 [LinkedIn](https://www.linkedin.com/school/gnosis-research-center) • 📺 [YouTube](https://www.youtube.com/@grc_iit) • ✉️ [Email](mailto:grc@illinoistech.edu)
</div>