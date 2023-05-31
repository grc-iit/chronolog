#include<iostream>
#include<vector>
#include<event.h>

#ifndef WRITE_H
#define WRITE_H

class storywriter
{
private:
    /* data */

public:
    storywriter(/* args */);
    ~storywriter();

    int writeStoryChunk(std::vector<Event> *storyChunk, const char* DATASET_NAME, const char* H5FILE_NAME);

};

#endif