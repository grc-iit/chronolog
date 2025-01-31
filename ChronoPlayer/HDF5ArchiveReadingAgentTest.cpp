//
// Created by kfeng on 1/14/25.
//
#include <csignal>
#include "HDF5ArchiveReadingAgent.h"
#include "ConfigurationManager.h"
#include "cmd_arg_parse.h"

namespace tl = thallium;

std::atomic<bool> running(true);
std::list<chronolog::StoryChunk*> list_of_chunks;
chronolog::HDF5ArchiveReadingAgent* agent_ptr = nullptr;

void signalHandler(int signum)
{
    std::cout << "Interrupt signal (" << signum << ") received.\n";

    for(auto &chunk: list_of_chunks)
    {
        delete chunk;
    }

    if (agent_ptr)
    {
        agent_ptr->shutdown();
    }
    delete agent_ptr;

    running = false;
    std::exit(signum);
}

int main(int argc, char**argv)
{
//    uint64_t current_timestamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::string chronicle_name = "chronicle_0_0";
    std::string story_name = "story_0_0";
    uint64_t start_time = 1736800000000000000;
    uint64_t end_time = start_time + 1000000000000000;

    signal(SIGINT, signalHandler);

    // Configure SetUp ________________________________________________________________________________________________
    std::string conf_file_path;
    conf_file_path = parse_conf_path_arg(argc, argv);
    if(conf_file_path.empty())
    {
        std::exit(EXIT_FAILURE);
    }
    ChronoLog::ConfigurationManager confManager(conf_file_path);
    int result = chronolog::chrono_monitor::initialize(confManager.CLIENT_CONF.CLIENT_LOG_CONF.LOGTYPE
                                                       , confManager.CLIENT_CONF.CLIENT_LOG_CONF.LOGFILE
                                                       , confManager.CLIENT_CONF.CLIENT_LOG_CONF.LOGLEVEL
                                                       , confManager.CLIENT_CONF.CLIENT_LOG_CONF.LOGNAME
                                                       , confManager.CLIENT_CONF.CLIENT_LOG_CONF.LOGFILESIZE
                                                       , confManager.CLIENT_CONF.CLIENT_LOG_CONF.LOGFILENUM
                                                       , confManager.CLIENT_CONF.CLIENT_LOG_CONF.FLUSHLEVEL);
    if(result == 1)
    {
        exit(EXIT_FAILURE);
    }

    tl::abt scope;

    std::string archive_path = confManager.GRAPHER_CONF.EXTRACTOR_CONF.story_files_dir;
    agent_ptr = new chronolog::HDF5ArchiveReadingAgent(archive_path);

    agent_ptr->initialize();

//    std::list<chronolog::StoryChunk*> list_of_chunks;
    std::cout << "Reading events in range [" << start_time << ", " << end_time << "]" << std::endl;
    agent_ptr->readArchivedStory(chronicle_name, story_name, start_time, end_time, list_of_chunks);
    std::cout << list_of_chunks.size() << " chunks is returned." << std::endl;

    bool all_events_within_time_range = true;
    for(auto &chunk: list_of_chunks)
    {
        for(auto &event: *chunk)
        {
            if(event.second.eventTime < start_time || event.second.eventTime > end_time)
            {
                std::cout << "Error: event time " << event.second.eventTime << " out of range" << std::endl;
                all_events_within_time_range = false;
                break;
            }
        }
        std::cout << "All " << chunk->getEventCount() << " events in the chunk are within the time range." << std::endl;
    }
    if(all_events_within_time_range)
    {
        std::cout << "All events in all chunks are within the time range." << std::endl;
    }

    while(running)
    {
        std::this_thread::sleep_for(std::chrono::seconds(10));
    }

    return 0;
}