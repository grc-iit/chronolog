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
#include <ChunkLoggingExtractor.h>
#include <ChunkExtractorCSV.h>
#include <ChunkExtractorRDMA.h>
#include <KeeperExtractionChain.h>

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
    LOG_INFO("[ExtractionModuleTest] starting contributing thread {} ", thread_id);

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


    LOG_INFO("[ExtractionModuleTest] exiting contributing thread {} ", thread_id);
}

////

std::string extraction_module_json_1 =
        std::string("{ \"ExtractionModule\": ") + "{ \"extraction_stream_count\":2," + "\"extractors\": { " +
        "\"test_single_rdma_extractor\": {" + "\"type\": \"single_endpoint_rdma_extractor\"," +
        "\"receiving_endpoint\": {" + "\"protocol_conf\": \"ofi+sockets\"," + "\"service_ip\": \"127.0.0.1\"," +
        "\"service_base_port\": 2230," + "\"service_provider_id\": 30" + " }" + "}" + "}" + "}" + "}";

std::string extraction_module_json_2 =
        std::string("{ \"ExtractionModule\": ") + "{ \"extraction_stream_count\":2," + "\"extractors\": { " +
        "\"test_dual_rdma_extractor\": {" + "\"type\": \"dual_endpoint_rdma_extractor\"," +
        "\"player_receiving_endpoint\": {" + "\"protocol_conf\": \"ofi+sockets\"," + "\"service_ip\": \"127.0.0.1\"," +
        "\"service_base_port\": 2232," + "\"service_provider_id\": 32" + " }," + "\"grapher_receiving_endpoint\": {" +
        "\"protocol_conf\": \"ofi+sockets\"," + "\"service_ip\": \"127.0.0.1\"," + "\"service_base_port\": 2233," +
        "\"service_provider_id\": 33" + "}" + "}" + "}" + "}" + "}";

///


int main()
{
    int contributor_threads = 5;
    int extraction_threads = 2;

    signal(SIGTERM, sigterm_handler);

    int result = chronolog::chrono_monitor::initialize(
            "file" //confManager.CLIENT_CONF.CLIENT_LOG_CONF.LOGTYPE
            ,
            "/tmp/keeper_extraction_test.log" //, confManager.CLIENT_CONF.CLIENT_LOG_CONF.LOGFILE
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

    std::string LOCAL_SERVICE_NA_STRING;
    localServiceId.get_service_as_string(LOCAL_SERVICE_NA_STRING);

    tl::engine* localEngine = nullptr;

    chronolog::LoggingExtractor logging_extractor;
    chronolog::StoryChunkExtractorCSV csv_extractor(localServiceId, "/tmp/");

    try
    {
        margo_instance_id margo_id = margo_init(LOCAL_SERVICE_NA_STRING.c_str(), MARGO_SERVER_MODE, 1, 1);
        localEngine = new tl::engine(margo_id);
    }
    catch(tl::exception const&)
    {
        return (-1);
    }

    // 1. Test single endpoint RDMA extractor instantiation
    chl::ServiceId receiving_service_id("ofi+sockets", "127.0.0.1", 3333, 33);
    chl::StoryChunkExtractorRDMA rdma_extractor(*localEngine, receiving_service_id);

    // 2. Test chained ExtractionModule instantiation with chained logging extractor & csv extractor
    chronolog::StoryChunkExtractionModule<chronolog::ChronoKeeperExtractionChain> extractionModule;

    extractionModule.getExtractionChain().add_extractor(logging_extractor);
    extractionModule.getExtractionChain().add_extractor(csv_extractor);
    extractionModule.getExtractionChain().add_extractor(rdma_extractor);

    extractionModule.initialize(extraction_threads);

    // 3.  Test ExtractionModule instantiation using json configuration object

    chronolog::StoryChunkExtractionModule<chronolog::ChronoKeeperExtractionChain> new_extractionModule;

    json_object* parsed_json = json_tokener_parse(extraction_module_json_1.c_str());

    if(!json_object_is_type(json_object_object_get(parsed_json, "ExtractionModule"), json_type_object))
    {
        std::cerr << "\n [ExtractionModuleConfiguration] Test parsing stage:"
                  << " failed : json_block  is not a json object" << std::endl;
        return -1;
    }

    json_object* extraction_module_block = json_object_object_get(parsed_json, "ExtractionModule");
    chronolog::ExtractionModuleConfiguration extraction_config;
    int return_value = extraction_config.parse_json_object(extraction_module_block);
    if(return_value != chl::CL_SUCCESS)
    {
        std::cerr << "\n [ExtractionModuleConfiguration] Test parsing stage:" << " failed  " << std::endl;
        return -1;
    }

    std::string log_string;
    extraction_config.to_string(log_string);

    std::cout << "\n [ExtractionModuleConfiguration] Test parsing stage:" << " passed " << log_string << std::endl;

    json_object_put(parsed_json);

    extractionModule.getExtractionChain().activate(*localEngine, extraction_config, localServiceId);

    new_extractionModule.initialize(extraction_config.extraction_stream_count);

    if(!new_extractionModule.is_initialized())
    {
        std::cerr << "\n [ExtractionModuleConfiguration] ExtractionModule failed to initialize " << std::endl;
        return -1;
    }

    chl::StoryChunkExtractionQueue& extractionQueue = new_extractionModule.getExtractionQueue();

    new_extractionModule.startExtraction();

    // 4. create chunk contributing threads
    std::thread contributors[contributor_threads];

    static uint32_t thread_id = 0;
    while(true == keep_running)
    {
        LOG_INFO("[ExtractionModuleTest] ExtractionChainTest is running");

        // now the main thread will start a few contributor threads, that would stash a few StoryChunks on the extractionQueue,
        // than fall asleep until it's time to wake up and start some more contributor threads...

        // the StoryChunkExtraction module is expected to run in the background and take care of the extraction duties

        for(short int i = 0; i < contributor_threads; ++i, ++thread_id)
        {
            std::thread t{chunk_contributor_thread, &extractionQueue, thread_id};
            contributors[i] = std::move(t);
        }
        for(int i = 0; i < contributor_threads; ++i) { contributors[i].join(); }

        std::this_thread::sleep_for(std::chrono::seconds(20));
    }


    // 5. Test ExtractionModule shutdown
    LOG_INFO("[ExtractionModuleTest] Shutting down StoryChunkExtractionModule for {}", chl::to_string(localServiceId));

    extractionModule.shutdownExtraction();

    delete localEngine;
    return 1;
}
