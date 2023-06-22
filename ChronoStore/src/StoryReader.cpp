#include <iostream>

#define H5_SIZEOF_SSIZE_T H5_SIZEOF_LONG_LONG

#include <hdf5.h>
#include <string>
#include <vector>
#include <filesystem>
#include <json-c/json.h>
#include <StoryReader.h>
#include <event.h>
#include <chunkattr.h>
#include <datasetreader.h>
#include <datasetminmax.h>
#include <log.h>
#include <errcode.h>

#define DATASET_RANK 1 // Dataset dimension
#define ATTRIBUTE_CHUNKMETADATA "ChunkMetadata"
#define ATTRIBUTE_DATASET_MIN "Min"
#define ATTRIBUTE_DATASET_MAX "Max"
#define ATTRIBUTE_TOTAL_DATASET_STORY_EVENTS "TotalStoryEvents"
#define DEBUG 0 // Set to 1 ito print H5 error messages to console

// Search first chunk to retrieve
int64_t searchFirstChunk(std::vector<ChunkAttr> *chunkAttributeData, uint64_t timestampToSearch, int64_t chunkEnd, int64_t chunkStart)
{
    int64_t currentChunk = INT_MIN;
    int64_t ansChunk = -1;
    if (chunkEnd == 0)
        return currentChunk;

    while (chunkStart <= chunkEnd)
    {
        currentChunk = chunkStart + (chunkEnd - chunkStart) / 2;
        if (chunkAttributeData->at(currentChunk).startTimeStamp == timestampToSearch)
        {
            return currentChunk;
        }
        else if (chunkAttributeData->at(currentChunk).startTimeStamp > timestampToSearch)
        {
            chunkEnd = currentChunk - 1;
        }
        else
        {
            chunkStart = currentChunk + 1;
            ansChunk = currentChunk;
        }
    }
    return ansChunk;
}

// Search last chunk to retrieve
int64_t searchLastChunk(std::vector<ChunkAttr> *chunkAttributeData, uint64_t timestampToSearch, int64_t chunkEnd, int64_t chunkStart)
{
    int64_t currentChunk = INT_MIN;
    if (chunkEnd == 0)
        return currentChunk;

    while (chunkStart <= chunkEnd)
    {
        currentChunk = chunkStart + (chunkEnd - chunkStart) / 2;
        if (chunkAttributeData->at(currentChunk).endTimeStamp == timestampToSearch)
        {
            return currentChunk;
        }
        else if (chunkAttributeData->at(currentChunk).endTimeStamp > timestampToSearch)
        {
            chunkEnd = currentChunk - 1;
        }
        else
        {
            chunkStart = currentChunk + 1;
        }
    }
    return chunkStart;
}

/**
 * Read data from story dataset
 * @param range: Range interval t1:t2 to read from.
 * @param STORY: story name
 * @param CHRONICLE: chronicle name
 * @return: struct DatasetReader ->{0,EventData} (EventData consist of the read values) if successful, else \n
 *          return {-1,EventData}   (Empty EventData) if failed.
 */
