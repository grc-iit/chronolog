#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>
#include <sys/wait.h>
#include <unistd.h>
#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>
#include "chronolog/client/client.h"
#include "chronolog/v1/chronolog.grpc.pb.h"

namespace
{
namespace wire = chronolog::v1;
namespace sdk = chronolog::client;
using Cause = chronolog::AcquisitionTerminationCause;
using Code = absl::StatusCode;
using namespace std::chrono_literals;
constexpr int64_t lease_ns = 300'000'000'000;
constexpr int64_t second_ns = 1'000'000'000;

// Finite grants, request-id dedupe, HELD, conditional and compare-and-swap acquire, Release, Renew and a Keeper that
// fences by recorded cause. It carries no admission evidence, so only the SDK scheduler can renew.
class LeasePeer final
    : public wire::Catalog::Service
    , public wire::Journal::Service
{
public:
    LeasePeer()
    {
        grpc::ServerBuilder builder;
        int port = 0;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(static_cast<wire::Catalog::Service*>(this));
        builder.RegisterService(static_cast<wire::Journal::Service*>(this));
        server_ = builder.BuildAndStart();
        endpoint_ = "127.0.0.1:" + std::to_string(port);
    }
    ~LeasePeer() override
    {
        {
            std::lock_guard lock(mutex);
            hold_acquire = false;
            block_renew = false;
        }
        cv.notify_all();
        server_->Shutdown(std::chrono::system_clock::now() + 2s);
        server_->Wait();
    }
    sdk::ClientOptions options()
    {
        sdk::ClientOptions o;
        o.catalog_endpoint = endpoint_;
        o.rpc_timeout = 2s;
        o.retry.max_retries = 1;
        o.retry.backoff = 0ms;
        o.lease.poll = 5ms;
        o.lease.margin = 0ms;
        o.lease.boottime_ns = [this] { return boot.load(); };
        return o;
    }
    struct Slot
    {
        uint64_t writer_id{};
        uint64_t incarnation{};
        bool live{};
        std::map<uint64_t, Cause> causes;
    };
    std::atomic<int64_t> boot{1000 * second_ns};
    std::mutex mutex;
    std::condition_variable cv;
    std::map<std::string, Slot> slots;
    std::map<std::string, std::pair<std::string, uint64_t>> grants;
    std::vector<wire::AcquireRequest> acquires;
    std::vector<std::vector<wire::RenewAcquisition>> renewals;
    std::atomic<size_t> releases{};
    std::atomic<size_t> appended{};
    std::atomic<bool> lose_reply{};
    std::atomic<bool> fail_release{};
    std::atomic<bool> fail_renew{};
    std::atomic<bool> not_registered{};
    bool hold_acquire{};
    bool block_renew{};
    size_t batches()
    {
        std::lock_guard lock(mutex);
        return renewals.size();
    }
    size_t dispatched()
    {
        std::lock_guard lock(mutex);
        return acquires.size();
    }
    wire::AcquireRequest last()
    {
        std::lock_guard lock(mutex);
        return acquires.back();
    }
    bool waitFor(const std::function<bool()>& done)
    {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, 10s, done);
    }
    void terminate(const std::string& identity, Cause cause)
    {
        std::lock_guard lock(mutex);
        auto& slot = slots.at(identity);
        slot.live = false;
        slot.causes[slot.incarnation] = cause;
    }
    // Another process takes the slot over explicitly.
    void foreignTakeover(const std::string& identity)
    {
        std::lock_guard lock(mutex);
        auto& slot = slots.at(identity);
        if(slot.live)
            slot.causes[slot.incarnation] = Cause::Superseded;
        ++slot.incarnation;
        slot.live = true;
    }
    size_t renewalsOf(uint64_t writer_id)
    {
        std::lock_guard lock(mutex);
        size_t n = 0;
        for(const auto& batch: renewals)
            for(const auto& t: batch) n += t.writer_id() == writer_id;
        return n;
    }
    grpc::Status Acquire(grpc::ServerContext*, const wire::AcquireRequest* r, wire::AcquireResponse* p) override
    {
        std::unique_lock lock(mutex);
        acquires.push_back(*r);
        cv.notify_all();
        cv.wait(lock, [&] { return !hold_acquire; });
        auto& slot = slots[r->writer_identity()];
        if(!slot.writer_id)
            slot.writer_id = slots.size();
        auto normalized = *r;
        normalized.clear_acquire_request_id();
        const auto inputs = normalized.SerializeAsString();
        auto refuse = [&](Code code, wire::AcquireRefusalReason reason)
        {
            p->mutable_status()->set_code(static_cast<int>(code));
            p->mutable_status()->set_message("refused");
            p->set_refusal_reason(reason);
            return grpc::Status::OK;
        };
        if(r->acquire_request_id().empty())
            return refuse(Code::kInvalidArgument, wire::ACQUIRE_REFUSAL_REASON_UNSPECIFIED);
        if(auto grant = grants.find(r->acquire_request_id()); grant != grants.end())
        {
            if(grant->second.first != inputs)
                return refuse(Code::kInvalidArgument, wire::ACQUIRE_REFUSAL_REASON_UNSPECIFIED);
            const auto matched = grant->second.second;
            if(matched != slot.incarnation)
            {
                p->set_incarnation(matched);
                p->set_current_incarnation(slot.incarnation);
                return refuse(Code::kFailedPrecondition, wire::ACQUIRE_REFUSAL_REASON_PRIOR_MISMATCH);
            }
            if(!slot.live)
            {
                p->set_incarnation(matched);
                p->set_termination_cause(static_cast<wire::AcquisitionTerminationCause>(slot.causes.at(matched)));
                return refuse(Code::kFailedPrecondition, wire::ACQUIRE_REFUSAL_REASON_UNSPECIFIED);
            }
        }
        else if(r->has_expected_prior_incarnation() && r->expected_prior_incarnation() != slot.incarnation)
        {
            if(slot.incarnation)
                p->set_current_incarnation(slot.incarnation);
            return refuse(Code::kFailedPrecondition, wire::ACQUIRE_REFUSAL_REASON_PRIOR_MISMATCH);
        }
        else if(slot.live && !r->takeover())
        {
            p->set_remaining_ns(lease_ns / 2);
            return refuse(Code::kFailedPrecondition, wire::ACQUIRE_REFUSAL_REASON_HELD);
        }
        else
        {
            if(slot.live)
                slot.causes[slot.incarnation] = Cause::Superseded;
            ++slot.incarnation;
            slot.live = true;
            grants[r->acquire_request_id()] = {inputs, slot.incarnation};
        }
        p->set_story_id(r->story_id());
        p->set_writer_id(slot.writer_id);
        p->set_incarnation(slot.incarnation);
        p->mutable_route()->set_epoch(1);
        p->mutable_route()->set_player(endpoint_);
        auto* keeper = p->mutable_route()->add_keepers();
        keeper->set_process_id("keeper");
        keeper->set_endpoint(endpoint_);
        *p->mutable_assigned_keeper() = *keeper;
        p->mutable_lease()->set_duration_ns(lease_ns);
        p->mutable_lease()->set_remaining_ns(lease_ns);
        if(lose_reply.exchange(false))
        {
            return {grpc::StatusCode::UNAVAILABLE, "reply lost after commit"};
        }
        return grpc::Status::OK;
    }
    Slot* slotOf(uint64_t writer_id)
    {
        for(auto& [identity, slot]: slots)
            if(slot.writer_id == writer_id)
                return &slot;
        return nullptr;
    }
    grpc::Status Release(grpc::ServerContext*, const wire::ReleaseRequest* r, wire::ReleaseResponse* p) override
    {
        std::lock_guard lock(mutex);
        ++releases;
        cv.notify_all();
        if(fail_release)
            return {grpc::StatusCode::UNAVAILABLE, "release reply lost"};
        auto* slot = slotOf(r->writer_id());
        if(!slot || r->incarnation() > slot->incarnation)
        {
            p->mutable_status()->set_code(static_cast<int>(Code::kNotFound));
            return grpc::Status::OK;
        }
        if(r->incarnation() == slot->incarnation && slot->live)
        {
            slot->live = false;
            slot->causes[slot->incarnation] = Cause::Released;
        }
        p->set_fenced(true);
        return grpc::Status::OK;
    }
    grpc::Status RenewAcquisitions(grpc::ServerContext* context,
                                   const wire::RenewAcquisitionsRequest* r,
                                   wire::RenewAcquisitionsResponse* p) override
    {
        std::unique_lock lock(mutex);
        renewals.emplace_back(r->acquisitions().begin(), r->acquisitions().end());
        cv.notify_all();
        for(int i = 0; block_renew && i < 2000 && !context->IsCancelled(); ++i) cv.wait_for(lock, 5ms);
        if(block_renew)
            return {grpc::StatusCode::CANCELLED, "renewal blocked"};
        if(fail_renew)
            return {grpc::StatusCode::UNAVAILABLE, "catalog unreachable"};
        for(const auto& t: r->acquisitions())
        {
            auto* result = p->add_results();
            *result->mutable_acquisition() = t;
            auto* slot = slotOf(t.writer_id());
            if(slot && slot->incarnation == t.incarnation() && slot->live)
            {
                result->mutable_lease()->set_duration_ns(lease_ns);
                result->mutable_lease()->set_remaining_ns(lease_ns);
            }
            else if(slot && slot->causes.contains(t.incarnation()))
            {
                result->mutable_status()->set_code(static_cast<int>(Code::kFailedPrecondition));
                result->set_termination_cause(
                        static_cast<wire::AcquisitionTerminationCause>(slot->causes.at(t.incarnation())));
            }
            else
                result->mutable_status()->set_code(static_cast<int>(Code::kNotFound));
        }
        return grpc::Status::OK;
    }
    grpc::Status Append(grpc::ServerContext*, const wire::AppendRequest* r, wire::AppendResponse* p) override
    {
        std::lock_guard lock(mutex);
        p->set_batch_id(r->batch_id());
        for(const auto& item: r->items())
        {
            auto* result = p->add_results();
            auto* slot = slotOf(item.writer_id());
            if(not_registered)
            {
                result->mutable_status()->set_code(static_cast<int>(Code::kFailedPrecondition));
                result->set_rejection(wire::APPEND_REJECTION_NOT_REGISTERED);
                continue;
            }
            if(!slot || item.incarnation() != slot->incarnation || !slot->live)
            {
                static const std::map<Cause, wire::AppendRejection> fences{
                        {Cause::Expired, wire::APPEND_REJECTION_FENCED_EXPIRED},
                        {Cause::Released, wire::APPEND_REJECTION_FENCED_RELEASED},
                        {Cause::Superseded, wire::APPEND_REJECTION_FENCED_SUPERSEDED},
                        {Cause::OwnerRemoved, wire::APPEND_REJECTION_FENCED_OWNER_REMOVED}};
                result->mutable_status()->set_code(static_cast<int>(Code::kFailedPrecondition));
                if(slot && slot->causes.contains(item.incarnation()))
                    result->set_rejection(fences.at(slot->causes.at(item.incarnation())));
                continue;
            }
            ++appended;
            result->mutable_id()->set_story_id(r->story_id());
            result->mutable_id()->set_writer_id(item.writer_id());
            result->mutable_id()->set_incarnation(item.incarnation());
            result->mutable_id()->set_sequence(item.sequence());
            result->mutable_assigned_hlc()->set_physical_ns(static_cast<int64_t>(item.sequence()));
            result->set_achieved_durability(r->durability());
        }
        return grpc::Status::OK;
    }

private:
    std::string endpoint_;
    std::unique_ptr<grpc::Server> server_;
};
const sdk::AppendSpec spec{{"", "payload", "", "", {}}};
std::optional<chronolog::AcquireRefusal> refusalOf(const absl::Status& s) { return sdk::acquireRefusalOf(s); }

