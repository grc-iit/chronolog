#include <unistd.h>
#include <map>
#include <mutex>
#include <chrono>
#include <unistd.h>

#include <thallium.hpp>

#include "chronolog_errcode.h"
#include "PlayerDataStore.h"
#include "chrono_monitor.h"

namespace chl = chronolog;
namespace tl = thallium;


////////////////////////

void chronolog::PlayerDataStore::collectIngestedEvents()
{
    LOG_DEBUG("[PlayerDataStore] Initiating collection of ingested story chunks. Current state={}, Active "
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
void chronolog::PlayerDataStore::extractDecayedStoryChunks()
{
    LOG_DEBUG("[PlayerDataStore] Initiating extraction of decayed story chunks. Current state={}, Active StoryPipelines={}, PipelinesWaitingForExit={}, ThreadID={}"
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

void chronolog::PlayerDataStore::retireDecayedPipelines()
{
    LOG_TRACE("[PlayerDataStore] Initiating retirement of decayed pipelines. Current state={}, Active StoryPipelines={}, PipelinesWaitingForExit={}, ThreadID={}"
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
                LOG_DEBUG("[PlayerDataStore] retiring pipeline StoryId {} timeline {}-{} acceptanceWindow {} retirementTime {}",
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

    LOG_TRACE("[PlayerDataStore] Completed retirement of decayed pipelines. Current state={}, Active StoryPipelines={}, PipelinesWaitingForExit={}, ThreadID={}"
              , state, theMapOfStoryPipelines.size(), pipelinesWaitingForExit.size(), tl::thread::self_id());
}

void chronolog::PlayerDataStore::dataCollectionTask()
{
    //run dataCollectionTask as long as the state == RUNNING
    // or there're still events left to collect and
    // storyPipelines left to retire...
    tl::xstream es = tl::xstream::self();
    LOG_DEBUG("[PlayerDataStore] Initiating DataCollectionTask. ESrank={}, ThreadID={}", es.get_rank()
              , tl::thread::self_id());

    while(!is_shutting_down() || !theIngestionQueue.is_empty() || !theMapOfStoryPipelines.empty())
    {
        LOG_DEBUG("[PlayerDataStore] Running DataCollection iteration. ESrank={}, ThreadID={}", es.get_rank()
                  , tl::thread::self_id());
        for(int i = 0; i < 6; ++i)
        {
            collectIngestedEvents();
            sleep(10);
        }
        extractDecayedStoryChunks();
        retireDecayedPipelines();
    }
    LOG_DEBUG("[PlayerDataStore] Exiting DataCollectionTask thread {}", tl::thread::self_id());
}

////////////////////////
void chronolog::PlayerDataStore::startDataCollection(int stream_count)
{
    std::lock_guard storeLock(dataStoreStateMutex);
    if(is_running() || is_shutting_down())
    {
        LOG_INFO("[PlayerDataStore] Data collection is already running or shutting down. Ignoring request.");
        return;
    }

    LOG_INFO("[PlayerDataStore] Starting data collection. StreamCount={}, ThreadID={}", stream_count
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
    LOG_INFO("[PlayerDataStore] Data collection started successfully. Stream count={}, ThreadID={}", stream_count
             , tl::thread::self_id());
}
//////////////////////////////

void chronolog::PlayerDataStore::shutdownDataCollection()
{
    LOG_INFO("[PlayerDataStore] Initiating shutdown of DataCollection. CurrentState={}, Active StoryPipelines={}, PipelinesWaitingForExit={}"
             , state, theMapOfStoryPipelines.size(), pipelinesWaitingForExit.size());

    // switch the state to shuttingDown
    std::lock_guard storeLock(dataStoreStateMutex);
    if(is_shutting_down())
    {
        LOG_INFO("[PlayerDataStore] Data collection is already shutting down. Ignoring additional shutdown request.");
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
    LOG_INFO("[PlayerDataStore] All data collection threads have been joined.");

    for(auto &es: dataStoreStreams)
    {
        es->join();
    }
    LOG_INFO("[PlayerDataStore] All data collection streams have been joined.");
    LOG_INFO("[PlayerDataStore] DataCollection shutdown completed.");
}

///////////////////////

//
chronolog::PlayerDataStore::~PlayerDataStore()
{
    LOG_INFO("[PlayerDataStore] Destructor called. Initiating shutdown. Active StoryPipelines count={}"
             , theMapOfStoryPipelines.size());
    shutdownDataCollection();
    LOG_INFO("[PlayerDataStore] Shutdown completed successfully. Active StoryPipelines count={}"
             , theMapOfStoryPipelines.size());
}
