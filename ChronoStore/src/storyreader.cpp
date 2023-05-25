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
storyreader::storyreader(){}

// Destructor storyreader
storyreader::~storyreader(){}

// Search first chunk to retrieve
int64_t searchFirstChunk(ChunkAttr* chunkData, uint64_t timestampToSearch, int64_t chunkEnd, int64_t chunkStart) {
    int64_t currentChunk = INT_MIN;
    int64_t ansChunk = -1;
    if(chunkEnd == 0)    return currentChunk;

    while (chunkStart <= chunkEnd) {
        currentChunk = chunkStart + (chunkEnd - chunkStart) / 2;
        if (chunkData[currentChunk].startTimeStamp == timestampToSearch) {
            return currentChunk;
        }
        else if (chunkData[currentChunk].startTimeStamp > timestampToSearch) {
            chunkEnd = currentChunk - 1;
        }
        else {
            chunkStart = currentChunk + 1;
            ansChunk = currentChunk;
        }
    }
    return ansChunk;
}

// Search last chunk to retrieve
int64_t searchLastChunk(ChunkAttr* chunkData, uint64_t timestampToSearch, int64_t chunkEnd, int64_t chunkStart) {
    int64_t currentChunk = INT_MIN;
    if(chunkEnd == 0)    return currentChunk;

    while (chunkStart <= chunkEnd) {
        currentChunk = chunkStart + (chunkEnd - chunkStart) / 2;
        if (chunkData[currentChunk].endTimeStamp == timestampToSearch) {
            return currentChunk;
        }
        else if (chunkData[currentChunk].endTimeStamp > timestampToSearch) {
            chunkEnd = currentChunk - 1;
        }
        else {
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
DatasetReader storyreader::readFromDataset(std::pair<uint64_t, uint64_t> range, const char* STORY, const char* CHRONICLE){
    // Disable automatic printing of HDF5 error stack
    if(!DEBUG){
        H5Eset_auto(H5E_DEFAULT, NULL, NULL);
    }
    std::vector<Event> resultVector;
    DatasetReader dr(-1, resultVector);
    hid_t   chronicle, story, storyDatasetSpace, eventType;
    herr_t  status;
    ChunkAttr* attributeDataBuffer;
    Event* eventBuffer;

    /**
     * Read data from the story
     */
    chronicle = H5Fopen(CHRONICLE, H5F_ACC_RDONLY, H5P_DEFAULT);
    story = H5Dopen2(chronicle, STORY, H5P_DEFAULT);
    storyDatasetSpace  = H5Dget_space(story);

    if(chronicle < 0 || story < 0 || storyDatasetSpace < 0){
        if(storyDatasetSpace >= 0)  H5Sclose(storyDatasetSpace);
        if(story >= 0)  H5Dclose(story);
        if(chronicle >= 0)  H5Fclose(chronicle);
        return dr;
    }

    /**
     * Open the attribute chunkmetadata
     * Get the attribute type and storyDatasetSpace
     * Get the number of DATASET_RANK of the storyDatasetSpace
     * Get the story creation property list for chunksize information
     * Read the attribute data into the buffer
     */
    hid_t attributeId = H5Aopen(story, ATTRIBUTE_CHUNKMETADATA, H5P_DEFAULT);

    hid_t attributeType = H5Aget_type(attributeId);
    hid_t attributeDataSpace = H5Aget_space(attributeId);

    hsize_t attributeDims = H5Sget_simple_extent_npoints(attributeDataSpace);

    attributeDataBuffer = (ChunkAttr *) malloc(attributeDims * sizeof(ChunkAttr));
    herr_t readChunkAttributeErr = H5Aread(attributeId, attributeType, attributeDataBuffer);

    if(attributeId < 0 || attributeDataBuffer == NULL || attributeType < 0 || attributeDataSpace < 0 || readChunkAttributeErr < 0){
        if(attributeDataBuffer != NULL) free(attributeDataBuffer);
        if(attributeType >= 0) H5Tclose(attributeType);
        if(attributeDataSpace >= 0)  H5Sclose(attributeDataSpace);
        if(attributeId >= 0) H5Aclose(attributeId);
        if(story >= 0) H5Dclose(story);
        if(chronicle >= 0) H5Fclose(chronicle);
        return dr;
    }

    if(attributeType >= 0)  H5Tclose(attributeType);
    if(attributeDataSpace >= 0)  H5Sclose(attributeDataSpace);
    if(attributeId >= 0)  H5Aclose(attributeId);

  
    DatasetMinMax dmm = readDatasetRange(STORY, CHRONICLE);
    if(dmm.status != 0 || dmm.MinMax.first > range.first || dmm.MinMax.second < range.second){
        free(attributeDataBuffer);
        if(story >= 0)    H5Dclose(story);
        if(chronicle >= 0)    H5Fclose(chronicle);
        dr.status = -1;
        // std::cout<<"Min: "<<dmm.MinMax[0]<<" Max: "<<dmm.MinMax[1]<<" "<<dmm.status<<std::endl;
        return dr; 
    }

    /*
     * Find the chunks to read
     */
    std::pair<int64_t, int64_t> chunksToRead = {INT_MIN , INT_MIN};
    if(attributeDims == 1){
        chunksToRead.first = 0;
        chunksToRead.second = 0;
    }
    else{
        chunksToRead.first = searchFirstChunk(attributeDataBuffer, range.first, attributeDims - 1, 0);
        if(chunksToRead.first != INT_MIN){
            chunksToRead.second = searchLastChunk(attributeDataBuffer, range.second, attributeDims - 1, chunksToRead.first);
        }
    }

    // std::cout<<"Chunks to read: "<<chunksToRead.first<<" "<<chunksToRead.second<<std::endl;
    /*
     * Check for the range of requested query of chunks whether the chunkToRead is valid
     */
    if(chunksToRead.second == INT_MIN || chunksToRead.first == INT_MIN){
        if(attributeDataBuffer != NULL) free(attributeDataBuffer);
        if(story >= 0)    H5Dclose(story);
        if(chronicle >= 0)    H5Fclose(chronicle);
        return dr;
    }

    /*
     *  Declare eventType
     */
    herr_t timeStampError, dataError;
    eventType = H5Tcreate(H5T_COMPOUND, sizeof(Event));
    timeStampError = H5Tinsert(eventType, "timeStamp", HOFFSET(Event, timeStamp), H5T_NATIVE_ULLONG);
    dataError = H5Tinsert(eventType, "data", HOFFSET(Event, data), H5T_NATIVE_CHAR);

    if(eventType < 0 || dataError < 0 || timeStampError < 0){
        if(eventType >=0 )  H5Tclose(eventType);
        if(attributeDataBuffer != NULL) free(attributeDataBuffer);
        if(story >= 0)  H5Dclose(story);
        if(chronicle >= 0)  H5Fclose(chronicle);
        return dr;
    }

    hsize_t datasetHyperslabStart[1] = {0};
    hsize_t storyChunkDims[1];
    herr_t memSpaceErr, datasetSpaceError;
    hid_t hyperslabMemorySpace;
    storyDatasetSpace  = H5Dget_space(story);
    hsize_t chunkOffset[1] = {0};
    try{

        /* 
        * Create a memory storyDatasetSpace to hold the chunk data 
        */
        for(int chunkIndex = chunksToRead.first; chunkIndex <= chunksToRead.second ; chunkIndex++){
            storyChunkDims[0] = {attributeDataBuffer[chunkIndex].totalChunkEvents};
            chunkOffset[0] = {attributeDataBuffer[chunkIndex].datasetIndex};

            hyperslabMemorySpace = H5Screate_simple(1, storyChunkDims, NULL);
            memSpaceErr = H5Sselect_hyperslab(hyperslabMemorySpace, H5S_SELECT_SET, datasetHyperslabStart, NULL, storyChunkDims, NULL);
            datasetSpaceError = H5Sselect_hyperslab(storyDatasetSpace, H5S_SELECT_SET, chunkOffset, NULL, storyChunkDims, NULL);

            eventBuffer = (Event*) malloc(attributeDataBuffer[chunkIndex].totalChunkEvents * sizeof(Event));

            status = H5Dread(story, eventType, hyperslabMemorySpace, storyDatasetSpace, H5P_DEFAULT, eventBuffer);
            if(status < 0 || datasetSpaceError < 0 || memSpaceErr < 0 || hyperslabMemorySpace < 0 || eventBuffer  == NULL || storyDatasetSpace < 0){
                free(attributeDataBuffer);
                if(hyperslabMemorySpace >= 0) H5Sclose(hyperslabMemorySpace);
                if(eventBuffer != NULL) free(eventBuffer);
                if(eventType >= 0)  H5Tclose(eventType);
                if(storyDatasetSpace >= 0)  H5Sclose(storyDatasetSpace);
                if(story >= 0)  H5Dclose(story);
                if(chronicle >= 0)  H5Fclose(chronicle);

                dr.eventData.erase(dr.eventData.begin(), dr.eventData.end());
                return dr;
            }

            for(hsize_t i = 0 ; i < storyChunkDims[0]; i++ ){
                if(range.first <= eventBuffer[i].timeStamp && eventBuffer[i].timeStamp <= range.second){
                    dr.eventData.push_back(eventBuffer[i]);
                }
            }
            free(eventBuffer);
        }

    } 
    catch (const std::bad_alloc& e)
    {

        if(eventBuffer != NULL) free(eventBuffer);
        if(attributeDataBuffer != NULL) free(attributeDataBuffer);
        if(hyperslabMemorySpace >= 0)   H5Sclose(hyperslabMemorySpace);
        if(eventType >= 0)  H5Tclose(eventType);
        if(story >= 0)  H5Dclose(story);
        if(chronicle >= 0)  H5Fclose(chronicle);

        std::cout<<"returned from catch\n";
        dr.eventData.erase(dr.eventData.begin(), dr.eventData.end());
        return dr;
    }

    /* 
    * Release resources
    */
    if(attributeDataBuffer != NULL) free(attributeDataBuffer);
    if(storyDatasetSpace >= 0)  H5Sclose(storyDatasetSpace);
    if(hyperslabMemorySpace >= 0) H5Sclose(hyperslabMemorySpace);
    if(eventType >= 0)  H5Tclose(eventType);
    if(story >= 0)  H5Dclose(story);
    if(chronicle >= 0)  H5Fclose(chronicle);
    dr.status = 0;
    return dr;
}

/**
 * Read story dataset Min and Max timeStamp
 * @param STORY: story name
 * @param CHRONICLE: chronicle name
 * @return: struct DatasetMinMax ->{0,MinMax} (MinMax is pair consisting of Min nd Max of dataset respectively) if successful, else \n
 *          return {-1,MinMax}   (MinMax empty) if failed.
 */
DatasetMinMax storyreader::readDatasetRange(const char* STORY, const char* CHRONICLE){

    std::pair<uint64_t, uint64_t> datasetRange;
    DatasetMinMax d(-1, datasetRange);
    std::vector<uint64_t> DatasetReader(2);
    hid_t   chronicle, story, attributeMinId, attributeMinDataSpace, attributeMaxId, attributeMaxDataSpace;
    herr_t  statusErrorMin, statusErrorMax;
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

    if(chronicle < 0 || story < 0 || attributeMinId < 0 || statusErrorMin < 0 || attributeMinDataSpace < 0 || attributeMaxId < 0 || attributeMaxDataSpace < 0 || statusErrorMax < 0){
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