//
// Created by kfeng on 7/27/23.
//

#include <iostream>
#include <random>
#include <vector>
#include <unordered_map>
#include <map>
#include <filesystem>
#include <sys/stat.h>
#include <unistd.h>
#include <cassert>
#include <hdf5.h>
#include <chrono_monitor.h>
#include <chronolog_errcode.h>
//#include <story_chunk_test_utils.h>

#define STORY "S1"
#define CHRONICLE "C1.h5"
#define NUM_OF_TESTS 200
#define DATASET_RANK 1
#define CHRONICLE_ROOT_DIR "/home/kfeng/chronolog_store/"
#define STORY_ID 1234567890
#define CLIENT_ID 1
#define CHRONICLE_NAME "Ares_Monitoring"
#define STORY_NAME "CPU_Utilization"

typedef uint64_t StoryId;
typedef uint64_t ChronicleId;
typedef uint32_t ClientId;
typedef uint64_t chrono_time;
typedef uint32_t chrono_index;
//typedef std::tuple<chrono_time, ClientId, chrono_index> EventSequence;

typedef struct LogEvent
{
    StoryId storyId;
    uint64_t eventTime;
    ClientId clientId;
    uint32_t eventIndex;
//    uint64_t logRecordSize;
    hvl_t logRecord;
} LogEvent;

LogEvent g_events[10];

hid_t genLogEventDataType(hid_t vlen_memtype_id);

typedef struct StoryChunk
{
    StoryId storyId;
    uint64_t startTime;
    uint64_t endTime;
    uint64_t revisionTime;
    uint64_t numLogEvents;
    LogEvent*logEvents;

    [[nodiscard]] uint64_t getStoryID() const
    { return storyId; }

    [[nodiscard]] uint64_t getStartTime() const
    { return startTime; }

    [[nodiscard]] uint64_t getEndTime() const
    { return endTime; }

    [[nodiscard]] uint64_t getNumEvents() const
    { return numLogEvents; }

    [[nodiscard]] uint64_t getRevisionTime() const
    { return revisionTime; }

    [[nodiscard]] LogEvent*getLogEvents() const
    { return logEvents; }

    [[nodiscard]] size_t getTotalPayloadSize() const
    {
        size_t total_size = 0;
        for(uint64_t i = 0; i < numLogEvents; i++)
        { total_size += logEvents[i].logRecord.len; }
        return total_size;
    }
} StoryChunk;

StoryChunk*generateStoryChunkArray(uint64_t story_id, uint64_t client_id, uint64_t start_time, uint64_t time_step
                                   , uint64_t end_time, uint64_t mean_event_size, uint64_t min_event_size
                                   , uint64_t max_event_size, double stddev, uint64_t num_events_per_story_chunk
                                   , uint64_t num_story_chunks)
{
    auto*story_chunk_array = new StoryChunk[num_story_chunks];
    std::random_device rd;
    std::mt19937_64 rng(rd());
    std::normal_distribution <double> event_size_dist((double)mean_event_size, stddev);

    for(uint64_t i = 0; i < num_story_chunks; i++)
    {
        story_chunk_array[i].storyId = story_id;
        story_chunk_array[i].startTime = start_time;
        story_chunk_array[i].endTime = end_time;
        story_chunk_array[i].revisionTime = end_time;
        story_chunk_array[i].numLogEvents = num_events_per_story_chunk;
        story_chunk_array[i].logEvents = new LogEvent[num_events_per_story_chunk];

        for(uint64_t j = 0; j < num_events_per_story_chunk; j++)
        {
            story_chunk_array[i].logEvents[j].storyId = story_id;
            story_chunk_array[i].logEvents[j].eventTime = start_time + j * time_step;
            story_chunk_array[i].logEvents[j].clientId = client_id;
            story_chunk_array[i].logEvents[j].eventIndex = j;
            auto log_record_size = (size_t)event_size_dist(rng);
            log_record_size = log_record_size < min_event_size ? min_event_size : log_record_size > max_event_size
                                                                                  ? max_event_size : log_record_size;
            story_chunk_array[i].logEvents[j].logRecord.len = log_record_size;
            story_chunk_array[i].logEvents[j].logRecord.p = new uint8_t[log_record_size + 1];
            for(uint64_t k = 0; k < log_record_size; k++)
            {
                ((uint8_t*)story_chunk_array[i].logEvents[j].logRecord.p)[k] = (j + k + 1) % 255;
            }
//            ((uint8_t *)story_chunk_array[i].logEvents[j].logRecord.p)[log_record_size] = '\0';
        }

        story_id++;
    }

    return story_chunk_array;
}

