#include <iostream>
#include <vector>

#define H5_SIZEOF_SSIZE_T H5_SIZEOF_LONG_LONG

#include <hdf5.h>
#include <fstream>
#include <string>
#include <StoryReader.h>
#include <StoryWriter.h>
#include <event.h>
#include <chrono>
#include<vector>
#include <chunkattr.h>
#include <datasetreader.h>
#include <datasetminmax.h>
#include <cstring>


#define ATTRIBUTE_CHUNKMETADATA "ChunkMetadata"
#define ATTRIBUTE_DATASET_MIN "Min"
#define ATTRIBUTE_DATASET_MAX "Max"
#define ATTRIBUTE_CHUNKMETADATA_RANK 1
#define ATTRIBUTE_TOTAL_DATASET_STORY_EVENTS "TotalStoryEvents"
#define STORY "S1"
#define CHRONICLE "C1.h5"
#define NUM_OF_TESTS 200

StoryReader sr;

void testWriteOperation(std::string fileName){
    std::vector<Event> chunk;
    __uint64_t time;
    std::string da;

    std::ifstream filePointer(fileName);
    filePointer.seekg(0, std::ios::beg);

    /*
    * Read data from file
    */
    Event e;
    while (filePointer >> time >> da) {
        e.timeStamp = time;
        strncpy(e.data, da.c_str(), sizeof(e.data) - 1);
        e.data[sizeof(e.data) - 1] = '\0';
        chunk.push_back(e);
    }
    filePointer.close();   
    int status;

    //Write to dataset
    StoryWriter sw;
    status = sw.writeStoryChunk(&chunk, STORY, CHRONICLE);
    if(!status)
    {
        std::cout<<"Data chunk written to story "<<STORY<<" dataset for chunk " <<fileName<<" of Chronicle "<<CHRONICLE<<std::endl;
    }
    else
    {
        std::cout<<"Error occured while writing data to dataset"<<std::endl;
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

int testReadOperation() {

    std::cout<<"Executing read test on story "<<STORY<<" of Chronicle "<<CHRONICLE<<" \nTotal read requests: "<<NUM_OF_TESTS<<std::endl;
    std::vector<std::pair<uint64_t, uint64_t>> range;
    DatasetMinMax d = sr.readDatasetRange(STORY, CHRONICLE);
    if(d.status != 0){
        std::cout<<"Error retrieving min max range for dataset story: " << STORY<<std::endl; 
        return -1;
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
            return -1;
        }
        totalDataRead += dr.eventData.size()*sizeof(Event);
    }

    // Calculate the duration in seconds
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(end_time - start_time).count();
    std::cout << "\nTime taken: " << duration << " seconds\nRead bandwidth: "<<(totalDataRead/(1024*1024*duration)) << " MB/second\n";


    int result = remove(CHRONICLE);
    if(result != 0) {
        std::cout << CHRONICLE << " not found!" <<std::endl;
        return -1;
    }
    return 0;
}

void generateData(std::string fileName, int64_t startTimeStamp){

    int64_t num_entries = 100000; // each chunk generates 100000 events
    std::vector<std::pair<int64_t, std::string>> entries(num_entries);
    int64_t num = startTimeStamp;

    // Const string in all entries for testing purpose
    std::string str = "R7YYRjIL82qc1jHXk80GXGvzmaGS4e5U2kzyfhpdbkxbY1fXUcfoBdGpXsE5AdjjViRe1e4519bZhQjn2zvZKC64g6B309k6l6wx";
    int j = 0;
    for (int i = 0; i < num_entries; i++) {
    entries[i] = std::make_pair(num, str);
    num+=2;
    }

    // Open file for writing
    std::ofstream out_file(fileName, std::ios::out | std::ios::app);

    // Append sorted entries to file
    for (const auto& entry : entries) {
    out_file << entry.first << " " << entry.second << " ";
    }

    // Close file
    out_file.close();
}
/**
 * @param argv[1]: storyChunksCount
 * @param argv[2]: startTimeStamp
 */
int main(int argc, char* argv[]) {
// The test program generates five chunk data containing 1000000 events in every chunk and writes to the storydataset for the chronicle H5. further the read and datarange queries are executed to test.

    if(argc < 2){
        std::cout<<"Check parameters\n@param argv[1]: storyChunksCount\n@param argv[2]: startTimeStamp\n";
        return 1;
    }
    int storyChunksCount = std::atoi(argv[1]);
    std::string filename = "testChunk";
    int64_t startTimeStamp = std::atoi(argv[2]);

    if(storyChunksCount < 0 || startTimeStamp < 0){
        std::cout<<"Enter valid value\n@param argv[1]: storyChunksCount\n@param argv[2]: startTimeStamp\n";
        return 1;
    }
    std::cout<<"Story chunk count: "<<storyChunksCount<<std::endl;
    for(int count = 0 ; count < storyChunksCount ; count++){

        //generate story chunk 
        std::string chunkname = filename + std::to_string(count);
        generateData(chunkname, startTimeStamp);

        // Write to dataset
        testWriteOperation(chunkname);
        int result = remove(chunkname.c_str());
        startTimeStamp += 110000;
    }
    std::cout << "Write to story dataset operation finished" << std::endl;

    // Retrieve story dataset min and max timestamp
    std::cout << "\nExecuting request to read story Min and Max timeStamp" << std::endl;
    testStoryRange();

    // Sequential reads
    int ret = testReadOperation();
    if(ret != 0){
        std::cout << "chronostore_test failed!" << std::endl;
    }
    else{
        std::cout << "chronostore_test finished!" << std::endl;
    }
    

    return 0;
}
