#include <iostream>

//#define H5_SIZEOF_SSIZE_T H5_SIZEOF_LONG_LONG

#include <fstream>
#include <string>
#include <cstring>
#include <vector>
#include <map>
#include <unordered_map>
#include <filesystem>
#include <sys/stat.h>
#include <sys/types.h>
#include <event.h>
#include <chunkattr.h>
#include <StoryWriter.h>
#include <chrono_monitor.h>
#include <chronolog_errcode.h>

#define DATASET_RANK 1 // Dataset dimension
#define ATTRIBUTE_CHUNKMETADATA "ChunkMetadata"
#define ATTRIBUTE_DATASET_MIN "Min"
#define ATTRIBUTE_DATASET_MAX "Max"
#define ATTRIBUTE_TOTAL_DATASET_STORY_EVENTS "TotalStoryEvents"
#define ATTRIBUTE_CHUNKMETADATA_RANK 1
#define DEBUG 0 // Set to 1 to print H5 error messages to console
#define CHUNK_SIZE 40000 // Number of events equal to page size of 4MB

/**
 * @brief Write a string attribute to a storyDataset.
 * @param story_chunk_dset: dataset id
 * @param attribute_name: attribute name
 * @param attribute_value: attribute value
 * @return: 0 if successful, else errcode, if failed.
 */
hid_t StoryWriter::writeStringAttribute(hid_t story_chunk_dset, const std::string &attribute_name
                                        , const std::string &attribute_value)
{
    hsize_t attribute_dims[1] = {attribute_value.size()};
    hid_t attr_id = H5Screate(H5S_SIMPLE);
    hid_t status = H5Sset_extent_simple(attr_id, 1, attribute_dims, nullptr);
    hid_t attr = H5Acreate2(story_chunk_dset, attribute_name.c_str(), H5T_NATIVE_CHAR, attr_id, H5P_DEFAULT
                            , H5P_DEFAULT);
    status += H5Awrite(attr, H5T_NATIVE_CHAR, attribute_value.c_str());
    status += H5Aclose(attr);
    status += H5Sclose(attr_id);
    return status;
}

/**
 * @brief Write a uint64_t attribute to a storyDataset.
 * @param story_chunk_dset: dataset id
 * @param attribute_name: attribute name
 * @param attribute_value: attribute value
 * @return: 0 if successful, else errcode, if failed.
 */
hid_t StoryWriter::writeUint64Attribute(hid_t story_chunk_dset, const std::string &attribute_name
                                        , const uint64_t &attribute_value)
{
    hsize_t attribute_dims[1] = {1};
    hid_t attr_id = H5Screate(H5S_SIMPLE);
    hid_t status = H5Sset_extent_simple(attr_id, 1, attribute_dims, nullptr);
    hid_t attr = H5Acreate2(story_chunk_dset, attribute_name.c_str(), H5T_NATIVE_UINT64, attr_id, H5P_DEFAULT
                            , H5P_DEFAULT);
    status += H5Awrite(attr, H5T_NATIVE_UINT64, &attribute_value);
    status += H5Aclose(attr);
    status += H5Sclose(attr_id);
    return status;
}

/**
 * @brief Write/Append data to storyDataset.
 * @param story_chunk_map: an ordered_map of <EventSequence, StoryChunk> pairs.
 * @param chronicle_name: chronicle name
 * @param story_name: story name
 * @return: 0 if successful, else errcode, if failed.
 */