DatasetReader StoryReader::readFromDataset(std::pair<uint64_t, uint64_t> range, const char *STORY, const char *CHRONICLE)
{
    // Disable automatic printing of HDF5 error stack
    if (!DEBUG)
    {
        H5Eset_auto(H5E_DEFAULT, NULL, NULL);
    }
    std::vector<Event> resultVector;
    DatasetReader dr(-1, resultVector);
    hid_t chronicle = -1, story = -1, storyDatasetSpace = -1, eventType = -1, attributeId = -1, attributeType = -1, attributeDataSpace = -1, hyperslabMemorySpace = -1;
    herr_t status = -1, readChunkAttributeErr = -1, timeStampError = -1, dataError = -1, memSpaceErr = -1, datasetSpaceError = -1;
    DatasetMinMax dmm(-1, std::make_pair(0, 0));
    std::pair<int64_t, int64_t> chunksToRead = {INT_MIN, INT_MIN};
    hsize_t datasetHyperslabStart[1] = {0}, storyChunkDims[1] = {0}, chunkOffset[1] = {0}, attributeDims = 0;
    std::vector<Event> *eventBuffer;
    std::vector<ChunkAttr> *attributeDataBuffer;

    /**
     * Read data from the story
     */
    chronicle = H5Fopen(CHRONICLE, H5F_ACC_RDONLY, H5P_DEFAULT);
    story = H5Dopen2(chronicle, STORY, H5P_DEFAULT);
    storyDatasetSpace = H5Dget_space(story);

    if (chronicle < 0 || story < 0 || storyDatasetSpace < 0)
    {
        goto release_resources;
    }

    /**
     * Open the attribute chunkmetadata
     * Get the attribute type and storyDatasetSpace
     * Get the number of DATASET_RANK of the storyDatasetSpace
     * Get the story creation property list for chunksize information
     * Read the attribute data into the buffer
     */
    attributeId = H5Aopen(story, ATTRIBUTE_CHUNKMETADATA, H5P_DEFAULT);

    attributeType = H5Aget_type(attributeId);
    attributeDataSpace = H5Aget_space(attributeId);

    attributeDims = H5Sget_simple_extent_npoints(attributeDataSpace);

    attributeDataBuffer = new std::vector<ChunkAttr>(attributeDims);
    if (attributeDataBuffer == nullptr)
    {
        return dr;
    }

    readChunkAttributeErr = H5Aread(attributeId, attributeType, attributeDataBuffer->data());
    if (attributeId < 0 || attributeType < 0 || attributeDataSpace < 0 || readChunkAttributeErr < 0)
    {
        goto release_resources;
    }

    if (attributeType >= 0)
        H5Tclose(attributeType);
    if (attributeDataSpace >= 0)
        H5Sclose(attributeDataSpace);
    if (attributeId >= 0)
        H5Aclose(attributeId);

    dmm = readDatasetRange(STORY, CHRONICLE);
    if (dmm.status != 0 || dmm.MinMax.first > range.first || dmm.MinMax.second < range.second)
    {
        goto release_resources;
    }

    /*
     * Find the chunks to read
     */
    if (attributeDims == 1)
    {
        chunksToRead.first = 0;
        chunksToRead.second = 0;
    }
    else
    {
        chunksToRead.first = searchFirstChunk(attributeDataBuffer, range.first, attributeDims - 1, 0);
        if (chunksToRead.first != INT_MIN)
        {
            chunksToRead.second = searchLastChunk(attributeDataBuffer, range.second, attributeDims - 1, chunksToRead.first);
        }
    }

    // std::cout<<"Chunks to read: "<<chunksToRead.first<<" "<<chunksToRead.second<<std::endl;
    /*
     * Check for the range of requested query of chunks whether the chunkToRead is valid
     */
    if (chunksToRead.second == INT_MIN || chunksToRead.first == INT_MIN)
    {
        if (story >= 0)
            H5Dclose(story);
        if (chronicle >= 0)
            H5Fclose(chronicle);
        return dr;
    }

    /*
     *  Declare eventType
     */
    eventType = H5Tcreate(H5T_COMPOUND, sizeof(Event));
    timeStampError = H5Tinsert(eventType, "timeStamp", HOFFSET(Event, timeStamp), H5T_NATIVE_ULLONG);
    dataError = H5Tinsert(eventType, "data", HOFFSET(Event, data), H5T_NATIVE_CHAR);

    if (eventType < 0 || dataError < 0 || timeStampError < 0)
    {
        goto release_resources;
    }

    storyDatasetSpace = H5Dget_space(story);
    chunkOffset[0] = {0};
    try
    {
        /*
         * Create a memory storyDatasetSpace to hold the chunk data
         */
        for (int chunkIndex = chunksToRead.first; chunkIndex <= chunksToRead.second; chunkIndex++)
        {
            storyChunkDims[0] = {attributeDataBuffer->at(chunkIndex).totalChunkEvents};
            chunkOffset[0] = {attributeDataBuffer->at(chunkIndex).datasetIndex};

            hyperslabMemorySpace = H5Screate_simple(1, storyChunkDims, NULL);
            memSpaceErr = H5Sselect_hyperslab(hyperslabMemorySpace, H5S_SELECT_SET, datasetHyperslabStart, NULL, storyChunkDims, NULL);
            datasetSpaceError = H5Sselect_hyperslab(storyDatasetSpace, H5S_SELECT_SET, chunkOffset, NULL, storyChunkDims, NULL);

            eventBuffer = new std::vector<Event>(attributeDataBuffer->at(chunkIndex).totalChunkEvents);
            if (eventBuffer == nullptr)
            {
                goto release_resources;
            }

            status = H5Dread(story, eventType, hyperslabMemorySpace, storyDatasetSpace, H5P_DEFAULT, eventBuffer->data());
            if (status < 0 || datasetSpaceError < 0 || memSpaceErr < 0 || hyperslabMemorySpace < 0 || storyDatasetSpace < 0)
            {
                if (eventBuffer != nullptr)
                    delete eventBuffer;
                goto release_resources;
            }

            for (hsize_t i = 0; i < storyChunkDims[0]; i++)
            {
                if (range.first <= eventBuffer->at(i).timeStamp && eventBuffer->at(i).timeStamp <= range.second)
                {
                    dr.eventData.push_back(eventBuffer->at(i));
                }
            }
            delete (eventBuffer);
        }
    }
    catch (const std::bad_alloc &e)
    {
        goto release_resources;
    }

    /*
     * Release resources
     */
    if (attributeDataBuffer != nullptr)
        delete (attributeDataBuffer);
    if (storyDatasetSpace >= 0)
        H5Sclose(storyDatasetSpace);
    if (hyperslabMemorySpace >= 0)
        H5Sclose(hyperslabMemorySpace);
    if (eventType >= 0)
        H5Tclose(eventType);
    if (story >= 0)
        H5Dclose(story);
    if (chronicle >= 0)
        H5Fclose(chronicle);
    dr.status = 0;
    return dr;

// goto
release_resources:
    if (attributeDataBuffer != nullptr)
        delete (attributeDataBuffer);
    if (storyDatasetSpace >= 0)
        H5Sclose(storyDatasetSpace);
    if (hyperslabMemorySpace >= 0)
        H5Sclose(hyperslabMemorySpace);
    if (eventType >= 0)
        H5Tclose(eventType);
    if (story >= 0)
        H5Dclose(story);
    if (chronicle >= 0)
        H5Fclose(chronicle);
    dr.status = -1;
    dr.eventData.erase(dr.eventData.begin(), dr.eventData.end());
    return dr;
}

