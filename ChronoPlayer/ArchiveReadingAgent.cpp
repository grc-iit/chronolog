#include <unistd.h>
#include <list>
#include <mutex>
#include <chrono>

#include <thallium.hpp>

#include "chronolog_errcode.h"
#include "chrono_monitor.h"

#include "StoryChunk.h"
#include "ArchiveReadingRequestQueue.h"
#include "ArchiveReadingAgent.h"
#include "StoryChunkExtractionQueue.h"

namespace chl = chronolog;
namespace tl = thallium;


////////////////////////

void chronolog::ArchiveReadingAgent::archiveReadingTask()
{
    LOG_DEBUG("[ReadingAgent] executing archiveReadingTask ReadingAgent state={}, ThreadID={}"
              , agentState, tl::thread::self_id());
    
    while( !is_shutting_down() )
    {
        if(theReadingRequestQueue.empty())
        {
            sleep(1);
            continue;
        }
         
        // 1. take a reading request off the reading queue
        // 2. read in the requested data from the archive store
        // 3. continue adding every StoryChunk read onto the StoryChunkIngestionQueue for merging with the current realtime stream of chunks from Keepers/Graphers
        //    or bypass the merging and add the StoryChunk directly onto the ExtractionQueue to be sent to the client
           
        chl::ArchiveReadingRequest readingRequest;
        theReadingRequestQueue.popReadingRequest(readingRequest); 
        std::list<chl::StoryChunk*> listOfChunks;

        theReadingAgent.readArchivedStory(readingRequest.chronicleName, readingRequest.storyName, readingRequest.startTime, readingRequest.endTime, listOfChunks);

        while( !listOfChunks.empty())
        {
            readingRequest.storyChunkQueue->stashStoryChunk(listOfChunks.front());
            listOfChunks.pop_front();
        }
                
    }

}

////////////////////////

void chronolog::ArchiveReadingAgent::startArchiveReading(int stream_count)
{
    std::lock_guard lock(agentStateMutex);
    if(is_running() || is_shutting_down())
    {
        LOG_INFO("[ReadingAgent] Data collection is already running or shutting down. Ignoring request.");
        return;
    }

    theReadingAgent.initialize();

    LOG_INFO("[ReadingAgent] Starting reading streams. StreamCount={}, ThreadID={}", stream_count
             , tl::thread::self_id());

    agentState = RUNNING;

    for(int i = 0; i < stream_count; ++i)
    {
        tl::managed <tl::xstream> es = tl::xstream::create();
        archiveReadingStreams.push_back(std::move(es));
    }

    for(int i = 0; i < 2 * stream_count; ++i)
    {
        tl::managed <tl::thread> th = archiveReadingStreams[i % (archiveReadingStreams.size())]->make_thread([p = this]()
                                                                                                   { p->archiveReadingTask(); });
        archiveReadingThreads.push_back(std::move(th));
    }

    LOG_INFO("[ReadingAgent] reading streams started successfully. Stream count={}, ThreadID={}", stream_count
             , tl::thread::self_id());
}
//////////////////////////////

void chronolog::ArchiveReadingAgent::shutdownArchiveReading()
{
    LOG_INFO("[ReadingAgent] Initiating shutdown of archive reading. agentState={}", agentState);

    // switch the state to shuttingDown
    std::lock_guard lock(agentStateMutex);
    if(is_shutting_down())
    {
        LOG_INFO("[ReadingAgent] reading agent is already shutting down. Ignoring additional shutdown request.");
        return;
    }

    agentState = SHUTTING_DOWN;

    theReadingRequestQueue.clear();

    // Join threads & execution streams while holding stateMutex
    // and just wait until all the events are collected and
    // all the storyPipelines decay and retire
    for(auto &th: archiveReadingThreads)
    {
        th->join();
    }
    LOG_INFO("[ReadingAgent] All archive reading threads have been joined.");

    for(auto &es: archiveReadingStreams)
    {
        es->join();
    }

    theReadingAgent.shutdown();

    LOG_INFO("[ReadingAgent] Archive reading is shutdown.");
}

///////////////////////

//
chronolog::ArchiveReadingAgent::~ArchiveReadingAgent()
{
    shutdownArchiveReading();
}
