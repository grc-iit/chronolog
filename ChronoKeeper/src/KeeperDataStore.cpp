#include <unistd.h>
#include <map>
#include <mutex>
#include <chrono>
#include <string>
#include <cstdint>
#include <memory>
#include <thallium.hpp>

#include <chronolog_errcode.h>
#include <KeeperDataStore.h>
#include <chrono_monitor.h>
#include <chronolog_profile.h>

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
    CL_PROFILE_REGION("keeper_ingest");
    CL_PROFILE_REGION("metadata_lookup");

    LOG_INFO("[KeeperDataStore] Start recording story: Chronicle={}, Story={}, StoryID={}", chronicle, story, story_id);

    // Get dataStoreMutex, check for story_id_presense & add new KeeperStoryPipeline if needed
    {
        CL_PROFILE_REGION("keeper_datastore_lock_wait");
        dataStoreMutex.lock();
    }
    std::lock_guard<std::mutex> storeLock(dataStoreMutex, std::adopt_lock);
    CL_PROFILE_REGION("keeper_datastore_lock_hold");
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
    {
        CL_PROFILE_REGION("keeper_datastore_lock_wait");
        dataStoreMutex.lock();
    }
    std::lock_guard<std::mutex> storeLock(dataStoreMutex, std::adopt_lock);
    CL_PROFILE_REGION("keeper_datastore_lock_hold");
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

