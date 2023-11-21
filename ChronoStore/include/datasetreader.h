#include <iostream>
#include <vector>
#include <event.h>

#ifndef DATASET_READER_H
#define DATASET_READER_H


typedef struct DatasetReader
{
    int status;
    std::vector <Event> eventData;

    DatasetReader(int initialStatus, const std::vector <Event> &initialMinMax)
    {
        status = initialStatus;
        eventData = initialMinMax;
    }
} DatasetReader;

#endif // DATASET_READER_H