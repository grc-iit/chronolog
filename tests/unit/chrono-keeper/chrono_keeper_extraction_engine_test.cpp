// The keeper's extraction engine must make progress on its own. The keeper's
// main thread is the primary execution stream, and between stats messages it
// sits in an OS sleep(10) that never yields to Argobots. A client engine
// without a progress thread runs its progress loop in the primary pool, so a
// chunk sent from an extraction stream waited for the next stats message:
// about three chunks per keeper every 10 s reached the grapher. Here the test's
// main thread is the primary stream and blocks the same way while a ULT on
// another stream ships a chunk through the RDMATransferAgent, as the keeper's
// extraction streams do.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <abt.h>
#include <thallium.hpp>
#include <thallium/serialization/stl/string.hpp>

#include <chrono_monitor.h>
#include <chronolog_errcode.h>
#include <ChunkReceipt.h>
#include <RDMATransferAgent.h>
#include <ServiceId.h>

#include <KeeperExtractionEngine.h>

namespace chl = chronolog;
namespace tl = thallium;

namespace
{
constexpr char kProtocol[] = "ofi+sockets";
constexpr uint16_t kReceiverProvider = 31;

void ensureLogger()
{
    static bool done = false;
    if(!done)
    {
        chl::chrono_monitor::initialize("console", "", chl::LogLevel::err, "extraction_engine_test_logger");
        done = true;
    }
}

// Initialized here, on the test's main thread, so that thread is the primary
// execution stream, as the keeper's main thread is.
void ensureArgobots()
{
    static bool done = false;
    if(!done)
    {
        ABT_init(0, nullptr);
        done = true;
    }
}
} // namespace

TEST(KeeperExtractionEngine, ShipsAChunkWhileTheMainThreadSleeps)
{
    ensureLogger();
    ensureArgobots();

    // A grapher stand-in: pulls the chunk's bulk and answers with its size,
    // as the grapher's receive_story_chunk does.
    tl::engine receiver(std::string(kProtocol) + "://127.0.0.1", THALLIUM_SERVER_MODE, true, 1);
    receiver.define(
            "receive_story_chunk",
            [&receiver](tl::request const& request, tl::bulk& b, chl::ServiceId const&)
            {
                std::vector<char> buffer(b.size());
                std::vector<std::pair<void*, std::size_t>> segments(1);
                segments[0].first = buffer.data();
                segments[0].second = buffer.size();
                tl::bulk local = receiver.expose(segments, tl::bulk_mode::write_only);
                b.on(request.get_endpoint()) >> local;
                chl::ChunkReceipt answer;
                answer.bytes = b.size();
                request.respond(answer);
            },
            kReceiverProvider,
            receiver.get_handler_pool());
    std::string const self = receiver.self();
    auto const port = static_cast<uint16_t>(std::stoul(self.substr(self.rfind(':') + 1)));
    chl::ServiceId const receiver_id(kProtocol, "127.0.0.1", port, kReceiverProvider);

    std::unique_ptr<tl::engine> sender(chl::createExtractionEngine(kProtocol));
    std::unique_ptr<chl::RDMATransferAgent> agent(
            chl::RDMATransferAgent::CreateRDMATransferAgent(*sender, receiver_id));
    ASSERT_NE(agent, nullptr);

    std::string const chunk(64 * 1024, 'c');
    std::atomic<bool> transferred{false};
    std::atomic<int> status{chl::CL_ERR_UNKNOWN};
    chl::ChunkReceipt receipt;
    tl::managed<tl::xstream> stream = tl::xstream::create();
    tl::managed<tl::thread> sender_ult = stream->make_thread(
            [&]()
            {
                status = agent->transfer_serialized_story_chunk(chunk, chl::ServiceId(), &receipt);
                transferred = true;
            });

    // Block the primary stream with an OS wait, as the keeper's main loop
    // does with sleep(10), for longer than a transfer on loopback takes.
    auto const start = std::chrono::steady_clock::now();
    auto const deadline = start + std::chrono::seconds(5);
    while(!transferred && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    bool const transferred_while_blocked = transferred;
    auto const waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);

    // Joining yields the primary stream, so an engine that depends on it
    // finishes the transfer here and the test still ends cleanly.
    sender_ult->join();
    stream->join();

    EXPECT_TRUE(transferred_while_blocked) << "the transfer did not complete in " << waited.count()
                                           << " ms while the primary execution stream was blocked";
    EXPECT_EQ(status.load(), chl::CL_SUCCESS);
    EXPECT_EQ(receipt.bytes, chunk.size());

    agent.reset();
    sender->finalize();
    sender.reset();
    receiver.finalize();
}
