#include <private/NativeThreadLabel.h>
#include <unistd.h>
#include <list>
#include <mutex>
#include <chrono>
#include <thallium.hpp>

#include <chronolog_errcode.h>
#include <chrono_monitor.h>
#include <StoryChunk.h>
#include <ArchiveReadingRequestQueue.h>
#include <ArchiveReadingAgent.h>
#include <QueryResponseTransferAgent.h>

namespace chl = chronolog;
namespace tl = thallium;


////////////////////////

void chronolog::ArchiveReadingAgent::archiveReadingTask()
{
    LOG_DEBUG("[ReadingAgent] executing archiveReadingTask ReadingAgent state={}, ThreadID={}",
              agentState.load(),
              chronolog::rpc::nativeThreadLabel());

    while(!is_shutting_down())
    {
        if(theReadingRequestQueue.empty())
        {
            sleep(1);
            continue;
        }

        // 1. take a reading request off the reading queue
        // 2. read in the requested data from the archive store
        // 3. pass the story chunks read from archive to the QueryResponseTransferAgent

        chl::ArchiveReadingRequest* readingRequest = theReadingRequestQueue.popReadingRequest();

        if(readingRequest == nullptr || !readingRequest->is_valid())
        {
            // queue might have been drained by another thread before this this thread got a hold
            // of the reading queue mutex
            continue;
        }

        std::list<chl::StoryChunk*> listOfChunks;

        int const read_status = theReadingAgent.readArchivedStory(readingRequest->chronicleName,
                                                                  readingRequest->storyName,
                                                                  readingRequest->startTime,
                                                                  readingRequest->endTime,
                                                                  listOfChunks);
        if(read_status != chl::CL_SUCCESS)
        {
            // the response still goes out; it is short by whatever those files held
            LOG_WARNING("[ReadingAgent] archive read for query {} Chronicle={}, Story={} failed on at least one file "
                        "({}); the replay is missing its events",
                        readingRequest->queryId,
                        readingRequest->chronicleName,
                        readingRequest->storyName,
                        read_status);
        }

        LOG_DEBUG("[ReadingAgent] Read {} StoryChunks for query {} Chronicle={}, Story={}, TimeRange=[{}, {})",
                  listOfChunks.size(),
                  readingRequest->queryId,
                  readingRequest->chronicleName,
                  readingRequest->storyName,
                  readingRequest->startTime,
                  readingRequest->endTime);

        // notify the queryResponseAgent that archive read is completed
        // even if no events within the query time range were found
        readingRequest->queryResponseAgent->addArchivedEventsToQueryResponse(readingRequest->queryId,
                                                                             listOfChunks,
                                                                             read_status);

        // in case queryResponseAgent didn't recognise the query and failed to drain listOfChunks
        // drain it here
        if(!listOfChunks.empty())
        {
            for(auto& chunk: listOfChunks) { delete chunk; }
            listOfChunks.clear();
        }

        delete readingRequest;
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

    LOG_INFO("[ReadingAgent] Starting reading streams. StreamCount={}, ThreadID={}",
             stream_count,
             chronolog::rpc::nativeThreadLabel());

    agentState = RUNNING;

    for(int i = 0; i < stream_count; ++i)
    {
        tl::managed<tl::xstream> es = tl::xstream::create();
        archiveReadingStreams.push_back(std::move(es));
        nativeHandles_.fetch_add(1, std::memory_order_release);
    }

    for(int i = 0; i < 2 * stream_count; ++i)
    {
        tl::managed<tl::thread> th = archiveReadingStreams[i % (archiveReadingStreams.size())]->make_thread(
                [p = this]() { p->archiveReadingTask(); });
        archiveReadingThreads.push_back(std::move(th));
        nativeHandles_.fetch_add(1, std::memory_order_release);
    }

    LOG_INFO("[ReadingAgent] reading streams started successfully. Stream count={}, ThreadID={}",
             stream_count,
             chronolog::rpc::nativeThreadLabel());
}
//////////////////////////////

void chronolog::ArchiveReadingAgent::shutdownArchiveReading()
{
    readingJoin.finish(
            [this]
            {
                LOG_INFO("[ReadingAgent] Initiating shutdown of archive reading. agentState={}", agentState.load());

                // switch the state to shuttingDown

                {
                    std::lock_guard stateLock(agentStateMutex);
                    agentState = SHUTTING_DOWN;
                }

                theReadingRequestQueue.clear();

                // Join threads & execution streams after publishing state under a short lock
                // and just wait until all the events are collected and
                // all the storyPipelines decay and retire
                for(auto& th: archiveReadingThreads) { th->join(); }
                auto archiveReadingThreadsCount = archiveReadingThreads.size();
                archiveReadingThreads.clear();
                nativeHandles_.fetch_sub(archiveReadingThreadsCount, std::memory_order_release);
                LOG_INFO("[ReadingAgent] All archive reading threads have been joined.");

                for(auto& es: archiveReadingStreams) { es->join(); }
                auto archiveReadingStreamsCount = archiveReadingStreams.size();
                archiveReadingStreams.clear();
                nativeHandles_.fetch_sub(archiveReadingStreamsCount, std::memory_order_release);

                theReadingAgent.shutdown();

                LOG_INFO("[ReadingAgent] Archive reading is shutdown.");
            });
}

///////////////////////

//
chronolog::ArchiveReadingAgent::~ArchiveReadingAgent() { shutdownArchiveReading(); }
