// ReplayService through a real in-process gRPC channel, backed by HotReplay and KeeperHotSource
// talking to fake Archive servers that stand in for Keepers.
#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>
#include "chrono-player/adapter/ReplayService.h"
#include "chrono-player/replay/HotReplay.h"
#include "chrono-player/replay/KeeperHotSource.h"
#include "chronolog/internal/v1/internal.grpc.pb.h"

namespace chronolog::player
{
namespace
{

using namespace std::chrono_literals;
namespace iv1 = chronolog::internal::v1;

constexpr StoryId kStory = 1;

v1::Event protoEvent(uint64_t writer, uint64_t sequence, int64_t hlc)
{
    v1::Event e;
    e.mutable_id()->set_story_id(kStory);
    e.mutable_id()->set_writer_id(writer);
    e.mutable_id()->set_incarnation(1);
    e.mutable_id()->set_sequence(sequence);
    e.mutable_hlc()->set_physical_ns(hlc);
    e.mutable_physical()->set_physical_ns(hlc);
    e.mutable_envelope()->set_payload("w" + std::to_string(writer) + "s" + std::to_string(sequence));
    e.set_durability(v1::DURABILITY_ACCEPTED);
    return e;
}

class FakeArchive final: public iv1::Archive::Service
{
public:
    void add(v1::Event event)
    {
        std::lock_guard lk(mu_);
        events_.push_back(std::move(event));
    }
    void seal(int64_t physical_ns)
    {
        std::lock_guard lk(mu_);
        sealed_ = physical_ns;
    }
    void setEpoch(uint64_t epoch)
    {
        std::lock_guard lk(mu_);
        epoch_ = epoch;
    }
    void setTruncated(bool truncated)
    {
        std::lock_guard lk(mu_);
        truncated_ = truncated;
    }
    void setDelay(std::chrono::milliseconds delay)
    {
        std::lock_guard lk(mu_);
        delay_ = delay;
    }

    grpc::Status FetchHot(grpc::ServerContext* context,
                          const iv1::FetchHotRequest* request,
                          grpc::ServerWriter<iv1::FetchHotResponse>* writer) override
    {
        std::vector<v1::Event> events;
        int64_t sealed;
        uint64_t epoch;
        bool truncated;
        std::chrono::milliseconds delay;
        {
            std::lock_guard lk(mu_);
            events = events_;
            sealed = sealed_;
            epoch = epoch_;
            truncated = truncated_;
            delay = delay_;
        }
        for(auto waited = 0ms; waited < delay && !context->IsCancelled(); waited += 20ms)
            std::this_thread::sleep_for(20ms);
        if(request->story_id() != kStory)
            return grpc::Status(grpc::StatusCode::NOT_FOUND, "unknown story");
        iv1::FetchHotResponse batch;
        for(const auto& e: events)
        {
            bool in;
            if(request->has_hlc())
            {
                Hlc h{e.hlc().physical_ns(), e.hlc().logical()};
                Hlc s{request->hlc().start().physical_ns(), request->hlc().start().logical()};
                Hlc t{request->hlc().end().physical_ns(), request->hlc().end().logical()};
                in = h >= s && h < t;
            }
            else
            {
                in = e.physical().physical_ns() >= request->physical().start_ns() &&
                     e.physical().physical_ns() < request->physical().end_ns();
            }
            if(in)
                *batch.mutable_batch()->add_events() = e;
            if(batch.batch().events_size() == 2)
            {
                writer->Write(batch);
                batch.Clear();
            }
        }
        if(batch.batch().events_size() > 0)
            writer->Write(batch);
        iv1::FetchHotResponse trailer;
        trailer.mutable_trailer()->set_epoch(epoch);
        trailer.mutable_trailer()->mutable_sealed_frontier()->set_physical_ns(sealed);
        trailer.mutable_trailer()->set_truncated(truncated);
        writer->Write(trailer);
        return grpc::Status::OK;
    }

private:
    std::mutex mu_;
    std::vector<v1::Event> events_;
    int64_t sealed_{200};
    uint64_t epoch_{7};
    bool truncated_{};
    std::chrono::milliseconds delay_{0};
};

struct FakeCatalog final: StoryCatalog
{
    absl::Status ensureLive(StoryId story) const override
    {
        auto it = stories.find(story);
        if(it == stories.end())
            return absl::FailedPreconditionError("unknown story");
        if(it->second)
            return absl::FailedPreconditionError("story is tombstoned");
        return absl::OkStatus();
    }
    std::map<StoryId, bool> stories{{kStory, false}, {2, true}};
};

struct FakeWriters final: WriterSource
{
    std::vector<WriterAssignment> writers(StoryId) const override { return {{2, 1, "keeper-a"}, {4, 1, "keeper-b"}}; }
};

struct ReadResult
{
    std::vector<v1::Event> events;
    std::vector<v1::Completion> completions;
    grpc::Status status;
};

class replay_adapter: public ::testing::Test
{
protected:
    void SetUp() override
    {
        a_.add(protoEvent(2, 1, 110));
        a_.add(protoEvent(2, 2, 130));
        a_.add(protoEvent(2, 3, 150));
        b_.add(protoEvent(4, 1, 120));
        b_.add(protoEvent(4, 2, 140));
        b_.add(protoEvent(4, 3, 160));
        a_server_ = serve(a_, a_addr_);
        b_server_ = serve(b_, b_addr_);

        Route route{7, {{"keeper-a", "keeper-a"}, {"keeper-b", "keeper-b"}}, "", ""};
        KeeperHotSourceOptions options;
        options.deadline = 500ms;
        source_ = std::make_shared<KeeperHotSource>(
                std::make_shared<StaticRouteSource>(route),
                std::make_shared<FakeWriters>(),
                [this](const KeeperRef& k) { return k.process_id == "keeper-a" ? a_addr_ : b_addr_; },
                options);
        HotReplayOptions replay_options;
        replay_options.batch_size = 2;
        replay_options.tail_poll = 20ms;
        service_ = std::make_unique<ReplayService>(std::make_shared<HotReplay>(source_, replay_options),
                                                   std::make_shared<FakeCatalog>());
        grpc::ServerBuilder builder;
        int port = 0;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(service_.get());
        server_ = builder.BuildAndStart();
        ASSERT_NE(server_, nullptr);
        stub_ = v1::Replay::NewStub(
                grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials()));
    }

