#ifndef STORYTELLER_CLIENT_H
#define STORYTELLER_CLIENT_H

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <thallium.hpp>

#include <chrono_monitor.h>
#include <ServiceId.h>
#include <chronolog_types.h>
#include <chronolog_client.h>

namespace chronolog
{

class ChronologTimer
{
public:
    uint64_t getTimestamp();
};

class KeeperRecordingClient;
class PlaybackQueryRpcClient;

class RoundRobinKeeperChoice
{
public:
    KeeperRecordingClient* chooseKeeper(std::vector<KeeperRecordingClient*> const& vectorOfKeepers,
                                        uint64_t chrono_tick)
    {
        uint64_t const bucket_ns = keeperTimeBucketNs();
        uint64_t const keeper_tick = bucket_ns > 1 ? chrono_tick / bucket_ns : chrono_tick;
        return vectorOfKeepers[keeper_tick % vectorOfKeepers.size()];
    }

private:
    static uint64_t keeperTimeBucketNs()
    {
        static uint64_t const bucket_ns = []() {
            char const* value = std::getenv("CHRONOLOG_CLIENT_KEEPER_TIME_BUCKET_NS");
            if(value == nullptr || *value == '\0')
            {
                return uint64_t{0};
            }
            char* end = nullptr;
            unsigned long long parsed = std::strtoull(value, &end, 10);
            if(end == value)
            {
                return uint64_t{0};
            }
            return static_cast<uint64_t>(parsed);
        }();
        return bucket_ns;
    }
};


class StorytellerClient
{
public:
    StorytellerClient(ChronologTimer& chronolog_timer, thallium::engine& client_tl_engine, ClientId const& client_id)
        : theTimer(chronolog_timer)
        , client_engine(client_tl_engine)
        , clientId(client_id)
    {
        LOG_DEBUG("[StorytellerClient] Initialized with ClientID: {}", clientId);
    }

    ~StorytellerClient();

    int addKeeperRecordingClient(ServiceId const&);
    int removeKeeperRecordingClient(ServiceId const&);

    StoryHandle* findStoryWritingHandle(ChronicleName const&, StoryName const&);

    StoryHandle* initializeStoryWritingHandle(ChronicleName const&,
                                              StoryName const&,
                                              StoryId const&,
                                              std::vector<ServiceId> const&,
                                              ServiceId const&);

    void removeAcquiredStoryHandle(ChronicleName const&, StoryName const&);

    uint64_t getTimestamp() { return theTimer.getTimestamp(); }

    ClientId const& getClientId() const { return clientId; }

    int get_event_index();

    //   ServiceId const& get_local_service_id() const    { return theClientQueryService.get_service_id(); }

private:
    StorytellerClient(StorytellerClient const&) = delete;

    StorytellerClient& operator=(StorytellerClient const&) = delete;

    ChronologTimer& theTimer;
    thallium::engine& client_engine;
    ClientId clientId;
    std::atomic<int> atomic_index;

    std::mutex recordingClientMapMutex;
    std::mutex acquiredStoryMapMutex;

    std::map<std::pair<uint32_t, uint16_t>, KeeperRecordingClient*> recordingClientMap;
    std::map<std::pair<std::string, std::string>, StoryHandle*> acquiredStoryHandles;
};


// this class definition lives in the client lib
template <class KeeperChoicePolicy>
class StoryWritingHandle: public StoryHandle
{
public:
    StoryWritingHandle(StorytellerClient& client,
                       ChronicleName const& a_chronicle,
                       StoryName const& a_story,
                       StoryId const& story_id)
        : theClient(client)
        , chronicle(a_chronicle)
        , story(a_story)
        , storyId(story_id)
        , keeperChoicePolicy(new KeeperChoicePolicy)
    //   , playbackQueryClient(nullptr)
    {
        LOG_DEBUG("[StoryWritingHandle] Initialized for Chronicle: {}, Story: {}", a_chronicle, a_story);
    }

    virtual ~StoryWritingHandle();

    virtual uint64_t log_event(std::string const&);
    LogEventFuture log_event_async(std::string const&) override;
    LogEventFuture log_events_async(std::vector<std::string> const&) override;
    LogEventFuture log_events_async_owned(std::vector<std::string>) override;
    uint64_t log_events_bounded_per_keeper(std::vector<std::string> const&,
                                           std::size_t keeper_batch_size,
                                           std::size_t max_outstanding_futures) override;
    std::unique_ptr<PerKeeperBoundedLogEventAppender> make_per_keeper_bounded_appender(
            std::size_t keeper_batch_size,
            std::size_t max_outstanding_futures) override;
    int replay_tail(uint64_t, uint64_t, std::vector<Event>&) override;
    int replay_tail_incremental(uint64_t, std::vector<Event>&) override;
    int replay_tail_incremental_packed(uint64_t, PackedReplayBatch&) override;

    // virtual int log_event(size_t size, void*data);

    void addRecordingClient(KeeperRecordingClient*);
    void removeRecordingClient(ServiceId const&);

private:
    class PerKeeperAppender;

    StorytellerClient& theClient;
    ChronicleName chronicle;
    StoryName story;
    StoryId storyId;
    KeeperChoicePolicy* keeperChoicePolicy;
    std::vector<KeeperRecordingClient*> storyKeepers;
    struct TailCursor
    {
        uint64_t eventTime{1};
        bool initialized{false};
        KeeperTailCursorToken journalCursor;
        using EventKey = std::tuple<uint64_t, uint64_t, uint32_t>;
        std::set<EventKey> seenEvents;
    };
    std::vector<TailCursor> keeperTailCursors;
};

} // namespace chronolog

#endif
