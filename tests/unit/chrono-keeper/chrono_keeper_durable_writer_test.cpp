// The keeper's durable write path: the order of its checks, that nothing is
// ingested unless the WAL took the event, and the pipeline a durable write
// starts for a story the keeper does not record yet.
//
// The WAL is a fake DurableLog that records its calls; the keeper data path
// runs in-process (no daemons, no network), as in
// chrono_keeper_wal_replay_test.cpp.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <abt.h>
#include <thallium.hpp>
#include <thallium/serialization/stl/string.hpp>

#include <chrono_monitor.h>
#include <chronolog_errcode.h>
#include <chronolog_types.h>

#include <ChunkReceipt.h>
#include <IngestionQueue.h>
#include <KeeperChunkRetentionStore.h>
#include <KeeperDataStore.h>
#include <KeeperDurableWriter.h>
#include <KeeperRecordingService.h>
#include <KeeperWal.h>
#include <StoryChunk.h>
#include <StoryChunkExtractionQueue.h>
#include <StoryIngestionHandle.h>
#include <WalObserver.h>

namespace chl = chronolog;
namespace tl = thallium;

namespace
{
void ensureLogger() // and Argobots
{
    static bool done = false;
    if(!done)
    {
        chl::chrono_monitor::initialize("console", "", chronolog::LogLevel::err, "durable_writer_test_logger");
        // the data store logs Argobots thread ids. Never finalized, so margo
        // leaves Argobots alone at engine finalize.
        ABT_init(0, nullptr);
        done = true;
    }
}

constexpr uint32_t kChunkDurationSecs = 30;
constexpr std::size_t kTailCapacity = 1024;
constexpr uint64_t kNoCap = UINT64_MAX;
constexpr chl::StoryId kStory = 41;

uint64_t now() { return std::chrono::high_resolution_clock::now().time_since_epoch().count(); }

// The keeper's data store and the queues it works on, with no services. Live
// tail reads are on so an event a pipeline collected is visible before its
// chunk seals. The acceptance window is 0 so the pipelines a test leaves
// behind retire at once when the Keeper goes away.
struct Keeper
{
    explicit Keeper(uint32_t acceptance_window_secs = 0)
        : data_store(ingestion_queue,
                     extraction_queue,
                     retention_store,
                     /*max_chunk_size=*/4096,
                     /*story_chunk_duration_secs=*/kChunkDurationSecs,
                     acceptance_window_secs,
                     /*inactive_pipeline_delay_secs=*/300)
    {}

    // The keeper's shutdown puts every pipeline on the waiting-for-exit list
    // and its collection threads retire them, which deletes them. No
    // collection thread runs here, so retire them the way one would.
    ~Keeper()
    {
        data_store.shutdownDataCollection();
        data_store.retireDecayedPipelines();
    }

    chl::IngestionQueue ingestion_queue;
    chl::StoryChunkExtractionQueue extraction_queue;
    chl::KeeperChunkRetentionStore retention_store{extraction_queue, kTailCapacity, 0, /*live_tail_read=*/true};
    chl::KeeperDataStore data_store;

    // The events the keeper took in for the story: collected into its
    // pipeline, or waiting as orphans when it has none.
    std::size_t ingested(chl::StoryId story_id)
    {
        data_store.collectIngestedEvents();
        std::size_t count = retention_store.getTailSequences(story_id, kTailCapacity).size();
        if(ingestion_queue.orphanStoryIds().count(story_id) != 0)
        {
            count += ingestion_queue.extractOrphansForStory(story_id).size();
        }
        return count;
    }
};

chl::EventSequence identityOf(chl::LogEvent const& event)
{
    return chl::EventSequence{event.time(), event.getClientId(), event.index()};
}

chl::LogEvent makeEvent(uint32_t index, std::string const& record = "payload")
{
    return chl::LogEvent(kStory, now(), /*client=*/9, index, record);
}

// A DurableLog that answers as told and records every call, in order.
class FakeDurableLog: public chl::DurableLog
{
public:
    chl::WalAdmit admit() const override
    {
        calls.push_back("admit");
        return admitAnswer;
    }

