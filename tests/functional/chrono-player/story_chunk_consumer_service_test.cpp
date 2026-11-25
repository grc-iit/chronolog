
#include <chrono>
#include <iostream>
#include <signal.h>
#include <thread>

#include <chrono_monitor.h>
#include <ServiceId.h>
#include <StoryChunk.h>
#include <StoryChunkIngestionQueue.h>
#include <StoryChunkConsumerService.h>
#include <private/RpcBulkControl.h>

namespace tl = thallium;
namespace chl = chronolog;

bool keep_running = true;
void sigterm_handler(int)
{
    std::cout << "Received SIGTERM signal. Initiating shutdown procedure." << std::endl;
    keep_running = false;
    return;
}


int main()
{

    signal(SIGTERM, sigterm_handler);

    int result = chronolog::chrono_monitor::initialize(
            "file" //confManager.CLIENT_CONF.CLIENT_LOG_CONF.LOGTYPE
            ,
            "/tmp/chunk_consumer_test.log" //, confManager.CLIENT_CONF.CLIENT_LOG_CONF.LOGFILE
            ,
            chronolog::LogLevel::debug // confManager.CLIENT_CONF.CLIENT_LOG_CONF.LOGLEVEL
            ,
            "ChunkConsumerTest" //confManager.CLIENT_CONF.CLIENT_LOG_CONF.LOGNAME
            ,
            100000000 //confManager.CLIENT_CONF.CLIENT_LOG_CONF.LOGFILESIZE
            ,
            2 //confManager.CLIENT_CONF.CLIENT_LOG_CONF.LOGFILENUM
            ,
            chronolog::LogLevel::debug //confManager.CLIENT_CONF.CLIENT_LOG_CONF.FLUSHLEVEL);
    );


    chl::ServiceId localServiceId("ofi+sockets", "127.0.0.1", 3333, 33);

    std::string LOCAL_SERVICE_NA_STRING;
    localServiceId.get_service_as_string(LOCAL_SERVICE_NA_STRING);

    tl::engine* localEngine = nullptr;
    chl::StoryChunkConsumerService* chunkConsumerService;

    chl::StoryChunkIngestionQueue ingestionQueue;
    auto runtime = std::make_shared<chl::rpc::RpcRuntime>();
    std::shared_ptr<chl::rpc::RpcBulkOwner> owner;
    chl::rpc::BulkScopeFence fence(owner, runtime);

    try
    {
        margo_instance_id margo_id = margo_init(LOCAL_SERVICE_NA_STRING.c_str(), MARGO_SERVER_MODE, 1, 1);
        auto native = std::make_shared<tl::engine>(margo_id);
        owner = chl::rpc::RpcBulkOwner::Create(native, runtime, chl::RpcTimeoutPolicy{}, false);
        localEngine = native.get();


        LOG_INFO("[ChunkConsumerServiceTest] starting StoryChunkConsumerService at {}", chl::to_string(localServiceId));

        chunkConsumerService =
                chronolog::StoryChunkConsumerService::CreateChunkConsumerService(owner,
                                                                                 localServiceId.getProviderId(),
                                                                                 ingestionQueue);
    }
    catch(tl::exception const&)
    {
        LOG_ERROR("[ChunkConsumerServiceTest] failed to create StoryChunkConsumerService");
        chunkConsumerService = nullptr;
        return (-1);
    }

    static uint32_t thread_id = 0;
    while(true == keep_running)
    {
        LOG_INFO("[ChunkConsumerServiceTest] Standalone Chunk Consumer Service is running");

        std::this_thread::sleep_for(std::chrono::seconds(20));
    }


    LOG_INFO("[ChunkConsumerServiceTest] Shutting down StoryChunkConsumerService at {}",
             chl::to_string(localServiceId));

    chl::rpc::retainBulkDependencies(owner, runtime);
    chunkConsumerService = nullptr;
    localEngine = nullptr;
    return 1;
}