TEST(ClientLeaseTest, IdleWriterRenewsThroughSharedScheduler)
{
    LeasePeer peer;
    auto client = sdk::Client::Connect(peer.options());
    ASSERT_TRUE(client.ok()) << client.status();
    auto a = client->acquire(1, "a");
    auto b = client->acquire(1, "b");
    ASSERT_TRUE(a.ok() && b.ok()) << a.status() << b.status();
    EXPECT_EQ(a->acquisition().lease.duration_ns, lease_ns);
    EXPECT_EQ(a->lease().estimated_remaining_ns, lease_ns);
    // Outside the lead window nothing renews; inside it one batch carries both idle Writers.
    peer.boot += lease_ns / 2 - second_ns;
    std::this_thread::sleep_for(50ms);
    EXPECT_EQ(peer.batches(), 0u);
    peer.boot += 2 * second_ns;
    ASSERT_TRUE(peer.waitFor([&] { return !peer.renewals.empty(); }));
    {
        std::lock_guard lock(peer.mutex);
        ASSERT_EQ(peer.renewals.front().size(), 2u);
        EXPECT_EQ(peer.renewals.front()[0].incarnation(), a->acquisition().incarnation);
    }
    ASSERT_TRUE(peer.waitFor([&] { return a->lease().renewals && b->lease().renewals; }));
    EXPECT_TRUE(a->lease().confirmed);
    EXPECT_EQ(b->lease().estimated_remaining_ns, lease_ns);
}

