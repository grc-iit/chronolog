#include <thallium.hpp>
#include <thallium/serialization/stl/string.hpp>
#include <GrapherRecordingService.h>
#include <RDMATransferAgent.h>
#include <private/RpcBulkOwner.h>
#include <chrono_monitor.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
using namespace std::chrono_literals;
namespace tl = thallium;
namespace chl = chronolog;
using Clock = std::chrono::steady_clock;
int main(int argc, char** argv)
{
    if(argc != 3)
        return 2;
    chl::chrono_monitor::initialize("console", "", chl::LogLevel::err, "bulk_receiver");
    margo_set_environment(nullptr);
    ABT_init(0, nullptr);
    chl::RpcTimeoutPolicy policy;
    policy.attempt_ms = 80;
    policy.transfer_ms = 160;
    auto engine = std::make_shared<tl::engine>(std::string(argv[1]) + "://127.0.0.1", THALLIUM_SERVER_MODE, true, 1);
    auto receiveRuntime = std::make_shared<chl::rpc::RpcRuntime>();
    auto receiveOwner = chl::rpc::RpcBulkOwner::Create(engine, receiveRuntime, policy, false);
    struct State
    {
        std::atomic<unsigned> barriers{0};
        std::atomic<bool> release{false};
        chl::ChunkIngestionQueue queue;
        std::mutex mutex;
        chl::StoryChunkDeque active, passive;
        chl::StoryChunkIngestionHandle handle{mutex, &active, &passive};
        ~State()
        {
            for(auto* chunk: active) delete chunk;
            for(auto* chunk: passive) delete chunk;
        }
    };
    auto state = std::make_shared<State>();
    receiveOwner->dependencies = state;
    auto& barriers = state->barriers;
    auto& release = state->release;
    auto& queue = state->queue;
    auto& mutex = state->mutex;
    auto& active = state->active;
    std::string scenario = argv[2];
    bool malformed = scenario == "decode-error" || scenario == "failure-reply-error";
    bool failure = scenario == "reply-error" || scenario == "failure-reply-error";
    if(failure)
        receiveOwner->incoming.observers.beforeAnswer = [state]
        {
            ++state->barriers;
            throw std::runtime_error("injected native response failure");
        };
    else if(!malformed)
        receiveOwner->incoming.observers.allocated = [state, engine]
        {
            ++state->barriers;
            while(!state->release) margo_thread_sleep(engine->get_margo_instance(), 1);
        };
    queue.addStoryIngestionHandle(19, &state->handle);
    auto service = chl::GrapherRecordingService::CreateRecordingService(*engine, 42, queue);
    if(!service)
        return 3;
    std::string self = engine->self();
    auto port = static_cast<uint16_t>(std::stoul(self.substr(self.rfind(':') + 1)));
    std::vector<std::shared_ptr<chl::rpc::RpcRuntime>> runtimes;
    std::vector<std::shared_ptr<chl::rpc::RpcBulkOwner>> owners;
    std::vector<std::unique_ptr<chl::RDMATransferAgent>> senders;
    for(int i = 0; i < 3; ++i)
    {
        auto senderEngine =
                std::make_shared<tl::engine>(std::string(argv[1]) + "://127.0.0.1", THALLIUM_CLIENT_MODE, true, 1);
        auto runtime = std::make_shared<chl::rpc::RpcRuntime>();
        auto owner = chl::rpc::RpcBulkOwner::Create(senderEngine, runtime, policy, true);
        senders.emplace_back(
                chl::RDMATransferAgent::CreateRDMATransferAgent(owner, chl::ServiceId(argv[1], "127.0.0.1", port, 42)));
        runtimes.push_back(runtime);
        owners.push_back(owner);
    }
    chl::StoryChunk chunk("chron", "story", 19, 100, 200);
    chunk.insertEvent(chl::LogEvent(19, 150, 1, 0, "received-event"));
    std::atomic<int> first{123}, second{123};
    std::thread a(
            [&] {
                first = malformed ? senders[0]->transfer_serialized_story_chunk("bad")
                                  : senders[0]->transfer_story_chunk(chunk);
            });
    std::thread b;
    if(!failure && !malformed)
        b = std::thread([&] { second = senders[1]->transfer_story_chunk(chunk); });
    auto until = Clock::now() + 1s;
    while(!failure && !malformed && barriers < 2 && Clock::now() < until) std::this_thread::sleep_for(1ms);
    int third = (failure || malformed) ? chl::CL_ERR_OUTCOME_UNCONFIRMED : senders[2]->transfer_story_chunk(chunk);
    until = Clock::now() + 1s;
    while(!failure && !malformed && receiveOwner->incoming.rejected() == 0 && Clock::now() < until)
        std::this_thread::sleep_for(1ms);
    bool capped = failure || malformed ||
                  (barriers == 2 && receiveOwner->incoming.used() == 2 && third == chl::CL_ERR_OUTCOME_UNCONFIRMED);
    release = true;
    a.join();
    if(b.joinable())
        b.join();
    until = Clock::now() + 2s;
    unsigned expected = malformed ? 0 : (failure ? 1 : 2);
    bool firstCollected = first != chl::CL_ERR_OUTCOME_UNCONFIRMED;
    bool secondCollected = second != chl::CL_ERR_OUTCOME_UNCONFIRMED;
    bool boundaryObserved = false;
    while(Clock::now() < until)
    {
        bool ingested;
        {
            std::lock_guard<std::mutex> lock(mutex);
            ingested = active.size() >= expected;
        }
        if(!failure && !malformed && ingested && receiveOwner->incoming.used() == 0)
        {
            // Known late replies retain their originating worker/snapshot until
            // consumed. Collect those exact outcomes before asserting teardown.
            if(!firstCollected)
                firstCollected = senders[0]->transfer_story_chunk(chunk) == chl::CL_SUCCESS;
            if(!secondCollected)
                secondCollected = senders[1]->transfer_story_chunk(chunk) == chl::CL_SUCCESS;
        }
        bool settled = failure ? (barriers == 1 && receiveOwner->incoming.used() == 1)
                               : (receiveOwner->incoming.used() == 0 && owners[0]->sources.used() == 0 &&
                                  owners[1]->sources.used() == 0);
        if(ingested && settled)
        {
            boundaryObserved = true;
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    size_t chunks;
    {
        std::lock_guard<std::mutex> lock(mutex);
        chunks = active.size();
    }
    capped = capped && chunks == expected && boundaryObserved;
    bool guarded = !failure || (first == chl::CL_ERR_OUTCOME_UNCONFIRMED && barriers == 1 && chunks == expected &&
                                receiveOwner->incoming.used() == 1);
    if(malformed && !failure)
        guarded = first == chl::CL_ERR_STORY_CHUNK_EXTRACTION && chunks == 0 && receiveOwner->incoming.used() == 0;
    std::cout << "barriers=" << barriers << " incoming=" << receiveOwner->incoming.used() << " first=" << first
              << " second=" << second << " excess=" << third << " ingested=" << chunks
              << " boundary_observed=" << boundaryObserved << " source_permits=" << owners[0]->sources.used() << ","
              << owners[1]->sources.used() << "," << owners[2]->sources.used() << std::endl;
    // Harness has no proof for quarantined peers. Deliberately retain the
    // native owners; production gates likewise do not call finalize here.
    if(!capped || !guarded)
        return 1;
    std::cout << "PASS actual grapher receiver " << argv[2] << std::endl;
    return 0;
}
