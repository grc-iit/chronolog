// Legacy V commits dc903e86 and 9981a6a3: replay never runs user code on a receive thread, and
// concurrent queries that time out on one Client are safe. The 4.0 SDK has no callbacks; a stream
// delivers on the thread that calls next(), and a late response must not reach a timed out call.
#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>
#include "chronolog/client/client.h"
#include "chronolog/v1/chronolog.grpc.pb.h"

namespace
{
namespace wire = chronolog::v1;
namespace sdk = chronolog::client;
using namespace std::chrono_literals;

class HeldReplayServer final
    : public wire::Catalog::Service
    , public wire::Replay::Service
{
public:
    HeldReplayServer()
    {
        grpc::ServerBuilder builder;
        int port = 0;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(static_cast<wire::Catalog::Service*>(this));
        builder.RegisterService(static_cast<wire::Replay::Service*>(this));
        server = builder.BuildAndStart();
        endpoint = "127.0.0.1:" + std::to_string(port);
    }
    ~HeldReplayServer()
    {
        release();
        server->Shutdown(std::chrono::system_clock::now() + 2s);
    }
    grpc::Status GetStory(grpc::ServerContext*, const wire::GetStoryRequest* r, wire::GetStoryResponse* out) override
    {
        auto* story = out->mutable_story();
        story->set_story_id(r->story_id());
        story->set_epoch(2);
        story->mutable_route()->set_epoch(2);
        story->mutable_route()->set_player(endpoint);
        return grpc::Status::OK;
    }
    // Holds every Read until released, then answers it late.
    grpc::Status Read(grpc::ServerContext* context,
                      const wire::ReadRequest*,
                      grpc::ServerWriter<wire::ReadResponse>* writer) override
    {
        ++held;
        {
            std::unique_lock lock(mutex);
            while(!released && !context->IsCancelled()) cv.wait_for(lock, 10ms, [&] { return released; });
        }
        wire::ReadResponse late;
        late.mutable_batch()->add_events()->mutable_id()->set_story_id(1);
        writer->Write(late);
        return grpc::Status::OK;
    }
    void release()
    {
        {
            std::lock_guard lock(mutex);
            released = true;
        }
        cv.notify_all();
    }
    sdk::ClientOptions options()
    {
        sdk::ClientOptions result;
        result.catalog_endpoint = endpoint;
        result.rpc_timeout = 5s;
        return result;
    }
    std::string endpoint;
    std::unique_ptr<grpc::Server> server;
    std::atomic<int> held{};
    std::mutex mutex;
    std::condition_variable cv;
    bool released{};
};

TEST(ClientReplayThreading, ConcurrentPullsThatTimeOutOnOneClientAreSafeAndLateResponsesAreDropped)
{
    HeldReplayServer server;
    auto client = sdk::Client::Connect(server.options());
    ASSERT_TRUE(client.ok());
    constexpr int threads = 8, rounds = 3;
    std::atomic<int> timed_out{0}, delivered{0};
    std::vector<std::thread> pool;
    for(int t = 0; t < threads; ++t)
        pool.emplace_back(
                [&]
                {
                    for(int round = 0; round < rounds; ++round)
                    {
                        auto read = client->read(1, {{0, 0}, {10, 0}});
                        if(!read.ok())
                            continue;
                        auto item = read->next(std::chrono::system_clock::now() + 50ms);
                        if(item.ok() && *item)
                            ++delivered;
                        else if(!item.ok() && absl::IsDeadlineExceeded(item.status()))
                            ++timed_out;
                    }
                });
    for(auto& thread: pool) thread.join();
    server.release();
    EXPECT_EQ(timed_out.load(), threads * rounds);
    EXPECT_EQ(delivered.load(), 0) << "a response that arrived after the deadline reached the caller";
}
} // namespace
