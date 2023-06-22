#ifndef READ_H
#define READ_H

#include <iostream>
#include <map>
#include <json-c/json.h>
#include "../../ChronoKeeper/StoryChunk.h"
#include <event.h>
#include <datasetreader.h>
#include <datasetminmax.h>
#include <log.h>

class StoryReader
{
private:
    std::string chronicle_root_dir;

    chronolog::StoryChunk deserializeStoryChunk(char *story_chunk_json_str,
                                                uint64_t story_id, uint64_t start_time, uint64_t end_time);


public:
    StoryReader() = default;

    explicit StoryReader(std::string& chronicle_root_dir) : chronicle_root_dir(chronicle_root_dir) {}

    ~StoryReader() = default;

    // Read data from H5 dataset
    DatasetReader readFromDataset(std::pair<uint64_t, uint64_t> range, const char* STORY, const char* CHRONICLE);

    // Read dataset data range i.e Min and Max
    DatasetMinMax readDatasetRange(const char* STORY, const char* CHRONICLE);

    // Read from all Story files in a Chronicle directory
    std::map<uint64_t, chronolog::StoryChunk> readAllStories(const std::string& chronicle_name);

    // Read from one Story file
    int readStory(const std::string &chronicle_name, const std::string &story_file_name,
                  std::map<uint64_t, chronolog::StoryChunk>& story_chunk_map);

    // Read from all Story Chunks in a Story file
    int readAllStoryChunks(hid_t& story_file,
                           std::map<uint64_t,
                           chronolog::StoryChunk>& story_chunk_map);

    // Read attribute from a Story Chunk dataset
    std::string readAttribute(hid_t dataset_id, const std::string& attribute_name);
};

#endif