    bool isDurable(chl::StoryId story_id, chl::EventSequence const& identity) const override
    {
        return durable.count({story_id, identity}) != 0;
    }

    std::optional<int> lookupDurable(chl::StoryId story_id, chl::EventSequence const& identity) override
    {
        calls.push_back("lookup");
        if(lookupAnswer.has_value())
        {
            return lookupAnswer;
        }
        if(isDurable(story_id, identity))
        {
            return chl::CL_SUCCESS;
        }
        return std::nullopt;
    }

    int
    appendDurable(chl::ChronicleName const& chronicle, chl::StoryName const& story, chl::LogEvent const& event) override
    {
        calls.push_back("append");
        appended.push_back({chronicle, story});
        if(onAppend)
        {
            onAppend();
        }
        if(appendAnswer == chl::CL_SUCCESS)
        {
            durable.insert({event.getStoryId(), identityOf(event)});
        }
        return appendAnswer;
    }

    chl::WalAdmit admitAnswer = chl::WalAdmit::Ok;
    // when set, lookupDurable returns it for every identity: the result of an
    // earlier attempt the request waited for
    std::optional<int> lookupAnswer;
    int appendAnswer = chl::CL_SUCCESS;
    std::function<void()> onAppend;

    std::set<std::pair<chl::StoryId, chl::EventSequence>> durable;
    mutable std::vector<std::string> calls;
    std::vector<std::pair<chl::ChronicleName, chl::StoryName>> appended;
};

// Records the chunks the retention store discards for a destroyed story.
class DiscardObserver: public chl::WalObserver
{
public:
    void chunkWritten(chl::StoryId, chl::StoryChunk const&) override {}
    void chunkDiscarded(chl::StoryId story_id, chl::StoryChunk const&) override { discarded.push_back(story_id); }
    void eventDiscarded(chl::StoryId, chl::EventSequence const&) override {}
    void storyDropped(chl::StoryId) override {}

    std::vector<chl::StoryId> discarded;
};

chl::StoryChunk* sealedChunk(chl::StoryId story_id, uint64_t start)
{
    auto* chunk = new chl::StoryChunk("chron", "story", story_id, start, start + 1000);
    chunk->insertEvent(chl::LogEvent(story_id, start, /*client=*/3, /*index=*/0, "sealed"));
    return chunk;
}
} // namespace

TEST(KeeperDurableWriter, WalOffIsUnavailable)
{
    ensureLogger();
    Keeper keeper;
    chl::KeeperDurableWriter writer(nullptr, keeper.ingestion_queue, keeper.retention_store, keeper.data_store, kNoCap);

    EXPECT_EQ(writer.write("chron", "story", makeEvent(0)), chl::CL_ERR_DURABILITY_UNAVAILABLE);
    EXPECT_EQ(keeper.ingested(kStory), 0u);
}

// A retry of an event the WAL already holds is answered from the WAL before
// any other check, so a full or failed WAL does not refuse an event that is on
// disk, and the event is not taken in again. The WAL holds the event from an
// earlier attempt and the story has no pipeline, so an ingest here would show
// as an orphan.
TEST(KeeperDurableWriter, LookupPrecedesEveryOtherCheck)
{
    ensureLogger();
    Keeper keeper;
    FakeDurableLog wal;
    chl::KeeperDurableWriter writer(&wal, keeper.ingestion_queue, keeper.retention_store, keeper.data_store, kNoCap);

    chl::LogEvent const event = makeEvent(0);
    wal.durable.insert({kStory, identityOf(event)});

    for(chl::WalAdmit answer: {chl::WalAdmit::Full, chl::WalAdmit::Unavailable})
    {
        wal.admitAnswer = answer;
        wal.calls.clear();
        EXPECT_EQ(writer.write("chron", "story", event), chl::CL_SUCCESS);
        EXPECT_EQ(wal.calls, (std::vector<std::string>{"lookup"}));
    }

    // the same for a cap the retained data exceeds
    keeper.retention_store.ingestSealedChunk(77, sealedChunk(77, 1000));
    chl::KeeperDurableWriter capped(&wal, keeper.ingestion_queue, keeper.retention_store, keeper.data_store, 0);
    wal.calls.clear();
    EXPECT_EQ(capped.write("chron", "story", event), chl::CL_SUCCESS);
    EXPECT_EQ(wal.calls, (std::vector<std::string>{"lookup"}));

    EXPECT_TRUE(wal.appended.empty());
    EXPECT_EQ(keeper.ingested(kStory), 0u);
}

