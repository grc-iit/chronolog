---
sidebar_position: 4
title: "Examples"
---

# Python Client Examples

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
