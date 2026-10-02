// Unit tests for StoryChunkExtractionModule, the drain loop shared by the
// keeper and the grapher. The module owns the extraction queue and the drain
// threads; the chain decides each chunk's fate. The contract under test:
//  - initialization refuses an inactive chain, and extraction does not start
//    before initialization;
//  - every stashed chunk is processed and then disposed exactly once, with the
//    status its processing returned, whether a drain thread or the shutdown
//    drain picks it up;
//  - the module never frees a chunk itself (the keeper's chain hands it to the
//    retention store, the grapher's deletes it);
//  - shutdown drains what is still queued, then flushes the outage buffers.
//
// The chain here only records calls; the keeper chain's real disposal is
// covered by chrono_keeper_extraction_chain_test.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include <abt.h>

#include <chrono_monitor.h>
#include <chronolog_errcode.h>
#include <StoryChunk.h>
#include <StoryChunkExtractionModule.h>

namespace chl = chronolog;

namespace
{
constexpr int kChunks = 200;

void ensureLogger()
{
    static bool done = false;
    if(!done)
    {
        chl::chrono_monitor::initialize("console", "", chl::LogLevel::err, "extraction_module_test_logger");
        done = true;
    }
}

// The drain threads are Argobots ULTs on their own execution streams.
void ensureArgobots()
{
    static bool done = false;
    if(!done)
    {
        ABT_init(0, nullptr);
        done = true;
    }
}

// Odd story ids fail to process and even ones succeed, so each disposal must
// carry its own chunk's status.
int statusFor(chl::StoryId story_id) { return (story_id % 2 == 0) ? chl::CL_SUCCESS : chl::CL_ERR_UNKNOWN; }

chl::StoryChunk* makeChunk(chl::StoryId story_id)
{
    auto* chunk = new chl::StoryChunk("chron", "story", story_id, 100, 200);
    chunk->insertEvent(chl::LogEvent(story_id, 150, 1, 0, "event"));
    return chunk;
}

class RecordingChain
{
public:
    struct Disposal
    {
        chl::StoryId story_id;
        int status;
        int times_processed;
    };

    bool active = true;

    bool is_active_chain() const { return active; }

    int process_chunk(chl::StoryChunk* chunk)
    {
        std::lock_guard<std::mutex> lock(mtx);
        processed[chunk]++;
        return statusFor(chunk->getStoryId());
    }

    // Takes ownership, as the real chains do: the module must not touch the
    // chunk after this.
    void dispose_chunk(chl::StoryChunk* chunk, int status)
    {
        std::lock_guard<std::mutex> lock(mtx);
        disposals.push_back({chunk->getStoryId(), status, processed[chunk]});
        if(++timesDisposed[chunk] == 1)
        {
            owned.emplace_back(chunk);
        }
    }

    void flush_outage_buffers() { flushes++; }

    std::size_t disposedCount()
    {
        std::lock_guard<std::mutex> lock(mtx);
        return disposals.size();
    }

    std::mutex mtx;
    std::map<chl::StoryChunk*, int> processed;
    std::map<chl::StoryChunk*, int> timesDisposed;
    std::vector<Disposal> disposals;
    std::vector<std::unique_ptr<chl::StoryChunk>> owned;
    std::atomic<int> flushes{0};
};

using Module = chl::StoryChunkExtractionModule<RecordingChain>;

void stashChunks(Module& module)
{
    for(int i = 0; i < kChunks; ++i) { module.getExtractionQueue().stashStoryChunk(makeChunk(i)); }
}

// every chunk of story ids [0, kChunks) was processed once and then disposed
// once with its own status
void expectEachDisposedOnce(RecordingChain& chain)
{
    std::lock_guard<std::mutex> lock(chain.mtx);
    ASSERT_EQ(chain.disposals.size(), (std::size_t)kChunks);
    std::set<chl::StoryId> stories;
    for(auto const& disposal: chain.disposals)
    {
        EXPECT_EQ(disposal.status, statusFor(disposal.story_id)) << "story " << disposal.story_id;
        EXPECT_EQ(disposal.times_processed, 1) << "story " << disposal.story_id;
        stories.insert(disposal.story_id);
    }
    EXPECT_EQ(stories.size(), (std::size_t)kChunks);
}

bool waitFor(std::function<bool()> const& condition, std::chrono::milliseconds timeout = std::chrono::seconds(10))
{
    auto const deadline = std::chrono::steady_clock::now() + timeout;
    while(!condition())
    {
        if(std::chrono::steady_clock::now() > deadline)
        {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}
} // namespace

TEST(ExtractionModule, InitializeRefusesAnInactiveChain)
{
    ensureLogger();
    ensureArgobots();
    Module module;
    module.getExtractionChain().active = false;

    EXPECT_EQ(module.initialize(2), chl::CL_ERR_INVALID_CONF);
    EXPECT_FALSE(module.is_initialized());
    module.startExtraction();
    EXPECT_FALSE(module.is_running());
}

TEST(ExtractionModule, ExtractionDoesNotStartBeforeInitialize)
{
    ensureLogger();
    ensureArgobots();
    Module module;

    module.startExtraction();
    EXPECT_FALSE(module.is_running());
}

TEST(ExtractionModule, DrainThreadsDisposeEveryChunkOnceWithItsStatus)
{
    ensureLogger();
    ensureArgobots();
    Module module;
    ASSERT_EQ(module.initialize(2), chl::CL_SUCCESS);
    module.startExtraction();
    ASSERT_TRUE(module.is_running());

    stashChunks(module);
    ASSERT_TRUE(waitFor([&] { return module.getExtractionChain().disposedCount() == (std::size_t)kChunks; }));
    module.shutdownExtraction();

    expectEachDisposedOnce(module.getExtractionChain());
}

TEST(ExtractionModule, ShutdownDrainsWhatIsQueuedThenFlushes)
{
    ensureLogger();
    ensureArgobots();
    Module module;
    ASSERT_EQ(module.initialize(2), chl::CL_SUCCESS);
    // never started: the shutdown drain is the only consumer
    stashChunks(module);

    module.shutdownExtraction();

    expectEachDisposedOnce(module.getExtractionChain());
    EXPECT_EQ(module.getExtractionChain().flushes, 1);
}

TEST(ExtractionModule, ShutdownRacingDrainThreadsDisposesEveryChunkOnce)
{
    ensureLogger();
    ensureArgobots();
    Module module;
    ASSERT_EQ(module.initialize(2), chl::CL_SUCCESS);
    module.startExtraction();

    // shut down with the queue full: the shutdown drain and the running drain
    // threads race for the same chunks
    stashChunks(module);
    module.shutdownExtraction();

    expectEachDisposedOnce(module.getExtractionChain());
    EXPECT_EQ(module.getExtractionChain().flushes, 1);
}