int StoryWriter::writeStoryChunks(const std::map <uint64_t, chronolog::StoryChunk> &story_chunk_map
                                  , const std::string &chronicle_name, const std::string &story_name)
{
    // Disable automatic printing of HDF5 error stack to console
    if(!DEBUG)
    {
        H5Eset_auto(H5E_DEFAULT, nullptr, nullptr);
    }

    // Iterate over storyChunks and logEvents to size of #events of each chunk and total size of #events of all chunks.
    // At the same time, serialize each chunk to a JSON string and store it in story_chunk_JSON_strs.
    hid_t status;
    hsize_t num_chunks = story_chunk_map.size();
    hsize_t total_num_events = 0;
    hsize_t total_chunk_size = 0;
    std::vector <hsize_t> num_events;
    std::vector <hsize_t> chunk_sizes;
    std::vector <std::string> story_chunk_JSON_strs;
    for(auto const &story_chunk_it: story_chunk_map)
    {
        hsize_t num_events_per_chunk = 0;
        hsize_t chunk_size = 0;
        json_object*story_chunk_JSON_obj = json_object_new_object();
        for(auto const &it: story_chunk_it.second)
        {
            num_events_per_chunk++;
            json_object*logRecordJSONObj = json_object_new_string(it.second.logRecord.c_str());
            json_object*sequenceTuple = serializeTupleToJsonObject(it.first);
            LOG_DEBUG("sequenceTuple: {}", json_object_to_json_string(sequenceTuple));
            LOG_DEBUG("logRecordJSONObj: {}", json_object_to_json_string(logRecordJSONObj));
            json_object_object_add(story_chunk_JSON_obj, json_object_get_string(sequenceTuple), logRecordJSONObj);
        }
        std::string story_chunk_JSON_str = json_object_to_json_string(story_chunk_JSON_obj);
        LOG_DEBUG("story_chunk_JSON_obj (size: {}): {}", story_chunk_JSON_str.size(), story_chunk_JSON_str.c_str());
        chunk_size = story_chunk_JSON_str.size();
        num_events.push_back(num_events_per_chunk);
        total_num_events += num_events_per_chunk;
        chunk_sizes.emplace_back(chunk_size);
        story_chunk_JSON_strs.emplace_back(story_chunk_JSON_str);
    }

    // Check if the directory for the Chronicle already exists
    struct stat st{};
    std::string chronicle_dir = chronicle_root_dir + chronicle_name;
    if(stat(chronicle_dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
    {
        // Create the directory for the Chronicle
        if(!std::filesystem::create_directory(chronicle_dir.c_str()))
        {
            LOG_ERROR("Failed to create chronicle directory: {}, errno: {}", chronicle_dir.c_str(), errno);
            return chronolog::CL_ERR_UNKNOWN;
        }
    }

    // Write each Story Chunk to a dataset in the Story file
    auto story_chunk_map_it = story_chunk_map.begin();
    auto num_event_it = num_events.begin();
    auto story_chunk_size_it = chunk_sizes.begin();
    auto story_chunk_json_str_it = story_chunk_JSON_strs.begin();
    std::unordered_map <uint64_t, hid_t> story_chunk_fd_map;
    for(; story_chunk_map_it !=
          story_chunk_map.end(); story_chunk_map_it++, num_event_it++, story_chunk_size_it++, story_chunk_json_str_it++)
    {
        // Check if the Story file has been opened/created already
        hid_t story_file;
        uint64_t story_id = story_chunk_map_it->second.getStoryId();
        std::string story_file_name = chronicle_dir + "/" + std::to_string(story_id);
        if(story_chunk_fd_map.find(story_id) != story_chunk_fd_map.end())
        {
            story_file = story_chunk_fd_map.find(story_id)->second;
            LOG_DEBUG("Story file already opened: {}", story_file_name.c_str());
        }
            // Story file does not exist, create the Story file if it does not exist
        else if(stat(story_file_name.c_str(), &st) != 0)
        {
            story_file = H5Fcreate(story_file_name.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
            if(story_file == H5I_INVALID_HID)
            {
                LOG_ERROR("Failed to create story file: {}", story_file_name.c_str());
                return chronolog::CL_ERR_UNKNOWN;
            }
            story_chunk_fd_map.emplace(story_id, story_file);
        }
        else
        {
            // Open the Story file if it exists
            story_file = H5Fopen(story_file_name.c_str(), H5F_ACC_RDWR, H5P_DEFAULT);
            if(story_file < 0)
            {
                LOG_ERROR("Failed to open story file: {}", story_file_name.c_str());
                return chronolog::CL_ERR_UNKNOWN;
            }
            story_chunk_fd_map.emplace(story_id, story_file);
        }

        // Check if the dataset for the Story Chunk exists
        hid_t story_chunk_dset, story_chunk_dspace;
        std::string story_chunk_dset_name = std::to_string(story_chunk_map_it->second.getStartTime()) + "_" +
                                            std::to_string(story_chunk_map_it->second.getEndTime());
        if(H5Lexists(story_file, story_chunk_dset_name.c_str(), H5P_DEFAULT) > 0)
        {
            LOG_ERROR("Story chunk already exists: {}", story_chunk_dset_name.c_str());
            return chronolog::CL_ERR_STORY_CHUNK_EXISTS;
        }

        // Create the dataspace for the dataset for the Story Chunk
        const hsize_t story_chunk_dset_dims[1] = {*story_chunk_size_it};
        const hsize_t story_chunk_dset_max_dims[1] = {H5S_UNLIMITED};
        story_chunk_dspace = H5Screate_simple(DATASET_RANK, story_chunk_dset_dims, nullptr);
        if(story_chunk_dspace < 0)
        {
            LOG_ERROR("Failed to create dataspace for story chunk: {}", story_chunk_dset_name.c_str());
            return chronolog::CL_ERR_UNKNOWN;
        }

        // Create the dataset for the Story Chunk
        story_chunk_dset = H5Dcreate2(story_file, story_chunk_dset_name.c_str(), H5T_NATIVE_B8, story_chunk_dspace
                                      , H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        if(story_chunk_dset < 0)
        {
            LOG_ERROR("Failed to create dataset for story chunk: {}", story_chunk_dset_name.c_str());
            // Print the detailed error message
            H5Eprint(H5E_DEFAULT, stderr);

            // Get the error stack and retrieve the error message
            H5Ewalk2(H5E_DEFAULT, H5E_WALK_DOWNWARD
                     , [](unsigned n, const H5E_error2_t*err_desc, void*client_data) -> herr_t
                    {
                        printf("Error #%u: %s\n", n, err_desc->desc);
                        return 0;
                    }, nullptr);
            return chronolog::CL_ERR_UNKNOWN;
        }

        // Open the dataset for the Story Chunk
//        story_chunk_dset = H5Dopen2(story_file, story_chunk_dset_name.c_str(), H5P_DEFAULT);
//        if (story_chunk_dset < 0)
//        {
//            LOG_ERROR("Failed to open dataset for story chunk: {}", story_chunk_dset_name.c_str());
//            return chronolog::CL_ERR_UNKNOWN;
//        }

        // Write the Story Chunk to the dataset
        status = H5Dwrite(story_chunk_dset, H5T_NATIVE_B8, H5S_ALL, H5S_ALL, H5P_DEFAULT
                          , story_chunk_json_str_it->c_str());
        if(status < 0)
        {
            LOG_ERROR("Failed to write story chunk to dataset: {}", story_chunk_dset_name.c_str());
            return chronolog::CL_ERR_UNKNOWN;
        }

        // Write chronicle_name attribute to the dataset
        status = writeStringAttribute(story_chunk_dset, "chronicle_name", chronicle_name);

        // Write story_name attribute to the dataset
        status = writeStringAttribute(story_chunk_dset, "story_name", story_name);

        // Write story_id attribute to the dataset
        status = writeUint64Attribute(story_chunk_dset, "story_id", story_id);

        // Write num_events attribute to the dataset
        status = writeUint64Attribute(story_chunk_dset, "num_events", *num_event_it);

        // Write start_time attribute to the dataset
        status = writeUint64Attribute(story_chunk_dset, "start_time", story_chunk_map_it->second.getStartTime());

        // Write end_time attribute to the dataset
        status = writeUint64Attribute(story_chunk_dset, "end_time", story_chunk_map_it->second.getEndTime());

        // Close dataset and data space
        status = H5Dclose(story_chunk_dset);
        status += H5Sclose(story_chunk_dspace);
        if(status < 0)
        {
            LOG_ERROR("Failed to close dataset or dataspace or file for story chunk: {}", story_chunk_dset_name.c_str());
            return chronolog::CL_ERR_UNKNOWN;
        }
    }

    // Close all files at the end
    for(auto &story_chunk_fd_map_it: story_chunk_fd_map)
    {
        status = H5Fclose(story_chunk_fd_map_it.second);
        if(status < 0)
        {
            char*file_name = new char[128];
            size_t file_name_len = 0;
            H5Fget_name(story_chunk_fd_map_it.second, file_name, file_name_len);
            LOG_ERROR("Failed to close file for story chunk: {}", file_name);
            free(file_name);
            return chronolog::CL_ERR_UNKNOWN;
        }
    }

    return chronolog::CL_SUCCESS;
}

/**
 * @brief Serialize a map to JSON object
 * @param obj: json_object to attach the Story Chunk to
 * @param story_chunk: Story Chunk to serialize
 */
void StoryWriter::serializeStoryChunk(json_object*obj, chronolog::StoryChunk &story_chunk)
{
    std::string story_chunk_name =
            std::to_string(story_chunk.getStartTime()) + "_" + std::to_string(story_chunk.getEndTime());
    std::stringstream ss;
    ss << "StoryChunk:{" << story_chunk.getStoryId() << ":"
    << story_chunk.getStartTime() << ":" << story_chunk.getEndTime() << "} has "
    << std::distance(story_chunk.begin(), story_chunk.end()) << " events: ";
    for(const auto & iter : story_chunk)
    {
        ss << "<" << std::get <0>(iter.first) << ", " << std::get <1>(iter.first) << ", "
           << std::get <2>(iter.first) << ">: " << iter.second.toString();
    }
    json_object_object_add(obj, story_chunk_name.c_str(), json_object_new_string(ss.str().c_str()));
}

/**
 * @brief Serialize a map of <start_time, StoryChunk> to a vector of JSON strings
 * @param story_chunk_map: map of <start_time, StoryChunk> to serialize
 * @return a vector of JSON strings, each of which is a serialized JSON string of a Story Chunk
 */
std::vector <std::string>
StoryWriter::serializeStoryChunkMap(std::map <std::uint64_t, chronolog::StoryChunk> &story_chunk_map)
{
    std::vector <std::string> strings;
    for(auto &map: story_chunk_map)
    {
        json_object*obj = json_object_new_object();
        serializeStoryChunk(obj, map.second);
        const char*json = json_object_to_json_string(obj);
        strings.emplace_back(json);
    }
    return strings;
}

// Convert a single element to a json_object
template <typename T>
json_object*StoryWriter::convertToJsonObject(const T &value)
{
    json_object*obj = nullptr;

    if constexpr(std::is_same_v <T, int>)
    {
        obj = json_object_new_int(value);
    }
    else if constexpr(std::is_same_v <T, double>)
    {
        obj = json_object_new_double(value);
    }
    else if constexpr(std::is_same_v <T, std::string>)
    {
        obj = json_object_new_string(value.c_str());
    }
    else if constexpr(std::is_same_v <T, uint64_t>)
    {
        obj = json_object_new_uint64(value);
    }
    else if constexpr(std::is_same_v <T, uint32_t>)
    {
        // TODO: make sure this is correct
        obj = json_object_new_uint64(value);
    }

    return obj;
}

// Serialize the std::tuple to a json_object
template <typename... Args, size_t... Is>
json_object*StoryWriter::serializeTupleToJsonObject(const std::tuple <Args...> &tuple, std::index_sequence <Is...>)
{
    json_object*obj = json_object_new_array();

    // Convert each element of the std::tuple to a json_object and add it to the array
    (json_object_array_add(obj, convertToJsonObject(std::get <Is>(tuple))), ...);

    return obj;
}

// Helper function to serialize the std::tuple to a json_object
template <typename... Args>
json_object*StoryWriter::serializeTupleToJsonObject(const std::tuple <Args...> &tuple)
{
    return serializeTupleToJsonObject(tuple, std::make_index_sequence <sizeof...(Args)>{});
}
