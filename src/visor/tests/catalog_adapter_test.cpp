// CatalogService end to end through a real in-process gRPC channel over the
// in-memory store.
#include <gtest/gtest.h>

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <memory>
#include <optional>
#include <set>
#include <thread>
#include <vector>

#include "visor/tests/TestSupport.h"
#include "visor/adapter/CatalogService.h"
#include "common/worker/WorkerPool.h"
#include "visor/catalog/InMemoryMetadataStore.h"
#include "visor/catalog/LeaseAuthority.h"
#include "visor/catalog/SqliteMetadataStore.h"
#include "chronolog/v1/chronolog.grpc.pb.h"
#include "visor/membership/StaticRouteMembership.h"

namespace chronolog::visor
{
namespace
{

using namespace std::chrono_literals;
using testing::twoKeeperTopology;

constexpr int kInvalidArgument = 3;
constexpr int kNotFound = 5;
constexpr int kAlreadyExists = 6;
constexpr int kFailedPrecondition = 9;

class catalog_adapter: public ::testing::Test
{
protected:
    void SetUp() override
    {
        membership_ =
                std::make_unique<StaticRouteMembership>(twoKeeperTopology(), 1, [](StoryId) { return true; }, 15s);
        // Fence timeout 50 ms keeps the unconfirmed case fast.
        store_ = std::make_unique<InMemoryMetadataStore>(
                twoKeeperTopology(),
                [this](const KeeperRef& keeper, uint64_t revision)
                { return membership_->waitApplied(keeper.process_id, revision, 50ms); });
        pool_ = std::make_unique<WorkerPool>(2, 64);
        service_ = std::make_unique<CatalogService>(*store_, *pool_, nullptr, membership_.get());

        grpc::ServerBuilder builder;
        int port = 0;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(service_.get());
        server_ = builder.BuildAndStart();
        ASSERT_NE(server_, nullptr);
        ASSERT_NE(port, 0);
        stub_ = v1::Catalog::NewStub(
                grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials()));
    }

    void TearDown() override
    {
        if(server_)
            server_->Shutdown(std::chrono::system_clock::now() + 2s);
    }

    template <class Rpc, class Request, class Response>
    grpc::Status call(Rpc rpc, const Request& request, Response* response)
    {
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + 10s);
        return (stub_.get()->*rpc)(&context, request, response);
    }

    StoryId makeStory()
    {
        v1::CreateChronicleRequest chronicle;
        chronicle.set_name("c");
        v1::CreateChronicleResponse chronicle_response;
        EXPECT_TRUE(call(&v1::Catalog::Stub::CreateChronicle, chronicle, &chronicle_response).ok());
        v1::CreateStoryRequest story;
        story.set_chronicle("c");
        story.set_name("s");
        v1::CreateStoryResponse story_response;
        EXPECT_TRUE(call(&v1::Catalog::Stub::CreateStory, story, &story_response).ok());
        EXPECT_EQ(story_response.status().code(), 0);
        return story_response.story().story_id();
    }

    v1::AcquireResponse acquire(StoryId story, const std::string& identity)
    {
        v1::AcquireRequest request;
        request.set_story_id(story);
        request.set_writer_identity(identity);
        request.set_acquire_request_id(newAcquireRequestId());
        v1::AcquireResponse response;
        EXPECT_TRUE(call(&v1::Catalog::Stub::Acquire, request, &response).ok());
        return response;
    }

    v1::ReleaseResponse release(const v1::AcquireResponse& acquired)
    {
        v1::ReleaseRequest request;
        request.set_story_id(acquired.story_id());
        request.set_writer_id(acquired.writer_id());
        request.set_incarnation(acquired.incarnation());
        v1::ReleaseResponse response;
        EXPECT_TRUE(call(&v1::Catalog::Stub::Release, request, &response).ok());
        return response;
    }

    std::unique_ptr<StaticRouteMembership> membership_;
    std::unique_ptr<InMemoryMetadataStore> store_;
    std::unique_ptr<WorkerPool> pool_;
    std::unique_ptr<CatalogService> service_;
    std::unique_ptr<grpc::Server> server_;
    std::unique_ptr<v1::Catalog::Stub> stub_;
};