TEST(ClientLeaseTest, BusyWriterRenewsWhenKeeperCannotReachCatalog)
{
    LeasePeer peer;
    auto client = sdk::Client::Connect(peer.options());
    ASSERT_TRUE(client.ok()) << client.status();
    auto busy = client->acquire(1, "busy");
    ASSERT_TRUE(busy.ok()) << busy.status();
    const auto writer_id = busy->acquisition().writer_id;
    std::atomic<bool> stop{};
    std::atomic<size_t> failures{}, receipts{};
    std::thread appender(
            [&]
            {
                for(int i = 0; i < 20000 && !stop; ++i)
                {
                    auto r = busy->append(spec);
                    ++(r.ok() ? receipts : failures);
                }
            });
    ASSERT_TRUE(peer.waitFor([&] { return peer.appended > 0; }));
    // OK admissions never refresh the local estimate, so the busy Writer still renews in its lead window.
    EXPECT_EQ(peer.renewalsOf(writer_id), 0u);
    peer.boot += lease_ns / 2 + second_ns;
    ASSERT_TRUE(peer.waitFor([&] { return busy->lease().renewals > 0; }));
    stop = true;
    appender.join();
    EXPECT_GE(peer.renewalsOf(writer_id), 1u);
    EXPECT_EQ(failures.load(), 0u);
    EXPECT_GT(receipts.load(), 0u);
    EXPECT_TRUE(busy->lease().confirmed);
}

