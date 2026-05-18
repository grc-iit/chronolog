---
sidebar_position: 2
title: "StoryHandle Class"
---

# StoryHandle Class

The class used to manage all Story-specific data access ChronoLog APIs such as recording.

```cpp
int StoryHandle::log_event(std::string const &event_record)
```

- Record Events to ChronoLog
- Return `CL_SUCCESS` on success, error code otherwise

```cpp
int StoryHandle::playback_story(uint64_t start, uint64_t end, std::vector<Event> &playback_events)
```

- Retrieve all Events in the Story within the timestamp range `[start, end]`.
- The Events are stored in `playback_events`.
- Returns `CL_SUCCESS` on success, an error code otherwise.
- If `start` or `end` timestamps are out of range, an error code `CL_ERR_OUT_OF_RANGE` will be returned.