int chronolog::KeeperDataStore::flushStoryRecording(chronolog::StoryId const& story_id,
                                                    uint64_t extraction_drain_timeout_ms,
                                                    bool async_drain_complete)
{
    auto const flush_start = std::chrono::high_resolution_clock::now();
    LOG_INFO("[KeeperDataStore] Flush story recording requested. StoryID={} extractionDrainTimeoutMs={} "
             "asyncDrainComplete={}",
             story_id,
             extraction_drain_timeout_ms,
             async_drain_complete ? 1 : 0);

    KeeperStoryPipeline* pipeline = nullptr;
    {
        CL_PROFILE_REGION("keeper_flush_story_lock_wait");
        dataStoreMutex.lock();
    }
    {
        std::lock_guard<std::mutex> storeLock(dataStoreMutex, std::adopt_lock);
        CL_PROFILE_REGION("keeper_flush_story_lock_hold");
        auto pipeline_iter = theMapOfStoryPipelines.find(story_id);
        if(pipeline_iter == theMapOfStoryPipelines.end())
        {
            LOG_WARNING("[KeeperDataStore] Flush requested for non-existent StoryID={}", story_id);
            return chronolog::CL_ERR_UNKNOWN;
        }

        pipeline = pipeline_iter->second;
        theMapOfStoryPipelines.erase(pipeline_iter);
        pipelinesWaitingForExit.erase(story_id);
        theIngestionQueue.removeIngestionHandle(story_id);
    }

    auto const finalize_start = std::chrono::high_resolution_clock::now();
    delete pipeline;
    auto const finalize_end = std::chrono::high_resolution_clock::now();

    if(async_drain_complete)
    {
        auto const finalize_us =
                std::chrono::duration_cast<std::chrono::nanoseconds>(finalize_end - finalize_start).count() / 1000.0;
        std::thread([this, story_id, extraction_drain_timeout_ms]() {
            auto const async_start = std::chrono::high_resolution_clock::now();
            bool const drained = theExtractionQueue.waitForStoryDrain(story_id, extraction_drain_timeout_ms);
            auto const drain_end = std::chrono::high_resolution_clock::now();
            int completion_notify_return = chronolog::CL_SUCCESS;
            int64_t completion_notify_us = 0;
            if(drained && storyDrainCompleteCallback)
            {
                auto const notify_start = std::chrono::high_resolution_clock::now();
                completion_notify_return = storyDrainCompleteCallback(story_id);
                auto const notify_end = std::chrono::high_resolution_clock::now();
                completion_notify_us =
                        std::chrono::duration_cast<std::chrono::microseconds>(notify_end - notify_start).count();
            }
            LOG_INFO("[KeeperDataStore] Async flush story drain completed. StoryID={} drained={} queuedChunks={} "
                     "inFlightChunks={} completion_notify_return={} completion_notify_us={} drain_wait_us={} "
                     "total_us={}",
                     story_id,
                     drained,
                     theExtractionQueue.queuedStoryChunkCount(story_id),
                     theExtractionQueue.inFlightStoryChunkCount(story_id),
                     completion_notify_return,
                     completion_notify_us,
                     std::chrono::duration_cast<std::chrono::nanoseconds>(drain_end - async_start).count() / 1000.0,
                     std::chrono::duration_cast<std::chrono::nanoseconds>(drain_end - async_start).count() / 1000.0);
        }).detach();

        auto const flush_end = std::chrono::high_resolution_clock::now();
        LOG_INFO("[KeeperDataStore] Flush story recording returned with async drain pending. StoryID={} queuedChunks={} "
                 "inFlightChunks={} finalize_us={} total_us={}",
                 story_id,
                 theExtractionQueue.queuedStoryChunkCount(story_id),
                 theExtractionQueue.inFlightStoryChunkCount(story_id),
                 finalize_us,
                 std::chrono::duration_cast<std::chrono::nanoseconds>(flush_end - flush_start).count() / 1000.0);
        return chronolog::CL_SUCCESS;
    }

    auto const drain_start = std::chrono::high_resolution_clock::now();
    bool const drained = theExtractionQueue.waitForStoryDrain(story_id, extraction_drain_timeout_ms);
    auto const drain_end = std::chrono::high_resolution_clock::now();
    int completion_notify_return = chronolog::CL_SUCCESS;
    int64_t completion_notify_us = 0;
    if(drained && storyDrainCompleteCallback)
    {
        auto const notify_start = std::chrono::high_resolution_clock::now();
        completion_notify_return = storyDrainCompleteCallback(story_id);
        auto const notify_end = std::chrono::high_resolution_clock::now();
        completion_notify_us =
                std::chrono::duration_cast<std::chrono::microseconds>(notify_end - notify_start).count();
    }
    LOG_INFO("[KeeperDataStore] Flush story recording completed. StoryID={} drained={} queuedChunks={} "
             "inFlightChunks={} completion_notify_return={} completion_notify_us={} finalize_us={} drain_wait_us={} "
             "total_us={}",
             story_id,
             drained,
             theExtractionQueue.queuedStoryChunkCount(story_id),
             theExtractionQueue.inFlightStoryChunkCount(story_id),
             completion_notify_return,
             completion_notify_us,
             std::chrono::duration_cast<std::chrono::nanoseconds>(finalize_end - finalize_start).count() / 1000.0,
             std::chrono::duration_cast<std::chrono::nanoseconds>(drain_end - drain_start).count() / 1000.0,
             std::chrono::duration_cast<std::chrono::nanoseconds>(drain_end - flush_start).count() / 1000.0);
    return (drained && completion_notify_return == chronolog::CL_SUCCESS) ? chronolog::CL_SUCCESS
                                                                          : chronolog::CL_ERR_UNKNOWN;
}

int chronolog::KeeperDataStore::collectTailEvents(chronolog::StoryId const& story_id,
                                                  chronolog::chrono_time start_time,
                                                  chronolog::chrono_time end_time,
                                                  std::vector<chronolog::LogEvent>& events)
{
    CL_PROFILE_REGION("keeper_tail_read");
    collectIngestedEvents();

    {
        CL_PROFILE_REGION("keeper_datastore_lock_wait");
        dataStoreMutex.lock();
    }
    std::lock_guard<std::mutex> storeLock(dataStoreMutex, std::adopt_lock);
    CL_PROFILE_REGION("keeper_datastore_lock_hold");

    auto pipeline_iter = theMapOfStoryPipelines.find(story_id);
    if(pipeline_iter == theMapOfStoryPipelines.end())
    {
        return chronolog::CL_ERR_UNKNOWN;
    }

    (*pipeline_iter).second->collectEvents(start_time, end_time, events);
    return chronolog::CL_SUCCESS;
}

