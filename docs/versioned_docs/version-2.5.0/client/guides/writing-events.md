---
sidebar_position: 1
title: "Writing Events"
---

# Basic Use of the Client API

The Client API is a foundational component of ChronoLog that allows developers to interact directly with the system programmatically. This is ideal for scenarios where a user or application needs to:

- Write logs to ChronoLog.
- Perform metadata operations such as creating, acquiring, and releasing Chronicles and Stories.
- Integrate ChronoLog into custom applications with precise control.

### Key Features

- Multi-threaded support for concurrent operations.
- Fine-grained control over ChronoLog functionality.

### Example Usage

Below is a simple example that demonstrates the basic use of the ChronoLog Client API without multithreading. This example provides a step-by-step guide to creating a chronicle, acquiring a story, logging events, and cleaning up resources.

```cpp
#include <chronolog_client.h>
#include <iostream>
#include <cassert>

int main() {
    // Configuration file path (update this to your configuration file location)
    std::string conf_file_path = "./conf.json";

    // Initialize the ChronoLog client
    ChronoLog::ConfigurationManager confManager(conf_file_path);
    chronolog::Client client(confManager);

    // Connect to the ChronoVisor
    int ret = client.Connect();
    if (ret != chronolog::CL_SUCCESS) {
        std::cerr << "Failed to connect to ChronoVisor. Error code: " << ret << std::endl;
        return -1;
    }

    // Define the chronicle and story names
    std::string chronicle_name = "example_chronicle";
    std::string story_name = "example_story";

    // Create a chronicle
    std::map<std::string, std::string> chronicle_attrs;
    chronicle_attrs.emplace("Priority", "High");
    ret = client.CreateChronicle(chronicle_name, chronicle_attrs, 0);
    if (ret != chronolog::CL_SUCCESS && ret != chronolog::CL_ERR_CHRONICLE_EXISTS) {
        std::cerr << "Failed to create chronicle. Error code: " << ret << std::endl;
        return -1;
    }

    // Acquire a story in the chronicle
    std::map<std::string, std::string> story_attrs;
    auto acquire_ret = client.AcquireStory(chronicle_name, story_name, story_attrs, 0);
    if (acquire_ret.first != chronolog::CL_SUCCESS) {
        std::cerr << "Failed to acquire story. Error code: " << acquire_ret.first << std::endl;
        return -1;
    }

    // Log events to the story
    auto story_handle = acquire_ret.second;
    for (int i = 0; i < 10; ++i) {
        story_handle->log_event("Event " + std::to_string(i));
    }

    // Release the story
    ret = client.ReleaseStory(chronicle_name, story_name);
    if (ret != chronolog::CL_SUCCESS) {
        std::cerr << "Failed to release story. Error code: " << ret << std::endl;
    }

    // Destroy the story
    ret = client.DestroyStory(chronicle_name, story_name);
    if (ret != chronolog::CL_SUCCESS) {
        std::cerr << "Failed to destroy story. Error code: " << ret << std::endl;
    }

    // Destroy the chronicle
    ret = client.DestroyChronicle(chronicle_name);
    if (ret != chronolog::CL_SUCCESS) {
        std::cerr << "Failed to destroy chronicle. Error code: " << ret << std::endl;
    }

    // Disconnect the client
    ret = client.Disconnect();
    if (ret != chronolog::CL_SUCCESS) {
        std::cerr << "Failed to disconnect. Error code: " << ret << std::endl;
    }

    return 0;
}
```

### Advanced Example: Multi-Threaded Test Case

For users interested in testing or deploying advanced use cases, the `client_lib_multi_storytellers` test demonstrates multi-threaded interactions with ChronoLog. This includes creating chronicles, acquiring and releasing stories, and logging events concurrently across multiple threads.

This test is particularly useful for:

- Evaluating performance under concurrent workloads.
- Simulating real-world scenarios with multiple writers.

The full implementation of `client_lib_multi_storytellers` can be found in the ChronoLog repository.
