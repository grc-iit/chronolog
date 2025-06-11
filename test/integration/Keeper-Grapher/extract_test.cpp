#include <thallium.hpp>
#include <random>
#include <deque>
#include <cereal/archives/binary.hpp>
#include "StoryChunk.h"
#include "cmd_arg_parse.h"
#include "ConfigurationManager.h"

namespace tl = thallium;

#define NUM_THREADS 1
#define NUM_STORY_CHUNKS 100
#define NUM_EVENTS 100
#define MAX_BULK_MEM_SIZE (1024 * 1024 * 4)

std::string rpc_name_g = "record_story_chunk";
tl::engine*tl_engine_g;
tl::remote_procedure bulk_put_g;
tl::provider_handle service_ph_g;
std::deque <chronolog::StoryChunk*> story_chunk_queue_g;
std::mutex story_chunk_queue_mutex_g;

chronolog::StoryChunk*generateRandomStoryChunk()
{
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution <> dis(1, 100);
    std::string chronicle_name = "CHRONICLE_" + std::to_string(dis(gen));
    std::string story_name = "STORY_" + std::to_string(dis(gen));
    uint64_t client_id = dis(gen);
    uint64_t story_id = dis(gen);
    uint64_t start_time = dis(gen) * 4;
    uint64_t end_time = start_time + NUM_EVENTS * 128;
//    std::string log_event_str_base = "FFFFFFFFFFFFFFFFFFFFFF" + std::to_string(story_id); // for #events=10
    std::string log_event_str_base = "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFF" + std::to_string(story_id); // for #events=100
//    std::string log_event_str_base = "FFFFFFFFFF = " + std::to_string(story_id); // for #events=1000
    auto*story_chunk = new chronolog::StoryChunk(chronicle_name, story_name, story_id, start_time, end_time);
    for(int i = 0; i < NUM_EVENTS; ++i)
    {
        chronolog::EventSequence event_sequence = chronolog::EventSequence(start_time, client_id, i);
        chronolog::LogEvent event;
        event.storyId = story_id;
        event.clientId = client_id;
        event.eventTime = start_time + i * 2;
        event.eventIndex = i;
        // for #events=10
//        event.logRecord =
//                log_event_str_base + ", AABBCCDDEEFFGGHHIIJJKKLLMMNNOOPPQQRRSSTTUUVVWWXXYYZZ" + std::to_string(i);
//        event.logRecord += event.logRecord;
//        event.logRecord += event.logRecord;
//        event.logRecord += event.logRecord;
//        event.logRecord += event.logRecord;
//        event.logRecord += event.logRecord;
//        event.logRecord += event.logRecord;
//        event.logRecord += event.logRecord;
//        event.logRecord += log_event_str_base + ", AABBCCDDEEFFGGHHIIJJKKLLMMNNOOPPQQRRSSTTUUVVWW" + std::to_string(i);
//        event.logRecord += log_event_str_base + ", AABBCCDDEEFFGGHHIIJJKKLLM" + std::to_string(i);
        // for #events=100
        event.logRecord =
                log_event_str_base + ", AABBCCDDEEFFGGHHIIJJKKLLMMNNOOPPQQRRSSTTUUVVWWXXYYZZ1" + std::to_string(i);
        event.logRecord += event.logRecord;
        event.logRecord += event.logRecord;
        event.logRecord += event.logRecord;
        event.logRecord +=
                log_event_str_base + ", AABBCCDDEEFFGGHHIIJJKKLLMMNNOOPPQQRRSSTTUUVVWWXXYYZZ1" + std::to_string(i);
        event.logRecord +=
                log_event_str_base + ", AABBCCDDEEFFGGHHIIJJKKLLMMNNOOPPQQRRSSTTUUVVWWXXYYZZ1" + std::to_string(i);
        event.logRecord +=
                log_event_str_base + ", AABBCCDDEEFFGGHHIIJJKKLLMMNNOOPPQQRRSSTTUUVVWWXXYYZZ" + std::to_string(i);
        // for #events=1000
//        event.logRecord = log_event_str_base + ", AABBCCDDEEFFGGHHIIJJKKLLMMNN = " + std::to_string(i);
        story_chunk->insertEvent(event);
    }

    return story_chunk;
}

size_t serializeWithCereal(chronolog::StoryChunk*story_chunk, std::stringstream &ss)
{
    cereal::BinaryOutputArchive oarchive(ss);
    oarchive(*story_chunk);
    return ss.str().size();
}

