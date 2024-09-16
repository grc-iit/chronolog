#include <iostream>

#define H5_SIZEOF_SSIZE_T H5_SIZEOF_LONG_LONG


#include <string>
#include <vector>
#include <filesystem>
#include <json-c/json.h>
#include <StoryReader.h>
#include <event.h>
#include <chunkattr.h>
#include <datasetreader.h>
#include <datasetminmax.h>
#include <chrono_monitor.h>
#include <chronolog_errcode.h>

#define DEBUG 0 // Set to 1 ito print H5 error messages to console

/**
 * Read all Stories in the Chronicle
 * @param chronicle_name Chronicle name to read
 * @return a map of StoryChunk objects sorted by its start_time
 */
std::map <uint64_t, chronolog::StoryChunk> StoryReader::readAllStories(const std::string &chronicle_name)
{
    std::map <uint64_t, chronolog::StoryChunk> story_chunk_map;
    std::string chronicle_dir = std::string(chronicle_root_dir) + chronicle_name;
    for(const auto &entry: std::filesystem::directory_iterator(chronicle_dir))
    {
        // Skip directories and non-HDF5 files
        if(entry.is_directory() || !H5Fis_hdf5(entry.path().c_str()))
        {
            continue;
        }

        // Read each Story file
        readStory(chronicle_name, entry.path().string(), story_chunk_map);
    }

    return story_chunk_map;
}

/**
 * Read all Story Chunks in the Story file
 * @param chronicle_name Chronicle name to read
 * @param story_file_name Story file name to read
 * @param story_chunk_map the map to store StoryChunk objects
 * @return CL_SUCCESS if successful, else error code
 */
int StoryReader::readStory(const std::string &chronicle_name, const std::string &story_file_name
                           , std::map <uint64_t, chronolog::StoryChunk> &story_chunk_map)
{
    // Open Story file
    hid_t story_file = H5Fopen(story_file_name.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT);
    if(story_file < 0)
    {
        LOG_ERROR("Failed to open story file: {}", story_file_name.c_str());
        return chronolog::CL_ERR_UNKNOWN;
    }

    // Read all Story Chunks
    readAllStoryChunks(story_file, story_chunk_map);

    // Close Story file
    H5Fclose(story_file);

    return chronolog::CL_SUCCESS;
}

/**
 * Read all Story Chunks in the Story file
 * @param story_file Story file to read
 * @param story_chunk_map the map to store StoryChunk objects
 * @return CL_SUCCESS if successful, else error code
 */
int StoryReader::readAllStoryChunks(hid_t &story_file, std::map <uint64_t, chronolog::StoryChunk> &story_chunk_map)
{
    hid_t status;
    hsize_t num_story_chunks;

    // Get number of Story Chunks first
    H5Gget_num_objs(story_file, &num_story_chunks);
    // Iterate over all of them
    for(hsize_t i = 0; i < num_story_chunks; i++)
    {
        // Get story_chunk_dset name length first
        ssize_t story_chunk_dset_name_len =
                H5Lget_name_by_idx(story_file, ".", H5_INDEX_NAME, H5_ITER_INC, i, nullptr, 0, H5P_DEFAULT) + 1;
        char*story_chunk_dset_name = new char[story_chunk_dset_name_len];

        // Then read story_chunk_dset name
        H5Lget_name_by_idx(story_file, ".", H5_INDEX_NAME, H5_ITER_INC, i, story_chunk_dset_name
                           , story_chunk_dset_name_len, H5P_DEFAULT);
        // Open story_chunk_dset
        chronolog::StoryChunk story_chunk = readOneStoryChunk(story_file, std::string(story_chunk_dset_name));

        // Add StoryChunk object to the map
        story_chunk_map.emplace(story_chunk.getStartTime(), story_chunk);
    }

    return chronolog::CL_SUCCESS;
}

/**
 * Read one Story Chunk from the Story file
 * @param story_file Story file to read
 * @param story_chunk_name Story Chunk name to read
 * @return a StoryChunk object
 */