    void TearDown() override
    {
        service_->shutdown();
        server_->Shutdown(std::chrono::system_clock::now() + 3s);
        a_server_->Shutdown(std::chrono::system_clock::now() + 1s);
        if(b_server_)
            b_server_->Shutdown(std::chrono::system_clock::now() + 1s);
    }

    std::unique_ptr<grpc::Server> serve(FakeArchive& archive, std::string& address)
    {
        grpc::ServerBuilder builder;
        int port = 0;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(&archive);
        auto server = builder.BuildAndStart();
        address = "127.0.0.1:" + std::to_string(port);
        return server;
    }

    std::unique_ptr<grpc::ClientContext> context()
    {
        auto ctx = std::make_unique<grpc::ClientContext>();
        ctx->set_deadline(std::chrono::system_clock::now() + 10s);
        return ctx;
    }

    static v1::ReadRequest hlcRead(int64_t start, int64_t end)
    {
        v1::ReadRequest request;
        request.set_story_id(kStory);
        request.mutable_hlc()->mutable_start()->set_physical_ns(start);
        request.mutable_hlc()->mutable_end()->set_physical_ns(end);
        return request;
    }

    ReadResult read(const v1::ReadRequest& request)
    {
        ReadResult result;
        auto ctx = context();
        auto reader = stub_->Read(ctx.get(), request);
        v1::ReadResponse response;
        for(int i = 0; i < 1000 && reader->Read(&response); ++i)
        {
            for(const auto& e: response.batch().events()) result.events.push_back(e);
            if(response.has_completion())
                result.completions.push_back(response.completion());
        }
        result.status = reader->Finish();
        return result;
    }

    v1::TailRequest tailFrom(const v1::Event& position)
    {
        v1::TailRequest request;
        request.set_story_id(kStory);
        *request.mutable_from()->mutable_hlc() = position.hlc();
        *request.mutable_from()->mutable_id() = position.id();
        return request;
    }

