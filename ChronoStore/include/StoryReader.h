#ifndef READ_H
#define READ_H

#include <iostream>
#include <map>
#include <json-c/json.h>
#include <StoryChunk.h>
#include <event.h>
#include <datasetreader.h>
#include <datasetminmax.h>
#include <chrono_monitor.h>
#include <hdf5.h>

class StoryReader
{
private:
    std::string chronicle_root_dir;

    // Deserialize StoryChunk from JSON string
    chronolog::StoryChunk
    deserializeStoryChunk(char*story_chunk_json_str, std::string chronicle_name, std::string story_name
                          , uint64_t story_id, uint64_t start_time, uint64_t end_time);

    // Find all Story Chunks that overlap with the given time range of a Story
    std::vector <std::string>
    findContiguousStoryChunks(const hid_t &story_file, uint64_t start_time, uint64_t end_time);

    // Check if a Story Chunk overlaps with the given time range
    static bool isStoryChunkOverlapping(const std::string &story_chunk_name, uint64_t start_time, uint64_t end_time);

    // Get all Story Chunks in a Story file
    std::vector <std::string> getAllStoryChunks(const hid_t &story_file);

public:
    StoryReader() = default;

    explicit StoryReader(std::string &chronicle_root_dir): chronicle_root_dir(chronicle_root_dir)
    {}

    ~StoryReader() = default;

    // Read Events within a time range from a Story file
    int
    readStoryRange(const std::string &chronicle_name, const uint64_t &story_id, uint64_t start_time, uint64_t end_time
                   , std::map <uint64_t, chronolog::StoryChunk> &story_chunk_map);

    // Read from all Story files in a Chronicle directory
    std::map <uint64_t, chronolog::StoryChunk> readAllStories(const std::string &chronicle_name);

    // Read from one Story file
    int readStory(const std::string &chronicle_name, const std::string &story_file_name
                  , std::map <uint64_t, chronolog::StoryChunk> &story_chunk_map);

    // Read all Story Chunks in a Story file
    int readAllStoryChunks(hid_t &story_file, std::map <uint64_t, chronolog::StoryChunk> &story_chunk_map);

    // Read a Story Chunk from a Story file
    chronolog::StoryChunk readOneStoryChunk(hid_t &story_file, const std::string &story_chunk_name);

    // Read string attribute from a Story Chunk dataset
    std::string readStringAttribute(hid_t &story_chunk_dset, const std::string &attribute_name);

    // Read uint64_t attribute from a Story Chunk dataset
    uint64_t readUint64Attribute(hid_t &story_chunk_dset, const std::string &attribute_name);
};

#endif