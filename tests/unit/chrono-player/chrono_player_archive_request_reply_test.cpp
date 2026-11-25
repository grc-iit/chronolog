// A replay whose archive read the reading agent cannot use still gets its
// reply (#741). PlaybackService stashes a query's response as not ready while
// an archive read is pending, and only the reading agent marks it ready; the
// query's owned work then takes it with takeReady and sends it to the client.
// An ArchiveReadingRequest that fails is_valid() (a start time of 0 did) used
// to be dropped, so the response never became ready and the client waited out
// its 180 s query timeout.
//
// The test stashes a response on a QueryResponseAgent as PlaybackService does,
// queues the archive request to an ArchiveReadingAgent over an empty archive
// directory, and waits for the response to become ready.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <vector>
#include <memory>
#include <string>
#include <thread>

#include <thallium.hpp>

#include <ArchiveReadingAgent.h>
#include <ArchiveReadingRequestQueue.h>
#include <PlaybackQueryResponse.h>
#include <PlaybackRpcOwner.h>
#include <QueryResponseTransferAgent.h>
#include <ServiceId.h>
#include <chrono_monitor.h>
#include <chronolog_errcode.h>
#include <private/RpcBulkSerialization.h>

namespace chl = chronolog;
namespace tl = thallium;

namespace chronolog
{
// The stash PlaybackService makes before it queues an archive read.
struct QueryResponseAgentTestAccess
{
    static int stash(QueryResponseAgent& agent, ClientQueryId query, std::shared_ptr<PlaybackQueryResponse> response)
    {
        return agent.stashOwnedQueryResponse(query, response, false);
    }
};
} // namespace chronolog

namespace
{
constexpr char kProtocol[] = "ofi+sockets";

void ensureLogger()
{
    static bool done = false;
    if(!done)
    {
        chl::chrono_monitor::initialize("console", "", chl::LogLevel::err, "archive_request_reply_test_logger");
        done = true;
    }
}

class ArchiveRequestReply: public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureLogger();
        archiveDir = std::filesystem::temp_directory_path() /
                     ("chronolog_archive_request_reply_" + std::to_string(::getpid()));
        std::filesystem::remove_all(archiveDir);
        std::filesystem::create_directories(archiveDir);

        owner = std::make_shared<chl::PlaybackRpcOwner>(
                std::make_unique<tl::engine>(std::string(kProtocol) + "://127.0.0.1", THALLIUM_SERVER_MODE, true, 1),
                chl::RpcTimeoutPolicy{});
        // The response is only marked ready here, never sent, so the route
        // needs no receiver.
        agent.reset(chl::QueryResponseAgent::CreateQueryResponseAgent(owner,
                                                                      chl::ServiceId(kProtocol, "127.0.0.1", 1, 57)));
        ASSERT_NE(agent, nullptr);

        readingAgent = std::make_unique<chl::ArchiveReadingAgent>(requestQueue, archiveDir.string());
        readingAgent->startArchiveReading(1);
    }

    void TearDown() override
    {
        if(readingAgent)
        {
            readingAgent->shutdownArchiveReading();
            readingAgent.reset();
        }
        agent.reset();
        if(owner && owner->engine)
            owner->engine->finalize();
        owner.reset();
        std::filesystem::remove_all(archiveDir);
    }

    // What PlaybackService does when the reply needs an archive read: the
    // response is stashed as not ready, and the request goes on the queue.
    void startArchivedQuery(chl::ClientQueryId query_id, uint64_t start, uint64_t end)
    {
        ASSERT_EQ(chl::QueryResponseAgentTestAccess::stash(*agent,
                                                           query_id,
                                                           std::make_shared<chl::PlaybackQueryResponse>(query_id)),
                  chl::CL_SUCCESS);
        requestQueue.pushReadingRequest(
                new chl::ArchiveReadingRequest(agent.get(), query_id, "chronicle", "story", start, end));
    }

    // The response once the reading agent has made it ready, or null after
    // timeout.
    std::shared_ptr<chl::PlaybackQueryResponse> waitReady(chl::ClientQueryId query_id, std::chrono::seconds timeout)
    {
        auto const until = std::chrono::steady_clock::now() + timeout;
        while(std::chrono::steady_clock::now() < until)
        {
            if(auto response = agent->takeReady(query_id))
                return response;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return {};
    }

    std::filesystem::path archiveDir;
    std::shared_ptr<chl::PlaybackRpcOwner> owner;
    std::unique_ptr<chl::QueryResponseAgent> agent;
    chl::ArchiveReadingRequestQueue requestQueue;
    std::unique_ptr<chl::ArchiveReadingAgent> readingAgent;
};

} // namespace

// A request that is_valid() refuses (here a start time of 0) still makes the
// query's response ready, marked incomplete, so the client gets
// CL_ERR_PARTIAL_RESULT instead of waiting for its timeout.
TEST_F(ArchiveRequestReply, UnusableRequestStillAnswersTheQuery)
{
    startArchivedQuery(11, 0, 1000);

    auto response = waitReady(11, std::chrono::seconds(10));
    ASSERT_NE(response, nullptr) << "the reading agent dropped the request and the query got no response";
    EXPECT_FALSE(response->complete);
    EXPECT_TRUE(response->events.empty());
}