// An earlier attempt with the same identity failed while this request waited
// for it: its answer is this request's answer, and nothing is admitted,
// appended or ingested.
TEST(KeeperDurableWriter, FailedEarlierAttemptIsReturned)
{
    ensureLogger();
    Keeper keeper;
    FakeDurableLog wal;
    chl::KeeperDurableWriter writer(&wal, keeper.ingestion_queue, keeper.retention_store, keeper.data_store, kNoCap);

    wal.lookupAnswer = chl::CL_ERR_DURABILITY_UNAVAILABLE;
    EXPECT_EQ(writer.write("chron", "story", makeEvent(0)), chl::CL_ERR_DURABILITY_UNAVAILABLE);
    EXPECT_EQ(wal.calls, (std::vector<std::string>{"lookup"}));
    EXPECT_TRUE(wal.appended.empty());
    EXPECT_EQ(keeper.ingested(kStory), 0u);
}

// Today the pipeline drops an empty record after answering, which would be a
// false durable answer; the lookup still comes first.
TEST(KeeperDurableWriter, EmptyRecordIsRefusedAfterTheLookup)
{
    ensureLogger();
    Keeper keeper;
    FakeDurableLog wal;
    chl::KeeperDurableWriter writer(&wal, keeper.ingestion_queue, keeper.retention_store, keeper.data_store, kNoCap);

    EXPECT_EQ(writer.write("chron", "story", makeEvent(0, "")), chl::CL_ERR_INVALID_ARG);
    EXPECT_EQ(wal.calls, (std::vector<std::string>{"lookup"}));
    EXPECT_TRUE(wal.appended.empty());
    EXPECT_EQ(keeper.ingested(kStory), 0u);
}

TEST(KeeperDurableWriter, FullAndCapAreFullAndNothingIngested)
{
    ensureLogger();
    Keeper keeper;
    FakeDurableLog wal;
    chl::KeeperDurableWriter writer(&wal, keeper.ingestion_queue, keeper.retention_store, keeper.data_store, kNoCap);

    wal.admitAnswer = chl::WalAdmit::Full;
    EXPECT_EQ(writer.write("chron", "story", makeEvent(0)), chl::CL_ERR_DURABILITY_FULL);
    wal.admitAnswer = chl::WalAdmit::Unavailable;
    EXPECT_EQ(writer.write("chron", "story", makeEvent(1)), chl::CL_ERR_DURABILITY_UNAVAILABLE);
    EXPECT_TRUE(wal.appended.empty());
    EXPECT_EQ(keeper.ingested(kStory), 0u);

    // the retention store holds more than the cap: full, though the WAL admits
    wal.admitAnswer = chl::WalAdmit::Ok;
    keeper.retention_store.ingestSealedChunk(77, sealedChunk(77, 1000));
    uint64_t const retained = keeper.retention_store.retainedByteCount();
    ASSERT_GT(retained, 0u);
    chl::KeeperDurableWriter capped(&wal,
                                    keeper.ingestion_queue,
                                    keeper.retention_store,
                                    keeper.data_store,
                                    retained - 1);
    EXPECT_EQ(capped.write("chron", "story", makeEvent(2)), chl::CL_ERR_DURABILITY_FULL);
    EXPECT_TRUE(wal.appended.empty());
    EXPECT_EQ(keeper.ingested(kStory), 0u);

    // at the cap the write goes through
    chl::KeeperDurableWriter at_cap(&wal, keeper.ingestion_queue, keeper.retention_store, keeper.data_store, retained);
    EXPECT_EQ(at_cap.write("chron", "story", makeEvent(3)), chl::CL_SUCCESS);
    EXPECT_EQ(wal.appended.size(), 1u);
    EXPECT_EQ(keeper.ingested(kStory), 1u);
}

