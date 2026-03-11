---
sidebar_position: 2
title: "Scripted Mode"
---

# Scripted Client Admin

The **Scripted Client Admin** extends the functionality of the Interactive Client Admin by enabling automation through scripts. This client is ideal for executing repetitive or predefined tasks without manual intervention.

### Key Features

- Accepts a list of arguments or script files to perform operations.
- Automates the creation, acquisition, and management of Chronicles, Stories, and Events.
- Suitable for running synthetic workloads or batch processing tasks.

### Example Script

Here is an example of a script file (`scripted_workload.sh`) to automate operations:

```bash
# Create a Chronicle and a Story, write events, and clean up
client_admin -c my_chronicle
client_admin -a -s my_chronicle my_story
client_admin -w "Event 1"
client_admin -w "Event 2"
client_admin -q -s my_chronicle my_story
client_admin -d -s my_chronicle my_story
client_admin -d -c my_chronicle
```

### Running the Script

Execute the script using the `-f` flag:

```bash
client_admin -f scripted_workload.sh
```

This example demonstrates how to automate the lifecycle of Chronicles and Stories, making the tool ideal for integration into continuous workflows or stress-testing scenarios.

### Additional Features

- **Performance Testing:** Use the `--perf` flag to collect performance metrics for operations.
- **Shared Stories:** Use the `--shared_story` flag to test collaborative access scenarios.
