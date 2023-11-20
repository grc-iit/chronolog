#include<iostream>

#ifndef CHUNK_ATTR_H
#define CHUNK_ATTR_H


typedef struct ChunkAttr
{
    uint64_t startTimeStamp;
    uint64_t endTimeStamp;
    uint datasetIndex;
    uint totalChunkEvents;
} ChunkAttr;

#endif // CHUNK_ATTR_H