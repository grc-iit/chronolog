#include <iostream>
#include <vector>

#define H5_SIZEOF_SSIZE_T H5_SIZEOF_LONG_LONG

#include <hdf5.h>
#include <fstream>
#include <string>
#include <storyreader.h>
#include <storywriter.h>
#include <event.h>
#include <chrono>
#include <vector>
#include <chunkattr.h>
#include <datasetreader.h>
#include <datasetminmax.h>
#include <cstring>
#include "log.h"


#define ATTRIBUTE_CHUNKMETADATA "ChunkMetadata"
#define ATTRIBUTE_DATASET_MIN "Min"
#define ATTRIBUTE_DATASET_MAX "Max"
#define ATTRIBUTE_CHUNKMETADATA_RANK 1
#define ATTRIBUTE_TOTAL_DATASET_STORY_EVENTS "TotalStoryEvents"
#define STORY "S1"
#define CHRONICLE "C1.h5"
#define NUM_OF_TESTS 200

storyreader sr;

void testWriteOperation(std::string fileName)
{
    std::vector <Event> chunk;
    __uint64_t time;
    std::string da;

    std::ifstream filePointer(fileName);
    if(!filePointer.is_open())
    {
        LOGE("[TestReadWrite] Failed to open file: {}", fileName);
        return;
    }
    filePointer.seekg(0, std::ios::beg);

    /*
    * Read data from file
    */
    Event e;
    while(filePointer >> time >> da)
    {
        e.timeStamp = time;
        strncpy(e.data, da.c_str(), sizeof(e.data) - 1);
        e.data[sizeof(e.data) - 1] = '\0';
        chunk.push_back(e);
    }
    filePointer.close();
    int status;

    //Write to dataset
    storywriter sw;
    status = sw.writeStoryChunk(&chunk, STORY, CHRONICLE);
    if(!status)
    {
        LOGI("[TestReadWrite] Successfully wrote data from file {} to the story dataset for Chronicle {}.", fileName
             , CHRONICLE);
    }
    else
    {
        LOGE("[TestReadWrite] Failed to write data from file {} to the story dataset for Chronicle {}. Error code: {}"
             , fileName, CHRONICLE, status);
    }
}

void testStoryRange()
{
    // Read Min and Max attribute from dataset
    DatasetMinMax d = sr.readDatasetRange(STORY, CHRONICLE);
    if(d.status == 0)
    {
        LOGI("[TestReadWrite] Successfully retrieved timestamp range for Story {}: Min Timestamp: {}, Max Timestamp: {}"
             , STORY, d.MinMax.first, d.MinMax.second);
    }
    else
    {
        LOGE("[TestReadWrite] Failed to retrieve timestamp range for Story {}. Error details: {}", STORY
             , d.errorMessage);
    }
}

int testReadOperation()
{
    LOGI("[TestReadWrite] Initiating read test for Story: {} of Chronicle: {}. Total read requests to perform: {}"
         , STORY, CHRONICLE, NUM_OF_TESTS);

    DatasetMinMax d = sr.readDatasetRange(STORY, CHRONICLE);
    if(d.status != 0)
    {
        LOGE("[TestReadWrite] Failed to retrieve min-max range for dataset story: {}", STORY);
        return -1;
    }

    std::vector <std::pair <uint64_t, uint64_t>> range;
    for(int64_t i = 0; i < NUM_OF_TESTS; i++)
    {
        auto seed = std::chrono::system_clock::now().time_since_epoch().count();
        srand(seed);

        int64_t start_val = rand() % (d.MinMax.second - d.MinMax.first + 1) + d.MinMax.first;
        int64_t end_val = rand() % (d.MinMax.second - start_val + 1) + start_val;
        range.push_back(std::make_pair(start_val, end_val));
    }

    int64_t totalDataRead = 0;
    auto start_time = std::chrono::high_resolution_clock::now();

    for(int i = 0; i < range.size(); i++)
    {
        DatasetReader dr = sr.readFromDataset(range[i], STORY, CHRONICLE);
        if(dr.status != 0 || dr.eventData.size() == 0)
        {
            LOGE("[TestReadWrite] Read test failed for test index: {}. Range: [{}, {}]", i + 1, range[i].first
                 , range[i].second);
            return -1;
        }
        totalDataRead += dr.eventData.size() * sizeof(Event);
    }

    // Calculate the duration in seconds
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast <std::chrono::seconds>(end_time - start_time).count();
    LOGI("[TestReadWrite] Read tests completed successfully.\nTime taken: {} seconds\nRead bandwidth: {} MB/second"
         , duration, (totalDataRead / (1024 * 1024 * duration)));

    int result = remove(CHRONICLE);
    if(result != 0)
    {
        LOGW("[TestReadWrite] Failed to remove chronicle: {}. Proceeding...", CHRONICLE);
        return -1;
    }
    return 0;
}

