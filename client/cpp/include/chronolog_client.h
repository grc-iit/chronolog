#ifndef CHRONOLOG_CLIENT_H
#define CHRONOLOG_CLIENT_H

#include <functional>
#include <string>
#include <tuple>
#include <vector>
#include <map>
#include <cstdint>
#include <fstream>

#include "ClientConfiguration.h"
#include "chronolog_types.h"
#include "client_errcode.h"

namespace chronolog
{

// Wire-protocol version exchanged on Connect. Bump this whenever the wire
// format between the client and any ChronoLog server component changes in
// an incompatible way; the Visor returns CL_ERR_PROTOCOL_VERSION_MISMATCH
// if a connecting client's version doesn't match the server's expectation.
static constexpr uint32_t CLIENT_PROTOCOL_VERSION = 2;

// 64-bit ClientId layout. The high 48 bits are the client's network endpoint
// (IPv4 + port) so a downstream consumer reading back events can identify the
// producer. The low 16 bits disambiguate multiple writer-only processes
// running on the same host (where port is 0).
//   bits [63:32] : ipv4 address in host byte order (0 if undetermined)
//   bits [31:16] : tcp/udp port (query service port for reader-mode clients,
//                  0 for writer-only clients)
//   bits [15:0]  : instance discriminator (pid & 0xFFFF)
struct ClientIdentity
{
    uint32_t ip = 0;
    uint16_t port = 0;
    uint16_t instance = 0;

    ClientId pack() const
    {
        return (static_cast<ClientId>(ip) << 32) | (static_cast<ClientId>(port) << 16) |
               static_cast<ClientId>(instance);
    }

    static ClientIdentity unpack(ClientId id)
    {
        return ClientIdentity{static_cast<uint32_t>(id >> 32),
                              static_cast<uint16_t>((id >> 16) & 0xFFFFu),
                              static_cast<uint16_t>(id & 0xFFFFu)};
    }
};

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

    uint64_t time() const { return eventTime; }

    ClientId const& client_id() const { return clientId; }

    uint32_t index() const { return eventIndex; }

    std::string const& log_record() const { return logRecord; }

    EventSequence sequence() const { return EventSequence{eventTime, clientId, eventIndex}; }

    Event(Event const& other)
        : eventTime(other.time())
        , clientId(other.client_id())
        , eventIndex(other.index())
        , logRecord(other.log_record())
    {}

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

    // serialization function used by thallium RPC providers
    template <typename SerArchiveT>
    void serialize(SerArchiveT& serT)
    {
        serT(eventTime, clientId, eventIndex, logRecord);
    }

private:
    uint64_t eventTime;
    ClientId clientId;
    uint32_t eventIndex;
    std::string logRecord;
};

class StoryHandle
{
public:
    virtual ~StoryHandle();

    virtual uint64_t log_event(std::string const&) = 0;

    // Tail read: play back the most recent `n` events of this story directly
    // from the assigned keepers' in-memory tail (sealed-but-not-yet-extracted
    // events), bypassing the player/archive. The client gathers each keeper's
    // last-N event sequences, selects the global last-N, then fetches just
    // those payloads. On success returns CL_SUCCESS and fills `events` in
    // ascending event order. Default implementation is a no-op.
    virtual int playback(size_t n, std::vector<Event>& events)
    {
        (void)n;
        (void)events;
        return CL_ERR_UNKNOWN;
    }
};

class ChronologClientImpl;

// Top-level Chronolog Client. Implementation details in ChronologClientImpl.
//
// Disconnect() auto-releases any stories still acquired by the client so the
// caller never has to walk its own acquisitions to clean up. The destructor
// calls Disconnect() as best-effort; the Client is always safe to delete.
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

    // Returns this client's packed ClientId — the same value that appears in
    // Event::client_id() / EventSequence::clientId for every event produced
    // by this client and surfaced at retrieval. Valid only after a successful
    // Connect(); returns 0 before that.
    ClientId client_id() const;

    int CreateChronicle(std::string const& chronicle_name);

    int DestroyChronicle(std::string const& chronicle_name);

    std::pair<int, StoryHandle*> AcquireStory(std::string const& chronicle_name, std::string const& story_name);

    int ReleaseStory(std::string const& chronicle_name, std::string const& story_name);

    int DestroyStory(std::string const& chronicle_name, std::string const& story_name);

    std::pair<int, std::vector<std::string>> ShowChronicles();

    std::pair<int, std::vector<std::string>> ShowStories(std::string const& chronicle_name);

    int ReplayStory(std::string const& chronicle,
                    std::string const& story,
                    uint64_t start,
                    uint64_t end,
                    std::vector<Event>& event_series);

    // Streaming overload: invoke `callback` once per event in the requested
    // [start, end) range, with no client-side std::vector<Event> materialization.
    //
    // The callback runs on the query service receive thread; the caller is
    // responsible for any locking it needs.
    //
    // The callback must not throw. If it does, the exception is caught and
    // logged, that event is skipped, and the remaining events in the response
    // are still delivered. Letting an exception escape into the Thallium ULT
    // would skip the RPC response and hang the polling thread until
    // CL_ERR_QUERY_TIMED_OUT.
    //
    // Scope note: this overload removes the client-side vector materialization,
    // but the wire-level response is unchanged — the Player still ships one
    // PlaybackQueryResponse per query and the events are fully deserialized
    // into a server-supplied buffer before the callback loop runs. Player-side
    // chunked delivery is tracked as a follow-up.
    using EventCallback = std::function<void(Event const&)>;
    int ReplayStory(std::string const& chronicle,
                    std::string const& story,
                    uint64_t start,
                    uint64_t end,
                    EventCallback callback);

private:
    ChronologClientImpl* chronologClientImpl;
};

} //namespace chronolog

#endif
