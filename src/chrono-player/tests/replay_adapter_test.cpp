// ReplayService through a real in-process gRPC channel, backed by HotReplay and KeeperHotSource
// talking to fake Archive servers that stand in for Keepers.
#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <map>
#include <mutex>
#include <thread>
#include "rpc/Channel.h"
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
    void setInstance(std::string instance)
    {
        std::lock_guard lk(mu_);
        instance_ = std::move(instance);
    }
    // Replaces what the Keeper holds and reports; a different instance is a restart.
    void hold(std::string instance, std::vector<v1::Event> events, int64_t physical_ns)
    {
        std::lock_guard lk(mu_);
        instance_ = std::move(instance);
        events_ = std::move(events);
        sealed_ = physical_ns;
    }
    void refuse(bool refused)
    {
        std::lock_guard lk(mu_);
        refused_ = refused;
    }
    bool waitCalls(unsigned calls)
    {
        std::unique_lock lk(mu_);
        return called_.wait_for(lk, 10s, [&] { return seen_ >= calls; });
    }

    std::atomic<unsigned> unavailable{0};
    std::atomic<unsigned> calls{0};
    std::atomic<bool> enforce_epoch{false};
    std::atomic<unsigned> stale_epochs{0};
    std::atomic<Epoch> accepted_epoch{0};

    // Both keepers of a read must be inside FetchHot at once for every call to see its peer.
    struct Rendezvous
    {
        std::mutex mu;
        std::condition_variable cv;
        unsigned arrived{0};
    };
    std::shared_ptr<Rendezvous> rendezvous;
    std::atomic<unsigned> met{0};

    grpc::Status FetchHot(grpc::ServerContext* context,
                          const iv1::FetchHotRequest* request,
                          grpc::ServerWriter<iv1::FetchHotResponse>* writer) override
    {
        ++calls;
        {
            std::lock_guard lk(mu_);
            ++seen_;
        }
        called_.notify_all();
        if(rendezvous)
        {
            std::unique_lock lk(rendezvous->mu);
            ++rendezvous->arrived;
            rendezvous->cv.notify_all();
            if(rendezvous->cv.wait_for(lk, 5s, [&] { return rendezvous->arrived >= 2; }))
                ++met;
        }
        unsigned remaining = unavailable.load();
        while(remaining && !unavailable.compare_exchange_weak(remaining, remaining - 1)) {}
        if(remaining)
            return grpc::Status(grpc::StatusCode::UNAVAILABLE, "snapshot not applied");
        std::vector<v1::Event> events;
        int64_t sealed;
        uint64_t epoch;
        bool truncated;
        bool refused;
        std::string instance;
        std::chrono::milliseconds delay;
        {
            std::lock_guard lk(mu_);
            refused = refused_;
            instance = instance_;
            events = events_;
            sealed = sealed_;
            epoch = epoch_;
            truncated = truncated_;
            delay = delay_;
        }
        if(enforce_epoch && request->expect_epoch() != epoch)
        {
            ++stale_epochs;
            return {grpc::StatusCode::FAILED_PRECONDITION, "stale epoch"};
        }
        accepted_epoch = request->expect_epoch();
        if(refused)
            return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "story was destroyed");
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
            if(in && request->has_physical_filter())
                in = e.physical().physical_ns() >= request->physical_filter().start_ns() &&
                     e.physical().physical_ns() < request->physical_filter().end_ns();
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
        trailer.mutable_trailer()->set_instance(instance);
        trailer.mutable_trailer()->mutable_sealed_frontier()->set_physical_ns(sealed);
        trailer.mutable_trailer()->set_truncated(truncated);
        writer->Write(trailer);
        return grpc::Status::OK;
    }

