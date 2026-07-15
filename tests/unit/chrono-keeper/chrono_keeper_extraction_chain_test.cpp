// Unit tests for the ChronoKeeperExtractionChain disposal seam: after the
// extraction module processes a chunk, dispose_chunk routes the transfer
// outcome into the KeeperChunkRetentionStore instead of deleting the chunk.
// In shim mode (expects_watermarks() == false — no grapher-bound extractor in
// the chain, or Task 2's transitional default) a successful send also counts
// as persisted, reproducing today's free-on-ack behavior so every commit
// stays deployable.

#include <gtest/gtest.h>

#include <chrono_monitor.h>
#include <StoryChunk.h>

#include <KeeperChunkRetentionStore.h>
#include <KeeperExtractionChain.h>
#include <StoryChunkExtractionQueue.h>
#include <chronolog_errcode.h>

namespace chl = chronolog;

static void ensureLogger()
{
    static bool done = false;
    if(!done)
    {
        chl::chrono_monitor::initialize("console", "", chronolog::LogLevel::err, "extraction_chain_test_logger");
        done = true;
    }
}

static chl::StoryChunk* makeChunk(chl::StoryId sid, uint64_t start, uint64_t end, int count)
{
    auto* chunk = new chl::StoryChunk("chron", "story", sid, start, end, 64);
    for(int i = 0; i < count; i++)
    {
        chl::LogEvent ev(sid, start + (uint64_t)i, 1, (chl::chrono_index)i, "e" + std::to_string(i));
        chunk->insertEvent(ev);
    }
    return chunk;
}

TEST(KeeperExtractionChain, DisposeWithoutStoreDeletesChunk)
{
    ensureLogger();
    chl::ChronoKeeperExtractionChain chain;
    // no retention store attached (defensive path): dispose must free the
    // chunk rather than leak it — ASan/valgrind verifies.
    chain.dispose_chunk(makeChunk(7, 100, 200, 3), chl::CL_SUCCESS);
    chain.dispose_chunk(makeChunk(7, 200, 300, 3), chl::CL_ERR_UNKNOWN);
    SUCCEED();
}

TEST(KeeperExtractionChain, ShimModeFreesOnAckOnceTailReleased)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperChunkRetentionStore store(q, 0); // capacity 0: tail releases at ingest
    chl::ChronoKeeperExtractionChain chain;
    chain.attachRetentionStore(&store);
    ASSERT_FALSE(chain.expects_watermarks()); // Task 2 shim: no watermark loop yet

    chl::StoryId sid = 7;
    store.ingestSealedChunk(sid, makeChunk(sid, 100, 200, 5));
    chl::StoryChunk* chunk = q.ejectStoryChunk();
    ASSERT_NE(chunk, nullptr);

    // drain success: shim counts the ack as persisted -> chunk freed
    chain.dispose_chunk(chunk, chl::CL_SUCCESS);
    EXPECT_EQ(store.retainedChunkCount(sid), 0u);
}

TEST(KeeperExtractionChain, ShimModeStillWaitsForTailRelease)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperChunkRetentionStore store(q, 100); // chunk stays tail-indexed
    chl::ChronoKeeperExtractionChain chain;
    chain.attachRetentionStore(&store);

    chl::StoryId sid = 7;
    store.ingestSealedChunk(sid, makeChunk(sid, 100, 200, 5));
    chl::StoryChunk* chunk = q.ejectStoryChunk();
    ASSERT_NE(chunk, nullptr);

    chain.dispose_chunk(chunk, chl::CL_SUCCESS);
    // acked and (shim-)persisted, but the tail still references it: the chunk
    // must stay readable — freeing it under the tail would break playback.
    EXPECT_EQ(store.retainedChunkCount(sid), 1u);
    auto evs = store.getTailEvents(sid, store.getTailSequences(sid, 5));
    EXPECT_EQ(evs.size(), 5u);
}

TEST(KeeperExtractionChain, FailedDrainRetainsChunkForResend)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperChunkRetentionStore store(q, 0);
    chl::ChronoKeeperExtractionChain chain;
    chain.attachRetentionStore(&store);

    chl::StoryId sid = 7;
    store.ingestSealedChunk(sid, makeChunk(sid, 100, 200, 5));
    chl::StoryChunk* chunk = q.ejectStoryChunk();
    ASSERT_NE(chunk, nullptr);

    // drain failure: today this deleted the chunk (data loss); now it stays
    // retained and the stall timer re-sends it.
    chain.dispose_chunk(chunk, chl::CL_ERR_UNKNOWN);
    EXPECT_EQ(store.retainedChunkCount(sid), 1u);
    EXPECT_EQ(store.requeueStalled(std::chrono::seconds(0)), 1u);
    EXPECT_EQ(q.ejectStoryChunk(), chunk);
}