/**
 * Read story dataset Min and Max timeStamp
 * @param STORY: story name
 * @param CHRONICLE: chronicle name
 * @return: struct DatasetMinMax ->{0,MinMax} (MinMax is pair consisting of Min nd Max of dataset respectively) if successful, else \n
 *          return {-1,MinMax}   (MinMax empty) if failed.
 */
DatasetMinMax StoryReader::readDatasetRange(const char *STORY, const char *CHRONICLE)
{

    std::pair<uint64_t, uint64_t> datasetRange;
    DatasetMinMax d(-1, datasetRange);
    std::vector<uint64_t> DatasetReader(2);
    hid_t chronicle, story, attributeMinId, attributeMinDataSpace, attributeMaxId, attributeMaxDataSpace;
    herr_t statusErrorMin, statusErrorMax;
    uint64_t attribute_value;

    /*
     * Open H5 chronicle and story
     */
    chronicle = H5Fopen(CHRONICLE, H5F_ACC_RDONLY, H5P_DEFAULT);
    story = H5Dopen2(chronicle, STORY, H5P_DEFAULT);

    /*
     * Open and read Min attribute
     */
    attributeMinId = H5Aopen(story, ATTRIBUTE_DATASET_MIN, H5P_DEFAULT);
    attributeMinDataSpace = H5Aget_space(attributeMinId);
    statusErrorMin = H5Aread(attributeMinId, H5T_NATIVE_UINT64, &datasetRange.first);

    /*
     * Open and read Max attribute
     */
    attributeMaxId = H5Aopen(story, ATTRIBUTE_DATASET_MAX, H5P_DEFAULT);
    attributeMaxDataSpace = H5Aget_space(attributeMaxId);
    statusErrorMax = H5Aread(attributeMaxId, H5T_NATIVE_UINT64, &datasetRange.second);

    if (chronicle < 0 || story < 0 || attributeMinId < 0 || statusErrorMin < 0 || attributeMinDataSpace < 0 || attributeMaxId < 0 || attributeMaxDataSpace < 0 || statusErrorMax < 0)
    {
        if (attributeMinDataSpace >= 0)
            H5Sclose(attributeMinDataSpace);
        if (attributeMinId >= 0)
            H5Aclose(attributeMinId);
        if (attributeMaxDataSpace >= 0)
            H5Sclose(attributeMaxDataSpace);
        if (attributeMaxId >= 0)
            H5Aclose(attributeMaxId);
        if (story >= 0)
            H5Dclose(story);
        if (chronicle >= 0)
            H5Fclose(chronicle);
        return d;
    }

    d.MinMax.first = (datasetRange.first);
    d.MinMax.second = (datasetRange.second);
    d.status = 0;

    /*
     * Release resources
     */
    if (attributeMinDataSpace >= 0)
        H5Sclose(attributeMinDataSpace);
    if (attributeMinId >= 0)
        H5Aclose(attributeMinId);
    if (attributeMaxDataSpace >= 0)
        H5Sclose(attributeMaxDataSpace);
    if (attributeMaxId >= 0)
        H5Aclose(attributeMaxId);
    if (story >= 0)
        H5Dclose(story);
    if (chronicle >= 0)
        H5Fclose(chronicle);

    return d;
}

