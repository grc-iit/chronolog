#include <unistd.h>
#include <map>
#include <mutex>
#include <chrono>
#include <unistd.h>

#include <thallium.hpp>

#include "chronolog_errcode.h"
#include "GrapherDataStore.h"
#include "chrono_monitor.h"

namespace chl = chronolog;
namespace tl = thallium;


////////////////////////

int chronolog::GrapherDataStore::startStoryRecording(std::string const &chronicle, std::string const &story
                                                    , chronolog::StoryId const &story_id, uint64_t start_time
                                                    , uint32_t time_chunk_duration, uint32_t access_window)
{
    LOG_INFO("[GrapherDataStore] Start recording story: Chronicle={}, Story={}, StoryId={}"
             , chronicle, story, story_id);

    // Get dataStoreMutex, check for story_id_presence & add new StoryPipeline if needed
    std::lock_guard storeLock(dataStoreMutex);
    auto pipeline_iter = theMapOfStoryPipelines.find(story_id);
    if(pipeline_iter != theMapOfStoryPipelines.end())
    {
        LOG_INFO("[GrapherDataStore] Story already being recorded. StoryId: {}", story_id);
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
            std::pair <chl::StoryId, chl::StoryPipeline*>(story_id, new chl::StoryPipeline(theExtractionQueue, chronicle
                                                                                           , story, story_id, start_time
                                                                                           , time_chunk_duration, access_window)));

    if(result.second)
    {
        LOG_INFO("[GrapherDataStore] New StoryPipeline created successfully. StoryId {}", story_id);
        pipeline_iter = result.first;
        //engage StoryPipeline with the IngestionQueue
        chl::StoryChunkIngestionHandle*ingestionHandle = (*pipeline_iter).second->getActiveIngestionHandle();
        theIngestionQueue.addStoryIngestionHandle(story_id, ingestionHandle);
        return chronolog::CL_SUCCESS;
    }
    else
    {
        LOG_ERROR("[GrapherDataStore] Failed to create StoryPipeline for StoryId: {}. Possible memory or resource issue."
                  , story_id);
        return chronolog::CL_ERR_UNKNOWN;
    }
}
////////////////////////

int chronolog::GrapherDataStore::stopStoryRecording(chronolog::StoryId const &story_id)
{
    LOG_DEBUG("[GrapherDataStore] Initiating stop recording for StoryId={}", story_id);
    // we do not yet disengage the StoryPipeline from the IngestionQueue right away
    // but put it on the WaitingForExit list to be finalized, persisted to disk , and
    // removed from memory at exit_time = now+acceptance_window...
    // unless there's a new story acquisition request comes before that moment
    std::lock_guard storeLock(dataStoreMutex);
    auto pipeline_iter = theMapOfStoryPipelines.find(story_id);
    if(pipeline_iter != theMapOfStoryPipelines.end())
    {
        uint64_t exit_time = std::chrono::high_resolution_clock::now().time_since_epoch().count() +
                             (*pipeline_iter).second->getAcceptanceWindow();
        pipelinesWaitingForExit[(*pipeline_iter).first] = (std::pair <chl::StoryPipeline*, uint64_t>(
                (*pipeline_iter).second, exit_time));
        LOG_INFO("[GrapherDataStore] Scheduled pipeline to retire: StoryId {} timeline {}-{} acceptanceWindow {} retirementTime {}",
                (*pipeline_iter).second->getStoryId(), (*pipeline_iter).second->TimelineStart(), (*pipeline_iter).second->TimelineEnd(),
                (*pipeline_iter).second->getAcceptanceWindow(), exit_time);
    }
    else
    {
        LOG_WARNING("[GrapherDataStore] Attempt to stop recording for non-existent StoryId={}", story_id);
    }
    return chronolog::CL_SUCCESS;
}

////////////////////////

