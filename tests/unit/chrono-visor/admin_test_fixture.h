#ifndef CHRONOLOG_ADMIN_TEST_FIXTURE_H
#define CHRONOLOG_ADMIN_TEST_FIXTURE_H
#include <gtest/gtest.h>
#include <KeeperRegistry.h>
#include <VisorClientPortal.h>
#include <DataStoreAdminClient.h>
#include <chrono_monitor.h>
#include <thallium/serialization/stl/string.hpp>
#include <thallium/serialization/stl/vector.hpp>
#include <atomic>
#include <chrono>
#include <thread>

namespace tl = thallium;
using namespace chronolog;
using namespace std::chrono_literals;

class AdminDeadline: public ::testing::TestWithParam<std::string>
{
protected:
    std::unique_ptr<tl::engine> peers;
    std::shared_ptr<KeeperRegistry> registry;
    std::atomic<int> starts[5]{};
    std::atomic<int> stops[5]{};
    int stopDelays[5]{};
    std::mutex rosterMutex;
    int delays[5]{0, 300, 300, 0, 0};
    int replies[5]{0, 0, 0, 0, 0};
    std::vector<tl::remote_procedure> procedures;
    std::vector<ServiceId> playerRoster;
    std::string protocol;
    uint16_t port{};
    ServiceId service(int provider) { return ServiceId(protocol, "127.0.0.1", port, provider); }
    KeeperIdCard keeper(int i) { return KeeperIdCard(1, ServiceId(protocol, "127.0.0.1", 30000 + i, 10 + i)); }
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
                        try
                        {
                            req.respond(replies[i]);
                        }
                        catch(...)
                        {}
                    },
                    static_cast<uint16_t>(i + 1)));
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
                    [](tl::request const& req)
                    {
                        try
                        {
                            req.respond(0);
                        }
                        catch(...)
                        {}
                    },
                    static_cast<uint16_t>(i + 1)));
        }
        RpcTimeoutPolicy policy;
        policy.control_ms = 120;
        policy.attempt_ms = 120;
        auto name = std::string(::testing::UnitTest::GetInstance()->current_test_info()->name());
        auto capacity = name.find("ThreeSlots") != std::string::npos
                                ? 3u
                                : (name.find("Saturation") != std::string::npos ? 32u : 8u);
        registry = name.find("Saturation") != std::string::npos ? std::make_shared<KeeperRegistry>(policy)
                                                                : std::make_shared<KeeperRegistry>(policy, capacity);
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
        for(auto& p: procedures) p.deregister();
        peers->finalize();
        peers.reset();
    }
};

#endif
