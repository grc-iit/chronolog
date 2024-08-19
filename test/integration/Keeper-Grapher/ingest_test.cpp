#include <thallium.hpp>
#include "ConfigurationManager.h"
#include "StoryChunk.h"
#include "cmd_arg_parse.h"
#include <cereal/archives/binary.hpp>

namespace tl = thallium;

#define MAX_BULK_MEM_SIZE (1024 * 1024 * 4)
#define NUM_STREAMS 4

class StoryChunkRecordService: public tl::provider <StoryChunkRecordService>
{
public:
    static StoryChunkRecordService*CreateStoryChunkRecordService(tl::engine &tl_engine
                                                                 , tl::managed <tl::pool> &tl_pool
                                                                 , uint16_t service_provider_id)
    {
        return new StoryChunkRecordService(tl_engine, tl_pool, service_provider_id);
    }

    StoryChunkRecordService(tl::engine &tl_engine, tl::managed <tl::pool> &tl_pool, uint16_t service_provider_id)
            : tl::provider <StoryChunkRecordService>(tl_engine, service_provider_id)
    {
        define("record_story_chunk", &StoryChunkRecordService::record_story_chunk, *tl_pool,
                            tl::ignore_return_value());
        get_engine().push_finalize_callback(this, [p = this]()
        { delete p; });
        LOG_DEBUG("[StoryChunkRecordService] StoryChunkRecordService setup complete");
    }

    ~StoryChunkRecordService()
    {
        LOG_DEBUG("[StoryChunkRecordService] Destructor called. Cleaning up...");
        get_engine().pop_finalize_callback(this);
    }

    void record_story_chunk(tl::request const &request, tl::bulk &b)
    {
        try
        {
            uint64_t esid = tl::xstream::self_rank();
            uint64_t tid = tl::thread::self_id();
            std::chrono::high_resolution_clock::time_point start, end;
            LOG_DEBUG("[StoryChunkRecordService] ES{}:T{}: StoryChunk recording RPC invoked", esid, tid);
            tl::endpoint ep = request.get_endpoint();
            LOG_DEBUG("[StoryChunkRecordService] ES{}:T{}: Endpoint obtained", esid, tid);
            std::vector <char> mem_vec(MAX_BULK_MEM_SIZE);
            std::vector <std::pair <void*, std::size_t>> segments(1);
            segments[0].first = (void*)(&mem_vec[0]);
            segments[0].second = mem_vec.size();
            LOG_DEBUG("[StoryChunkRecordService] ES{}:T{}: Bulk memory prepared, size: {}", esid, tid, mem_vec.size());
            tl::engine tl_engine = get_engine();
            LOG_DEBUG("[StoryChunkRecordService] ES{}:T{}: Engine addr: {}", esid, tid, (void*)&tl_engine);
            tl::bulk local = tl_engine.expose(segments, tl::bulk_mode::write_only);
            LOG_DEBUG("[StoryChunkRecordService] ES{}:T{}: Bulk memory exposed", esid, tid);
            b.on(ep) >> local;
            LOG_DEBUG("[StoryChunkRecordService] ES{}:T{}: Received {} bytes of StoryChunk data", esid, tid, b.size());
            chronolog::StoryChunk story_chunk;
            start = std::chrono::high_resolution_clock::now();
            deserializedWithCereal(&mem_vec[0], b.size() - 1, story_chunk);
            end = std::chrono::high_resolution_clock::now();
            LOG_DEBUG("[StoryChunkRecordService] ES{}:T{}: StoryChunk received: StoryID: {}, StartTime: {}", esid, tid
                      , story_chunk.getStoryId(), story_chunk.getStartTime());
            LOG_INFO("[StoryChunkRecordService] ES{}:T{}: Deserialization took {} us", esid, tid,
                    std::chrono::duration_cast <std::chrono::nanoseconds>(end - start).count() / 1000.0);
            request.respond(b.size());
            LOG_DEBUG("[StoryChunkRecordService] ES{}:T{}: StoryChunk recording RPC responded {}", esid, tid, b.size());
        }
        catch(tl::exception &e)
        {
            LOG_ERROR("[StoryChunkRecordService] Fail to ingest story chunk, Thallium exception caught: {}", e.what());
        }
        catch(cereal::Exception &e)
        {
            LOG_ERROR("[StoryChunkRecordService] Fail to ingest story chunk, Cereal exception caught: {}", e.what());
        }
        catch(std::exception &e)
        {
            LOG_ERROR("[StoryChunkRecordService] Fail to ingest story chunk, std::exception caught: {}", e.what());
        }
        catch(...)
        {
            LOG_ERROR("[StoryChunkRecordService] Fail to ingest story chunk, unknown exception caught");
        }
    }

private:
    void deserializedWithCereal(char*buffer, size_t size, chronolog::StoryChunk &story_chunk)
    {
        std::string str(buffer, buffer + size);
        std::istringstream iss(str);
        cereal::BinaryInputArchive iarchive(iss);
        iarchive(story_chunk);
    }
};

int main(int argc, char**argv)
{
    std::string conf_file_path;
    conf_file_path = parse_conf_path_arg(argc, argv);
    ChronoLog::ConfigurationManager confManager(conf_file_path);
    int result = chronolog::chrono_monitor::initialize("console", confManager.GRAPHER_CONF.LOG_CONF.LOGFILE
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
            confManager.GRAPHER_CONF.KEEPER_GRAPHER_DRAIN_SERVICE_CONF.PROTO_CONF + "://" +
            confManager.GRAPHER_CONF.KEEPER_GRAPHER_DRAIN_SERVICE_CONF.IP + ":" +
            std::to_string(confManager.GRAPHER_CONF.KEEPER_GRAPHER_DRAIN_SERVICE_CONF.BASE_PORT);
    tl::engine extraction_engine = tl::engine(KEEPER_COLLECTOR_NA_STRING, THALLIUM_SERVER_MODE);
    std::stringstream ss;
    ss << extraction_engine.self();
    LOG_DEBUG("[standalone_ingest_test] Engine: {}", ss.str());

    std::vector <tl::managed <tl::xstream>> tl_es_vec;
    tl::managed <tl::pool> tl_pool = tl::pool::create(tl::pool::access::spmc);
    LOG_DEBUG("[standalone_ingest_test] Pool created");
    for(int j = 0; j < NUM_STREAMS; j++)
    {
        tl::managed <tl::xstream> es = tl::xstream::create(tl::scheduler::predef::deflt, *tl_pool);
        LOG_DEBUG("[standalone_ingest_test] A new stream is created");
        tl_es_vec.push_back(std::move(es));
    }

    int service_provider_id = confManager.GRAPHER_CONF.KEEPER_GRAPHER_DRAIN_SERVICE_CONF.SERVICE_PROVIDER_ID;
    StoryChunkRecordService*story_chunk_recording_service = StoryChunkRecordService::CreateStoryChunkRecordService(
            extraction_engine, tl_pool, service_provider_id);
    LOG_DEBUG("[standalone_ingest_test] StoryChunkRecordService created");

    extraction_engine.wait_for_finalize();
    for(int j = 0; j < NUM_STREAMS; j++)
    {
        tl_es_vec[j]->join();
    }

    delete story_chunk_recording_service;

    return 0;
}
