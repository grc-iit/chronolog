---
sidebar_position: 1
title: "Setup"
---

# Python Client

The **Python Client** is a wrapper around the Client API, designed to enhance usability and extend ChronoLog's functionality for a broader range of use cases. This client allows users to interact with ChronoLog using Python, making it accessible to data scientists, researchers, and developers who prefer scripting in Python.

### Key Features

- Simplified API for interacting with ChronoLog.
- Compatible with popular Python libraries for data processing and analysis.
- Ideal for prototyping, data analysis, and integration into Python-based pipelines.

### Example Usage

The Python Client is built using the `pybind11` library to create Python bindings for the ChronoLog Client API. Below is an example of a Python test script that demonstrates its usage:

```python
import py_chronolog_client

print("Basic test for py_chronolog_client")

# Create ClientPortalServiceConf instance with connection credentials
clientConf = py_chronolog_client.ClientPortalServiceConf("ofi+sockets", "127.0.0.1", 5555, 55)

# Instantiate ChronoLog Client object
client = py_chronolog_client.Client(clientConf)

# Attempt to acquire a story before connecting
attrs = {}
return_tuple = client.AcquireStory("py_chronicle", "my_story", attrs, 1)
print("\nAttempt to acquire story without ChronoVisor connection returns:", return_tuple)

# Connect to ChronoVisor
return_code = client.Connect()
print("\nclient.Connect() call returns:", return_code)

# Create a chronicle
return_code = client.CreateChronicle("py_chronicle", attrs, 1)
print("\nclient.CreateChronicle() returned:", return_code)

# Acquire a story within the chronicle
return_tuple = client.AcquireStory("py_chronicle", "my_story", attrs, 1)
print("\nclient.AcquireStory() returned:", return_tuple)

if return_tuple[0] == 0:
    print("\nAcquired Story = my_story within Chronicle = py_chronicle")

    # Log events to the story
    print("\nLogging events to my_story")
    story_handle = return_tuple[1]
    story_handle.log_event("py_event")
    story_handle.log_event("py_event.2")
    story_handle.log_event("py_event.3")
    story_handle.log_event("py_event.4")

# Release the story
return_code = client.ReleaseStory("py_chronicle", "my_story")
print("\nclient.ReleaseStory() returned:", return_code)

# Disconnect the client
return_code = client.Disconnect()
print("\nclient.Disconnect() returned:", return_code)
```

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

The Python Client enables seamless interaction with ChronoLog, making it highly adaptable for various scenarios such as event logging, data analysis, and workflow integration.
