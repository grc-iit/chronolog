---
sidebar_position: 1
title: "Overview"
---

# Python Client Overview

The **Python Client** is a wrapper around the Client API, designed to enhance usability and extend ChronoLog's functionality for a broader range of use cases. This client allows users to interact with ChronoLog using Python, making it accessible to data scientists, researchers, and developers who prefer scripting in Python.

### Key Features

- Simplified API for interacting with ChronoLog.
- Compatible with popular Python libraries for data processing and analysis.
- Ideal for prototyping, data analysis, and integration into Python-based pipelines.

### Installation and Setup

1. Build and install ChronoLog, ensuring `py_chronolog_client` is compiled and available in your library directory.
2. Update environment variables:
   - Add the library path to `LD_LIBRARY_PATH`:
     ```bash
     export LD_LIBRARY_PATH=/path/to/chronolog/lib:$LD_LIBRARY_PATH
     ```
   - Add the library path to `PYTHONPATH`:
     ```bash
     export PYTHONPATH=/path/to/chronolog/lib:$PYTHONPATH
     ```
3. Create a symbolic link for the Python client:
   ```bash
   ln -s /path/to/chronolog/lib/py_chronolog_client.[python-version-linux-version].so /path/to/chronolog/lib/py_chronolog_client.so
   ```