private:
    std::mutex mu_;
    std::condition_variable called_;
    unsigned seen_{};
    bool refused_{};
    std::string instance_;
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
        std::lock_guard lk(mu);
        ++lookups;
        auto it = stories.find(story);
        if(it == stories.end())
            return absl::FailedPreconditionError("unknown story");
        if(it->second)
            return absl::FailedPreconditionError("story is tombstoned");
        return absl::OkStatus();
    }
    void tombstone(StoryId story)
    {
        std::lock_guard lk(mu);
        stories[story] = true;
    }
    unsigned asked() const
    {
        std::lock_guard lk(mu);
        return lookups;
    }
    mutable std::mutex mu;
    mutable unsigned lookups{};
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
        tune(options);
        source_ = std::make_shared<KeeperHotSource>(
                std::make_shared<StaticRouteSource>(route),
                std::make_shared<FakeWriters>(),
                [this](const KeeperRef& k)
                {
                    const auto& address = k.process_id == "keeper-a" ? a_addr_ : b_addr_;
                    return "localhost" + address.substr(address.find(':'));
                },
                options);
        HotReplayOptions replay_options;
        replay_options.batch_size = 2;
        replay_options.tail_poll = 20ms;
        replay_options.story_live = [catalog = catalog_](StoryId story) { return catalog->ensureLive(story); };
        service_ = std::make_unique<ReplayService>(std::make_shared<HotReplay>(source_, replay_options), catalog_);
        grpc::ServerBuilder builder;
        chronolog::rpc::applyServerPolicy(builder);
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
        chronolog::rpc::applyServerPolicy(builder);
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

    virtual void tune(KeeperHotSourceOptions&) {}

    FakeArchive a_, b_;
    std::shared_ptr<FakeCatalog> catalog_ = std::make_shared<FakeCatalog>();
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

TEST_F(replay_adapter, CachedChannelRecoversAfterKeeperRestart)
{
    auto before = read(hlcRead(100, 200));
    ASSERT_EQ(before.completions.size(), 1u);
    ASSERT_TRUE(before.completions[0].complete());
    b_server_->Shutdown(std::chrono::system_clock::now());
    b_server_.reset();
    auto down = read(hlcRead(100, 200));
    ASSERT_EQ(down.completions.size(), 1u);
    EXPECT_EQ(down.completions[0].reason(), v1::INCOMPLETE_REASON_SOURCE_FAILED);
    FakeArchive restarted;
    for(auto time: {120, 140, 160}) restarted.add(protoEvent(4, (time - 100) / 20, time));
    grpc::ServerBuilder builder;
    chronolog::rpc::applyServerPolicy(builder);
    builder.AddListeningPort(b_addr_, grpc::InsecureServerCredentials());
    builder.RegisterService(&restarted);
    b_server_ = builder.BuildAndStart();
    ASSERT_TRUE(b_server_);
    auto after = read(hlcRead(100, 200));
    EXPECT_EQ(after.events.size(), 6u);
    EXPECT_EQ(after.completions.size(), 1u);
    if(!after.completions.empty())
    {
        EXPECT_TRUE(after.completions[0].complete());
    }
    b_server_->Shutdown(std::chrono::system_clock::now() + 1s);
    b_server_.reset();
}

TEST_F(replay_adapter, KeeperReadinessIsRetriedWithinDeadline)
{
    b_.unavailable = 2;
    auto r = read(hlcRead(100, 200));
    ASSERT_TRUE(r.status.ok());
    ASSERT_EQ(r.completions.size(), 1u);
    EXPECT_TRUE(r.completions[0].complete());
    EXPECT_EQ(r.events.size(), 6u);
    EXPECT_EQ(b_.calls.load(), 3u);
}

TEST_F(replay_adapter, KeeperReadinessRetriesStopAtDeadline)
{
    b_.unavailable = 1000;
    const auto start = std::chrono::steady_clock::now();
    auto r = read(hlcRead(100, 200));
    EXPECT_LT(std::chrono::steady_clock::now() - start, 2s);
    ASSERT_EQ(r.completions.size(), 1u);
    EXPECT_EQ(r.completions[0].reason(), v1::INCOMPLETE_REASON_SOURCE_FAILED);
    EXPECT_GE(b_.calls.load(), 2u);
    EXPECT_LT(b_.calls.load(), 20u);
}

