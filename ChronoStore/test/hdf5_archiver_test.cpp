#include <iostream>
#include <vector>
#include <map>
#include <string>
#include <chrono>
#include <random>
#include <filesystem>
#include <hdf5.h>
#include <StoryWriter.h>
#include <StoryReader.h>
#include <chrono_monitor.h>
#include <story_chunk_test_utils.h>
#include "chronolog_types.h"

#define STORY "S1"
#define CHRONICLE "C1.h5"
#define NUM_OF_TESTS 200

#define CHRONICLE_ROOT_DIR "/home/kfeng/chronolog_store/"

bool compareLogEvent(const chronolog::LogEvent &event1, const chronolog::LogEvent &event2)
{
    if(event1.storyId != event2.storyId)
    {
        return false;
    }
    if(event1.clientId != event2.clientId)
    {
        return false;
    }
    if(event1.eventIndex != event2.eventIndex)
    {
        return false;
    }
    if(event1.eventTime != event2.eventTime)
    {
        return false;
    }
    if(event1.logRecord != event2.logRecord)
    {
        return false;
    }
    return true;
}

bool compareStoryChunk(chronolog::StoryChunk &story_chunk1, chronolog::StoryChunk &story_chunk2)
{
    auto size1 = std::distance(story_chunk1.begin(), story_chunk1.end());
    auto size2 = std::distance(story_chunk2.begin(), story_chunk2.end());
    if(size1 != size2)
    {
        return false;
    }
    auto it1 = story_chunk1.begin();
    auto it2 = story_chunk2.begin();
    while(it1 != story_chunk1.end())
    {
        if(it1->first != it2->first)
        {
            return false;
        }
        if(!compareLogEvent(it1->second, it2->second))
        {
            return false;
        }
        it1++;
        it2++;
    }
    return true;
}

bool compareStoryChunkMaps(std::map <uint64_t, chronolog::StoryChunk> &map1
                           , std::map <uint64_t, chronolog::StoryChunk> &map2)
{
    if(map1.size() != map2.size())
    {
        return false;
    }
    for(auto &it: map1)
    {
        if(map2.find(it.first) == map2.end())
        {
            return false;
        }
        if(!compareStoryChunk(it.second, map2[it.first]))
        {
            return false;
        }
    }
    return true;
}

void testWriteOperation(const std::map <uint64_t, chronolog::StoryChunk> &story_chunk_map, std::string &chronicle_name
                        , std::string &story_name)
{
    std::string chronicle_root_dir = CHRONICLE_ROOT_DIR;
    StoryWriter writer(chronicle_root_dir);
    writer.writeStoryChunks(story_chunk_map, chronicle_name, story_name);
}

void testRangeReadOperation(const std::string &chronicle_name, const uint64_t &story_id, uint64_t start_time
                            , uint64_t end_time, std::map <uint64_t, chronolog::StoryChunk> &story_chunk_map)
{
    std::string chronicle_root_dir = CHRONICLE_ROOT_DIR;
    StoryReader sr(chronicle_root_dir);
    sr.readStoryRange(chronicle_name, story_id, start_time, end_time, story_chunk_map);
}

std::map <uint64_t, chronolog::StoryChunk> testReadAllOperation(uint64_t story_id, std::string &chronicle_name)
{
    hid_t status;
    std::map <uint64_t, chronolog::StoryChunk> story_chunk_map;
    std::string chronicle_root_dir = CHRONICLE_ROOT_DIR;
    StoryReader sr(chronicle_root_dir);
    story_chunk_map = sr.readAllStories(chronicle_name);
    return story_chunk_map;
}

/**
 * @param argv[1]: number of StoryChunks
 * @param argv[2]: number of Events in each StoryChunk
 * @param argv[3]: mean Event size
 * @param argv[4]: minimum Event size
 * @param argv[5]: maximum Event size
 * @param argv[6]: standard deviation of Event size
 */
