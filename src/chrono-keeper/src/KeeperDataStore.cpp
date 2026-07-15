#include <unistd.h>
#include <map>
#include <mutex>
#include <chrono>
#include <string>
#include <cstdint>
#include <memory>
#include <deque>
#include <unordered_set>
#include <thallium.hpp>

#include <chronolog_errcode.h>
#include <KeeperDataStore.h>
#include <chrono_monitor.h>

namespace chl = chronolog;
namespace tl = thallium;

///////////////////////
class ClocksourceCPPStyle
{
public:
    uint64_t getTimestamp() { return std::chrono::steady_clock::now().time_since_epoch().count(); }
};

////////////////////////

int chronolog::KeeperDataStore::startStoryRecording(std::string const& chronicle,
                                                    std::string const& story,
                                                    chronolog::StoryId const& story_id,
                                                    uint64_t start_time,
                                                    uint32_t time_chunk_duration,
                                                    uint32_t access_window)
{
    LOG_INFO("[KeeperDataStore] Start recording story: Chronicle={}, Story={}, StoryID={}", chronicle, story, story_id);

    // Get dataStoreMutex, check for story_id_presense & add new KeeperStoryPipeline if needed
    std::lock_guard storeLock(dataStoreMutex);
    auto pipeline_iter = theMapOfStoryPipelines.find(story_id);
    if(pipeline_iter != theMapOfStoryPipelines.end())
    {
        LOG_INFO("[KeeperDataStore] Story already being recorded. StoryID: {}", story_id);
        //check it the pipeline was put on the waitingForExit list by the previous acquisition
        // and remove it from there
        auto waiting_iter = pipelinesWaitingForExit.find(story_id);
        if(waiting_iter != pipelinesWaitingForExit.end())
        {
            pipelinesWaitingForExit.erase(waiting_iter);
        }

        return chronolog::CL_SUCCESS;
    }

    auto result = theMapOfStoryPipelines.emplace(
            std::pair<chl::StoryId, chl::KeeperStoryPipeline*>(story_id,
                                                               new chl::KeeperStoryPipeline(theExtractionQueue,
                                                                                            theTailStore,
                                                                                            chronicle,
                                                                                            story,
                                                                                            story_id,
                                                                                            start_time,
                                                                                            story_chunk_duration_secs,
                                                                                            acceptance_window_secs)));

    if(result.second)
    {
        LOG_INFO("[KeeperDataStore] New KeeperStoryPipeline created successfully. StoryID: {}", story_id);
        pipeline_iter = result.first;
        //engage KeeperStoryPipeline with the IngestionQueue
        StoryIngestionHandle* ingestionHandle = (*pipeline_iter).second->getActiveIngestionHandle();
        theIngestionQueue.addStoryIngestionHandle(story_id, ingestionHandle);
        return chronolog::CL_SUCCESS;
    }
    else
    {
        LOG_ERROR("[KeeperDataStore] Failed to create KeeperStoryPipeline for StoryID: {}. Possible memory or resource "
                  "issue.",
                  story_id);
        return chronolog::CL_ERR_UNKNOWN;
    }
}
////////////////////////

int chronolog::KeeperDataStore::stopStoryRecording(chronolog::StoryId const& story_id)
{
    LOG_DEBUG("[KeeperDataStore] Initiating stop recording for StoryID={}", story_id);
    // we do not yet disengage the KeeperStoryPipeline from the IngestionQueue right away
    // but put it on the WaitingForExit list to be finalized, persisted to disk , and
    // removed from memory at exit_time = now+acceptance_window...
    // unless there's a new story acquisition request comes before that moment
    std::lock_guard storeLock(dataStoreMutex);
    auto pipeline_iter = theMapOfStoryPipelines.find(story_id);
    if(pipeline_iter != theMapOfStoryPipelines.end())
    {
        uint64_t exit_time = std::chrono::high_resolution_clock::now().time_since_epoch().count() +
                             (*pipeline_iter).second->getAcceptanceWindow();
        pipelinesWaitingForExit[(*pipeline_iter).first] =
                (std::pair<chl::KeeperStoryPipeline*, uint64_t>((*pipeline_iter).second, exit_time));
        LOG_INFO(
                "[KeeperDataStore] Added KeeperStoryPipeline to waiting list for finalization. StoryID={}, ExitTime={}",
                story_id,
                exit_time);
    }
    else
    {
        LOG_WARNING("[KeeperDataStore] Attempted to stop recording for non-existent StoryID={}", story_id);
    }
    return chronolog::CL_SUCCESS;
}

