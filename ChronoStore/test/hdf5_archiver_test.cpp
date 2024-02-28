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
#include <log.h>
#include <story_chunk_test_utils.h>
#include "chrono_common/chronolog_types.h"

#define STORY "S1"
#define CHRONICLE "C1.h5"
#define NUM_OF_TESTS 200

#define CHRONICLE_ROOT_DIR "/home/kfeng/chronolog_store/"
#define CLIENT_ID 1
#define CHRONICLE_NAME "Ares_Monitoring"
#define STORY_NAME "CPU_Utilization"

void testWriteOperation(const std::map <uint64_t, chronolog::StoryChunk> &story_chunk_map, std::string &chronicle_name
                        , std::string &story_name)
{
    std::string chronicle_root_dir = CHRONICLE_ROOT_DIR;
    StoryWriter writer(chronicle_root_dir);
    writer.writeStoryChunks(story_chunk_map, chronicle_name);
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
    if(story_chunk_map == story_chunk_map2)
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
    if(story_chunk_map3 == story_chunk_map4)
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
