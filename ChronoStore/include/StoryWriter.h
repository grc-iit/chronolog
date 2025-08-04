#ifndef STORY_WRITER_H
#define STORY_WRITER_H

#include <iostream>
#include <vector>
#include <map>
#include <event.h>
#include <json-c/json.h>
#include <StoryChunk.h>
#include <hdf5.h>

class StoryWriter
{
private:
    std::string chronicle_root_dir;

    template <typename T>
    json_object*convertToJsonObject(const T &value);

    template <typename... Args, size_t... Is>
    json_object*serializeTupleToJsonObject(const std::tuple <Args...> &tuple, std::index_sequence <Is...>);

    template <typename... Args>
    json_object*serializeTupleToJsonObject(const std::tuple <Args...> &tuple);

    hid_t
    writeStringAttribute(hid_t story_chunk_dset, const std::string &attribute_name, const std::string &attribute_value);

    hid_t
    writeUint64Attribute(hid_t story_chunk_dset, const std::string &attribute_name, const uint64_t &attribute_value);

public:
    StoryWriter() = default;

    explicit StoryWriter(std::string &chronicle_root_dir): chronicle_root_dir(chronicle_root_dir)
    {};

    ~StoryWriter() = default;

    int writeStoryChunks(const std::map <uint64_t, chronolog::StoryChunk> &story_chunk_map
                         , const std::string &chronicle_name, const std::string &story_name);

    static void serializeStoryChunk(json_object*obj, chronolog::StoryChunk &story_chunk);

    static std::vector <std::string>
    serializeStoryChunkMap(std::map <std::uint64_t, chronolog::StoryChunk> &story_chunk_map);
};

#endif