TEST_F(replay_adapter, EveryKeeperOfAReadIsFetchedConcurrently)
{
    auto rendezvous = std::make_shared<FakeArchive::Rendezvous>();
    a_.rendezvous = rendezvous;
    b_.rendezvous = rendezvous;
    auto result = read(hlcRead(100, 300));
    EXPECT_TRUE(result.status.ok());
    EXPECT_GE(a_.met.load(), 1u) << "keeper a never saw keeper b in flight, so the fetches ran one after another";
    EXPECT_GE(b_.met.load(), 1u);
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

TEST_F(replay_adapter, HotFetchSharesOneRetentionBudgetAcrossKeepers)
{
    Route route{7, {{"keeper-a", "keeper-a"}, {"keeper-b", "keeper-b"}}, "", ""};
    KeeperHotSourceOptions options;
    options.deadline = 500ms;
    options.read_max_events = 4;
    KeeperHotSource source(
            std::make_shared<StaticRouteSource>(route),
            std::make_shared<FakeWriters>(),
            [this](const KeeperRef& k)
            {
                const auto& address = k.process_id == "keeper-a" ? a_addr_ : b_addr_;
                return "localhost" + address.substr(address.find(':'));
            },
            options);
    auto fetched = source.fetch(kStory, {Range::Axis::Hlc, {100, 0}, {200, 0}});
    ASSERT_TRUE(fetched.ok());
    size_t total = 0;
    bool truncated = false;
    for(const auto& keeper: fetched->keepers)
    {
        EXPECT_TRUE(keeper.frontier.answered);
        total += keeper.events.size();
        truncated |= keeper.frontier.truncated;
    }
    EXPECT_EQ(total, 4);
    EXPECT_TRUE(truncated);
}

TEST_F(replay_adapter, PhysicalReadAboveTheLimitIsTruncatedWithoutClaim)
{
    HotReplayOptions options;
    options.read_max_events = 2;
    HotReplay replay(source_, options);
    auto stream = replay.read(kStory, {Range::Axis::Physical, {100, 0}, {200, 0}});
    ASSERT_TRUE(stream.ok());
    size_t count = 0;
    std::optional<Completion> completion;
    for(size_t i = 0; i < 20; ++i)
    {
        auto batch = (*stream)->next();
        ASSERT_TRUE(batch.ok());
        if(!*batch)
            break;
        count += (**batch).events.size();
        if((**batch).completion)
            completion = (**batch).completion;
    }
    EXPECT_LE(count, 2);
    ASSERT_TRUE(completion);
    EXPECT_FALSE(completion->complete);
    EXPECT_EQ(completion->reason, IncompleteReason::Truncated);
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

#include "../../../tests/contract/replay/replay_adapter_test.cpp"

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
    // A Keeper never shows an event below the seal it reported, so the new event lies above it and the seals move on.
    a_.add(protoEvent(2, 4, 210));
    a_.seal(300);
    b_.seal(300);
    pump(4);
    EXPECT_EQ(seen, (std::vector<int64_t>{140, 150, 160, 210}));
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

// A Keeper that refuses a destroyed story is evidence, not proof: only the Catalog ends the Tail (I6.13).
TEST_F(replay_adapter, TailEndsFailedPreconditionWhenAKeeperRefusesAndTheCatalogConfirms)
{
    auto ctx = context();
    auto reader = stub_->Tail(ctx.get(), tailFrom(protoEvent(2, 2, 130)));
    v1::TailResponse response;
    size_t seen = 0;
    for(int i = 0; i < 100 && seen < 3 && reader->Read(&response); ++i) seen += response.batch().events_size();
    ASSERT_EQ(seen, 3u);
    catalog_->tombstone(kStory);
    a_.refuse(true);
    b_.refuse(true);
    while(reader->Read(&response)) {}
    EXPECT_EQ(reader->Finish().error_code(), grpc::StatusCode::FAILED_PRECONDITION);
}

TEST_F(replay_adapter, TailStallsWhileTheCatalogDoesNotConfirmAKeeperRefusal)
{
    auto ctx = context();
    auto reader = stub_->Tail(ctx.get(), tailFrom(protoEvent(2, 2, 130)));
    v1::TailResponse response;
    size_t seen = 0;
    for(int i = 0; i < 100 && seen < 3 && reader->Read(&response); ++i) seen += response.batch().events_size();
    ASSERT_EQ(seen, 3u);
    a_.refuse(true);
    b_.refuse(true);
    ASSERT_TRUE(a_.waitCalls(a_.calls.load() + 5));
    auto more = std::async(std::launch::async, [&] { return reader->Read(&response); });
    EXPECT_GE(catalog_->asked(), 2u);
    a_.refuse(false);
    b_.refuse(false);
    a_.add(protoEvent(2, 4, 210));
    a_.seal(300);
    b_.seal(300);
    ASSERT_EQ(more.wait_for(10s), std::future_status::ready);
    ASSERT_TRUE(more.get());
    ASSERT_EQ(response.batch().events_size(), 1);
    EXPECT_EQ(response.batch().events(0).hlc().physical_ns(), 210);
    catalog_->tombstone(kStory);
    a_.refuse(true);
    b_.refuse(true);
    more = std::async(std::launch::async, [&] { return reader->Read(&response); });
    ASSERT_EQ(more.wait_for(10s), std::future_status::ready);
    EXPECT_FALSE(more.get());
    EXPECT_EQ(reader->Finish().error_code(), grpc::StatusCode::FAILED_PRECONDITION);
}

TEST_F(replay_adapter, TailKeepsStallingWhileAKeeperIsDown)
{
    auto ctx = context();
    auto reader = stub_->Tail(ctx.get(), tailFrom(protoEvent(2, 2, 130)));
    v1::TailResponse response;
    size_t seen = 0;
    for(int i = 0; i < 100 && seen < 3 && reader->Read(&response); ++i) seen += response.batch().events_size();
    ASSERT_EQ(seen, 3u);
    const unsigned asked = catalog_->asked();
    a_.unavailable = 1000000;
    ASSERT_TRUE(a_.waitCalls(a_.calls.load() + 5));
    auto more = std::async(std::launch::async, [&] { return reader->Read(&response); });
    // No Keeper refused the story, so there is nothing for the Catalog to confirm.
    EXPECT_EQ(catalog_->asked(), asked);
    ctx->TryCancel();
    EXPECT_FALSE(more.get());
    EXPECT_EQ(reader->Finish().error_code(), grpc::StatusCode::CANCELLED);
}

TEST_F(replay_adapter, TailStopsFetchingASourceWhoseBufferIsFull)
{
    b_.refuse(true);
    HotReplayOptions options;
    options.read_max_events = 2;
    options.tail_poll = 1ms;
    HotReplay replay(source_, options);
    Event start;
    start.id.story_id = kStory;
    auto stream = replay.tail(kStory, start);
    ASSERT_TRUE(stream.ok());
    const auto calls = a_.calls.load();
    auto next = std::async(std::launch::async, [&] { return (*stream)->next(); });
    const bool polled = b_.waitCalls(b_.calls.load() + 5);
    const auto after = a_.calls.load();
    (*stream)->cancel();
    (void)next.get();
    ASSERT_TRUE(polled);
    EXPECT_EQ(after, calls);
}

TEST_F(replay_adapter, TailBoundsBufferedPayloadAndResumesAfterDelivery)
{
    a_.hold("a1", {}, 300);
    for(int64_t t: {110, 130, 150})
    {
        auto e = protoEvent(2, t, t);
        e.mutable_envelope()->set_payload(std::string(12, 'x'));
        a_.add(std::move(e));
    }
    b_.hold("b1", {}, 100);
    HotReplayOptions options;
    options.tail_max_bytes = 48;
    options.batch_size = 10;
    options.tail_poll = 1ms;
    HotReplay replay(source_, options);
    Event start;
    start.id.story_id = kStory;
    auto stream = replay.tail(kStory, start);
    ASSERT_TRUE(stream.ok());
    const auto before = a_.calls.load();
    auto next = std::async(std::launch::async, [&] { return (*stream)->next(); });
    const bool polled = b_.waitCalls(b_.calls.load() + 5);
    const auto after = a_.calls.load();
    b_.seal(300);
    if(next.wait_for(5s) != std::future_status::ready)
        (*stream)->cancel();
    auto first = next.get();
    ASSERT_TRUE(polled);
    EXPECT_EQ(after, before);
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(*first);
    ASSERT_EQ((**first).events.size(), 2u);
    EXPECT_EQ((**first).events[0].hlc.physical_ns, 110);
    EXPECT_EQ((**first).events[1].hlc.physical_ns, 130);
    auto last = std::async(std::launch::async, [&] { return (*stream)->next(); });
    if(last.wait_for(5s) != std::future_status::ready)
        (*stream)->cancel();
    auto batch = last.get();
    ASSERT_TRUE(batch.ok());
    ASSERT_TRUE(*batch);
    ASSERT_EQ((**batch).events.size(), 1u);
    EXPECT_EQ((**batch).events[0].hlc.physical_ns, 150);
    (*stream)->cancel();
}

TEST_F(replay_adapter, TailDeliversAnEventLargerThanItsByteShare)
{
    a_.hold("a1", {}, 300);
    for(int64_t time: {110, 130})
    {
        auto e = protoEvent(2, time, time);
        e.mutable_envelope()->set_payload(std::string(100, 'x'));
        a_.add(std::move(e));
    }
    b_.hold("b1", {}, 100);
    HotReplayOptions options;
    options.tail_max_bytes = 96;
    options.batch_size = 10;
    options.tail_poll = 1ms;
    HotReplay replay(source_, options);
    Event start;
    start.id.story_id = kStory;
    auto stream = replay.tail(kStory, start);
    ASSERT_TRUE(stream.ok());
    const auto before = a_.calls.load();
    auto next = std::async(std::launch::async, [&] { return (*stream)->next(); });
    const bool polled = b_.waitCalls(b_.calls.load() + 5);
    const auto after = a_.calls.load();
    b_.seal(300);
    if(next.wait_for(5s) != std::future_status::ready)
        (*stream)->cancel();
    auto batch = next.get();
    ASSERT_TRUE(polled);
    EXPECT_EQ(after, before);
    ASSERT_TRUE(batch.ok());
    ASSERT_TRUE(*batch);
    ASSERT_EQ((**batch).events.size(), 1u);
    EXPECT_EQ((**batch).events[0].hlc, (Hlc{110, 0}));
    EXPECT_EQ((**batch).events[0].envelope.payload, std::string(100, 'x'));
    EXPECT_FALSE((**batch).completion);
    next = std::async(std::launch::async, [&] { return (*stream)->next(); });
    if(next.wait_for(5s) != std::future_status::ready)
        (*stream)->cancel();
    batch = next.get();
    ASSERT_TRUE(batch.ok());
    ASSERT_TRUE(*batch);
    ASSERT_EQ((**batch).events.size(), 1u);
    EXPECT_EQ((**batch).events[0].hlc, (Hlc{130, 0}));
    EXPECT_EQ((**batch).events[0].envelope.payload, std::string(100, 'x'));
    EXPECT_FALSE((**batch).completion);
    EXPECT_GT(a_.calls.load(), before);
    (*stream)->cancel();
}

TEST_F(replay_adapter, TailAcrossAKeeperRestartHasNoGapOrDuplicate)
{
    a_.setInstance("a1");
    b_.hold("b1", {protoEvent(4, 1, 120)}, 125);
    auto ctx = context();
    auto reader = stub_->Tail(ctx.get(), tailFrom(protoEvent(2, 0, 100)));
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
    // keeper-b's seal holds the Tail at 125 while keeper-a's later events wait in the Player.
    pump(2);
    EXPECT_EQ(seen, (std::vector<int64_t>{110, 120}));
    // keeper-b shows an event below the ones keeper-a showed, and keeper-a restarts with its DURABLE events recovered.
    b_.hold("b1", {protoEvent(4, 1, 120), protoEvent(4, 2, 135), protoEvent(4, 3, 160)}, 300);
    a_.hold("a2", {protoEvent(2, 2, 130), protoEvent(2, 3, 150), protoEvent(2, 4, 210)}, 300);
    pump(7);
    EXPECT_EQ(seen, (std::vector<int64_t>{110, 120, 130, 135, 150, 160, 210}));
    ctx->TryCancel();
    while(reader->Read(&response)) {}
}

class replay_adapter_small_budget: public replay_adapter
{
    void tune(KeeperHotSourceOptions& options) override { options.read_max_events = 3; }
};

// With one budget shared by both Keepers the second would answer nothing and the Tail would stand still.
TEST_F(replay_adapter_small_budget, TailMakesProgressWhenTheBudgetIsSmallerThanTheBacklog)
{
    for(int64_t at: {170, 190}) a_.add(protoEvent(2, at, at));
    for(int64_t at: {180, 200}) b_.add(protoEvent(4, at, at));
    auto ctx = context();
    auto reader = stub_->Tail(ctx.get(), tailFrom(protoEvent(2, 0, 100)));
    std::vector<int64_t> seen;
    v1::TailResponse response;
    for(int i = 0; i < 100 && seen.size() < 9 && reader->Read(&response); ++i)
        for(const auto& e: response.batch().events()) seen.push_back(e.hlc().physical_ns());
    EXPECT_EQ(seen, (std::vector<int64_t>{110, 120, 130, 140, 150, 160, 170, 180, 190}));
    ctx->TryCancel();
    while(reader->Read(&response)) {}
}

TEST_F(replay_adapter, CallsAfterShutdownAreUnavailable)
{
    service_->shutdown();
    EXPECT_EQ(read(hlcRead(100, 200)).status.error_code(), grpc::StatusCode::UNAVAILABLE);
}

} // namespace
} // namespace chronolog::player