TEST(KeeperDurableWriter, FailedAppendIsNotIngested)
{
    ensureLogger();
    Keeper keeper;
    FakeDurableLog wal;
    chl::KeeperDurableWriter writer(&wal, keeper.ingestion_queue, keeper.retention_store, keeper.data_store, kNoCap);

    for(int answer: {chl::CL_ERR_DURABILITY_UNAVAILABLE, chl::CL_ERR_INVALID_ARG})
    {
        wal.appendAnswer = answer;
        EXPECT_EQ(writer.write("chron", "story", makeEvent(0)), answer);
    }
    EXPECT_EQ(wal.appended.size(), 2u);
    EXPECT_EQ(keeper.ingested(kStory), 0u);
}

// No visor start for the story has reached this keeper (it restarted while
// the client held the story): the durable write starts the pipeline, so the
// event is collected into it instead of waiting as an orphan.
TEST(KeeperDurableWriter, UnknownStoryGetsAPipeline)
{
    ensureLogger();
    Keeper keeper;
    FakeDurableLog wal;
    chl::KeeperDurableWriter writer(&wal, keeper.ingestion_queue, keeper.retention_store, keeper.data_store, kNoCap);

    chl::LogEvent const event = makeEvent(0);
    ASSERT_EQ(writer.write("chron", "story", event), chl::CL_SUCCESS);
    EXPECT_EQ(wal.appended, (std::vector<std::pair<chl::ChronicleName, chl::StoryName>>{{"chron", "story"}}));

    EXPECT_TRUE(keeper.ingestion_queue.orphanStoryIds().empty());
    keeper.data_store.collectIngestedEvents();
    EXPECT_EQ(keeper.retention_store.getTailSequences(kStory, kTailCapacity),
              (std::vector<chl::EventSequence>{identityOf(event)}));

    // a second write uses the same pipeline; a visor start that arrives later
    // finds it too
    ASSERT_EQ(writer.write("chron", "story", makeEvent(1)), chl::CL_SUCCESS);
    EXPECT_EQ(keeper.data_store.startStoryRecording("chron", "story", kStory, now()), chl::CL_SUCCESS);
    EXPECT_EQ(keeper.ingested(kStory), 2u);
}

// Nothing on the keeper stops a story whose pipeline a durable write started,
// so it decays as a stopped story's does: it retires an acceptance window
// later (0 here), unless a visor start takes it over first.
TEST(KeeperDurableWriter, DurablePipelineRetiresUnlessAStartKeepsIt)
{
    ensureLogger();
    constexpr chl::StoryId kKept = kStory + 1;
    Keeper keeper(/*acceptance_window_secs=*/0);

    ASSERT_EQ(keeper.data_store.ensureStoryPipeline("chron", "story", kStory), chl::CL_SUCCESS);
    ASSERT_EQ(keeper.data_store.ensureStoryPipeline("chron", "kept", kKept), chl::CL_SUCCESS);
    ASSERT_EQ(keeper.data_store.startStoryRecording("chron", "kept", kKept, now()), chl::CL_SUCCESS);
    keeper.data_store.retireDecayedPipelines();

    // the retired story has no ingestion handle, so its next event is an
    // orphan; the kept story's goes into its pipeline
    keeper.ingestion_queue.ingestLogEvent(makeEvent(0));
    keeper.ingestion_queue.ingestLogEvent(chl::LogEvent(kKept, now(), /*client=*/9, 0, "payload"));
    EXPECT_EQ(keeper.ingestion_queue.orphanStoryIds(), (std::unordered_set<chl::StoryId>{kStory}));

    // a durable write for the retired story starts a new pipeline, which takes
    // the orphan in at the next collection
    FakeDurableLog wal;
    chl::KeeperDurableWriter writer(&wal, keeper.ingestion_queue, keeper.retention_store, keeper.data_store, kNoCap);
    ASSERT_EQ(writer.write("chron", "story", makeEvent(1)), chl::CL_SUCCESS);
    EXPECT_EQ(keeper.ingested(kStory), 2u);
    EXPECT_EQ(keeper.ingested(kKept), 1u);
}