TEST(ClientLeaseTest, ResumeUsesBoottimeAndRenewsImmediately)
{
    LeasePeer peer;
    auto options = peer.options();
    options.lease.poll = 50ms;
    auto client = sdk::Client::Connect(options);
    ASSERT_TRUE(client.ok()) << client.status();
    auto writer = client->acquire(1, "suspended");
    ASSERT_TRUE(writer.ok()) << writer.status();
    // A suspend advances CLOCK_BOOTTIME by minutes while the steady clock advances by almost nothing.
    const auto steady = std::chrono::steady_clock::now();
    peer.boot += lease_ns * 2 / 3;
    ASSERT_TRUE(peer.waitFor([&] { return writer->lease().renewals > 0; }));
    EXPECT_LT(std::chrono::steady_clock::now() - steady, std::chrono::nanoseconds(lease_ns / 2));
    EXPECT_EQ(writer->lease().estimated_remaining_ns, lease_ns);
}

TEST(ClientLeaseTest, LocalHorizonNeverStopsDispatch)
{
    LeasePeer peer;
    peer.fail_renew = true;
    auto client = sdk::Client::Connect(peer.options());
    ASSERT_TRUE(client.ok()) << client.status();
    auto writer = client->acquire(1, "partitioned");
    ASSERT_TRUE(writer.ok()) << writer.status();
    peer.boot += lease_ns * 2;
    ASSERT_TRUE(peer.waitFor([&] { return !peer.renewals.empty(); }));
    peer.boot += second_ns;
    ASSERT_TRUE(peer.waitFor([&] { return peer.renewals.size() >= 2; }));
    const auto lease = writer->lease();
    EXPECT_FALSE(lease.confirmed);
    EXPECT_EQ(lease.estimated_remaining_ns, 0);
    EXPECT_FALSE(lease.termination_cause);
    // A missed horizon and failed renewals are diagnostics; the Keeper stays the admission authority.
    for(int i = 0; i < 3; ++i)
    {
        auto appended = writer->append(spec);
        ASSERT_TRUE(appended.ok()) << appended.status();
        EXPECT_EQ(appended->event_id.sequence, static_cast<uint64_t>(i + 1));
    }
    std::vector<sdk::AppendSpec> batch(4, spec);
    auto streamed = writer->appendBatch(batch);
    ASSERT_TRUE(streamed.ok()) << streamed.status();
    for(const auto& item: *streamed) EXPECT_TRUE(item.ok()) << item.status();
}

