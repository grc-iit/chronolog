#include <algorithm>
#include <map>
#include <optional>
// Reusable suite: include this .cpp in the implementation instantiation TU,
// provide the documented fresh harness factory, then INSTANTIATE_TEST_SUITE_P.
// Do not also compile that suite separately into the same test executable.
#include <gtest/gtest.h>
#include "chronolog/metadata_store.h"
#include "chronolog/acquire_refusal.h"
namespace chronolog::contract
{
// Fence confirmations default to enabled; the dedicated release test changes delivery.
// restart replaces sut with a new instance opening the same durable state.
enum class AuthorityClockMode
{
    Ticking,
    Jump
};
struct MetadataStoreHarness
{
    std::unique_ptr<MetadataStore> sut;
    // Control only delivery of Keeper fence confirmation; timeout is configured
    // short in the fixture. Does not mutate Catalog commit or result semantics.
    std::function<void(bool)> confirmReleaseFence;
    std::function<void()> restart;
    // Whether restart reopens durable state rather than retaining a RAM double.
    bool durable_restart{};
    // G2 factories explicitly select static proof semantics when enabling leases.
    bool static_fence_proof;
    bool acquisition_leases{};
    std::function<void(int64_t, AuthorityClockMode)> advanceAuthorityClock;
    std::function<absl::Status()> stepExpirySweep;
    std::function<void(std::function<void()>)> beforeNextAcquireSample;
    std::function<absl::StatusOr<Acquisition>(const std::string&)> requestGrant;
    // Optional dispatch slots until G2 adds the implemented contract methods.
    std::function<absl::StatusOr<Acquisition>(StoryId, std::string, AcquireOptions)> acquireWithOptions;
    std::function<absl::StatusOr<std::vector<RenewAcquisitionResult>>(const std::vector<RenewAcquisition>&)>
            renewAcquisitions;
    int64_t lease_default_ns{}, lease_min_ns{}, lease_max_ns{};
    StoryId story{};
};
using MetadataStoreFactory = std::function<std::unique_ptr<MetadataStoreHarness>()>;
class MetadataStoreContract: public ::testing::TestWithParam<MetadataStoreFactory>
{
protected:
    std::unique_ptr<MetadataStoreHarness> h;
    void SetUp() override
    {
        h = GetParam()();
        ASSERT_NE(h, nullptr);
        ASSERT_NE(h->sut, nullptr);
        auto c = h->sut->createChronicle("c");
        ASSERT_TRUE(c.ok());
        auto s = h->sut->createStory("c", "s");
        ASSERT_TRUE(s.ok());
        h->story = s->id;
    }
};

// Every store factory runs the finite-grant contract before enforcement.
TEST_P(MetadataStoreContract, EveryAcquisitionHasAFiniteLease)
{
    ASSERT_TRUE(h->acquisition_leases);
    auto grant = h->sut->acquire(h->story, "finite");
    ASSERT_TRUE(grant.ok()) << grant.status();
    EXPECT_GT(grant->lease.duration_ns, 0);
    EXPECT_GE(grant->lease.remaining_ns, 0);
    EXPECT_LE(grant->lease.remaining_ns, grant->lease.duration_ns);
    EXPECT_FALSE(grant->keeper_preference);
}

TEST_P(MetadataStoreContract, LeaseRequestUsesDefaultAndClamps)
{
    ASSERT_TRUE(h->acquisition_leases);
    ASSERT_TRUE(h->acquireWithOptions);
    ASSERT_GT(h->lease_min_ns, 1);
    ASSERT_LE(h->lease_min_ns, h->lease_default_ns);
    ASSERT_LE(h->lease_default_ns, h->lease_max_ns);
    for(auto requested: {std::optional<int64_t>{}, std::optional<int64_t>{1}, std::optional<int64_t>{INT64_MAX}})
    {
        AcquireOptions options;
        options.lease_duration_ns = requested;
        options.acquire_request_id = requested ? "duration-request-0000000000000001" + std::to_string(*requested)
                                               : "default-request-0000000000000001";
        auto grant = h->acquireWithOptions(h->story, options.acquire_request_id, options);
        ASSERT_TRUE(grant.ok()) << grant.status();
        EXPECT_EQ(grant->lease.duration_ns,
                  requested ? std::clamp(*requested, h->lease_min_ns, h->lease_max_ns) : h->lease_default_ns);
    }
    for(int64_t duration: {0, -1})
    {
        AcquireOptions options;
        options.lease_duration_ns = duration;
        options.acquire_request_id = "invalid-duration-request-00000001";
        auto grant = h->acquireWithOptions(h->story, "invalid-duration", options);
        EXPECT_TRUE(absl::IsInvalidArgument(grant.status()));
    }
}

TEST_P(MetadataStoreContract, RetriedAcquireAfterLostReplyReturnsTheSameGrant)
{
    ASSERT_TRUE(h->acquisition_leases);
    ASSERT_TRUE(h->acquireWithOptions);
    ASSERT_TRUE(h->advanceAuthorityClock);
    AcquireOptions options;
    options.acquire_request_id = "lost-reply-request-00000000000001";
    auto first = h->acquireWithOptions(h->story, "retry", options);
    ASSERT_TRUE(first.ok()) << first.status();
    h->advanceAuthorityClock(first->lease.duration_ns / 4, AuthorityClockMode::Ticking);
    auto retry = h->acquireWithOptions(h->story, "retry", options);
    ASSERT_TRUE(retry.ok()) << retry.status();
    EXPECT_EQ(retry->writer_id, first->writer_id);
    EXPECT_EQ(retry->incarnation, first->incarnation);
    EXPECT_EQ(retry->route, first->route);
    EXPECT_EQ(retry->assigned_keeper, first->assigned_keeper);
    EXPECT_EQ(retry->lease.duration_ns, first->lease.duration_ns);
    EXPECT_LT(retry->lease.remaining_ns, first->lease.remaining_ns);
    // A different logical Acquire of the live identity is HELD, and a same-id retry never renews.
    AcquireOptions other;
    other.acquire_request_id = "lost-reply-other-request-00000001";
    auto held = h->acquireWithOptions(h->story, "retry", other);
    ASSERT_TRUE(absl::IsFailedPrecondition(held.status())) << held.status();
    EXPECT_EQ(getAcquireRefusal(held.status())->refusal_reason, AcquireRefusalReason::Held);
    auto again = h->acquireWithOptions(h->story, "retry", options);
    ASSERT_TRUE(again.ok()) << again.status();
    EXPECT_EQ(again->incarnation, first->incarnation);
    EXPECT_LE(again->lease.remaining_ns, retry->lease.remaining_ns);
}

TEST_P(MetadataStoreContract, SameIdTerminalRetryReportsCauseAndMatchedIncarnation)
{
    ASSERT_TRUE(h->acquisition_leases);
    ASSERT_TRUE(h->acquireWithOptions);
    AcquireOptions options;
    options.acquire_request_id = "terminal-retry-request-0000000001";
    auto first = h->acquireWithOptions(h->story, "terminal", options);
    ASSERT_TRUE(first.ok()) << first.status();
    ASSERT_TRUE(h->sut->release(h->story, first->writer_id, first->incarnation).ok());
    auto retry = h->acquireWithOptions(h->story, "terminal", options);
    ASSERT_TRUE(absl::IsFailedPrecondition(retry.status()));
    auto refusal = getAcquireRefusal(retry.status());
    ASSERT_TRUE(refusal);
    EXPECT_EQ(refusal->matched_incarnation, first->incarnation);
    EXPECT_EQ(refusal->termination_cause, AcquisitionTerminationCause::Released);
    EXPECT_FALSE(refusal->current_incarnation);
}

TEST_P(MetadataStoreContract, SameIdRetryPreservesTupleWhenExpiryCommitsBeforeSample)
{
    if(!h->durable_restart)
        return;
    ASSERT_TRUE(h->beforeNextAcquireSample);
    ASSERT_TRUE(h->requestGrant);
    AcquireOptions options;
    options.acquire_request_id = "expiry-sample-request-00000000001";
    auto first = h->acquireWithOptions(h->story, "expiry-sample", options);
    ASSERT_TRUE(first.ok()) << first.status();
    h->beforeNextAcquireSample(
            [&]
            {
                h->advanceAuthorityClock(first->lease.duration_ns, AuthorityClockMode::Ticking);
                ASSERT_TRUE(h->stepExpirySweep().ok());
            });
    auto retry = h->acquireWithOptions(h->story, "expiry-sample", options);
    ASSERT_TRUE(absl::IsFailedPrecondition(retry.status())) << retry.status();
    auto refusal = getAcquireRefusal(retry.status());
    ASSERT_TRUE(refusal);
    EXPECT_EQ(refusal->matched_incarnation, first->incarnation);
    EXPECT_EQ(refusal->termination_cause, AcquisitionTerminationCause::Expired);
    auto matched = h->requestGrant(options.acquire_request_id);
    ASSERT_TRUE(matched.ok()) << matched.status();
    EXPECT_EQ(matched->story_id, first->story_id);
    EXPECT_EQ(matched->writer_id, first->writer_id);
    EXPECT_EQ(matched->incarnation, first->incarnation);
}

TEST_P(MetadataStoreContract, RestartRetainsDurationAndTerminalCause)
{
    ASSERT_TRUE(h->acquisition_leases);
    ASSERT_TRUE(h->acquireWithOptions);
    ASSERT_TRUE(h->renewAcquisitions);
    ASSERT_TRUE(h->restart);
    AcquireOptions options;
    options.acquire_request_id = "restart-request-00000000000000001";
    auto first = h->acquireWithOptions(h->story, "restart", options);
    ASSERT_TRUE(first.ok()) << first.status();
    RenewAcquisition tuple{h->story, first->writer_id, first->incarnation};
    h->restart();
    auto live = h->renewAcquisitions({tuple});
    ASSERT_TRUE(live.ok()) << live.status();
    ASSERT_EQ(live->size(), 1u);
    ASSERT_TRUE(live->front().status.ok());
    ASSERT_TRUE(live->front().lease);
    EXPECT_EQ(live->front().lease->duration_ns, first->lease.duration_ns);
    ASSERT_TRUE(h->sut->release(tuple.story_id, tuple.writer_id, tuple.incarnation).ok());
    h->restart();
    auto terminal = h->renewAcquisitions({tuple});
    ASSERT_TRUE(terminal.ok()) << terminal.status();
    ASSERT_EQ(terminal->size(), 1u);
    EXPECT_TRUE(absl::IsFailedPrecondition(terminal->front().status));
    EXPECT_EQ(terminal->front().termination_cause, AcquisitionTerminationCause::Released);
    EXPECT_FALSE(terminal->front().lease);
}

TEST_P(MetadataStoreContract, CompareAndSwapTakeoverRequiresCurrentPriorIncarnation)
{
    auto first = h->sut->acquire(h->story, "cas");
    ASSERT_TRUE(first.ok()) << first.status();
    AcquireOptions options;
    options.takeover = true;
    options.expected_prior_incarnation = first->incarnation;
    options.acquire_request_id = "cas-request-00000000000000000001";
    auto second = h->sut->acquire(h->story, "cas", options);
    ASSERT_TRUE(second.ok()) << second.status();
    EXPECT_EQ(second->incarnation, first->incarnation + 1);
    options.acquire_request_id = "cas-mismatch-000000000000000001";
    auto mismatch = h->sut->acquire(h->story, "cas", options);
    ASSERT_TRUE(absl::IsFailedPrecondition(mismatch.status()));
    auto detail = getAcquireRefusal(mismatch.status());
    ASSERT_TRUE(detail);
    EXPECT_EQ(detail->refusal_reason, AcquireRefusalReason::PriorMismatch);
    EXPECT_EQ(detail->current_incarnation, second->incarnation);
    ASSERT_TRUE(h->sut->release(h->story, second->writer_id, second->incarnation).ok());
    options.expected_prior_incarnation = second->incarnation;
    options.acquire_request_id = "cas-terminal-000000000000000001";
    auto terminal_match = h->sut->acquire(h->story, "cas", options);
    ASSERT_TRUE(terminal_match.ok()) << terminal_match.status();
    EXPECT_EQ(terminal_match->incarnation, second->incarnation + 1);
}

TEST_P(MetadataStoreContract, PriorMismatchReportsCurrentIncarnation)
{
    AcquireOptions options;
    options.takeover = true;
    options.expected_prior_incarnation = 1;
    options.acquire_request_id = "absent-prior-000000000000000001";
    auto absent = h->sut->acquire(h->story, "absent", options);
    ASSERT_TRUE(absl::IsFailedPrecondition(absent.status()));
    auto detail = getAcquireRefusal(absent.status());
    ASSERT_TRUE(detail);
    EXPECT_EQ(detail->refusal_reason, AcquireRefusalReason::PriorMismatch);
    EXPECT_FALSE(detail->current_incarnation);
    auto first = h->sut->acquire(h->story, "absent");
    ASSERT_TRUE(first.ok());
    options.expected_prior_incarnation = first->incarnation + 1;
    auto mismatch = h->sut->acquire(h->story, "absent", options);
    detail = getAcquireRefusal(mismatch.status());
    ASSERT_TRUE(detail);
    EXPECT_EQ(detail->current_incarnation, first->incarnation);
}

TEST_P(MetadataStoreContract, ExplicitTakeoverRecordsSuperseded)
{
    AcquireOptions options;
    options.acquire_request_id = "cause-first-0000000000000000001";
    auto first = h->sut->acquire(h->story, "cause", options);
    ASSERT_TRUE(first.ok());
    AcquireOptions takeover;
    takeover.takeover = true;
    takeover.acquire_request_id = "cause-second-000000000000000001";
    auto second = h->sut->acquire(h->story, "cause", takeover);
    ASSERT_TRUE(second.ok());
    auto retry = h->sut->acquire(h->story, "cause", options);
    auto detail = getAcquireRefusal(retry.status());
    ASSERT_TRUE(detail);
    EXPECT_EQ(detail->matched_incarnation, first->incarnation);
    EXPECT_EQ(detail->current_incarnation, second->incarnation);
    auto results = h->sut->renewAcquisitions({{h->story, first->writer_id, first->incarnation}});
    ASSERT_TRUE(results.ok());
    ASSERT_EQ(results->size(), 1u);
    EXPECT_EQ(results->front().termination_cause, AcquisitionTerminationCause::Superseded);
    auto release = h->sut->release(h->story, first->writer_id, first->incarnation);
    ASSERT_TRUE(release.ok());
    auto same = h->sut->renewAcquisitions({{h->story, first->writer_id, first->incarnation}});
    ASSERT_TRUE(same.ok());
    EXPECT_EQ(same->front().termination_cause, AcquisitionTerminationCause::Superseded);
}

TEST_P(MetadataStoreContract, RequestIdRejectsChangedInputsWithoutMutation)
{
    AcquireOptions options;
    options.acquire_request_id = "changed-inputs-0000000000000001";
    auto grant = h->sut->acquire(h->story, "inputs", options);
    ASSERT_TRUE(grant.ok());
    for(int mutation = 0; mutation < 4; ++mutation)
    {
        auto changed = options;
        if(mutation == 0)
            changed.lease_duration_ns = 1;
        if(mutation == 1)
            changed.takeover = true;
        if(mutation == 2)
            changed.expected_prior_incarnation = grant->incarnation;
        auto result = h->sut->acquire(h->story, mutation == 3 ? "different" : "inputs", changed);
        EXPECT_TRUE(absl::IsInvalidArgument(result.status()));
    }
    auto retry = h->sut->acquire(h->story, "inputs", options);
    ASSERT_TRUE(retry.ok());
    EXPECT_EQ(retry->incarnation, grant->incarnation);
}

TEST_P(MetadataStoreContract, StaticSuspendOrRestartExcludesDowntime)
{
    auto grant = h->sut->acquire(h->story, "gap");
    ASSERT_TRUE(grant.ok());
    h->advanceAuthorityClock(grant->lease.duration_ns / 2, AuthorityClockMode::Ticking);
    h->advanceAuthorityClock(grant->lease.duration_ns * 2, AuthorityClockMode::Jump);
    auto renewed = h->sut->renewAcquisitions({{h->story, grant->writer_id, grant->incarnation}});
    ASSERT_TRUE(renewed.ok()) << renewed.status();
    ASSERT_TRUE(renewed->front().status.ok()) << renewed->front().status;
    ASSERT_TRUE(renewed->front().lease);
    EXPECT_GT(renewed->front().lease->remaining_ns, grant->lease.duration_ns * 3 / 4);
}

TEST_P(MetadataStoreContract, ChronicleAndStoryCrud)
{
    auto c = h->sut->getChronicle("c");
    ASSERT_TRUE(c.ok());
    EXPECT_FALSE(c->tombstoned);
    auto s = h->sut->getStory(h->story);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(s->name, "s");
    auto cs = h->sut->listChronicles();
    ASSERT_TRUE(cs.ok());
    EXPECT_EQ(cs->size(), 1u);
    auto ss = h->sut->listStories("c");
    ASSERT_TRUE(ss.ok());
    EXPECT_EQ(ss->size(), 1u);
}

TEST_P(MetadataStoreContract, AssignedIdsDoNotAliasConcatenatedNames)
{
    ASSERT_TRUE(h->sut->createChronicle("a").ok());
    ASSERT_TRUE(h->sut->createChronicle("ab").ok());
    auto a = h->sut->createStory("a", "bc");
    auto b = h->sut->createStory("ab", "c");
    ASSERT_TRUE(a.ok());
    ASSERT_TRUE(b.ok());
    EXPECT_NE(a->id, b->id);
}

TEST_P(MetadataStoreContract, PersistedIncarnationStrictlyIncreases)
{
    ASSERT_TRUE(h->restart);
    auto a = h->sut->acquire(h->story, "writer");
    ASSERT_TRUE(a.ok());
    ASSERT_TRUE(h->sut->release(h->story, a->writer_id, a->incarnation).ok());
    h->restart();
    auto b = h->sut->acquire(h->story, "writer");
    ASSERT_TRUE(b.ok());
    EXPECT_EQ(b->writer_id, a->writer_id);
    EXPECT_GT(b->incarnation, a->incarnation);
}

TEST_P(MetadataStoreContract, AcquireReturnsRouteAndEpoch)
{
    auto a = h->sut->acquire(h->story, "writer");
    ASSERT_TRUE(a.ok());
    EXPECT_EQ(a->story_id, h->story);
    auto s = h->sut->getStory(h->story);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(a->route.epoch, s->epoch);
    EXPECT_FALSE(a->assigned_keeper.process_id.empty());
    EXPECT_NE(std::find(a->route.keepers.begin(), a->route.keepers.end(), a->assigned_keeper), a->route.keepers.end());
}

TEST_P(MetadataStoreContract, OldReleaseCannotReleaseNewIncarnation)
{
    auto a = h->sut->acquire(h->story, "writer");
    ASSERT_TRUE(a.ok());
    ASSERT_TRUE(h->sut->release(h->story, a->writer_id, a->incarnation).ok());
    auto b = h->sut->acquire(h->story, "writer");
    ASSERT_TRUE(b.ok());
    EXPECT_TRUE(h->sut->release(h->story, a->writer_id, a->incarnation).ok());
    EXPECT_TRUE(h->sut->release(h->story, b->writer_id, b->incarnation).ok());
}

TEST_P(MetadataStoreContract, StoryTombstonePermanence)
{
    ASSERT_TRUE(h->restart);
    ASSERT_TRUE(h->sut->destroyStory(h->story).ok());
    h->restart();
    auto replacement = h->sut->createStory("c", "s");
    ASSERT_TRUE(replacement.ok());
    EXPECT_NE(replacement->id, h->story);
    EXPECT_FALSE(h->sut->acquire(h->story, "writer").ok());
    auto s = h->sut->getStory(h->story);
    ASSERT_TRUE(s.ok());
    EXPECT_TRUE(s->tombstoned);
}

TEST_P(MetadataStoreContract, ChronicleTombstonePermanence)
{
    ASSERT_TRUE(h->sut->destroyChronicle("c").ok());
    EXPECT_TRUE(h->sut->createChronicle("c").ok());
    EXPECT_TRUE(h->sut->createStory("c", "new").ok());
    auto s = h->sut->getStory(h->story);
    ASSERT_TRUE(s.ok());
    EXPECT_TRUE(s->tombstoned);
}

TEST_P(MetadataStoreContract, EpochCompareAndSet)
{
    auto s = h->sut->getStory(h->story);
    ASSERT_TRUE(s.ok());
    auto a = h->sut->compareAndSetEpoch(h->story, s->epoch, s->epoch + 1);
    ASSERT_TRUE(a.ok());
    EXPECT_EQ(*a, s->epoch + 1);
    EXPECT_FALSE(h->sut->compareAndSetEpoch(h->story, s->epoch, s->epoch + 2).ok());
}

TEST_P(MetadataStoreContract, DestroyRefusesActiveAcquisitions)
{
    auto a = h->sut->acquire(h->story, "writer");
    ASSERT_TRUE(a.ok());
    EXPECT_EQ(h->sut->destroyStory(h->story).code(), absl::StatusCode::kFailedPrecondition);
    EXPECT_EQ(h->sut->destroyChronicle("c").code(), absl::StatusCode::kFailedPrecondition);
    ASSERT_TRUE(h->sut->release(h->story, a->writer_id, a->incarnation).ok());
    EXPECT_TRUE(h->sut->destroyChronicle("c").ok());
}

TEST_P(MetadataStoreContract, EpochMustStrictlyIncrease)
{
    auto story = h->sut->getStory(h->story);
    ASSERT_TRUE(story.ok());
    EXPECT_EQ(h->sut->compareAndSetEpoch(h->story, story->epoch, story->epoch).status().code(),
              absl::StatusCode::kInvalidArgument);
}

TEST_P(MetadataStoreContract, AssignedKeeperStableWithinWriterEpoch)
{
    auto a = h->sut->acquire(h->story, "writer");
    ASSERT_TRUE(a.ok());
    ASSERT_FALSE(a->assigned_keeper.process_id.empty());
    ASSERT_TRUE(h->sut->release(h->story, a->writer_id, a->incarnation).ok());
    auto b = h->sut->acquire(h->story, "writer");
    ASSERT_TRUE(b.ok());
    ASSERT_EQ(a->route.epoch, b->route.epoch);
    EXPECT_EQ(a->writer_id, b->writer_id);
    EXPECT_EQ(a->assigned_keeper, b->assigned_keeper);
}

TEST_P(MetadataStoreContract, ReleaseReportsFenceState)
{
    ASSERT_TRUE(h->confirmReleaseFence);
    h->confirmReleaseFence(false);
    auto first = h->sut->acquire(h->story, "writer");
    ASSERT_TRUE(first.ok());
    auto pending = h->sut->release(h->story, first->writer_id, first->incarnation);
    ASSERT_TRUE(pending.ok());
    EXPECT_FALSE(pending->fenced);
    EXPECT_GT(pending->revision, 0u);
    auto second = h->sut->acquire(h->story, "writer");
    ASSERT_TRUE(second.ok());
    EXPECT_GT(second->incarnation, first->incarnation);
    h->confirmReleaseFence(true);
    auto confirmed = h->sut->release(h->story, second->writer_id, second->incarnation);
    ASSERT_TRUE(confirmed.ok());
    EXPECT_TRUE(confirmed->fenced);
    EXPECT_GT(confirmed->revision, pending->revision);
}
TEST_P(MetadataStoreContract, RevisionSurvivesRestart)
{
    if(!h->durable_restart)
        GTEST_SKIP() << "in-memory store has no durable restart";
    ASSERT_TRUE(h->restart);
    auto a = h->sut->acquire(h->story, "writer");
    ASSERT_TRUE(a.ok());
    auto first = h->sut->release(h->story, a->writer_id, a->incarnation);
    ASSERT_TRUE(first.ok());
    h->restart();
    auto b = h->sut->acquire(h->story, "writer");
    ASSERT_TRUE(b.ok());
    auto second = h->sut->release(h->story, b->writer_id, b->incarnation);
    ASSERT_TRUE(second.ok());
    EXPECT_GT(second->revision, first->revision);
    auto retry = h->sut->release(h->story, a->writer_id, a->incarnation);
    ASSERT_TRUE(retry.ok());
    EXPECT_EQ(retry->revision, first->revision);
}
TEST_P(MetadataStoreContract, RevisionSurvivesLeaderChange)
{
    if(!h->durable_restart)
        GTEST_SKIP() << "in-memory store has no durable restart";
    ASSERT_TRUE(h->restart);
    auto a = h->sut->acquire(h->story, "writer");
    ASSERT_TRUE(a.ok());
    auto first = h->sut->release(h->story, a->writer_id, a->incarnation);
    ASSERT_TRUE(first.ok());
    h->restart();
    auto b = h->sut->acquire(h->story, "writer");
    ASSERT_TRUE(b.ok());
    auto second = h->sut->release(h->story, b->writer_id, b->incarnation);
    ASSERT_TRUE(second.ok());
    EXPECT_GT(second->revision, first->revision);
    auto retry = h->sut->release(h->story, a->writer_id, a->incarnation);
    ASSERT_TRUE(retry.ok());
    EXPECT_EQ(retry->revision, first->revision);
}
namespace
{
AcquireOptions withId(std::string id, std::optional<uint64_t> expected = {}, bool takeover = false)
{
    AcquireOptions options;
    options.acquire_request_id = std::move(id);
    options.expected_prior_incarnation = expected;
    options.takeover = takeover;
    return options;
}
std::optional<AcquisitionTerminationCause> causeOf(MetadataStoreHarness& h, const Acquisition& a)
{
    auto results = h.sut->renewAcquisitions({{a.story_id, a.writer_id, a.incarnation}});
    if(!results.ok() || results->size() != 1)
        return std::nullopt;
    return results->front().termination_cause;
}
} // namespace

TEST_P(MetadataStoreContract, UnexpiredLeaseRefusesSupersessionWithoutTakeover)
{
    auto first = h->sut->acquire(h->story, "held");
    ASSERT_TRUE(first.ok()) << first.status();
    auto refused = h->sut->acquire(h->story, "held", withId("held-plain-request-0000000000001"));
    ASSERT_TRUE(absl::IsFailedPrecondition(refused.status())) << refused.status();
    auto refusal = getAcquireRefusal(refused.status());
    ASSERT_TRUE(refusal);
    EXPECT_EQ(refusal->refusal_reason, AcquireRefusalReason::Held);
    EXPECT_GT(refusal->remaining_ns, 0);
    EXPECT_LE(refusal->remaining_ns, first->lease.duration_ns);
    EXPECT_FALSE(refusal->termination_cause);
    // No mutation: the holder stays current and live, and no incarnation was consumed.
    auto live = h->sut->renewAcquisitions({{h->story, first->writer_id, first->incarnation}});
    ASSERT_TRUE(live.ok());
    EXPECT_TRUE(live->front().status.ok()) << live->front().status;
    ASSERT_TRUE(h->sut->release(h->story, first->writer_id, first->incarnation).ok());
    EXPECT_EQ(causeOf(*h, *first), AcquisitionTerminationCause::Released);
    auto next = h->sut->acquire(h->story, "held");
    ASSERT_TRUE(next.ok());
    EXPECT_EQ(next->incarnation, first->incarnation + 1);
}

TEST_P(MetadataStoreContract, ConditionalAcquireRequiresCurrentTerminalIncarnation)
{
    auto first = h->sut->acquire(h->story, "conditional");
    ASSERT_TRUE(first.ok());
    auto live = h->sut->acquire(h->story, "conditional", withId("conditional-live-00000000000001", first->incarnation));
    ASSERT_TRUE(absl::IsFailedPrecondition(live.status()));
    EXPECT_EQ(getAcquireRefusal(live.status())->refusal_reason, AcquireRefusalReason::Held);
    ASSERT_TRUE(h->sut->release(h->story, first->writer_id, first->incarnation).ok());
    auto next =
            h->sut->acquire(h->story, "conditional", withId("conditional-terminal-000000000001", first->incarnation));
    ASSERT_TRUE(next.ok()) << next.status();
    EXPECT_EQ(next->incarnation, first->incarnation + 1);
    EXPECT_EQ(next->writer_id, first->writer_id);
}

TEST_P(MetadataStoreContract, ConditionalRecoveryCannotTakeOverANewerHolder)
{
    auto first = h->sut->acquire(h->story, "recovery");
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(h->sut->release(h->story, first->writer_id, first->incarnation).ok());
    auto newer = h->sut->acquire(h->story, "recovery");
    ASSERT_TRUE(newer.ok());
    for(bool takeover: {false, true})
    {
        auto stale = h->sut->acquire(
                h->story,
                "recovery",
                withId(takeover ? "recovery-cas-000000000000000001" : "recovery-plain-00000000000000001",
                       first->incarnation,
                       takeover));
        ASSERT_TRUE(absl::IsFailedPrecondition(stale.status())) << stale.status();
        auto refusal = getAcquireRefusal(stale.status());
        ASSERT_TRUE(refusal);
        EXPECT_EQ(refusal->refusal_reason, AcquireRefusalReason::PriorMismatch);
        EXPECT_EQ(refusal->current_incarnation, newer->incarnation);
    }
    auto live = h->sut->renewAcquisitions({{h->story, newer->writer_id, newer->incarnation}});
    ASSERT_TRUE(live.ok());
    EXPECT_TRUE(live->front().status.ok());
    EXPECT_EQ(causeOf(*h, *first), AcquisitionTerminationCause::Released);
}

TEST_P(MetadataStoreContract, ExpiryCommitChecksCurrentIncarnationAndUnreleased)
{
    ASSERT_TRUE(h->advanceAuthorityClock);
    ASSERT_TRUE(h->stepExpirySweep);
    auto released = h->sut->acquire(h->story, "released-first");
    auto dead = h->sut->acquire(h->story, "dead-holder");
    ASSERT_TRUE(released.ok());
    ASSERT_TRUE(dead.ok());
    ASSERT_TRUE(h->sut->release(h->story, released->writer_id, released->incarnation).ok());
    h->advanceAuthorityClock(dead->lease.duration_ns + 1, AuthorityClockMode::Ticking);
    ASSERT_TRUE(h->stepExpirySweep().ok());
    EXPECT_EQ(causeOf(*h, *dead), AcquisitionTerminationCause::Expired);
    EXPECT_EQ(causeOf(*h, *released), AcquisitionTerminationCause::Released);
    // Expiry keeps the writer identity and affinity: the next incarnation is an ordinary conditional Acquire.
    auto next = h->sut->acquire(h->story, "dead-holder", withId("expired-next-000000000000000001", dead->incarnation));
    ASSERT_TRUE(next.ok()) << next.status();
    EXPECT_EQ(next->writer_id, dead->writer_id);
    EXPECT_EQ(next->incarnation, dead->incarnation + 1);
    EXPECT_EQ(next->assigned_keeper, dead->assigned_keeper);
}

TEST_P(MetadataStoreContract, RenewalBeforeExpirySelectionPreventsExpiry)
{
    ASSERT_TRUE(h->advanceAuthorityClock);
    ASSERT_TRUE(h->stepExpirySweep);
    auto grant = h->sut->acquire(h->story, "renewed");
    ASSERT_TRUE(grant.ok());
    const auto T = grant->lease.duration_ns;
    h->advanceAuthorityClock(T * 3 / 4, AuthorityClockMode::Ticking);
    auto renewed = h->sut->renewAcquisitions({{h->story, grant->writer_id, grant->incarnation}});
    ASSERT_TRUE(renewed.ok());
    ASSERT_TRUE(renewed->front().status.ok()) << renewed->front().status;
    h->advanceAuthorityClock(T / 2, AuthorityClockMode::Ticking);
    ASSERT_TRUE(h->stepExpirySweep().ok());
    auto live = h->sut->renewAcquisitions({{h->story, grant->writer_id, grant->incarnation}});
    ASSERT_TRUE(live.ok());
    EXPECT_TRUE(live->front().status.ok()) << live->front().status;
}

TEST_P(MetadataStoreContract, RenewalAtDeadlineSelectsExpiry)
{
    ASSERT_TRUE(h->advanceAuthorityClock);
    ASSERT_TRUE(h->stepExpirySweep);
    auto grant = h->sut->acquire(h->story, "late-renewal");
    ASSERT_TRUE(grant.ok());
    h->advanceAuthorityClock(grant->lease.duration_ns, AuthorityClockMode::Ticking);
    auto late = h->sut->renewAcquisitions({{h->story, grant->writer_id, grant->incarnation}});
    ASSERT_TRUE(late.ok());
    EXPECT_TRUE(absl::IsUnavailable(late->front().status)) << late->front().status;
    EXPECT_FALSE(late->front().lease);
    ASSERT_TRUE(h->stepExpirySweep().ok());
    EXPECT_EQ(causeOf(*h, *grant), AcquisitionTerminationCause::Expired);
}

TEST_P(MetadataStoreContract, SelectedExpiryCannotAcknowledgeRenewal)
{
    ASSERT_TRUE(h->advanceAuthorityClock);
    ASSERT_TRUE(h->stepExpirySweep);
    auto grant = h->sut->acquire(h->story, "selected");
    ASSERT_TRUE(grant.ok());
    h->advanceAuthorityClock(grant->lease.duration_ns + 1, AuthorityClockMode::Ticking);
    for(int attempt = 0; attempt < 2; ++attempt)
    {
        auto refused = h->sut->renewAcquisitions({{h->story, grant->writer_id, grant->incarnation}});
        ASSERT_TRUE(refused.ok());
        EXPECT_TRUE(absl::IsUnavailable(refused->front().status)) << refused->front().status;
    }
    ASSERT_TRUE(h->stepExpirySweep().ok());
    EXPECT_EQ(causeOf(*h, *grant), AcquisitionTerminationCause::Expired);
    auto release = h->sut->release(h->story, grant->writer_id, grant->incarnation);
    ASSERT_TRUE(release.ok());
    EXPECT_EQ(causeOf(*h, *grant), AcquisitionTerminationCause::Expired);
}

TEST_P(MetadataStoreContract, ExpiredReleaseRetryReturnsOriginalRevision)
{
    ASSERT_TRUE(h->advanceAuthorityClock);
    ASSERT_TRUE(h->stepExpirySweep);
    auto grant = h->sut->acquire(h->story, "expired-release");
    ASSERT_TRUE(grant.ok());
    auto earlier = h->sut->acquire(h->story, "earlier-release");
    ASSERT_TRUE(earlier.ok());
    auto before = h->sut->release(h->story, earlier->writer_id, earlier->incarnation);
    ASSERT_TRUE(before.ok());
    h->advanceAuthorityClock(grant->lease.duration_ns + 1, AuthorityClockMode::Ticking);
    ASSERT_TRUE(h->stepExpirySweep().ok());
    ASSERT_EQ(causeOf(*h, *grant), AcquisitionTerminationCause::Expired);
    // A later Release polls the original expiry revision; it never relabels or allocates another.
    auto first = h->sut->release(h->story, grant->writer_id, grant->incarnation);
    ASSERT_TRUE(first.ok()) << first.status();
    EXPECT_GT(first->revision, before->revision);
    auto retry = h->sut->release(h->story, grant->writer_id, grant->incarnation);
    ASSERT_TRUE(retry.ok());
    EXPECT_EQ(retry->revision, first->revision);
    EXPECT_EQ(causeOf(*h, *grant), AcquisitionTerminationCause::Expired);
    auto after = h->sut->acquire(h->story, "later-release");
    ASSERT_TRUE(after.ok());
    auto later = h->sut->release(h->story, after->writer_id, after->incarnation);
    ASSERT_TRUE(later.ok());
    EXPECT_GT(later->revision, first->revision);
}

TEST_P(MetadataStoreContract, DestroyMaterializesLeaderDueSet)
{
    ASSERT_TRUE(h->advanceAuthorityClock);
    auto grant = h->sut->acquire(h->story, "due-at-destroy");
    ASSERT_TRUE(grant.ok());
    h->advanceAuthorityClock(grant->lease.duration_ns + 1, AuthorityClockMode::Ticking);
    // No sweep ran: destroy itself materializes the due holder before I3.6's active check.
    EXPECT_TRUE(h->sut->destroyStory(h->story).ok());
    EXPECT_EQ(causeOf(*h, *grant), AcquisitionTerminationCause::Expired);
    auto story = h->sut->getStory(h->story);
    ASSERT_TRUE(story.ok());
    EXPECT_TRUE(story->tombstoned);
}

TEST_P(MetadataStoreContract, DestroyStillRefusesALiveSiblingLease)
{
    ASSERT_TRUE(h->advanceAuthorityClock);
    auto sibling_story = h->sut->createStory("c", "sibling");
    ASSERT_TRUE(sibling_story.ok());
    auto dead = h->sut->acquire(h->story, "dead-sibling");
    ASSERT_TRUE(dead.ok());
    const auto T = dead->lease.duration_ns;
    h->advanceAuthorityClock(T / 2, AuthorityClockMode::Ticking);
    auto live = h->sut->acquire(sibling_story->id, "live-sibling");
    ASSERT_TRUE(live.ok());
    h->advanceAuthorityClock(T / 2 + 1, AuthorityClockMode::Ticking);
    EXPECT_EQ(h->sut->destroyChronicle("c").code(), absl::StatusCode::kFailedPrecondition);
    EXPECT_EQ(causeOf(*h, *dead), AcquisitionTerminationCause::Expired);
    EXPECT_FALSE(h->sut->getStory(h->story)->tombstoned);
    EXPECT_FALSE(h->sut->getStory(sibling_story->id)->tombstoned);
    auto renewed = h->sut->renewAcquisitions({{live->story_id, live->writer_id, live->incarnation}});
    ASSERT_TRUE(renewed.ok());
    EXPECT_TRUE(renewed->front().status.ok()) << renewed->front().status;
    EXPECT_TRUE(h->sut->destroyStory(h->story).ok());
    ASSERT_TRUE(h->sut->release(live->story_id, live->writer_id, live->incarnation).ok());
    EXPECT_TRUE(h->sut->destroyChronicle("c").ok());
}

TEST_P(MetadataStoreContract, DestroyRefusalCommitsMaterializedExpiry)
{
    ASSERT_TRUE(h->advanceAuthorityClock);
    auto dead = h->sut->acquire(h->story, "dead");
    ASSERT_TRUE(dead.ok());
    const auto T = dead->lease.duration_ns;
    h->advanceAuthorityClock(T / 2, AuthorityClockMode::Ticking);
    auto live = h->sut->acquire(h->story, "live");
    ASSERT_TRUE(live.ok());
    h->advanceAuthorityClock(T / 2 + 1, AuthorityClockMode::Ticking);
    EXPECT_EQ(h->sut->destroyStory(h->story).code(), absl::StatusCode::kFailedPrecondition);
    EXPECT_FALSE(h->sut->getStory(h->story)->tombstoned);
    EXPECT_EQ(causeOf(*h, *dead), AcquisitionTerminationCause::Expired);
    auto renewed = h->sut->renewAcquisitions({{h->story, live->writer_id, live->incarnation}});
    ASSERT_TRUE(renewed.ok());
    EXPECT_TRUE(renewed->front().status.ok()) << renewed->front().status;
    // The refused destroy's expiry stays committed across Release polls and a later successful destroy.
    auto release = h->sut->release(h->story, dead->writer_id, dead->incarnation);
    ASSERT_TRUE(release.ok());
    EXPECT_EQ(causeOf(*h, *dead), AcquisitionTerminationCause::Expired);
    ASSERT_TRUE(h->sut->release(h->story, live->writer_id, live->incarnation).ok());
    EXPECT_TRUE(h->sut->destroyStory(h->story).ok());
    EXPECT_EQ(causeOf(*h, *dead), AcquisitionTerminationCause::Expired);
}

TEST_P(MetadataStoreContract, StaticDestroyRequiresConfirmedExpiryFence)
{
    ASSERT_TRUE(h->advanceAuthorityClock);
    ASSERT_TRUE(h->stepExpirySweep);
    ASSERT_TRUE(h->confirmReleaseFence);
    auto grant = h->sut->acquire(h->story, "expired-owner");
    ASSERT_TRUE(grant.ok());
    h->advanceAuthorityClock(grant->lease.duration_ns + 1, AuthorityClockMode::Ticking);
    ASSERT_TRUE(h->stepExpirySweep().ok());
    ASSERT_EQ(causeOf(*h, *grant), AcquisitionTerminationCause::Expired);
    h->confirmReleaseFence(false);
    if(h->static_fence_proof)
    {
        EXPECT_EQ(h->sut->destroyStory(h->story).code(), absl::StatusCode::kFailedPrecondition);
        EXPECT_EQ(h->sut->destroyChronicle("c").code(), absl::StatusCode::kFailedPrecondition);
        EXPECT_FALSE(h->sut->getStory(h->story)->tombstoned);
        h->confirmReleaseFence(true);
    }
    EXPECT_TRUE(h->sut->destroyChronicle("c").ok());
    EXPECT_TRUE(h->sut->getStory(h->story)->tombstoned);
    EXPECT_EQ(causeOf(*h, *grant), AcquisitionTerminationCause::Expired);
}

TEST_P(MetadataStoreContract, StaticDestroyRequiresConfirmedSupersessionFence)
{
    ASSERT_TRUE(h->confirmReleaseFence);
    auto first = h->sut->acquire(h->story, "superseded-owner");
    ASSERT_TRUE(first.ok());
    auto second = h->sut->acquire(h->story,
                                  "superseded-owner",
                                  withId("supersede-fence-cas-0000000000001", first->incarnation, true));
    ASSERT_TRUE(second.ok()) << second.status();
    ASSERT_TRUE(h->sut->release(h->story, second->writer_id, second->incarnation).ok());
    h->confirmReleaseFence(false);
    if(h->static_fence_proof)
    {
        EXPECT_EQ(h->sut->destroyStory(h->story).code(), absl::StatusCode::kFailedPrecondition);
        EXPECT_EQ(h->sut->destroyChronicle("c").code(), absl::StatusCode::kFailedPrecondition);
        EXPECT_FALSE(h->sut->getStory(h->story)->tombstoned);
        h->confirmReleaseFence(true);
    }
    EXPECT_TRUE(h->sut->destroyStory(h->story).ok());
    EXPECT_TRUE(h->sut->getStory(h->story)->tombstoned);
    EXPECT_EQ(causeOf(*h, *first), AcquisitionTerminationCause::Superseded);
    EXPECT_EQ(causeOf(*h, *second), AcquisitionTerminationCause::Released);
}
namespace
{
std::vector<std::string> pathsUnder(MetadataStore& store, const std::string& prefix, uint32_t limit = 100)
{
    auto listed = store.listStoriesByPrefix(prefix, limit);
    std::vector<std::string> paths;
    if(!listed.ok())
        return {"<" + std::string(listed.status().message()) + ">"};
    for(const auto& story: listed->stories) paths.push_back(story.chronicle + "/" + story.name);
    std::sort(paths.begin(), paths.end());
    return paths;
}
} // namespace

TEST_P(MetadataStoreContract, NameRulesMakeStoryPathsUnique)
{
    EXPECT_TRUE(absl::IsInvalidArgument(h->sut->createChronicle("a/b").status()));
    EXPECT_TRUE(absl::IsInvalidArgument(h->sut->createChronicle("/a").status()));
    EXPECT_TRUE(absl::IsInvalidArgument(h->sut->createChronicle("a/").status()));
    for(const char* bad: {"/x", "x/", "x//y", "/", "//"})
        EXPECT_TRUE(absl::IsInvalidArgument(h->sut->createStory("c", bad).status())) << bad;
    ASSERT_TRUE(h->sut->createChronicle("a").ok());
    ASSERT_TRUE(h->sut->createChronicle("ab").ok());
    auto nested = h->sut->createStory("a", "b/c");
    ASSERT_TRUE(nested.ok()) << nested.status();
    ASSERT_TRUE(h->sut->createStory("ab", "c").ok());
    EXPECT_TRUE(absl::IsAlreadyExists(h->sut->createStory("a", "b/c").status()));
    // The first segment of a path is always the chronicle, so each path names one story.
    EXPECT_EQ(pathsUnder(*h->sut, "a/b/c"), (std::vector<std::string>{"a/b/c"}));
    EXPECT_EQ(pathsUnder(*h->sut, "ab/c"), (std::vector<std::string>{"ab/c"}));
    // Refusals create nothing, and "c/s" from SetUp is untouched.
    auto stories = h->sut->listStories("c");
    ASSERT_TRUE(stories.ok());
    EXPECT_EQ(stories->size(), 1u);
    EXPECT_EQ(h->sut->listChronicles()->size(), 3u);
}

TEST_P(MetadataStoreContract, ReservedSegmentsAreRefused)
{
    for(const char* bad: {"@x", "@"})
        EXPECT_TRUE(absl::IsInvalidArgument(h->sut->createChronicle(bad).status())) << bad;
    for(const char* bad: {"@x", "x/@y", "@x/y", "x/y/@"})
        EXPECT_TRUE(absl::IsInvalidArgument(h->sut->createStory("c", bad).status())) << bad;
    // An `@` anywhere but the start of a segment is an ordinary character.
    ASSERT_TRUE(h->sut->createChronicle("a@b").ok());
    ASSERT_TRUE(h->sut->createStory("c", "x@").ok());
    ASSERT_TRUE(h->sut->createStory("c", "x/y@z").ok());
    EXPECT_EQ(h->sut->listStories("c")->size(), 3u);
    EXPECT_EQ(h->sut->listChronicles()->size(), 2u);
    // A prefix may name a reserved segment: it is not refused as malformed, it simply matches nothing.
    EXPECT_EQ(pathsUnder(*h->sut, "c/@x"), (std::vector<std::string>{}));
}

TEST_P(MetadataStoreContract, ChronicleAndStoryPropertiesAreStoredAndInert)
{
    Properties full;
    full.tier_policy = "archive-after-7d";
    full.retention_ns = 5'000'000'000;
    full.granularity = Granularity::Ms;
    Properties zero_retention;
    zero_retention.retention_ns = 0;
    Properties negative;
    negative.retention_ns = -1;
    Properties unknown_granularity;
    unknown_granularity.granularity = static_cast<Granularity>(99);

    ASSERT_TRUE(h->sut->createChronicle("kept", full).ok());
    ASSERT_TRUE(h->sut->createChronicle("zero", zero_retention).ok());
    EXPECT_TRUE(absl::IsInvalidArgument(h->sut->createChronicle("neg", negative).status()));
    EXPECT_TRUE(absl::IsInvalidArgument(h->sut->createChronicle("neg", unknown_granularity).status()));
    EXPECT_TRUE(absl::IsNotFound(h->sut->getChronicle("neg").status()));
    auto kept = h->sut->createStory("kept", "s", full);
    ASSERT_TRUE(kept.ok()) << kept.status();
    ASSERT_TRUE(h->sut->createStory("kept", "zero", zero_retention).ok());
    EXPECT_TRUE(absl::IsInvalidArgument(h->sut->createStory("kept", "neg", negative).status()));
    EXPECT_TRUE(absl::IsInvalidArgument(h->sut->createStory("kept", "neg", unknown_granularity).status()));
    ASSERT_TRUE(h->sut->createStory("kept", "plain").ok());

    // Inert: the stored properties change no behavior of the story that carries them.
    auto grant = h->sut->acquire(kept->id, "writer");
    ASSERT_TRUE(grant.ok()) << grant.status();
    ASSERT_TRUE(h->sut->release(kept->id, grant->writer_id, grant->incarnation).ok());
    // Immutable: a second create of a live name is refused and leaves the first row as it was.
    Properties other;
    other.tier_policy = "other";
    EXPECT_TRUE(absl::IsAlreadyExists(h->sut->createChronicle("kept", other).status()));
    EXPECT_TRUE(absl::IsAlreadyExists(h->sut->createStory("kept", "s", other).status()));

    auto check = [&]
    {
        auto chronicle = h->sut->getChronicle("kept");
        ASSERT_TRUE(chronicle.ok());
        EXPECT_EQ(chronicle->properties, full);
        EXPECT_EQ(h->sut->getChronicle("zero")->properties, Properties{});
        auto listed = h->sut->listChronicles();
        ASSERT_TRUE(listed.ok());
        for(const auto& c: *listed)
        {
            if(c.name == "kept")
                EXPECT_EQ(c.properties, full);
            else
                EXPECT_EQ(c.properties, Properties{}) << c.name;
        }
        auto story = h->sut->getStory(kept->id);
        ASSERT_TRUE(story.ok());
        EXPECT_EQ(story->properties, full);
        auto stories = h->sut->listStories("kept");
        ASSERT_TRUE(stories.ok());
        ASSERT_EQ(stories->size(), 3u);
        for(const auto& s: *stories) EXPECT_EQ(s.properties, s.name == "s" ? full : Properties{}) << s.name;
        auto prefixed = h->sut->listStoriesByPrefix("kept/s", 10);
        ASSERT_TRUE(prefixed.ok());
        ASSERT_EQ(prefixed->stories.size(), 1u);
        EXPECT_EQ(prefixed->stories.front().properties, full);
    };
    check();
    ASSERT_TRUE(h->restart);
    h->restart();
    check();
}

TEST_P(MetadataStoreContract, ListByPrefixIsOneRevisionAndSegmentExact)
{
    for(uint32_t bad_limit: {0u, 65537u})
        EXPECT_TRUE(absl::IsInvalidArgument(h->sut->listStoriesByPrefix("c", bad_limit).status())) << bad_limit;
    for(const char* bad: {"", "/", "/c", "c/", "c//s", "//"})
        EXPECT_TRUE(absl::IsInvalidArgument(h->sut->listStoriesByPrefix(bad, 10).status())) << bad;
    EXPECT_TRUE(h->sut->listStoriesByPrefix("c", 65536).ok());

    ASSERT_TRUE(h->sut->createChronicle("p").ok());
    ASSERT_TRUE(h->sut->createChronicle("pq").ok());
    std::map<std::string, StoryId> ids;
    for(const auto& path: {"p/a", "p/a/b", "p/a/b/c", "p/ab", "pq/a"})
    {
        const std::string text = path;
        const auto slash = text.find('/');
        auto created = h->sut->createStory(text.substr(0, slash), text.substr(slash + 1));
        ASSERT_TRUE(created.ok()) << text << " " << created.status();
        ids[text] = created->id;
    }
    using Paths = std::vector<std::string>;
    // Whole segments only: `p` does not match `pq/a` and `p/a` does not match `p/ab`.
    EXPECT_EQ(pathsUnder(*h->sut, "p"), (Paths{"p/a", "p/a/b", "p/a/b/c", "p/ab"}));
    EXPECT_EQ(pathsUnder(*h->sut, "p/a"), (Paths{"p/a", "p/a/b", "p/a/b/c"}));
    EXPECT_EQ(pathsUnder(*h->sut, "p/a/b"), (Paths{"p/a/b", "p/a/b/c"}));
    EXPECT_EQ(pathsUnder(*h->sut, "p/a/b/c"), (Paths{"p/a/b/c"}));
    EXPECT_EQ(pathsUnder(*h->sut, "pq"), (Paths{"pq/a"}));
    EXPECT_EQ(pathsUnder(*h->sut, "p/a/b/c/d"), (Paths{}));
    EXPECT_EQ(pathsUnder(*h->sut, "none"), (Paths{}));

    // More than limit matches returns none of them and says so; exactly limit returns all.
    auto over = h->sut->listStoriesByPrefix("p", 3);
    ASSERT_TRUE(over.ok());
    EXPECT_TRUE(over->limit_exceeded);
    EXPECT_TRUE(over->stories.empty());
    auto exact = h->sut->listStoriesByPrefix("p", 4);
    ASSERT_TRUE(exact.ok());
    EXPECT_FALSE(exact->limit_exceeded);
    EXPECT_EQ(exact->stories.size(), 4u);
    for(const auto& story: exact->stories) EXPECT_EQ(ids.at(story.chronicle + "/" + story.name), story.id);

    // The result carries the Catalog revision of the read, which never regresses and advances with the counter.
    const uint64_t before = exact->revision;
    auto grant = h->sut->acquire(ids.at("p/a"), "writer");
    ASSERT_TRUE(grant.ok()) << grant.status();
    auto released = h->sut->release(ids.at("p/a"), grant->writer_id, grant->incarnation);
    ASSERT_TRUE(released.ok());
    auto after = h->sut->listStoriesByPrefix("p", 100);
    ASSERT_TRUE(after.ok());
    EXPECT_GT(after->revision, before);
    EXPECT_GE(after->revision, released->revision);

    // A tombstoned story leaves the result and its parents keep theirs.
    ASSERT_TRUE(h->sut->destroyStory(ids.at("p/a/b/c")).ok());
    EXPECT_EQ(pathsUnder(*h->sut, "p/a/b/c"), (Paths{}));
    EXPECT_EQ(pathsUnder(*h->sut, "p/a"), (Paths{"p/a", "p/a/b"}));
    ASSERT_TRUE(h->sut->destroyChronicle("pq").ok());
    EXPECT_EQ(pathsUnder(*h->sut, "pq"), (Paths{}));
}

GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(MetadataStoreContract);
} // namespace chronolog::contract
