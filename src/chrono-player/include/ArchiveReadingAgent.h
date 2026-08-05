#ifndef ARCHIVE_READING_AGENT_H
#define ARCHIVE_READING_AGENT_H

#include <string>
#include <cstdint>
#include <vector>
#include <list>
#include <map>
#include <mutex>
#include <thallium.hpp>

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
    // read_aux_files: serve replay from the rotated/auxiliary archive files in
    // addition to the main one (ArchiveReaders.read_aux_files; off by default).
    // manifest_enabled: build the archive index from the manifest rather than by
    // scanning the archive directory (ArchiveReaders.manifest_enabled; on by
    // default, and it falls back to scanning by itself when there is no manifest).
    ArchiveReadingAgent(ArchiveReadingRequestQueue& request_queue,
                        std::string const& archive_path,
                        bool read_aux_files = false,
                        bool manifest_enabled = true)
        : theReadingRequestQueue(request_queue)
        , agentState(UNKNOWN)
        , theReadingAgent(archive_path,
                          true, // Default to polling mode
                          std::chrono::milliseconds(5000),
                          manifest_enabled)
        , readAuxFiles(read_aux_files)
    {}

    ~ArchiveReadingAgent();

    bool is_running() const { return (RUNNING == agentState); }

    bool is_shutting_down() const { return (SHUTTING_DOWN == agentState); }

    void startArchiveReading(int stream_count);

    void shutdownArchiveReading();

    void archiveReadingTask();

private:
    ArchiveReadingAgent(ArchiveReadingAgent const&) = delete;

    ArchiveReadingAgent& operator=(ArchiveReadingAgent const&) = delete;

    ArchiveReadingRequestQueue& theReadingRequestQueue;

    std::mutex agentStateMutex;
    ReadingAgentState agentState;
    std::vector<thallium::managed<thallium::xstream>> archiveReadingStreams;
    std::vector<thallium::managed<thallium::thread>> archiveReadingThreads;

    //archiveSpecific readingAgent (template later on)
    HDF5ArchiveReadingAgent theReadingAgent;

    // include rotated/auxiliary archive files in replay (ArchiveReaders.read_aux_files)
    bool readAuxFiles;
};

} // namespace chronolog
#endif
