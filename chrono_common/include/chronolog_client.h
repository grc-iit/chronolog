#ifndef CHRONOLOG_CLIENT_H
#define CHRONOLOG_CLIENT_H

#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include <memory>
#include <mutex>

#include <chimaera/chimaera.h>

#include "client_errcode.h"

namespace chronolog {

typedef std::string StoryName;
typedef std::string ChronicleName;
typedef uint64_t ClientId;
typedef uint64_t chrono_time;
typedef uint32_t chrono_index;

class Event {
 public:
  Event(chrono_time event_time = 0,
        ClientId client_id = 0,
        chrono_index index = 0,
        std::string const& record = std::string())
      : eventTime(event_time),
        clientId(client_id),
        eventIndex(index),
        logRecord(record) {}

  uint64_t time() const { return eventTime; }
  ClientId const& client_id() const { return clientId; }
  uint32_t index() const { return eventIndex; }
  std::string const& log_record() const { return logRecord; }

  Event(Event const& other)
      : eventTime(other.time()),
        clientId(other.client_id()),
        eventIndex(other.index()),
        logRecord(other.log_record()) {}

  Event& operator=(const Event& other) {
    if (this != &other) {
      eventTime = other.time();
      clientId = other.client_id();
      eventIndex = other.index();
      logRecord = other.log_record();
    }
    return *this;
  }

  bool operator==(const Event& other) const {
    return (eventTime == other.eventTime &&
            clientId == other.clientId &&
            eventIndex == other.eventIndex);
  }

  bool operator!=(const Event& other) const { return !(*this == other); }

  bool operator<(const Event& other) const {
    if (eventTime < other.time()) return true;
    if (eventTime == other.time() && clientId < other.client_id()) return true;
    if (eventTime == other.time() && clientId == other.client_id() &&
        eventIndex < other.index())
      return true;
    return false;
  }

  inline std::string to_string() const {
    return "{Event: " + std::to_string(eventTime) + ":" +
           std::to_string(clientId) + ":" + std::to_string(eventIndex) + ":" +
           logRecord + "}";
  }

 private:
  uint64_t eventTime;
  ClientId clientId;
  uint32_t eventIndex;
  std::string logRecord;
};

class StoryHandle {
 public:
  virtual ~StoryHandle() = default;
  virtual uint64_t log_event(std::string const& record) = 0;
};

class ClientImpl;

/// Wrapper around the new chimod-based keeper + player clients.
/// Presents the same public API that Plugins and tools were written against.
class Client {
 public:
  /// Construct a client that delegates to the keeper and player chimods
  /// identified by the given pool IDs.
  Client(const chi::PoolId& keeper_pool_id,
         const chi::PoolId& player_pool_id);

  ~Client();

  // Non-copyable
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  int CreateChronicle(const std::string& chronicle_name);

  int DestroyChronicle(const std::string& chronicle_name);

  std::pair<int, StoryHandle*> AcquireStory(
      const std::string& chronicle_name,
      const std::string& story_name);

  int ReleaseStory(const std::string& chronicle_name,
                   const std::string& story_name);

  int ReplayStory(const std::string& chronicle_name,
                  const std::string& story_name,
                  uint64_t start,
                  uint64_t end,
                  std::vector<Event>& events);

  int ShowChronicles(std::vector<std::string>& out);

  int ShowStories(const std::string& chronicle_name,
                  std::vector<std::string>& out);

 private:
  std::unique_ptr<ClientImpl> impl_;
};

}  // namespace chronolog

#endif  // CHRONOLOG_CLIENT_H