void chronolog::GrapherDataStore::collectIngestedEvents()
{
    LOG_DEBUG("[GrapherDataStore] Initiating collection of ingested story chunks. Current state={}, Active "
              "StoryPipelines={}, PipelinesWaitingForExit={}, ThreadID={}"
              , state, theMapOfStoryPipelines.size(), pipelinesWaitingForExit.size(), tl::thread::self_id());
    theIngestionQueue.drainOrphanChunks();

    std::lock_guard storeLock(dataStoreMutex);
    for(auto pipeline_iter = theMapOfStoryPipelines.begin();
        pipeline_iter != theMapOfStoryPipelines.end(); ++pipeline_iter)
    {
        //INNA: this can be delegated to different threads handling individual storylines...
        (*pipeline_iter).second->collectIngestedEvents();
    }
}

////////////////////////
void chronolog::GrapherDataStore::extractDecayedStoryChunks()
{
    LOG_DEBUG("[GrapherDataStore] Initiating extraction of decayed story chunks. Current state={}, Active StoryPipelines={}, PipelinesWaitingForExit={}, ThreadID={}"
              , state, theMapOfStoryPipelines.size(), pipelinesWaitingForExit.size(), tl::thread::self_id());

    uint64_t current_time = std::chrono::high_resolution_clock::now().time_since_epoch().count();

    std::lock_guard storeLock(dataStoreMutex);
    for(auto pipeline_iter = theMapOfStoryPipelines.begin();
        pipeline_iter != theMapOfStoryPipelines.end(); ++pipeline_iter)
    {
        (*pipeline_iter).second->extractDecayedStoryChunks(current_time);
    }
}
////////////////////////

void chronolog::GrapherDataStore::retireDecayedPipelines()
{
    LOG_TRACE("[GrapherDataStore] Initiating retirement of decayed pipelines. Current state={}, Active StoryPipelines={}, PipelinesWaitingForExit={}, ThreadID={}"
              , state, theMapOfStoryPipelines.size(), pipelinesWaitingForExit.size(), tl::thread::self_id());

    if(!theMapOfStoryPipelines.empty())
    {
        std::lock_guard storeLock(dataStoreMutex);

        uint64_t current_time = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        for(auto pipeline_iter = pipelinesWaitingForExit.begin(); pipeline_iter != pipelinesWaitingForExit.end();)
        {
            if(current_time >= (*pipeline_iter).second.second)
            {
                //current_time >= pipeline exit_time
                StoryPipeline * pipeline = (*pipeline_iter).second.first;
                LOG_DEBUG("[GrapherDataStore] retiring pipeline StoryId {} timeline {}-{} acceptanceWindow {} retirementTime {}",
                        pipeline->getStoryId(), pipeline->TimelineStart(), pipeline->TimelineEnd(), pipeline->getAcceptanceWindow(), (*pipeline_iter).second.second);
                theMapOfStoryPipelines.erase(pipeline->getStoryId());
                theIngestionQueue.removeStoryIngestionHandle(pipeline->getStoryId());
                pipeline_iter = pipelinesWaitingForExit.erase(pipeline_iter);
                delete pipeline;
            }
            else
            { pipeline_iter++; }

        }
    }

    LOG_TRACE("[GrapherDataStore] Completed retirement of decayed pipelines. Current state={}, Active StoryPipelines={}, PipelinesWaitingForExit={}, ThreadID={}"
              , state, theMapOfStoryPipelines.size(), pipelinesWaitingForExit.size(), tl::thread::self_id());
}

void chronolog::GrapherDataStore::dataCollectionTask()
{
    //run dataCollectionTask as long as the state == RUNNING
    // or there're still events left to collect and
    // storyPipelines left to retire...
    tl::xstream es = tl::xstream::self();
    LOG_DEBUG("[GrapherDataStore] Initiating DataCollectionTask. ESrank={}, ThreadID={}", es.get_rank()
              , tl::thread::self_id());

    while(!is_shutting_down() || !theIngestionQueue.is_empty() || !theMapOfStoryPipelines.empty())
    {
        LOG_DEBUG("[GrapherDataStore] Running DataCollection iteration. ESrank={}, ThreadID={}", es.get_rank()
                  , tl::thread::self_id());
        for(int i = 0; i < 6; ++i)
        {
            collectIngestedEvents();
            sleep(10);
        }
        extractDecayedStoryChunks();
        retireDecayedPipelines();
    }
    LOG_DEBUG("[GrapherDataStore] Exiting DataCollectionTask thread {}", tl::thread::self_id());
}

