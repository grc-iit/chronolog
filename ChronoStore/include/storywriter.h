#ifndef STORY_WRITER_H
#define STORY_WRITER_H

#include <iostream>
#include <vector>
#include <map>
#include <event.h>
#include <json-c/json.h>
#include "../../ChronoKeeper/StoryChunk.h"

#define CHRONICLE_ROOT_DIR "/home/kfeng/chronolog_store/"

class storywriter
{
private:
    /* data */

    template <typename T>
    json_object *convertToJsonObject(const T &value);

    template <typename... Args, size_t... Is>
    json_object *serializeTupleToJsonObject(const std::tuple<Args...> &tuple, std::index_sequence<Is...>);

    template <typename... Args>
    json_object *serializeTupleToJsonObject(const std::tuple<Args...> &tuple);

    hid_t writeAttribute(hid_t dataset_id, const std::string &attribute_name, const std::string &attribute_value);

public:
    storywriter(/* args */);

    ~storywriter();

    int writeStoryChunk(std::vector<Event> *storyChunk, const char *DATASET_NAME, const char *H5FILE_NAME);

    int writeStoryChunks(const std::map<uint64_t, chronolog::StoryChunk> &story_chunk_map,
                         const std::string &chronicle_name);

    void serializeMap(json_object *obj, std::map<std::string, std::string> &map);

    std::vector<std::string> serializeMaps(std::map<std::string, std::map<std::string, std::string>> &maps);
};

#endif