// Every peer here is loopback. Run both transports with FI_TCP_IFACE=lo.
#include "ChronologClientImpl.h"
#include "ClientQueryService.h"
#include <AcquireStoryResponseMsg.h>
#include <ConnectResponseMsg.h>
#include <PlaybackQueryResponse.h>
#include <private/RpcBulkSerialization.h>
#include <thallium/serialization/stl/string.hpp>
#include <thallium/serialization/stl/vector.hpp>
#include <atomic>
#include <future>
#include <iostream>
#include <limits>
#include <optional>
#include <mutex>
#include <thread>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <csignal>
using namespace std::chrono_literals;
namespace chl = chronolog;
namespace tl = thallium;
using Clock = std::chrono::steady_clock;
namespace chronolog
{
struct ClientQueryServiceTestAccess
{
    static void lock(ClientQueryService& reader) { reader.queryServiceMutex.lock(); }
    static void unlock(ClientQueryService& reader) { reader.queryServiceMutex.unlock(); }
    static void lastId(ClientQueryService& reader) { reader.queryIndex = std::numeric_limits<int32_t>::max() - 1; }
    static std::weak_ptr<PlaybackQuery> firstQuery(ClientQueryService& reader)
    {
        std::lock_guard<std::timed_mutex> lock(reader.queryServiceMutex);
        return reader.activeQueryMap.empty() ? std::weak_ptr<PlaybackQuery>{} : reader.activeQueryMap.begin()->second;
    }
};
} // namespace chronolog
namespace
{
bool until(std::function<bool()> condition, std::chrono::milliseconds wait = 3000ms)
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
struct Peer
{
    struct Shared
    {
        std::atomic<unsigned> submissions{0}, acks{0}, deliveries{0}, responseAttempts{0}, controls{0}, records{0},
                lastQuery{0};
        std::atomic<bool> holdAck{false}, holdResponse{false}, ackFirst{true}, sendResponse{true}, badAck{false},
                negativeAck{false}, duplicate{false}, duplicateDone{false};
        std::atomic<uint64_t> duplicateReceipt{std::numeric_limits<uint64_t>::max()};
        std::atomic<int> ackDelayMs{0}, responseDelayMs{0}, events{1};
    };
    Shared* shared;
    pid_t pid;
    uint16_t port;
    explicit Peer(std::string const& protocol)
    {
        auto* mapping = mmap(nullptr, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if(mapping == MAP_FAILED)
            throw std::runtime_error("shared peer mapping");
        shared = new(mapping) Shared;
        int ready[2];
        if(pipe(ready) != 0)
            throw std::runtime_error("ready pipe");
        pid = fork();
        if(pid == 0)
        {
            prctl(PR_SET_PDEATHSIG, SIGTERM);
            if(getppid() == 1)
                _exit(2);
            close(ready[0]);
            // This actual owning engine is constructed in a cold child before
            // any ChronoLog owner/worker is created in the parent.
            tl::engine engine(protocol + "://127.0.0.1:0", THALLIUM_SERVER_MODE, true, 1);
            auto address = static_cast<std::string>(engine.self());
            auto servicePort = static_cast<uint16_t>(std::stoul(address.substr(address.rfind(':') + 1)));
            chl::ServiceId service(protocol, "127.0.0.1", servicePort, 25);
            std::vector<tl::remote_procedure> procedures;
            procedures.push_back(engine.define(
                    "Connect",
                    [](tl::request const& request, uint32_t, chl::ClientId id, uint32_t)
                    { request.respond(chl::ConnectResponseMsg(chl::CL_SUCCESS, id)); },
                    uint16_t{25}));
            procedures.push_back(engine.define(
                    "CreateChronicle",
                    [this](tl::request const& request, chl::ClientId, std::string const&)
                    {
                        ++shared->controls;
                        request.respond(int(chl::CL_SUCCESS));
                    },
                    uint16_t{25}));
            procedures.push_back(engine.define(
                    "AcquireStory",
                    [service](tl::request const& request, chl::ClientId, std::string const&, std::string const&)
                    { request.respond(chl::AcquireStoryResponseMsg(chl::CL_SUCCESS, 19, {service}, service)); },
                    uint16_t{25}));
            procedures.push_back(engine.define(
                    "ReleaseStory",
                    [](tl::request const& request, chl::ClientId, std::string const&, std::string const&)
                    { request.respond(int(chl::CL_SUCCESS)); },
                    uint16_t{25}));
            procedures.push_back(engine.define(
                    "record_event",
                    [this](tl::request const& request, chl::LogEvent const&)
                    {
                        ++shared->records;
                        request.respond(int(chl::CL_SUCCESS));
                    },
                    uint16_t{25}));
            std::mutex lastMutex;
            std::optional<std::pair<chl::ServiceId, uint32_t>> last;
            auto receive = engine.define("receive_query_response");
            auto send = [&](chl::ServiceId const& receiver, uint32_t id)
            {
                chl::PlaybackQueryResponse response(id);
                for(int i = 0; i < shared->events; ++i) response.events.emplace_back(150 + i, 1, i, "response");
                auto bytes = chl::rpc::serializeBulk(response);
                auto bulk = engine.expose({{bytes.data(), bytes.size()}}, tl::bulk_mode::read_only);
                std::string destination;
                receiver.get_service_as_string(destination);
                auto target = tl::provider_handle(engine.lookup(destination), receiver.getProviderId());
                ++shared->responseAttempts;
                uint64_t receipt = receive.on(target)(bulk);
                ++shared->deliveries;
                return receipt;
            };
            procedures.push_back(engine.define(
                    "story_playback_request",
                    [&](tl::request const& request,
                        chl::ServiceId const& receiver,
                        uint32_t id,
                        chl::StoryId,
                        std::string const&,
                        std::string const&,
                        uint64_t,
                        uint64_t)
                    {
                        {
                            std::lock_guard<std::mutex> lock(lastMutex);
                            last.emplace(receiver, id);
                        }
                        ++shared->submissions;
                        shared->lastQuery = id;
                        if(shared->ackDelayMs)
                            margo_thread_sleep(engine.get_margo_instance(), shared->ackDelayMs);
                        while(shared->holdAck) margo_thread_sleep(engine.get_margo_instance(), 1);
                        if(shared->ackFirst)
                        {
                            if(shared->negativeAck)
                                request.respond(int(chl::CL_ERR_RPC_CAPACITY));
                            else
                                request.respond(uint32_t(shared->badAck ? id + 1 : id));
                            ++shared->acks;
                        }
                        if(shared->responseDelayMs)
                            margo_thread_sleep(engine.get_margo_instance(), shared->responseDelayMs);
                        while(shared->holdResponse) margo_thread_sleep(engine.get_margo_instance(), 1);
                        if(shared->sendResponse)
                            send(receiver, id);
                        if(!shared->ackFirst)
                        {
                            request.respond(id);
                            ++shared->acks;
                        }
                    },
                    uint16_t{25}));
            write(ready[1], &servicePort, sizeof(servicePort));
            close(ready[1]);
            for(;;)
            {
                if(shared->duplicate.exchange(false))
                {
                    std::pair<chl::ServiceId, uint32_t> destination;
                    {
                        std::lock_guard<std::mutex> lock(lastMutex);
                        destination = last.value();
                    }
                    // The existing child primary issues the second real source;
                    // no extra native owner or per-call thread is constructed.
                    shared->duplicateReceipt = send(destination.first, destination.second);
                    shared->duplicateDone = true;
                }
                else
                    std::this_thread::sleep_for(1ms);
            }
        }
        close(ready[1]);
        if(read(ready[0], &port, sizeof(port)) != sizeof(port))
            throw std::runtime_error("peer startup");
        close(ready[0]);
    }
    ~Peer()
    {
        kill(pid, SIGTERM);
        waitpid(pid, nullptr, 0);
        // Peer termination is fixture cleanup, not production native settlement.
        shared->~Shared();
        munmap(shared, sizeof(Shared));
    }
};
struct CaptureState
{
    std::atomic<unsigned> calls{0}, destroyed{0}, afterReturn{0};
    std::atomic<bool> active{false}, release{false}, returned{false}, native{true};
    std::atomic<int> nested{123};
};
struct Capture
{
    std::shared_ptr<CaptureState> state;
    ~Capture() { ++state->destroyed; }
};
} // namespace
int main(int argc, char** argv)
{
    if(argc != 3)
        return 2;
    std::string protocol = argv[1], scenario = argv[2];
    Peer peer(protocol);
    chl::ClientPortalServiceConf portal;
    portal.PROTO_CONF = protocol;
    portal.IP = "127.0.0.1";
    portal.PORT = peer.port;
    portal.PROVIDER_ID = 25;
    portal.RPC_POLICY.replay_ms = scenario == "shipped-default"                                ? 180000
                                  : (scenario == "admission" || scenario == "duplicate-owner") ? 500
                                                                                               : 90;
    portal.RPC_POLICY.attempt_ms = scenario == "unknown-capacity" ? 35 : 5000;
    chl::ClientQueryServiceConf query{protocol, "127.0.0.1", 23087, 75};
    auto facade = chl::ChronologClientImpl::GetClientImplInstance(portal, chl::READER_MODE, query);
    auto client = std::make_unique<chl::Client>(portal, query);
    if(client->Connect() != chl::CL_SUCCESS)
        return 3;
    auto acquired = client->AcquireStory("chron", "story");
    if(acquired.first != chl::CL_SUCCESS || !acquired.second)
        return 4;
    auto context = facade->context();
    auto* reader = context->reader;
    bool pass = true;
    int status = 123;
    auto started = Clock::now();
    std::vector<chl::Event> output{{9, 9, 9, "sentinel"}};
    if(scenario == "shipped-default")
    {
        peer.shared->holdAck = true;
        started = Clock::now();
        status = client->ReplayStory("chron", "story", 100, 200, output);
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
        pass = status == chl::CL_ERR_QUERY_TIMED_OUT && elapsed >= 175000 && elapsed <= 185000 &&
               peer.shared->submissions == 1 && peer.shared->acks == 0 && peer.shared->deliveries == 0 &&
               output.size() == 1 && output.front().log_record() == "sentinel";
    }
    else if(scenario == "submission" || scenario == "original" || scenario == "drop-late" ||
            scenario == "drop-output-late")
    {
        peer.shared->ackFirst = false;
        peer.shared->ackDelayMs = scenario == "original" ? 65 : 210;
        auto lateOutput = (scenario == "drop-late" || scenario == "drop-output-late")
                                  ? std::make_unique<std::vector<chl::Event>>(output)
                                  : nullptr;
        auto& target = lateOutput ? *lateOutput : output;
        if(scenario == "original")
            status = facade->replay_story("chron", "story", 100, 200, output, started - 65ms);
        else
            status = client->ReplayStory("chron", "story", 100, 200, target);
        auto elapsed = Clock::now() - started;
        pass = status == chl::CL_ERR_QUERY_TIMED_OUT && elapsed < (scenario == "original" ? 75ms : 160ms) &&
               target.size() == 1 && target.front().log_record() == "sentinel";
        if(scenario == "drop-late" || scenario == "drop-output-late")
        {
            lateOutput.reset(); // Destroy the actual vector object before the late transfer.
            if(scenario == "drop-output-late")
                pass = until([&] { return peer.shared->deliveries == 1; }) && pass;
            client.reset();
            facade.reset();
            auto entryAfterClose = context->acquireNative();
            pass = pass && context->closing() && !entryAfterClose && !context->cleanupComplete;
            pass = until([&] { return peer.shared->responseAttempts == 1; }) && pass;
            pass = pass && context->engine && context->replayQueries.used() == 1;
            // Closed native admission intentionally sends no new response. The
            // child source and its actual engine stay alive on its pending call
            // until fixture cleanup; no receipt is invented as settlement.
        }
        else
        {
            pass = until([&] { return peer.shared->deliveries == 1; }) && pass;
            pass = pass && output.size() == 1 && output.front().log_record() == "sentinel";
        }
    }
    else if(scenario == "map-lock")
    {
        std::promise<void> locked;
        std::thread holder(
                [&]
                {
                    chl::ClientQueryServiceTestAccess::lock(*reader);
                    locked.set_value();
                    std::this_thread::sleep_for(210ms);
                    chl::ClientQueryServiceTestAccess::unlock(*reader);
                });
        locked.get_future().wait();
        started = Clock::now();
        status = client->ReplayStory("chron", "story", 100, 200, output);
        pass = status == chl::CL_ERR_QUERY_TIMED_OUT && Clock::now() - started < 160ms && peer.shared->submissions == 0;
        holder.join();
    }
    else if(scenario == "admission")
    {
        peer.shared->holdResponse = true;
        auto first = std::async(std::launch::async,
                                [&]
                                {
                                    std::vector<chl::Event> result;
                                    return client->ReplayStory("chron", "story", 100, 200, result);
                                });
        pass = until([&] { return peer.shared->acks == 1; });
        auto second = std::async(std::launch::async,
                                 [&]
                                 {
                                     std::vector<chl::Event> result;
                                     return client->ReplayStory("chron", "story", 100, 200, result);
                                 });
        pass = until([&] { return peer.shared->acks == 2; }) && pass;
        pass = pass && context->replayQueries.used() == 2;
        auto release = std::async(std::launch::async,
                                  [&]
                                  {
                                      std::this_thread::sleep_for(210ms);
                                      peer.shared->holdResponse = false;
                                  });
        started = Clock::now();
        status = client->ReplayStory("chron", "story", 100, 200, output);
        pass = pass && status == chl::CL_ERR_RPC_CAPACITY && Clock::now() - started < 90ms &&
               peer.shared->submissions == 2;
        release.get();
        pass = first.get() == chl::CL_SUCCESS && second.get() == chl::CL_SUCCESS && pass;
    }
    else if(scenario == "cleanup-drop")
    {
        // The actual submission has a known ACK. No incoming source exists;
        // only consumed query-map roots keep the native lease charged.
        peer.shared->sendResponse = false;
        auto replay =
                std::async(std::launch::async, [&] { return client->ReplayStory("chron", "story", 100, 200, output); });
        pass = until([&] { return peer.shared->acks == 1; });
        chl::ClientQueryServiceTestAccess::lock(*reader);
        auto ready = replay.wait_for(150ms);
        bool retained = context->replayQueries.used() == 1;
        client.reset();
        facade.reset();
        bool retainedAtClose = !context->cleanupComplete.load();
        chl::ClientQueryServiceTestAccess::unlock(*reader);
        status = replay.get();
        pass = pass && ready == std::future_status::ready && retained && retainedAtClose &&
               status == chl::CL_ERR_QUERY_TIMED_OUT;
        pass = until([&] { return context->cleanupComplete.load(std::memory_order_acquire); }) && pass;
        pass = pass && !context->engine && context->reader == nullptr && context->replayQueries.used() == 0 &&
               ABT_initialized() == ABT_ERR_UNINITIALIZED;
    }
    else if(scenario == "cleanup-lock")
    {
        peer.shared->holdResponse = true;
        auto state = std::make_shared<CaptureState>();
        auto capture = std::make_shared<Capture>();
        capture->state = state;
        auto vectorCall =
                std::async(std::launch::async, [&] { return client->ReplayStory("chron", "story", 100, 200, output); });
        pass = until([&] { return peer.shared->acks == 1; });
        auto callbackCall = std::async(std::launch::async,
                                       [&, state, capture = std::move(capture)]() mutable
                                       {
                                           chl::Client::EventCallback callback =
                                                   [state, capture = std::move(capture)](chl::Event const&)
                                           { ++state->calls; };
                                           return client->ReplayStory("chron", "story", 100, 200, std::move(callback));
                                       });
        pass = until([&] { return peer.shared->acks == 2; }) && pass;
        chl::ClientQueryServiceTestAccess::lock(*reader);
        auto firstReady = vectorCall.wait_for(150ms), secondReady = callbackCall.wait_for(20ms);
        // Release before any failed-test get: the watchdog is not the evidence.
        bool retained = context->replayQueries.used() == 2;
        chl::ClientQueryServiceTestAccess::unlock(*reader);
        status = vectorCall.get();
        int callbackStatus = callbackCall.get();
        pass = pass && firstReady == std::future_status::ready && secondReady == std::future_status::ready &&
               status == chl::CL_ERR_QUERY_TIMED_OUT && callbackStatus == chl::CL_ERR_QUERY_TIMED_OUT && retained &&
               state->calls == 0 && state->destroyed == 1 && output.size() == 1 &&
               output.front().log_record() == "sentinel";
        peer.shared->holdResponse = false;
        pass = until([&] { return peer.shared->deliveries == 2 && context->incomingResponses.used() == 0; }) && pass;
        // Pruning precedes query admission, so these inert map roots cannot
        // permanently exhaust capacity after the contended lock is released.
        int recovered = client->ReplayStory("chron", "story", 100, 200, output);
        pass = pass && recovered == chl::CL_SUCCESS && output.size() == 1 && output.front().log_record() == "response";
        pass = until([&] { return context->replayQueries.used() == 0; }) && pass;
        pass = reader->pruneConsumedQueries() && pass;
    }
    else if(scenario == "unknown-capacity")
    {
        peer.shared->ackDelayMs = 210;
        auto firstCall =
                std::async(std::launch::async, [&] { return client->ReplayStory("chron", "story", 100, 200, output); });
        pass = until([&] { return peer.shared->submissions == 1; });
        auto observed = chl::ClientQueryServiceTestAccess::firstQuery(*reader);
        int first = firstCall.get();
        auto firstState = observed.lock();
        bool nativeReady = firstState && firstState->submissionReady.load(std::memory_order_acquire);
        std::cerr << "first=" << first << " query_alive=" << bool(firstState) << " native_ready=" << nativeReady
                  << " native_status=" << (nativeReady ? firstState->submissionStatus : 999)
                  << " queries=" << context->replayQueries.used() << '\n';
        // The independent caller expiry is not native settlement. The actual
        // timed forward may still be running; either state must retain its query.
        pass = pass && firstState && context->replayQueries.used() == 1 &&
               (!nativeReady || firstState->submissionStatus == chl::CL_ERR_OUTCOME_UNCONFIRMED);
        firstState.reset(); // No test root can hide a missing production permit owner.
        int second = client->ReplayStory("chron", "story", 100, 200, output);
        started = Clock::now();
        status = client->ReplayStory("chron", "story", 100, 200, output);
        pass = pass && first == chl::CL_ERR_QUERY_TIMED_OUT && second == chl::CL_ERR_QUERY_TIMED_OUT &&
               context->replayQueries.used() == 2 && status == chl::CL_ERR_RPC_CAPACITY &&
               Clock::now() - started < 60ms && peer.shared->submissions == 2;
        pass = until([&] { return peer.shared->deliveries == 2; }) && pass;
        int afterLate = client->ReplayStory("chron", "story", 100, 200, output);
        std::cerr << "after_late=" << afterLate << " queries=" << context->replayQueries.used()
                  << " query_alive=" << !observed.expired() << '\n';
        pass = pass && afterLate == chl::CL_ERR_RPC_CAPACITY && context->replayQueries.used() == 2 &&
               !observed.expired();
        pass = pass && output.size() == 1 && output.front().log_record() == "sentinel";
    }
    else if(scenario == "last-id")
    {
        chl::ClientQueryServiceTestAccess::lastId(*reader);
        int first = client->ReplayStory("chron", "story", 100, 200, output);
        status = client->ReplayStory("chron", "story", 100, 200, output);
        pass = first == chl::CL_SUCCESS && status == chl::CL_ERR_RPC_CAPACITY && peer.shared->submissions == 1 &&
               peer.shared->lastQuery == std::numeric_limits<int32_t>::max();
    }
    else if(scenario == "response-before-ack")
    {
        peer.shared->ackFirst = false;
        status = client->ReplayStory("chron", "story", 100, 200, output);
        pass = until([&] { return peer.shared->deliveries == 1; }) && status == chl::CL_SUCCESS && output.size() == 1 &&
               output.front().log_record() == "response";
    }
    else if(scenario == "bad-ack" || scenario == "negative-ack")
    {
        peer.shared->badAck = scenario == "bad-ack";
        peer.shared->negativeAck = scenario == "negative-ack";
        if(scenario == "negative-ack")
            chl::ClientQueryServiceTestAccess::lastId(*reader);
        peer.shared->sendResponse = false;
        status = client->ReplayStory("chron", "story", 100, 200, output);
        pass = status == chl::CL_ERR_NO_PLAYERS && Clock::now() - started < 160ms;
    }
    else if(scenario == "duplicate-owner")
    {
        auto state = std::make_shared<CaptureState>();
        auto capture = std::make_shared<Capture>();
        capture->state = state;
        auto first =
                std::async(std::launch::async,
                           [&, state, capture = std::move(capture)]() mutable
                           {
                               auto caller = std::this_thread::get_id();
                               chl::Client::EventCallback callback =
                                       [context, state, caller, capture = std::move(capture)](chl::Event const&)
                               {
                                   ABT_unit_id id = 0;
                                   if(ABT_thread_self_id(&id) != ABT_SUCCESS || std::this_thread::get_id() == caller)
                                       state->native = false;
                                   ++state->calls;
                                   state->active = true;
                                   while(!state->release) margo_thread_sleep(context->engine->get_margo_instance(), 1);
                               };
                               return client->ReplayStory("chron", "story", 100, 200, std::move(callback));
                           });
        pass = until([&] { return state->active.load(); });
        peer.shared->duplicate = true;
        pass = until([&] { return peer.shared->duplicateDone.load(); }) && pass;
        unsigned incoming = context->incomingResponses.used();
        // A fully pulled duplicate is rejected with scalar0. It owns no staged
        // result and must release its incoming permit while the publisher stays
        // in the already-active cooperative callback.
        pass = pass && peer.shared->duplicateReceipt == 0 && incoming == 1 && state->calls == 1;
        state->release = true;
        status = first.get();
        pass = until([&] { return peer.shared->deliveries == 2 && context->incomingResponses.used() == 0; }) && pass;
        pass = pass && status == chl::CL_SUCCESS && state->native && state->calls == 1 && state->destroyed == 1;
        std::cout << "duplicate_receipt=" << peer.shared->duplicateReceipt << " incoming_while_callback=" << incoming
                  << " callbacks=" << state->calls << " captures_destroyed=" << state->destroyed << '\n';
    }
    else if(scenario.rfind("callback-", 0) == 0)
    {
        auto state = std::make_shared<CaptureState>();
        auto capture = std::make_shared<Capture>();
        capture->state = state;
        peer.shared->events = 3;
        if(scenario == "callback-late")
        {
            peer.shared->ackFirst = false;
            peer.shared->ackDelayMs = 210;
        }
        auto caller = std::this_thread::get_id();
        auto work = chl::Client::EventCallback(
                [state, capture = std::move(capture), caller, scenario, facade, handle = acquired.second](
                        chl::Event const&)
                {
                    ABT_unit_id id = 0;
                    if(ABT_thread_self_id(&id) != ABT_SUCCESS || std::this_thread::get_id() == caller)
                        state->native = false;
                    if(state->returned)
                        ++state->afterReturn;
                    auto n = ++state->calls;
                    if(scenario == "callback-active" && n == 1)
                    {
                        state->active = true;
                        while(!state->release) margo_thread_sleep(facade->context()->engine->get_margo_instance(), 1);
                    }
                    if(scenario == "callback-control")
                        state->nested = facade->CreateChronicle("callback");
                    if(scenario == "callback-append")
                        state->nested = handle->append_event("callback").status;
                    if(scenario == "callback-throw" && n == 1)
                        throw std::runtime_error("expected callback failure");
                });
        std::future<void> release;
        if(scenario == "callback-active")
            release = std::async(std::launch::async,
                                 [state]
                                 {
                                     if(until([&] { return state->active.load(); }))
                                         std::this_thread::sleep_for(150ms);
                                     state->release = true;
                                 });
        started = Clock::now();
        status = client->ReplayStory("chron", "story", 100, 200, std::move(work));
        auto elapsed = Clock::now() - started;
        state->returned = true;
        if(release.valid())
            release.get();
        pass = until([&] { return peer.shared->deliveries == 1 && context->incomingResponses.used() == 0; }) &&
               state->native && state->afterReturn == 0 && state->destroyed == 1;
        if(scenario == "callback-active")
            pass = pass && status == chl::CL_ERR_QUERY_TIMED_OUT && state->calls == 1 && elapsed >= 145ms &&
                   elapsed < 240ms;
        else if(scenario == "callback-late")
            pass = pass && status == chl::CL_ERR_QUERY_TIMED_OUT && state->calls == 0 && elapsed < 160ms;
        else
            pass = pass && status == chl::CL_SUCCESS && state->calls == 3;
        if(scenario == "callback-control" || scenario == "callback-append")
            pass = pass && state->nested == chl::CL_SUCCESS;
        std::cout << "callback_calls=" << state->calls << " native=" << state->native
                  << " capture_destroyed=" << state->destroyed << " after_return=" << state->afterReturn
                  << " nested=" << state->nested
                  << " callback_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() << '\n';
        if(scenario == "callback-throw")
            pass = pass && client->ReplayStory("chron", "story", 100, 200, output) == chl::CL_SUCCESS &&
                   output.size() == 3;
    }
    else
        return 2;
    std::cout << (pass ? "PASS " : "FAIL ") << protocol << ' ' << scenario << " status=" << status
              << " submissions=" << peer.shared->submissions << " acks=" << peer.shared->acks
              << " deliveries=" << peer.shared->deliveries
              << " elapsed_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count()
              << std::endl;
    return pass ? 0 : 1;
}
