#include <chrono_monitor.h>
#include <KeeperRegClient.h>
#include <private/RpcBulkOwner.h>
#include <chrono_monitor.h>
#include <gtest/gtest.h>
#include <future>
#include <thread>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
using namespace std::chrono_literals;
namespace chl = chronolog;
namespace rpc = chronolog::rpc;
namespace
{
struct Peer
{
    struct Shared
    {
        std::atomic<bool> hold{true};
        std::atomic<unsigned> entries{0};
    };
    Shared* shared;
    pid_t pid;
    uint16_t port;
    explicit Peer(std::string const& protocol)
    {
        shared = new(mmap(nullptr, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0)) Shared;
        int ready[2];
        if(pipe(ready) != 0)
            throw std::runtime_error("peer pipe");
        pid = fork();
        if(pid == 0)
        {
            prctl(PR_SET_PDEATHSIG, SIGTERM);
            if(getppid() == 1)
                _exit(2);
            close(ready[0]);
            thallium::engine server(protocol + "://127.0.0.1:0", THALLIUM_SERVER_MODE, true, 1);
            std::string address = server.self();
            auto servicePort = uint16_t(std::stoul(address.substr(address.rfind(':') + 1)));
            auto reg = server.define(
                    "register_keeper",
                    [this, &server](thallium::request const& request, chl::KeeperRegistrationMsg const&)
                    {
                        ++shared->entries;
                        while(shared->hold) margo_thread_sleep(server.get_margo_instance(), 1);
                        request.respond(int(chl::CL_SUCCESS));
                    },
                    uint16_t{25});
            auto stats = server.define(
                    "handle_stats_msg",
                    [](thallium::request const&, chl::KeeperStatsMsg const&) {},
                    uint16_t{25});
            stats.disable_response();
            auto unreg = server.define(
                    "unregister_keeper",
                    [](thallium::request const& request, chl::KeeperIdCard const&)
                    { request.respond(int(chl::CL_SUCCESS)); },
                    uint16_t{25});
            write(ready[1], &servicePort, sizeof(servicePort));
            close(ready[1]);
            for(;;) std::this_thread::sleep_for(1s);
        }
        close(ready[1]);
        if(read(ready[0], &port, sizeof(port)) != sizeof(port))
            throw std::runtime_error("peer startup");
        close(ready[0]);
    }
    ~Peer()
    {
        shared->hold = false;
        // This exact unreaped fork child is a fixture, never a production daemon.
        kill(pid, SIGTERM);
        waitpid(pid, nullptr, 0);
        shared->~Shared();
        munmap(shared, sizeof(Shared));
    }
};
struct Construct: rpc::OwnedRpcWork<int>
{
    std::shared_ptr<rpc::RpcBulkOwner> owner;
    std::string protocol;
    Construct(std::shared_ptr<rpc::RpcBulkOwner> owner, std::string protocol)
        : owner(std::move(owner))
        , protocol(std::move(protocol))
    {}
    rpc::RpcResult<int> run(rpc::OperationContext&) override
    {
        return {chl::CL_SUCCESS, false, owner->initialize({protocol, MARGO_CLIENT_MODE, 1})};
    }
};
class RegistryLifecycle: public ::testing::TestWithParam<char const*>
{
};
TEST_P(RegistryLifecycle, RegisterExpiresWithoutReplacingTheOriginalLiveNativeCall)
{
    chl::chrono_monitor::initialize("console", "", chl::LogLevel::critical, "registry_lifecycle");
    Peer peer(GetParam()); // Fork before this process initializes its actual ABI owner.
    chl::RpcTimeoutPolicy policy;
    policy.attempt_ms = 50;
    auto runtime = std::make_shared<rpc::RpcRuntime>();
    auto process = std::make_shared<rpc::NativeProcessGeneration>();
    auto owner = rpc::RpcBulkOwner::Prepare(runtime, policy, false, std::make_unique<rpc::NativeMidRuntime>(process));
    auto made = runtime->invoke<int>(rpc::RpcDeadline::after(5s), std::make_shared<Construct>(owner, GetParam()));
    ASSERT_TRUE(made.reply);
    ASSERT_EQ(chl::CL_SUCCESS, *made.reply);
    std::string address = std::string(GetParam()) + "://127.0.0.1:" + std::to_string(peer.port);
    auto client = std::unique_ptr<chl::KeeperRegistryClient>(
            chl::KeeperRegistryClient::CreateKeeperRegistryClient(*owner->engine, address, 25));
    ASSERT_TRUE(client);
    chl::KeeperIdCard id(1, chl::ServiceId(GetParam(), "127.0.0.1", 6666, 66));
    auto message = chl::KeeperRegistrationMsg(id, chl::ServiceId(GetParam(), "127.0.0.1", 7777, 77));
    auto call = std::async(std::launch::async, [&] { return client->send_register_msg(message); });
    auto ready = call.wait_for(150ms);
    EXPECT_EQ(std::future_status::ready, ready);
    peer.shared->hold = false; // Release even if the old unbounded path failed.
    EXPECT_EQ(chl::CL_ERR_DEADLINE_EXCEEDED, call.get());
    std::this_thread::sleep_for(50ms);
    EXPECT_EQ(chl::CL_SUCCESS, client->send_register_msg(message));
    EXPECT_EQ(1u, peer.shared->entries.load());
    client.reset();
    owner->retirementReady = true;
    owner.reset();
    EXPECT_EQ(chl::CL_SUCCESS, runtime->drain(rpc::RpcDeadline::after(5s)));
    EXPECT_TRUE(process->cleanupComplete());
    EXPECT_EQ(ABT_ERR_UNINITIALIZED, ABT_initialized());
}
INSTANTIATE_TEST_SUITE_P(Transports, RegistryLifecycle, ::testing::Values("ofi+sockets", "ofi+tcp"));
} // namespace
