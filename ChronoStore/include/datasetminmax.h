#include<iostream>
#include<vector>

#ifndef DATASET_MIN_MAX_H
#define DATASET_MIN_MAX_H


typedef struct DatasetMinMax
{
    int status;
    std::pair <uint64_t, uint64_t> MinMax;

    DatasetMinMax(int initialStatus, const std::pair <uint64_t, uint64_t> &initialMinMax)
    {
        status = initialStatus;
        MinMax = initialMinMax;
    }

} DatasetMinMax;

#endif // DATASET_MIN_MAX_H