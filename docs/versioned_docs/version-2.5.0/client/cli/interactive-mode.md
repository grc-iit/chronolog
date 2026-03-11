---
sidebar_position: 1
title: "Interactive Mode"
---

# Interactive Client Admin

The **Interactive Client Admin** is a command-line tool that provides an interactive interface for users to manage and operate on ChronoLog deployments. This client is designed for ease of use, allowing real-time interaction with the system.

### Key Features

- Interactive mode activated using the `-i` flag.
- Provides a guided interface for managing Chronicles, Stories, and Events.
- Outputs detailed configuration and logging information for troubleshooting and monitoring.

### Supported Commands

```
-c <chronicle_name>         # Create a Chronicle
-a -s <chronicle_name> <story_name>  # Acquire a Story in a Chronicle
-w <event_string>           # Write an Event with a payload
-q -s <chronicle_name> <story_name>  # Release a Story in a Chronicle
-d -s <chronicle_name> <story_name>  # Destroy a Story in a Chronicle
-d -c <chronicle_name>      # Destroy a Chronicle
-disconnect                 # Disconnect from the server
```

### Example Usage

To run the Interactive Client Admin, execute the following command:

```bash
client_admin -i
```

Once launched, the tool will display the list of available commands. You can then enter commands interactively, such as:

```bash
-c my_chronicle
-a -s my_chronicle my_story
-w "This is a test event"
-q -s my_chronicle my_story
-d -s my_chronicle my_story
-d -c my_chronicle
-disconnect
```

The interactive mode is particularly useful for users who are new to ChronoLog or for debugging and monitoring a live system. Error messages and status outputs are displayed after each command to ensure clear feedback.
