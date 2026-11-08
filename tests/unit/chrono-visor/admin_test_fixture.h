#ifndef CHRONOLOG_ADMIN_TEST_FIXTURE_H
#define CHRONOLOG_ADMIN_TEST_FIXTURE_H
#include <gtest/gtest.h>
#include <KeeperRegistry.h>
#include <KeeperRegistryService.h>
#include <VisorClientPortal.h>
#include <DataStoreAdminClient.h>
#include <chrono_monitor.h>
#include <chronolog_types.h>
#include <thallium/serialization/stl/string.hpp>
#include <thallium/serialization/stl/vector.hpp>
#include <atomic>
#include <chrono>
#include <thread>

namespace tl = thallium;
using namespace chronolog;
using namespace std::chrono_literals;

namespace chronolog
{
struct KeeperRegistryTestAccess
{
    static std::unique_lock<std::mutex> lock(KeeperRegistry& registry)
    {
        return std::unique_lock<std::mutex>(registry.registryLock);
    }
    static std::shared_ptr<KeeperRegistry>
    create(RpcTimeoutPolicy const& p, std::size_t n, std::shared_ptr<AdminLookup> lookup)
    {
        return std::shared_ptr<KeeperRegistry>(new KeeperRegistry(p, n, std::move(lookup)));
    }
    static std::weak_ptr<void> provider(KeeperRegistry& registry)
    {
        return registry.keeperRegistryService->lifetimeToken_;
    }
};
} // namespace chronolog
class AdminDeadline: public ::testing::TestWithParam<std::string>
{
protected:
    std::unique_ptr<tl::engine> peers;
    bool peerFinalized = false;
    std::shared_ptr<KeeperRegistry> registry;
    std::atomic<int> starts[5]{};
    std::atomic<unsigned> completions[5]{};
    std::atomic<unsigned> appendCounts[5]{};
    std::atomic<int> stops[5]{};
    int stopDelays[5]{};
    bool suppressShutdownReply[5]{};
    int shutdownAcks[5]{1, 1, 1, 1, 1};
    std::atomic<unsigned> shutdownEntries[5]{};
    std::atomic<bool> holdShutdown[5]{};
    virtual std::shared_ptr<AdminLookup> lookupDependency() { return {}; }
    std::mutex rosterMutex;
    int delays[5]{0, 300, 300, 0, 0};
    int replies[5]{0, 0, 0, 0, 0};
    std::vector<tl::remote_procedure> procedures;
    std::vector<ServiceId> playerRoster;
    std::string protocol;
    uint16_t port{};
    ServiceId service(int provider) { return ServiceId(protocol, "127.0.0.1", port, provider); }
    virtual ServiceId keeperRecordingService(int i)
    {
        return ServiceId(protocol, "127.0.0.1", static_cast<uint16_t>(30000 + i), static_cast<uint16_t>(10 + i));
    }
    KeeperIdCard keeper(int i) { return KeeperIdCard(1, keeperRecordingService(i)); }
    virtual RpcTimeoutPolicy fixturePolicy() const
    {
        RpcTimeoutPolicy policy;
        policy.control_ms = 120;
        policy.attempt_ms = 120;
        auto name = std::string(::testing::UnitTest::GetInstance()->current_test_info()->name());
        if(name.find("Shutdown") != std::string::npos)
        {
            policy.shutdown_ms = 80;
            policy.attempt_ms = 20;
        }
        return policy;
    }
    virtual std::size_t fixtureCapacity() const
    {
        auto name = std::string(::testing::UnitTest::GetInstance()->current_test_info()->name());
        return name.find("ThreeSlots") != std::string::npos ? 3u
                                                            : (name.find("Saturation") != std::string::npos ? 32u : 8u);
    }
    virtual bool useDefaultAdminCapacity() const { return false; }
    void SetUp() override
    {
        protocol = GetParam();
        chrono_monitor::initialize("console", "", LogLevel::critical, "B07", 1048576, 1, LogLevel::critical);
        peers = std::make_unique<tl::engine>(protocol + "://127.0.0.1:0", THALLIUM_SERVER_MODE, true, 8);
        std::string address = peers->self();
        port = static_cast<uint16_t>(std::stoi(address.substr(address.find_last_of(':') + 1)));
        for(int i = 0; i < 5; ++i)
        {
            auto start = [this, i](tl::request const& req, std::string const&, std::string const&, StoryId, uint64_t)
            {
                ++starts[i];
                margo_thread_sleep(peers->get_margo_instance(), delays[i]);
                ++completions[i];
                try
                {
                    req.respond(replies[i]);
                }
                catch(...)
                {}
            };
            procedures.push_back(peers->define("start_story_recording", start, static_cast<uint16_t>(i + 1)));
            procedures.push_back(peers->define(
                    "start_story_recording_with_keepers",
                    [this, i](tl::request const& req,
                              std::string const&,
                              std::string const&,
                              StoryId,
                              uint64_t,
                              std::vector<ServiceId> const& roster)
                    {
                        ++starts[i];
                        {
                            std::lock_guard<std::mutex> lock(rosterMutex);
                            playerRoster = roster;
                        }
                        margo_thread_sleep(peers->get_margo_instance(), delays[i]);
                        ++completions[i];
                        try
                        {
                            req.respond(replies[i]);
                        }
                        catch(...)
                        {}
                    },
                    static_cast<uint16_t>(i + 1)));
            procedures.push_back(peers->define(
                    "record_event",
                    [this, i](tl::request const& req, LogEvent const&)
                    {
                        ++appendCounts[i];
                        try
                        {
                            req.respond(CL_SUCCESS);
                        }
                        catch(...)
                        {}
                    },
                    static_cast<uint16_t>(10 + i)));
            procedures.push_back(peers->define(
                    "stop_story_recording",
                    [this, i](tl::request const& req, StoryId)
                    {
                        ++stops[i];
                        margo_thread_sleep(peers->get_margo_instance(), stopDelays[i]);
                        try
                        {
                            req.respond(0);
                        }
                        catch(...)
                        {}
                    },
                    static_cast<uint16_t>(i + 1)));
            procedures.push_back(peers->define(
                    "shutdown_data_collection",
                    [this, i](tl::request const& req)
                    {
                        ++shutdownEntries[i];
                        while(holdShutdown[i].load(std::memory_order_acquire))
                            margo_thread_sleep(peers->get_margo_instance(), 1);
                        if(suppressShutdownReply[i])
                            return;
                        try
                        {
                            req.respond(shutdownAcks[i]);
                        }
                        catch(...)
                        {}
                    },
                    static_cast<uint16_t>(i + 1)));
        }
        auto policy = fixturePolicy();
        auto name = std::string(::testing::UnitTest::GetInstance()->current_test_info()->name());
        auto capacity = fixtureCapacity();
        if(auto dependency = lookupDependency())
            registry = KeeperRegistryTestAccess::create(policy, capacity, std::move(dependency));
        else
            registry = useDefaultAdminCapacity() || name.find("Saturation") != std::string::npos
                               ? KeeperRegistry::Create(policy)
                               : KeeperRegistry::Create(policy, capacity);
        RPCProviderConf conf;
        conf.PROTO_CONF = protocol;
        conf.IP = "127.0.0.1";
        conf.BASE_PORT = 0;
        conf.SERVICE_PROVIDER_ID = 30;
        ASSERT_EQ(registry->InitializeRegistryService(conf, 0), CL_SUCCESS);
        ASSERT_EQ(registry->registerGrapherProcess(GrapherRegistrationMsg(GrapherIdCard(1, service(1)), service(1))),
                  CL_SUCCESS);
        ASSERT_EQ(registry->registerPlayerProcess(PlayerRegistrationMsg(PlayerIdCard(1, service(5)), service(5))),
                  CL_SUCCESS);
        for(int i = 1; i <= 3; ++i)
            ASSERT_EQ(registry->registerKeeperProcess(KeeperRegistrationMsg(keeper(i), service(i + 1))), CL_SUCCESS);
    }
    std::vector<rpc::RpcReservation> occupy(std::size_t count, rpc::RpcDeadline const& deadline)
    {
        std::vector<rpc::RpcReservation> tokens;
        while(!deadline.expired())
        {
            while(tokens.size() < count)
            {
                auto token = registry->adminOwner()->runtime.reserve(deadline);
                if(!token)
                    break;
                tokens.push_back(std::move(token));
            }
            if(tokens.size() == count)
                return tokens;
            tokens.clear();
            std::this_thread::sleep_for(1ms);
        }
        return tokens;
    }
    void attach(VisorClientPortal& portal, bool native = false)
    {
        if(native)
        {
            RPCProviderConf conf;
            conf.PROTO_CONF = protocol;
            conf.IP = "127.0.0.1";
            conf.BASE_PORT = 0;
            conf.SERVICE_PROVIDER_ID = 40;
            ASSERT_EQ(portal.StartServices(conf, registry), CL_SUCCESS);
        }
        else
            portal.core()->registry = registry;
        ClientId client = 100;
        uint64_t offset = 0;
        ASSERT_EQ(portal.ClientConnect(0, client, offset), CL_SUCCESS);
        ASSERT_EQ(portal.CreateChronicle(client, "chronicle"), CL_SUCCESS);
    }
    void TearDown() override
    {
        std::this_thread::sleep_for(350ms);
        registry.reset();
        if(!peerFinalized)
        {
            for(auto& p: procedures) p.deregister();
            peers->finalize();
        }
        peers.reset();
    }
};

#endif
