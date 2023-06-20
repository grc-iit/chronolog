#include <iostream>

//#define H5_SIZEOF_SSIZE_T H5_SIZEOF_LONG_LONG

#include <hdf5.h>
#include <fstream>
#include <string>
#include <cstring>
#include <vector>
#include <map>
#include <unordered_map>
#include <sys/stat.h>
#include <sys/types.h>
#include <event.h>
#include <chunkattr.h>
#include <storywriter.h>
#include <log.h>
#include <errcode.h>

#define DATASET_RANK 1 // Dataset dimension
#define ATTRIBUTE_CHUNKMETADATA "ChunkMetadata"
#define ATTRIBUTE_DATASET_MIN "Min"
#define ATTRIBUTE_DATASET_MAX "Max"
#define ATTRIBUTE_TOTAL_DATASET_STORY_EVENTS "TotalStoryEvents"
#define ATTRIBUTE_CHUNKMETADATA_RANK 1
#define DEBUG 0 // Set to 1 to print H5 error messages to console
#define CHUNK_SIZE 40000 // Number of events equal to page size of 4MB

// Constructor storywriter
storywriter::storywriter()= default;

// Destructor storywriter
storywriter::~storywriter()= default;

/**
 * @brief Write an attribute to a storyDataset.
 * @param story_chunk_dset: dataset id
 * @param attribute_name: attribute name
 * @param attribute_value: attribute value
 * @return: 0 if successful, else errcode, if failed.
 */
hid_t storywriter::writeAttribute(hid_t story_chunk_dset,
                                  const std::string &attribute_name,
                                  const std::string &attribute_value)
{
    hsize_t num_event_size_dims[1] = {attribute_value.size()};
    hid_t attr_id = H5Screate(H5S_SIMPLE);
    hid_t status = H5Sset_extent_simple(attr_id, 1, num_event_size_dims, nullptr);
    hid_t num_event_attr = H5Acreate2(story_chunk_dset, "num_events", H5T_NATIVE_LLONG,
                                      attr_id, H5P_DEFAULT, H5P_DEFAULT);
    status += H5Awrite(num_event_attr, H5T_NATIVE_FLOAT, attribute_value.c_str());
    status += H5Aclose(num_event_attr);
    status += H5Sclose(attr_id);
    return status;
}

/**
 * @brief Write/Append data to storyDataset.
 * @param story_chunk_map: an ordered_map of <EventSequence, LogEvent> pairs.
 * @param chronicle_name: chronicle name
 * @return: 0 if successful, else errcode, if failed.
 */