    FakeArchive a_, b_;
    std::string a_addr_, b_addr_;
    std::unique_ptr<grpc::Server> a_server_, b_server_, server_;
    std::shared_ptr<KeeperHotSource> source_;
    std::unique_ptr<ReplayService> service_;
    std::unique_ptr<v1::Replay::Stub> stub_;
};

TEST_F(replay_adapter, ReadIsCompleteWhenEveryKeeperSealReachesEnd)
{
    auto r = read(hlcRead(100, 200));
    ASSERT_TRUE(r.status.ok()) << r.status.error_message();
    ASSERT_EQ(r.events.size(), 6u);
    for(size_t i = 1; i < r.events.size(); ++i)
        EXPECT_LT(r.events[i - 1].hlc().physical_ns(), r.events[i].hlc().physical_ns());
    EXPECT_EQ(r.events[0].envelope().payload(), "w2s1");
    ASSERT_EQ(r.completions.size(), 1u);
    EXPECT_TRUE(r.completions[0].complete());
    EXPECT_EQ(r.completions[0].reason(), v1::INCOMPLETE_REASON_UNSPECIFIED);
    EXPECT_EQ(r.completions[0].frontier().physical_ns(), 200);
}

TEST_F(replay_adapter, EqualityOfSealAndEndIsSufficient)
{
    b_.seal(200);
    auto r = read(hlcRead(100, 200));
    ASSERT_EQ(r.completions.size(), 1u);
    EXPECT_TRUE(r.completions[0].complete());
}

TEST_F(replay_adapter, LaggingKeeperNamesItsWriters)
{
    b_.seal(150);
    auto r = read(hlcRead(100, 200));
    ASSERT_TRUE(r.status.ok());
    ASSERT_EQ(r.completions.size(), 1u);
    EXPECT_FALSE(r.completions[0].complete());
    EXPECT_EQ(r.completions[0].reason(), v1::INCOMPLETE_REASON_LAGGING_WRITERS);
    ASSERT_EQ(r.completions[0].laggards_size(), 1);
    EXPECT_EQ(r.completions[0].laggards(0).writer_id(), 4u);
    EXPECT_EQ(r.completions[0].laggards(0).frontier().physical_ns(), 150);
}

TEST_F(replay_adapter, KeeperThatIsDownIsSourceFailed)
{
    b_server_->Shutdown(std::chrono::system_clock::now());
    b_server_.reset();
    auto r = read(hlcRead(100, 200));
    ASSERT_TRUE(r.status.ok());
    EXPECT_EQ(r.events.size(), 3u);
    ASSERT_EQ(r.completions.size(), 1u);
    EXPECT_FALSE(r.completions[0].complete());
    EXPECT_EQ(r.completions[0].reason(), v1::INCOMPLETE_REASON_SOURCE_FAILED);
}

TEST_F(replay_adapter, KeeperSlowerThanTheDeadlineIsSourceFailed)
{
    b_.setDelay(3s);
    auto r = read(hlcRead(100, 200));
    ASSERT_EQ(r.completions.size(), 1u);
    EXPECT_EQ(r.completions[0].reason(), v1::INCOMPLETE_REASON_SOURCE_FAILED);
}

TEST_F(replay_adapter, KeeperEpochMismatchIsSourceFailed)
{
    b_.setEpoch(6);
    auto r = read(hlcRead(100, 200));
    ASSERT_EQ(r.completions.size(), 1u);
    EXPECT_FALSE(r.completions[0].complete());
    EXPECT_EQ(r.completions[0].reason(), v1::INCOMPLETE_REASON_SOURCE_FAILED);
}

TEST_F(replay_adapter, TruncatedKeeperIsTruncated)
{
    a_.setTruncated(true);
    auto r = read(hlcRead(100, 200));
    ASSERT_EQ(r.completions.size(), 1u);
    EXPECT_EQ(r.completions[0].reason(), v1::INCOMPLETE_REASON_TRUNCATED);
}

TEST_F(replay_adapter, PhysicalAxisIsUnbounded)
{
    v1::ReadRequest request;
    request.set_story_id(kStory);
    request.mutable_physical()->set_start_ns(100);
    request.mutable_physical()->set_end_ns(200);
    auto r = read(request);
    ASSERT_TRUE(r.status.ok());
    EXPECT_EQ(r.events.size(), 6u);
    ASSERT_EQ(r.completions.size(), 1u);
    EXPECT_FALSE(r.completions[0].complete());
    EXPECT_EQ(r.completions[0].reason(), v1::INCOMPLETE_REASON_PHYSICAL_AXIS_UNBOUNDED);
}

TEST_F(replay_adapter, UnsetRangeIsInvalidArgument)
{
    v1::ReadRequest request;
    request.set_story_id(kStory);
    auto r = read(request);
    EXPECT_EQ(r.status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
    EXPECT_TRUE(r.events.empty());
}

TEST_F(replay_adapter, TombstonedStoryFailsPrecondition)
{
    auto request = hlcRead(100, 200);
    request.set_story_id(2);
    EXPECT_EQ(read(request).status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
}

TEST_F(replay_adapter, UnknownStoryFailsPrecondition)
{
    auto request = hlcRead(100, 200);
    request.set_story_id(99);
    EXPECT_EQ(read(request).status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
}

TEST_F(replay_adapter, TailWithoutPositionIsInvalidArgument)
{
    v1::TailRequest request;
    request.set_story_id(kStory);
    auto ctx = context();
    auto reader = stub_->Tail(ctx.get(), request);
    v1::TailResponse response;
    EXPECT_FALSE(reader->Read(&response));
    EXPECT_EQ(reader->Finish().error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_F(replay_adapter, TailResumesExclusivelyAndFollowsNewEvents)
{
    auto ctx = context();
    auto reader = stub_->Tail(ctx.get(), tailFrom(protoEvent(2, 2, 130)));
    std::vector<int64_t> seen;
    v1::TailResponse response;
    auto pump = [&](size_t want)
    {
        for(int i = 0; i < 100 && seen.size() < want && reader->Read(&response); ++i)
        {
            EXPECT_FALSE(response.has_completion());
            for(const auto& e: response.batch().events()) seen.push_back(e.hlc().physical_ns());
        }
    };
    pump(3);
    EXPECT_EQ(seen, (std::vector<int64_t>{140, 150, 160}));
    a_.add(protoEvent(2, 4, 170));
    pump(4);
    EXPECT_EQ(seen, (std::vector<int64_t>{140, 150, 160, 170}));
    ctx->TryCancel();
    while(reader->Read(&response)) {}
    EXPECT_EQ(reader->Finish().error_code(), grpc::StatusCode::CANCELLED);
}

TEST_F(replay_adapter, ClientCancelReleasesTheStream)
{
    auto ctx = context();
    auto reader = stub_->Tail(ctx.get(), tailFrom(protoEvent(2, 2, 130)));
    v1::TailResponse response;
    ASSERT_TRUE(reader->Read(&response));
    EXPECT_EQ(service_->activeStreams(), 1u);
    ctx->TryCancel();
    while(reader->Read(&response)) {}
    reader->Finish();
    for(int i = 0; i < 250 && service_->activeStreams() != 0; ++i) std::this_thread::sleep_for(20ms);
    EXPECT_EQ(service_->activeStreams(), 0u);
}

TEST_F(replay_adapter, ShutdownEndsATailWithAnIncompleteCompletion)
{
    auto ctx = context();
    auto reader = stub_->Tail(ctx.get(), tailFrom(protoEvent(2, 2, 130)));
    v1::TailResponse response;
    size_t events = 0;
    for(int i = 0; i < 100 && events < 3 && reader->Read(&response); ++i) events += response.batch().events_size();
    ASSERT_EQ(events, 3u);
    service_->shutdown();
    std::vector<v1::Completion> completions;
    for(int i = 0; i < 100 && reader->Read(&response); ++i)
        if(response.has_completion())
            completions.push_back(response.completion());
    EXPECT_TRUE(reader->Finish().ok());
    ASSERT_EQ(completions.size(), 1u);
    EXPECT_FALSE(completions[0].complete());
}

TEST_F(replay_adapter, CallsAfterShutdownAreUnavailable)
{
    service_->shutdown();
    EXPECT_EQ(read(hlcRead(100, 200)).status.error_code(), grpc::StatusCode::UNAVAILABLE);
}

} // namespace
} // namespace chronolog::player