TEST_F(catalog_adapter, GetStoryCarriesCurrentRoute)
{
    const auto id = makeStory();
    v1::GetStoryRequest request;
    request.set_story_id(id);
    v1::GetStoryResponse response;
    ASSERT_TRUE(call(&v1::Catalog::Stub::GetStory, request, &response).ok());
    ASSERT_EQ(response.status().code(), 0);
    EXPECT_EQ(response.story().route().epoch(), response.story().epoch());
    EXPECT_FALSE(response.story().route().player().empty());
    EXPECT_EQ(response.story().route().keepers_size(), 2);
    v1::ListStoriesRequest list;
    list.set_chronicle("c");
    v1::ListStoriesResponse stories;
    ASSERT_TRUE(call(&v1::Catalog::Stub::ListStories, list, &stories).ok());
    ASSERT_EQ(stories.stories_size(), 1);
    EXPECT_EQ(stories.stories(0).route().epoch(), stories.stories(0).epoch());
}

TEST_F(catalog_adapter, AcquireCarriesAssignedKeeperEpochOneAndAStableWriterId)
{
    const StoryId story = makeStory();
    auto first = acquire(story, "w1");
    EXPECT_EQ(first.status().code(), 0);
    EXPECT_EQ(first.story_id(), story);
    EXPECT_EQ(first.writer_id(), 1u);
    EXPECT_EQ(first.incarnation(), 1u);
    EXPECT_EQ(first.route().epoch(), 1u);
    ASSERT_EQ(first.route().keepers_size(), 2);
    EXPECT_TRUE(first.assigned_keeper().process_id() == first.route().keepers(0).process_id() ||
                first.assigned_keeper().process_id() == first.route().keepers(1).process_id());
    EXPECT_FALSE(first.assigned_keeper().endpoint().empty());

    EXPECT_EQ(release(first).status().code(), 0);
    auto second = acquire(story, "w1");
    EXPECT_EQ(second.writer_id(), first.writer_id());
    EXPECT_EQ(second.incarnation(), 2u);
    EXPECT_EQ(second.assigned_keeper().process_id(), first.assigned_keeper().process_id());
    EXPECT_NE(acquire(story, "w2").writer_id(), first.writer_id());
}