int storywriter::writeStoryChunks(const std::map<uint64_t, chronolog::StoryChunk> &story_chunk_map,
                                  const std::string &chronicle_name)
{
    // Disable automatic printing of HDF5 error stack to console
    if (! DEBUG)
    {
        H5Eset_auto(H5E_DEFAULT, nullptr, nullptr);
    }

    // Iterate over storyChunks and logEvents to size of #events of each chunk and total size of #events of all chunks.
    // At the same time, serialize each chunk to a JSON string and store it in story_chunk_JSON_strs.
    hid_t status;
    hsize_t num_chunks = story_chunk_map.size();
    hsize_t total_num_events = 0;
    hsize_t total_chunk_size = 0;
    std::vector<hsize_t> num_events;
    std::vector<hsize_t> chunk_sizes;
    std::vector<std::string> story_chunk_JSON_strs;
    for (auto const &story_chunk_it : story_chunk_map)
    {
        hsize_t numb_events = 0;
        hsize_t chunk_size = 0;
        json_object *story_chunk_JSON_obj = json_object_new_object();
        for (auto const &it : story_chunk_it.second)
        {
            numb_events++;
            chunk_size += it.second.logRecord.size();
            json_object *logRecordJSONObj = json_object_new_string(it.second.logRecord.c_str());
            json_object *sequenceTuple = serializeTupleToJsonObject(it.first);
            json_object_object_add(story_chunk_JSON_obj, json_object_get_string(sequenceTuple), logRecordJSONObj);
        }
        num_events.push_back(numb_events);
        total_num_events += numb_events;
        chunk_sizes.emplace_back(chunk_size);
        story_chunk_JSON_strs.emplace_back(json_object_to_json_string(story_chunk_JSON_obj));
    }

    // Check if the directory for the Chronicle already exists
    struct stat st{};
    std::string chronicle_dir = CHRONICLE_ROOT_DIR + chronicle_name;
    if (stat(chronicle_dir.c_str(), &st) != 0 || ! S_ISDIR(st.st_mode))
    {
        // Create the directory for the Chronicle
        if (mkdir(chronicle_dir.c_str(), 0775) < 0)
        {
            LOGE("Failed to create chronicle directory: %s, errno: %d", chronicle_dir.c_str(), errno);
            return CL_ERR_UNKNOWN;
        }
    }

    // Write each Story Chunk to a dataset in the Story file
    auto story_chunk_map_it = story_chunk_map.begin();
    auto num_event_it = num_events.begin();
    auto story_chunk_size_it = chunk_sizes.begin();
    auto story_chunk_json_str_it = story_chunk_JSON_strs.begin();
    std::unordered_map<uint64_t, hid_t> story_chunk_fd_map;
    for (;
         story_chunk_map_it != story_chunk_map.end();
         story_chunk_map_it++, num_event_it++, story_chunk_size_it++, story_chunk_json_str_it++)
    {
        // Check if the Story file has been opened/created already
        hid_t story_file;
        uint64_t story_id = story_chunk_map_it->second.getStoryID();
        std::string story_file_name = chronicle_dir + "/" + std::to_string(story_id);
        if (story_chunk_fd_map.find(story_id) != story_chunk_fd_map.end())
        {
            story_file = story_chunk_fd_map.find(story_id)->second;
            LOGD("Story file already opened: %s", story_file_name.c_str());
        }
        // Story file does not exist, create the Story file if it does not exist
        else if (stat(story_file_name.c_str(), &st) != 0)
        {
            story_file = H5Fcreate(story_file_name.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
            if (story_file == H5I_INVALID_HID)
            {
                LOGE("Failed to create story file: %s", story_file_name.c_str());
                return CL_ERR_UNKNOWN;
            }
            story_chunk_fd_map.emplace(story_id, story_file);
        }
        else
        {
            // Open the Story file if it exists
            story_file = H5Fopen(story_file_name.c_str(), H5F_ACC_RDWR, H5P_DEFAULT);
            if (story_file < 0)
            {
                LOGE("Failed to open story file: %s", story_file_name.c_str());
                return CL_ERR_UNKNOWN;
            }
            story_chunk_fd_map.emplace(story_id, story_file);
        }

        // Check if the dataset for the Story Chunk exists
        hid_t story_chunk_dset, story_chunk_dspace;
        std::string story_chunk_dset_name = std::to_string(story_chunk_map_it->second.getStartTime());
        if (H5Lexists(story_file, story_chunk_dset_name.c_str(), H5P_DEFAULT) > 0)
        {
            LOGE("Story chunk already exists: %s", story_chunk_dset_name.c_str());
            return CL_ERR_STORY_CHUNK_EXISTS;
        }

        // Create the dataspace for the dataset for the Story Chunk
        const hsize_t story_chunk_dset_dims[1] = {*story_chunk_size_it};
        const hsize_t story_chunk_dset_max_dims[1] = {H5S_UNLIMITED};
        story_chunk_dspace = H5Screate_simple(DATASET_RANK, story_chunk_dset_dims, nullptr);
        if (story_chunk_dspace < 0)
        {
            LOGE("Failed to create dataspace for story chunk: %s", story_chunk_dset_name.c_str());
            return CL_ERR_UNKNOWN;
        }

        // Create the dataset for the Story Chunk
        story_chunk_dset = H5Dcreate2(story_file, story_chunk_dset_name.c_str(), H5T_NATIVE_B8, story_chunk_dspace,
                                      H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        if (story_chunk_dset < 0)
        {
            LOGE("Failed to create dataset for story chunk: %s", story_chunk_dset_name.c_str());
            // Print the detailed error message
            H5Eprint(H5E_DEFAULT, stderr);

            // Get the error stack and retrieve the error message
            H5Ewalk2(H5E_DEFAULT, H5E_WALK_DOWNWARD, [](unsigned n,
                    const H5E_error2_t *err_desc,
                    void *client_data) -> herr_t {
                printf("Error #%u: %s\n", n, err_desc->desc);
                return 0;
            }, NULL);
            return CL_ERR_UNKNOWN;
        }

        // Open the dataset for the Story Chunk
//        story_chunk_dset = H5Dopen2(story_file, story_chunk_dset_name.c_str(), H5P_DEFAULT);
//        if (story_chunk_dset < 0)
//        {
//            LOGE("Failed to open dataset for story chunk: %s", story_chunk_dset_name.c_str());
//            return CL_ERR_UNKNOWN;
//        }

        // Write the Story Chunk to the dataset
        status = H5Dwrite(story_chunk_dset, H5T_NATIVE_B8, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                                story_chunk_json_str_it->c_str());
        if (status < 0)
        {
            LOGE("Failed to write story chunk to dataset: %s", story_chunk_dset_name.c_str());
            return CL_ERR_UNKNOWN;
        }

        // Write storyId attribute to the dataset
        status = writeAttribute(story_chunk_dset, "story_id", std::to_string(story_id));

        // Write num_events attribute to the dataset
        status = writeAttribute(story_chunk_dset, "num_events", std::to_string(*num_event_it));

        // Write end_time attribute to the dataset
        status = writeAttribute(story_chunk_dset, "end_time", std::to_string(story_chunk_map_it->second.getEndTime()));

        // Close dataset and data space
        status = H5Dclose(story_chunk_dset);
        status += H5Sclose(story_chunk_dspace);
        if (status < 0)
        {
            LOGE("Failed to close dataset or dataspace or file for story chunk: %s", story_chunk_dset_name.c_str());
            return CL_ERR_UNKNOWN;
        }
    }

    // Close all files at the end
    for (auto &story_chunk_fd_map_it : story_chunk_fd_map)
    {
        status = H5Fclose(story_chunk_fd_map_it.second);
        if (status < 0)
        {
            char *file_name = new char[128];
            size_t file_name_len = 0;
            H5Fget_name(story_chunk_fd_map_it.second, file_name, file_name_len);
            LOGE("Failed to close file for story chunk: %s", file_name);
            free(file_name);
            return CL_ERR_UNKNOWN;
        }
    }

    return CL_SUCCESS;
}

/**
 * Write/Append data to storyDataset. 
 * @param storyChunk: Data to write to story dataset.
 * @param STORY: story name
 * @param CHRONICLE: chronicle name
 * @return: 0 if successful, else -1, if failed.
 */
int storywriter::writeStoryChunk(std::vector<Event> *storyChunk, const char* STORY, const char* CHRONICLE) {

    /*
     * Disable automatic printing of HDF5 error stack to console
     */
    if(!DEBUG){
        H5Eset_auto(H5E_DEFAULT, NULL, NULL);
    }

    hid_t   ChronicleH5 = -1, storyDataset = -1, storyDatasetSpace = -1, chunkPropId = -1, hyperslabMemorySpace = -1, eventType = -1, chunkmetaAttribute = -1, attr = -1, attr1 = -1, attr_DatasetMin = -1, attr_DatasetMax = -1, attr_DatasetTotalStoryEvents = -1;
    hid_t   attributeId = -1, attributeType = -1, attributeDataSpace = -1;
    herr_t  status = -1, ret = -1;
    hsize_t attributeCount = 0;
    hsize_t chunksize = CHUNK_SIZE;
    hsize_t datasetHyperslabStart[1] = {0};
    hsize_t storySpaceStart[1] = {0};
    std::vector<ChunkAttr> *attributeDataBuffer, *extendChunkMetadataBuffer;

    /*
     * Chunk Dimension and number of events in a story chunk
     */
    hsize_t chunkDims[1] = {storyChunk->size()};
    const hsize_t numberOfEvents = storyChunk->size() - 1;

    /*
    * Allocate memory for attribute buffer
    */
    attributeDataBuffer = new std::vector<ChunkAttr>((numberOfEvents / CHUNK_SIZE) + 1);
    if(attributeDataBuffer == nullptr){
        return -1;
    }

    /*
    * Dimensions for the storyDataset
    */
    const hsize_t storyChunkDims[] = {storyChunk->size() - 1};

    /*
    * User defined compound datatype - consists "timeStamp" and "data"
    */
    herr_t timeStampError, dataError;
    eventType = H5Tcreate(H5T_COMPOUND, sizeof(Event));
    timeStampError = H5Tinsert(eventType, "timeStamp", HOFFSET(Event, timeStamp), H5T_NATIVE_ULONG);
    dataError = H5Tinsert(eventType, "data", HOFFSET(Event, data), H5T_NATIVE_CHAR);
    if(eventType < 0 || dataError < 0 || timeStampError < 0){
        if(eventType >=0 )  H5Tclose(eventType);
        return -1;
    }

    hsize_t storyDatasetSpaceStart = 0;
    hsize_t newStorySize[1];

    /*
    * Check if the ChronicleH5 already exists
    */
    if (H5Fis_hdf5(CHRONICLE) > 0)
    {
        /*
        * Open ChronicleH5 and check if the storyDataset exists
        */
        ChronicleH5 = H5Fopen(CHRONICLE, H5F_ACC_RDWR, H5P_DEFAULT);
        if(ChronicleH5 < 0){
            if(eventType >=0 )  H5Tclose(eventType);
            return -1;
        }
        if(H5Lexists(ChronicleH5, STORY, H5P_DEFAULT) > 0){
            /*
            * Open storyDataset
            */

            storyDataset = H5Dopen2(ChronicleH5, STORY, H5P_DEFAULT);

            /*
            * Get the existing size of the storyDataset
            */
            attributeId = H5Aopen(storyDataset, ATTRIBUTE_TOTAL_DATASET_STORY_EVENTS, H5P_DEFAULT);
            attributeType = H5Aget_type(attributeId);
            attributeDataSpace = H5Aget_space(attributeId);

            hsize_t totalStoryEvents;
            status = H5Aread(attributeId, attributeType, &totalStoryEvents);

            if(status < 0 || attributeDataSpace < 0 || attributeId < 0 || attributeType < 0 || storyDataset < 0 || ChronicleH5 < 0){
                goto release_resources;
            }

            if(attributeId >= 0)  H5Aclose(attributeId);
            if(attributeDataSpace >= 0)  H5Sclose(attributeDataSpace);
            if(attributeType >= 0)  H5Tclose(attributeType);

            /*
            * Define the new size of the storyDataset and extend it
            */
            storyDatasetSpaceStart = totalStoryEvents;
            newStorySize[0] = {totalStoryEvents + storyChunkDims[0]};
            status = H5Dset_extent(storyDataset, newStorySize);
            if(status < 0 ){
                goto release_resources;
            }

            /* 
            * Commit new size of the storyDataset. Close the storyDataset
            */
            H5Dclose(storyDataset);

            /* 
            * Open storyDataset and get extended dataspace
            */
            storyDataset = H5Dopen2(ChronicleH5, STORY, H5P_DEFAULT);
            storyDatasetSpace = H5Dget_space(storyDataset);
            if(storyDatasetSpace < 0 || storyDataset < 0){
                goto release_resources;
            }
        }
        /*
        * Create the storyDataset
        */
        else{

            /*
            * Enable chunking for the storyDataset
            */
            chunkPropId = H5Pcreate(H5P_DATASET_CREATE);
            status = H5Pset_chunk(chunkPropId, 1, chunkDims);

            /*
            * Create the storyDataset.
            */
            const hsize_t datasetMaxDims[] = {H5S_UNLIMITED};
            storyDatasetSpace = H5Screate_simple(DATASET_RANK, storyChunkDims, datasetMaxDims );
            if(storyDatasetSpace < 0 || ChronicleH5 < 0 || status < 0 || chunkPropId < 0){
                goto release_resources;
            }
            storyDataset = H5Dcreate2(ChronicleH5, STORY, eventType, storyDatasetSpace, H5P_DEFAULT, chunkPropId, H5P_DEFAULT);
            if(storyDataset < 0){
                goto release_resources;
            }
        }
    }
    /*
    * Create the ChronicleH5 and storyDataset
    */
    else
    {
        ChronicleH5 = H5Fcreate(CHRONICLE, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
        /*
        * Enable chunking for the storyDataset
        */
        chunkPropId = H5Pcreate(H5P_DATASET_CREATE);
        status = H5Pset_chunk(chunkPropId, 1, chunkDims);

        /*
        * Create the storyDataset.
        */
        const hsize_t datasetMaxDims[] = {H5S_UNLIMITED};
        storyDatasetSpace = H5Screate_simple(DATASET_RANK, storyChunkDims, datasetMaxDims );
        if(storyDatasetSpace < 0 || ChronicleH5 < 0 || status < 0 || chunkPropId < 0){
            goto release_resources;
        }

        storyDataset = H5Dcreate2(ChronicleH5, STORY, eventType, storyDatasetSpace, H5P_DEFAULT, chunkPropId, H5P_DEFAULT);
        if(storyDataset < 0){
            goto release_resources;
        }
    }

    /*
    * Write data to the storyDataset
    */
    storySpaceStart[0] = storyDatasetSpaceStart;
    hyperslabMemorySpace = H5Screate_simple(DATASET_RANK, storyChunkDims, NULL);

    herr_t memSpaceErr, datasetSpaceError;
    memSpaceErr = H5Sselect_hyperslab(hyperslabMemorySpace, H5S_SELECT_SET, datasetHyperslabStart, NULL, storyChunkDims, NULL);
    datasetSpaceError = H5Sselect_hyperslab(storyDatasetSpace, H5S_SELECT_SET, storySpaceStart, NULL, storyChunkDims, NULL);
    if(memSpaceErr < 0 || datasetSpaceError < 0 || hyperslabMemorySpace < 0){
        goto release_resources;
    }

    status = H5Dwrite(storyDataset, eventType, hyperslabMemorySpace, storyDatasetSpace, H5P_DEFAULT, storyChunk->data());
    if(status < 0){
        goto release_resources;
    }

    H5Sclose(hyperslabMemorySpace);
    H5Sclose(storyDatasetSpace);

    for (hsize_t eventIndex = 0; eventIndex < storyChunkDims[0]; eventIndex += CHUNK_SIZE) {
        if(storyChunkDims[0] - eventIndex < CHUNK_SIZE){
            chunksize = storyChunkDims[0] - eventIndex;
        }
        attributeDataBuffer->at(attributeCount).startTimeStamp = storyChunk->at(eventIndex).timeStamp;
        attributeDataBuffer->at(attributeCount).endTimeStamp = storyChunk->at(eventIndex + chunksize - 1).timeStamp;
        attributeDataBuffer->at(attributeCount).datasetIndex = eventIndex;
        attributeDataBuffer->at(attributeCount).totalChunkEvents = chunksize;
        attributeCount++;
    }

    /*
    * Create datatype for the ChunkMetadata attribute.
    */
    herr_t startError, endError,datasetIndexError , totalChunkEventsError, attrMaxError, attrStoryEventsError, attrMinError, attrError;
    chunkmetaAttribute = H5Tcreate(H5T_COMPOUND, sizeof(ChunkAttr));
    startError = H5Tinsert(chunkmetaAttribute, "startTimeStamp", HOFFSET(ChunkAttr, startTimeStamp), H5T_NATIVE_ULONG);
    endError = H5Tinsert(chunkmetaAttribute, "endTimeStamp", HOFFSET(ChunkAttr, endTimeStamp), H5T_NATIVE_ULONG);
    datasetIndexError = H5Tinsert(chunkmetaAttribute, "datasetIndex", HOFFSET(ChunkAttr, datasetIndex), H5T_NATIVE_UINT);
    totalChunkEventsError = H5Tinsert(chunkmetaAttribute, "totalChunkEvents", HOFFSET(ChunkAttr, totalChunkEvents), H5T_NATIVE_UINT);
    if(chunkmetaAttribute < 0 || endError < 0 || startError < 0 || totalChunkEventsError < 0 || datasetIndexError < 0){
        goto release_resources;
    }

    if(H5Aexists_by_name(storyDataset, ".", ATTRIBUTE_CHUNKMETADATA, H5P_DEFAULT) > 0){
        /*
        * Open the attribute, get attribute id, type and space
        */
        attributeId = H5Aopen(storyDataset, ATTRIBUTE_CHUNKMETADATA, H5P_DEFAULT);
        attributeType = H5Aget_type(attributeId);
        attributeDataSpace = H5Aget_space(attributeId);
        hsize_t attributeChunkMetadataDims = H5Sget_simple_extent_npoints(attributeDataSpace);
        H5Sclose(attributeDataSpace);

        hsize_t attributeDims[] = {attributeChunkMetadataDims + attributeCount};
        extendChunkMetadataBuffer = new std::vector<ChunkAttr>(attributeDims[0]);
        if(extendChunkMetadataBuffer == nullptr){
            goto release_resources;
        }
        /*
        * Read the attribute data into the buffer and delete the attribute
        */
        status = H5Aread(attributeId, attributeType, extendChunkMetadataBuffer->data());
        if(status < 0 || attributeId < 0 || attributeType < 0 || attributeChunkMetadataDims == HSIZE_UNDEF){
            goto release_resources;
        }

        if(attributeId >= 0)    H5Aclose(attributeId);
        herr_t deleteError = H5Adelete(storyDataset, ATTRIBUTE_CHUNKMETADATA);

        attr = H5Screate(H5S_SIMPLE);
        ret  = H5Sset_extent_simple(attr, ATTRIBUTE_CHUNKMETADATA_RANK, attributeDims, NULL);

        attr1 = H5Acreate2(storyDataset, ATTRIBUTE_CHUNKMETADATA, attributeType, attr, H5P_DEFAULT, H5P_DEFAULT);

        if(deleteError < 0 || attr < 0 || ret < 0 || attr1 < 0){
            goto release_resources;
        }

        int lastEventIndex = (*extendChunkMetadataBuffer)[attributeChunkMetadataDims - 1].datasetIndex + (*extendChunkMetadataBuffer)[attributeChunkMetadataDims - 1].totalChunkEvents;
        for(auto index = 0 ; index < attributeCount ; index++ ){
            extendChunkMetadataBuffer->at(attributeChunkMetadataDims + index).startTimeStamp = attributeDataBuffer->at(index).startTimeStamp;
            extendChunkMetadataBuffer->at(attributeChunkMetadataDims + index).endTimeStamp = attributeDataBuffer->at(index).endTimeStamp;
            extendChunkMetadataBuffer->at(attributeChunkMetadataDims + index).datasetIndex = attributeDataBuffer->at(index).datasetIndex + lastEventIndex;
            extendChunkMetadataBuffer->at(attributeChunkMetadataDims + index).totalChunkEvents = attributeDataBuffer->at(index).totalChunkEvents;

        }
        ret = H5Awrite(attr1, attributeType, extendChunkMetadataBuffer->data());

        /*
        * Modify MAX attribute
        */
        attr_DatasetMax = H5Aopen(storyDataset, ATTRIBUTE_DATASET_MAX, H5P_DEFAULT);
        attrMaxError = H5Awrite(attr_DatasetMax, H5T_NATIVE_ULONG, &storyChunk->at(storyChunkDims[0] - 1));

        attr_DatasetTotalStoryEvents = H5Aopen(storyDataset, ATTRIBUTE_TOTAL_DATASET_STORY_EVENTS, H5P_DEFAULT);
        attrStoryEventsError = H5Awrite(attr_DatasetTotalStoryEvents, H5T_NATIVE_ULONG, &newStorySize[0]);

        if(ret < 0 || attr_DatasetMax < 0 || attr_DatasetTotalStoryEvents < 0 || attrMaxError < 0 || attrStoryEventsError < 0){
            goto release_resources;
        }

        // Release extendAttributeBuffer memory
        delete extendChunkMetadataBuffer;
    }
    else{
        hsize_t attributeDims[] = {attributeCount};
        attr = H5Screate(H5S_SIMPLE);
        ret  = H5Sset_extent_simple(attr, ATTRIBUTE_CHUNKMETADATA_RANK, attributeDims, NULL);

        /*
        * Create array attribute and write array attribute.
        */
        attr1 = H5Acreate2(storyDataset, ATTRIBUTE_CHUNKMETADATA, chunkmetaAttribute, attr, H5P_DEFAULT, H5P_DEFAULT);
        attrError = H5Awrite(attr1, chunkmetaAttribute, attributeDataBuffer->data());

        /*
        * Create and write to Min and Max and totalStoryEvents attribute.
        */
        attributeDims[0] = {1};
        attr = H5Screate_simple(1, attributeDims, NULL);

        attr_DatasetMin = H5Acreate2(storyDataset, ATTRIBUTE_DATASET_MIN, H5T_NATIVE_ULONG, attr, H5P_DEFAULT, H5P_DEFAULT);
        attrMinError = H5Awrite(attr_DatasetMin, H5T_NATIVE_ULONG, &storyChunk->at(0).timeStamp);

        attr_DatasetMax = H5Acreate2(storyDataset, ATTRIBUTE_DATASET_MAX, H5T_NATIVE_ULONG, attr, H5P_DEFAULT, H5P_DEFAULT);
        attrMaxError = H5Awrite(attr_DatasetMax, H5T_NATIVE_ULONG, &storyChunk->at(storyChunkDims[0]-1).timeStamp);

        attr_DatasetTotalStoryEvents = H5Acreate2(storyDataset, ATTRIBUTE_TOTAL_DATASET_STORY_EVENTS, H5T_NATIVE_ULONG, attr, H5P_DEFAULT, H5P_DEFAULT);
        attrStoryEventsError = H5Awrite(attr_DatasetTotalStoryEvents, H5T_NATIVE_ULONG, &storyChunkDims[0]);
        if(attrStoryEventsError < 0 || attr_DatasetTotalStoryEvents < 0 || attrMinError < 0 || attr_DatasetMin < 0 || attrMaxError < 0 || attr_DatasetMax < 0 || attr < 0 || attr1 < 0 || attrError < 0)
        {
            goto release_resources;
        }
    }

    /*
    * Release resources
    */
    if(attributeDataBuffer != nullptr) {    delete attributeDataBuffer;}
    if(attr1 >= 0)  H5Aclose(attr1);
    if(attr >= 0)   H5Sclose(attr);
    if(chunkPropId >= 0)    H5Pclose(chunkPropId);
    if(attributeDataSpace >= 0)    H5Sclose(attributeDataSpace);
    if(attributeId >= 0)    H5Aclose(attributeId);
    if(attributeType >= 0)    H5Tclose(attributeType);
    if(attr_DatasetMin >= 0)    H5Aclose(attr_DatasetMin);
    if(attr_DatasetMax >= 0)    H5Aclose(attr_DatasetMax);
    if(attr_DatasetTotalStoryEvents >= 0)    H5Aclose(attr_DatasetTotalStoryEvents);
    if(chunkmetaAttribute >=0 ) H5Tclose(chunkmetaAttribute);
    if(eventType >=0 )  H5Tclose(eventType);
    if(storyDataset >= 0)   H5Dclose(storyDataset);
    if(ChronicleH5 >= 0)  H5Fclose(ChronicleH5);
    return 0;


    // goto
    release_resources:
    if(attributeDataBuffer != nullptr) {    delete attributeDataBuffer;}
    if(attr1 >= 0)  H5Aclose(attr1);
    if(attr >= 0)   H5Sclose(attr);
    if(chunkPropId >= 0)    H5Pclose(chunkPropId);
    if(attributeDataSpace >= 0)    H5Sclose(attributeDataSpace);
    if(attributeId >= 0)    H5Aclose(attributeId);
    if(attributeType >= 0)    H5Tclose(attributeType);
    if(attr_DatasetMin >= 0)    H5Aclose(attr_DatasetMin);
    if(attr_DatasetMax >= 0)    H5Aclose(attr_DatasetMax);
    if(attr_DatasetTotalStoryEvents >= 0)    H5Aclose(attr_DatasetTotalStoryEvents);
    if(chunkmetaAttribute >=0 ) H5Tclose(chunkmetaAttribute);
    if(eventType >=0 )  H5Tclose(eventType);
    if(storyDataset >= 0)   H5Dclose(storyDataset);
    if(ChronicleH5 >= 0)  H5Fclose(ChronicleH5);
    return -1;

}

/**
 * @brief Serialize a map to JSON object
 * @param obj: json_object to attach the map to
 * @param map: map to serialize
 */
void storywriter::serializeMap(json_object *obj, std::map<std::string, std::string> &map)
{
    for (auto & it : map)
    {
        json_object *value = json_object_new_string(it.second.c_str());
        json_object_object_add(obj, it.first.c_str(), value);
    }
}

/**
 * @brief Serialize a map of maps to a vector of JSON strings
 * @param maps: map of maps to serialize
 * @return a vector of JSON strings
 */
std::vector<std::string> storywriter::serializeMaps(std::map<std::string, std::map<std::string, std::string>> &maps)
{
    std::vector<std::string> strings;
    for (auto & map : maps)
    {
        json_object *obj = json_object_new_object();
        serializeMap(obj, map.second);
        const char *json = json_object_to_json_string(obj);
        strings.emplace_back(json);
    }
    return strings;
}

// Convert a single element to a json_object
template <typename T>
json_object *storywriter::convertToJsonObject(const T &value) {
    json_object *obj = nullptr;

    if constexpr (std::is_same_v<T, int>) {
        obj = json_object_new_int(value);
    } else if constexpr (std::is_same_v<T, double>) {
        obj = json_object_new_double(value);
    } else if constexpr (std::is_same_v<T, std::string>) {
        obj = json_object_new_string(value.c_str());
    }
    // Add more type conversions for other types as needed

    return obj;
}

// Serialize the std::tuple to a json_object
template <typename... Args, size_t... Is>
json_object *storywriter::serializeTupleToJsonObject(const std::tuple<Args...> &tuple, std::index_sequence<Is...>) {
    json_object *obj = json_object_new_array();

    // Convert each element of the std::tuple to a json_object and add it to the array
    (json_object_array_add(obj, convertToJsonObject(std::get<Is>(tuple))), ...);

    return obj;
}

// Helper function to serialize the std::tuple to a json_object
template <typename... Args>
json_object *storywriter::serializeTupleToJsonObject(const std::tuple<Args...> &tuple) {
    return serializeTupleToJsonObject(tuple, std::make_index_sequence<sizeof...(Args)>{});
}
