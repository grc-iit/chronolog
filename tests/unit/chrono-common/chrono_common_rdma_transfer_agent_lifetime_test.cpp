// Deleting one RDMATransferAgent leaves the other agents of the same engine
// working (#707, #743). A keeper holds one agent per receiver (its grapher and
// its player). Every agent sends receive_story_chunk through its engine's
// RpcBulkOwner, and two receivers with the same provider id share that RPC ID
// on the keeper's engine. Before #740 the agent's destructor deregistered it,
// and the next chunk of every other agent failed with HG_NOENTRY.
//
// Each receiver is a grapher's real receive service on an engine of its own,
// with the same provider id; the sender engine has one agent per receiver.
// Over ofi+sockets on 127.0.0.1. Engines are left to process exit, as the
// communication tests of the chunk transfer do.

#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <thallium.hpp>
#include <thallium/serialization/stl/string.hpp>

#include <GrapherRecordingService.h>
#include <RDMATransferAgent.h>
#include <ServiceId.h>
#include <StoryWatermarkRegistry.h>
#include <chrono_monitor.h>
#include <chronolog_errcode.h>
#include <private/RpcBulkOwner.h>

namespace chl = chronolog;
namespace tl = thallium;

namespace
{
constexpr char kProtocol[] = "ofi+sockets";
constexpr uint16_t kReceiverProvider = 42;
constexpr chl::StoryId kStory = 19;

void ensureLogger()
{
    static bool done = false;
    if(!done)
    {
        chl::chrono_monitor::initialize("console", "", chl::LogLevel::err, "rdma_transfer_agent_lifetime_test_logger");
        done = true;
    }
}

bool waitUntil(std::function<bool()> const& condition, std::chrono::seconds timeout = std::chrono::seconds(10))
{
    auto const until = std::chrono::steady_clock::now() + timeout;
    while(!condition())
    {
        if(std::chrono::steady_clock::now() >= until)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

// A grapher's receive side: its RpcBulkOwner, its recording service, and the
// ingestion handle the received keeper partial chunks of kStory land in.
struct Receiver
{
    struct Ingested
    {
        chl::ChunkIngestionQueue queue;
        std::mutex mutex;
        chl::StoryChunkDeque active, passive;
        chl::StoryChunkIngestionHandle handle{mutex, &active, &passive};
        chl::StoryWatermarkRegistry registry;
        ~Ingested()
        {
            for(auto* chunk: active) delete chunk;
            for(auto* chunk: passive) delete chunk;
        }
        size_t count()
        {
            std::lock_guard<std::mutex> lock(mutex);
            return active.size() + passive.size();
        }
    };

    explicit Receiver(chl::RpcTimeoutPolicy const& policy)
        : engine(std::make_shared<tl::engine>(std::string(kProtocol) + "://127.0.0.1", THALLIUM_SERVER_MODE, true, 1))
        , runtime(std::make_shared<chl::rpc::RpcRuntime>())
        , ingested(std::make_shared<Ingested>())
    {
        owner = chl::rpc::RpcBulkOwner::Create(engine, runtime, policy, false);
        owner->dependencies = ingested;
        ingested->queue.addStoryIngestionHandle(kStory, &ingested->handle);
        service = chl::GrapherRecordingService::CreateRecordingService(owner,
                                                                       kReceiverProvider,
                                                                       ingested->queue,
                                                                       {},
                                                                       &ingested->registry);
    }

    chl::ServiceId id() const
    {
        std::string const self = engine->self();
        auto const port = static_cast<uint16_t>(std::stoul(self.substr(self.rfind(':') + 1)));
        return chl::ServiceId(kProtocol, "127.0.0.1", port, kReceiverProvider);
    }

    std::shared_ptr<tl::engine> engine;
    std::shared_ptr<chl::rpc::RpcRuntime> runtime;
    std::shared_ptr<Ingested> ingested;
    std::shared_ptr<chl::rpc::RpcBulkOwner> owner;
    chl::GrapherRecordingService* service = nullptr;
};

chl::StoryChunk chunkAt(uint64_t start)
{
    chl::StoryChunk chunk("chronicle", "story", kStory, start, start + 100);
    chunk.insertEvent(chl::LogEvent(kStory, start + 50, 1, 0, std::string(4096, 'c')));
    return chunk;
}
} // namespace

// The keeper deletes the agent of a receiver it no longer sends to. The agent
// of the other receiver still ships a chunk to it.
TEST(RDMATransferAgentLifetime, DeletingOneAgentLeavesTheOtherWorking)
{
    ensureLogger();
    chl::RpcTimeoutPolicy policy;
    policy.attempt_ms = 2000;
    policy.transfer_ms = 2000;
    auto* stayingReceiver = new Receiver(policy);
    auto* leavingReceiver = new Receiver(policy);
    ASSERT_NE(stayingReceiver->service, nullptr);
    ASSERT_NE(leavingReceiver->service, nullptr);

    auto senderEngine =
            std::make_shared<tl::engine>(std::string(kProtocol) + "://127.0.0.1", THALLIUM_CLIENT_MODE, true, 1);
    auto senderRuntime = std::make_shared<chl::rpc::RpcRuntime>();
    auto sender = chl::rpc::RpcBulkOwner::Create(senderEngine, senderRuntime, policy, true);
    std::unique_ptr<chl::RDMATransferAgent> staying(
            chl::RDMATransferAgent::CreateRDMATransferAgent(sender, stayingReceiver->id()));
    chl::RDMATransferAgent* leaving = chl::RDMATransferAgent::CreateRDMATransferAgent(sender, leavingReceiver->id());
    ASSERT_NE(staying, nullptr);
    ASSERT_NE(leaving, nullptr);
    ASSERT_EQ(leaving->transfer_story_chunk(chunkAt(100)), chl::CL_SUCCESS);
    ASSERT_TRUE(waitUntil([&] { return leavingReceiver->ingested->count() == 1; }));

    delete leaving;

    EXPECT_EQ(staying->transfer_story_chunk(chunkAt(200)), chl::CL_SUCCESS)
            << "the agent that stayed could not send its chunk";
    EXPECT_TRUE(waitUntil([&] { return stayingReceiver->ingested->count() == 1; }));
    EXPECT_EQ(leavingReceiver->ingested->count(), 1u);
    // The receivers, the sender and their engines are left to process exit.
}
