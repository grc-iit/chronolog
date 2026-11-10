// The grapher's receive_story_chunk against a keeper's RDMATransferAgent, both
// on native engines in this process: how a keeper partial chunk is pulled and
// what the keeper is answered. One scenario per run:
//   pieces    the chunk is pulled in pieces of transfer_piece_bytes, and in
//             1 MiB pieces by default, and arrives intact
//   checksum  bytes that do not match the keeper's CRC-32C are refused with
//             nothing ingested; the same chunk sent correctly is ingested
//   duplicate a resend of a written chunk gets the first receipt and is not
//             ingested again; a resend before the write is ingested again
#include <thallium.hpp>
#include <thallium/serialization/stl/string.hpp>
#include <GrapherRecordingService.h>
#include <RDMATransferAgent.h>
#include <private/Crc32c.h>
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
namespace
{
bool until(std::function<bool()> const& condition, std::chrono::milliseconds wait = 3000ms)
{
    auto end = Clock::now() + wait;
    while(!condition())
    {
        if(Clock::now() >= end)
            return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}
struct Ingested
{
    chl::ChunkIngestionQueue queue;
    std::mutex mutex;
    chl::StoryChunkDeque active, passive;
    chl::StoryChunkIngestionHandle handle{mutex, &active, &passive};
    ~Ingested()
    {
        for(auto* chunk: active) delete chunk;
        for(auto* chunk: passive) delete chunk;
    }
    size_t count()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return active.size();
    }
    std::string firstRecord(size_t at)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if(at >= active.size() || active[at]->empty())
            return {};
        return active[at]->begin()->second.logRecord;
    }
};
bool fail(std::string const& why)
{
    std::cout << "FAIL " << why << std::endl;
    return false;
}
} // namespace
int main(int argc, char** argv)
{
    if(argc != 3)
        return 2;
    std::string const protocol = argv[1], scenario = argv[2];
    chl::chrono_monitor::initialize("console", "", chl::LogLevel::err, "chunk_receive");
    margo_set_environment(nullptr);
    ABT_init(0, nullptr);
    chl::RpcTimeoutPolicy policy;
    policy.attempt_ms = 2000;
    policy.transfer_ms = 2000;
    auto engine = std::make_shared<tl::engine>(protocol + "://127.0.0.1", THALLIUM_SERVER_MODE, true, 1);
    auto receiveRuntime = std::make_shared<chl::rpc::RpcRuntime>();
    auto receiveOwner = chl::rpc::RpcBulkOwner::Create(engine, receiveRuntime, policy, false);
    auto ingested = std::make_shared<Ingested>();
    receiveOwner->dependencies = ingested;
    ingested->queue.addStoryIngestionHandle(19, &ingested->handle);
    auto pieces = std::make_shared<std::atomic<unsigned>>(0);
    receiveOwner->incoming.observers.initiated = [pieces](margo_request) { ++*pieces; };
    if(scenario == "pieces")
        receiveOwner->incoming.pieceBytes = 4096;
    auto registry = std::make_shared<chl::StoryWatermarkRegistry>();
    receiveOwner->dependencies =
            std::make_shared<std::pair<std::shared_ptr<Ingested>, std::shared_ptr<void>>>(ingested, registry);
    if(!chl::GrapherRecordingService::CreateRecordingService(receiveOwner, 42, ingested->queue, {}, registry.get()))
        return 3;
    std::string self = engine->self();
    auto port = static_cast<uint16_t>(std::stoul(self.substr(self.rfind(':') + 1)));
    auto senderEngine = std::make_shared<tl::engine>(protocol + "://127.0.0.1", THALLIUM_CLIENT_MODE, true, 1);
    auto sendRuntime = std::make_shared<chl::rpc::RpcRuntime>();
    auto sendOwner = chl::rpc::RpcBulkOwner::Create(senderEngine, sendRuntime, policy, true);
    std::unique_ptr<chl::RDMATransferAgent> sender(
            chl::RDMATransferAgent::CreateRDMATransferAgent(sendOwner,
                                                            chl::ServiceId(protocol, "127.0.0.1", port, 42)));
    bool pass = sender != nullptr;
    if(pass && scenario == "pieces")
    {
        chl::StoryChunk small("chron", "story", 19, 100, 200);
        small.insertEvent(chl::LogEvent(19, 150, 1, 0, std::string(20000, 's')));
        auto size = chl::RDMATransferAgent::serialize(small)->bytes.size();
        pass = sender->transfer_story_chunk(small) == chl::CL_SUCCESS || fail("small chunk not acknowledged");
        pass = pass && (until([&] { return ingested->count() == 1; }) || fail("small chunk not ingested"));
        unsigned expected = static_cast<unsigned>((size + 4095) / 4096);
        pass = pass && (*pieces == expected || fail("small chunk: " + std::to_string(*pieces) + " pieces, expected " +
                                                    std::to_string(expected)));
        pass = pass && (ingested->firstRecord(0) == std::string(20000, 's') || fail("small chunk changed"));
        std::cout << "transfer_piece_bytes=4096 bytes=" << size << " pieces=" << *pieces << std::endl;
        // The default: 1 MiB pieces.
        receiveOwner->incoming.pieceBytes = chl::rpc::kBulkPieceBytes;
        *pieces = 0;
        chl::StoryChunk large("chron", "story", 19, 200, 300);
        large.insertEvent(chl::LogEvent(19, 250, 1, 1, std::string(2621440, 'L')));
        size = chl::RDMATransferAgent::serialize(large)->bytes.size();
        pass = pass && (sender->transfer_story_chunk(large) == chl::CL_SUCCESS || fail("large chunk not acknowledged"));
        pass = pass && (until([&] { return ingested->count() == 2; }) || fail("large chunk not ingested"));
        pass = pass && (*pieces == 3 || fail("large chunk: " + std::to_string(*pieces) + " pieces, expected 3"));
        pass = pass && (ingested->firstRecord(1) == std::string(2621440, 'L') || fail("large chunk changed"));
        std::cout << "transfer_piece_bytes=1048576 bytes=" << size << " pieces=" << *pieces << std::endl;
    }
    else if(pass && scenario == "checksum")
    {
        chl::StoryChunk chunk("chron", "story", 19, 100, 200);
        chunk.insertEvent(chl::LogEvent(19, 150, 1, 0, "checked record"));
        auto serialized = chl::RDMATransferAgent::serialize(chunk);
        std::string bytes = serialized->bytes;
        auto bulk = senderEngine->expose({{bytes.data(), bytes.size()}}, tl::bulk_mode::read_only);
        auto remote = senderEngine->define("receive_story_chunk");
        tl::provider_handle peer(senderEngine->lookup(self), 42);
        chl::ChunkReceipt refused = remote.on(peer)(bulk, chl::ServiceId{}, serialized->checksum ^ 1u);
        pass = (refused.bytes == 0 && refused.receipt == 0) || fail("a wrong checksum was acknowledged");
        pass = pass && (until([&] { return receiveOwner->incoming.used() == 0; }) || fail("receive slot kept"));
        pass = pass && (ingested->count() == 0 || fail("a chunk with a wrong checksum was ingested"));
        pass = pass && (sender->transfer_story_chunk(chunk) == chl::CL_SUCCESS || fail("correct chunk refused"));
        pass = pass && (until([&] { return ingested->count() == 1; }) || fail("correct chunk not ingested"));
        pass = pass && (ingested->firstRecord(0) == "checked record" || fail("correct chunk changed"));
        std::cout << "refused bytes=" << refused.bytes << " then ingested=" << ingested->count() << std::endl;
    }
    else if(pass && scenario == "duplicate")
    {
        chl::ServiceId reporter(protocol, "127.0.0.1", 5555, 55);
        chl::StoryChunk chunk("chron", "story", 19, 100, 200);
        chunk.insertEvent(chl::LogEvent(19, 150, 1, 0, "sent twice"));
        chl::ChunkReceipt first, second, third;
        pass = sender->transfer_story_chunk(chunk, reporter, &first) == chl::CL_SUCCESS || fail("first send");
        pass = pass && (until([&] { return ingested->count() == 1; }) || fail("first send not ingested"));
        // Not written yet: the resend is ingested again with a new receipt.
        pass = pass && (sender->transfer_story_chunk(chunk, reporter, &second) == chl::CL_SUCCESS ||
                        fail("resend before the write"));
        pass = pass && (until([&] { return ingested->count() == 2; }) || fail("unwritten resend not ingested"));
        pass = pass && (second.receipt > first.receipt || fail("unwritten resend kept the old receipt"));
        // The newest delivery is written; a resend now gets its receipt only.
        registry->holdReceipt(19, second.receipt);
        registry->receiptMerged(19, second.receipt);
        registry->releaseReceipt(19, second.receipt);
        pass = pass && (sender->transfer_story_chunk(chunk, reporter, &third) == chl::CL_SUCCESS ||
                        fail("resend after the write"));
        std::this_thread::sleep_for(200ms);
        pass = pass && (ingested->count() == 2 || fail("a written chunk was ingested again"));
        pass = pass && (third.receipt == second.receipt || fail("resend after the write got another receipt"));
        pass = pass && (third.grapher_instance == registry->instanceId() || fail("receipt from another instance"));
        std::cout << "receipts first=" << first.receipt << " second=" << second.receipt << " third=" << third.receipt
                  << " ingested=" << ingested->count() << std::endl;
    }
    else if(pass)
        pass = fail("unknown scenario " + scenario);
    std::cout << (pass ? "PASS " : "FAIL ") << protocol << ' ' << scenario << std::endl;
    // Engines are left to process exit, as rpc_bulk_receiver_test does.
    return pass ? 0 : 1;
}
