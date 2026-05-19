#ifndef CHRONOLOG_CLIENT_H
#define CHRONOLOG_CLIENT_H

#include <deque>
#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include <cstddef>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <utility>

#include "ClientConfiguration.h"
#include "client_errcode.h"

namespace chronolog
{

typedef std::string StoryName;
typedef std::string ChronicleName;
typedef uint64_t ClientId;
typedef uint64_t chrono_time;
typedef uint32_t chrono_index;

// Wire-protocol version exchanged on Connect. Bump this whenever the wire
// format between the client and any ChronoLog server component changes in
// an incompatible way; the Visor returns CL_ERR_PROTOCOL_VERSION_MISMATCH
// if a connecting client's version doesn't match the server's expectation.
static constexpr uint32_t CLIENT_PROTOCOL_VERSION = 1;

class Event
{
public:
    Event(chrono_time event_time = 0,
          ClientId client_id = 0,
          chrono_index index = 0,
          std::string const& record = std::string())
        : eventTime(event_time)
        , clientId(client_id)
        , eventIndex(index)
        , logRecord(record)
    {}

    Event(chrono_time event_time, ClientId client_id, chrono_index index, std::string&& record)
        : eventTime(event_time)
        , clientId(client_id)
        , eventIndex(index)
        , logRecord(std::move(record))
    {}

    uint64_t time() const { return eventTime; }

    ClientId const& client_id() const { return clientId; }

    uint32_t index() const { return eventIndex; }

    std::string const& log_record() const { return logRecord; }

    Event(Event const& other)
        : eventTime(other.time())
        , clientId(other.client_id())
        , eventIndex(other.index())
        , logRecord(other.log_record())
    {}

    Event(Event&& other) noexcept = default;

    Event& operator=(const Event& other)
    {
        if(this != &other)
        {
            eventTime = other.time();
            clientId = other.client_id();
            eventIndex = other.index();
            logRecord = other.log_record();
        }
        return *this;
    }

    Event& operator=(Event&& other) noexcept = default;

    bool operator==(const Event& other) const
    {
        return (eventTime == other.eventTime && clientId == other.clientId && eventIndex == other.eventIndex);
    }

    bool operator!=(const Event& other) const { return !(*this == other); }

    bool operator<(const Event& other) const
    {
        if((eventTime < other.time()) || (eventTime == other.time() && clientId < other.client_id()) ||
           (eventTime == other.time() && clientId == other.client_id() && eventIndex < other.index()))
        {
            return true;
        }
        else
        {
            return false;
        }
    }


    inline std::string to_string() const
    {
        return "{Event: " + std::to_string(eventTime) + ":" + std::to_string(clientId) + ":" +
               std::to_string(eventIndex) + ":" + logRecord + "}";
    }

private:
    uint64_t eventTime;
    ClientId clientId;
    uint32_t eventIndex;
    std::string logRecord;
};

class PackedReplayBatch
{
public:
    void clear()
    {
        eventTimes.clear();
        clientIds.clear();
        eventIndexes.clear();
        blobIndexes.clear();
        payloadOffsets.clear();
        payloadSizes.clear();
        payloadBlobs.clear();
    }

    [[nodiscard]] std::size_t event_count() const { return eventTimes.size(); }

    [[nodiscard]] std::size_t payload_bytes() const
    {
        std::size_t total = 0;
        for(auto const& blob: payloadBlobs)
        {
            total += blob.size();
        }
        return total;
    }

    [[nodiscard]] chrono_time time(std::size_t index) const { return eventTimes.at(index); }
    [[nodiscard]] ClientId client_id(std::size_t index) const { return clientIds.at(index); }
    [[nodiscard]] chrono_index event_index(std::size_t index) const { return eventIndexes.at(index); }
    [[nodiscard]] std::size_t payload_size(std::size_t index) const
    {
        return static_cast<std::size_t>(payloadSizes.at(index));
    }

    [[nodiscard]] std::string payload(std::size_t index) const
    {
        std::size_t const blob_index = static_cast<std::size_t>(blobIndexes.at(index));
        uint64_t const offset = payloadOffsets.at(index);
        uint64_t const size = payloadSizes.at(index);
        if(blob_index >= payloadBlobs.size())
        {
            throw std::out_of_range("PackedReplayBatch blob index out of range");
        }
        std::string const& blob = payloadBlobs[blob_index];
        if(offset > blob.size() || size > blob.size() - offset)
        {
            throw std::out_of_range("PackedReplayBatch payload range out of range");
        }
        return blob.substr(static_cast<std::size_t>(offset), static_cast<std::size_t>(size));
    }

    std::vector<chrono_time> eventTimes;
    std::vector<ClientId> clientIds;
    std::vector<chrono_index> eventIndexes;
    std::vector<uint32_t> blobIndexes;
    std::vector<uint64_t> payloadOffsets;
    std::vector<uint64_t> payloadSizes;
    std::vector<std::string> payloadBlobs;
};

class LogEventFuture
{
public:
    class State
    {
    public:
        virtual ~State() = default;
        virtual uint64_t wait() = 0;
        virtual std::size_t future_count() const { return 1; }
    };