TEST_F(catalog_adapter, MalformedRequestsFailTheWholeRequestWithInvalidArgument)
{
    v1::CreateChronicleResponse chronicle;
    EXPECT_EQ(call(&v1::Catalog::Stub::CreateChronicle, v1::CreateChronicleRequest(), &chronicle).error_code(),
              grpc::StatusCode::INVALID_ARGUMENT);
    v1::GetStoryResponse story;
    EXPECT_EQ(call(&v1::Catalog::Stub::GetStory, v1::GetStoryRequest(), &story).error_code(),
              grpc::StatusCode::INVALID_ARGUMENT);
    v1::AcquireRequest acquire_request;
    acquire_request.set_story_id(1);
    v1::AcquireResponse acquired;
    EXPECT_EQ(call(&v1::Catalog::Stub::Acquire, acquire_request, &acquired).error_code(),
              grpc::StatusCode::INVALID_ARGUMENT);
    // Every producer sends a request id; an Acquire without one is malformed and mutates nothing.
    acquire_request.set_writer_identity("no-id");
    EXPECT_EQ(call(&v1::Catalog::Stub::Acquire, acquire_request, &acquired).error_code(),
              grpc::StatusCode::INVALID_ARGUMENT);
    EXPECT_TRUE(store_->snapshotAcquisitions()->active.empty());
    v1::ReleaseResponse released;
    EXPECT_EQ(call(&v1::Catalog::Stub::Release, v1::ReleaseRequest(), &released).error_code(),
              grpc::StatusCode::INVALID_ARGUMENT);
    v1::CompareAndSetEpochResponse epoch;
    EXPECT_EQ(call(&v1::Catalog::Stub::CompareAndSetEpoch, v1::CompareAndSetEpochRequest(), &epoch).error_code(),
              grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_F(catalog_adapter, WellFormedButStaleRequestsAreDomainResultsNotGrpcErrors)
{
    v1::AcquireRequest unknown;
    unknown.set_story_id(999);
    unknown.set_writer_identity("w");
    unknown.set_acquire_request_id(newAcquireRequestId());
    v1::AcquireResponse acquired;
    EXPECT_TRUE(call(&v1::Catalog::Stub::Acquire, unknown, &acquired).ok());
    EXPECT_EQ(acquired.status().code(), kNotFound);

    v1::GetChronicleRequest missing;
    missing.set_name("nope");
    v1::GetChronicleResponse got;
    EXPECT_TRUE(call(&v1::Catalog::Stub::GetChronicle, missing, &got).ok());
    EXPECT_EQ(got.status().code(), kNotFound);

    const StoryId story = makeStory();
    v1::CreateChronicleRequest again;
    again.set_name("c");
    v1::CreateChronicleResponse created;
    EXPECT_TRUE(call(&v1::Catalog::Stub::CreateChronicle, again, &created).ok());
    EXPECT_EQ(created.status().code(), kAlreadyExists);

    // I3.6: destroy while acquired is FAILED_PRECONDITION in the item status.
    auto held = acquire(story, "w1");
    v1::DestroyStoryRequest destroy;
    destroy.set_story_id(story);
    v1::DestroyStoryResponse status;
    EXPECT_TRUE(call(&v1::Catalog::Stub::DestroyStory, destroy, &status).ok());
    EXPECT_EQ(status.status().code(), kFailedPrecondition);
    v1::DestroyChronicleRequest destroy_chronicle;
    destroy_chronicle.set_name("c");
    v1::DestroyChronicleResponse destroyed;
    EXPECT_TRUE(call(&v1::Catalog::Stub::DestroyChronicle, destroy_chronicle, &destroyed).ok());
    EXPECT_EQ(destroyed.status().code(), kFailedPrecondition);

    // A second plain acquire of a live identity is a typed HELD domain result with no mutation.
    const auto revision = store_->snapshotAcquisitions()->revision;
    auto refused = acquire(story, "w1");
    EXPECT_EQ(refused.status().code(), kFailedPrecondition);
    EXPECT_EQ(refused.refusal_reason(), v1::ACQUIRE_REFUSAL_REASON_HELD);
    EXPECT_GT(refused.remaining_ns(), 0);
    EXPECT_EQ(store_->snapshotAcquisitions()->revision, revision);

    // Explicit CAS takeover supersedes it; a retried release of the old incarnation returns the supersession
    // revision.
    v1::AcquireRequest takeover;
    takeover.set_story_id(story);
    takeover.set_writer_identity("w1");
    takeover.set_takeover(true);
    takeover.set_expected_prior_incarnation(held.incarnation());
    takeover.set_acquire_request_id(newAcquireRequestId());
    v1::AcquireResponse newer;
    EXPECT_TRUE(call(&v1::Catalog::Stub::Acquire, takeover, &newer).ok());
    EXPECT_EQ(newer.status().code(), 0);
    EXPECT_EQ(newer.incarnation(), held.incarnation() + 1);
    auto retry = release(held);
    EXPECT_EQ(retry.status().code(), 0);
    EXPECT_GT(retry.revision(), revision);
    EXPECT_EQ(release(held).revision(), retry.revision());
    EXPECT_EQ(release(newer).status().code(), 0);

    // Static destroy waits for the old owner's current instance to apply the supersession revision.
    EXPECT_TRUE(call(&v1::Catalog::Stub::DestroyStory, destroy, &status).ok());
    EXPECT_EQ(status.status().code(), kFailedPrecondition);
    const auto owner = held.assigned_keeper().process_id();
    ASSERT_TRUE(membership_
                        ->registerProcess(
                                Process{owner, "instance-1", held.assigned_keeper().endpoint(), ProcessRole::Keeper})
                        .ok());
    ASSERT_TRUE(membership_->heartbeat(owner, "instance-1", retry.revision()).ok());
    EXPECT_TRUE(call(&v1::Catalog::Stub::DestroyStory, destroy, &status).ok());
    EXPECT_EQ(status.status().code(), 0);
}

TEST_F(catalog_adapter, ListsAndEpochCompareAndSet)
{
    const StoryId story = makeStory();
    v1::ListChroniclesResponse chronicles;
    EXPECT_TRUE(call(&v1::Catalog::Stub::ListChronicles, v1::ListChroniclesRequest(), &chronicles).ok());
    ASSERT_EQ(chronicles.chronicles_size(), 1);
    EXPECT_EQ(chronicles.chronicles(0).name(), "c");
    v1::ListStoriesRequest list;
    list.set_chronicle("c");
    v1::ListStoriesResponse stories;
    EXPECT_TRUE(call(&v1::Catalog::Stub::ListStories, list, &stories).ok());
    ASSERT_EQ(stories.stories_size(), 1);
    EXPECT_EQ(stories.stories(0).story_id(), story);
    EXPECT_EQ(stories.stories(0).epoch(), 1u);

    v1::CompareAndSetEpochRequest cas;
    cas.set_story_id(story);
    cas.set_expected(1);
    cas.set_desired(2);
    v1::CompareAndSetEpochResponse epoch;
    EXPECT_TRUE(call(&v1::Catalog::Stub::CompareAndSetEpoch, cas, &epoch).ok());
    EXPECT_EQ(epoch.status().code(), 0);
    EXPECT_EQ(epoch.epoch(), 2u);
    EXPECT_TRUE(call(&v1::Catalog::Stub::CompareAndSetEpoch, cas, &epoch).ok());
    EXPECT_EQ(epoch.status().code(), kFailedPrecondition);
}

TEST_F(catalog_adapter, NameRulesAndPropertiesAreItemResultsAndRoundTrip)
{
    auto createChronicle = [&](const std::string& name)
    {
        v1::CreateChronicleRequest request;
        request.set_name(name);
        v1::CreateChronicleResponse response;
        EXPECT_TRUE(call(&v1::Catalog::Stub::CreateChronicle, request, &response).ok()) << name;
        return response;
    };
    auto createStory = [&](const std::string& name, v1::CreateStoryRequest request = {})
    {
        request.set_chronicle("c");
        request.set_name(name);
        v1::CreateStoryResponse response;
        EXPECT_TRUE(call(&v1::Catalog::Stub::CreateStory, request, &response).ok()) << name;
        return response;
    };
    for(const char* bad: {"a/b", "@x", "/a"}) EXPECT_EQ(createChronicle(bad).status().code(), kInvalidArgument) << bad;
    ASSERT_EQ(createChronicle("c").status().code(), 0);
    for(const char* bad: {"/x", "x/", "x//y", "@x", "x/@y"})
        EXPECT_EQ(createStory(bad).status().code(), kInvalidArgument) << bad;

    v1::CreateStoryRequest properties;
    properties.set_tier_policy("cold");
    properties.set_retention_ns(7'000'000'000);
    properties.set_granularity(v1::GRANULARITY_US);
    const auto kept = createStory("a/b", properties);
    ASSERT_EQ(kept.status().code(), 0);
    EXPECT_EQ(kept.story().tier_policy(), "cold");
    EXPECT_EQ(kept.story().retention_ns(), 7'000'000'000);
    EXPECT_EQ(kept.story().granularity(), v1::GRANULARITY_US);
    v1::CreateStoryRequest negative;
    negative.set_retention_ns(-1);
    EXPECT_EQ(createStory("neg", negative).status().code(), kInvalidArgument);
    v1::CreateStoryRequest zero;
    zero.set_retention_ns(0);
    const auto none = createStory("none", zero);
    ASSERT_EQ(none.status().code(), 0);
    EXPECT_FALSE(none.story().has_tier_policy());
    EXPECT_FALSE(none.story().has_retention_ns());
    EXPECT_EQ(none.story().granularity(), v1::GRANULARITY_UNSPECIFIED);

    v1::GetStoryRequest get;
    get.set_story_id(kept.story().story_id());
    v1::GetStoryResponse fetched;
    ASSERT_TRUE(call(&v1::Catalog::Stub::GetStory, get, &fetched).ok());
    EXPECT_EQ(fetched.story().tier_policy(), "cold");
    EXPECT_EQ(fetched.story().granularity(), v1::GRANULARITY_US);

    v1::CreateChronicleRequest chronicle;
    chronicle.set_name("p");
    chronicle.set_retention_ns(9);
    chronicle.set_granularity(v1::GRANULARITY_S);
    v1::CreateChronicleResponse created;
    ASSERT_TRUE(call(&v1::Catalog::Stub::CreateChronicle, chronicle, &created).ok());
    EXPECT_EQ(created.chronicle().retention_ns(), 9);
    v1::ListChroniclesResponse listed;
    ASSERT_TRUE(call(&v1::Catalog::Stub::ListChronicles, v1::ListChroniclesRequest(), &listed).ok());
    ASSERT_EQ(listed.chronicles_size(), 2);
    EXPECT_FALSE(listed.chronicles(0).has_retention_ns());
    EXPECT_EQ(listed.chronicles(1).retention_ns(), 9);
    EXPECT_EQ(listed.chronicles(1).granularity(), v1::GRANULARITY_S);
}

TEST_F(catalog_adapter, ListStoriesByPrefixReturnsOneRevisionAndRefusesOverLimit)
{
    v1::CreateChronicleRequest chronicle;
    chronicle.set_name("c");
    v1::CreateChronicleResponse created_chronicle;
    ASSERT_TRUE(call(&v1::Catalog::Stub::CreateChronicle, chronicle, &created_chronicle).ok());
    std::set<StoryId> expected;
    for(const char* name: {"a", "a/b", "ab"})
    {
        v1::CreateStoryRequest story;
        story.set_chronicle("c");
        story.set_name(name);
        story.set_tier_policy(name);
        v1::CreateStoryResponse created;
        ASSERT_TRUE(call(&v1::Catalog::Stub::CreateStory, story, &created).ok());
        ASSERT_EQ(created.status().code(), 0);
        if(std::string(name) != "ab")
            expected.insert(created.story().story_id());
    }
    auto list = [&](const std::string& prefix, uint32_t limit)
    {
        v1::ListStoriesByPrefixRequest request;
        request.set_prefix(prefix);
        request.set_limit(limit);
        v1::ListStoriesByPrefixResponse response;
        const auto status = call(&v1::Catalog::Stub::ListStoriesByPrefix, request, &response);
        return std::make_pair(status, response);
    };

    const auto [status, response] = list("c/a", 10);
    ASSERT_TRUE(status.ok());
    EXPECT_EQ(response.status().code(), 0);
    EXPECT_FALSE(response.limit_exceeded());
    std::set<StoryId> got;
    for(const auto& story: response.stories())
    {
        got.insert(story.story_id());
        EXPECT_FALSE(story.tombstoned());
        EXPECT_TRUE(story.has_route());
        EXPECT_EQ(story.tier_policy(), story.name());
    }
    EXPECT_EQ(got, expected);

    // Over the limit: none returned and the flag set, still an OK RPC at a revision.
    const auto [over_status, over] = list("c/a", 1);
    ASSERT_TRUE(over_status.ok());
    EXPECT_TRUE(over.limit_exceeded());
    EXPECT_EQ(over.stories_size(), 0);
    const auto [empty_status, empty] = list("c/none", 1);
    ASSERT_TRUE(empty_status.ok());
    EXPECT_FALSE(empty.limit_exceeded());
    EXPECT_EQ(empty.stories_size(), 0);

    // The revision is the Catalog counter: an Acquire and Release advance it.
    const StoryId story = *expected.begin();
    const auto acquired = acquire(story, "writer");
    ASSERT_EQ(acquired.status().code(), 0);
    ASSERT_EQ(release(acquired).status().code(), 0);
    const auto [later_status, later] = list("c/a", 10);
    ASSERT_TRUE(later_status.ok());
    EXPECT_GT(later.revision(), response.revision());

    // Malformed requests fail the whole request.
    for(const auto& [prefix, limit]: std::vector<std::pair<std::string, uint32_t>>{{"", 10},
                                                                                   {"/c", 10},
                                                                                   {"c/", 10},
                                                                                   {"c//a", 10},
                                                                                   {"c", 0},
                                                                                   {"c", 65537}})
        EXPECT_EQ(list(prefix, limit).first.error_code(), grpc::StatusCode::INVALID_ARGUMENT) << prefix << limit;
}

TEST_F(catalog_adapter, ConcurrentAcquiresGetDistinctWriterIds)
{
    const StoryId story = makeStory();
    constexpr int kWriters = 16;
    std::vector<std::thread> threads;
    std::vector<uint64_t> ids(kWriters);
    for(int i = 0; i < kWriters; ++i)
    {
        threads.emplace_back(
                [&, i]
                {
                    v1::AcquireRequest request;
                    request.set_story_id(story);
                    request.set_writer_identity("writer-" + std::to_string(i));
                    request.set_acquire_request_id(newAcquireRequestId());
                    v1::AcquireResponse response;
                    if(call(&v1::Catalog::Stub::Acquire, request, &response).ok() && response.status().code() == 0)
                        ids[i] = response.writer_id();
                });
    }
    for(auto& thread: threads) thread.join();
    std::set<uint64_t> distinct(ids.begin(), ids.end());
    EXPECT_EQ(distinct.size(), static_cast<size_t>(kWriters));
    EXPECT_EQ(distinct.count(0), 0u);
}

} // namespace
} // namespace chronolog::visor

namespace chronolog::visor
{
TEST_F(catalog_adapter, AcquireLeaseValidationDoesNotMutate)
{
    const auto story = makeStory();
    v1::AcquireRequest request;
    request.set_story_id(story);
    request.set_writer_identity("invalid");
    request.set_acquire_request_id("validation-request-000000000001");
    const auto before = store_->snapshotAcquisitions()->revision;
    for(int malformed = 0; malformed < 5; ++malformed)
    {
        auto input = request;
        if(malformed == 0)
            input.set_lease_duration_ns(0);
        if(malformed == 1)
            input.set_lease_duration_ns(-1);
        if(malformed == 2)
            input.set_preferred_keeper_process_id("");
        if(malformed == 3)
            input.set_expected_prior_incarnation(0);
        if(malformed == 4)
            input.set_acquire_request_id(std::string(129, 'x'));
        v1::AcquireResponse response;
        EXPECT_TRUE(call(&v1::Catalog::Stub::Acquire, input, &response).ok());
        EXPECT_EQ(response.status().code(), 3);
        EXPECT_EQ(store_->snapshotAcquisitions()->revision, before);
    }
    v1::AcquireResponse grant;
    ASSERT_TRUE(call(&v1::Catalog::Stub::Acquire, request, &grant).ok());
    ASSERT_EQ(grant.status().code(), 0);
    EXPECT_GT(grant.lease().duration_ns(), 0);
    EXPECT_GE(grant.lease().remaining_ns(), 0);
    v1::AcquireResponse retry;
    ASSERT_TRUE(call(&v1::Catalog::Stub::Acquire, request, &retry).ok());
    EXPECT_EQ(retry.incarnation(), grant.incarnation());
    v1::ReleaseRequest release;
    release.set_story_id(story);
    release.set_writer_id(grant.writer_id());
    release.set_incarnation(grant.incarnation());
    v1::ReleaseResponse released;
    ASSERT_TRUE(call(&v1::Catalog::Stub::Release, release, &released).ok());
    ASSERT_TRUE(call(&v1::Catalog::Stub::Acquire, request, &retry).ok());
    EXPECT_EQ(retry.status().code(), 9);
    EXPECT_EQ(retry.termination_cause(), v1::ACQUISITION_TERMINATION_CAUSE_RELEASED);
    EXPECT_EQ(retry.story_id(), story);
    EXPECT_EQ(retry.writer_id(), grant.writer_id());
    EXPECT_EQ(retry.incarnation(), grant.incarnation());
}
TEST_F(catalog_adapter, RenewBatchPreservesPerEntryResults)
{
    const auto story = makeStory();
    auto live = acquire(story, "live");
    auto terminal = acquire(story, "terminal");
    v1::ReleaseRequest release;
    release.set_story_id(story);
    release.set_writer_id(terminal.writer_id());
    release.set_incarnation(terminal.incarnation());
    v1::ReleaseResponse released;
    ASSERT_TRUE(call(&v1::Catalog::Stub::Release, release, &released).ok());
    const auto revision = store_->snapshotAcquisitions()->revision;
    v1::RenewAcquisitionsRequest request;
    for(const auto& tuple: std::vector<RenewAcquisition>{{story, live.writer_id(), live.incarnation()},
                                                         {story, terminal.writer_id(), terminal.incarnation()},
                                                         {story, 99999, 1}})
    {
        auto* item = request.add_acquisitions();
        item->set_story_id(tuple.story_id);
        item->set_writer_id(tuple.writer_id);
        item->set_incarnation(tuple.incarnation);
    }
    v1::RenewAcquisitionsResponse response;
    ASSERT_TRUE(call(&v1::Catalog::Stub::RenewAcquisitions, request, &response).ok());
    ASSERT_EQ(response.results_size(), 3);
    EXPECT_EQ(response.results(0).status().code(), 0);
    EXPECT_TRUE(response.results(0).has_lease());
    EXPECT_EQ(response.results(1).status().code(), 9);
    EXPECT_EQ(response.results(1).termination_cause(), v1::ACQUISITION_TERMINATION_CAUSE_RELEASED);
    EXPECT_EQ(response.results(2).status().code(), 5);
    for(int i = 0; i < 3; ++i)
        EXPECT_EQ(response.results(i).acquisition().SerializeAsString(), request.acquisitions(i).SerializeAsString());
    EXPECT_EQ(store_->snapshotAcquisitions()->revision, revision);
    request.mutable_acquisitions(1)->set_incarnation(0);
    EXPECT_EQ(call(&v1::Catalog::Stub::RenewAcquisitions, request, &response).error_code(),
              grpc::StatusCode::INVALID_ARGUMENT);
    EXPECT_EQ(store_->snapshotAcquisitions()->revision, revision);
}
} // namespace chronolog::visor

namespace chronolog::visor
{
TEST_F(catalog_adapter, AcquirePreferenceResultMatchesAssignedKeeperAndRoute)
{
    const auto story = makeStory();
    auto acquire_with = [&](const std::string& identity, std::optional<std::string> hint)
    {
        v1::AcquireRequest request;
        request.set_story_id(story);
        request.set_writer_identity(identity);
        request.set_acquire_request_id(newAcquireRequestId());
        if(hint)
            request.set_preferred_keeper_process_id(*hint);
        v1::AcquireResponse response;
        EXPECT_TRUE(call(&v1::Catalog::Stub::Acquire, request, &response).ok());
        EXPECT_EQ(response.status().code(), 0);
        return response;
    };
    auto routed = [](const v1::AcquireResponse& r)
    {
        for(const auto& k: r.route().keepers())
            if(k.process_id() == r.assigned_keeper().process_id())
                return k.endpoint() == r.assigned_keeper().endpoint();
        return false;
    };
    // writer_id 1 maps to keeper-b by modulo.
    const auto honored = acquire_with("w1", "keeper-a");
    EXPECT_EQ(honored.keeper_preference(), v1::KEEPER_PREFERENCE_RESULT_HONORED);
    EXPECT_EQ(honored.assigned_keeper().process_id(), "keeper-a");
    EXPECT_TRUE(routed(honored));
    const auto stale = acquire_with("w2", "keeper-z");
    EXPECT_EQ(stale.keeper_preference(), v1::KEEPER_PREFERENCE_RESULT_NOT_IN_ROUTE);
    EXPECT_EQ(stale.assigned_keeper().process_id(), stale.route().keepers(stale.writer_id() % 2).process_id());
    EXPECT_TRUE(routed(stale));
    const auto absent = acquire_with("w3", std::nullopt);
    EXPECT_FALSE(absent.has_keeper_preference());
    EXPECT_TRUE(routed(absent));
    ASSERT_EQ(release(honored).status().code(), 0);
    const auto retained = acquire_with("w1", "keeper-b");
    EXPECT_EQ(retained.keeper_preference(), v1::KEEPER_PREFERENCE_RESULT_RETAINED);
    EXPECT_EQ(retained.assigned_keeper().process_id(), "keeper-a");
    EXPECT_TRUE(routed(retained));
}

TEST(PreferredKeeperEmptyRoute, RefusesWithoutAGrant)
{
    InMemoryMetadataStore memory(Topology{{}, "grapher:50053", "player:50054"});
    testing::TempDir dir;
    auto sqlite =
            SqliteMetadataStore::open((dir.path() / "catalog").string(), Topology{{}, "grapher:50053", "player:50054"});
    ASSERT_TRUE(sqlite.ok()) << sqlite.status();
    for(MetadataStore* store: {static_cast<MetadataStore*>(&memory), static_cast<MetadataStore*>(sqlite->get())})
    {
        ASSERT_TRUE(store->createChronicle("c").ok());
        const auto story = store->createStory("c", "s");
        ASSERT_TRUE(story.ok());
        AcquireOptions options;
        options.preferred_keeper_process_id = "keeper-a";
        const auto refused = store->acquire(story->id, "w", options);
        EXPECT_EQ(refused.status().code(), absl::StatusCode::kFailedPrecondition);
        auto snapshot = dynamic_cast<AcquisitionLedger*>(store)->snapshotAcquisitions();
        ASSERT_TRUE(snapshot.ok());
        EXPECT_TRUE(snapshot->active.empty());
    }
}
} // namespace chronolog::visor