void standaloneExtraction()
{
    size_t result;
    uint64_t tid = tl::thread::self_id();
    std::chrono::high_resolution_clock::time_point start, end;

    LOG_INFO("[standalone_extract_test] T{}: Running standalone StoryChunk Extraction test.", tid);
    while(!story_chunk_queue_g.empty())
    {
        // get StoryChunk
        std::lock_guard <std::mutex> lock(story_chunk_queue_mutex_g);
        if(story_chunk_queue_g.empty())
        {
            LOG_INFO("[standalone_extract_test] T{}: StoryChunk queue is empty. Exiting ...", tid);
            return;
        }
        chronolog::StoryChunk*story_chunk = story_chunk_queue_g.front();
        story_chunk_queue_g.pop_front();
        LOG_DEBUG("[standalone_extract_test] T{}: Extracting a story chunk with StoryID: {}, StartTime: {} ...", tid
                  , story_chunk->getStoryId(), story_chunk->getStartTime());

        // serialize StoryChunk
        start = std::chrono::high_resolution_clock::now();
        size_t serialized_story_chunk_size;
        char *serialized_buf = new char[MAX_BULK_MEM_SIZE];
        std::ostringstream oss(std::ios::binary);
        oss.rdbuf()->pubsetbuf(serialized_buf, MAX_BULK_MEM_SIZE);
        cereal::BinaryOutputArchive oarchive(oss);
        oarchive(*story_chunk);
        serialized_story_chunk_size = oss.tellp();
        end = std::chrono::high_resolution_clock::now();
//        for(auto i = 0; i < serialized_story_chunk_size; ++i)
//        {
//            std::cout << serialized_buf[i] << " ";
//        }
//        std::cout << std::endl;
        size_t story_chunk_size = sizeof(uint64_t) * 5; // for five uint64_t members in StoryChunk
        story_chunk_size += story_chunk->getChronicleName().size();
        story_chunk_size += story_chunk->getStoryName().size();
        for(const auto &iter: *story_chunk)
        {
            story_chunk_size += sizeof(iter.first) + sizeof(iter.second.storyId) + sizeof(iter.second.eventTime) +
                                sizeof(iter.second.clientId) + sizeof(iter.second.eventIndex) +
                                iter.second.logRecord.size();
        }
        LOG_INFO("[standalone_extract_test] T{}: StoryChunk size before serialization: {}", tid, story_chunk_size);
        LOG_INFO("[standalone_extract_test] T{}: StoryChunk size after serialization: {}", tid
                 , serialized_story_chunk_size);
        LOG_INFO("[standalone_extract_test] T{}: Serialization took {} us", tid,
                std::chrono::duration_cast <std::chrono::nanoseconds>(end - start).count() / 1000.0);

        // prepare memory segments
        std::vector <std::pair <void*, std::size_t>> segments(1);
        segments[0].first = (void*)(serialized_buf);
        segments[0].second = serialized_story_chunk_size + 1;
        tl::bulk tl_bulk = tl_engine_g->expose(segments, tl::bulk_mode::read_only);

        // call RPC
        LOG_DEBUG("[standalone_extract_test] T{}: Calling RPC on Grapher with serialized story chunk size: {} ...", tid
                  , tl_bulk.size());
        start = std::chrono::high_resolution_clock::now();
        result = bulk_put_g.on(service_ph_g)(tl_bulk);
        end = std::chrono::high_resolution_clock::now();
        LOG_DEBUG("[standalone_extract_test] T{}: RPC call on Grapher returned with result: {}", tid, chronolog::to_string_client(result));
        LOG_INFO("[standalone_extract_test] T{}: RPC call on Grapher took {} us", tid,
                std::chrono::duration_cast <std::chrono::nanoseconds>(end - start).count() / 1000.0);

        // result check
        if(result == serialized_story_chunk_size + 1)
        {
            LOG_INFO("[standalone_extract_test] T{}: Successfully drained a story chunk to Grapher", tid);
        }
        else
        {
            LOG_ERROR("[standalone_extract_test] T{}: Failed to drain a story chunk to Grapher, Error Code: {}", tid
                      , chronolog::to_string_client(result));
        }

        // release memory
        delete[] serialized_buf;
    }
    LOG_DEBUG("[standalone_extract_test] T{}: Exiting ...", tid);
}

