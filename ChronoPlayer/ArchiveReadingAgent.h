#ifndef ARCHIVE_READING_AGENT_H
#define ARCHIVE_READING_AGENT_H

#include <vector>
#include <list>
#include <map>
#include <mutex>

#include <thallium.hpp>

#include "ArchiveReadingRequest.h"
#include "StoryChunkIngestionQueue.h"

namespace chronolog
{

class DummyReadingAgent 
{
public:
    DummyReadingAgent()
    {}
   
    ~DummyReadingAgent()
    {}

    int initialize()
    { return 1; }

    int shutdown()
    { return 1; }

    int readArchivedStory( ChronicleName, StoryName, uint64_t, uint64_t, std::list<StoryChunk*>&)
    { return 1; }
};

class ArchiveReadingAgent
{

    enum ReadingAgentState
    {
        UNKNOWN = 0, 
        RUNNING = 1,   
        SHUTTING_DOWN = 2   
    };


public:
    ArchiveReadingAgent( ArchiveReadingRequestQueue & request_queue, StoryChunkIngestionQueue & ingestion_queue)
        : agentState(UNKNOWN)
        , theReadingRequestQueue(request_queue)
        , theIngestionQueue(ingestion_queue)
    {}

    ~ArchiveReadingAgent();

    bool is_running() const
    { return (RUNNING == agentState); }

    bool is_shutting_down() const
    { return (SHUTTING_DOWN == agentState); }

    void startArchiveReading(int stream_count);

    void shutdownArchiveReading();

    void archiveReadingTask();

private:
    ArchiveReadingAgent(ArchiveReadingAgent const &) = delete;

    ArchiveReadingAgent &operator=(ArchiveReadingAgent const &) = delete;

    ArchiveReadingRequestQueue & theReadingRequestQueue;
    StoryChunkIngestionQueue & theIngestionQueue;

    ReadingAgentState agentState;
    std::mutex agentStateMutex;
    std::vector <thallium::managed <thallium::xstream>> archiveReadingStreams;
    std::vector <thallium::managed <thallium::thread>> archiveReadingThreads;

    //archiveSpecific readingAgent (template later on)
    DummyReadingAgent theReadingAgent;

};

}
#endif
