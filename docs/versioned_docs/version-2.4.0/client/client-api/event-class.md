---
sidebar_position: 3
title: "Event Class"
---

# Event Class

The Event class represents the smallest data unit in ChronoLog. Each Event contains a timestamp, client ID, an index, and a log record.

### Constructor

```cpp
Event::Event(chrono_time event_time = 0, ClientId client_id = 0, chrono_index index = 0, std::string const &record = std::string())
```

- Creates an Event object.
- event_time: The timestamp of the event.
- client_id: The client ID that generated the event.
- index: The event index for ordering.
- record: The actual data record of the event.

### Getters

```cpp
uint64_t Event::time() const
```

- Returns the timestamp of the Event.

```cpp
ClientId const &Event::client_id() const
```

- Returns the client ID of the Event.

```cpp
uint32_t Event::index() const
```

- Returns the index of the Event.

```cpp
std::string const &Event::log_record() const
```

- Returns the log record of the Event.

### Operators

```cpp
bool Event::operator==(const Event &other) const
```

- Checks if two Events are equal based on event_time, client_id, and eventIndex.

```cpp
bool Event::operator!=(const Event &other) const
```

- Checks if two Events are different.

```cpp
bool Event::operator<(const Event &other) const
```

- Compares Events based on event_time, then client_id, and finally eventIndex.

```cpp
std::string Event::toString() const
```

- Returns a string representation of the Event in the format: `{Event:<eventTime>:<clientId>:<eventIndex>:<logRecord>}`
