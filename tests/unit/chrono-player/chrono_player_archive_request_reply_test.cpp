// A replay whose archive read the reading agent cannot use still gets its
// reply (#741). PlaybackService stashes a query's response as incomplete while
// an archive read is pending, and only the reading agent hands it over. An
// ArchiveReadingRequest that fails is_valid() (a start time of 0 did) used to
// be dropped, so the client waited out its 180 s query timeout.
//
// The test stands up a fake client query service that receives the response,
// a QueryResponseAgent sending to it, and an ArchiveReadingAgent over an empty
// archive directory, all over ofi+sockets on 127.0.0.1.

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <cereal/archives/binary.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/vector.hpp>
#include <thallium.hpp>

#include <ArchiveReadingAgent.h>
#include <ArchiveReadingRequestQueue.h>
#include <PlaybackQueryResponse.h>
#include <QueryResponseTransferAgent.h>
#include <ServiceId.h>
#include <chrono_monitor.h>
#include <chronolog_errcode.h>

namespace chl = chronolog;
namespace tl = thallium;

namespace
{
constexpr char kProtocol[] = "ofi+sockets";
constexpr uint16_t kReceiverProvider = 57;

void ensureLogger()
{
    static bool done = false;
    if(!done)
    {
        chl::chrono_monitor::initialize("console", "", chl::LogLevel::err, "archive_request_reply_test_logger");
        done = true;
    }
}

// Stands in for the client's ClientQueryService: receives the responses the
// player's QueryResponseAgent sends and keeps them by query id.
class FakeQueryReceiver: public tl::provider<FakeQueryReceiver>
{
public:
    FakeQueryReceiver(tl::engine& engine, uint16_t provider_id)
        : tl::provider<FakeQueryReceiver>(engine, provider_id)
        , theEngine(engine)
    {
        define("receiver_is_available", &FakeQueryReceiver::receiver_is_available);
        define("receive_query_response", &FakeQueryReceiver::receive_query_response);
    }

    void receiver_is_available(tl::request const& request) { request.respond(true); }

    void receive_query_response(tl::request const& request, tl::bulk& remote_bulk)
    {
        std::vector<char> buffer(remote_bulk.size());
        std::vector<std::pair<void*, std::size_t>> segments{{buffer.data(), buffer.size()}};
        tl::bulk local = theEngine.expose(segments, tl::bulk_mode::write_only);
        remote_bulk.on(request.get_endpoint()) >> local;

        chl::PlaybackQueryResponse response;
        std::istringstream iss(std::string(buffer.data(), buffer.size()), std::ios::binary);
        cereal::BinaryInputArchive iarchive(iss);
        iarchive(response);
        {
            std::lock_guard<std::mutex> lock(mutex);
            responses[response.query_id] = response;
        }
        request.respond(buffer.size());
    }

    // waits up to timeout for the response of query_id
    bool waitFor(chl::ClientQueryId query_id, std::chrono::seconds timeout, chl::PlaybackQueryResponse& out)
    {
        auto const until = std::chrono::steady_clock::now() + timeout;
        while(std::chrono::steady_clock::now() < until)
        {
            {
                std::lock_guard<std::mutex> lock(mutex);
                auto it = responses.find(query_id);
                if(it != responses.end())
                {
                    out = it->second;
                    return true;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return false;
    }

private:
    tl::engine& theEngine;
    std::mutex mutex;
    std::map<chl::ClientQueryId, chl::PlaybackQueryResponse> responses;
};

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

        receiverEngine =
                std::make_unique<tl::engine>(std::string(kProtocol) + "://127.0.0.1", THALLIUM_SERVER_MODE, true, 1);
        receiver = std::make_unique<FakeQueryReceiver>(*receiverEngine, kReceiverProvider);
        playerEngine =
                std::make_unique<tl::engine>(std::string(kProtocol) + "://127.0.0.1", THALLIUM_SERVER_MODE, true, 1);

        std::string const self = receiverEngine->self();
        auto const port = static_cast<uint16_t>(std::stoul(self.substr(self.rfind(':') + 1)));
        sender = chl::QueryResponseAgent::CreateQueryResponseAgent(
                *playerEngine,
                chl::ServiceId(kProtocol, "127.0.0.1", port, kReceiverProvider));
        ASSERT_NE(sender, nullptr);
        sender->startResponseThreads(1);

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
        if(sender)
        {
            sender->stopResponseThreads();
            delete sender;
            sender = nullptr;
        }
        playerEngine->finalize();
        receiver.reset();
        receiverEngine->finalize();
        std::filesystem::remove_all(archiveDir);
    }

    // What PlaybackService does when the reply needs an archive read: the
    // response is stashed as not ready, and the request goes on the queue.
    void startArchivedQuery(chl::ClientQueryId query_id, uint64_t start, uint64_t end)
    {
        auto* response = new chl::PlaybackQueryResponse(query_id);
        ASSERT_EQ(sender->stashQueryResponseRecord(query_id, response, false), chl::CL_SUCCESS);
        requestQueue.pushReadingRequest(
                new chl::ArchiveReadingRequest(sender, query_id, "chronicle", "story", start, end));
    }

    std::filesystem::path archiveDir;
    std::unique_ptr<tl::engine> receiverEngine;
    std::unique_ptr<FakeQueryReceiver> receiver;
    std::unique_ptr<tl::engine> playerEngine;
    chl::QueryResponseAgent* sender = nullptr;
    chl::ArchiveReadingRequestQueue requestQueue;
    std::unique_ptr<chl::ArchiveReadingAgent> readingAgent;
};

} // namespace

// A request that is_valid() refuses (here a start time of 0) still sends the
// query's response, marked incomplete, so the client gets
// CL_ERR_PARTIAL_RESULT instead of waiting for its timeout.
TEST_F(ArchiveRequestReply, UnusableRequestStillAnswersTheQuery)
{
    startArchivedQuery(11, 0, 1000);

    chl::PlaybackQueryResponse response;
    ASSERT_TRUE(receiver->waitFor(11, std::chrono::seconds(10), response))
            << "the reading agent dropped the request and the query got no response";
    EXPECT_FALSE(response.complete);
    EXPECT_TRUE(response.events.empty());
}

// The usable request of the same shape is answered complete: the archive is
// empty, and an empty answer is the whole answer.
TEST_F(ArchiveRequestReply, UsableRequestAnswersComplete)
{
    startArchivedQuery(12, 1, 1000);

    chl::PlaybackQueryResponse response;
    ASSERT_TRUE(receiver->waitFor(12, std::chrono::seconds(10), response));
    EXPECT_TRUE(response.complete);
    EXPECT_TRUE(response.events.empty());
}
