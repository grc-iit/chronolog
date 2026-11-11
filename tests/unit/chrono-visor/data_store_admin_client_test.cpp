// Deleting one DataStoreAdminClient leaves the others on the same engine
// working (#743). The visor holds one DataStoreAdminClient per keeper, grapher
// and player, all defining the same RPC names on one engine, so they share the
// RPC IDs. The destructor used to deregister those IDs, which took them away
// from every other client: once a keeper left, the visor could no longer
// start a story on the keepers that stayed, and every acquire failed with
// CL_ERR_NO_KEEPERS.
//
// The test stands up two fake data store admin services, each on an engine of
// its own, and a visor-side engine with one DataStoreAdminClient per service,
// over ofi+sockets on 127.0.0.1.

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <string>

#include <thallium.hpp>
#include <thallium/serialization/stl/string.hpp>

#include <DataStoreAdminClient.h>
#include <chrono_monitor.h>
#include <chronolog_errcode.h>
#include <chronolog_types.h>

namespace chl = chronolog;
namespace tl = thallium;

namespace
{
constexpr char kProtocol[] = "ofi+sockets";
constexpr uint16_t kAdminProvider = 77;

void ensureLogger()
{
    static bool done = false;
    if(!done)
    {
        chl::chrono_monitor::initialize("console", "", chl::LogLevel::err, "data_store_admin_client_test_logger");
        done = true;
    }
}

// Stands in for a keeper's DataStoreAdminService: answers the admin RPCs the
// visor sends and counts them.
class FakeAdminService: public tl::provider<FakeAdminService>
{
public:
    FakeAdminService(tl::engine& engine, uint16_t provider_id)
        : tl::provider<FakeAdminService>(engine, provider_id)
    {
        define("collection_service_available", &FakeAdminService::collection_service_available);
        define("start_story_recording", &FakeAdminService::start_story_recording);
        define("stop_story_recording", &FakeAdminService::stop_story_recording);
    }

    void collection_service_available(tl::request const& request) { request.respond(1); }

    void start_story_recording(tl::request const& request,
                               chl::ChronicleName const&,
                               chl::StoryName const&,
                               chl::StoryId const&,
                               uint64_t)
    {
        ++started;
        request.respond(static_cast<int>(chl::CL_SUCCESS));
    }

    void stop_story_recording(tl::request const& request, chl::StoryId const&)
    {
        ++stopped;
        request.respond(static_cast<int>(chl::CL_SUCCESS));
    }

    std::atomic<int> started{0};
    std::atomic<int> stopped{0};
};

class DataStoreAdminClientLifetime: public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureLogger();
        for(int i = 0; i < 2; ++i)
        {
            peerEngine[i] = std::make_unique<tl::engine>(std::string(kProtocol) + "://127.0.0.1",
                                                         THALLIUM_SERVER_MODE,
                                                         true,
                                                         1);
            peer[i] = std::make_unique<FakeAdminService>(*peerEngine[i], kAdminProvider);
        }
        visorEngine =
                std::make_unique<tl::engine>(std::string(kProtocol) + "://127.0.0.1", THALLIUM_SERVER_MODE, true, 1);
    }

    void TearDown() override
    {
        visorEngine->finalize();
        for(int i = 0; i < 2; ++i)
        {
            peer[i].reset();
            peerEngine[i]->finalize();
        }
    }

    chl::DataStoreAdminClient* clientFor(int i)
    {
        return chl::DataStoreAdminClient::CreateDataStoreAdminClient(*visorEngine,
                                                                     peerEngine[i]->self(),
                                                                     kAdminProvider);
    }

    std::unique_ptr<tl::engine> peerEngine[2];
    std::unique_ptr<FakeAdminService> peer[2];
    std::unique_ptr<tl::engine> visorEngine;
};

} // namespace

// The visor deletes the admin client of a keeper that left. The admin client
// of the keeper that stayed still starts and stops a story on it.
TEST_F(DataStoreAdminClientLifetime, DeletingOneClientLeavesTheOtherWorking)
{
    chl::DataStoreAdminClient* leaving = clientFor(0);
    std::unique_ptr<chl::DataStoreAdminClient> staying(clientFor(1));
    ASSERT_NE(leaving, nullptr);
    ASSERT_NE(staying, nullptr);
    ASSERT_EQ(leaving->collection_is_available(), 1);
    ASSERT_EQ(staying->collection_is_available(), 1);

    delete leaving;

    EXPECT_EQ(staying->collection_is_available(), 1);
    EXPECT_EQ(staying->send_start_story_recording("chronicle", "story", 7, 1000), chl::CL_SUCCESS);
    EXPECT_EQ(staying->send_stop_story_recording(7), chl::CL_SUCCESS);
    EXPECT_EQ(peer[1]->started.load(), 1);
    EXPECT_EQ(peer[1]->stopped.load(), 1);
    EXPECT_EQ(peer[0]->started.load(), 0);
}

// A keeper that comes back gets a new admin client after the old one was
// deleted; both it and the client that stayed work.
TEST_F(DataStoreAdminClientLifetime, ClientCreatedAfterADeleteWorks)
{
    std::unique_ptr<chl::DataStoreAdminClient> staying(clientFor(1));
    delete clientFor(0);
    std::unique_ptr<chl::DataStoreAdminClient> returning(clientFor(0));
    ASSERT_NE(returning, nullptr);

    EXPECT_EQ(returning->send_start_story_recording("chronicle", "story", 8, 1000), chl::CL_SUCCESS);
    EXPECT_EQ(staying->send_start_story_recording("chronicle", "story", 8, 1000), chl::CL_SUCCESS);
    EXPECT_EQ(peer[0]->started.load(), 1);
    EXPECT_EQ(peer[1]->started.load(), 1);
}