std::map<uint64_t, chronolog::StoryChunk> StoryReader::readAllStories(const std::string &chronicle_name)
{
    std::map<uint64_t, chronolog::StoryChunk> story_chunk_map;
    std::string chronicle_dir = std::string(chronicle_root_dir) + chronicle_name;
    for (const auto &entry : std::filesystem::directory_iterator(chronicle_dir))
    {
        // Skip directories and non-HDF5 files
        if (entry.is_directory() || !H5Fis_hdf5(entry.path().c_str()))
        {
            continue;
        }

        // Read each Story file
        readStory(chronicle_name, entry.path().string(), story_chunk_map);
    }

    return story_chunk_map;
}

int StoryReader::readStory(const std::string &chronicle_name, const std::string &story_file_name,
                           std::map<uint64_t, chronolog::StoryChunk> &story_chunk_map)
{
    // Open Story file
    hid_t story_file = H5Fopen(story_file_name.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT);
    if (story_file < 0)
    {
        LOGE("Failed to open story file: %s", story_file_name.c_str());
        return CL_ERR_UNKNOWN;
    }

    // Read all Story Chunks
    readAllStoryChunks(story_file, story_chunk_map);

    // Close Story file
    H5Fclose(story_file);

    return CL_SUCCESS;
}

int StoryReader::readAllStoryChunks(hid_t &story_file, std::map<uint64_t, chronolog::StoryChunk> &story_chunk_map)
{
    hid_t status;
    hsize_t num_story_chunks;

    // Get number of Story Chunks first
    H5Gget_num_objs(story_file, &num_story_chunks);
    // Iterate over all of them
    for (hsize_t i = 0; i < num_story_chunks; i++)
    {
        // Get story_chunk_dset name length first
        ssize_t story_chunk_dset_name_len = H5Lget_name_by_idx(story_file, ".", H5_INDEX_NAME, H5_ITER_INC, i,
                                                               nullptr, 0, H5P_DEFAULT) +
                                            1;
        char *story_chunk_dset_name = new char[story_chunk_dset_name_len];
        // Then read story_chunk_dset name
        H5Lget_name_by_idx(story_file, ".", H5_INDEX_NAME, H5_ITER_INC, i,
                           story_chunk_dset_name, story_chunk_dset_name_len, H5P_DEFAULT);
        // Open story_chunk_dset
        hid_t story_chunk_dset = H5Dopen(story_file, story_chunk_dset_name, H5P_DEFAULT);
        if (story_chunk_dset < 0)
        {
            LOGE("Failed to open story_chunk_dset: %s", story_chunk_dset_name);
            continue;
        }

        hid_t story_chunk_dspace = H5Dget_space(story_chunk_dset);
        int story_chunk_dset_rank = H5Sget_simple_extent_ndims(story_chunk_dspace);
        auto *story_chunk_dset_dims = new hsize_t[story_chunk_dset_rank];
        H5Sget_simple_extent_dims(story_chunk_dspace, story_chunk_dset_dims, nullptr);

        // Read the dataset
        char *story_chunk_dset_buf = new char[story_chunk_dset_dims[0]];
        memset(story_chunk_dset_buf, 0, story_chunk_dset_dims[0]);
        status = H5Dread(story_chunk_dset, H5T_NATIVE_B8, H5S_ALL, H5S_ALL, H5P_DEFAULT, story_chunk_dset_buf);
        if (status < 0)
        {
            LOGE("Failed to read story_chunk_dset: %s", story_chunk_dset_name);
            continue;
        }
        LOGD("read %ld bytes from story_chunk_dset: %s", strlen(story_chunk_dset_buf), story_chunk_dset_name);

        // Read story_id_str attribute
        std::string story_id_str = readAttribute(story_chunk_dset, "story_id");
        uint64_t story_id = std::stoull(story_id_str);

        // Read start_time attribute
        std::string start_time_str = readAttribute(story_chunk_dset, "start_time");
        uint64_t start_time = std::stoull(start_time_str);

        // Read end_time attribute
        std::string end_time_str = readAttribute(story_chunk_dset, "end_time");
        uint64_t end_time = std::stoull(end_time_str);

        // Convert Story Chunk bytes to map of Story Chunks
        chronolog::StoryChunk story_chunk = deserializeStoryChunk(story_chunk_dset_buf,
                                                                  story_id, start_time, end_time);
        story_chunk_map.emplace(story_chunk.getStartTime(), story_chunk);

        free(story_chunk_dset_buf);
        free(story_chunk_dset_dims);
        free(story_chunk_dset_name);

        H5Dclose(story_chunk_dset);
        H5Sclose(story_chunk_dspace);
    }

    return CL_SUCCESS;
}