chronolog::StoryChunk StoryReader::readOneStoryChunk(hid_t &story_file, const std::string &story_chunk_name)
{
    hid_t status;
    // Open story_chunk_dset
    hid_t story_chunk_dset = H5Dopen(story_file, story_chunk_name.c_str(), H5P_DEFAULT);
    if(story_chunk_dset < 0)
    {
        LOG_ERROR("Failed to open story_chunk_dset: {}", story_chunk_name.c_str());
        return {};
    }

    hid_t story_chunk_dspace = H5Dget_space(story_chunk_dset);
    int story_chunk_dset_rank = H5Sget_simple_extent_ndims(story_chunk_dspace);
    auto*story_chunk_dset_dims = new hsize_t[story_chunk_dset_rank];
    H5Sget_simple_extent_dims(story_chunk_dspace, story_chunk_dset_dims, nullptr);

    // Read the dataset
    char*story_chunk_dset_buf = new char[story_chunk_dset_dims[0]];
    memset(story_chunk_dset_buf, 0, story_chunk_dset_dims[0]);
    status = H5Dread(story_chunk_dset, H5T_NATIVE_B8, H5S_ALL, H5S_ALL, H5P_DEFAULT, story_chunk_dset_buf);
    if(status < 0)
    {
        LOG_ERROR("Failed to read story_chunk_dset: {}", story_chunk_name.c_str());
        return {};
    }
    LOG_DEBUG("read {} bytes from story_chunk_dset: {}", strlen(story_chunk_dset_buf), story_chunk_name.c_str());

    // Read chronicle_name attribute
    std::string chronicle_name = readStringAttribute(story_chunk_dset, "chronicle_name");

    // Read story_name attribute
    std::string story_name = readStringAttribute(story_chunk_dset, "story_name");

    // Read story_id_str attribute
    uint64_t story_id = readUint64Attribute(story_chunk_dset, "story_id");

    // Read start_time attribute
    uint64_t start_time = readUint64Attribute(story_chunk_dset, "start_time");

    // Read end_time attribute
    uint64_t end_time = readUint64Attribute(story_chunk_dset, "end_time");

    // Convert Story Chunk bytes to map of Story Chunks
    chronolog::StoryChunk story_chunk = deserializeStoryChunk(story_chunk_dset_buf, chronicle_name, story_name
                                                              , story_id, start_time, end_time);

    free(story_chunk_dset_buf);
    free(story_chunk_dset_dims);

    H5Dclose(story_chunk_dset);
    H5Sclose(story_chunk_dspace);

    return story_chunk;
}

/**
 * Read a string attribute from a Story Chunk dataset
 * @param story_chunk_dset Story Chunk dataset id
 * @param attribute_name attribute name to read
 * @return attribute value
 */
std::string StoryReader::readStringAttribute(hid_t &story_chunk_dset, const std::string &attribute_name) // NOLINT
// (*-convert-member-functions-to-static)
{
    hid_t attribute_id = H5Aopen(story_chunk_dset, attribute_name.c_str(), H5P_DEFAULT);
    if(attribute_id < 0)
    {
        LOG_ERROR("Failed to open attribute: {}", attribute_name.c_str());
        return {};
    }
    std::string attribute_value;
    attribute_value.reserve(128);
    size_t attribute_value_len = 0;
    H5Aread(attribute_id, H5T_NATIVE_CHAR, attribute_value.data());
    H5Aclose(attribute_id);
    return attribute_value;
}

/**
 * Read a uint64_t attribute from a Story Chunk dataset
 * @param story_chunk_dset Story Chunk dataset id
 * @param attribute_name attribute name to read
 * @return attribute value
 */
uint64_t StoryReader::readUint64Attribute(hid_t &story_chunk_dset
                                          , const std::string &attribute_name) // NOLINT(*-convert-member-functions-to-static)
{
    hid_t attribute_id = H5Aopen(story_chunk_dset, attribute_name.c_str(), H5P_DEFAULT);
    if(attribute_id < 0)
    {
        LOG_ERROR("Failed to open attribute: {}", attribute_name.c_str());
        return {};
    }
    uint64_t attribute_value;
    H5Aread(attribute_id, H5T_NATIVE_UINT64, &attribute_value);
    H5Aclose(attribute_id);
    return attribute_value;
}

/**
 * Deserialize a Story Chunk from a JSON string
 * @param story_chunk_json_str JSON string read from a Story Chunk dataset
 * @param chronicle_name Chronicle name
 * @param story_name Story name
 * @param story_id StoryID
 * @param start_time start time of the Story Chunk
 * @param end_time end time of the Story Chunk
 * @return a Story Chunk
 */
chronolog::StoryChunk
StoryReader::deserializeStoryChunk(char*story_chunk_json_str, std::string chronicle_name, std::string story_name
                                   , uint64_t story_id, uint64_t start_time, uint64_t end_time)
{
    struct json_object*obj = json_tokener_parse(story_chunk_json_str);
    chronolog::StoryChunk story_chunk(chronicle_name, story_name, story_id, start_time, end_time);
    enum json_type type = json_object_get_type(obj);
    if(type == json_type_object)
    {
        json_object_object_foreach(obj, key, val)
        {
            // get event_time, client_id, index from key
            // for example, get event_time = 1, client_id = 2, index = 3 from key = "[ 1, 2, 3 ]"
            std::string key_str = std::string(key);
            // Find the positions of the commas and brackets in the key
            size_t comma_pos1 = key_str.find(',');
            size_t comma_pos2 = key_str.find(',', comma_pos1 + 1);
            size_t closing_bracket_pos = key_str.find(']');

            // Extract the values between the commas and brackets
            uint64_t event_time = std::stoull(key_str.substr(2, comma_pos1 - 2));
            uint32_t client_id = std::stoul(key_str.substr(comma_pos1 + 2, comma_pos2 - comma_pos1 - 2));
            uint32_t index = std::stoul(key_str.substr(comma_pos2 + 2, closing_bracket_pos - comma_pos2 - 2));

            LOG_DEBUG("val: {}", json_object_get_string(val));
            chronolog::LogEvent event(story_id, event_time, client_id, index, json_object_get_string(val));
            story_chunk.insertEvent(event);
        }
    }
    else
    {
        LOG_ERROR("Failed to parse story_chunk_json_str: {}", story_chunk_json_str);
        exit(chronolog::CL_ERR_UNKNOWN);
    }
    return story_chunk;
}

