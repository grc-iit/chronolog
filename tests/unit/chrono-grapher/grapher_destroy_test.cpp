// Destroying a story deletes its archive files, but one of its windows can still
// be mid-write when the destroy runs: an extraction stream took it off the queue
// just before. The write goes to a temporary name and is linked into place only
// when complete, so a delete that runs first misses it and the destroyed story's
// file appears afterwards -- and a story recreated under the same name would
// replay it. The destroy has to wait for every chunk taken off the queue to be
// processed, not only for the queue to be empty.

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <unistd.h>

#include <json-c/json.h>
#include <thallium.hpp>

#include <chrono_monitor.h>
#include <chronolog_errcode.h>
#include <ChunkIngestionQueue.h>
#include <ExtractionModuleConfiguration.h>
#include <GrapherDataStore.h>
#include <GrapherExtractionChain.h>
#include <ServiceId.h>
#include <StoryChunk.h>
#include <StoryChunkExtractionQueue.h>

namespace chl = chronolog;
namespace fs = std::filesystem;

namespace
{
constexpr uint64_t NS = 1000000000ULL;
constexpr chl::StoryId kStory = 5;

void ensureLogger()
{
    static bool done = false;
    if(!done)
    {
        chl::chrono_monitor::initialize("console", "", chl::LogLevel::err, "grapher_destroy_test_logger");
        done = true;
    }
}

chl::StoryChunk* windowOfStory(uint64_t start_secs)
{
    auto* window = new chl::StoryChunk("C", "S", kStory, start_secs * NS, (start_secs + 30) * NS);
    window->insertEvent(chl::LogEvent(kStory, start_secs * NS + 1, 1, 0, "event"));
    return window;
}

std::size_t filesOfStory(fs::path const& dir)
{
    std::size_t count = 0;
    for(auto const& entry: fs::directory_iterator(dir))
    {
        if(entry.path().filename().string().rfind("C.S.", 0) == 0)
        {
            ++count;
        }
    }
    return count;
}
} // namespace

TEST(ExtractionQueue, ATakenChunkKeepsTheQueueBusyUntilItIsProcessed)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue queue;
    EXPECT_TRUE(queue.idle());

    queue.stashStoryChunk(windowOfStory(60));
    EXPECT_FALSE(queue.idle());

    chl::StoryChunk* taken = queue.ejectStoryChunk();
    ASSERT_NE(taken, nullptr);
    // empty, but the chunk is still being processed
    EXPECT_TRUE(queue.empty());
    EXPECT_FALSE(queue.idle());

    delete taken;
    queue.chunkProcessed();
    EXPECT_TRUE(queue.idle());
}

TEST(ExtractionQueue, AnEmptyEjectDoesNotCountAsInProcess)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue queue;
    EXPECT_EQ(queue.ejectStoryChunk(), nullptr);
    EXPECT_TRUE(queue.idle());
}

TEST(GrapherDestroy, AWindowBeingWrittenWhenItsStoryIsDestroyedIsDeletedToo)
{
    ensureLogger();
    thallium::abt argobots; // the data store runs its collection on Argobots streams
    fs::path const dir = fs::temp_directory_path() / ("chronolog_grapher_destroy_test_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);

    chl::ExtractionModuleConfiguration extraction_conf;
    extraction_conf.extractors["hdf5_extractor"] = json_tokener_parse(
            ("{\"type\": \"hdf5_extractor\", \"hdf5_archive_dir\": \"" + dir.string() + "\"}").c_str());
    chl::ChronoGrapherExtractionChain chain;
    ASSERT_EQ(chain.activate(chl::ServiceId(), extraction_conf), chl::CL_SUCCESS);

    chl::StoryChunkExtractionQueue extraction_queue;
    chl::ChunkIngestionQueue ingestion_queue;
    chl::GrapherDataStore data_store(ingestion_queue, extraction_queue, 4096, 30, 60, 300, &chain);
    data_store.startDataCollection(1);

    // an extraction stream takes the story's window off the queue ...
    extraction_queue.stashStoryChunk(windowOfStory(60));
    chl::StoryChunk* taken = extraction_queue.ejectStoryChunk();
    ASSERT_NE(taken, nullptr);

    // ... and the story is destroyed while that window is being written; the
    // destroy worker polls every 50 ms, so this gives it several chances to run
    ASSERT_EQ(data_store.destroyStory(kStory, "C", "S"), chl::CL_SUCCESS);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // the write completes: the window's file appears under its name
    ASSERT_EQ(chain.process_chunk(taken), chl::CL_SUCCESS);
    ASSERT_EQ(filesOfStory(dir), 1u);
    delete taken;
    extraction_queue.chunkProcessed();

    // the destroy deletes it once the write is done
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while(filesOfStory(dir) != 0 && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_EQ(filesOfStory(dir), 0u) << "the destroyed story's window was published after its files were deleted";

    data_store.shutdownDataCollection();
    fs::remove_all(dir);
}