// ensureStoryPipeline leaves a pipeline that exists as it is: a stop that put
// it on the waiting-for-exit list still retires it (here at once, with a zero
// acceptance window), and a pipeline a visor start owns is not put on that
// list.
TEST(KeeperDurableWriter, EnsureStoryPipelineLeavesAnExistingPipelineAlone)
{
    ensureLogger();
    constexpr chl::StoryId kStopped = kStory;
    constexpr chl::StoryId kRecorded = kStory + 1;
    Keeper keeper(/*acceptance_window_secs=*/0);

    ASSERT_EQ(keeper.data_store.startStoryRecording("chron", "stopped", kStopped, now()), chl::CL_SUCCESS);
    ASSERT_EQ(keeper.data_store.stopStoryRecording(kStopped), chl::CL_SUCCESS);
    ASSERT_EQ(keeper.data_store.ensureStoryPipeline("chron", "stopped", kStopped), chl::CL_SUCCESS);

    ASSERT_EQ(keeper.data_store.startStoryRecording("chron", "recorded", kRecorded, now()), chl::CL_SUCCESS);
    ASSERT_EQ(keeper.data_store.ensureStoryPipeline("chron", "recorded", kRecorded), chl::CL_SUCCESS);

    keeper.data_store.retireDecayedPipelines();

    // the stopped story's pipeline is gone, so its next event is an orphan;
    // the recorded story's event reaches its pipeline
    keeper.ingestion_queue.ingestLogEvent(chl::LogEvent(kStopped, now(), /*client=*/9, 0, "payload"));
    keeper.ingestion_queue.ingestLogEvent(chl::LogEvent(kRecorded, now(), /*client=*/9, 0, "payload"));
    EXPECT_EQ(keeper.ingestion_queue.orphanStoryIds(), (std::unordered_set<chl::StoryId>{kStopped}));
    EXPECT_EQ(keeper.ingested(kRecorded), 1u);
}

// The writer asks the ingestion queue, not the data store, whether the story
// has a pipeline, so a durable write for a recorded story never waits on the
// data store's mutex, which the collector holds for a whole tick. Here the
// story has an ingestion handle and no pipeline: the write must not start one.
// If it did, that pipeline would retire at once (zero acceptance window) and
// take the handle out of the ingestion queue with it.
TEST(KeeperDurableWriter, StoryWithAnIngestionHandleSkipsTheDataStore)
{
    ensureLogger();
    Keeper keeper(/*acceptance_window_secs=*/0);
    std::mutex handle_mutex;
    chl::EventDeque active;
    chl::EventDeque passive;
    chl::StoryIngestionHandle handle(handle_mutex, &active, &passive);
    keeper.ingestion_queue.addStoryIngestionHandle(kStory, &handle);

    FakeDurableLog wal;
    chl::KeeperDurableWriter writer(&wal, keeper.ingestion_queue, keeper.retention_store, keeper.data_store, kNoCap);
    ASSERT_EQ(writer.write("chron", "story", makeEvent(0)), chl::CL_SUCCESS);
    EXPECT_EQ(active.size(), 1u);

    keeper.data_store.retireDecayedPipelines();
    keeper.ingestion_queue.ingestLogEvent(makeEvent(1));
    EXPECT_TRUE(keeper.ingestion_queue.orphanStoryIds().empty());
    EXPECT_EQ(active.size(), 2u);

    keeper.ingestion_queue.removeIngestionHandle(kStory);
}