int main(int argc, char**argv)
{
    std::string conf_file_path;
    conf_file_path = parse_conf_path_arg(argc, argv);
    ChronoLog::ConfigurationManager confManager(conf_file_path);
    int result = chronolog::chrono_monitor::initialize("console", confManager.KEEPER_CONF.KEEPER_LOG_CONF.LOGFILE
                                                       , confManager.KEEPER_CONF.KEEPER_LOG_CONF.LOGLEVEL
                                                       , confManager.KEEPER_CONF.KEEPER_LOG_CONF.LOGNAME
                                                       , confManager.KEEPER_CONF.KEEPER_LOG_CONF.LOGFILESIZE
                                                       , confManager.KEEPER_CONF.KEEPER_LOG_CONF.LOGFILENUM
                                                       , confManager.KEEPER_CONF.KEEPER_LOG_CONF.FLUSHLEVEL);
    if(result == 1)
    {
        exit(EXIT_FAILURE);
    }

    for(int i = 0; i < NUM_STORY_CHUNKS; ++i)
    {
        chronolog::StoryChunk*story_chunk = generateRandomStoryChunk();
        story_chunk_queue_g.push_back(story_chunk);
    }

    /**
     * Standalone StoryChunk Extraction in a separate thread
     */
    // setup engine in client mode
    std::string KEEPER_GRAPHER_PROTOCOL = confManager.KEEPER_CONF.KEEPER_GRAPHER_DRAIN_SERVICE_CONF.RPC_CONF.PROTO_CONF;
    tl_engine_g = new tl::engine(KEEPER_GRAPHER_PROTOCOL + "://", THALLIUM_CLIENT_MODE);
    uint64_t tid = tl::thread::self_id();
    std::stringstream ss;
    ss << tl_engine_g->self();
    LOG_DEBUG("[extract_test_main] T{}: Engine: {} ...", tid, ss.str());

    // define RPC
    bulk_put_g = tl_engine_g->define(rpc_name_g);
    LOG_DEBUG("[extract_test_main] T{}: RPC defined with name: {}", tid, rpc_name_g);

    // get provider handle
    std::string KEEPER_GRAPHER_NA_STRING =
            KEEPER_GRAPHER_PROTOCOL + "://" + confManager.KEEPER_CONF.KEEPER_GRAPHER_DRAIN_SERVICE_CONF.RPC_CONF.IP +
            ":" + std::to_string(confManager.KEEPER_CONF.KEEPER_GRAPHER_DRAIN_SERVICE_CONF.RPC_CONF.BASE_PORT);
    uint16_t extraction_provider_id = confManager.KEEPER_CONF.KEEPER_GRAPHER_DRAIN_SERVICE_CONF.RPC_CONF.SERVICE_PROVIDER_ID;
    LOG_DEBUG("[extract_test_main] T{}: Looking up {} at: {} with provider id {} ...", tid, rpc_name_g
              , KEEPER_GRAPHER_NA_STRING, extraction_provider_id);
    service_ph_g = tl::provider_handle(tl_engine_g->lookup(KEEPER_GRAPHER_NA_STRING), extraction_provider_id);
    if(service_ph_g.is_null())
    {
        LOG_ERROR("[extract_test_main] T{}: Failed to lookup Grapher service provider handle", tid);
        throw std::runtime_error("Failed to lookup Grapher service provider handle");
    }
    LOG_DEBUG("[extract_test_main] T{}: Successfully obtained service provider handle", tid);

    // create a thread to call RPC
    tl::abt scope;
    std::vector <tl::managed <tl::xstream>> extraction_streams;
    tl::managed <tl::pool> tl_pool = tl::pool::create(tl::pool::access::spmc, tl::pool::kind::fifo_wait);
    for(int i = 0; i < NUM_THREADS; i++)
    {
        tl::managed <tl::xstream> es = tl::xstream::create(tl::scheduler::predef::basic_wait, *tl_pool);
        extraction_streams.push_back(std::move(es));
    }
    std::vector <tl::managed <tl::thread>> extraction_threads;
    for(int i = 0; i < NUM_THREADS; ++i)
    {
        tl::managed <tl::thread> th = extraction_streams[i % extraction_streams.size()]->make_thread([]()
                                                                                                     { standaloneExtraction(); });
        extraction_threads.push_back(std::move(th));
        LOG_DEBUG("[extract_test_main] T{}: Extraction thread {} created.", tid, i);
    }

    LOG_DEBUG("[extract_test_main] T{}: Waiting for engine to finalize ...", tid);
    tl_engine_g->wait_for_finalize();
    LOG_DEBUG("[extract_test_main] T{}: Engine has been successfully finalized.", tid);

    LOG_INFO("[extract_test_main] T{}: Waiting for extraction threads to shut down ...", tid);
    for(auto &eth: extraction_threads)
    {
        eth->join();
    }
    LOG_DEBUG("[extract_test_main] T{}: Extraction threads successfully shut down.");

    LOG_DEBUG("[extract_test_main] T{}: Waiting for streams to close ...", tid);
    for(auto &es: extraction_streams)
    {
        es->join();
    }
    LOG_DEBUG("[extract_test_main] T{}: Streams have been successfully closed.");

    for(auto &story_chunk: story_chunk_queue_g)
    {
        delete story_chunk;
    }
    delete tl_engine_g;
    return 0;
}
