#include <vector>
#include <event.h>

#ifndef LEGACY_STORY_WRITER_H
#define LEGACY_STORY_WRITER_H

class LegacyStoryWriter
{
private:
    /* data */

public:
    LegacyStoryWriter(/* args */);

    ~LegacyStoryWriter();

    int writeStoryChunk(std::vector<Event>* storyChunk, const char* DATASET_NAME, const char* H5FILE_NAME);
};

#endif