namespace chronolog::player
{
TEST_F(replay_adapter, ReadRecoversCorrectEpochAfterKeeperRefusesSupersededRoute)
{
    class ChangedRoute final: public RouteSource
    {
    public:
        explicit ChangedRoute(std::atomic<unsigned>& refused)
            : refused_(refused)
        {}
        absl::StatusOr<Route> route(StoryId) const override { return Route{7, {{"keeper-a", "keeper-a"}}, "", ""}; }
        absl::StatusOr<RouteState>
        routeStateAfter(StoryId, Epoch epoch, std::chrono::system_clock::time_point) const override
        {
            EXPECT_EQ(epoch, 7);
            EXPECT_GT(refused_.load(), 0u);
            RouteState state;
            state.route = Route{8, {{"keeper-a", "keeper-a"}}, "", ""};
            return state;
        }

    private:
        std::atomic<unsigned>& refused_;
    };
    a_.setEpoch(8);
    a_.enforce_epoch = true;
    auto routes = std::make_shared<ChangedRoute>(a_.stale_epochs);
    auto source = std::make_shared<KeeperHotSource>(routes, nullptr, [this](const KeeperRef&) { return a_addr_; });
    ReplayService service(std::make_shared<HotReplay>(source), catalog_);
    grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    auto server = builder.BuildAndStart();
    ASSERT_NE(server, nullptr);
    auto stub = v1::Replay::NewStub(rpc::peerChannel("127.0.0.1:" + std::to_string(port)));
    auto ctx = context();
    auto reader = stub->Read(ctx.get(), hlcRead(100, 200));
    v1::ReadResponse response;
    bool complete = false;
    size_t events = 0;
    while(reader->Read(&response))
    {
        events += response.batch().events_size();
        if(response.has_completion())
            complete = response.completion().complete();
    }
    EXPECT_TRUE(reader->Finish().ok());
    EXPECT_TRUE(complete);
    EXPECT_EQ(events, 3u);
    EXPECT_EQ(a_.stale_epochs, 1u);
    EXPECT_EQ(a_.accepted_epoch, 8u);
    service.shutdown();
    server->Shutdown(std::chrono::system_clock::now() + 2s);
}
} // namespace chronolog::player