/**
 * Read Events within a time range from a Story file
 * @param chronicle_name Chronicle name to read
 * @param story_id StoryID to read
 * @param start_time start time of the range
 * @param end_time end time of the range
 * @param story_chunk_map map of Story Chunks to be populated
 * @return CL_SUCCESS on success, else error code
 */
int StoryReader::readStoryRange(const std::string &chronicle_name, const uint64_t &story_id, uint64_t start_time
                                , uint64_t end_time, std::map <uint64_t, chronolog::StoryChunk> &story_chunk_map)
{
    std::string story_file_name = chronicle_root_dir + "/" + chronicle_name + "/" + std::to_string(story_id);
    // Open Story file
    hid_t story_file = H5Fopen(story_file_name.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT);
    if(story_file < 0)
    {
        LOG_ERROR("Failed to open story file: {}", story_file_name.c_str());
        return chronolog::CL_ERR_UNKNOWN;
    }
    // Get all overlapped Story Chunk names
    std::vector <std::string> story_chunk_names_overlap = findContiguousStoryChunks(story_file, start_time, end_time);
    // Read Story Chunks
    for(const auto &story_chunk_name: story_chunk_names_overlap)
    {
        chronolog::StoryChunk story_chunk = readOneStoryChunk(story_file, story_chunk_name);
        story_chunk_map.emplace(story_chunk.getStartTime(), story_chunk);
    }

    return chronolog::CL_SUCCESS;
}

/**
 * Find Story Chunks that overlap with a time rangend
 * @param story_file Story file id
 * @param start_time start time
 * @param end_time end time
 * @return a vector of Story Chunk names that overlap with the time range
 */
std::vector <std::string>
StoryReader::findContiguousStoryChunks(const hid_t &story_file, uint64_t start_time, uint64_t end_time)
{
    std::vector <std::string> story_chunk_names_cont_overlap;
    // Get all Story Chunk names
    std::vector <std::string> story_chunk_names_all = getAllStoryChunks(story_file);
    // Find Story Chunks that overlap with the time range
    for(const auto &story_chunk_name: story_chunk_names_all)
    {
        if(isStoryChunkOverlapping(story_chunk_name, start_time, end_time))
        {
            story_chunk_names_cont_overlap.push_back(story_chunk_name);
        }
    }
    return story_chunk_names_cont_overlap;
}

/**
 * Check if a Story Chunk overlaps with a time range
 * @param story_chunk_name Story Chunk name
 * @param start_time start time
 * @param end_time end time
 * @return true if the Story Chunk overlaps with the time range, else false
 */
bool StoryReader::isStoryChunkOverlapping(const std::string &story_chunk_name, uint64_t start_time, uint64_t end_time)
{
    uint64_t story_chunk_start_time = std::strtoll(
            story_chunk_name.substr(0, story_chunk_name.find_last_of('_')).c_str(), nullptr, 10);
    uint64_t story_chunk_end_time = std::strtoll(story_chunk_name.substr(story_chunk_name.find_last_of('_') + 1).c_str()
                                                 , nullptr, 10);
    return (story_chunk_start_time >= start_time || story_chunk_end_time < end_time);
}

/**
 * Get all Story Chunks in a Story file
 * @param story_file Story file to read
 * @return a vector of Story Chunk names
 */
std::vector <std::string>
StoryReader::getAllStoryChunks(const hid_t &story_file) // NOLINT(*-convert-member-functions-to-static)
{
    std::vector <std::string> story_chunk_names;
    // Get all Story Chunk dataset names
    H5G_info_t group_info;
    H5Gget_info(story_file, &group_info);
    for(int i = 0; i < group_info.nlinks; i++)
    {
        char group_name[256];
        H5Gget_objname_by_idx(story_file, i, group_name, 256);
        std::string group_name_str(group_name);
        if(group_name_str.find("story_chunk") != std::string::npos)
        {
            story_chunk_names.push_back(group_name_str);
        }
    }
    return story_chunk_names;
}
