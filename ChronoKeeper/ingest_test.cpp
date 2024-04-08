#include <thallium.hpp>
#include "StoryChunk.h"
//#include "StoryChunkReceiver.h"
#include "cmd_arg_parse.h"
#include "../external_libs/cereal/include/cereal/archives/binary.hpp"
#include "ConfigurationManager.h"

namespace tl = thallium;

#define MAX_BULK_MEM_SIZE (1024 * 1024)
#define NUM_STREAMS 1

size_t deserializedWithCereal(char *buffer, size_t size, chronolog::StoryChunk &story_chunk)
{
    std::stringstream ss;
    ss.write(buffer, size);
    cereal::BinaryInputArchive iarchive(ss);
    iarchive(story_chunk);
    return sizeof(buffer);
}

int main(int argc, char**argv)
{
    std::string conf_file_path;
    conf_file_path = parse_conf_path_arg(argc, argv);
    ChronoLog::ConfigurationManager confManager(conf_file_path);
    int result = Logger::initialize("console", confManager.GRAPHER_CONF.LOG_CONF.LOGFILE
                                    , confManager.GRAPHER_CONF.LOG_CONF.LOGLEVEL
                                    , confManager.GRAPHER_CONF.LOG_CONF.LOGNAME
                                    , confManager.GRAPHER_CONF.LOG_CONF.LOGFILESIZE
                                    , confManager.GRAPHER_CONF.LOG_CONF.LOGFILENUM
                                    , confManager.GRAPHER_CONF.LOG_CONF.FLUSHLEVEL);
    if(result == 1)
    {
        exit(EXIT_FAILURE);
    }
    LOG_INFO("[standalone_ingest_test] Running StoryChunk Ingestion Server.");

    /**
     * Keeper-push
     */
    std::string KEEPER_COLLECTOR_NA_STRING =
//            confManager.COLLECTOR_CONF.KEEPER_GRAPHER_DRAIN_SERVICE_CONF.RPC_CONF.PROTO_CONF +
            "ofi+sockets://" +
            confManager.GRAPHER_CONF.KEEPER_GRAPHER_DRAIN_SERVICE_CONF.IP + ":" +
            std::to_string(confManager.GRAPHER_CONF.KEEPER_GRAPHER_DRAIN_SERVICE_CONF.BASE_PORT);
//    std::string KEEPER_COLLECTOR_NA_STRING = "ofi+tcp;ofi_rxm://172.17.0.1:37709";
    tl::engine extraction_engine = tl::engine(KEEPER_COLLECTOR_NA_STRING, THALLIUM_SERVER_MODE);
    std::stringstream ss;
    ss << extraction_engine.self();
    LOG_DEBUG("[standalone_ingest_test] Engine: {}", ss.str());

//    chronolog::StoryChunkReceiver storyChunkReceiver(extraction_engine);;
//
//    storyChunkReceiver.startReceiverThreads(1);
//
//    storyChunkReceiver.shutdownReceiverThreads();

    std::vector <tl::managed <tl::xstream>> tl_es_vec;
    tl::managed <tl::pool> tl_pool = tl::pool::create(tl::pool::access::spmc);
    LOG_DEBUG("[standalone_ingest_test] Pool created");
    for(int j = 0; j < NUM_STREAMS; j++)
    {
        tl::managed <tl::xstream> es = tl::xstream::create(tl::scheduler::predef::deflt, *tl_pool);
        LOG_DEBUG("[standalone_ingest_test] A new stream is created");
        tl_es_vec.push_back(std::move(es));
    }

    std::function <void(const tl::request &, tl::bulk &)> drain_story_chunk_to_collector = [&extraction_engine](
            const tl::request &req, tl::bulk &b)
    {
        LOG_DEBUG("[standalone_ingest_test] Bulk transfer rpc invoked");
        tl::endpoint ep = req.get_endpoint();
        LOG_DEBUG("[standalone_ingest_test] Endpoint obtained");
        std::vector <char> mem_vec(MAX_BULK_MEM_SIZE);
        mem_vec.reserve(MAX_BULK_MEM_SIZE);
        mem_vec.resize(MAX_BULK_MEM_SIZE);
        std::vector <std::pair <void*, std::size_t>> segments(1);
        segments[0].first = (void*)(&mem_vec[0]);
        segments[0].second = mem_vec.size();
        LOG_DEBUG("[standalone_ingest_test] Bulk memory prepared, size: {}", mem_vec.size());
        LOG_DEBUG("[standalone_ingest_test] Engine addr: {}", (void*)&extraction_engine);
        tl::bulk local = extraction_engine.expose(segments, tl::bulk_mode::write_only);
        LOG_DEBUG("[standalone_ingest_test] Bulk memory exposed");
        b.on(ep) >> local;
        LOG_DEBUG("[standalone_ingest_test] Received {} bytes of data in bulk transfer mode", b.size());
//        for(auto i = 0; i < b.size() - 1; ++i)
//        {
//            std::cout << (char)*(char*)(&mem_vec[0]+i) << " ";
//        }
//        std::cout << std::endl;
        chronolog::StoryChunk story_chunk;
        deserializedWithCereal(&mem_vec[0], b.size() - 1, story_chunk);
        LOG_DEBUG("[standalone_ingest_test] StoryChunk received: StoryID: {}, StartTime: {}"
                  , story_chunk.getStoryId(), story_chunk.getStartTime());
        req.respond(b.size());
        LOG_DEBUG("[standalone_ingest_test] Bulk transfer rpc responded");
    };

    extraction_engine.define("bulk_put", drain_story_chunk_to_collector, 0, *tl_pool);

    extraction_engine.wait_for_finalize();
    for(int j = 0; j < NUM_STREAMS; j++)
    {
        tl_es_vec[j]->join();
    }

    return 0;
}