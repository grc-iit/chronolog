#include <StoryChunkExtractionModule.h>
#include <chrono_monitor.h>
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>
using namespace std::chrono_literals;
namespace
{
struct BlockedChain
{
    std::atomic<bool> entered{false}, release{false};
    std::atomic<unsigned> processed{0}, disposed{0}, flushed{0};
    bool is_active_chain() const { return true; }
    int process_chunk(chronolog::StoryChunk*)
    {
        ++processed;
        entered = true;
        while(!release.load(std::memory_order_acquire)) std::this_thread::sleep_for(1ms);
        return chronolog::CL_SUCCESS;
    }
    void dispose_chunk(chronolog::StoryChunk* chunk, int)
    {
        ++disposed;
        delete chunk;
    }
    void flush_outage_buffers() { ++flushed; }
};
template <class Predicate>
bool eventually(Predicate predicate)
{
    auto end = std::chrono::steady_clock::now() + 2s;
    while(!predicate() && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(1ms);
    return predicate();
}
TEST(ExtractionLifecycle, RepeatedShutdownWaitsForTheSameBlockedExtractorAndJoinsOnce)
{
    chronolog::chrono_monitor::initialize("console", "", chronolog::LogLevel::critical, "extraction_lifecycle");
    thallium::abt native;
    chronolog::StoryChunkExtractionModule<BlockedChain> module(1);
    auto& chain = module.getExtractionChain();
    struct Release
    {
        BlockedChain& chain;
        ~Release() { chain.release = true; }
    } release{chain};
    ASSERT_EQ(chronolog::CL_SUCCESS, module.initialize(1));
    module.startExtraction();
    std::vector<chronolog::StoryChunk*> chunks{new chronolog::StoryChunk("C", "S", 1, 0, 1)};
    module.getExtractionQueue().stashStoryChunks(chunks);
    ASSERT_TRUE(eventually([&] { return chain.entered.load(); }));
    std::atomic<bool> firstDone{false}, secondDone{false};
    std::thread first(
            [&]
            {
                module.shutdownExtraction();
                firstDone = true;
            });
    EXPECT_TRUE(eventually([&] { return module.is_shutting_down(); }));
    std::thread second(
            [&]
            {
                module.shutdownExtraction();
                secondDone = true;
            });
    std::this_thread::sleep_for(40ms);
    EXPECT_FALSE(firstDone.load());
    EXPECT_FALSE(secondDone.load());
    chain.release = true;
    first.join();
    second.join();
    module.shutdownExtraction();
    EXPECT_EQ(1u, chain.processed.load());
    EXPECT_EQ(1u, chain.disposed.load());
    EXPECT_EQ(1u, chain.flushed.load());
}
} // namespace
