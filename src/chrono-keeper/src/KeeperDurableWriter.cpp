#include <optional>

#include <chronolog_errcode.h>
#include <chrono_monitor.h>
#include <IngestionQueue.h>
#include <KeeperChunkRetentionStore.h>
#include <KeeperDataStore.h>
#include <KeeperDurableWriter.h>
#include <KeeperWal.h>

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
{}

int chronolog::KeeperDurableWriter::write(ChronicleName const& chronicle, StoryName const& story, LogEvent const& event)
{
    if(theLog == nullptr)
    {
        return chl::CL_ERR_DURABILITY_UNAVAILABLE;
    }

    StoryId const story_id = event.getStoryId();
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

    std::size_t const retained = theTailStore.retainedByteCount();
    if(retained > durableCapBytes)
    {
        LOG_DEBUG("[KeeperDurableWriter] StoryId={} refused: the retention store holds {} bytes, above the durable "
                  "cap of {}",
                  story_id,
                  retained,
                  durableCapBytes);
        return chl::CL_ERR_DURABILITY_FULL;
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

    int const status = theLog->appendDurable(chronicle, story, event);
    if(status != chl::CL_SUCCESS)
    {
        return status;
    }
    theIngestionQueue.ingestLogEvent(event);
    return chl::CL_SUCCESS;
}