////////////////////////

void chronolog::KeeperDataStore::collectIngestedEvents()
{
    LOG_DEBUG("[KeeperDataStore] Initiating collection of ingested events. Current state={}, Active "
              "KeeperStoryPipelines={}, "
              "PipelinesWaitingForExit={}, ThreadID={}",
              state,
              theMapOfStoryPipelines.size(),
              pipelinesWaitingForExit.size(),
              tl::thread::self_id());
    theIngestionQueue.drainOrphanEvents();

    std::lock_guard storeLock(dataStoreMutex);
    for(auto pipeline_iter = theMapOfStoryPipelines.begin(); pipeline_iter != theMapOfStoryPipelines.end();
        ++pipeline_iter)
    {
        //INNA: this can be delegated to different threads handling individual storylines...
        (*pipeline_iter).second->collectIngestedEvents();
    }
}

////////////////////////
void chronolog::KeeperDataStore::extractDecayedStoryChunks()
{
    LOG_DEBUG("[KeeperDataStore] Initiating extraction of decayed story chunks. Current state={}, Active "
              "KeeperStoryPipelines={}, PipelinesWaitingForExit={}, ThreadID={}",
              state,
              theMapOfStoryPipelines.size(),
              pipelinesWaitingForExit.size(),
              tl::thread::self_id());

    uint64_t current_time = std::chrono::high_resolution_clock::now().time_since_epoch().count();

    std::lock_guard storeLock(dataStoreMutex);
    for(auto pipeline_iter = theMapOfStoryPipelines.begin(); pipeline_iter != theMapOfStoryPipelines.end();
        ++pipeline_iter)
    {
        (*pipeline_iter).second->extractDecayedStoryChunks(current_time);
    }
}
////////////////////////

void chronolog::KeeperDataStore::retireDecayedPipelines()
{
    LOG_DEBUG("[KeeperDataStore] Initiating retirement of decayed pipelines. Current state={}, Active "
              "KeeperStoryPipelines={}, PipelinesWaitingForExit={}, ThreadID={}",
              state,
              theMapOfStoryPipelines.size(),
              pipelinesWaitingForExit.size(),
              tl::thread::self_id());

    if(!theMapOfStoryPipelines.empty())
    {
        std::lock_guard storeLock(dataStoreMutex);

        uint64_t current_time = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        for(auto pipeline_iter = pipelinesWaitingForExit.begin(); pipeline_iter != pipelinesWaitingForExit.end();)
        {
            if(current_time >= (*pipeline_iter).second.second)
            {
                //current_time >= pipeline exit_time
                KeeperStoryPipeline* pipeline = (*pipeline_iter).second.first;
                // remember the story's names before the pipeline is gone so that
                // any late orphaned events (which carry only a storyId) can still
                // be sealed with the correct chronicle/story identity.
                retiredStoryNames[pipeline->getStoryId()] =
                        std::make_pair(pipeline->getChronicleName(), pipeline->getStoryName());
                theMapOfStoryPipelines.erase(pipeline->getStoryId());
                theIngestionQueue.removeIngestionHandle(pipeline->getStoryId());
                pipeline_iter = pipelinesWaitingForExit.erase(pipeline_iter); //pipeline->getStoryId());
                delete pipeline;
            }
            else
            {
                pipeline_iter++;
            }
        }
    }

    // Rescue any events stranded in the ingestion orphan queue for stories that
    // were just retired (or retired earlier) and seal them for archival before
    // they are lost. Invoked here so a direct retireDecayedPipelines() call --
    // including the per-tick one -- always performs the rescue.
    sealOrphanedEvents();

    //swipe through pipelineswaiting and remove all those with nullptr
    LOG_DEBUG("[KeeperDataStore] Completed retirement of decayed pipelines. Current state={}, Active "
              "KeeperStoryPipelines={}, PipelinesWaitingForExit={}, ThreadID={}",
              state,
              theMapOfStoryPipelines.size(),
              pipelinesWaitingForExit.size(),
              tl::thread::self_id());
}