int main(int argc, char*argv[])
{
    if(argc < 8)
    {
        std::cout << "Usage: " << argv[0]
                  << " #StoryChunks #EventsInEachChunk meanEventSize minEventSize maxEventSize stddev"
                  << " startTime endTime" << std::endl;
        return 1;
    }
    std::random_device rd;
    std::mt19937_64 rng(rd());
    std::uniform_int_distribution <uint64_t> dist(0, UINT64_MAX);
    uint64_t num_story_chunks = strtol(argv[1], nullptr, 10);
    uint64_t num_events_per_story_chunk = strtol(argv[2], nullptr, 10);
    uint64_t mean_event_size = strtol(argv[3], nullptr, 10);
    uint64_t min_event_size = strtol(argv[4], nullptr, 10);
    uint64_t max_event_size = strtol(argv[5], nullptr, 10);
    double stddev = strtod(argv[6], nullptr);
    uint64_t start_time = strtol(argv[7], nullptr, 10);
    uint64_t end_time = strtol(argv[8], nullptr, 10);
    std::string chronicle_name = CHRONICLE_NAME;
    std::string story_name = STORY_NAME;
    uint64_t story_id = dist(rng);
    uint64_t client_id = CLIENT_ID;

    int result = chronolog::chrono_monitor::initialize("console", "hdf5_archiver_test.log", spdlog::level::debug
                                                       , "hdf5_archiver_test", 102400, 1, spdlog::level::debug);
    if(result == 1)
    {
        exit(EXIT_FAILURE);
    }

    std::cout << "StoryID: " << story_id << std::endl << "#StoryChunks: " << num_story_chunks << std::endl
              << "#EventsInEachChunk: " << num_events_per_story_chunk << std::endl << "meanEventSize: "
              << mean_event_size << std::endl << "minEventSize: " << min_event_size << std::endl << "maxEventSize: "
              << max_event_size << std::endl << "stddev: " << stddev << std::endl << "startTime: " << start_time
              << std::endl << "endTime: " << end_time << std::endl;

    // Generate StoryChunks
    std::cout << "Generating StoryChunks..." << std::endl;
    uint64_t time_step = (end_time - start_time) / num_events_per_story_chunk;
    std::map <uint64_t, chronolog::StoryChunk> story_chunk_map = generateStoryChunks(story_id, client_id, start_time
                                                                                     , time_step, end_time
                                                                                     , mean_event_size, min_event_size
                                                                                     , max_event_size, stddev
                                                                                     , num_events_per_story_chunk
                                                                                     , num_story_chunks);

    // Clean up Chronicle directory
    std::cout << "Cleaning up Chronicle directory..." << std::endl;
    for(const auto &entry: std::filesystem::directory_iterator(CHRONICLE_ROOT_DIR))
    {
        std::filesystem::remove_all(entry.path());
    }
    std::filesystem::create_directory(CHRONICLE_ROOT_DIR);

    // Test write operation
    std::cout << "Testing write operation..." << std::endl;
    testWriteOperation(story_chunk_map, chronicle_name, story_name);

    // Test read operation
    std::cout << "Testing read operation..." << std::endl;
    std::map <uint64_t, chronolog::StoryChunk> story_chunk_map2 = testReadAllOperation(story_id, chronicle_name);

    // Validate read all results
    std::cout << "Validating read all results ..." << std::endl;
    if(compareStoryChunkMaps(story_chunk_map, story_chunk_map2))
    {
        std::cout << "Validation passed!" << std::endl;
    }
    else
    {
        std::cout << "Validation failed!" << std::endl;
        std::cout << "Correct: " << std::endl;
        for(const auto &story_chunk_str: StoryWriter::serializeStoryChunkMap(story_chunk_map))
        {
            std::cout << story_chunk_str << std::endl;
        }
        std::cout << "Result: " << std::endl;
        for(const auto &story_chunk_str: StoryWriter::serializeStoryChunkMap(story_chunk_map2))
        {
            std::cout << story_chunk_str << std::endl;
        }
    }

    // Test range read operation
    std::cout << "Testing range read operation..." << std::endl;
    uint64_t range_start_time = start_time + dist(rng) % (end_time - start_time) / 2;
    uint64_t range_end_time = range_start_time + dist(rng) % (end_time - range_start_time);
    std::map <uint64_t, chronolog::StoryChunk> story_chunk_map3;
    testRangeReadOperation(chronicle_name, story_id, range_start_time, range_end_time, story_chunk_map3);

    // Generate reference range read results to validate
    std::map <uint64_t, chronolog::StoryChunk> story_chunk_map4;
    for(auto &it: story_chunk_map)
    {
        if(it.first >= range_start_time || it.first < range_end_time)
        {
            std::map <chronolog::EventSequence, chronolog::LogEvent> event_map;
            for(const auto &event_it: it.second)
            {
                uint64_t event_time = std::get <0>(event_it.first);
                if(event_time >= range_start_time && event_time < range_end_time)
                {
                    event_map.emplace(event_it.first, event_it.second);
                }
            }
            chronolog::StoryChunk story_chunk;
            if(!event_map.empty())
            {
                for(const auto &event: event_map)
                {
                    story_chunk.insertEvent(event.second);
                }
            }
        }
    }

    // Validate range read results
    std::cout << "Validating range read [" << range_start_time << ", " << range_end_time << ") " << "out of ["
              << start_time << ", " << end_time << ") results ..." << std::endl;
    if(compareStoryChunkMaps(story_chunk_map3, story_chunk_map4))
    {
        std::cout << "Validation passed!" << std::endl;
    }
    else
    {
        std::cout << "Validation failed!" << std::endl;
        std::cout << "Correct: " << std::endl;
        for(const auto &story_chunk_str: StoryWriter::serializeStoryChunkMap(story_chunk_map4))
        {
            std::cout << story_chunk_str << std::endl;
        }
        std::cout << "Result: " << std::endl;
        for(const auto &story_chunk_str: StoryWriter::serializeStoryChunkMap(story_chunk_map3))
        {
            std::cout << story_chunk_str << std::endl;
        }
    }

    return 0;
}