std::map <uint64_t, StoryChunk>
generateStoryChunkMap(uint64_t story_id, uint64_t client_id, uint64_t start_time, uint64_t time_step, uint64_t end_time
                      , uint64_t mean_event_size, uint64_t min_event_size, uint64_t max_event_size, double stddev
                      , uint64_t num_events_per_story_chunk, uint64_t num_story_chunks)
{
    std::map <uint64_t, StoryChunk> story_chunk_map;
    std::random_device rd;
    std::mt19937_64 rng(rd());
    std::normal_distribution <double> event_size_dist((double)mean_event_size, stddev);

    for(uint64_t i = 0; i < num_story_chunks; i++)
    {
        StoryChunk story_chunk;
        story_chunk.storyId = story_id;
        story_chunk.startTime = start_time;
        story_chunk.endTime = end_time;
        story_chunk.revisionTime = end_time;
        story_chunk.numLogEvents = num_events_per_story_chunk;
        story_chunk.logEvents = new LogEvent[num_events_per_story_chunk];

        for(uint64_t j = 0; j < num_events_per_story_chunk; j++)
        {
            story_chunk.logEvents[j].storyId = story_id;
            story_chunk.logEvents[j].eventTime = start_time + j * time_step;
            story_chunk.logEvents[j].clientId = client_id;
            story_chunk.logEvents[j].eventIndex = j;
            auto log_record_size = (size_t)event_size_dist(rng);
            log_record_size = log_record_size < min_event_size ? min_event_size : log_record_size > max_event_size
                                                                                  ? max_event_size : log_record_size;
            story_chunk.logEvents[j].logRecord.len = log_record_size;
            story_chunk.logEvents[j].logRecord.p = new uint8_t[log_record_size + 1];
            for(uint64_t k = 0; k < log_record_size; k++)
            {
                ((uint8_t*)story_chunk.logEvents[j].logRecord.p)[k] = (j + k + 1) % 255;
            }
//            ((uint8_t *)story_chunk.logEvents[j].logRecord.p)[log_record_size] = '\0';
        }
        story_chunk_map.emplace(story_id, story_chunk);

        story_id++;
    }

    return story_chunk_map;
}