TEST(ClientLeaseTest, WriterMovePreservesSchedulerRegistration)
{
    LeasePeer peer;
    auto client = sdk::Client::Connect(peer.options());
    ASSERT_TRUE(client.ok()) << client.status();
    std::vector<sdk::Writer> writers;
    uint64_t writer_id = 0;
    {
        auto acquired = client->acquire(1, "moved");
        ASSERT_TRUE(acquired.ok()) << acquired.status();
        writer_id = acquired->acquisition().writer_id;
        sdk::Writer moved = std::move(*acquired);
        writers.push_back(std::move(moved));
        writers.reserve(64); // reallocation moves it again
    }
    peer.boot += lease_ns / 2 + second_ns;
    ASSERT_TRUE(peer.waitFor([&] { return writers.back().lease().renewals > 0; }));
    // Dropping the Writer drops its registration without retaining an abandoned Writer.
    writers.clear();
    const auto renewed = peer.renewalsOf(writer_id);
    peer.boot += lease_ns;
    std::this_thread::sleep_for(50ms);
    EXPECT_EQ(peer.renewalsOf(writer_id), renewed);
}

TEST(ClientLeaseTest, CloseStopsRenewalAndJoinsScheduler)
{
    LeasePeer peer;
    std::optional<sdk::Client> client;
    {
        auto connected = sdk::Client::Connect(peer.options());
        ASSERT_TRUE(connected.ok()) << connected.status();
        client = std::move(*connected);
    }
    auto closed = client->acquire(1, "closed");
    auto open = client->acquire(1, "open");
    ASSERT_TRUE(closed.ok() && open.ok());
    auto released = closed->release();
    ASSERT_TRUE(released.ok()) << released.status();
    peer.boot += lease_ns / 2 + second_ns;
    ASSERT_TRUE(peer.waitFor([&] { return open->lease().renewals > 0; }));
    EXPECT_EQ(peer.renewalsOf(closed->acquisition().writer_id), 0u);
    // Shutdown cancels an in-flight renewal and joins the scheduler.
    {
        std::lock_guard lock(peer.mutex);
        peer.block_renew = true;
    }
    const auto before = peer.batches();
    peer.boot += lease_ns;
    ASSERT_TRUE(peer.waitFor([&] { return peer.renewals.size() > before; }));
    client.reset();
    const auto after = peer.batches();
    peer.boot += lease_ns;
    std::this_thread::sleep_for(50ms);
    EXPECT_EQ(peer.batches(), after);
}

