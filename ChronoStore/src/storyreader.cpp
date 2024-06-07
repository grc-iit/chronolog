#include <iostream>

#define H5_SIZEOF_SSIZE_T H5_SIZEOF_LONG_LONG

#include <hdf5.h>
#include <string>
#include <vector>
#include <storyreader.h>
#include <event.h>
#include <chunkattr.h>
#include <datasetreader.h>
#include <datasetminmax.h>

#define DATASET_RANK 1 // Dataset dimension
#define ATTRIBUTE_CHUNKMETADATA "ChunkMetadata"
#define ATTRIBUTE_DATASET_MIN "Min"
#define ATTRIBUTE_DATASET_MAX "Max"
#define ATTRIBUTE_TOTAL_DATASET_STORY_EVENTS "TotalStoryEvents"
#define DEBUG 0 // Set to 1 ito print H5 error messages to console


// Constructor storyreader
storyreader::storyreader()
{}

// Destructor storyreader
storyreader::~storyreader()
{}

// Search first chunk to retrieve
int64_t searchFirstChunk(std::vector <ChunkAttr>*chunkAttributeData, uint64_t timestampToSearch, int64_t chunkEnd
                         , int64_t chunkStart)
{
    int64_t currentChunk = INT_MIN;
    int64_t ansChunk = -1;
    if(chunkEnd == 0) return currentChunk;

    while(chunkStart <= chunkEnd)
    {
        currentChunk = chunkStart + (chunkEnd - chunkStart) / 2;
        if(chunkAttributeData->at(currentChunk).startTimeStamp == timestampToSearch)
        {
            return currentChunk;
        }
        else if(chunkAttributeData->at(currentChunk).startTimeStamp > timestampToSearch)
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
int64_t searchLastChunk(std::vector <ChunkAttr>*chunkAttributeData, uint64_t timestampToSearch, int64_t chunkEnd
                        , int64_t chunkStart)
{
    int64_t currentChunk = INT_MIN;
    if(chunkEnd == 0) return currentChunk;

    while(chunkStart <= chunkEnd)
    {
        currentChunk = chunkStart + (chunkEnd - chunkStart) / 2;
        if(chunkAttributeData->at(currentChunk).endTimeStamp == timestampToSearch)
        {
            return currentChunk;
        }
        else if(chunkAttributeData->at(currentChunk).endTimeStamp > timestampToSearch)
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
DatasetReader storyreader::readFromDataset(std::pair <uint64_t, uint64_t> range, const char*STORY, const char*CHRONICLE)
{
    // Disable automatic printing of HDF5 error stack
    if(!DEBUG)
    {
        H5Eset_auto(H5E_DEFAULT, NULL, NULL);
    }
    std::vector <Event> resultVector;
    DatasetReader dr(-1, resultVector);
    hid_t chronicle = -1, story = -1, storyDatasetSpace = -1, eventType = -1, attributeId = -1, attributeType = -1, attributeDataSpace = -1, hyperslabMemorySpace = -1;
    herr_t status = -1, readChunkAttributeErr = -1, timeStampError = -1, dataError = -1, memSpaceErr = -1, datasetSpaceError = -1;
    DatasetMinMax dmm(-1, std::make_pair(0, 0));
    std::pair <int64_t, int64_t> chunksToRead = {INT_MIN, INT_MIN};
    hsize_t datasetHyperslabStart[1] = {0}, storyChunkDims[1] = {0}, chunkOffset[1] = {0}, attributeDims = 0;
    std::vector <Event>*eventBuffer;
    std::vector <ChunkAttr>*attributeDataBuffer;

    /**
     * Read data from the story
     */
    chronicle = H5Fopen(CHRONICLE, H5F_ACC_RDONLY, H5P_DEFAULT);
    story = H5Dopen2(chronicle, STORY, H5P_DEFAULT);
    storyDatasetSpace = H5Dget_space(story);

    if(chronicle < 0 || story < 0 || storyDatasetSpace < 0)
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

    attributeDataBuffer = new std::vector <ChunkAttr>(attributeDims);
    if(attributeDataBuffer == nullptr)
    {
        return dr;
    }

    readChunkAttributeErr = H5Aread(attributeId, attributeType, attributeDataBuffer->data());
    if(attributeId < 0 || attributeType < 0 || attributeDataSpace < 0 || readChunkAttributeErr < 0)
    {
        goto release_resources;
    }

    if(attributeType >= 0) H5Tclose(attributeType);
    if(attributeDataSpace >= 0) H5Sclose(attributeDataSpace);
    if(attributeId >= 0) H5Aclose(attributeId);

    dmm = readDatasetRange(STORY, CHRONICLE);
    if(dmm.status != 0 || dmm.MinMax.first > range.first || dmm.MinMax.second < range.second)
    {
        goto release_resources;
    }

    /*
     * Find the chunks to read
     */
    if(attributeDims == 1)
    {
        chunksToRead.first = 0;
        chunksToRead.second = 0;
    }
    else
    {
        chunksToRead.first = searchFirstChunk(attributeDataBuffer, range.first, attributeDims - 1, 0);
        if(chunksToRead.first != INT_MIN)
        {
            chunksToRead.second = searchLastChunk(attributeDataBuffer, range.second, attributeDims - 1
                                                  , chunksToRead.first);
        }
    }

    // std::cout<<"Chunks to read: "<<chunksToRead.first<<" "<<chunksToRead.second<<std::endl;
    /*
     * Check for the range of requested query of chunks whether the chunkToRead is valid
     */
    if(chunksToRead.second == INT_MIN || chunksToRead.first == INT_MIN)
    {
        if(story >= 0) H5Dclose(story);
        if(chronicle >= 0) H5Fclose(chronicle);
        return dr;
    }

    /*
     *  Declare eventType
     */
    eventType = H5Tcreate(H5T_COMPOUND, sizeof(Event));
    timeStampError = H5Tinsert(eventType, "timeStamp", HOFFSET(Event, timeStamp), H5T_NATIVE_ULLONG);
    dataError = H5Tinsert(eventType, "data", HOFFSET(Event, data), H5T_NATIVE_CHAR);

    if(eventType < 0 || dataError < 0 || timeStampError < 0)
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
        for(int chunkIndex = chunksToRead.first; chunkIndex <= chunksToRead.second; chunkIndex++)
        {
            storyChunkDims[0] = {attributeDataBuffer->at(chunkIndex).totalChunkEvents};
            chunkOffset[0] = {attributeDataBuffer->at(chunkIndex).datasetIndex};

            hyperslabMemorySpace = H5Screate_simple(1, storyChunkDims, NULL);
            memSpaceErr = H5Sselect_hyperslab(hyperslabMemorySpace, H5S_SELECT_SET, datasetHyperslabStart, NULL
                                              , storyChunkDims, NULL);
            datasetSpaceError = H5Sselect_hyperslab(storyDatasetSpace, H5S_SELECT_SET, chunkOffset, NULL, storyChunkDims
                                                    , NULL);

            eventBuffer = new std::vector <Event>(attributeDataBuffer->at(chunkIndex).totalChunkEvents);
            if(eventBuffer == nullptr)
            {
                goto release_resources;
            }

            status = H5Dread(story, eventType, hyperslabMemorySpace, storyDatasetSpace, H5P_DEFAULT
                             , eventBuffer->data());
            if(status < 0 || datasetSpaceError < 0 || memSpaceErr < 0 || hyperslabMemorySpace < 0 ||
               storyDatasetSpace < 0)
            {
                if(eventBuffer != nullptr) delete eventBuffer;
                goto release_resources;
            }

            for(hsize_t i = 0; i < storyChunkDims[0]; i++)
            {
                if(range.first <= eventBuffer->at(i).timeStamp && eventBuffer->at(i).timeStamp <= range.second)
                {
                    dr.eventData.push_back(eventBuffer->at(i));
                }
            }
            delete (eventBuffer);
        }
    }
    catch(const std::bad_alloc &e)
    {
        goto release_resources;
    }

    /* 
    * Release resources
    */
    if(attributeDataBuffer != nullptr) delete (attributeDataBuffer);
    if(storyDatasetSpace >= 0) H5Sclose(storyDatasetSpace);
    if(hyperslabMemorySpace >= 0) H5Sclose(hyperslabMemorySpace);
    if(eventType >= 0) H5Tclose(eventType);
    if(story >= 0) H5Dclose(story);
    if(chronicle >= 0) H5Fclose(chronicle);
    dr.status = 0;
    return dr;

    //goto
    release_resources:
    if(attributeDataBuffer != nullptr) delete (attributeDataBuffer);
    if(storyDatasetSpace >= 0) H5Sclose(storyDatasetSpace);
    if(hyperslabMemorySpace >= 0) H5Sclose(hyperslabMemorySpace);
    if(eventType >= 0) H5Tclose(eventType);
    if(story >= 0) H5Dclose(story);
    if(chronicle >= 0) H5Fclose(chronicle);
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
DatasetMinMax storyreader::readDatasetRange(const char*STORY, const char*CHRONICLE)
{

    std::pair <uint64_t, uint64_t> datasetRange;
    DatasetMinMax d(-1, datasetRange);
    std::vector <uint64_t> DatasetReader(2);
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

    if(chronicle < 0 || story < 0 || attributeMinId < 0 || statusErrorMin < 0 || attributeMinDataSpace < 0 ||
       attributeMaxId < 0 || attributeMaxDataSpace < 0 || statusErrorMax < 0)
    {
        if(attributeMinDataSpace >= 0) H5Sclose(attributeMinDataSpace);
        if(attributeMinId >= 0) H5Aclose(attributeMinId);
        if(attributeMaxDataSpace >= 0) H5Sclose(attributeMaxDataSpace);
        if(attributeMaxId >= 0) H5Aclose(attributeMaxId);
        if(story >= 0) H5Dclose(story);
        if(chronicle >= 0) H5Fclose(chronicle);
        return d;
    }

    d.MinMax.first = (datasetRange.first);
    d.MinMax.second = (datasetRange.second);
    d.status = 0;

    /*
     * Release resources
     */
    if(attributeMinDataSpace >= 0) H5Sclose(attributeMinDataSpace);
    if(attributeMinId >= 0) H5Aclose(attributeMinId);
    if(attributeMaxDataSpace >= 0) H5Sclose(attributeMaxDataSpace);
    if(attributeMaxId >= 0) H5Aclose(attributeMaxId);
    if(story >= 0) H5Dclose(story);
    if(chronicle >= 0) H5Fclose(chronicle);

    return d;
}