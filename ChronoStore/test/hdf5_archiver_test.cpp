#include <iostream>
#include <vector>
#include <map>
#include <string>
#include <chrono>
#include <random>
#include <hdf5.h>
#include <storywriter.h>
#include <storyreader.h>

#define ATTRIBUTE_CHUNKMETADATA "ChunkMetadata"
#define ATTRIBUTE_DATASET_MIN "Min"
#define ATTRIBUTE_DATASET_MAX "Max"
#define ATTRIBUTE_CHUNKMETADATA_RANK 1
#define ATTRIBUTE_TOTAL_DATASET_STORY_EVENTS "TotalStoryEvents"
#define STORY "S1"
#define CHRONICLE "C1.h5"
#define NUM_OF_TESTS 200

#define CLIENT_ID 1
#define CHRONICLE_NAME "Ares_Monitoring"
#define STORY_NAME "CPU_Utilization"
#define STORY_ID 2378540293847398

storyreader sr;

void testWriteOperation(const std::map<uint64_t, chronolog::StoryChunk>& story_chunk_map,
                        std::string &chronicle_name,
                        std::string &story_name)
{
    storywriter writer;
    writer.writeStoryChunks(story_chunk_map, chronicle_name);
}

void testStoryRange()
{
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

/**
 * @brief Generate a StoryChunk with random events with random Event size following normal distribution
 * @param story_id: StoryID
 * @param start_time: Start time of the StoryChunk
 * @param time_step: Time step between events
 * @param end_time: End time of the StoryChunk
 * @param mean_event_size: Mean Event size
 * @param min_event_size: Minimum Event size
 * @param max_event_size: Maximum Event size
 * @param stddev: Standard deviation of Event size
 * @param num_events: Number of events in the StoryChunk
 * @return a StoryChunk
 */
chronolog::StoryChunk
generateStoryChunk(uint64_t story_id, uint64_t start_time, uint64_t time_step, uint64_t end_time,
                   uint64_t mean_event_size, uint64_t min_event_size, uint64_t max_event_size, double stddev,
                   uint64_t num_events)
{
    chronolog::StoryChunk story_chunk(story_id, start_time, end_time);
    std::random_device rd;
    std::mt19937_64 generator(rd());
    std::normal_distribution<double> normal_dist(mean_event_size, stddev);

    for (int i = 0; i < num_events; ++i)
    {
        chronolog::EventSequence event_sequence = chronolog::EventSequence(start_time, CLIENT_ID, i);
        chronolog::LogEvent event;
        event.storyId = story_id;
        event.clientId = CLIENT_ID;
        event.eventTime = start_time + i * time_step;
        event.eventIndex = i;
        uint64_t string_size = static_cast<int>(normal_dist(generator));
        if (string_size < min_event_size) string_size = min_event_size;
        if (string_size > max_event_size) string_size = max_event_size;
        std::string random_str;
        for (int j = 0; j < string_size; ++j)
        {
            char random_char = static_cast<char>(generator() % 26 + 'A');
            random_str.push_back(random_char);
        }
        event.logRecord = random_str;
        story_chunk.insertEvent(event);
    }
    return story_chunk;
}

/**
 * @brief Generate a map of StoryChunks with random events with random Event size following normal distribution
 * @param story_id: StoryID
 * @param start_time: Start time of the StoryChunk
 * @param time_step: Time step between events
 * @param end_time: End time of the StoryChunk
 * @param mean_event_size: Mean Event size
 * @param min_event_size: Minimum Event size
 * @param max_event_size: Maximum Event size
 * @param stddev: Standard deviation of Event size
 * @param num_events_per_chunk: Number of events in the StoryChunk
 * @param num_chunks: Number of chunks to generate
 * @return a map of StoryChunks
 */
std::map<uint64_t, chronolog::StoryChunk>
generateStoryChunks(uint64_t story_id, uint64_t start_time, uint64_t time_step, uint64_t end_time,
                    uint64_t mean_event_size, uint64_t min_event_size, uint64_t max_event_size, double stddev,
                    uint64_t num_events_per_chunk, uint64_t num_chunks)
{
    std::map<uint64_t, chronolog::StoryChunk> story_chunks;
    for (int i = 0; i < num_chunks; ++i)
    {
        uint64_t chunk_start_time = start_time + i * num_events_per_chunk * time_step;
        uint64_t chunk_end_time = chunk_start_time + num_events_per_chunk * time_step;
        chronolog::StoryChunk story_chunk = generateStoryChunk(story_id,
                                                               chunk_start_time,
                                                               time_step,
                                                               chunk_end_time,
                                                               mean_event_size,
                                                               min_event_size,
                                                               max_event_size,
                                                               stddev,
                                                               num_events_per_chunk);
        story_chunks.emplace(chunk_start_time, story_chunk);
    }
    return story_chunks;
}

/**
 * @param argv[1]: number of StoryChunks
 * @param argv[2]: number of Events in each StoryChunk
 * @param argv[3]: mean Event size
 * @param argv[4]: minimum Event size
 * @param argv[5]: maximum Event size
 * @param argv[6]: standard deviation of Event size
 */
int main(int argc, char* argv[])
{
    if (argc < 8)
    {
        std::cout << "Usage: " << argv[0]
                  << " #StoryChunks #EventsInEachChunk meanEventSize minEventSize maxEventSize stddev"
                  << " startTime endTime" << std::endl;
        return 1;
    }
    std::random_device rd;
    std::mt19937_64 rng(rd());
    std::uniform_int_distribution<uint64_t> dist(0, UINT64_MAX);
    uint64_t num_story_chunks = strtol(argv[1], nullptr, 10);
    uint64_t num_events_per_story_chunk = strtol(argv[2], nullptr, 10);
    uint64_t mean_event_size = strtol(argv[3], nullptr, 10);
    uint64_t min_event_size = strtol(argv[4], nullptr, 10);
    uint64_t max_event_size = strtol(argv[5], nullptr, 10);
    double stddev = strtod(argv[6], nullptr);
    uint64_t start_time = strtol(argv[7], nullptr, 10);
    uint64_t end_time = strtol(argv[8], nullptr, 10);
    std::string chronicle_name = CHRONICLE_NAME;
    std::string story_name = STORY_NAME;
    uint64_t story_id = dist(rng);
    uint64_t client_id = CLIENT_ID;

    std::cout << "StoryID: " << story_id << std::endl
              << "#StoryChunks: " << num_story_chunks << std::endl
              << "#EventsInEachChunk: " << num_events_per_story_chunk << std::endl
              << "meanEventSize: " << mean_event_size << std::endl
              << "minEventSize: " << min_event_size << std::endl
              << "maxEventSize: " << max_event_size << std::endl
              << "stddev: " << stddev << std::endl
              << "startTime: " << start_time << std::endl
              << "endTime: " << end_time << std::endl;

    // Generate StoryChunks
    std::cout << "Generating StoryChunks..." << std::endl;
    uint64_t time_step = (end_time - start_time) / num_events_per_story_chunk;
    std::map<uint64_t, chronolog::StoryChunk> story_chunk_map = generateStoryChunks(story_id,
                                                                                    start_time,
                                                                                    time_step,
                                                                                    end_time,
                                                                                    mean_event_size,
                                                                                    min_event_size,
                                                                                    max_event_size,
                                                                                    stddev,
                                                                                    num_events_per_story_chunk,
                                                                                    num_story_chunks);

    // Test write operation
    std::cout << "Testing write operation..." << std::endl;
    testWriteOperation(story_chunk_map, chronicle_name, story_name);

    return 0;
}
