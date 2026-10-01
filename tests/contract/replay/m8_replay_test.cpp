#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>
#include <filesystem>
#include <limits>
#include <mutex>
#include <unistd.h>
#include "chrono-player/adapter/Convert.h"
#include "chrono-player/replay/HotReplay.h"
#include "chrono-player/replay/KeeperHotSource.h"

namespace chronolog::player
{
namespace
{
namespace wire = internal::v1;
Event event(int64_t time, uint64_t writer = 2)
{
    Event e;
    e.id = {1, writer, 1, static_cast<uint64_t>(time)};
    e.hlc = {time, 0};
    e.physical = {time, 0, ClockStatus::Synced};
    e.durability = Durability::Durable;
    return e;
}
class Routes final: public RouteSource
{
public:
    RouteState state;
    bool policy{};
    absl::StatusOr<Route> route(StoryId) const override { return state.route; }
    absl::StatusOr<RouteState> routeState(StoryId) const override { return state; }
    bool physicalPolicy(StoryId) const override { return policy; }
    int64_t skewLimitNs() const override { return 10; }
};
class KeeperDriver final: public wire::Archive::Service
{
public:
    Epoch epoch{2};
    std::string instance{"current"};
    Hlc seal{1000, 0};
    int64_t physical{1000};
    bool failed{}, truncated{}, lie{};
    std::vector<Event> events;
    std::mutex mutex;
    std::vector<wire::FetchHotRequest> requests;
    grpc::Status FetchHot(grpc::ServerContext*,
                          const wire::FetchHotRequest* q,
                          grpc::ServerWriter<wire::FetchHotResponse>* writer) override
    {
        std::lock_guard lock(mutex);
        requests.push_back(*q);
        if(failed)
            return {grpc::StatusCode::UNAVAILABLE, "failed source"};
        if(!lie && ((!q->expect_instance().empty() && q->expect_instance() != instance) || q->expect_epoch() != epoch))
            return {grpc::StatusCode::FAILED_PRECONDITION, "different owner"};
        wire::FetchHotResponse batch;
        for(const auto& e: events)
        {
            bool match = q->has_hlc() ? e.hlc >= convert::fromProto(q->hlc().start()) &&
                                                e.hlc < convert::fromProto(q->hlc().end())
                                      : true;
            if(match)
                *batch.mutable_batch()->add_events() = convert::toProto(e);
        }
        if(batch.has_batch() && !writer->Write(batch))
            return grpc::Status::CANCELLED;
        wire::FetchHotResponse end;
        auto* trailer = end.mutable_trailer();
        trailer->set_epoch(epoch);
        trailer->set_instance(instance);
        *trailer->mutable_sealed_frontier() = convert::toProto(seal);
        trailer->set_physical_frontier_ns(physical);
        trailer->set_truncated(truncated);
        writer->Write(end);
        return grpc::Status::OK;
    }
};
class ReplayContract: public ::testing::Test
{
protected:
    KeeperDriver current, old;
    std::unique_ptr<grpc::Server> current_server, old_server;
    std::shared_ptr<Routes> routes = std::make_shared<Routes>();
    std::shared_ptr<KeeperHotSource> source;
    std::unique_ptr<FileTierStore> archive_writer;
    HotReplayOptions options;
    std::filesystem::path root;
    std::vector<Event> returned;
    Completion completion;
    std::string start(KeeperDriver& driver, std::unique_ptr<grpc::Server>& server)
    {
        grpc::ServerBuilder builder;
        int port = 0;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(&driver);
        server = builder.BuildAndStart();
        if(!server)
            throw std::runtime_error("server failed");
        return "127.0.0.1:" + std::to_string(port);
    }
    void SetUp() override
    {
        auto endpoint = start(current, current_server);
        auto predecessor_endpoint = start(old, old_server);
        routes->state.route = {2, {{"current", endpoint}}, "", ""};
        routes->state.ordering_cut = {900, 0};
        routes->state.predecessors = {{{"old", predecessor_endpoint}, "old-instance", 1, {200, 0}, 300}};
        old.epoch = 1;
        old.instance = "old-instance";
        old.seal = {200, 0};
        old.events = {event(140)};
        current.events = {event(240, 4)};
        source = std::make_shared<KeeperHotSource>(
                routes,
                nullptr,
                [](const KeeperRef& k) { return k.endpoint; },
                KeeperHotSourceOptions{std::chrono::milliseconds(500), 0, 100});
        char pattern[] = "/tmp/chronolog-replay-m8-XXXXXX";
        root = ::mkdtemp(pattern);
        auto opened = FileTierStore::Open(root, "writer", {{1, {100, 0}}});
        ASSERT_TRUE(opened.ok()) << opened.status();
        archive_writer = *std::move(opened);
        auto reader = FileTierStore::OpenReadOnly(root, std::chrono::hours(1));
        ASSERT_TRUE(reader.ok()) << reader.status();
        options.archive = std::shared_ptr<FileTierStore>(*std::move(reader));
        options.batch_size = 2;
    }
    void TearDown() override
    {
        source.reset();
        current_server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(1));
        old_server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(1));
        options.archive.reset();
        archive_writer.reset();
        std::filesystem::remove_all(root);
    }
    void read(Range range = {Range::Axis::Hlc, {100, 0}, {300, 0}})
    {
        HotReplay replay(source, options);
        auto stream = replay.read(1, range);
        ASSERT_TRUE(stream.ok()) << stream.status();
        returned.clear();
        int completions = 0;
        for(int n = 0; n < 100; ++n)
        {
            auto b = (*stream)->next();
            ASSERT_TRUE(b.ok()) << b.status();
            if(!*b)
            {
                EXPECT_EQ(completions, 1);
                return;
            }
            EXPECT_EQ(completions, 0);
            returned.insert(returned.end(), (**b).events.begin(), (**b).events.end());
            if((**b).completion)
            {
                completion = *(**b).completion;
                ++completions;
            }
        }
        FAIL() << "unbounded stream";
    }
};
TEST_F(ReplayContract, UndrainedPredecessorIsASource)
{
    read();
    EXPECT_TRUE(completion.complete);
    ASSERT_EQ(returned.size(), 2);
    EXPECT_EQ(returned[0].hlc, (Hlc{140, 0}));
    ASSERT_EQ(old.requests.size(), 1);
    EXPECT_EQ(old.requests[0].expect_instance(), "old-instance");
    EXPECT_EQ(old.requests[0].expect_epoch(), 1);
    EXPECT_EQ(convert::fromProto(old.requests[0].hlc().end()), (Hlc{200, 0}));
}
TEST_F(ReplayContract, DeadPredecessorIsSourceFailed)
{
    old.failed = true;
    read();
    EXPECT_FALSE(completion.complete);
    EXPECT_EQ(completion.reason, IncompleteReason::SourceFailed);
}
TEST_F(ReplayContract, ReplacementInstanceCannotAnswerForItsPredecessor)
{
    old.instance = "replacement";
    old.lie = true;
    read();
    EXPECT_FALSE(completion.complete);
    EXPECT_EQ(completion.reason, IncompleteReason::SourceFailed);
}
TEST_F(ReplayContract, TrailerEpochMismatchIsSourceFailed)
{
    current.epoch = 3;
    current.lie = true;
    read();
    EXPECT_EQ(completion.reason, IncompleteReason::SourceFailed);
}
TEST_F(ReplayContract, DrainedPredecessorHistoryComesFromTheArchive)
{
    ASSERT_TRUE(archive_writer->publish({"old", 1, {100, 0}, {200, 0}, old.events, false}).ok());
    routes->state.predecessors.clear();
    routes->state.archived_below = {200, 0};
    read();
    EXPECT_TRUE(completion.complete);
    ASSERT_EQ(returned.size(), 2);
    EXPECT_EQ(returned[0].hlc, (Hlc{140, 0}));
    EXPECT_TRUE(old.requests.empty());
}
TEST_F(ReplayContract, TruncatedReadFrontierIsACompletePrefix)
{
    old.events = {event(120), event(180)};
    old.truncated = true;
    old.seal = {160, 0};
    read();
    EXPECT_EQ(completion.reason, IncompleteReason::Truncated);
    EXPECT_EQ(completion.frontier, (Hlc{160, 0}));
    ASSERT_EQ(returned.size(), 1);
    EXPECT_LT(returned[0].hlc, completion.frontier);
    std::vector<Event> first = returned;
    old.truncated = false;
    old.seal = {200, 0};
    read({Range::Axis::Hlc, completion.frontier, {300, 0}});
    EXPECT_TRUE(completion.complete);
    first.insert(first.end(), returned.begin(), returned.end());
    ASSERT_EQ(first.size(), 3);
    EXPECT_EQ(first[0].hlc, (Hlc{120, 0}));
    EXPECT_EQ(first[1].hlc, (Hlc{180, 0}));
    EXPECT_EQ(first[2].hlc, (Hlc{240, 0}));
}
TEST_F(ReplayContract, OwnCutControlsCoverageAndSourceSelection)
{
    old.seal = {190, 0};
    read();
    EXPECT_EQ(completion.reason, IncompleteReason::LaggingWriters);
    old.requests.clear();
    read({Range::Axis::Hlc, {200, 0}, {300, 0}});
    EXPECT_TRUE(completion.complete);
    EXPECT_TRUE(old.requests.empty());
}
TEST_F(ReplayContract, AbandonedRangesStaySourceFailed)
{
    routes->state.predecessors.clear();
    routes->state.abandoned = {{Range::Axis::Hlc, {150, 0}, {200, 0}}};
    read();
    EXPECT_EQ(completion.reason, IncompleteReason::SourceFailed);
    read({Range::Axis::Hlc, {200, 0}, {300, 0}});
    EXPECT_TRUE(completion.complete);
    read({Range::Axis::Physical, {400, 0}, {500, 0}});
    EXPECT_EQ(completion.reason, IncompleteReason::SourceFailed);
}
TEST_F(ReplayContract, PhysicalPredecessorSpanIncludesPhysicalCeilingAndSkew)
{
    old.events.clear();
    read({Range::Axis::Physical, {305, 0}, {320, 0}});
    ASSERT_EQ(old.requests.size(), 1);
    EXPECT_TRUE(old.requests[0].has_physical());
    old.requests.clear();
    read({Range::Axis::Physical, {310, 0}, {320, 0}});
    EXPECT_TRUE(old.requests.empty());
    routes->state.predecessors[0].own_physical_ceiling_ns = std::numeric_limits<int64_t>::max() - 3;
    read({Range::Axis::Physical,
          {std::numeric_limits<int64_t>::max() - 2, 0},
          {std::numeric_limits<int64_t>::max(), 0}});
    EXPECT_EQ(old.requests.size(), 1);
}
TEST_F(ReplayContract, PredecessorPhysicalFrontierMustCoverEnd)
{
    routes->policy = true;
    ASSERT_TRUE(archive_writer->publish({"empty", 1, {100, 0}, {1000, 0}, {}, true}).ok());
    old.events.clear();
    old.physical = 309;
    read({Range::Axis::Physical, {305, 0}, {320, 0}});
    EXPECT_EQ(completion.reason, IncompleteReason::LaggingWriters);
    old.physical = 320;
    read({Range::Axis::Physical, {305, 0}, {320, 0}});
    EXPECT_TRUE(completion.complete);
}
TEST_F(ReplayContract, TruncatedPrefixAlsoRespectsCurrentSealAndArchiveRecordStart)
{
    old.events = {event(120), event(180)};
    old.truncated = true;
    old.seal = {190, 0};
    current.seal = {175, 0};
    routes->state.archived_below = {200, 0};
    ASSERT_TRUE(archive_writer->publish({"first", 1, {100, 0}, {150, 0}, {event(120)}, false}).ok());
    ASSERT_TRUE(archive_writer->publish({"second", 1, {150, 0}, {200, 0}, {event(160)}, false}).ok());
    read();
    EXPECT_EQ(completion.reason, IncompleteReason::Truncated);
    EXPECT_EQ(completion.frontier, (Hlc{150, 0}));
    ASSERT_EQ(returned.size(), 1);
    EXPECT_EQ(returned[0].hlc, (Hlc{120, 0}));
}
TEST_F(ReplayContract, LostArchiveWindowCannotLieBelowTruncatedPrefix)
{
    old.events = {event(120), event(180)};
    old.truncated = true;
    routes->state.archived_below = {200, 0};
    ASSERT_TRUE(archive_writer->publish({"first", 1, {100, 0}, {150, 0}, {event(120)}, false}).ok());
    ASSERT_TRUE(archive_writer->publish({"lost", 1, {150, 0}, {200, 0}, {event(160)}, false}).ok());
    auto records = archive_writer->manifest(1);
    ASSERT_TRUE(records.ok());
    for(const auto& r: *records)
        if(r.start == Hlc{150, 0})
            std::filesystem::remove(root / r.file);
    archive_writer.reset();
    auto recovered = FileTierStore::Open(root, "writer", {{1, {100, 0}}});
    ASSERT_TRUE(recovered.ok());
    archive_writer = *std::move(recovered);
    read();
    EXPECT_EQ(completion.reason, IncompleteReason::Truncated);
    EXPECT_EQ(completion.frontier, (Hlc{150, 0}));
    ASSERT_EQ(returned.size(), 1);
    EXPECT_EQ(returned[0].hlc, (Hlc{120, 0}));
}
TEST_F(ReplayContract, PredecessorOwnedEpochMismatchIsSourceFailed)
{
    old.epoch = 2;
    old.lie = true;
    read();
    EXPECT_EQ(completion.reason, IncompleteReason::SourceFailed);
}
TEST_F(ReplayContract, TruncatedEmptyPredecessorHasNoCompletePrefix)
{
    old.events.clear();
    old.truncated = true;
    read();
    EXPECT_EQ(completion.reason, IncompleteReason::Truncated);
    EXPECT_EQ(completion.frontier, (Hlc{100, 0}));
    EXPECT_TRUE(returned.empty());
}
} // namespace
} // namespace chronolog::player