// The usable request of the same shape is answered complete: the archive is
// empty, and an empty answer is the whole answer.
TEST_F(ArchiveRequestReply, UsableRequestAnswersComplete)
{
    startArchivedQuery(12, 1, 1000);

    auto response = waitReady(12, std::chrono::seconds(10));
    ASSERT_NE(response, nullptr);
    EXPECT_TRUE(response->complete);
    EXPECT_TRUE(response->events.empty());
}

// Deleting one QueryResponseAgent leaves the other agents of the same engine
// working (#707, #743). The player holds one agent per client receiver. Every
// agent sends receive_query_response through its engine's PlaybackRpcOwner,
// and two clients whose receivers use the same provider id share that RPC ID
// on the player's engine. Before #740 the agent's destructor deregistered it,
// and the next response of every other client's agent failed with HG_NOENTRY.
//
// Each fake client receiver runs on an engine of its own, with the provider id
// the client library uses, pulls the response the agent exposes and records
// its query id.
namespace
{
constexpr uint16_t kReceiverProvider = 57;

class FakeQueryReceiver
{
public:
    explicit FakeQueryReceiver(tl::engine& engine)
        : engine_(engine)
    {
        procedure_ = engine_.define(
                "receive_query_response",
                [this](tl::request const& request, tl::bulk& source)
                {
                    std::vector<char> bytes(source.size());
                    auto target = engine_.expose({{bytes.data(), bytes.size()}}, tl::bulk_mode::write_only);
                    source.on(request.get_endpoint()) >> target;
                    chl::PlaybackQueryResponse response;
                    if(chl::rpc::deserializeBulk(bytes.data(), bytes.size(), response))
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        queries_.push_back(response.query_id);
                    }
                    request.respond(static_cast<uint64_t>(bytes.size()));
                },
                kReceiverProvider);
    }

    bool received(chl::ClientQueryId query)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::find(queries_.begin(), queries_.end(), query) != queries_.end();
    }

    bool waitFor(chl::ClientQueryId query, std::chrono::seconds timeout)
    {
        auto const until = std::chrono::steady_clock::now() + timeout;
        while(std::chrono::steady_clock::now() < until)
        {
            if(received(query))
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }

private:
    tl::engine& engine_;
    tl::remote_procedure procedure_{};
    std::mutex mutex_;
    std::vector<chl::ClientQueryId> queries_;
};

chl::ServiceId receiverOf(tl::engine& engine)
{
    std::string const self = engine.self();
    auto const port = static_cast<uint16_t>(std::stoul(self.substr(self.rfind(':') + 1)));
    return chl::ServiceId(kProtocol, "127.0.0.1", port, kReceiverProvider);
}
} // namespace

TEST(QueryResponseAgentLifetime, DeletingAnotherClientsAgentLeavesThisOneWorking)
{
    ensureLogger();
    auto newEngine = []
    { return std::make_unique<tl::engine>(std::string(kProtocol) + "://127.0.0.1", THALLIUM_SERVER_MODE, true, 1); };
    auto stayingEngine = newEngine();
    auto leavingEngine = newEngine();
    FakeQueryReceiver stayingReceiver(*stayingEngine);
    FakeQueryReceiver leavingReceiver(*leavingEngine);

    auto owner = std::make_shared<chl::PlaybackRpcOwner>(newEngine(), chl::RpcTimeoutPolicy{});
    auto runtime = std::make_shared<chl::rpc::RpcRuntime>(6);
    owner->attach(runtime);
    owner->initializeProcedure();

    std::unique_ptr<chl::QueryResponseAgent> staying(
            chl::QueryResponseAgent::CreateQueryResponseAgent(owner, receiverOf(*stayingEngine)));
    chl::QueryResponseAgent* leaving =
            chl::QueryResponseAgent::CreateQueryResponseAgent(owner, receiverOf(*leavingEngine));
    ASSERT_NE(staying, nullptr);
    ASSERT_NE(leaving, nullptr);
    ASSERT_EQ(leaving->transferQueryResponseToClient(chl::PlaybackQueryResponse(12)), chl::CL_SUCCESS);
    ASSERT_TRUE(leavingReceiver.waitFor(12, std::chrono::seconds(10)));

    delete leaving;

    EXPECT_EQ(staying->transferQueryResponseToClient(chl::PlaybackQueryResponse(13)), chl::CL_SUCCESS);
    EXPECT_TRUE(stayingReceiver.waitFor(13, std::chrono::seconds(10)))
            << "the response of the agent that stayed did not arrive";
    EXPECT_FALSE(leavingReceiver.received(13));

    staying.reset();
    owner->quiesce();
    stayingEngine->finalize();
    leavingEngine->finalize();
}
