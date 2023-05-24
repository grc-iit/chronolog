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
#include <chunkattr.h>
#include <datasetreader.h>
#include <datasetminmax.h>
#include <cstring>


#define ATTRIBUTE_CHUNKMETADATA "ChunkMetadata"
#define ATTRIBUTE_DATASET_MIN "Min"
#define ATTRIBUTE_DATASET_MAX "Max"
#define ATTRIBUTE_CHUNKMETADATA_RANK 1
#define ATTRIBUTE_TOTAL_DATASET_STORY_EVENTS "TotalStoryEvents"
#define FILE_NAME "C1S1C1.txt"
#define STORY "S1"
#define CHRONICLE "C1.h5"
#define NUM_OF_TESTS 2000

storyreader sr;

void testWriteOperation(){
    std::vector<Event> storyChunk;
    __uint64_t time;
    std::string da;

    std::ifstream filePointer(FILE_NAME);
    filePointer.seekg(0, std::ios::beg);

    /*
    * Read data from file
    */
    Event e;
    while (filePointer >> time >> da) {
        e.timeStamp = time;
        strcpy(e.data, da.c_str());
        storyChunk.push_back(e);
    }
    filePointer.close();    
    int status;
    
    //Write to dataset
    storywriter sw;
    status = sw.writeStoryChunk(storyChunk, STORY, CHRONICLE);
    if(!status)
    {
        std::cout<<"\nData written to file sucessfully!\n"<<std::endl;
    }
    else
    {
        std::cout<<"\nError occured while writing data to dataset\n"<<std::endl;
    }
}

void testStoryRange(){
    // Read Min and Max attribute from dataset
    DatasetMinMax d = sr.readDatasetRange(STORY, CHRONICLE);
    if(d.status == 0){
        std::cout << "Min Timestamp: " << d.MinMax.first << " Max Timestamp: " << d.MinMax.second << std::endl;
    }
    else{
        std::cout<<"Error retrieving min max for story: " << STORY<<std::endl; 
    }
}

void testReadOperation() {

    std::cout<<"Executing read test on story "<<STORY<<" of Chronicle "<<CHRONICLE<<" \nTotal read requests: "<<NUM_OF_TESTS<<std::endl;
    std::vector<std::pair<uint64_t, uint64_t>> range;
    DatasetMinMax d = sr.readDatasetRange(STORY, CHRONICLE);
    if(d.status != 0){
        std::cout<<"Error retrieving min max range for dataset story: " << STORY<<std::endl; 
    }

    for(int64_t i = 0 ; i < NUM_OF_TESTS ; i++){

        auto seed = std::chrono::system_clock::now().time_since_epoch().count();
        srand(seed);

        int64_t start_val = rand() % (d.MinMax.second - d.MinMax.first + 1) + d.MinMax.first;
        int64_t end_val = rand() % (d.MinMax.second - start_val + 1) + start_val;
        range.push_back(std::make_pair(start_val, end_val));
    }

    int64_t totalDataRead = 0;
    auto start_time = std::chrono::high_resolution_clock::now();
    for(int i = 0 ; i < range.size() ; i++){
        DatasetReader dr = sr.readFromDataset(range[i], STORY, CHRONICLE);
        if(dr.status != 0 || dr.eventData.size() == 0){
            // std::cout<<"Dataset read test failed on test: "<<i+1<<"\t"<<range[i].first<<"\t"<<range[i].second<<std::endl;
            return;
        }
        totalDataRead += dr.eventData.size()*sizeof(Event);
    }

    // Calculate the duration in seconds
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(end_time - start_time).count();
    std::cout << "\nTime taken: " << duration << " seconds\nRead bandwidth: "<<(totalDataRead/(1024*1024*duration)) << " MB/second\n";


    int result = remove(CHRONICLE);

    return ;
}


int main(int argc, char* argv[]) {

    // Write to dataset
    testWriteOperation();

    // Retrieve story dataset min and max timestamp
    testStoryRange();

    // Sequential reads
    testReadOperation();
    

    return 0;
}