std::string StoryReader::readAttribute(hid_t dataset_id, const std::string &attribute_name)
{
    hid_t attribute_id = H5Aopen(dataset_id, attribute_name.c_str(), H5P_DEFAULT);
    if (attribute_id < 0)
    {
        LOGE("Failed to open attribute: %s", attribute_name.c_str());
        return {};
    }
    std::string attribute_value;
    attribute_value.reserve(128);
    size_t attribute_value_len = 0;
    H5Aread(attribute_id, H5T_NATIVE_CHAR, attribute_value.data());
    H5Aclose(attribute_id);
    return attribute_value;
}

chronolog::StoryChunk StoryReader::deserializeStoryChunk(char *story_chunk_json_str,
                                                         uint64_t story_id, uint64_t start_time, uint64_t end_time)
{
    struct json_object *obj = json_tokener_parse(story_chunk_json_str);
    chronolog::StoryChunk story_chunk(story_id, start_time, end_time);
    enum json_type type = json_object_get_type(obj);
    if (type == json_type_object)
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

            LOGD("val: %s", json_object_get_string(val));
            chronolog::LogEvent event(story_id, event_time, client_id, index, json_object_get_string(val));
            story_chunk.insertEvent(event);
            LOGD("#Events: %ld", story_chunk.getNumEvents());
        }
    }
    else
    {
        LOGE("Failed to parse story_chunk_json_str: %s", story_chunk_json_str);
        exit(CL_ERR_UNKNOWN);
    }
    LOGD("#Events: %ld", story_chunk.getNumEvents());
    return story_chunk;
}