TEST(ClientLeaseTest, SameProcessRetryAcrossCallsKeepsAcquireId)
{
    LeasePeer peer;
    auto client = sdk::Client::Connect(peer.options());
    ASSERT_TRUE(client.ok()) << client.status();
    // Lost reply after commit, then the caller retries with the exposed id: the same grant, no new incarnation.
    auto id = client->newAcquireRequestId();
    ASSERT_TRUE(id.ok()) << id.status();
    EXPECT_EQ(id->size(), 32u);
    sdk::AcquireOptions options;
    options.acquire_request_id = *id;
    peer.lose_reply = true;
    auto lost = client->acquire(1, "explicit", options);
    EXPECT_EQ(lost.status().code(), Code::kUnavailable) << lost.status();
    auto retried = client->acquire(1, "explicit", options);
    ASSERT_TRUE(retried.ok()) << retried.status();
    EXPECT_EQ(retried->acquisition().incarnation, 1u);
    EXPECT_EQ(retried->acquisition().lease.duration_ns, lease_ns);
    // The id now belongs to a Writer: reuse is refused locally and never dispatched.
    const auto dispatched = peer.dispatched();
    EXPECT_EQ(client->acquire(1, "explicit", options).status().code(), Code::kFailedPrecondition);
    EXPECT_EQ(peer.dispatched(), dispatched);
    // A deliberate new logical call gets HELD with the authority's remainder, never a takeover.
    auto fresh = client->acquire(1, "explicit");
    ASSERT_EQ(fresh.status().code(), Code::kFailedPrecondition) << fresh.status();
    auto held = refusalOf(fresh.status());
    ASSERT_TRUE(held);
    EXPECT_EQ(held->refusal_reason, chronolog::AcquireRefusalReason::Held);
    EXPECT_EQ(held->remaining_ns, lease_ns / 2);
    EXPECT_FALSE(peer.last().takeover());
    EXPECT_NE(peer.last().acquire_request_id(), *id);
    // The same id with different options is refused before dispatch.
    auto other = client->newAcquireRequestId();
    ASSERT_TRUE(other.ok());
    options.acquire_request_id = *other;
    peer.lose_reply = true;
    EXPECT_EQ(client->acquire(1, "other", options).status().code(), Code::kUnavailable);
    options.lease_duration_ns = lease_ns / 3;
    const auto before = peer.dispatched();
    EXPECT_EQ(client->acquire(1, "other", options).status().code(), Code::kInvalidArgument);
    EXPECT_EQ(peer.dispatched(), before);

    // Lost reply then an identical acquire without an id reuses the Client's own unresolved id.
    peer.lose_reply = true;
    EXPECT_EQ(client->acquire(1, "implicit").status().code(), Code::kUnavailable);
    auto again = client->acquire(1, "implicit");
    ASSERT_TRUE(again.ok()) << again.status();
    EXPECT_EQ(again->acquisition().incarnation, 1u);
    {
        std::lock_guard lock(peer.mutex);
        const auto n = peer.acquires.size();
        EXPECT_EQ(peer.acquires[n - 1].acquire_request_id(), peer.acquires[n - 2].acquire_request_id());
        EXPECT_FALSE(peer.acquires[n - 1].takeover());
    }

    // Concurrent dispatch of one id: one call reaches the Catalog, the other waits and is refused.
    auto shared = client->newAcquireRequestId();
    ASSERT_TRUE(shared.ok());
    sdk::AcquireOptions concurrent;
    concurrent.acquire_request_id = *shared;
    {
        std::lock_guard lock(peer.mutex);
        peer.hold_acquire = true;
    }
    absl::StatusOr<sdk::Writer> first = absl::UnknownError(""), second = absl::UnknownError("");
    std::thread a([&] { first = client->acquire(1, "concurrent", concurrent); });
    ASSERT_TRUE(peer.waitFor([&] { return peer.acquires.back().acquire_request_id() == *shared; }));
    std::thread b([&] { second = client->acquire(1, "concurrent", concurrent); });
    {
        std::lock_guard lock(peer.mutex);
        peer.hold_acquire = false;
    }
    peer.cv.notify_all();
    a.join();
    b.join();
    ASSERT_TRUE(first.ok()) << first.status();
    EXPECT_EQ(second.status().code(), Code::kFailedPrecondition) << second.status();
    std::lock_guard lock(peer.mutex);
    EXPECT_EQ(std::count_if(peer.acquires.begin(),
                            peer.acquires.end(),
                            [&](const auto& r) { return r.acquire_request_id() == *shared; }),
              1);
}

TEST(ClientLeaseTest, TwoProcessesCannotShareIncarnationByReusingRequestId)
{
    LeasePeer peer;
    auto first = sdk::Client::Connect(peer.options());
    auto second = sdk::Client::Connect(peer.options());
    ASSERT_TRUE(first.ok() && second.ok());
    auto id = first->newAcquireRequestId();
    ASSERT_TRUE(id.ok());
    sdk::AcquireOptions copied;
    copied.acquire_request_id = *id;
    auto owner = first->acquire(1, "slot", copied);
    ASSERT_TRUE(owner.ok()) << owner.status();
    // Copied or checkpoint-restored retry state is refused before dispatch in another Client.
    const auto dispatched = peer.dispatched();
    EXPECT_EQ(second->acquire(1, "slot", copied).status().code(), Code::kInvalidArgument);
    copied.acquire_request_id = "restored-from-a-checkpoint";
    EXPECT_EQ(second->acquire(1, "slot", copied).status().code(), Code::kInvalidArgument);
    EXPECT_EQ(peer.dispatched(), dispatched);
    // Fresh-id CAS against a stale prior refuses and names the newer holder; against the exact prior it fences it.
    sdk::AcquireOptions cas;
    cas.takeover = true;
    cas.expected_prior_incarnation = owner->acquisition().incarnation + 1;
    auto stale = second->acquire(1, "slot", cas);
    ASSERT_EQ(stale.status().code(), Code::kFailedPrecondition);
    ASSERT_TRUE(refusalOf(stale.status()));
    EXPECT_EQ(refusalOf(stale.status())->refusal_reason, chronolog::AcquireRefusalReason::PriorMismatch);
    EXPECT_EQ(refusalOf(stale.status())->current_incarnation, owner->acquisition().incarnation);
    cas.expected_prior_incarnation = owner->acquisition().incarnation;
    auto successor = second->acquire(1, "slot", cas);
    ASSERT_TRUE(successor.ok()) << successor.status();
    EXPECT_EQ(successor->acquisition().incarnation, owner->acquisition().incarnation + 1);
    auto fenced = owner->append(spec);
    EXPECT_EQ(sdk::rejectionOf(fenced.status()), sdk::AppendRejection::FencedSuperseded) << fenced.status();
    EXPECT_TRUE(successor->append(spec).ok());
}

