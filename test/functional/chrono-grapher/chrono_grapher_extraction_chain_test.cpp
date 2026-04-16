#include <chrono>
#include <iostream>
#include <signal.h>
#include <thread>
#include <variant>
#include <vector>

#include <chrono_monitor.h>
#include <ServiceId.h>
#include <StoryChunk.h>
#include <StoryChunkExtractionModule.h>
#include <ChunkExtractorCSV.h>
#include <HDF5FileChunkExtractor.h>
#include <GrapherExtractionChain.h>

namespace tl = thallium;
namespace chl = chronolog;

bool keep_running = true;
void sigterm_handler(int)
{
    std::cout << "Received SIGTERM signal. Initiating shutdown procedure." << std::endl;
    keep_running = false;
    return;
}

static constexpr uint64_t NS = 1000000000ULL;

void chunk_contributor_thread(chl::StoryChunkExtractionQueue* extractionQueue, uint32_t thread_id)
{
    LOG_INFO("[GrapherExtractionChainTest] starting contributing thread {} ", thread_id);

    for(unsigned int k = 0; k < 10; ++k)
    {
        auto time_now = std::chrono::high_resolution_clock::now();
        uint64_t chunk_starttime = time_now.time_since_epoch().count();
        uint64_t chunk_endtime = (time_now + std::chrono::seconds(5)).time_since_epoch().count();

        chl::StoryChunk* story_chunk =
                new chl::StoryChunk("Chronicle", "Story" + std::to_string(k), k, chunk_starttime, chunk_endtime);

        for(unsigned int i = 0; i < 10; ++i)
        {
            story_chunk->insertEvent(
                    chl::LogEvent{k,
                                  chunk_starttime + i,
                                  thread_id,
                                  1,
                                  "thread " + std::to_string(thread_id) + " line " + std::to_string(i)});
        }

        extractionQueue->stashStoryChunk(story_chunk);
    }


    LOG_INFO("[GrapherExtractionChainTest] exiting contributing thread {} ", thread_id);
}


std::string extraction_module_json_string =
        std::string("{ \"ExtractionModule\": ") + "{ \"extraction_stream_count\":2," + "\"extractors\": { " +
        "\"test_csv_extractor\": { \"type\": \"csv_extractor\", \"csv_archive_dir\": \"/tmp/csv_archive\" }" + "," +
        "\"test_hdf5_extractor\": { \"type\": \"hdf5_extractor\", \"hdf5_archive_dir\": \"/tmp/hdf5_archive\" }" + "} 
        + "}" + "}";

int main()
{
    int contributor_threads = 5;
    int extraction_threads = 2;

    signal(SIGTERM, sigterm_handler);

    int result = chronolog::chrono_monitor::initialize(
            "file" //confManager.CLIENT_CONF.CLIENT_LOG_CONF.LOGTYPE
            ,
            "/tmp/extraction_test.log" //, confManager.CLIENT_CONF.CLIENT_LOG_CONF.LOGFILE
            ,
            chronolog::LogLevel::debug // confManager.CLIENT_CONF.CLIENT_LOG_CONF.LOGLEVEL
            ,
            "ExtractionModuleTest" //confManager.CLIENT_CONF.CLIENT_LOG_CONF.LOGNAME
            ,
            100000000 //confManager.CLIENT_CONF.CLIENT_LOG_CONF.LOGFILESIZE
            ,
            2 //confManager.CLIENT_CONF.CLIENT_LOG_CONF.LOGFILENUM
            ,
            chronolog::LogLevel::debug //confManager.CLIENT_CONF.CLIENT_LOG_CONF.FLUSHLEVEL);
    );


    chl::ServiceId localServiceId("ofi+sockets", "127.0.0.1", 2225, 25);

    chronolog::LoggingExtractor logging_extractor;
    chronolog::StoryChunkExtractorCSV csv_extractor(localServiceId, "/tmp/");
    chronolog::HDF5FileChunkExtractor hdf5_extractor("/tmp/");

    // 2. Test chained ExtractionModule instantiation with ChronoGrapherExtractionChain
    chronolog::StoryChunkExtractionModule<chronolog::ChronoGrapherExtractionChain> extractionModule;

    extractionModule.getExtractionChain().add_extractor(csv_extractor);
    extractionModule.getExtractionChain().add_extractor(hdf5_extractor);

    extractionModule.initialize(extraction_threads);

    if(!extractionModule.is_initialized())
    {

        LOG_ERROR("[GrapherExtractionChainTest] ExtractionModule failed to initialize ");
        return (-1);
    }
    // 3. Start extraction threads
    chl::StoryChunkExtractionQueue& extractionQueue = extractionModule.getExtractionQueue();

    extractionModule.startExtraction();

    // 4. create chunk contributing threads
    std::thread contributors[contributor_threads];

    static uint32_t thread_id = 0;
    while(true == keep_running)
    {
        LOG_INFO("[GrapherExtractionChainTest] ExtractionChainTest is running");

        // now the main thread will start a few contributor threads, that would stash a few StoryChunks on the extractionQueue,
        // than fall asleep until it's time to wake up and start some more contributor threads...

        // the StoryChunkExtraction module is expected to run in the background and take care of the extruction duties

        for(short int i = 0; i < contributor_threads; ++i, ++thread_id)
        {
            std::thread t{chunk_contributor_thread, &extractionQueue, thread_id};
            contributors[i] = std::move(t);
        }
        for(int i = 0; i < contributor_threads; ++i) { contributors[i].join(); }

        std::this_thread::sleep_for(std::chrono::seconds(20));
    }


    // 5. Test ExtractionModule shutdown
    LOG_INFO("[GrapherExtractionChainTest] Shutting down StoryChunkExtractionModule for {}",
             chl::to_string(localServiceId));

    extractionModule.shutdownExtraction();

    return 1;
}
