#include <thallium.hpp>
#include <thallium/serialization/stl/string.hpp>
#include <RDMATransferAgent.h>
#include <StoryChunk.h>
#include <private/RpcBulkOwner.h>
#include <private/RpcBulkSerialization.h>
#include <chrono_monitor.h>
#include <client_errcode.h>
#include <RpcRuntime.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <numeric>
#include <vector>
using namespace std::chrono_literals;
namespace tl = thallium;
namespace chl = chronolog;
using Clock = std::chrono::steady_clock;
static void require(bool value, char const* text)
{
    if(!value)
        throw std::runtime_error(text);
}
template <class T>
auto prepareHook(T& owner, std::function<void()> hook, int) -> decltype(owner.beforeSerialize = hook, void())
{
    owner.beforeSerialize = std::move(hook);
}
template <class T>
void prepareHook(T&, std::function<void()>, long)
{}
int main(int argc, char** argv)
{
    if(argc < 2 || argc > 3)
        return 2;
    std::string scenario = argc == 3 ? argv[2] : "late";
    chl::chrono_monitor::initialize("console", "", chl::LogLevel::err, "bulk_lifetime");
    margo_set_environment(nullptr);
    ABT_init(0, nullptr);
    auto sender = std::make_shared<tl::engine>(std::string(argv[1]) + "://127.0.0.1", THALLIUM_CLIENT_MODE, true, 1);
    auto receiver = std::make_shared<tl::engine>(std::string(argv[1]) + "://127.0.0.1", THALLIUM_SERVER_MODE, true, 1);
    auto runtime = std::make_shared<chl::rpc::RpcRuntime>();
    chl::RpcTimeoutPolicy policy;
    policy.attempt_ms = 80;
    policy.transfer_ms = 160;
    bool during = scenario == "during" || scenario == "large-chunk";
    if(during)
    {
        policy.attempt_ms = 200;
        policy.transfer_ms = 400;
    }
    if(scenario == "default" || scenario == "large-chunk")
        policy = chl::RpcTimeoutPolicy{};
    auto owner = chl::rpc::RpcBulkOwner::Create(sender, runtime, policy, true);
    struct State
    {
        std::atomic<bool> released{false};
        std::atomic<unsigned> started{0}, completed{0};
        std::atomic<uint64_t> checksum{0};
        std::atomic<bool> pending{false}, sawDifferent{false}, sawReporter{false}, sawRoute{false};
        std::atomic<uint64_t> largeBytes{0}, largeChecksum{0}, cumulativeChecksum{0};
    };
    auto state = std::make_shared<State>();
    owner->dependencies = state;
    auto handler = [state,
                    receiver,
                    during,
                    typed = (scenario == "identity" || scenario == "same-range" || scenario == "prep-expired" ||
                             scenario == "route-key" || scenario == "large-chunk")](tl::request const& request,
                                                                                    tl::bulk& source,
                                                                                    chl::ServiceId const& reporter)
    {
        unsigned receipt = ++state->started;
        if(reporter.getProviderId() == 27)
            state->sawReporter = true;
        if(!during)
            while(!state->released) margo_thread_sleep(receiver->get_margo_instance(), 1);
        std::vector<char> bytes(source.size());
        auto local = receiver->expose({{bytes.data(), bytes.size()}}, tl::bulk_mode::write_only);
        if(during)
        {
            auto endpoint = request.get_endpoint();
            margo_request native = nullptr;
            if(margo_bulk_itransfer(receiver->get_margo_instance(),
                                    HG_BULK_PULL,
                                    endpoint.get_addr(),
                                    source.get_bulk(),
                                    0,
                                    local.get_bulk(),
                                    0,
                                    bytes.size(),
                                    &native) != HG_SUCCESS)
                return;
            int done = 1;
            margo_test(native, &done);
            state->pending = !done;
            while(!state->released) margo_thread_sleep(receiver->get_margo_instance(), 1);
            if(margo_wait(native) != HG_SUCCESS)
                return;
        }
        else
            source.on(request.get_endpoint()) >> local;
        if(typed)
        {
            chl::StoryChunk decoded;
            if(chl::rpc::deserializeBulk(bytes.data(), bytes.size(), decoded))
                for(auto const& event: decoded)
                {
                    if(event.second.logRecord == "DIFFERENT")
                        state->sawDifferent = true;
                    if(event.second.logRecord.size() > 64 * 1024 * 1024)
                    {
                        state->largeBytes = event.second.logRecord.size();
                        state->largeChecksum =
                                std::accumulate(event.second.logRecord.begin(),
                                                event.second.logRecord.end(),
                                                uint64_t{0},
                                                [](uint64_t sum, unsigned char value) { return sum + value; });
                    }
                }
        }
        state->checksum = std::accumulate(bytes.begin(),
                                          bytes.end(),
                                          uint64_t{0},
                                          [](uint64_t sum, unsigned char value) { return sum + value; });
        state->cumulativeChecksum.fetch_add(state->checksum);
        chl::ChunkReceipt reply;
        reply.bytes = bytes.size();
        reply.grapher_instance = 17;
        reply.receipt = receipt;
        request.respond(reply);
        ++state->completed;
    };
    auto rpc = receiver->define("receive_story_chunk", handler, uint16_t{42});
    auto other = receiver->define(
            "receive_story_chunk",
            [handler, state](tl::request const& request, tl::bulk& source, chl::ServiceId const& reporter)
            {
                state->sawRoute = true;
                handler(request, source, reporter);
            },
            uint16_t{43});
    std::string self = receiver->self();
    auto port = static_cast<uint16_t>(std::stoul(self.substr(self.rfind(':') + 1)));
    chl::ServiceId route(argv[1], "127.0.0.1", port, 42);
    auto agent = std::unique_ptr<chl::RDMATransferAgent>(chl::RDMATransferAgent::CreateRDMATransferAgent(owner, route));
    require(bool(agent), "owned sender factory failed");
    if(scenario == "empty")
    {
        chl::ChunkReceipt answer;
        answer.bytes = 77;
        int status = agent->transfer_serialized_story_chunk({}, {}, &answer);
        require(status == chl::CL_ERR_INVALID_ARG && answer.bytes == 77 && state->started == 0,
                "empty input accepted a failure byte count");
        std::cout << "PASS empty source rejected before exposure" << std::endl;
        return 0;
    }
    if(scenario == "queued")
    {
        auto token = runtime->reserve(chl::rpc::RpcDeadline::after(20ms));
        auto permit = owner->sources.acquire();
        auto outcome = std::make_shared<chl::rpc::BulkReplyState<chl::ChunkReceipt>>();
        auto work = std::make_shared<chl::rpc::BulkSendWork<chl::ChunkReceipt>>(
                owner,
                *sender,
                owner->receiveChunk,
                [owner](auto const& deadline) { return owner->provision("receive_story_chunk", 42, deadline); },
                route,
                std::string(1024, 'Z'),
                std::move(*permit),
                chl::ServiceId{},
                outcome);
        std::this_thread::sleep_for(30ms);
        auto result = runtime->invoke<chl::ChunkReceipt>(std::move(token), std::move(work));
        auto until = Clock::now() + 2s;
        while((!outcome->known || owner->sources.used() != 0) && Clock::now() < until) std::this_thread::sleep_for(1ms);
        require(!result.may_have_dispatched && outcome->known && state->started == 0 && owner->sources.used() == 0,
                "queued cleanup left a false unknown or source permit");
        std::cout << "PASS queued known no-dispatch cleanup status=" << outcome->status << std::endl;
        return 0;
    }
    if(scenario == "prep-expired")
        prepareHook(
                *owner,
                [state]
                {
                    while(!state->released) std::this_thread::sleep_for(1ms);
                },
                0);
    auto delay = (scenario == "default" || scenario == "large-chunk") ? 5200ms : (during ? 800ms : 350ms);
    std::thread releaseThread(
            [state, delay]
            {
                std::this_thread::sleep_for(delay);
                state->released = true;
            });
    chl::StoryChunk a("chron", "story", 19, 100, 200), b("chron", "story", 19, 300, 400);
    a.insertEvent(
            chl::LogEvent(19,
                          150,
                          1,
                          0,
                          scenario == "large-chunk" ? std::string(64 * 1024 * 1024 + 17, 'Z') : std::string("A")));
    b.insertEvent(chl::LogEvent(19, 350, 1, 0, "B"));
    size_t size = during ? 64 * 1024 * 1024 + 17 : 1024 * 1024;
    std::string bytes((scenario == "identity" || scenario == "same-range" || scenario == "prep-expired" ||
                       scenario == "route-key" || scenario == "large-chunk")
                              ? 0
                              : size,
                      'Z');
    chl::ChunkReceipt receipt;
    auto before = Clock::now();
    int result = (scenario == "identity" || scenario == "same-range" || scenario == "prep-expired" ||
                  scenario == "route-key" || scenario == "large-chunk")
                         ? agent->transfer_story_chunk(a, {}, &receipt)
                         : agent->transfer_serialized_story_chunk(bytes, {}, &receipt);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - before).count();
    std::cout << "caller_ms=" << elapsed << " status=" << result << " receipt=" << receipt.bytes
              << " source_bytes=" << bytes.size() << std::endl;
    bytes.assign(bytes.size(), 'X');
    bytes.clear();
    bytes.shrink_to_fit();
    chl::StoryChunk different("chron", "story", 19, 100, 200);
    different.insertEvent(chl::LogEvent(19, 150, 1, 0, "DIFFERENT"));
    int differentFirst = 0;
    if(scenario == "same-range")
        differentFirst = agent->transfer_story_chunk(different);
    if(scenario == "capacity")
    {
        std::string secondBytes(size, 'Z');
        differentFirst = agent->transfer_serialized_story_chunk(secondBytes);
        long maximum = 0;
        unsigned rejected = 0;
        for(unsigned i = 0; i < 32; ++i)
        {
            auto start = Clock::now();
            int status = agent->transfer_serialized_story_chunk(secondBytes);
            maximum = std::max(maximum,
                               std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
            if(status == chl::CL_ERR_RPC_CAPACITY)
                ++rejected;
        }
        std::cout << "source_slots=" << owner->sources.used() << " rejected=" << rejected
                  << " rejection_max_ms=" << maximum << std::endl;
        require(result == chl::CL_ERR_OUTCOME_UNCONFIRMED && differentFirst == chl::CL_ERR_OUTCOME_UNCONFIRMED &&
                        rejected == 32 && owner->sources.used() == 2 && maximum < 100,
                "source capacity did not bound timed-out transfers");
    }
    releaseThread.join();
    auto until = Clock::now() + 5s;
    for(;;)
    {
        bool settled = owner->sources.used() == 0;
        if(scenario == "identity" || scenario == "same-range" || scenario == "route-key" || scenario == "large-chunk")
        {
            settled = true;
            std::lock_guard<std::timed_mutex> lock(owner->pendingMutex);
            for(auto const& entry: owner->chunks) settled = settled && entry.second->state->known.load();
        }
        if(settled || Clock::now() >= until)
            break;
        std::this_thread::sleep_for(1ms);
    }
    if(scenario == "identity")
    {
        chl::ChunkReceipt answerB, answerA;
        int statusB = agent->transfer_story_chunk(b, {}, &answerB);
        int statusA = agent->transfer_story_chunk(a, {}, &answerA);
        std::cout << "A_first=" << result << " B=" << statusB << ":" << answerB.receipt << " A_late=" << statusA << ":"
                  << answerA.receipt << " forwards=" << state->started << std::endl;
        require(result == chl::CL_ERR_OUTCOME_UNCONFIRMED && statusB == chl::CL_SUCCESS && statusA == chl::CL_SUCCESS &&
                        answerA.receipt == 1 && answerB.receipt == 2 && state->started == 2,
                "interleaved chunk lost its original late receipt");
    }
    else if(scenario == "same-range")
    {
        chl::ChunkReceipt answerDifferent, answerOriginal;
        int changed = agent->transfer_story_chunk(different, {}, &answerDifferent);
        int original = agent->transfer_story_chunk(a, {}, &answerOriginal);
        std::cout << "different_first=" << differentFirst << " different=" << changed << ":" << answerDifferent.receipt
                  << " original=" << original << ":" << answerOriginal.receipt << " saw_payload=" << state->sawDifferent
                  << std::endl;
        require(differentFirst != chl::CL_SUCCESS && changed == chl::CL_SUCCESS && original == chl::CL_SUCCESS &&
                        answerDifferent.receipt == 2 && answerOriginal.receipt == 1 && state->started == 2 &&
                        state->sawDifferent,
                "same-range different payload consumed another chunk receipt");
    }
    else if(scenario == "route-key")
    {
        chl::ChunkReceipt reporterReply, routeReply, originalReply;
        int reporterStatus =
                agent->transfer_story_chunk(a, chl::ServiceId(argv[1], "127.0.0.1", 12347, 27), &reporterReply);
        auto routeAgent = std::unique_ptr<chl::RDMATransferAgent>(
                chl::RDMATransferAgent::CreateRDMATransferAgent(owner, chl::ServiceId(argv[1], "127.0.0.1", port, 43)));
        int routeStatus = chl::CL_ERR_RPC_CAPACITY;
        auto ready = Clock::now() + 1s;
        do {
            routeStatus = routeAgent->transfer_story_chunk(a, {}, &routeReply);
            if(routeStatus == chl::CL_ERR_RPC_CAPACITY)
                std::this_thread::sleep_for(1ms);
        } while(routeStatus == chl::CL_ERR_RPC_CAPACITY && Clock::now() < ready);
        int original = agent->transfer_story_chunk(a, {}, &originalReply);
        require(reporterStatus == chl::CL_SUCCESS && routeStatus == chl::CL_SUCCESS && original == chl::CL_SUCCESS &&
                        reporterReply.receipt == 2 && routeReply.receipt == 3 && originalReply.receipt == 1 &&
                        state->sawReporter && state->sawRoute && state->started == 3,
                "route/reporter consumed another attempt receipt");
    }
    else if(scenario == "large-chunk")
    {
        chl::ChunkReceipt known;
        int status = agent->transfer_story_chunk(a, {}, &known);
        std::cout << "payload_bytes=" << state->largeBytes << " encoded_bytes=" << known.bytes
                  << " payload_checksum=" << state->largeChecksum << std::endl;
        require(result == chl::CL_ERR_OUTCOME_UNCONFIRMED && status == chl::CL_SUCCESS && known.receipt == 1 &&
                        known.bytes > 64 * 1024 * 1024 && state->largeBytes == 64 * 1024 * 1024 + 17 &&
                        state->largeChecksum == uint64_t(64 * 1024 * 1024 + 17) * 'Z' && state->pending &&
                        state->started == 1,
                "large legal chunk bytes/identity were not retained across actual pull");
    }
    else if(scenario == "capacity")
    {
        require(state->completed == 2 && state->cumulativeChecksum == 2 * uint64_t(size) * 'Z',
                "capacity case lost an admitted source");
    }
    else if(scenario == "prep-expired")
    {
        prepareHook(*owner, {}, 0);
        require(result == chl::CL_ERR_DEADLINE_EXCEEDED && state->started == 0,
                "expired owned preparation dispatched or stayed unknown");
        require(agent->transfer_story_chunk(a) == chl::CL_SUCCESS,
                "known pre-dispatch failure prevented a fresh attempt");
    }
    else
    {
        auto ceiling = scenario == "default" ? 5300 : (during ? 600 : 250);
        require(result == chl::CL_ERR_OUTCOME_UNCONFIRMED && elapsed < ceiling && receipt.bytes == 0,
                "bulk sender exceeded independent attempt bound or claimed receipt");
        require(state->completed == 1 && state->checksum == uint64_t(size) * 'Z', "late pull saw changed/freed source");
        if(during)
            require(state->pending, "during-pull fixture failed to observe a pending native request");
    }
    std::cout << "PASS " << scenario << " completed=" << state->completed << " native_pending=" << state->pending
              << " checksum=" << state->checksum << std::endl;
    return 0;
}
