//
// Created by kfeng on 7/6/23.
//

#ifndef CHRONOLOG_STORY_CHUNK_TEST_UTILS_H
#define CHRONOLOG_STORY_CHUNK_TEST_UTILS_H

#include <random>
#include <StoryChunk.h>

#define CHRONICLE_NAME "Ares_Monitoring"
#define STORY_NAME "CPU_Utilization"
#define CLIENT_ID 1

/**
 * @brief Generate a StoryChunk with random events with random Event size following normal distribution
 * @param story_id: StoryID
 * @param client_id: ClientID
 * @param start_time: Start time of the StoryChunk
 * @param time_step: Time step between events
 * @param end_time: End time of the StoryChunk
 * @param mean_event_size: Mean Event size
 * @param min_event_size: Minimum Event size
 * @param max_event_size: Maximum Event size
 * @param stddev: Standard deviation of Event size
 * @param num_events_per_story_chunk: Number of events in the StoryChunk
 * @param total_event_size: Total size of all events in the StoryChunk
 * @return a StoryChunk
 */
chronolog::StoryChunk
generateStoryChunk(uint64_t story_id, chronolog::ClientId client_id, uint64_t start_time, uint64_t time_step
                   , uint64_t end_time, uint64_t mean_event_size, uint64_t min_event_size, uint64_t max_event_size
                   , double stddev, uint64_t num_events_per_story_chunk, uint64_t &total_event_size)
{
    chronolog::StoryChunk story_chunk(CHRONICLE_NAME, STORY_NAME, story_id, start_time, end_time);
    std::random_device rd;
    std::mt19937_64 generator(rd());
    std::normal_distribution <double> normal_dist(mean_event_size, stddev);

    for(int i = 0; i < num_events_per_story_chunk; ++i)
    {
        chronolog::EventSequence event_sequence = chronolog::EventSequence(start_time, client_id, i);
        chronolog::LogEvent event;
        event.storyId = story_id;
        event.clientId = client_id;
        event.eventTime = start_time + i * time_step;
        event.eventIndex = i;
        uint64_t string_size = static_cast<int>(normal_dist(generator));
        if(string_size < min_event_size) string_size = min_event_size;
        if(string_size > max_event_size) string_size = max_event_size;
        total_event_size += string_size;
        std::string random_str;
        for(int j = 0; j < string_size; ++j)
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
 * @param client_id: ClientID
 * @param start_time: Start time of the StoryChunk
 * @param time_step: Time step between events
 * @param end_time: End time of the StoryChunk
 * @param mean_event_size: Mean Event size
 * @param min_event_size: Minimum Event size
 * @param max_event_size: Maximum Event size
 * @param stddev: Standard deviation of Event size
 * @param num_events_per_story_chunk: Number of events in the StoryChunk
 * @param num_story_chunks: Number of chunks to generate
 * @return a map of StoryChunks
 */
std::map <uint64_t, chronolog::StoryChunk>
generateStoryChunks(uint64_t story_id, chronolog::ClientId client_id, uint64_t start_time, uint64_t time_step
                    , uint64_t end_time, uint64_t mean_event_size, uint64_t min_event_size, uint64_t max_event_size
                    , double stddev, uint64_t num_events_per_story_chunk, uint64_t num_story_chunks)
{
    std::map <uint64_t, chronolog::StoryChunk> story_chunks;
    uint64_t total_event_size = 0;
    for(int i = 0; i < num_story_chunks; ++i)
    {
        uint64_t chunk_start_time = start_time + i * num_events_per_story_chunk * time_step;
        uint64_t chunk_end_time = chunk_start_time + num_events_per_story_chunk * time_step;
        chronolog::StoryChunk story_chunk = generateStoryChunk(story_id, client_id, chunk_start_time, time_step
                                                               , chunk_end_time, mean_event_size, min_event_size
                                                               , max_event_size, stddev, num_events_per_story_chunk
                                                               , total_event_size);
        story_chunks.emplace(chunk_start_time, story_chunk);
    }
    std::cout << "Total data size: " << total_event_size << std::endl;
    return story_chunks;
}

#endif //CHRONOLOG_STORY_CHUNK_TEST_UTILS_H