void generateData(std::string fileName, int64_t startTimeStamp)
{

    int64_t num_entries = 100000; // each chunk generates 100000 events
    std::vector <std::pair <int64_t, std::string>> entries(num_entries);
    int64_t num = startTimeStamp;

    // Const string in all entries for testing purpose
    std::string str = "R7YYRjIL82qc1jHXk80GXGvzmaGS4e5U2kzyfhpdbkxbY1fXUcfoBdGpXsE5AdjjViRe1e4519bZhQjn2zvZKC64g6B309k6l6wx";
    int j = 0;
    for(int i = 0; i < num_entries; i++)
    {
        entries[i] = std::make_pair(num, str);
        num += 2;
    }

    // Open file for writing
    std::ofstream out_file(fileName, std::ios::out|std::ios::app);
    if(!out_file.is_open())
    {
        LOGE("[TestReadWrite] Failed to open file '{}' for writing.", fileName);
        return; // Return if file opening fails.
    }

    // Append sorted entries to file
    for(const auto &entry: entries)
    {
        out_file << entry.first << " " << entry.second << " ";
    }

    // Close file
    out_file.close();
    LOGI("[TestReadWrite] Successfully generated and appended {} entries to file '{}'.", num_entries, fileName);
}

/**
 * @param argv[1]: storyChunksCount
 * @param argv[2]: startTimeStamp
 */
int main(int argc, char*argv[])
{
    // The test program generates five chunk data containing 1000000 events in every chunk and writes to the
    // storydataset for the chronicle H5. further the read and datarange queries are executed to test.
    if(argc < 2)
    {
        LOGE("[TestReadWrite] Insufficient arguments provided. Expected: storyChunksCount startTimeStamp");
        return 1;
    }

    int storyChunksCount = std::atoi(argv[1]);
    int64_t startTimeStamp = std::atoi(argv[2]);

    if(storyChunksCount < 0 || startTimeStamp < 0)
    {
        LOGE("[TestReadWrite] Invalid arguments provided. Ensure storyChunksCount is positive and startTimeStamp >= 0.");
        return 1;
    }
    LOGI("[TestReadWrite] Starting tests with {} story chunks.", storyChunksCount);

    for(int count = 0; count < storyChunksCount; count++)
    {
        //generate story chunk 
        std::string chunkname = "testChunk" + std::to_string(count);
        generateData(chunkname, startTimeStamp);

        // Write to dataset
        testWriteOperation(chunkname);
        int result = remove(chunkname.c_str());
        if(result != 0)
        {
            LOGW("[TestReadWrite] Failed to remove chunk: {}", chunkname);
        }
        startTimeStamp += 110000;
    }

    LOGI("[TestReadWrite] Completed writing data to story dataset.");

    // Retrieve and log the story dataset's min and max timestamps.
    LOGI("[TestReadWrite] Retrieving and logging story Min and Max timestamps.");
    testStoryRange();

    // Sequential reads
    int ret = testReadOperation();
    if(ret != 0)
    {
        LOGE("[TestReadWrite] Sequential read test failed!");
    }
    else
    {
        LOGI("[TestReadWrite] Sequential read test completed successfully.");
    }

    return 0;
}