    LogEventFuture() = default;
    explicit LogEventFuture(std::shared_ptr<State> state) : state(std::move(state)) {}

    [[nodiscard]] bool valid() const { return static_cast<bool>(state); }

    [[nodiscard]] std::size_t future_count() const
    {
        if(!state)
        {
            return 0;
        }
        return state->future_count();
    }

    uint64_t wait()
    {
        if(!state)
        {
            return 0;
        }
        return state->wait();
    }

private:
    std::shared_ptr<State> state;
};

class PerKeeperBoundedLogEventAppender
{
public:
    virtual ~PerKeeperBoundedLogEventAppender() = default;

    virtual uint64_t append(std::string const&) = 0;
    virtual uint64_t append_many(std::vector<std::string>) = 0;
    virtual uint64_t flush() = 0;
    [[nodiscard]] virtual uint64_t future_count() const { return 0; }
    [[nodiscard]] virtual uint64_t future_count_max_per_call() const { return 0; }
    [[nodiscard]] virtual uint64_t future_wait_count() const { return 0; }
    [[nodiscard]] virtual uint64_t future_wait_ns() const { return 0; }
    [[nodiscard]] virtual uint64_t future_wait_max_ns() const { return 0; }
};

class StoryHandle
{
public:
    virtual ~StoryHandle();

    virtual uint64_t log_event(std::string const&) = 0;
    virtual LogEventFuture log_event_async(std::string const&);
    virtual LogEventFuture log_events_async(std::vector<std::string> const&);
    virtual LogEventFuture log_events_async_owned(std::vector<std::string>);
    virtual uint64_t log_events_bounded(std::vector<std::string> const&, std::size_t batch_size,
                                        std::size_t max_outstanding);
    virtual uint64_t log_events_bounded_per_keeper(std::vector<std::string> const&,
                                                   std::size_t keeper_batch_size,
                                                   std::size_t max_outstanding_futures);
    virtual std::unique_ptr<PerKeeperBoundedLogEventAppender> make_per_keeper_bounded_appender(
            std::size_t keeper_batch_size,
            std::size_t max_outstanding_futures);

    virtual int replay_tail(uint64_t, uint64_t, std::vector<Event>&) { return CL_ERR_NO_PLAYERS; }

    virtual int replay_tail_incremental(uint64_t, std::vector<Event>&) { return CL_ERR_NO_PLAYERS; }

    virtual int replay_tail_incremental_packed(uint64_t, PackedReplayBatch&) { return CL_ERR_NO_PLAYERS; }
};

class BoundedLogEventAppender
{
public:
    BoundedLogEventAppender(StoryHandle&, std::size_t batch_size, std::size_t max_outstanding);

    uint64_t append(std::string const&);
    uint64_t append_many(std::vector<std::string>);
    uint64_t flush();

private:
    uint64_t wait_oldest();
    bool submit_batch();

    StoryHandle& story_handle;
    std::size_t batch_size;
    std::size_t max_outstanding;
    std::deque<LogEventFuture> pending_writes;
    std::vector<std::string> pending_batch;
    uint64_t last_timestamp = 0;
};

class ChronologClientImpl;

// Top-level Chronolog Client. Implementation details in ChronologClientImpl.
//
// NOTE: ReleaseStory() must be called for all acquired stories before Disconnect();
// otherwise Disconnect() returns CL_ERR_ACQUIRED (-4) and the client record is not removed.
// TODO: Visor should auto-release acquired stories on Disconnect() — removing this requirement.
// The destructor calls Disconnect() as best-effort; the Client is always safe to delete.
class Client
{
public:
    // client is instantiated in writer only mode, capable of only producing events
    Client(ClientPortalServiceConf const&);

    //client is intantiated in writer/reader mode, capable of both producing and consuming events
    Client(ClientPortalServiceConf const&, ClientQueryServiceConf const&);

    ~Client();

    int Connect();

    int Disconnect();

    int CreateChronicle(std::string const& chronicle_name, std::map<std::string, std::string> const& attrs, int& flags);

    int DestroyChronicle(std::string const& chronicle_name);

    std::pair<int, StoryHandle*> AcquireStory(std::string const& chronicle_name,
                                              std::string const& story_name,
                                              const std::map<std::string, std::string>& attrs,
                                              int& flags);

    int ReleaseStory(std::string const& chronicle_name, std::string const& story_name);

    int DestroyStory(std::string const& chronicle_name, std::string const& story_name);

    int GetChronicleAttr(std::string const& chronicle_name, const std::string& key, std::string& value);

    int EditChronicleAttr(std::string const& chronicle_name, const std::string& key, const std::string& value);

    std::vector<std::string>& ShowChronicles(std::vector<std::string>&);

    std::vector<std::string>& ShowStories(std::string const& chronicle_name, std::vector<std::string>&);

    int ReplayStory(std::string const& chronicle,
                    std::string const& story,
                    uint64_t start,
                    uint64_t end,
                    std::vector<Event>& event_series);

private:
    ChronologClientImpl* chronologClientImpl;
};

} //namespace chronolog

#endif
