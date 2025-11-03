#include <chrono>
#include <iostream>
#include <signal.h>

#include <chrono_monitor.h>
#include <ServiceId.h>
#include <StoryChunk.h>
#include <StoryChunkExtractionModule.h>
#include <ChunkExtractorCSV.h>

namespace tl = thallium;
namespace chl = chronolog;

bool keep_running = true;
void sigterm_handler(int)
{
    std::cout<< "Received SIGTERM signal. Initiating shutdown procedure."<<std::endl;
    keep_running = false;
    return;
}

static constexpr uint64_t NS = 1000000000ULL;

void chunk_contributor_thread( chl::StoryChunkExtractionQueue * extractionQueue, uint32_t thread_id)
{
    //auto thread_id_ = std::this_thread::get_id();
    LOG_INFO( "[ExtractionModuleTest] starting contributing {} ",thread_id); //, std::to_string(thread_id_));

    for(int k=0; k< 10; ++k) 
    {
        uint64_t time_now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        chl::StoryChunk * story_chunk = new chl::StoryChunk("Chronicle","Story", 1, time_now, time_now+500);
   
        for(int i =0; i< 10; ++i) 
        { 
            story_chunk->insertEvent(chl::LogEvent{ 1, time_now + i, thread_id, 1,
                             "line "+std::to_string(i)});
        }

        extractionQueue->stashStoryChunk(story_chunk);
    }

 
    LOG_INFO( "[ExtractionModuleTest] exiting contributing {} ",thread_id);
    std::this_thread::sleep_for(std::chrono::seconds(10));
}

int main()
{
    int contributor_threads = 5;
    int extraction_threads =2;

    signal(SIGTERM, sigterm_handler);

    int result = chronolog::chrono_monitor::initialize( "file" //confManager.CLIENT_CONF.CLIENT_LOG_CONF.LOGTYPE
                                                    ,"/tmp/extraction_test.log"  //, confManager.CLIENT_CONF.CLIENT_LOG_CONF.LOGFILE
                                                    , spdlog::level::debug// confManager.CLIENT_CONF.CLIENT_LOG_CONF.LOGLEVEL
                                                    , "ExtractionModuleTest"//confManager.CLIENT_CONF.CLIENT_LOG_CONF.LOGNAME
                                                    , 100000000//confManager.CLIENT_CONF.CLIENT_LOG_CONF.LOGFILESIZE
                                                    , 2//confManager.CLIENT_CONF.CLIENT_LOG_CONF.LOGFILENUM
                                                    , spdlog::level::debug//confManager.CLIENT_CONF.CLIENT_LOG_CONF.FLUSHLEVEL);
                                                    );


    chl::ServiceId localServiceId("ofi+sockets", "127.0.0.1",2225,25);

    std::string LOCAL_SERVICE_NA_STRING;
    localServiceId.get_service_as_string(LOCAL_SERVICE_NA_STRING);

    tl::engine * localEngine = nullptr;

    chronolog::LoggingExtractor logging_extractor;
    chronolog::StoryChunkExtractorCSV csv_extractor(localServiceId,"/tmp/");

    //chronolog::StoryChunkExtractionModule<chl::LoggingExtractor > * chunkExtractionModule = nullptr;
    chronolog::StoryChunkExtractionModule< chronolog::StoryChunkExtractorCSV > * chunkExtractionModule = nullptr;
    
    try
    {
        margo_instance_id margo_id = margo_init(LOCAL_SERVICE_NA_STRING.c_str(), MARGO_SERVER_MODE, 1, 1);
        localEngine = new tl::engine(margo_id);


       // chunkExtractionModule = new chronolog::StoryChunkExtractionModule<chl::LoggingExtractor> (logging_extractor);
        chunkExtractionModule = new chronolog::StoryChunkExtractionModule<chl::StoryChunkExtractorCSV> (csv_extractor);

    }
    catch(tl::exception const &)
    {
        chunkExtractionModule = nullptr;
    }

    if(nullptr == chunkExtractionModule)
    {
        return(-1);
    }

    //Start extraction threads

    chl::StoryChunkExtractionQueue & extractionQueue = chunkExtractionModule->getExtractionQueue();

    chunkExtractionModule->startExtraction(extraction_threads);

    //Start chunk contributing threads
    std::thread contributors[contributor_threads];

 static uint32_t thread_id = 0; 
 while( true ==  keep_running)
    {
        LOG_INFO( "[ExtractionModuleTest] ExtractionChainTest is running");

        // now we will be starting a few contributor threads, that would stash a few StoryChunks on the extractionQueue, 
        // peiodically than fall asleep until it's the next time to wake up and start some more contributor threads

        // the StoryChunkExtraction module is expected to run in the background and take care of the extruction duties

        for ( short int i =0; i < contributor_threads; ++i, ++thread_id) 
        {
            std::thread t{ chunk_contributor_thread, &extractionQueue, thread_id};
            contributors[i] = std::move(t);
        }
        for(int i = 0; i < contributor_threads; ++i)
        {    contributors[i].join(); }

        std::this_thread::sleep_for(std::chrono::seconds(5));
    }

    
    LOG_INFO( "[ExtractionModuleTest] Shutting down StoryChunkExtractionModule for {}", chl::to_string(localServiceId));

    delete chunkExtractionModule;
    delete localEngine;
return 1;
}
