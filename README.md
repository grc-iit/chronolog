# ChronoLog
![OPEN SOURCE](https://img.shields.io/badge/GNOSIS_RESEARCH_CENTER-blue)
![OPEN SOURCE](https://img.shields.io/badge/OPEN_SOURCE-grey)
[![License](https://img.shields.io/github/license/grc-iit/ChronoLog.svg)](LICENSE)
[![GitHub issues](https://img.shields.io/github/issues/grc-iit/ChronoLog.svg)](https://github.com/grc-iit/ChronoLog/issues)

[![Main_Branch](https://img.shields.io/badge/Branch-Main-green)](https://github.com/grc-iit/ChronoLog/tree/main)
[![Develop_Branch](https://img.shields.io/badge/Branch-Develop-yellow)](https://github.com/grc-iit/ChronoLog/tree/develop)
[![GitHub release](https://img.shields.io/github/release/grc-iit/ChronoLog.svg)](https://github.com/grc-iit/ChronoLog/releases/latest)


<div style="text-align: center;">
    <img src="doc/images/logos/logo-chronolog.png" alt="ChronoLog" width="200">
</div>

This project will design and implement ChronoLog, a distributed and tiered shared log storage ecosystem.
ChronoLog uses physical time to distribute log entries while providing total log ordering.
It also utilizes multiple storage tiers to elastically scale the log capacity (i.e., auto-tiering).
ChronoLog will serve as a foundation for developing scalable new plugins, including a SQL-like query engine for log data, a streaming processor
leveraging the time-based data distribution, a log-based key-value store, and a log-based TensorFlow module.
Learn more at https://www.chronolog.dev

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