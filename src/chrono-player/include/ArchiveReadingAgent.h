#ifndef ARCHIVE_READING_AGENT_H
#define ARCHIVE_READING_AGENT_H

#include <string>
#include <cstdint>
#include <vector>
#include <list>
#include <map>
#include <mutex>
#include <thallium.hpp>
#include <LifecycleRecord.h>

#include "ArchiveReadingRequestQueue.h"
#include "HDF5ArchiveReadingAgent.h"

namespace chronolog
{

class DummyReadingAgent
{
public:
    DummyReadingAgent() {}

    ~DummyReadingAgent() {}

    int initialize() { return 1; }

    int shutdown() { return 1; }

    int readArchivedStory(ChronicleName, StoryName, uint64_t, uint64_t, std::list<StoryChunk*>&) { return 1; }
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
    ArchiveReadingAgent(ArchiveReadingRequestQueue& request_queue, std::string const& archive_path)
        : theReadingRequestQueue(request_queue)
        , agentState(UNKNOWN)
        , theReadingAgent(archive_path)
    {}

    ~ArchiveReadingAgent();
    std::size_t nativeHandleCount() const noexcept { return nativeHandles_.load(std::memory_order_acquire); }

    bool is_running() const { return (RUNNING == agentState); }

    bool is_shutting_down() const { return (SHUTTING_DOWN == agentState); }

    void startArchiveReading(int stream_count);

    void shutdownArchiveReading();

    void archiveReadingTask();

private:
    ArchiveReadingAgent(ArchiveReadingAgent const&) = delete;

    ArchiveReadingAgent& operator=(ArchiveReadingAgent const&) = delete;

    ArchiveReadingRequestQueue& theReadingRequestQueue;

    std::atomic<std::size_t> nativeHandles_{0};
    std::mutex agentStateMutex;
    std::atomic<ReadingAgentState> agentState;
    rpc::JoinRecord readingJoin;
    std::vector<thallium::managed<thallium::xstream>> archiveReadingStreams;
    std::vector<thallium::managed<thallium::thread>> archiveReadingThreads;

    //archiveSpecific readingAgent (template later on)
    HDF5ArchiveReadingAgent theReadingAgent;
};

} // namespace chronolog
#endif
