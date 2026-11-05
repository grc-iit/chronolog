#include "ClientRpcContext.h"
#include "ClientQueryService.h"
#include "ChronologClientImpl.h"
#include <ConnectResponseMsg.h>
#include <private/RpcBulkSerialization.h>
#include <thallium/serialization/stl/string.hpp>
#include <PlaybackQueryResponse.h>
#include <chrono_monitor.h>
#include <atomic>
#include <functional>
#include <future>
#include <limits>
#include <iostream>
#include <thread>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <csignal>
using namespace std::chrono_literals;
namespace chl = chronolog;
namespace tl = thallium;
namespace chronolog
{
struct ClientRpcContextTestAccess
{
    static void observer(ClientRpcContext& owner, std::function<void(ClientQueryService&, bool)> function)
    {
        owner.readerRegistrationObserver_ = std::move(function);
    }
};
struct ClientQueryServiceTestAccess
{
    static size_t stagedCapacity(ClientQueryService& service)
    {
        std::lock_guard<std::timed_mutex> lock(service.queryServiceMutex);
        return service.activeQueryMap.size() == 1
                       ? service.activeQueryMap.begin()->second->events.capacity()
                       : (service.activeQueryMap.empty() ? 0 : std::numeric_limits<size_t>::max());
    }
};
} // namespace chronolog
// The test's direct reader entry captures one expiry and real leases. Private
// production adapters require that expiry explicitly; no fallback resets it.
template <class Output>
int replay(std::shared_ptr<chl::ClientRpcContext> const& owner,
           Output&& output,
           std::chrono::milliseconds budget = 180s)
{
    auto started = chl::rpc::RpcDeadline::Clock::now();
    auto native = owner->acquireNative();
    auto policy = owner->policy->tryAcquire();
    if(!native || !policy)
        return chl::CL_ERR_RPC_CAPACITY;
    auto deadline = chl::rpc::RpcDeadline::fromStart(started, budget);
    auto retained = std::make_shared<chl::ReplayEntry>(std::move(*native), std::move(*policy));
    return owner->reader
            ->replay_story("chron", "story", 100, 200, std::forward<Output>(output), deadline, std::move(retained));
}
struct Work: chl::rpc::OwnedRpcWork<int>
{
    std::function<int(chl::rpc::OperationContext&)> body;
    explicit Work(std::function<int(chl::rpc::OperationContext&)> body)
        : body(std::move(body))
    {}
    chl::rpc::RpcResult<int> run(chl::rpc::OperationContext& context) override
    {
        try
        {
            return {chl::CL_SUCCESS, false, body(context)};
        }
        catch(...)
        {
            return {chl::CL_ERR_UNKNOWN, false, chl::CL_ERR_UNKNOWN};
        }
    }
};
struct Native
{
    std::shared_ptr<chl::ClientRpcContext> context;
    std::shared_ptr<chl::rpc::RpcRuntime> runtime;
    std::shared_ptr<chl::ChronologClientImpl> facade;
    explicit Native(std::string const& protocol, uint16_t peerPort, bool sdk = false)
    {
        chl::ClientPortalServiceConf portal;
        portal.PROTO_CONF = protocol;
        portal.IP = "127.0.0.1";
        portal.PORT = peerPort;
        portal.PROVIDER_ID = 25;
        chl::ClientQueryServiceConf query{protocol, "127.0.0.1", 23075, 75};
        if(sdk)
        {
            facade = chl::ChronologClientImpl::GetClientImplInstance(portal, chl::READER_MODE, query);
            context = facade->context();
            runtime = facade->runtime();
            return;
        }
        runtime = std::make_shared<chl::rpc::RpcRuntime>();
        context = std::make_shared<chl::ClientRpcContext>(portal, true, query);
        context->retainFacade();
        context->runtime = runtime;
        auto retained = context;
        runtime->retainOwner(retained, [retained] { retained->cleanup(); });
    }
    int initialize()
    {
        if(facade)
            return facade->Connect();
        auto retained = context;
        return runtime
                ->invoke<int>(chl::rpc::RpcDeadline::after(30s),
                              std::make_shared<Work>([retained](auto& operation)
                                                     { return retained->initialize(operation.deadline()); }))
                .reply.value_or(-999);
    }
};