TEST(ClientLeaseTest, ForkThenAcquireRefusesInheritedHandles)
{
    LeasePeer peer;
    std::optional<sdk::Client> client;
    {
        auto connected = sdk::Client::Connect(peer.options());
        ASSERT_TRUE(connected.ok()) << connected.status();
        client = std::move(*connected);
    }
    std::optional<sdk::Writer> writer;
    {
        auto acquired = client->acquire(1, "parent");
        ASSERT_TRUE(acquired.ok()) << acquired.status();
        writer = std::move(*acquired);
    }
    peer.boot += lease_ns / 2 + second_ns;
    ASSERT_TRUE(peer.waitFor([&] { return writer->lease().renewals > 0; }));
    const pid_t child = fork();
    ASSERT_GE(child, 0);
    if(child == 0)
    {
        alarm(20);
        int failed = 0;
        failed |= client->acquire(1, "child").status().code() != Code::kFailedPrecondition ? 1 : 0;
        failed |= client->newAcquireRequestId().status().code() != Code::kFailedPrecondition ? 2 : 0;
        failed |= writer->append(spec).status().code() != Code::kFailedPrecondition ? 4 : 0;
        failed |= writer->release().status().code() != Code::kFailedPrecondition ? 8 : 0;
        // Inherited handles are dropped without joining the parent's scheduler thread.
        writer.reset();
        client.reset();
        _exit(failed);
    }
    int status = 0;
    ASSERT_EQ(waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status)) << status;
    EXPECT_EQ(WEXITSTATUS(status), 0);
    // The parent still owns and renews its Writer.
    EXPECT_TRUE(writer->append(spec).ok());
    const auto renewed = writer->lease().renewals;
    peer.boot += lease_ns;
    ASSERT_TRUE(peer.waitFor([&] { return writer->lease().renewals > renewed; }));
}

TEST(ClientLeaseTest, FailedReleaseDoesNotLockOutNextOwnOperation)
{
    LeasePeer peer;
    auto client = sdk::Client::Connect(peer.options());
    ASSERT_TRUE(client.ok()) << client.status();
    auto last = [&] { return peer.last(); };
    // Release stays unconfirmed: retry it, then compare-and-swap the own prior with a fresh id.
    {
        auto writer = client->acquire(1, "lost");
        ASSERT_TRUE(writer.ok());
        peer.fail_release = true;
        EXPECT_EQ(writer->release().status().code(), Code::kUnavailable);
    }
    const auto first_id = last().acquire_request_id();
    auto successor = client->acquire(1, "lost");
    ASSERT_TRUE(successor.ok()) << successor.status();
    EXPECT_EQ(successor->acquisition().incarnation, 2u);
    EXPECT_TRUE(last().takeover());
    EXPECT_EQ(last().expected_prior_incarnation(), 1u);
    EXPECT_NE(last().acquire_request_id(), first_id);
    EXPECT_EQ(peer.releases.load(), 2u);
    peer.fail_release = false;

    // The retried Release confirms: a plain acquire follows.
    {
        auto writer = client->acquire(1, "recovered");
        ASSERT_TRUE(writer.ok());
        peer.fail_release = true;
        EXPECT_FALSE(writer->release().ok());
        peer.fail_release = false;
    }
    auto reopened = client->acquire(1, "recovered");
    ASSERT_TRUE(reopened.ok()) << reopened.status();
    EXPECT_FALSE(last().takeover());
    EXPECT_FALSE(last().has_expected_prior_incarnation());

    // A dropped Writer that was never released is recovered by compare-and-swap, not HELD for five minutes.
    {
        auto writer = client->acquire(1, "dropped");
        ASSERT_TRUE(writer.ok());
    }
    auto taken = client->acquire(1, "dropped");
    ASSERT_TRUE(taken.ok()) << taken.status();
    EXPECT_TRUE(last().takeover());
    EXPECT_EQ(last().expected_prior_incarnation(), 1u);

    // A newer foreign holder refuses with a typed conflict; nothing takes it over.
    {
        auto writer = client->acquire(1, "foreign");
        ASSERT_TRUE(writer.ok());
        peer.fail_release = true;
        EXPECT_FALSE(writer->release().ok());
    }
    peer.foreignTakeover("foreign");
    auto conflict = client->acquire(1, "foreign");
    ASSERT_EQ(conflict.status().code(), Code::kFailedPrecondition) << conflict.status();
    auto refusal = refusalOf(conflict.status());
    ASSERT_TRUE(refusal);
    EXPECT_EQ(refusal->refusal_reason, chronolog::AcquireRefusalReason::PriorMismatch);
    EXPECT_EQ(refusal->current_incarnation, 2u);
    std::lock_guard lock(peer.mutex);
    for(const auto& r: peer.acquires) EXPECT_FALSE(r.takeover() && !r.has_expected_prior_incarnation());
}

