#include <iostream>

#define H5_SIZEOF_SSIZE_T H5_SIZEOF_LONG_LONG

#include <hdf5.h>
#include <fstream>
#include <string>
#include <cstring>
#include <vector>
#include <event.h>
#include <chunkattr.h>
#include <storywriter.h>

#define DATASET_RANK 1 // Dataset dimension
#define ATTRIBUTE_CHUNKMETADATA "ChunkMetadata"
#define ATTRIBUTE_DATASET_MIN "Min"
#define ATTRIBUTE_DATASET_MAX "Max"
#define ATTRIBUTE_TOTAL_DATASET_STORY_EVENTS "TotalStoryEvents"
#define ATTRIBUTE_CHUNKMETADATA_RANK 1
#define DEBUG 0 // Set to 1 ito print H5 error messages to console
#define CHUNK_SIZE 40000 // Number of events equal to page size of 4MB

// Constructor storywriter
storywriter::storywriter(){}

// Destructor storywriter
storywriter::~storywriter(){}

/**
 * Write/Append data to storyDataset. 
 * @param storyChunk: Data to write to story dataset.
 * @param STORY: story name
 * @param CHRONICLE: chronicle name
 * @return: 0 if successful, else -1, if failed.
 */
int storywriter::writeStoryChunk(std::vector<Event> storyChunk, const char* STORY, const char* CHRONICLE) {

    // Disable automatic printing of HDF5 error stack to console
    if(!DEBUG){
        H5Eset_auto(H5E_DEFAULT, NULL, NULL);
    }

    hid_t   ChronicleH5, storyDataset, storyDatasetSpace, chunkPropId, hyperslabMemorySpace, eventType, chunkmetaAttribute, attr, attr1, attr_DatasetMin, attr_DatasetMax, attr_DatasetTotalStoryEvents;
    hid_t   attributeId, attributeType, attributeDataSpace;
    herr_t  status, ret;

    Event* eventBuffer;
    ChunkAttr* attributeDataBuffer;
    ChunkAttr* extendChunkMetadataBuffer;

    /* 
    * Chunk Dimension and number of events in a story chunk
    */
    const hsize_t chunkDims[1] = {CHUNK_SIZE};
    const hsize_t numberOfEvents = storyChunk.size() - 1;

    /*
    * Allocate memory for events buffer
    * TODO: Modification required in case of variable sized events
    */
    eventBuffer = (Event*) malloc(numberOfEvents * sizeof(Event));
    if(eventBuffer == NULL){
        return -1;
    }

    /*
    * Allocate memory for attribute buffer
    */
    attributeDataBuffer = (ChunkAttr*) malloc(((numberOfEvents / CHUNK_SIZE) + 1) * sizeof(ChunkAttr));
    if(attributeDataBuffer == NULL){
        if(eventBuffer != NULL) free(eventBuffer);
        return -1;
    }

    /*
    * Read data from storyChunk into eventBuffer
    */
    for(auto event = 0 ; event < numberOfEvents ; event++) {
    eventBuffer[event].timeStamp = storyChunk[event].timeStamp;
    strcpy(eventBuffer[event].data, storyChunk[event].data);
    }

    /*
    * Dimensions for the storyDataset
    */
    const hsize_t storyChunkDims[] = {storyChunk.size() - 1};

    /*
    * User defined compound datatype - consists "timeStamp" and "data"
    */
    herr_t timeStampError, dataError;
    eventType = H5Tcreate(H5T_COMPOUND, sizeof(Event));
    timeStampError = H5Tinsert(eventType, "timeStamp", HOFFSET(Event, timeStamp), H5T_NATIVE_ULONG);
    dataError = H5Tinsert(eventType, "data", HOFFSET(Event, data), H5T_NATIVE_CHAR);
    if(eventType < 0 || dataError < 0 || timeStampError < 0){
        if(eventType >=0 )  H5Tclose(eventType);
        if(attributeDataBuffer != NULL) free(attributeDataBuffer);
        if(eventBuffer != NULL) free(eventBuffer);
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

        ChronicleH5 = H5Fopen(CHRONICLE, H5F_ACC_RDONLY, H5P_DEFAULT);
        if(ChronicleH5 < 0){
            if(eventType >=0 )  H5Tclose(eventType);
            if(attributeDataBuffer != NULL) free(attributeDataBuffer);
            if(eventBuffer != NULL) free(eventBuffer);
            return -1;
        }
        if(H5Lexists(ChronicleH5, STORY, H5P_DEFAULT) > 0){
            /*
            * Open ChronicleH5 and storyDataset
            */
            H5Fclose(ChronicleH5);
            ChronicleH5 = H5Fopen(CHRONICLE, H5F_ACC_RDWR, H5P_DEFAULT);

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
                if(attributeId >= 0)  H5Aclose(attributeId);
                if(attributeDataSpace >= 0)  H5Sclose(attributeDataSpace);
                if(attributeType >= 0)  H5Tclose(attributeType);
                if(storyDataset >= 0)  H5Dclose(storyDataset);
                if(ChronicleH5 >= 0)  H5Fclose(ChronicleH5);
                if(eventType >=0 )  H5Tclose(eventType);
                if(attributeDataBuffer != NULL) free(attributeDataBuffer);
                if(eventBuffer != NULL) free(eventBuffer);
                return -1;
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
                if(storyDataset >= 0)  H5Dclose(storyDataset);
                if(ChronicleH5 >= 0)  H5Fclose(ChronicleH5);
                if(eventType >=0 )  H5Tclose(eventType);
                if(attributeDataBuffer != NULL) free(attributeDataBuffer);
                if(eventBuffer != NULL) free(eventBuffer);
                return -1;
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
                if(storyDatasetSpace >= 0)  H5Sclose(storyDatasetSpace);
                if(storyDataset >= 0)  H5Dclose(storyDataset);
                if(ChronicleH5 >= 0)  H5Fclose(ChronicleH5);
                if(eventType >=0 )  H5Tclose(eventType);
                if(attributeDataBuffer != NULL) free(attributeDataBuffer);
                if(eventBuffer != NULL) free(eventBuffer);
                return -1;
            }
        }

        /*
        * Create the storyDataset
        */
        else{
            std::cout << "Dataset not found..." << std::endl;
            H5Fclose(ChronicleH5);
            ChronicleH5 = H5Fopen(CHRONICLE, H5F_ACC_RDWR, H5P_DEFAULT);

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
                if(chunkPropId >= 0)    H5Pclose(chunkPropId);
                if(storyDatasetSpace >= 0)  H5Sclose(storyDatasetSpace);
                if(ChronicleH5 >= 0)  H5Fclose(ChronicleH5);
                if(eventType >=0 )  H5Tclose(eventType);
                if(attributeDataBuffer != NULL) free(attributeDataBuffer);
                if(eventBuffer != NULL) free(eventBuffer);
                return -1;
            }

            storyDataset = H5Dcreate2(ChronicleH5, STORY, eventType, storyDatasetSpace, H5P_DEFAULT, chunkPropId, H5P_DEFAULT);
            if(storyDataset < 0){
                
                if(chunkPropId >= 0)    H5Pclose(chunkPropId);
                if(storyDatasetSpace >= 0)  H5Sclose(storyDatasetSpace);
                if(ChronicleH5 >= 0)  H5Fclose(ChronicleH5);
                if(eventType >=0 )  H5Tclose(eventType);
                if(attributeDataBuffer != NULL) free(attributeDataBuffer);
                if(eventBuffer != NULL) free(eventBuffer);
                return -1;
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
            if(chunkPropId >= 0)    H5Pclose(chunkPropId);
            if(storyDatasetSpace >= 0)  H5Sclose(storyDatasetSpace);
            if(ChronicleH5 >= 0)  H5Fclose(ChronicleH5);
            if(attributeDataBuffer != NULL) free(attributeDataBuffer);
            if(eventBuffer != NULL) free(eventBuffer);
            if(eventType >=0 )  H5Tclose(eventType);
            return -1;
        }

        storyDataset = H5Dcreate2(ChronicleH5, STORY, eventType, storyDatasetSpace, H5P_DEFAULT, chunkPropId, H5P_DEFAULT);
        if(storyDataset < 0){
            if(chunkPropId >= 0)    H5Pclose(chunkPropId);
            if(storyDatasetSpace >= 0)  H5Sclose(storyDatasetSpace);
            if(ChronicleH5 >= 0)  H5Fclose(ChronicleH5);
            if(attributeDataBuffer != NULL) free(attributeDataBuffer);
            if(eventBuffer != NULL) free(eventBuffer);
            if(eventType >=0 )  H5Tclose(eventType);
            return -1;
        }
    }

    /*
    * Write data to the storyDataset
    */
    hsize_t datasetHyperslabStart[] = {0};
    hsize_t storySpaceStart[] = {storyDatasetSpaceStart};
    hyperslabMemorySpace = H5Screate_simple(DATASET_RANK, storyChunkDims, NULL);

    herr_t memSpaceErr, datasetSpaceError;
    memSpaceErr = H5Sselect_hyperslab(hyperslabMemorySpace, H5S_SELECT_SET, datasetHyperslabStart, NULL, storyChunkDims, NULL);
    datasetSpaceError = H5Sselect_hyperslab(storyDatasetSpace, H5S_SELECT_SET, storySpaceStart, NULL, storyChunkDims, NULL);
    if(memSpaceErr < 0 || datasetSpaceError < 0 || hyperslabMemorySpace < 0){
        if(hyperslabMemorySpace >= 0)  H5Sclose(hyperslabMemorySpace);
        if(storyDatasetSpace >= 0)  H5Sclose(storyDatasetSpace);
        if(storyDataset >= 0)   H5Dclose(storyDataset);
        if(ChronicleH5 >= 0)  H5Fclose(ChronicleH5);
        if(attributeDataBuffer != NULL) free(attributeDataBuffer);
        if(eventBuffer != NULL) free(eventBuffer);
        if(eventType >=0 )  H5Tclose(eventType);
        return -1;
    }

    status = H5Dwrite(storyDataset, eventType, hyperslabMemorySpace, storyDatasetSpace, H5P_DEFAULT, &eventBuffer[0]);
    if(status < 0){
        if(hyperslabMemorySpace >= 0)  H5Sclose(hyperslabMemorySpace);
        if(storyDatasetSpace >= 0)  H5Sclose(storyDatasetSpace);
        if(storyDataset >= 0)   H5Dclose(storyDataset);
        if(ChronicleH5 >= 0)  H5Fclose(ChronicleH5);
        if(attributeDataBuffer != NULL) free(attributeDataBuffer);
        if(eventBuffer != NULL) free(eventBuffer);
        if(eventType >=0 )  H5Tclose(eventType);
        return -1;
    }

    H5Sclose(hyperslabMemorySpace);
    H5Sclose(storyDatasetSpace);

    hsize_t attributeCount = 0;
    hsize_t chunksize = CHUNK_SIZE;
    for (hsize_t eventIndex = 0; eventIndex < storyChunkDims[0]; eventIndex += CHUNK_SIZE) {
        if(storyChunkDims[0] - eventIndex < CHUNK_SIZE){
            chunksize = storyChunkDims[0] - eventIndex;
        }
        attributeDataBuffer[attributeCount].startTimeStamp = eventBuffer[eventIndex].timeStamp;
        attributeDataBuffer[attributeCount].endTimeStamp = eventBuffer[eventIndex + chunksize - 1].timeStamp;
        attributeDataBuffer[attributeCount].datasetIndex = eventIndex;
        attributeDataBuffer[attributeCount].totalChunkEvents = chunksize;
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
        if(attributeDataBuffer != NULL) free(attributeDataBuffer);
        if(eventBuffer != NULL) free(eventBuffer);
        if(chunkmetaAttribute >=0 ) H5Tclose(chunkmetaAttribute);
        if(eventType >=0 )  H5Tclose(eventType);
        if(storyDataset >= 0)   H5Dclose(storyDataset);
        if(ChronicleH5 >= 0)  H5Fclose(ChronicleH5);
        return -1;
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
        extendChunkMetadataBuffer = (ChunkAttr *) malloc(attributeDims[0] * sizeof(ChunkAttr));

        /*
        * Read the attribute data into the buffer and delete the attribute
        */
        status = H5Aread(attributeId, attributeType, extendChunkMetadataBuffer);
        if(status < 0 || attributeId < 0 || attributeType < 0 || extendChunkMetadataBuffer == NULL || attributeChunkMetadataDims == HSIZE_UNDEF){
            if(attributeId >= 0)  H5Aclose(attributeId);
            if(attributeType >= 0)  H5Tclose(attributeType);
            if(extendChunkMetadataBuffer != NULL)   free(extendChunkMetadataBuffer);
            if(attributeDataBuffer != NULL) free(attributeDataBuffer);
            if(eventBuffer != NULL) free(eventBuffer);
            if(chunkmetaAttribute >=0 ) H5Tclose(chunkmetaAttribute);
            if(eventType >=0 )  H5Tclose(eventType);
            if(storyDataset >= 0)   H5Dclose(storyDataset);
            if(ChronicleH5 >= 0)  H5Fclose(ChronicleH5);
            return -1;
        }

        if(attributeId >= 0)    H5Aclose(attributeId);
        herr_t deleteError = H5Adelete(storyDataset, ATTRIBUTE_CHUNKMETADATA);

        attr = H5Screate(H5S_SIMPLE);
        ret  = H5Sset_extent_simple(attr, ATTRIBUTE_CHUNKMETADATA_RANK, attributeDims, NULL);

        attr1 = H5Acreate2(storyDataset, ATTRIBUTE_CHUNKMETADATA, attributeType, attr, H5P_DEFAULT, H5P_DEFAULT);

        if(deleteError < 0 || attr < 0 || ret < 0 || attr1 < 0){
            if(attr >= 0)   H5Sclose(attr);
            if(attr1 >= 0)  H5Aclose(attr1);
            if(extendChunkMetadataBuffer != NULL)   free(extendChunkMetadataBuffer);
            if(attributeDataBuffer != NULL) free(attributeDataBuffer);
            if(eventBuffer != NULL) free(eventBuffer);
            if(attributeType >= 0)  H5Tclose(attributeType);
            if(chunkmetaAttribute >=0 ) H5Tclose(chunkmetaAttribute);
            if(eventType >=0 )  H5Tclose(eventType);
            if(storyDataset >= 0)   H5Dclose(storyDataset);
            if(ChronicleH5 >= 0)  H5Fclose(ChronicleH5);
            return -1;
        }

        int lastEventIndex = extendChunkMetadataBuffer[attributeChunkMetadataDims - 1].datasetIndex + extendChunkMetadataBuffer[attributeChunkMetadataDims - 1].totalChunkEvents;
        for(auto index = 0 ; index < attributeCount ; index++ ){
            extendChunkMetadataBuffer[attributeChunkMetadataDims + index].startTimeStamp = attributeDataBuffer[index].startTimeStamp;
            extendChunkMetadataBuffer[attributeChunkMetadataDims + index].endTimeStamp = attributeDataBuffer[index].endTimeStamp;
            extendChunkMetadataBuffer[attributeChunkMetadataDims + index].datasetIndex = attributeDataBuffer[index].datasetIndex + lastEventIndex;
            extendChunkMetadataBuffer[attributeChunkMetadataDims + index].totalChunkEvents = attributeDataBuffer[index].totalChunkEvents;

        }
        ret = H5Awrite(attr1, attributeType, extendChunkMetadataBuffer);

        /*
        * Modify MAX attribute
        */
        attr_DatasetMax = H5Aopen(storyDataset, ATTRIBUTE_DATASET_MAX, H5P_DEFAULT);
        attrMaxError = H5Awrite(attr_DatasetMax, H5T_NATIVE_ULONG, &eventBuffer[storyChunkDims[0]-1].timeStamp);

        attr_DatasetTotalStoryEvents = H5Aopen(storyDataset, ATTRIBUTE_TOTAL_DATASET_STORY_EVENTS, H5P_DEFAULT);
        attrStoryEventsError = H5Awrite(attr_DatasetTotalStoryEvents, H5T_NATIVE_ULONG, &newStorySize[0]);

        if(ret < 0 || attr_DatasetMax < 0 || attr_DatasetTotalStoryEvents < 0 || attrMaxError < 0 || attrStoryEventsError < 0){
            if(attr_DatasetMax >= 0)    H5Aclose(attr_DatasetMax);
            if(attr_DatasetTotalStoryEvents >= 0)    H5Aclose(attr_DatasetTotalStoryEvents);

            if(extendChunkMetadataBuffer != NULL)   free(extendChunkMetadataBuffer);
            if(attributeDataBuffer != NULL) free(attributeDataBuffer);

            if(attr >=0)    H5Sclose(attr);
            if(attr1 >=0)    H5Aclose(attr1);
            if(attributeType >= 0)  H5Tclose(attributeType);
            if(chunkmetaAttribute >=0 ) H5Tclose(chunkmetaAttribute);
            if(eventType >=0 )  H5Tclose(eventType);
            if(storyDataset >= 0)   H5Dclose(storyDataset);
            if(ChronicleH5 >= 0)  H5Fclose(ChronicleH5);
            return -1;
        }

        // Release extendAttributeBuffer memory
        free(extendChunkMetadataBuffer);
    }
    else{
        hsize_t attributeDims[] = {attributeCount};
        attr = H5Screate(H5S_SIMPLE);
        ret  = H5Sset_extent_simple(attr, ATTRIBUTE_CHUNKMETADATA_RANK, attributeDims, NULL);

        /*
        * Create array attribute and write array attribute.
        */
        attr1 = H5Acreate2(storyDataset, ATTRIBUTE_CHUNKMETADATA, chunkmetaAttribute, attr, H5P_DEFAULT, H5P_DEFAULT);
        attrError = H5Awrite(attr1, chunkmetaAttribute, attributeDataBuffer);

        /*
        * Create and write to Min and Max and totalStoryEvents attribute.
        */
        attributeDims[0] = {1};
        attr = H5Screate_simple(1, attributeDims, NULL);

        attr_DatasetMin = H5Acreate2(storyDataset, ATTRIBUTE_DATASET_MIN, H5T_NATIVE_ULONG, attr, H5P_DEFAULT, H5P_DEFAULT);
        attrMinError = H5Awrite(attr_DatasetMin, H5T_NATIVE_ULONG, &eventBuffer[0].timeStamp);

        attr_DatasetMax = H5Acreate2(storyDataset, ATTRIBUTE_DATASET_MAX, H5T_NATIVE_ULONG, attr, H5P_DEFAULT, H5P_DEFAULT);
        attrMaxError = H5Awrite(attr_DatasetMax, H5T_NATIVE_ULONG, &eventBuffer[storyChunkDims[0]-1].timeStamp);

        attr_DatasetTotalStoryEvents = H5Acreate2(storyDataset, ATTRIBUTE_TOTAL_DATASET_STORY_EVENTS, H5T_NATIVE_ULONG, attr, H5P_DEFAULT, H5P_DEFAULT);
        attrStoryEventsError = H5Awrite(attr_DatasetTotalStoryEvents, H5T_NATIVE_ULONG, &storyChunkDims[0]);
        if(attrStoryEventsError < 0 || attr_DatasetTotalStoryEvents < 0 || attrMinError < 0 || attr_DatasetMin < 0 || attrMaxError < 0 || attr_DatasetMax < 0 || attr < 0 || attr1 < 0 || attrError < 0)
        {
            if(attr_DatasetTotalStoryEvents >= 0)   H5Aclose(attr_DatasetTotalStoryEvents);
            if(attr_DatasetMax >= 0)    H5Aclose(attr_DatasetMax);
            if(attr_DatasetMin >= 0)    H5Aclose(attr_DatasetMin);
            if(attr1 >= 0)  H5Aclose(attr1);
            if(attr >= 0)   H5Sclose(attr);
            if(attributeDataBuffer != NULL) free(attributeDataBuffer);

            if(chunkmetaAttribute >=0 ) H5Tclose(chunkmetaAttribute);
            if(eventType >=0 )  H5Tclose(eventType);
            if(storyDataset >= 0)   H5Dclose(storyDataset);
            if(ChronicleH5 >= 0)  H5Fclose(ChronicleH5);
            return 1;
        }
    }

    /*
    * Release resources
    */
    if(attributeDataBuffer != NULL) free(attributeDataBuffer);
    if(eventBuffer != NULL) free(eventBuffer);

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
}