void chronolog::KeeperDataStore::sealOrphanedEvents()
{
    // Late events that arrived after their story's pipeline was retired can no
    // longer be re-homed into a live ingestion handle (drainOrphanEvents only
    // re-homes to existing handles). Rather than let them languish in the
    // orphan queue and be dropped at shutdown, seal them into a recovery
    // StoryChunk -- using the story's retained names -- and hand it to the
    // extraction queue for archival.
    std::unordered_set<chl::StoryId> orphan_story_ids = theIngestionQueue.orphanStoryIds();
    if(orphan_story_ids.empty())
    { return; }

    std::lock_guard storeLock(dataStoreMutex);
    for(chl::StoryId const& story_id: orphan_story_ids)
    {
        // Active stories: the tick's drainOrphanEvents re-homes their events
        // into the live handle, so leave those alone.
        if(theMapOfStoryPipelines.find(story_id) != theMapOfStoryPipelines.end())
        { continue; }
        // Stories never acquired on this keeper: keep buffering (an acquisition
        // may still be in flight) and we have no names to seal them with anyway.
        auto name_iter = retiredStoryNames.find(story_id);
        if(name_iter == retiredStoryNames.end())
        { continue; }

        std::deque<chl::LogEvent> orphans = theIngestionQueue.extractOrphansForStory(story_id);
        if(orphans.empty())
        { continue; }

        uint64_t min_time = orphans.front().time();
        uint64_t max_time = orphans.front().time();
        for(auto const& orphan_event: orphans)
        {
            min_time = std::min(min_time, orphan_event.time());
            max_time = std::max(max_time, orphan_event.time());
        }
        // StoryChunk end_time is exclusive; +1 so the latest event fits.
        chl::StoryChunk* recovery_chunk = new chl::StoryChunk(name_iter->second.first,
                                                              name_iter->second.second,
                                                              story_id,
                                                              min_time,
                                                              max_time + 1,
                                                              orphans.size());
        for(auto const& orphan_event: orphans)
        { recovery_chunk->insertEvent(orphan_event); }

        if(recovery_chunk->empty())
        {
            delete recovery_chunk;
        }
        else
        {
            LOG_WARNING("[KeeperDataStore] Sealed {} late orphaned event(s) for retired story {} into a recovery "
                        "chunk for archival.",
                        recovery_chunk->getEventCount(),
                        story_id);
            // through the retention store (which stashes it to the extraction
            // queue itself) so the recovery data gets the same durability
            // gating as a regular sealed chunk
            theTailStore.ingestSealedChunk(story_id, recovery_chunk);
        }
    }
}

////////////////////////

void chronolog::KeeperDataStore::dataCollectionTask()
{
    //run dataCollectionTask as long as the state == RUNNING
    // or there're still events left to collect and
    // storyPipelines left to retire...
    tl::xstream es = tl::xstream::self();
    LOG_DEBUG("[KeeperDataStore] Initiating DataCollectionTask. ESrank={}, ThreadID={}",
              es.get_rank(),
              tl::thread::self_id());

    while(!is_shutting_down() || !theIngestionQueue.is_empty() || !theMapOfStoryPipelines.empty())
    {
        LOG_DEBUG("[KeeperDataStore] Running DataCollection iteration. ESrank={}, ThreadID={}",
                  es.get_rank(),
                  tl::thread::self_id());
        for(int i = 0; i < 1; ++i)
        {
            collectIngestedEvents();
            sleep(1);
        }
        extractDecayedStoryChunks();
        retireDecayedPipelines();
    }
    LOG_DEBUG("[KeeperDataStore] Exiting DataCollectionTask thread {}", tl::thread::self_id());
}