////////////////////////
void chronolog::GrapherDataStore::startDataCollection(int stream_count)
{
    std::lock_guard storeLock(dataStoreStateMutex);
    if(is_running() || is_shutting_down())
    {
        LOG_INFO("[GrapherDataStore] Data collection is already running or shutting down. Ignoring request.");
        return;
    }

    LOG_INFO("[GrapherDataStore] Starting data collection. StreamCount={}, ThreadID={}", stream_count
             , tl::thread::self_id());
    state = RUNNING;

    for(int i = 0; i < stream_count; ++i)
    {
        tl::managed <tl::xstream> es = tl::xstream::create();
        dataStoreStreams.push_back(std::move(es));
    }

    for(int i = 0; i < 2 * stream_count; ++i)
    {
        tl::managed <tl::thread> th = dataStoreStreams[i % (dataStoreStreams.size())]->make_thread([p = this]()
                                                                                                   { p->dataCollectionTask(); });
        dataStoreThreads.push_back(std::move(th));
    }
    LOG_INFO("[GrapherDataStore] Data collection started successfully. Stream count={}, ThreadID={}", stream_count
             , tl::thread::self_id());
}
//////////////////////////////

void chronolog::GrapherDataStore::shutdownDataCollection()
{
    LOG_INFO("[GrapherDataStore] Initiating shutdown of DataCollection. CurrentState={}, Active StoryPipelines={}, PipelinesWaitingForExit={}"
             , state, theMapOfStoryPipelines.size(), pipelinesWaitingForExit.size());

    // switch the state to shuttingDown
    std::lock_guard storeLock(dataStoreStateMutex);
    if(is_shutting_down())
    {
        LOG_INFO("[GrapherDataStore] Data collection is already shutting down. Ignoring additional shutdown request.");
        return;
    }
    state = SHUTTING_DOWN;

    if(!theMapOfStoryPipelines.empty())
    {
        // label all existing Pipelines as waiting to exit
        std::lock_guard storeLock(dataStoreMutex);
        uint64_t current_time = std::chrono::high_resolution_clock::now().time_since_epoch().count();

        for(auto pipeline_iter = theMapOfStoryPipelines.begin();
            pipeline_iter != theMapOfStoryPipelines.end(); ++pipeline_iter)
        {
            if(pipelinesWaitingForExit.find((*pipeline_iter).first) == pipelinesWaitingForExit.end())
            {
                uint64_t exit_time = current_time + (*pipeline_iter).second->getAcceptanceWindow();
                pipelinesWaitingForExit[(*pipeline_iter).first] = (std::pair <chl::StoryPipeline*, uint64_t>(
                        (*pipeline_iter).second, exit_time));
            }
        }
    }

    // Join threads & execution streams while holding stateMutex
    // and just wait until all the events are collected and
    // all the storyPipelines decay and retire
    for(auto &th: dataStoreThreads)
    {
        th->join();
    }
    LOG_INFO("[GrapherDataStore] All data collection threads have been joined.");

    for(auto &es: dataStoreStreams)
    {
        es->join();
    }
    LOG_INFO("[GrapherDataStore] All data collection streams have been joined.");
    LOG_INFO("[GrapherDataStore] DataCollection shutdown completed.");
}

///////////////////////

//
chronolog::GrapherDataStore::~GrapherDataStore()
{
    LOG_INFO("[GrapherDataStore] Destructor called. Initiating shutdown. Active StoryPipelines count={}"
             , theMapOfStoryPipelines.size());
    shutdownDataCollection();
    LOG_INFO("[GrapherDataStore] Shutdown completed successfully. Active StoryPipelines count={}"
             , theMapOfStoryPipelines.size());
}