////////////////////////

void chronolog::KeeperDataStore::collectIngestedEvents()
{
    CL_PROFILE_REGION("keeper_ingest");

    LOG_DEBUG("[KeeperDataStore] Initiating collection of ingested events. Current state={}, Active "
              "KeeperStoryPipelines={}, "
              "PipelinesWaitingForExit={}, ThreadID={}",
              state,
              theMapOfStoryPipelines.size(),
              pipelinesWaitingForExit.size(),
              tl::thread::self_id());
    theIngestionQueue.drainOrphanEvents();

    {
        CL_PROFILE_REGION("keeper_datastore_lock_wait");
        dataStoreMutex.lock();
    }
    std::lock_guard<std::mutex> storeLock(dataStoreMutex, std::adopt_lock);
    CL_PROFILE_REGION("keeper_datastore_lock_hold");
    CL_PROFILE_COUNTER("keeper_active_pipeline_count", theMapOfStoryPipelines.size());
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
    CL_PROFILE_REGION("keeper_flush");

    LOG_DEBUG("[KeeperDataStore] Initiating extraction of decayed story chunks. Current state={}, Active "
              "KeeperStoryPipelines={}, PipelinesWaitingForExit={}, ThreadID={}",
              state,
              theMapOfStoryPipelines.size(),
              pipelinesWaitingForExit.size(),
              tl::thread::self_id());

    uint64_t current_time = std::chrono::high_resolution_clock::now().time_since_epoch().count();

    {
        CL_PROFILE_REGION("keeper_datastore_lock_wait");
        dataStoreMutex.lock();
    }
    std::lock_guard<std::mutex> storeLock(dataStoreMutex, std::adopt_lock);
    CL_PROFILE_REGION("keeper_datastore_lock_hold");
    CL_PROFILE_COUNTER("keeper_active_pipeline_count", theMapOfStoryPipelines.size());
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
    //swipe through pipelineswaiting and remove all those with nullptr
    LOG_DEBUG("[KeeperDataStore] Completed retirement of decayed pipelines. Current state={}, Active "
              "KeeperStoryPipelines={}, PipelinesWaitingForExit={}, ThreadID={}",
              state,
              theMapOfStoryPipelines.size(),
              pipelinesWaitingForExit.size(),
              tl::thread::self_id());
}

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
            usleep(data_collection_poll_interval_us);
        }
        extractDecayedStoryChunks();
        retireDecayedPipelines();
    }
    LOG_DEBUG("[KeeperDataStore] Exiting DataCollectionTask thread {}", tl::thread::self_id());
}

////////////////////////
void chronolog::KeeperDataStore::startDataCollection(int stream_count, int threads_per_stream)
{
    std::lock_guard storeLock(dataStoreStateMutex);
    if(is_running() || is_shutting_down())
    {
        LOG_INFO("[KeeperDataStore] Data collection is already running or shutting down. Ignoring request.");
        return;
    }

    if(stream_count <= 0)
    {
        stream_count = 1;
    }
    if(threads_per_stream <= 0)
    {
        threads_per_stream = 1;
    }

    LOG_INFO("[KeeperDataStore] Starting data collection. StreamCount={} ThreadsPerStream={} ThreadID={}",
             stream_count,
             threads_per_stream,
             tl::thread::self_id());
    state = RUNNING;

    for(int i = 0; i < stream_count; ++i)
    {
        tl::managed<tl::xstream> es = tl::xstream::create();
        dataStoreStreams.push_back(std::move(es));
    }

    for(int i = 0; i < threads_per_stream * stream_count; ++i)
    {
        tl::managed<tl::thread> th =
                dataStoreStreams[i % (dataStoreStreams.size())]->make_thread([p = this]() { p->dataCollectionTask(); });
        dataStoreThreads.push_back(std::move(th));
    }
    LOG_INFO("[KeeperDataStore] Data collection started successfully. StreamCount={} ThreadsPerStream={} ThreadID={}",
             stream_count,
             threads_per_stream,
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
