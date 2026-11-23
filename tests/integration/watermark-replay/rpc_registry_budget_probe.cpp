// Actual default-budget RegistrySession on one original admin MID and C2.
#include <KeeperRegClient.h>
#include <private/RegistrySession.h>
#include <private/ServiceLifecycle.h>
#include <chrono_monitor.h>
#include <json-c/json.h>
#include <iostream>
#include <optional>
#include <functional>
#include <unistd.h>
namespace rpc = chronolog::rpc;
namespace chl = chronolog;
using namespace std::chrono_literals;
namespace chronolog::rpc
{
struct RegistrySessionTestAccess
{
    static auto pending(std::shared_ptr<RegistrySession> const& value, RegistrySession::Action action)
    {
        std::lock_guard<std::timed_mutex> lock(value->mutex_);
        return value->pending_[action];
    }
    static auto hold(std::shared_ptr<RegistrySession> const& value)
    {
        return std::unique_lock<std::timed_mutex>(value->mutex_);
    }
};
} // namespace chronolog::rpc
namespace
{
using Clock = rpc::RpcDeadline::Clock;
long elapsed(Clock::time_point start)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}
struct Action: rpc::OwnedRpcWork<int>
{
    std::function<int()> action;
    explicit Action(std::function<int()> action)
        : action(std::move(action))
    {}
    rpc::RpcResult<int> run(rpc::OperationContext&) override
    {
        int status = action();
        return {status, false, status};
    }
};
void output(json_object* value)
{
    std::cout << "RESULT " << json_object_to_json_string_ext(value, JSON_C_TO_STRING_PLAIN) << std::endl;
    json_object_put(value);
}
void integer(json_object* value, char const* key, int64_t number)
{
    json_object_object_add(value, key, json_object_new_int64(number));
}
void boolean(json_object* value, char const* key, bool flag)
{
    json_object_object_add(value, key, json_object_new_boolean(flag));
}
} // namespace
int main(int argc, char** argv)
{
    auto startupEntered = Clock::now();
    if(argc != 4)
        return 2;
    chl::chrono_monitor::initialize("console", "", chl::LogLevel::critical, "registry_budget_probe");
    std::string protocol = argv[1], address = argv[2], mode = argv[3];
    chl::RpcTimeoutPolicy policy; // Actual shipped 5000/30000 defaults; never shortened.
    auto startup = rpc::RpcDeadline::fromStart(startupEntered, std::chrono::milliseconds(policy.startup_ms));
    auto runtime = std::make_shared<rpc::RpcRuntime>(2);
    auto process = std::make_shared<rpc::NativeProcessGeneration>(1);
    auto owner = rpc::RpcBulkOwner::Prepare(runtime, policy, false, std::make_unique<rpc::NativeMidRuntime>(process));
    std::weak_ptr<rpc::RpcBulkOwner> weak = owner;
    auto made = runtime->invoke<int>(
            startup,
            std::make_shared<Action>(
                    [&] { return owner->initialize({protocol + "://127.0.0.1:0", MARGO_SERVER_MODE, 1}); }));
    if(!made.reply || *made.reply != chl::CL_SUCCESS)
        return 3;
    auto originalMid = reinterpret_cast<uintptr_t>(owner->engine->get_margo_instance());
    auto session = rpc::RegistrySession::Create(owner, "keeper", address, 88, startup);
    if(!session)
        return 4;
    std::string ownAddress = owner->engine->self();
    auto port = static_cast<uint16_t>(std::stoul(ownAddress.substr(ownAddress.rfind(':') + 1)));
    chl::ServiceId endpoint(protocol, "127.0.0.1", port, 77);
    chl::KeeperIdCard id(7, endpoint);
    chl::KeeperRegistrationMsg message(id, endpoint);
    std::optional<rpc::RpcDeadline> stop;
    std::shared_ptr<rpc::RegistrySession::Completion> originalRegister, originalUnregister;
    auto call = [&](rpc::RegistrySession::Action action, rpc::RpcDeadline const& total)
    {
        auto entry = Clock::now();
        auto attempt = rpc::RpcDeadline::fromStart(entry, std::chrono::milliseconds(policy.attempt_ms));
        if(total.expiresAt() < attempt.expiresAt())
            attempt = total;
        int status;
        do {
            status = action == rpc::RegistrySession::Register ? session->send(action, message, attempt)
                                                              : session->send(action, id, attempt);
            if(status == chl::CL_ERR_RPC_CAPACITY)
                std::this_thread::sleep_for(1ms);
        } while(status == chl::CL_ERR_RPC_CAPACITY && !attempt.expired());
        auto pending = rpc::RegistrySessionTestAccess::pending(session, action);
        auto value = json_object_new_object();
        integer(value, "action", action);
        integer(value, "status", status);
        integer(value, "elapsed_ms", elapsed(entry));
        integer(value, "completion", reinterpret_cast<uintptr_t>(pending.get()));
        integer(value, "mid", originalMid);
        boolean(value, "done", pending && pending->done.load(std::memory_order_acquire));
        if(pending && pending->done.load(std::memory_order_acquire))
        {
            boolean(value, "known", pending->known);
            boolean(value, "dispatched", pending->dispatched);
        }
        output(value);
        return status;
    };
    if(mode == "startup")
    {
        while(!startup.expired())
        {
            auto status = call(rpc::RegistrySession::Register, startup);
            if(!originalRegister)
                originalRegister = rpc::RegistrySessionTestAccess::pending(session, rpc::RegistrySession::Register);
            if(status == chl::CL_SUCCESS)
                break;
            std::this_thread::sleep_for(1ms);
        }
        auto value = json_object_new_object();
        integer(value, "startup_elapsed_ms", elapsed(startupEntered));
        integer(value, "attempt_ms", policy.attempt_ms);
        integer(value, "startup_ms", policy.startup_ms);
        output(value);
    }
    else if(call(rpc::RegistrySession::Register, startup) != chl::CL_SUCCESS)
        return 5;
    std::optional<std::unique_lock<std::timed_mutex>> held;
    std::optional<rpc::RpcPending<int>> locked;
    std::optional<rpc::RpcDeadline> lockedDeadline;
    auto lockedResult = std::make_shared<std::atomic<int>>(12345);
    auto lockedElapsed = std::make_shared<std::atomic<long>>(-1);
    auto snapshot = [&]
    {
        auto value = json_object_new_object();
        auto proof = process->snapshot();
        integer(value, "mid", originalMid);
        integer(value, "generation", proof.generation);
        integer(value, "initialized", proof.initializedMids);
        integer(value, "finalized", proof.finalizedMids);
        boolean(value, "complete", proof.complete);
        boolean(value, "abt_uninitialized", proof.abtUninitialized);
        boolean(value, "owner_retained", !weak.expired());
        integer(value, "primary_releases", proof.primaryReleases);
        unsigned available = 0;
        if(!proof.complete)
        {
            std::vector<rpc::RpcReservation> observed;
            while(observed.size() < 2)
            {
                auto token = runtime->reserve(rpc::RpcDeadline::after(100ms));
                if(!token)
                    break;
                observed.push_back(std::move(token));
            }
            available = observed.size();
        }
        integer(value, "available_C2", available);
        for(auto action: {rpc::RegistrySession::Register, rpc::RegistrySession::Unregister})
        {
            auto pending = rpc::RegistrySessionTestAccess::pending(session, action);
            auto part = json_object_new_object();
            integer(part, "completion", reinterpret_cast<uintptr_t>(pending.get()));
            boolean(part, "done", pending && pending->done.load(std::memory_order_acquire));
            if(pending && pending->done.load(std::memory_order_acquire))
                boolean(part, "known", pending->known);
            json_object_object_add(value, action == rpc::RegistrySession::Register ? "register" : "unregister", part);
        }
        output(value);
    };
    std::cout << "READY" << std::endl;
    for(std::string command; std::getline(std::cin, command);)
    {
        if(command == "QUIT")
            break; // Unknown test process exit is never native retirement.
        if(command == "SNAP")
            snapshot();
        else if(command == "REGISTER")
            call(rpc::RegistrySession::Register, rpc::RpcDeadline::after(5s));
        else if(command == "EXPIRED")
            call(rpc::RegistrySession::Register, startup);
        else if(command == "UNREGISTER")
        {
            if(!stop)
                stop = rpc::RpcDeadline::after(30s);
            call(rpc::RegistrySession::Unregister, *stop);
            if(!originalUnregister)
                originalUnregister = rpc::RegistrySessionTestAccess::pending(session, rpc::RegistrySession::Unregister);
        }
        else if(command == "UNREGISTER_STOP")
        {
            stop = rpc::RpcDeadline::after(30s);
            auto entry = Clock::now();
            while(!stop->expired())
            {
                auto status = call(rpc::RegistrySession::Unregister, *stop);
                if(status == chl::CL_SUCCESS)
                    break;
                std::this_thread::sleep_for(1ms);
            }
            auto value = json_object_new_object();
            integer(value, "stop_elapsed_ms", elapsed(entry));
            output(value);
        }
        else if(command == "LOCK")
        {
            held.emplace(rpc::RegistrySessionTestAccess::hold(session));
            lockedDeadline = rpc::RpcDeadline::after(5s);
            auto entry = Clock::now();
            locked.emplace(runtime->submit<int>(
                    runtime->reserve(*lockedDeadline),
                    std::make_shared<Action>(
                            [&, entry]
                            {
                                int result = session->send(rpc::RegistrySession::Unregister, id, *lockedDeadline);
                                *lockedResult = result;
                                *lockedElapsed = elapsed(entry);
                                return result;
                            })));
            std::cout << "LOCKED" << std::endl;
        }
        else if(command == "UNLOCK")
        {
            held.reset();
            auto result = locked->wait();
            auto observe = Clock::now() + 1500ms;
            while(lockedElapsed->load() < 0 && Clock::now() < observe) std::this_thread::sleep_for(1ms);
            auto value = json_object_new_object();
            integer(value, "status", result.status);
            integer(value, "locked_status", lockedResult->load());
            integer(value, "locked_elapsed_ms", lockedElapsed->load());
            output(value);
        }
        else if(command == "DROP")
        {
            owner.reset();
            snapshot();
        }
        else if(command == "DRAIN")
        {
            if(auto value = weak.lock())
                value->retirementReady = true;
            auto entry = Clock::now();
            int status = runtime->drain(stop ? *stop : rpc::RpcDeadline::after(30s));
            auto value = json_object_new_object();
            integer(value, "drain_status", status);
            integer(value, "drain_elapsed_ms", elapsed(entry));
            output(value);
            snapshot();
        }
        else if(command == "CLEAN")
        {
            if(auto value = weak.lock())
                value->retirementReady = true;
            auto entry = Clock::now();
            int status = runtime->drain(rpc::RpcDeadline::after(30s));
            auto value = json_object_new_object();
            integer(value, "clean_status", status);
            integer(value, "elapsed_ms", elapsed(entry));
            output(value);
            snapshot();
        }
        else
            return 6;
    }
    return 0;
}