////////////////////////
void chronolog::KeeperDataStore::startDataCollection(int stream_count)
{
    std::lock_guard storeLock(dataStoreStateMutex);
    if(is_running() || is_shutting_down())
    {
        LOG_INFO("[KeeperDataStore] Data collection is already running or shutting down. Ignoring request.");
        return;
    }

    LOG_INFO("[KeeperDataStore] Starting data collection. StreamCount={}, ThreadID={}",
             stream_count,
             tl::thread::self_id());
    state = RUNNING;

    for(int i = 0; i < stream_count; ++i)
    {
        tl::managed<tl::xstream> es = tl::xstream::create();
        dataStoreStreams.push_back(std::move(es));
    }

    for(int i = 0; i < 2 * stream_count; ++i)
    {
        tl::managed<tl::thread> th =
                dataStoreStreams[i % (dataStoreStreams.size())]->make_thread([p = this]() { p->dataCollectionTask(); });
        dataStoreThreads.push_back(std::move(th));
    }
    LOG_INFO("[KeeperDataStore] Data collection started successfully. Stream count={}, ThreadID={}",
             stream_count,
             tl::thread::self_id());
}
//////////////////////////////

void chronolog::KeeperDataStore::shutdownDataCollection()
{
    LOG_INFO(
            "[KeeperDataStore] Initiating shutdown of DataCollection. CurrentState={}, Active KeeperStoryPipelines={}, "
            "PipelinesWaitingForExit={}",
            state,
            theMapOfStoryPipelines.size(),
            pipelinesWaitingForExit.size());

    // switch the state to shuttingDown
    std::lock_guard storeLock(dataStoreStateMutex);
    if(is_shutting_down())
    {
        LOG_INFO("[KeeperDataStore] Data collection is already shutting down. Ignoring additional shutdown request.");
        return;
    }
    state = SHUTTING_DOWN;

    if(!theMapOfStoryPipelines.empty())
    {
        // label all existing Pipelines as waiting to exit
        std::lock_guard storeLock(dataStoreMutex);
        uint64_t current_time = std::chrono::high_resolution_clock::now().time_since_epoch().count();

        for(auto pipeline_iter = theMapOfStoryPipelines.begin(); pipeline_iter != theMapOfStoryPipelines.end();
            ++pipeline_iter)
        {
            if(pipelinesWaitingForExit.find((*pipeline_iter).first) == pipelinesWaitingForExit.end())
            {
                uint64_t exit_time = current_time + (*pipeline_iter).second->getAcceptanceWindow();
                pipelinesWaitingForExit[(*pipeline_iter).first] =
                        (std::pair<chl::KeeperStoryPipeline*, uint64_t>((*pipeline_iter).second, exit_time));
            }
        }
    }

    // Join threads & execution streams while holding stateMutex
    // and just wait until all the events are collected and
    // all the storyPipelines decay and retire
    for(auto& th: dataStoreThreads) { th->join(); }
    LOG_INFO("[KeeperDataStore] All data collection threads have been joined.");

    for(auto& es: dataStoreStreams) { es->join(); }
    LOG_INFO("[KeeperDataStore] All data collection streams have been joined.");
    LOG_INFO("[KeeperDataStore] DataCollection shutdown completed.");
}

///////////////////////

//
chronolog::KeeperDataStore::~KeeperDataStore()
{
    LOG_INFO("[KeeperDataStore] Destructor called. Initiating shutdown. Active KeeperStoryPipelines count={}",
             theMapOfStoryPipelines.size());
    shutdownDataCollection();
    LOG_INFO("[KeeperDataStore] Shutdown completed successfully. Active KeeperStoryPipelines count={}",
             theMapOfStoryPipelines.size());
}