// A stale writer must not undo a destroy: the pipeline a durable write starts
// leaves the retention store's drop mark, so a chunk of the destroyed story
// that seals afterwards is still discarded (and its WAL records released).
// A visor start, by contrast, clears it.
TEST(KeeperDurableWriter, EnsureStoryPipelineKeepsTheDropMark)
{
    ensureLogger();
    Keeper keeper;
    DiscardObserver observer;
    keeper.retention_store.setWalObserver(&observer);

    keeper.data_store.applyWatermarkReport(kStory, chl::kStoryDroppedWatermark);
    ASSERT_EQ(keeper.data_store.ensureStoryPipeline("chron", "story", kStory), chl::CL_SUCCESS);
    keeper.retention_store.ingestSealedChunk(kStory, sealedChunk(kStory, 1000));
    EXPECT_EQ(observer.discarded, (std::vector<chl::StoryId>{kStory}));
    EXPECT_EQ(keeper.retention_store.retainedChunkCount(kStory), 0u);

    // through the writer as well
    FakeDurableLog wal;
    chl::KeeperDurableWriter writer(&wal, keeper.ingestion_queue, keeper.retention_store, keeper.data_store, kNoCap);
    ASSERT_EQ(writer.write("chron", "story", makeEvent(0)), chl::CL_SUCCESS);
    keeper.retention_store.ingestSealedChunk(kStory, sealedChunk(kStory, 2000));
    EXPECT_EQ(observer.discarded, (std::vector<chl::StoryId>{kStory, kStory}));
    EXPECT_EQ(keeper.retention_store.retainedChunkCount(kStory), 0u);

    // startStoryRecording means the id is in use again
    ASSERT_EQ(keeper.data_store.startStoryRecording("chron", "story", kStory, now()), chl::CL_SUCCESS);
    keeper.retention_store.ingestSealedChunk(kStory, sealedChunk(kStory, 3000));
    EXPECT_EQ(observer.discarded.size(), 2u);
    EXPECT_EQ(keeper.retention_store.retainedChunkCount(kStory), 1u);

    keeper.retention_store.setWalObserver(nullptr);
}

// record_event_durable is defined on the recording service whether the WAL is
// on or off, so a client of a keeper without a WAL gets UNAVAILABLE instead of
// "not registered". Over ofi+sockets on 127.0.0.1.
TEST(KeeperDurableWriter, RecordEventDurableAnswersWithTheWalOff)
{
    ensureLogger();
    constexpr uint16_t kProvider = 31;
    Keeper keeper;
    chl::KeeperDurableWriter writer(nullptr, keeper.ingestion_queue, keeper.retention_store, keeper.data_store, kNoCap);
    tl::engine keeper_engine("ofi+sockets://127.0.0.1", THALLIUM_SERVER_MODE, true, 1);
    tl::engine client_engine("ofi+sockets://127.0.0.1", THALLIUM_SERVER_MODE, true, 1);
    chl::KeeperRecordingService::CreateKeeperRecordingService(
            keeper_engine,
            kProvider,
            keeper.ingestion_queue,
            keeper.retention_store,
            [&writer](chl::ChronicleName const& chronicle, chl::StoryName const& story, chl::LogEvent const& event)
            { return writer.write(chronicle, story, event); });

    {
        tl::remote_procedure record_event_durable = client_engine.define("record_event_durable");
        tl::provider_handle keeper_handle(client_engine.lookup(keeper_engine.self()), kProvider);
        chl::ChronicleName const chronicle = "chron";
        chl::StoryName const story = "story";
        int const answer = record_event_durable.on(keeper_handle)(chronicle, story, makeEvent(0));
        EXPECT_EQ(answer, chl::CL_ERR_DURABILITY_UNAVAILABLE);
    }
    EXPECT_EQ(keeper.ingested(kStory), 0u);

    client_engine.finalize();
    // deletes the recording service before the writer and the stores it uses
    keeper_engine.finalize();
}

TEST(KeeperDurableWriter, IngestHappensAfterAppendReturns)
{
    ensureLogger();
    Keeper keeper;
    FakeDurableLog wal;
    chl::KeeperDurableWriter writer(&wal, keeper.ingestion_queue, keeper.retention_store, keeper.data_store, kNoCap);

    std::optional<std::size_t> ingested_during_append;
    wal.onAppend = [&]() { ingested_during_append = keeper.ingested(kStory); };

    ASSERT_EQ(writer.write("chron", "story", makeEvent(0)), chl::CL_SUCCESS);
    EXPECT_EQ(wal.calls, (std::vector<std::string>{"lookup", "admit", "append"}));
    ASSERT_TRUE(ingested_during_append.has_value());
    EXPECT_EQ(*ingested_during_append, 0u);
    EXPECT_EQ(keeper.ingested(kStory), 1u);
}