int writeStoryChunks(std::map <uint64_t, StoryChunk> &story_chunk_map, StoryChunk*story_chunk_array
                     , const std::string &chronicle_dir, hvl_t*wdata)
{
    herr_t status;
    uint64_t story_id;
    auto story_chunk_map_it = story_chunk_map.begin();
    std::unordered_map <uint64_t, hid_t> story_chunk_fd_map;
    LOG_INFO("Writing StoryChunks to Chronicle ...");
//    for (;
//            story_chunk_map_it != story_chunk_map.end();
//            story_chunk_map_it++)
    for(uint64_t i = 0; i < 1; i++)
    {
//        StoryChunk story_chunk = story_chunk_map_it->second;
        StoryChunk story_chunk = story_chunk_array[i];
        // Check if the Story file has been opened/created already
        hid_t story_file;
        story_id = story_chunk.getStoryID();
        std::string story_file_name = chronicle_dir + "/" + std::to_string(story_id) + ".h5";
        story_file = H5Fcreate(story_file_name.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
        LOG_DEBUG("H5Fcreate returns: {}", story_file);
//        if (story_chunk_fd_map.find(story_id) != story_chunk_fd_map.end())
//        {
//            story_file = story_chunk_fd_map.find(story_id)->second;
//            LOG_DEBUG("Story file already opened: {}", story_file_name.c_str());
//        }
        // Story file does not exist, create the Story file if it does not exist
//        else if (stat(story_file_name.c_str(), &st) != 0)
//        {
//            story_file = H5Fcreate(story_file_name.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
//            if (story_file == H5I_INVALID_HID)
//            {
//                LOG_ERROR("Failed to create story file: {}", story_file_name.c_str());
//                return chronolog::CL_ERR_UNKNOWN;
//            }
//            story_chunk_fd_map.emplace(story_id, story_file);
//        }
//        else
        {
            // Open the Story file if it exists
            story_file = H5Fopen(story_file_name.c_str(), H5F_ACC_RDWR, H5P_DEFAULT);
            if(story_file < 0)
            {
                LOG_ERROR("Failed to open story file: {}", story_file_name.c_str());
                return chronolog::CL_ERR_UNKNOWN;
            }
//            story_chunk_fd_map.emplace(story_id, story_file);
        }

        // Check if the dataset for the Story Chunk exists
        hid_t story_chunk_dset, story_chunk_dspace;
        std::string story_chunk_dset_name =
                std::to_string(story_chunk.getStartTime()) + "_" + std::to_string(story_chunk.getEndTime());
        if(H5Lexists(story_file, story_chunk_dset_name.c_str(), H5P_DEFAULT) > 0)
        {
            LOG_ERROR("Story chunk already exists: {}", story_chunk_dset_name.c_str());
            return chronolog::CL_ERR_STORY_CHUNK_EXISTS;
        }

        /**
         * H5Tvlen type based implementation
         */
        // Define variable-length payload data type for memory type
        hid_t event_payload_memtype = H5Tvlen_create(H5T_NATIVE_UINT8);
        LOG_DEBUG("Creating event payload memory type using H5Tvlen_create returns: {}", event_payload_memtype);
        hid_t vlen_memtype_id = H5Tcopy(event_payload_memtype);
        LOG_DEBUG("H5Tcopy(event_payload_memtype) returns: {}", vlen_memtype_id);
        // Define LogEvent data type
        LOG_INFO("Generating LogEvent data type for memory type ...");
        hid_t log_event_memtype = genLogEventDataType(vlen_memtype_id);

        // Define variable-length payload data type for file type
        hid_t event_payload_filetype = H5Tvlen_create(H5T_STD_I8LE);
        LOG_DEBUG("Creating event payload file type using H5Tvlen_create returns: {}", event_payload_filetype);
        hid_t vlen_filetype_id = H5Tcopy(event_payload_filetype);
        LOG_DEBUG("H5Tcopy(event_payload_filetype) returns: {}", vlen_filetype_id);
        // Define LogEvent data type
        LOG_INFO("Generating LogEvent data type for file type ...");
        hid_t log_event_filetype = genLogEventDataType(vlen_filetype_id);

        // Create the dataspace for the dataset for the Story Chunk
        const hsize_t story_chunk_dset_dims[1] = {story_chunk.getNumEvents()};
        story_chunk_dspace = H5Screate_simple(DATASET_RANK, story_chunk_dset_dims, nullptr);
        LOG_DEBUG("H5Screate_simple returns: {}", story_chunk_dspace);
        if(story_chunk_dspace < 0)
        {
            LOG_ERROR("Failed to create dataspace for story chunk: {}", story_chunk_dset_name.c_str());
            return chronolog::CL_ERR_UNKNOWN;
        }

        // Create the property list
        hid_t story_chunk_dset_creation_plist = H5Pcreate(H5P_DATASET_CREATE);
        LOG_DEBUG("H5Pcreate returns: {}", story_chunk_dset_creation_plist);

        // Create the dataset for the Story Chunk
        // Set #chunks to number of Events in the Story Chunk
        hsize_t story_chunk_dset_chunk_dims[1] = {story_chunk.getNumEvents()};
        status = H5Pset_chunk(story_chunk_dset_creation_plist, DATASET_RANK, story_chunk_dset_chunk_dims);
        LOG_DEBUG("H5Pset_chunk returns: {}", status);
        LOG_INFO("Creating StoryChunk {} in Story file: {}", story_chunk_dset_name.c_str(), story_file_name.c_str());
        story_chunk_dset = H5Dcreate(story_file, story_chunk_dset_name.c_str(), log_event_filetype, story_chunk_dspace
                                     , H5P_DEFAULT, story_chunk_dset_creation_plist, H5P_DEFAULT);
        status = H5Pclose(story_chunk_dset_creation_plist);
        LOG_DEBUG("H5Pclose returns: {}", status);
        if(story_chunk_dset < 0)
        {
            LOG_ERROR("Failed to create dataset for story chunk: {}", story_chunk_dset_name.c_str());
            return chronolog::CL_ERR_UNKNOWN;
        }

        // Write the Story Chunk to the dataset
        LOG_DEBUG("{}", ((char*)story_chunk.logEvents[0].logRecord.p)[0]);
        LOG_DEBUG("{}", ((char*)story_chunk.logEvents[0].logRecord.p)[7]);
        LOG_DEBUG("{}", ((char*)story_chunk.logEvents[0].logRecord.p)[0]);
        LOG_DEBUG("{}", ((char*)story_chunk.logEvents[0].logRecord.p)[7]);
        LOG_INFO("Writing StoryChunk {} to Story file: {}", story_chunk_dset_name.c_str(), story_file_name.c_str());
//        status = H5Dwrite(story_chunk_dset, H5T_NATIVE_B8, H5S_ALL, H5S_ALL, H5P_DEFAULT,
//                          &story_chunk_map_it->second);
        status = H5Dwrite(story_chunk_dset, log_event_memtype, H5S_ALL, H5S_ALL, H5P_DEFAULT, wdata);
        LOG_DEBUG("H5Dwrite returns: {}", status);
//        free(event_payload_data);
        if(status < 0)
        {
            LOG_ERROR("Failed to write story chunk to dataset: {}", story_chunk_dset_name.c_str());
            // Print the detailed error message
            H5Eprint(H5E_DEFAULT, stderr);

            // Get the error stack and retrieve the error message
            H5Ewalk2(H5E_DEFAULT, H5E_WALK_DOWNWARD
                     , [](unsigned n, const H5E_error2_t*err_desc, void*client_data) -> herr_t
                    {
                        printf("Error #%u: {}\n", n, err_desc->desc);
                        return 0;
                    }, nullptr);
            return chronolog::CL_ERR_UNKNOWN;
        }
        status = H5Dvlen_reclaim(log_event_memtype, story_chunk_dspace, H5P_DEFAULT, &g_events);
        LOG_DEBUG("H5Dvlen_reclaim returns: ", status);

        // Close file, dataset and data space
        status = H5Fclose(story_file);
        LOG_DEBUG("H5Fclose returns: {}", status);
        status = H5Dclose(story_chunk_dset);
        LOG_DEBUG("H5Dclose returns: {}", status);
        status = H5Sclose(story_chunk_dspace);
        LOG_DEBUG("H5Sclose returns: {}", status);
        status = H5Tclose(log_event_memtype);
        LOG_DEBUG("H5Tclose returns: {}", status);
        status = H5Tclose(log_event_filetype);
        LOG_DEBUG("H5Tclose returns: {}", status);
        status = H5Tclose(event_payload_memtype);
        LOG_DEBUG("H5Tclose returns: {}", status);
        status = H5Tclose(vlen_memtype_id);
        LOG_DEBUG("H5Tclose returns: {}", status);
        if(status < 0)
        {
            LOG_ERROR("Failed to close dataset or dataspace or file for story chunk: {}"
                      , story_chunk_dset_name.c_str());
            return chronolog::CL_ERR_UNKNOWN;
        }
    }

    // Close all files at the end
//    for (auto &story_chunk_fd_map_it : story_chunk_fd_map)
//    {
//        status = H5Fclose(story_chunk_fd_map_it.second);
//        if (status < 0)
//        {
//            char *story_file_name = new char[128];
//            size_t file_name_len = 0;
//            H5Fget_name(story_chunk_fd_map_it.second, story_file_name, file_name_len);
//            LOG_ERROR("Failed to close file for story chunk: {}", story_file_name);
//            free(story_file_name);
//            return chronolog::CL_ERR_UNKNOWN;
//        }
//    }
    LOG_INFO("Done writing StoryChunks to Chronicle file ...");
    return chronolog::CL_SUCCESS;
}

hid_t genLogEventDataType(hid_t vlen_memtype_id)
{
    herr_t status;
    hid_t log_event_dtype = H5Tcreate(H5T_COMPOUND, sizeof(LogEvent));
    LOG_DEBUG("H5Tcreate(H5T_COMPOUND, sizeof(LogEvent)) returns: {}", log_event_dtype);
    status = H5Tinsert(log_event_dtype, "storyId", HOFFSET(LogEvent, storyId), H5T_NATIVE_UINT64);
    LOG_DEBUG("H5Tinsert returns: {}", status);
    status = H5Tinsert(log_event_dtype, "eventTime", HOFFSET(LogEvent, eventTime), H5T_NATIVE_UINT64);
    LOG_DEBUG("H5Tinsert returns: {}", status);
    status = H5Tinsert(log_event_dtype, "clientId", HOFFSET(LogEvent, clientId), H5T_NATIVE_UINT32);
    LOG_DEBUG("H5Tinsert returns: {}", status);
    status = H5Tinsert(log_event_dtype, "eventIndex", HOFFSET(LogEvent, eventIndex), H5T_NATIVE_UINT32);
    LOG_DEBUG("H5Tinsert returns: {}", status);
//        status = H5Tinsert(log_event_memtype,
//                           "logRecordSize",
//                           HOFFSET(LogEvent, logRecordSize),
//                           H5T_NATIVE_UINT64);
//        LOG_DEBUG("H5Tinsert returns: {}", status);
    status = H5Tinsert(log_event_dtype, "logRecord", HOFFSET(LogEvent, logRecord), vlen_memtype_id);
    LOG_DEBUG("H5Tinsert returns: {}", status);
    if(status < 0)
    {
        LOG_ERROR("Failed to create data type for log event, status: {}", status);
        return chronolog::CL_ERR_UNKNOWN;
    }
    return log_event_dtype;
}

int readStoryChunks(std::map <uint64_t, StoryChunk> &story_chunk_map, StoryChunk*story_chunk_array
                    , const std::string &chronicle_dir, hvl_t*rdata)
{
    herr_t status;
    uint64_t story_id;
    auto story_chunk_map_it = story_chunk_map.begin();
    std::unordered_map <uint64_t, hid_t> story_chunk_fd_map;
    LOG_INFO("Reading StoryChunks from Chronicle ...");
    // Read StoryChunks
    LOG_INFO("Reading StoryChunks ...");
    for(uint64_t i = 0; i < 1; i++)
    {
//        StoryChunk story_chunk = story_chunk_map_it->second;
        StoryChunk story_chunk = story_chunk_array[i];
        // Open the Story file
        hid_t story_file;
        story_id = story_chunk.getStoryID();
        std::string story_file_name = chronicle_dir + "/" + std::to_string(story_id) + ".h5";
        story_file = H5Fopen(story_file_name.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT);
        LOG_DEBUG("H5Fopen returns: {}", story_file);

        /**
         * H5Tvlen type based implementation
         */
        // Define variable-length payload data type
        hid_t event_payload_memtype = H5Tvlen_create(H5T_NATIVE_UINT8);
        LOG_DEBUG("Creating event payload memory type using H5Tvlen_create returns: {}", event_payload_memtype);
        hid_t vlen_memtype_id = H5Tcopy(event_payload_memtype);
        LOG_DEBUG("H5Tcopy(event_payload_type) returns: {}", vlen_memtype_id);
        // Define LogEvent data type
        LOG_INFO("Generating LogEvent data type for memory type ...");
        hid_t log_event_memtype = genLogEventDataType(vlen_memtype_id);

        // Open the dataset for the Story Chunk
        hid_t story_chunk_dset;
        std::string story_chunk_dset_name =
                std::to_string(story_chunk.getStartTime()) + "_" + std::to_string(story_chunk.getEndTime());
        LOG_INFO("Opening StoryChunk {} in Story file: {}", story_chunk_dset_name.c_str(), story_file_name.c_str());
        story_chunk_dset = H5Dopen(story_file, ("/" + story_chunk_dset_name).c_str(), H5P_DEFAULT);
        LOG_DEBUG("H5Dopen returns: {}", story_chunk_dset);
        if(story_chunk_dset < 0)
        {
            LOG_ERROR("Failed to open dataset for story chunk: {}", story_chunk_dset_name.c_str());
            return chronolog::CL_ERR_UNKNOWN;
        }

        // Get size and prepare memory
        hid_t story_chunk_dspace;
        hsize_t story_chunk_dset_dims[1] = {story_chunk.getNumEvents()};
        story_chunk_dspace = H5Dget_space(story_chunk_dset);
        int ndims = H5Sget_simple_extent_dims(story_chunk_dspace, story_chunk_dset_dims, nullptr);
        LOG_INFO("Allocating memory for {}-D dimensional data in StoryChunk {} in Story file: {}", ndims
                 , story_chunk_dset_name.c_str(), story_file_name.c_str());
        rdata = (hvl_t*)malloc((story_chunk_dset_dims[0] + 1) * sizeof(hvl_t));

        // Read data
        LOG_INFO("Reading StoryChunk {} from Story file: {}", story_chunk_dset_name.c_str(), story_file_name.c_str());
        status = H5Dread(story_chunk_dset, event_payload_memtype, H5S_ALL, H5S_ALL, H5P_DEFAULT, rdata);

        // Cleanup
        status += H5Dvlen_reclaim(log_event_memtype, story_chunk_dspace, H5P_DEFAULT, rdata);
        status += H5Dclose(story_chunk_dset);
        status += H5Sclose(story_chunk_dspace);
        status += H5Tclose(log_event_memtype);
        status += H5Fclose(story_file);
        if(status < 0)
        {
            LOG_ERROR("Failed to close dataset or dataspace or file for story chunk: {}"
                      , story_chunk_dset_name.c_str());
            return chronolog::CL_ERR_UNKNOWN;
        }
    }
    LOG_INFO("Done reading StoryChunks from Chronicle file ...");
    return chronolog::CL_SUCCESS;
}

int main(int argc, char*argv[])
{
    if(argc < 8)
    {
        std::cout << "Usage: " << argv[0]
                  << " #StoryChunks #EventsInEachChunk meanEventSize minEventSize maxEventSize stddev"
                  << " startTime endTime" << std::endl;
        return 1;
    }
    int i, ret, nCount = 10;
    size_t len;
    hvl_t*wdata, *rdata = nullptr;
    char*bptr;
    char*str[] = {"Wake Me up When", "September", "Ends", "Ever tried", "Ever failed", "No matter", "Try again"
                  , "Fail again", "Fail better", "I am the master of my fate"};
//    std::random_device rd;
//    std::mt19937_64 rng(rd());
    std::uniform_int_distribution <uint64_t> dist(0, UINT64_MAX);
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
    uint64_t story_id = STORY_ID; //dist(rng);
    uint64_t client_id = CLIENT_ID;

    int result = chronolog::chrono_monitor::initialize("console", "cmp_vlen_bytes_dtype_test.log", spdlog::level::debug
                                                       , "cmp_vlen_bytes_dtype_test", 102400, 1, spdlog::level::debug);
    if(result == 1)
    {
        exit(EXIT_FAILURE);
    }

    std::cout << "StoryID: " << story_id << std::endl << "#StoryChunks: " << num_story_chunks << std::endl
              << "#EventsInEachChunk: " << num_events_per_story_chunk << std::endl << "meanEventSize: "
              << mean_event_size << std::endl << "minEventSize: " << min_event_size << std::endl << "maxEventSize: "
              << max_event_size << std::endl << "stddev: " << stddev << std::endl << "startTime: " << start_time
              << std::endl << "endTime: " << end_time << std::endl;

    for(i = 0; i < nCount; i++)
    {
        g_events[i].storyId = i + 10;
        g_events[i].eventTime = (i + 1) * 100;
        g_events[i].clientId = (i + 1) * 10;
        g_events[i].eventIndex = i;
        len = strlen(str[i]);
//        g_events[i].logRecordSize = len;
        if((bptr = (char*)malloc(++len * sizeof(char))))
        {

            strcpy(bptr, str[i]);

            g_events[i].logRecord.p = (uint8_t*)bptr;
            g_events[i].logRecord.len = len;
        }
    }

    wdata = (hvl_t*)malloc(sizeof(hvl_t) * nCount);
    for(i = 0; i < nCount; i++)
    {
        wdata[i].len = g_events[i].logRecord.len;
        wdata[i].p = g_events[i].logRecord.p;
    }

    // Generate StoryChunks
    std::cout << "Generating StoryChunks..." << std::endl;
    uint64_t time_step = (end_time - start_time) / num_events_per_story_chunk;
    StoryChunk*story_chunk_array = generateStoryChunkArray(story_id, client_id, start_time, time_step, end_time
                                                           , mean_event_size, min_event_size, max_event_size, stddev
                                                           , num_events_per_story_chunk, num_story_chunks);
    std::map <uint64_t, StoryChunk> story_chunk_map = generateStoryChunkMap(story_id, client_id, start_time, time_step
                                                                            , end_time, mean_event_size, min_event_size
                                                                            , max_event_size, stddev
                                                                            , num_events_per_story_chunk
                                                                            , num_story_chunks);

    hsize_t total_num_events = 0;
    std::vector <hsize_t> num_events;
    for(uint64_t j = 0; j < num_story_chunks; j++)
    {
//        hsize_t num_events_per_chunk = story_chunk_array[j].getNumEvents();
        hsize_t num_events_per_chunk = story_chunk_map[j].getNumEvents();
        num_events.push_back(num_events_per_chunk);
        total_num_events += num_events_per_chunk;
    }

    // Check if the directory for the Chronicle already exists
    struct stat st{};
    std::string chronicle_dir = CHRONICLE_ROOT_DIR + chronicle_name;
    if(stat(chronicle_dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
    {
        // Create the directory for the Chronicle
        if(!std::filesystem::create_directory(chronicle_dir.c_str()))
        {
            LOG_ERROR("Failed to create chronicle directory: {}, errno: {}", chronicle_dir.c_str(), errno);
            return chronolog::CL_ERR_UNKNOWN;
        }
    }

    // Write StoryChunks
    ret = writeStoryChunks(story_chunk_map, story_chunk_array, chronicle_dir, wdata);
    assert(ret == chronolog::CL_SUCCESS);

    // Read StoryChunks
    ret = readStoryChunks(story_chunk_map, story_chunk_array, chronicle_dir, rdata);
    assert(ret == chronolog::CL_SUCCESS);

    // Cleanup
    for(i = 0; i < nCount; i++)
    { free(g_events[i].logRecord.p); }
    for(uint64_t j = 0; j < num_story_chunks; j++)
    {
        for(uint64_t k = 0; k < num_events[j]; k++)
        { free(story_chunk_array[j].logEvents[k].logRecord.p); }
        delete[] story_chunk_array[j].logEvents;
    }
    delete[] story_chunk_array;
    free(rdata);
}
