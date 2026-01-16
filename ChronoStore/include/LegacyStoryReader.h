#include <cstdint>
#include <utility>
#include <event.h>
#include <datasetreader.h>
#include <datasetminmax.h>

#ifndef LEGACY_STORY_READER_H
#define LEGACY_STORY_READER_H

class LegacyStoryReader
{
private:
    /* data */
public:
    LegacyStoryReader(/* args */);

    ~LegacyStoryReader();

    // Read data from H5 dataset
    DatasetReader readFromDataset(std::pair<uint64_t, uint64_t> range, const char* STORY, const char* CHRONICLE);

    // Read dataset data range i.e Min and Max
    DatasetMinMax readDatasetRange(const char* STORY, const char* CHRONICLE);
};

#endif
