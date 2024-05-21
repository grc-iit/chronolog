# ChronoLog
![OPEN SOURCE](https://img.shields.io/badge/GNOSIS_RESEARCH_CENTER-blue)
![OPEN SOURCE](https://img.shields.io/badge/OPEN_SOURCE-grey)
[![License](https://img.shields.io/github/license/grc-iit/ChronoLog.svg)](LICENSE)
[![GitHub issues](https://img.shields.io/github/issues/grc-iit/ChronoLog.svg)](https://github.com/grc-iit/ChronoLog/issues)

[![Main_Branch](https://img.shields.io/badge/Branch-Main-green)](https://github.com/grc-iit/ChronoLog/tree/main)
[![Develop_Branch](https://img.shields.io/badge/Branch-Develop-yellow)](https://github.com/grc-iit/ChronoLog/tree/develop)
[![GitHub release](https://img.shields.io/github/release/grc-iit/ChronoLog.svg)](https://github.com/grc-iit/ChronoLog/releases/latest)

This project will design and implement ChronoLog, a distributed and tiered shared log storage ecosystem. 
ChronoLog uses physical time to distribute log entries while providing total log ordering. 
It also utilizes multiple storage tiers to elastically scale the log capacity (i.e., auto-tiering). 
ChronoLog will serve as a foundation for developing scalable new plugins, including a SQL-like query engine for log data, a streaming processor 
leveraging the time-based data distribution, a log-based key-value store, and a log-based TensorFlow module.

<div style="text-align: center;">
    <img src="https://raw.githubusercontent.com/grc-iit/ChronoLog/169-convert-readme-into-a-landing-page/doc/images/logos/logo-chronolog.png" alt="ChronoLog" width="600">
</div>

# Project Wiki
<a href="https://github.com/grc-iit/ChronoLog/wiki/1.-Getting-Started">
    <img src="https://img.shields.io/badge/-1.Getting_Started-informational" alt="Getting Started" style="zoom: 150%;">
</a>
<a href="https://github.com/grc-iit/ChronoLog/wiki/2.-Installation">
    <img src="https://img.shields.io/badge/-2.Installation-informational" alt="Installation" style="zoom: 150%;">
</a>
<a href="https://github.com/grc-iit/ChronoLog/wiki/3.-Configuration">
    <img src="https://img.shields.io/badge/-3.Configuration-informational" alt="Configuration" style="zoom: 150%;">
</a>
<a href="https://github.com/grc-iit/ChronoLog/wiki/4.-Technical-Reference">
    <img src="https://img.shields.io/badge/-4.Technical_Reference-informational" alt="Technical Reference" style="zoom: 150%;">
</a>
<a href="https://github.com/grc-iit/ChronoLog/wiki/5.-Contributors-Guidelines">
    <img src="https://img.shields.io/badge/-5.Contributors_Guidelines-informational" alt="Contributors Guidelines" style="zoom: 150%;">
</a>
<a href="https://github.com/grc-iit/ChronoLog/wiki/6.-Code-Style-Guidelines">
    <img src="https://img.shields.io/badge/-6.Code_Style_Guidelines-informational" alt="Code Style Guidelines" style="zoom: 150%;">
</a>

# Members

<a href="https://www.iit.edu">
    <img src="https://raw.githubusercontent.com/grc-iit/ChronoLog/169-convert-readme-into-a-landing-page/doc/images/logos/IIT.png" alt="Illinois Tech" width="150">
</a>

<a href="https://www.uchicago.edu/">
    <img src="https://raw.githubusercontent.com/grc-iit/ChronoLog/169-convert-readme-into-a-landing-page/doc/images/logos/university-of-chicago.png" alt="University Of Chicago" width="150">
</a>

# Collaborators
<a href="https://www.llnl.gov/">
    <img src="https://raw.githubusercontent.com/grc-iit/ChronoLog/169-convert-readme-into-a-landing-page/doc/images/logos/llnl.jpg" alt="Lawrence Livermore National Lab" width="75">
</a>
<a href="https://www6.slac.stanford.edu/">
    <img src="https://raw.githubusercontent.com/grc-iit/ChronoLog/169-convert-readme-into-a-landing-page/doc/images/logos/slac.png" alt="SLAC National Accelerator Lab" width="75">
</a>
<a href="https://www.3redpartners.com/">
    <img src="https://raw.githubusercontent.com/grc-iit/ChronoLog/169-convert-readme-into-a-landing-page/doc/images/logos/3red.png" alt="3RedPartners" width="75">
</a>
<a href="https://www.depaul.edu/">
    <img src="https://raw.githubusercontent.com/grc-iit/ChronoLog/169-convert-readme-into-a-landing-page/doc/images/logos/depaul.png" alt="DePaul University" width="75">
</a>
<a href="https://www.wisc.edu/">
    <img src="https://raw.githubusercontent.com/grc-iit/ChronoLog/169-convert-readme-into-a-landing-page/doc/images/logos/university-of-wisconsin.jpg" alt="University of Wisconsin Madison" width="75">
</a>
<a href="https://www.paratools.com/">
    <img src="https://raw.githubusercontent.com/grc-iit/ChronoLog/169-convert-readme-into-a-landing-page/doc/images/logos/paratools.png" alt="ParaTools, Inc." width="75">
</a>
<a href="https://illinois.edu/">
    <img src="https://raw.githubusercontent.com/grc-iit/ChronoLog/169-convert-readme-into-a-landing-page/doc/images/logos/university-of-illinois.jpg" alt="University of Illinois at Urbana-Champaign" width="75">
</a>
<a href="https://www.anl.gov/">
    <img src="https://raw.githubusercontent.com/grc-iit/ChronoLog/169-convert-readme-into-a-landing-page/doc/images/logos/argonne.jpeg" alt="Argonne National Lab" width="75">
</a>
<a href="https://omnibond.com/">
    <img src="https://raw.githubusercontent.com/grc-iit/ChronoLog/169-convert-readme-into-a-landing-page/doc/images/logos/omnibond.png" alt="OmniBond Systems LLC" width="75">
</a>


# Sponsors
<a href="https://www.nsf.gov">
    <img src="https://raw.githubusercontent.com/grc-iit/ChronoLog/169-convert-readme-into-a-landing-page/doc/images/logos/nsf-fb7efe9286a9b499c5907d82af3e70fd.png" alt="NSFLOGO" width="150">
</a>

National Science Foundation (NSF CSSI-2104013)

# Learn More
[![WEBSITE](https://img.shields.io/badge/-Website-blue?style=flat-square&logo=Wordpress&logoColor=white&link=https://www.chronolog.dev)](https://www.chronolog.dev)
[![Linkedin Badge](https://img.shields.io/badge/-LinkedIn-blue?style=flat-square&logo=Linkedin&logoColor=white&link=https://www.linkedin.com/school/gnosis-research-center/)](https://www.linkedin.com/school/gnosis-research-center/)
[![GitHub Badge](https://img.shields.io/badge/-GitHub-black?style=flat-square&logo=Github&logoColor=white&link=https://www.linkedin.com/school/gnosis-research-center/)](https://www.linkedin.com/school/gnosis-research-center/)
[![X Badge](https://img.shields.io/badge/-Twitter-black?style=flat-square&logo=X&logoColor=white&link=https://www.linkedin.com/school/gnosis-research-center/)](https://www.linkedin.com/school/gnosis-research-center/)
[![YouTube Badge](https://img.shields.io/badge/-YouTube-red?style=flat-square&logo=Youtube&logoColor=white&link=https://www.linkedin.com/school/gnosis-research-center/)](https://www.linkedin.com/school/gnosis-research-center/)

