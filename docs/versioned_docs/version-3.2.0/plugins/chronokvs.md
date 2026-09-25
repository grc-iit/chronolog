---
sidebar_position: 2
title: "ChronoKVS"
---

# ChronoKVS

ChronoKVS turns ChronoLog into a **multi-version key-value store (MV-KVS)**. It allows storing, retrieving, and auditing key-value pairs with full version history, enabling temporal queries and state reconstruction at any point in time.

### Initialization

:::note
The configuration file defines connection details such as server addresses, ports, and deployment parameters.
:::

It can be used for both local and distributed deployments.

- For distributed deployments, providing a configuration file is mandatory.
- For local deployments, the configuration file is optional and only needed if you want to override the default settings.

ChronoKVS can be initialized using either the default configuration or a user-provided configuration file, depending on the deployment scenario.

```cpp
chronokvs::ChronoKVS();
```

Default constructor. Intended for local deployments using the default configuration. No configuration file is required unless custom settings are needed.

```cpp
chronokvs::ChronoKVS(const std::string& config_path);
```

Constructor that loads configuration from a file. Required for distributed deployments and optional for local deployments when customizing connection or deployment parameters.

### API

```cpp
std::uint64_t put(const std::string& key, const std::string& value);
```

Stores a key-value pair and returns the timestamp assigned to it.

```cpp
std::string get(const std::string& key, uint64_t timestamp);
```

Retrieves the value of a key at the given timestamp. Returns an empty string if no event exists at that exact timestamp.

```cpp
std::vector<EventData> get_history(const std::string& key);
```

Returns the complete historical sequence of values for a key.

```cpp
std::vector<EventData> get_range(const std::string& key, uint64_t start_timestamp, uint64_t end_timestamp);
```

Retrieves all events for the given key within the half-open time interval `[start_timestamp, end_timestamp)`. Returns events with timestamp >= `start_timestamp` and < `end_timestamp`.

```cpp
std::optional<EventData> get_earliest(const std::string& key);
```

Retrieves the earliest (oldest) event for the given key. Returns `std::nullopt` if no events exist for that key.

```cpp
std::optional<EventData> get_latest(const std::string& key);
```

Retrieves the latest (most recent) event for the given key. Returns `std::nullopt` if no events exist for that key.