TEST(ClientLeaseTest, ExpiredAutoAcquireIsConditional)
{
    LeasePeer peer;
    auto client = sdk::Client::Connect(peer.options());
    ASSERT_TRUE(client.ok()) << client.status();
    auto last = [&] { return peer.last(); };
    // Keeper fences with FENCED_EXPIRED: the old Writer freezes and the next acquire is conditional on it.
    auto writer = client->acquire(1, "expired");
    ASSERT_TRUE(writer.ok());
    peer.terminate("expired", Cause::Expired);
    auto fenced = writer->append(spec);
    EXPECT_EQ(sdk::rejectionOf(fenced.status()), sdk::AppendRejection::FencedExpired) << fenced.status();
    EXPECT_EQ(writer->lease().termination_cause, Cause::Expired);
    auto successor = client->acquire(1, "expired");
    ASSERT_TRUE(successor.ok()) << successor.status();
    EXPECT_FALSE(last().takeover());
    EXPECT_EQ(last().expected_prior_incarnation(), 1u);
    EXPECT_EQ(writer->append(spec).status().code(), Code::kFailedPrecondition);
    EXPECT_TRUE(successor->append(spec).ok());

    // A renewal that learns EXPIRED also stops renewing; a newer holder then answers PRIOR_MISMATCH.
    auto renewed = client->acquire(1, "renewed");
    ASSERT_TRUE(renewed.ok());
    peer.terminate("renewed", Cause::Expired);
    peer.boot += lease_ns / 2 + second_ns;
    ASSERT_TRUE(peer.waitFor([&] { return renewed->lease().termination_cause.has_value(); }));
    EXPECT_EQ(renewed->lease().termination_cause, Cause::Expired);
    peer.foreignTakeover("renewed");
    auto conflict = client->acquire(1, "renewed");
    ASSERT_EQ(conflict.status().code(), Code::kFailedPrecondition);
    ASSERT_TRUE(refusalOf(conflict.status()));
    EXPECT_EQ(refusalOf(conflict.status())->refusal_reason, chronolog::AcquireRefusalReason::PriorMismatch);
    EXPECT_FALSE(last().takeover());
    EXPECT_EQ(last().expected_prior_incarnation(), 1u);
}

TEST(ClientLeaseTest, NotRegisteredAfterKeeperRestartResolvesThroughCatalog)
{
    LeasePeer peer;
    auto client = sdk::Client::Connect(peer.options());
    ASSERT_TRUE(client.ok()) << client.status();
    peer.not_registered = true;
    // Catalog still holds the grant: the registration refusal stands and no fence is invented.
    auto live = client->acquire(1, "live");
    ASSERT_TRUE(live.ok());
    auto refused = live->append(spec);
    EXPECT_EQ(sdk::rejectionOf(refused.status()), sdk::AppendRejection::NotRegistered) << refused.status();
    EXPECT_EQ(peer.renewalsOf(live->acquisition().writer_id), 1u);
    // Catalog reports the recorded cause: the Writer freezes with it after one classification.
    auto expired = client->acquire(1, "gone");
    ASSERT_TRUE(expired.ok());
    peer.terminate("gone", Cause::Expired);
    auto fenced = expired->append(spec);
    EXPECT_EQ(fenced.status().code(), Code::kFailedPrecondition);
    EXPECT_EQ(sdk::rejectionOf(fenced.status()), sdk::AppendRejection::FencedExpired) << fenced.status();
    EXPECT_EQ(peer.renewalsOf(expired->acquisition().writer_id), 1u);
    EXPECT_EQ(expired->lease().termination_cause, Cause::Expired);
}
} // namespace