struct Peer
{
    struct Shared
    {
        std::atomic<bool> delivered{false}, allowAck{false};
        std::atomic<unsigned> controls{0};
    };
    Shared* shared;
    pid_t pid;
    uint16_t port = 0;
    Peer(std::string const& protocol, std::string const& scenario)
    {
        shared = new(mmap(nullptr, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0)) Shared;
        int descriptors[2];
        if(pipe(descriptors) != 0)
            throw std::runtime_error("pipe");
        pid = fork();
        if(pid == 0)
        {
            close(descriptors[0]);
            tl::engine engine(protocol + "://127.0.0.1", THALLIUM_SERVER_MODE, true, 1);
            auto connect = engine.define(
                    "Connect",
                    [](tl::request const& request, uint32_t, chl::ClientId id, uint32_t)
                    { request.respond(chl::ConnectResponseMsg(chl::CL_SUCCESS, id)); },
                    uint16_t{25});
            auto control = engine.define(
                    "CreateChronicle",
                    [this](tl::request const& request, chl::ClientId, std::string const&)
                    {
                        ++shared->controls;
                        request.respond(int(chl::CL_SUCCESS));
                    },
                    uint16_t{25});
            auto receive = engine.define("receive_query_response");
            auto send = [&](chl::ServiceId const& receiver, uint32_t query)
            {
                chl::PlaybackQueryResponse response(query);
                unsigned count = scenario == "callback-active" ? 3 : (scenario == "callback-throw" ? 2 : 1);
                for(unsigned i = 0; i < count; ++i)
                    response.events.push_back(chl::Event{150 + i, 1, i, "owned-response"});
                auto payload = chl::rpc::serializeBulk(response);
                auto bulk = engine.expose({{payload.data(), payload.size()}}, tl::bulk_mode::read_only);
                std::string address;
                receiver.get_service_as_string(address);
                auto peer = tl::provider_handle(engine.lookup(address), receiver.getProviderId());
                uint64_t received = receive.on(peer)(bulk);
                shared->delivered = received == payload.size();
                return received;
            };
            auto playback = engine.define(
                    "story_playback_request",
                    [&](tl::request const& request,
                        chl::ServiceId const& receiver,
                        uint32_t query,
                        chl::StoryId,
                        std::string const&,
                        std::string const&,
                        uint64_t,
                        uint64_t)
                    {
                        bool early = scenario == "callback-active";
                        if(early)
                            request.respond(query);
                        if(scenario == "late-private" || scenario == "callback-late")
                            margo_thread_sleep(engine.get_margo_instance(), 1200);
                        send(receiver, query);
                        if(scenario == "staged-expiry")
                            while(!shared->allowAck) margo_thread_sleep(engine.get_margo_instance(), 1);
                        if(!early)
                            request.respond(query);
                    },
                    uint16_t{25});
            auto probe = engine.define(
                    "probe_reader",
                    [&](tl::request const& request, chl::ServiceId const& receiver)
                    { request.respond(send(receiver, 999)); },
                    uint16_t{25});
            std::string address = engine.self();
            address += '\n';
            write(descriptors[1], address.data(), address.size());
            close(descriptors[1]);
            for(;;) pause();
        }
        close(descriptors[1]);
        std::string address;
        char value;
        while(read(descriptors[0], &value, 1) == 1 && value != '\n') address += value;
        close(descriptors[0]);
        if(address.empty())
            throw std::runtime_error("peer startup");
        port = static_cast<uint16_t>(std::stoul(address.substr(address.rfind(':') + 1)));
    }
    ~Peer()
    {
        kill(pid, SIGTERM);
        waitpid(pid, nullptr, 0);
        shared->~Shared();
        munmap(shared, sizeof(Shared));
    }
};
int main(int argc, char** argv)
{
    if(argc != 3)
        return 2;
    std::string protocol = argv[1], scenario = argv[2];
    bool registration = scenario == "register-before" || scenario == "register-after";
    // Fork the cold peer before creating the actual client's workers or ABI
    // owner. The accepted production client permits one active generation.
    auto peer = std::make_unique<Peer>(protocol, scenario);
    Native client(protocol, peer->port, scenario == "callback-control");
    std::atomic<bool> rooted{false}, observed{false};
    if(registration)
    {
        std::weak_ptr<chl::ClientRpcContext> weak = client.context;
        chl::ClientRpcContextTestAccess::observer(
                *client.context,
                [weak, &rooted, &observed, scenario](chl::ClientQueryService& provider, bool after)
                {
                    auto owner = weak.lock();
                    rooted = owner && owner->reader == &provider;
                    observed = true;
                    if(after == (scenario == "register-after"))
                        throw std::runtime_error("injected registration failure");
                });
    }
    int initialized = client.initialize();
    if(registration)
    {
        auto* retained = client.context->reader;
        int retry = client.initialize();
        bool success = initialized == chl::CL_ERR_UNKNOWN && retry == chl::CL_ERR_UNKNOWN && observed && rooted &&
                       retained && retained == client.context->reader;
        std::cout << "init=" << initialized << " retry=" << retry << " observed=" << observed << " root=" << rooted
                  << " reader=" << (retained != nullptr) << std::endl;
        if(!success)
            return 1;
        if(scenario == "register-after")
        {
            auto actual = client.context;
            auto port = peer->port;
            auto result = client.runtime->invoke<int>(
                    chl::rpc::RpcDeadline::after(5s),
                    std::make_shared<Work>(
                            [actual, port, protocol](auto&)
                            {
                                auto probe = actual->engine->define("probe_reader");
                                auto destination = tl::provider_handle(
                                        actual->engine->lookup(protocol + "://127.0.0.1:" + std::to_string(port)),
                                        25);
                                uint64_t bytes = probe.on(destination)(actual->reader->get_service_id());
                                return static_cast<int>(bytes);
                            }));
            if(result.reply.value_or(-1) != 0)
                return 6;
            std::cout << "partial callback answered existing scalar0" << std::endl;
        }
        std::cout << "PASS actual reader rooted through " << scenario << std::endl;
        return 0;
    }
    if(initialized != chl::CL_SUCCESS)
        return 3;
    client.context->reader->addStoryReader("chron", "story", 19, chl::ServiceId(protocol, "127.0.0.1", peer->port, 25));
    auto budget = (scenario == "late-private" || scenario == "staged-expiry" || scenario == "callback-active" ||
                   scenario == "callback-late")
                          ? 1s
                          : 180s;
    if(scenario.rfind("callback-", 0) == 0)
    {
        struct CallbackState
        {
            std::atomic<unsigned> calls{0}, afterReturn{0}, capturesDestroyed{0};
            std::atomic<bool> returned{false}, finished{false}, native{true};
            std::atomic<int> control{123};
        };
        struct CallbackCapture
        {
            std::shared_ptr<CallbackState> state;
            ~CallbackCapture()
            {
                ++state->capturesDestroyed;
                if(state->returned)
                    ++state->afterReturn;
            }
        };
        auto state = std::make_shared<CallbackState>();
        auto actual = client.context;
        auto facade = client.facade;
        auto call = [actual, facade, state, scenario, budget]() -> int
        {
            auto lease = actual->acquireNative();
            if(!lease)
                return chl::CL_ERR_RPC_CAPACITY;
            auto caller = std::this_thread::get_id();
            auto capture = std::make_shared<CallbackCapture>();
            capture->state = state;
            chl::Client::EventCallback callback =
                    [state, facade, caller, scenario, capture = std::move(capture)](chl::Event const&)
            {
                ABT_unit_id native = 0;
                if(ABT_thread_self_id(&native) != ABT_SUCCESS || std::this_thread::get_id() == caller)
                    state->native = false;
                if(state->returned)
                    ++state->afterReturn;
                unsigned call = ++state->calls;
                if(scenario == "callback-active" && call == 1)
                    std::this_thread::sleep_for(1200ms);
                if(scenario == "callback-control")
                    state->control = facade->CreateChronicle("from_callback");
                state->finished = true;
                if(scenario == "callback-throw" && call == 1)
                    throw std::runtime_error("user callback failure");
            };
            int status = replay(actual, std::move(callback), budget);
            state->returned = true;
            return status;
        };
        auto begin = std::chrono::steady_clock::now();
        int status = call();
        auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count();
        auto settled = std::chrono::steady_clock::now() + 2s;
        while(actual->incomingResponses.used() != 0 && std::chrono::steady_clock::now() < settled)
            std::this_thread::sleep_for(1ms);
        bool success = state->native && state->afterReturn == 0 && state->capturesDestroyed == 1 &&
                       actual->incomingResponses.used() == 0;
        if(scenario == "callback-active")
            success = success && status == chl::CL_ERR_QUERY_TIMED_OUT && state->finished && state->calls == 1 &&
                      elapsed >= 1100;
        else if(scenario == "callback-late")
            success = success && status == chl::CL_ERR_QUERY_TIMED_OUT && state->calls == 0;
        else if(scenario == "callback-throw")
        {
            success = success && status == chl::CL_SUCCESS && state->calls == 2;
            auto followupLease = actual->acquireNative();
            std::vector<chl::Event> events;
            success = success && replay(actual, events) == chl::CL_SUCCESS && events.size() == 2;
        }
        else if(scenario == "callback-control")
            success = success && status == chl::CL_SUCCESS && state->control == chl::CL_SUCCESS &&
                      peer->shared->controls == 1;
        else
            success = success && status == chl::CL_SUCCESS && state->calls == 1;
        std::cout << "callback_status=" << status << " native_thread=" << state->native << " calls=" << state->calls
                  << " finished=" << state->finished << " captures_destroyed=" << state->capturesDestroyed
                  << " after_return=" << state->afterReturn << " control=" << state->control
                  << " elapsed_ms=" << elapsed << std::endl;
        if(!success)
            return 1;
        std::cout << "PASS actual client " << scenario << std::endl;
        return 0;
    }
    auto output = std::make_shared<std::vector<chl::Event>>();
    output->push_back(chl::Event{9, 9, 9, "unchanged"});
    auto actualClient = client.context;
    auto call = [actualClient, output, budget]() -> int
    {
        auto lease = actualClient->acquireNative();
        if(!lease)
            return chl::CL_ERR_RPC_CAPACITY;
        return replay(actualClient, *output, budget);
    };
    // The coordinator is an ordinary caller, leaving Runtime2 for native work.
    auto pending = std::async(std::launch::async, std::move(call));
    bool stagingFreed = true;
    if(scenario == "staged-expiry")
    {
        auto until = std::chrono::steady_clock::now() + 2s;
        while((!peer->shared->delivered || actualClient->incomingResponses.used() != 0) &&
              std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(1ms);
        // The ACK is still withheld. B09 can consume a matching response before
        // that ACK; incoming release must leave no private staging allocation.
        size_t capacity = chl::ClientQueryServiceTestAccess::stagedCapacity(*actualClient->reader);
        stagingFreed = peer->shared->delivered && actualClient->incomingResponses.used() == 0 && capacity == 0;
        std::cout << "discarded_staging_capacity=" << capacity << std::endl;
        peer->shared->allowAck = true;
    }
    int status = pending.get();
    bool success;
    if(scenario == "late-private")
        success = status == chl::CL_ERR_QUERY_TIMED_OUT && output->size() == 1 &&
                  output->front().log_record() == "unchanged";
    else
        success = status == chl::CL_SUCCESS && peer->shared->delivered && output->size() == 1 &&
                  output->front().log_record() == "owned-response";
    auto settled = std::chrono::steady_clock::now() + 2s;
    while(actualClient->incomingResponses.used() != 0 && std::chrono::steady_clock::now() < settled)
        std::this_thread::sleep_for(1ms);
    success = success && actualClient->incomingResponses.used() == 0;
    if(scenario == "staged-expiry")
        success = success && peer->shared->delivered && stagingFreed;
    std::cout << "status=" << status << " response_before_ack=" << peer->shared->delivered
              << " output=" << output->front().log_record() << std::endl;
    if(!success)
        return 1;
    std::cout << "PASS actual client " << scenario << std::endl;
    return 0;
}
