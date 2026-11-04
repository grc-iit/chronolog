#include <optional>

#include <chronolog_errcode.h>
#include <chrono_monitor.h>
#include <IngestionQueue.h>
#include <KeeperChunkRetentionStore.h>
#include <KeeperDataStore.h>
#include <KeeperDurableWriter.h>
#include <KeeperWal.h>
#include <StoryIdOf.h>

namespace chl = chronolog;

chronolog::KeeperDurableWriter::KeeperDurableWriter(DurableLog* log,
                                                    IngestionQueue& ingestion_queue,
                                                    KeeperChunkRetentionStore& tail_store,
                                                    KeeperDataStore& data_store,
                                                    uint64_t durable_cap_bytes)
    : theLog(log)
    , theIngestionQueue(ingestion_queue)
    , theTailStore(tail_store)
    , theDataStore(data_store)
    , durableCapBytes(durable_cap_bytes)
    , durableResumeBytes(durable_cap_bytes / 4 * 3)
{}

int chronolog::KeeperDurableWriter::write(ChronicleName const& chronicle, StoryName const& story, LogEvent const& event)
{
    if(theLog == nullptr)
    {
        return chl::CL_ERR_DURABILITY_UNAVAILABLE;
    }

    StoryId const story_id = event.getStoryId();

    // The WAL keeps the names and recovery recomputes the id from them, so an
    // event under another id would be acknowledged under one id and recovered
    // under another.
    StoryId const named_id = storyIdOf(chronicle, story);
    if(story_id != named_id)
    {
        LOG_ERROR("[KeeperDurableWriter] StoryId={} refused: Chronicle={}, Story={} have StoryId={}",
                  story_id,
                  chronicle,
                  story,
                  named_id);
        return chl::CL_ERR_INVALID_ARG;
    }

    EventSequence const identity{event.time(), event.getClientId(), event.index()};

    // A retry is answered by its first attempt before anything may refuse it:
    // that attempt was ingested by its own request once its group synced.
    std::optional<int> const earlier = theLog->lookupDurable(story_id, identity);
    if(earlier.has_value())
    {
        LOG_DEBUG("[KeeperDurableWriter] StoryId={} event {}:{}:{} answered from the WAL: {}",
                  story_id,
                  event.time(),
                  event.getClientId(),
                  event.index(),
                  *earlier);
        return *earlier;
    }

    if(event.getRecord().empty())
    {
        return chl::CL_ERR_INVALID_ARG;
    }

    switch(theLog->admit())
    {
        case WalAdmit::Ok:
            break;
        case WalAdmit::Unavailable:
            return chl::CL_ERR_DURABILITY_UNAVAILABLE;
        case WalAdmit::Full:
            return chl::CL_ERR_DURABILITY_FULL;
    }

    // Only chunks the grapher has not confirmed count: a confirmed chunk the
    // tail index still holds is freed without the grapher.
    std::size_t const unconfirmed = theTailStore.unconfirmedByteCount();
    if(unconfirmed > durableCapBytes)
    {
        if(!overCap.exchange(true))
        {
            LOG_WARNING("[KeeperDurableWriter] Refusing durable writes: the retention store holds {} bytes the grapher "
                        "has not confirmed, above durable_cap_mb ({} bytes). They are admitted again at {} bytes",
                        unconfirmed,
                        durableCapBytes,
                        durableResumeBytes);
        }
        return chl::CL_ERR_DURABILITY_FULL;
    }
    if(overCap.load())
    {
        if(unconfirmed > durableResumeBytes)
        {
            LOG_DEBUG("[KeeperDurableWriter] StoryId={} refused: {} unconfirmed bytes, above the resume level of {}",
                      story_id,
                      unconfirmed,
                      durableResumeBytes);
            return chl::CL_ERR_DURABILITY_FULL;
        }
        if(overCap.exchange(false))
        {
            LOG_INFO("[KeeperDurableWriter] Admitting durable writes again: {} unconfirmed bytes, at most {}",
                     unconfirmed,
                     durableResumeBytes);
        }
    }

    // Only a story without an ingestion handle goes to the data store, whose
    // mutex the collector holds for a whole tick; a recorded story's write
    // never waits on it.
    if(!theIngestionQueue.hasStoryIngestionHandle(story_id))
    {
        int const pipeline_status = theDataStore.ensureStoryPipeline(chronicle, story, story_id);
        if(pipeline_status != chl::CL_SUCCESS)
        {
            // nothing was stored, so the request may be sent again
            LOG_ERROR("[KeeperDurableWriter] StoryId={} refused: no pipeline could be started for Chronicle={}, "
                      "Story={} (error {})",
                      story_id,
                      chronicle,
                      story,
                      pipeline_status);
            return chl::CL_ERR_DURABILITY_UNAVAILABLE;
        }
    }

    // The lookup above can miss an attempt with this identity that is still
    // on its way to the WAL (held past the client's deadline before it got
    // there); appendDurable then joins it. That attempt ingests the event, so
    // this one must not: copies in two chunks would be shipped twice.
    bool wrote = false;
    int const status = theLog->appendDurable(chronicle, story, event, wrote);
    if(status != chl::CL_SUCCESS)
    {
        return status;
    }
    if(wrote)
    {
        theIngestionQueue.ingestLogEvent(event);
    }
    return chl::CL_SUCCESS;
}
