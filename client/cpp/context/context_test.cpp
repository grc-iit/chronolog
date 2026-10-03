#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <algorithm>
#include <set>
#include <mutex>
#include <condition_variable>
#include "chronolog/context/context.h"
#include "chronolog/v1/chronolog.grpc.pb.h"
#include "internal.h"

namespace
{
namespace ctx = chronolog::context;
namespace wire = chronolog::v1;
using namespace std::chrono_literals;
std::string deterministic(const wire::AppendItem& item)
{
    std::string bytes;
    {
        google::protobuf::io::StringOutputStream buffer(&bytes);
        google::protobuf::io::CodedOutputStream output(&buffer);
        output.SetSerializationDeterministic(true);
        item.SerializeToCodedStream(&output);
    }
    return bytes;
}

class Peer
    : public wire::Catalog::Service
    , public wire::Journal::Service
    , public wire::Replay::Service
{
public:
    Peer()
    {
        grpc::ServerBuilder builder;
        int port = 0;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(static_cast<wire::Catalog::Service*>(this));
        builder.RegisterService(static_cast<wire::Journal::Service*>(this));
        builder.RegisterService(static_cast<wire::Replay::Service*>(this));
        server = builder.BuildAndStart();
        endpoint = "127.0.0.1:" + std::to_string(port);
    }
    ~Peer() override
    {
        server->Shutdown(std::chrono::system_clock::now() + 2s);
        server->Wait();
    }
    ctx::ContextOptions options()
    {
        ctx::ContextOptions options;
        options.sdk.catalog_endpoint = endpoint;
        options.sdk.player_endpoint = endpoint;
        options.sdk.retry.max_retries = 0;
        options.sdk.rpc_timeout = 2s;
        return options;
    }
    wire::Route route()
    {
        wire::Route r;
        r.set_epoch(1);
        r.set_player(endpoint);
        for(const auto& id: {"slow", "fast"})
        {
            auto* keeper = r.add_keepers();
            keeper->set_process_id(id);
            keeper->set_endpoint(endpoint);
        }
        return r;
    }
    grpc::Status CreateChronicle(grpc::ServerContext*,
                                 const wire::CreateChronicleRequest*,
                                 wire::CreateChronicleResponse* p) override
    {
        p->mutable_status()->set_code(static_cast<int>(absl::StatusCode::kAlreadyExists));
        return grpc::Status::OK;
    }
    grpc::Status
    GetChronicle(grpc::ServerContext*, const wire::GetChronicleRequest* request, wire::GetChronicleResponse* p) override
    {
        p->mutable_chronicle()->set_name(request->name());
        return grpc::Status::OK;
    }
    grpc::Status
    ListStories(grpc::ServerContext*, const wire::ListStoriesRequest* request, wire::ListStoriesResponse* p) override
    {
        for(uint64_t id = 1; id <= 2; ++id)
        {
            auto* story = p->add_stories();
            story->set_story_id(id);
            story->set_chronicle(request->chronicle());
            story->set_name(id == 1 ? "notes" : "old");
            story->set_tombstoned(id == 2);
        }
        return grpc::Status::OK;
    }
    grpc::Status
    CreateStory(grpc::ServerContext*, const wire::CreateStoryRequest* request, wire::CreateStoryResponse* p) override
    {
        p->mutable_story()->set_story_id(3);
        p->mutable_story()->set_chronicle(request->chronicle());
        p->mutable_story()->set_name(request->name());
        return grpc::Status::OK;
    }
    grpc::Status
    GetStory(grpc::ServerContext*, const wire::GetStoryRequest* request, wire::GetStoryResponse* p) override
    {
        auto* story = p->mutable_story();
        story->set_story_id(request->story_id());
        story->set_chronicle("team");
        story->set_name("notes");
        story->set_epoch(1);
        if(route_available)
            *story->mutable_route() = route();
        return grpc::Status::OK;
    }
    grpc::Status Acquire(grpc::ServerContext*, const wire::AcquireRequest* request, wire::AcquireResponse* p) override
    {
        std::lock_guard lock(mutex);
        acquisitions.push_back(*request);
        p->set_story_id(request->story_id());
        p->set_writer_id(acquisitions.size());
        p->set_incarnation(1);
        *p->mutable_route() = route();
        *p->mutable_assigned_keeper() = p->route().keepers(0);
        return grpc::Status::OK;
    }
    grpc::Status Release(grpc::ServerContext*, const wire::ReleaseRequest*, wire::ReleaseResponse* p) override
    {
        std::lock_guard lock(mutex);
        ++releases;
        p->set_fenced(false);
        return grpc::Status::OK;
    }
    grpc::Status
    AppendStream(grpc::ServerContext*,
                 grpc::ServerReaderWriter<wire::AppendStreamResponse, wire::AppendStreamRequest>* stream) override
    {
        wire::AppendStreamRequest request;
        while(stream->Read(&request))
        {
            std::lock_guard lock(mutex);
            appends.push_back(request);
            wire::AppendStreamResponse response;
            response.set_batch_id(request.batch_id());
            for(const auto& item: request.items())
            {
                auto* result = response.add_results();
                result->mutable_status()->set_code(static_cast<int>(append_code));
                result->mutable_status()->set_message("FENCED_SUPERSEDED misleading text");
                result->set_rejection(static_cast<wire::AppendRejection>(reason));
                if(append_code == absl::StatusCode::kOk)
                {
                    result->mutable_id()->set_story_id(request.story_id());
                    result->mutable_id()->set_writer_id(item.writer_id());
                    result->mutable_id()->set_incarnation(item.incarnation());
                    result->mutable_id()->set_sequence(item.sequence());
                    result->mutable_assigned_hlc()->set_physical_ns(item.causal_floor().physical_ns() + 1);
                    result->set_achieved_durability(request.durability());
                }
            }
            if(!stream->Write(response))
                break;
        }
        return grpc::Status::OK;
    }
    grpc::Status Read(grpc::ServerContext*,
                      const wire::ReadRequest* request,
                      grpc::ServerWriter<wire::ReadResponse>* stream) override
    {
        std::lock_guard lock(mutex);
        reads.push_back(*request);
        if(read_error)
            return {grpc::StatusCode::UNAVAILABLE, "read source unavailable"};
        wire::ReadResponse response;
        if(request->hlc().end().physical_ns() <= 100)
        {
            for(uint64_t n = 1; n <= 5; ++n)
            {
                const int64_t hlc = n <= 2 ? 10 : static_cast<int64_t>(n * 10);
                if(hlc < request->hlc().start().physical_ns() || hlc >= request->hlc().end().physical_ns())
                    continue;
                auto* event = response.mutable_batch()->add_events();
                event->mutable_id()->set_story_id(request->story_id());
                event->mutable_id()->set_writer_id(n);
                event->mutable_id()->set_incarnation(1);
                event->mutable_id()->set_sequence(1);
                event->mutable_hlc()->set_physical_ns(hlc);
                event->mutable_envelope()->set_payload("payload");
            }
            stream->Write(response);
            response.Clear();
        }
        auto* completion = response.mutable_completion();
        const auto verdict =
                request->story_id() == failed_story ? chronolog::IncompleteReason::SourceFailed : read_reason;
        completion->set_complete(verdict == chronolog::IncompleteReason::None ||
                                 (verify_below_seal && request->hlc().end().physical_ns() <= seal));
        completion->set_reason(static_cast<wire::IncompleteReason>(verdict));
        completion->mutable_frontier()->set_physical_ns(seal ? seal : request->hlc().end().physical_ns());
        if(read_reason == chronolog::IncompleteReason::LaggingWriters)
        {
            auto* laggard = completion->add_laggards();
            laggard->set_writer_id(9);
            laggard->set_incarnation(2);
            laggard->mutable_frontier()->set_physical_ns(seal);
        }
        stream->Write(response);
        return grpc::Status::OK;
    }
    grpc::Status Tail(grpc::ServerContext* context,
                      const wire::TailRequest* request,
                      grpc::ServerWriter<wire::TailResponse>* stream) override
    {
        std::unique_lock lock(mutex);
        tails.push_back(*request);
        tail_ready.notify_all();
        tail_ready.wait_until(lock,
                              std::chrono::system_clock::now() + 2s,
                              [&] { return tails.size() >= expected_tails; });
        if(tail_error)
            return {grpc::StatusCode::DEADLINE_EXCEEDED, "remote deadline"};
        if(idle_tail)
        {
            tail_ready.wait_until(lock, context->deadline(), [&] { return context->IsCancelled(); });
            return {grpc::StatusCode::CANCELLED, "idle cancellation"};
        }
        wire::TailResponse response;
        auto* event = response.mutable_batch()->add_events();
        event->mutable_id()->set_story_id(request->story_id());
        event->mutable_id()->set_writer_id(1);
        event->mutable_id()->set_incarnation(1);
        event->mutable_id()->set_sequence(1);
        event->mutable_envelope()->set_payload("oversized");
        *event->mutable_hlc() = request->from().hlc();
        event->mutable_hlc()->set_physical_ns(event->hlc().physical_ns() + 1);
        stream->Write(response);
        response.Clear();
        response.mutable_completion()->set_complete(false);
        stream->Write(response);
        return grpc::Status::OK;
    }
    std::string endpoint;
    std::unique_ptr<grpc::Server> server;
    std::mutex mutex;
    std::vector<wire::AcquireRequest> acquisitions;
    std::vector<wire::AppendStreamRequest> appends;
    std::vector<wire::ReadRequest> reads;
    std::vector<wire::TailRequest> tails;
    size_t releases{};
    size_t expected_tails{1};
    std::condition_variable tail_ready;
    absl::StatusCode append_code{absl::StatusCode::kOk};
    chronolog::AppendRejection reason{chronolog::AppendRejection::Unspecified};
    chronolog::IncompleteReason read_reason{chronolog::IncompleteReason::None};
    int64_t seal{};
    chronolog::StoryId failed_story{};
    bool route_available{true};
    bool verify_below_seal{false};
    bool read_error{false};
    bool tail_error{false};
    bool idle_tail{false};
};
ctx::Memory memory(std::string id, std::string payload = "memory")
{
    ctx::Memory result;
    result.operation_id = std::move(id);
    result.envelope.payload = std::move(payload);
    return result;
}
ctx::OpenOptions writable(std::string session = "run")
{
    ctx::OpenOptions options;
    options.session_id = std::move(session);
    return options;
}
ctx::ContextRef ref(uint64_t id = 1) { return {id, "team", "notes"}; }

TEST(ContextApi, SessionIdentityAndFloor)
{
    Peer peer;
    auto options = peer.options();
    options.max_writable_sessions = 2;
    auto client = ctx::ContextClient::Connect(options);
    ASSERT_TRUE(client.ok()) << client.status();
    auto a = client->open(ref(), {"agent\"\\\n", "slot"}, writable());
    ASSERT_TRUE(a.ok()) << a.status();
    auto alias = client->open(ref(), {"agent\"\\\n", "slot"}, writable("later"));
    ASSERT_TRUE(alias.ok());
    EXPECT_EQ(*a, *alias);
    auto resume = writable();
    resume.resume = ctx::Checkpoint{};
    resume.resume->identity = {"other", "slot"};
    resume.resume->context = ref(2);
    resume.resume->causal_floor = {400, 9};
    auto b = client->open(ref(2), {"other", "slot"}, resume);
    ASSERT_TRUE(b.ok()) << b.status();
    EXPECT_EQ((*a)->status().causal_floor, (chronolog::Hlc{400, 9}));
    auto receipt = (*a)->remember(memory("first"));
    ASSERT_TRUE(receipt.ok());
    ASSERT_TRUE(receipt->current.receipt);
    EXPECT_GT(receipt->current.receipt->hlc, (chronolog::Hlc{400, 9}));
    EXPECT_EQ(client->open(ref(3), {"other", "slot"}, writable()).status().code(),
              absl::StatusCode::kResourceExhausted);
    ctx::OpenOptions read_only;
    read_only.access = ctx::Access::ReadOnly;
    auto reader = client->open(ref(3), {"other", "slot"}, read_only);
    ASSERT_TRUE(reader.ok());
    EXPECT_EQ(peer.acquisitions.size(), 2u);
    EXPECT_EQ(peer.acquisitions[0].writer_identity(), "agent-context/v2:[\"agent\\\"\\\\\\n\",\"slot\"]");
    auto closed = (*a)->close();
    ASSERT_TRUE(closed.ok());
    EXPECT_TRUE(closed->release_committed);
    EXPECT_FALSE(closed->fenced);
    EXPECT_EQ((*alias)->status().state, ctx::SessionState::Closed);
    auto reopened = client->open(ref(), {"agent\"\\\n", "slot"}, writable());
    ASSERT_TRUE(reopened.ok()) << reopened.status();
    ASSERT_EQ(peer.acquisitions.size(), 3u);
    EXPECT_FALSE(peer.acquisitions[2].takeover());
    EXPECT_EQ(peer.acquisitions[2].expected_prior_incarnation(), 1u);
}

TEST(ContextApi, OnePendingOperationAndRedrive)
{
    Peer peer;
    peer.append_code = absl::StatusCode::kUnavailable;
    auto client = ctx::ContextClient::Connect(peer.options());
    ASSERT_TRUE(client.ok());
    auto session = client->open(ref(), {"agent", "slot"}, writable());
    ASSERT_TRUE(session.ok());
    auto a = (*session)->remember(memory("A"));
    ASSERT_TRUE(a.ok());
    EXPECT_EQ(a->current.outcome, ctx::MemoryOutcome::Unknown);
    EXPECT_EQ(a->state, ctx::SessionState::TransportPending);
    auto b = (*session)->remember(memory("B"));
    ASSERT_TRUE(b.ok());
    EXPECT_EQ(b->current.outcome, ctx::MemoryOutcome::Rejected);
    EXPECT_EQ(b->blocking_operation_id, "A");
    ASSERT_EQ(peer.appends.size(), 2u);
    EXPECT_TRUE(deterministic(peer.appends[0].items(0)) == deterministic(peer.appends[1].items(0)));
    peer.append_code = absl::StatusCode::kOk;
    b = (*session)->remember(memory("B"));
    ASSERT_TRUE(b.ok());
    ASSERT_EQ(b->resolved_prior.size(), 1u);
    EXPECT_EQ(b->resolved_prior[0].operation_id, "A");
    EXPECT_EQ(b->resolved_prior[0].outcome, ctx::MemoryOutcome::Durable);
    ASSERT_TRUE(b->current.receipt);
    EXPECT_EQ(b->current.receipt->event_id.sequence, 2u);
    EXPECT_TRUE(deterministic(peer.appends[2].items(0)) == deterministic(peer.appends[0].items(0)));
    EXPECT_EQ(peer.appends[3].items(0).envelope().payload(), "memory");
    EXPECT_EQ((*session)->remember(memory("A", "changed")).status().code(), absl::StatusCode::kFailedPrecondition);
    a = (*session)->remember(memory("A"));
    ASSERT_TRUE(a.ok());
    EXPECT_EQ(a->current.receipt->event_id.sequence, 1u);
    EXPECT_EQ(peer.appends.size(), 4u);
}

TEST(ContextApi, RememberOutcomes)
{
    for(auto reason: {chronolog::AppendRejection::FencedReleased,
                      chronolog::AppendRejection::FencedSuperseded,
                      chronolog::AppendRejection::FencedExpired,
                      chronolog::AppendRejection::FencedOwnerRemoved,
                      chronolog::AppendRejection::Unspecified,
                      chronolog::AppendRejection::DedupeWindow,
                      chronolog::AppendRejection::SequenceGap})
    {
        Peer peer;
        peer.append_code = absl::StatusCode::kFailedPrecondition;
        peer.reason = reason;
        auto client = ctx::ContextClient::Connect(peer.options());
        ASSERT_TRUE(client.ok());
        auto session = client->open(ref(), {"agent", "slot"}, writable());
        ASSERT_TRUE(session.ok());
        auto a = (*session)->remember(memory("A"));
        ASSERT_TRUE(a.ok());
        if(reason == chronolog::AppendRejection::SequenceGap)
        {
            EXPECT_EQ(a->current.outcome, ctx::MemoryOutcome::Rejected);
            EXPECT_EQ(a->state, ctx::SessionState::Ready);
        }
        else if(reason == chronolog::AppendRejection::DedupeWindow)
        {
            EXPECT_EQ(a->current.outcome, ctx::MemoryOutcome::Unknown);
            EXPECT_EQ(a->state, ctx::SessionState::TransportPending);
        }
        else
        {
            EXPECT_EQ(a->current.outcome, ctx::MemoryOutcome::Unknown);
            EXPECT_EQ(a->state,
                      reason == chronolog::AppendRejection::FencedExpired ||
                                      reason == chronolog::AppendRejection::FencedOwnerRemoved
                              ? ctx::SessionState::NeedsReconcile
                              : ctx::SessionState::Fenced);
            auto b = (*session)->remember(memory("B"));
            ASSERT_TRUE(b.ok());
            EXPECT_EQ(b->current.outcome, ctx::MemoryOutcome::Fenced);
            EXPECT_EQ(peer.appends.size(), 1u);
            auto reconciled = (*session)->reconcile();
            if(a->state == ctx::SessionState::Fenced)
            {
                EXPECT_EQ(reconciled.status().code(), absl::StatusCode::kFailedPrecondition);
                EXPECT_EQ(peer.acquisitions.size(), 1u);
            }
            else
            {
                // The conditional successor's marker meets the same fence and stays UNKNOWN.
                ASSERT_TRUE(reconciled.ok()) << reconciled.status();
                ASSERT_EQ(peer.acquisitions.size(), 2u);
                EXPECT_FALSE(peer.acquisitions[1].takeover());
                EXPECT_EQ(peer.acquisitions[1].expected_prior_incarnation(), 1u);
                EXPECT_FALSE(reconciled->attempted);
                EXPECT_EQ(chronolog::client::rejectionOf(reconciled->status), reason);
                EXPECT_EQ((*session)->status().state, ctx::SessionState::NeedsReconcile);
            }
        }
        EXPECT_EQ(chronolog::client::rejectionOf(a->current.status), reason);
    }
    Peer peer;
    auto client = ctx::ContextClient::Connect(peer.options());
    ASSERT_TRUE(client.ok());
    auto session = client->open(ref(), {"agent", "slot"}, writable());
    ASSERT_TRUE(session.ok());
    auto m = memory("default");
    m.durability = chronolog::Durability::Unspecified;
    auto result = (*session)->remember(m);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result->current.outcome, ctx::MemoryOutcome::Durable);
    auto accepted = memory("ram");
    accepted.durability = chronolog::Durability::Accepted;
    result = (*session)->remember(accepted);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result->current.outcome, ctx::MemoryOutcome::RamOnlyMayVanish);
    auto invalid = memory("invalid");
    invalid.envelope.content_type = "application/json";
    invalid.envelope.payload = "{";
    EXPECT_EQ((*session)->remember(invalid).status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_EQ(peer.appends.size(), 2u);
}

TEST(ContextApi, RecallPrefixAndLimits)
{
    Peer peer;
    auto client = ctx::ContextClient::Connect(peer.options());
    ASSERT_TRUE(client.ok());
    ctx::OpenOptions ro;
    ro.access = ctx::Access::ReadOnly;
    auto session = client->open(ref(), {"agent", "slot"}, ro);
    ASSERT_TRUE(session.ok());
    ctx::RecallOptions options;
    options.end = chronolog::Hlc{100, 0};
    options.limits.max_events = 1;
    auto page = (*session)->recall(options);
    ASSERT_TRUE(page.ok());
    ASSERT_EQ(page->events.size(), 2u);
    ASSERT_TRUE(page->completion);
    EXPECT_TRUE(page->completion->complete);
    EXPECT_FALSE(page->answer_complete);
    EXPECT_EQ(page->delivered_prefix_end, (chronolog::Hlc{30, 0}));
    EXPECT_EQ(page->after->hlc, (chronolog::Hlc{10, 0}));
    EXPECT_EQ(page->completion_range->start, (chronolog::Hlc{}));
    ASSERT_TRUE(page->next_cursor);
    std::vector<uint64_t> ids{1, 2};
    for(int call = 0; call < 3; ++call)
    {
        options.cursor = page->next_cursor;
        page = (*session)->recall(options);
        ASSERT_TRUE(page.ok());
        ASSERT_EQ(page->events.size(), 1u);
        ids.push_back(page->events[0].id.writer_id);
    }
    EXPECT_EQ(ids, (std::vector<uint64_t>{1, 2, 3, 4, 5}));
    EXPECT_TRUE(page->answer_complete);
    EXPECT_FALSE(page->next_cursor);
    options.cursor.reset();
    for(auto reason: {chronolog::IncompleteReason::SourceFailed,
                      chronolog::IncompleteReason::LaggingWriters,
                      chronolog::IncompleteReason::Truncated})
    {
        peer.read_reason = reason;
        peer.seal = 40;
        page = (*session)->recall(options);
        ASSERT_TRUE(page.ok());
        ASSERT_TRUE(page->completion);
        EXPECT_EQ(page->completion->reason, reason);
        EXPECT_FALSE(page->answer_complete);
        if(reason == chronolog::IncompleteReason::Truncated)
            EXPECT_TRUE(page->next_cursor);
        else
        {
            EXPECT_FALSE(page->after);
            EXPECT_FALSE(page->next_cursor);
        }
    }
    peer.read_reason = chronolog::IncompleteReason::None;
    peer.seal = 0;
    options.limits.max_raw_bytes = 1;
    page = (*session)->recall(options);
    ASSERT_TRUE(page.ok());
    EXPECT_EQ(page->limited, ctx::DeliveryLimit::OversizedEvent);
    ASSERT_EQ(page->events.size(), 2u);
    EXPECT_GT(page->raw_bytes, options.limits.max_raw_bytes);
}

TEST(ContextApi, FollowFromNowStartsAtOrBelowEverySeal)
{
    Peer peer;
    peer.expected_tails = 2;
    peer.seal = 200;
    peer.read_reason = chronolog::IncompleteReason::LaggingWriters;
    auto client = ctx::ContextClient::Connect(peer.options());
    ASSERT_TRUE(client.ok());
    ctx::OpenOptions ro;
    ro.access = ctx::Access::ReadOnly;
    auto a = client->open(ref(), {"agent", "slot"}, ro);
    auto b = client->open(ref(2), {"agent", "slot"}, ro);
    ASSERT_TRUE(a.ok());
    ASSERT_TRUE(b.ok());
    std::vector<ctx::FollowInput> inputs{{*a}, {*b}};
    auto result = client->follow(inputs);
    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_TRUE(result->status.ok()) << result->status;
    ASSERT_EQ(result->pages.size(), 2u);
    for(const auto& output: result->pages)
    {
        ASSERT_TRUE(output.starting_cut);
        EXPECT_LE(*output.starting_cut, (chronolog::Hlc{200, 0}));
        EXPECT_FALSE(output.page.answer_complete);
    }
    EXPECT_EQ(peer.reads.size(), 2u);
    for(const auto& request: peer.reads)
        EXPECT_LE(request.hlc().end().physical_ns() - request.hlc().start().physical_ns(), 1'000'000'000);
    EXPECT_EQ(peer.tails.size(), 2u);
    for(const auto& request: peer.tails) EXPECT_EQ(request.from().hlc().physical_ns(), 200);
}

TEST(ContextApi, FollowRefusesUncertifiedRoute)
{
    for(int mode = 0; mode < 2; ++mode)
    {
        Peer peer;
        peer.read_reason = chronolog::IncompleteReason::SourceFailed;
        peer.route_available = mode == 0;
        auto client = ctx::ContextClient::Connect(peer.options());
        ASSERT_TRUE(client.ok());
        ctx::OpenOptions ro;
        ro.access = ctx::Access::ReadOnly;
        auto session = client->open(ref(), {"agent", "slot"}, ro);
        ASSERT_TRUE(session.ok());
        const ctx::FollowInput input{*session};
        auto result = client->follow(std::span(&input, 1));
        ASSERT_TRUE(result.ok());
        EXPECT_FALSE(result->status.ok());
        EXPECT_FALSE(result->pages[0].starting_cut);
        EXPECT_FALSE(result->pages[0].resume);
        if(mode == 0)
        {
            EXPECT_EQ(result->pages[0].uncertified_route_keepers, (std::vector<std::string>{"fast", "slow"}));
        }
        EXPECT_TRUE(peer.tails.empty());
    }
    Peer peer;
    peer.failed_story = 2;
    peer.seal = 200;
    peer.read_reason = chronolog::IncompleteReason::LaggingWriters;
    auto client = ctx::ContextClient::Connect(peer.options());
    ASSERT_TRUE(client.ok());
    ctx::OpenOptions ro;
    ro.access = ctx::Access::ReadOnly;
    auto good = client->open(ref(), {"agent", "slot"}, ro);
    auto bad = client->open(ref(2), {"agent", "slot"}, ro);
    ASSERT_TRUE(good.ok());
    ASSERT_TRUE(bad.ok());
    const std::vector<ctx::FollowInput> inputs{{*good}, {*bad}};
    auto result = client->follow(inputs);
    ASSERT_TRUE(result.ok());
    EXPECT_FALSE(result->status.ok());
    EXPECT_EQ(result->pages[0].page.events.size(), 1u);
    EXPECT_FALSE(result->pages[1].resume);
    EXPECT_EQ(peer.tails.size(), 1u);
}
TEST(ContextApi, RecoveryNeverAdoptsARecordedWriter)
{
    Peer peer;
    auto config = peer.options();
    config.max_writable_sessions = 1;
    auto client = ctx::ContextClient::Connect(config);
    ASSERT_TRUE(client.ok());
    auto options = writable();
    options.resume = ctx::Checkpoint{};
    options.resume->context = ref();
    options.resume->identity = {"agent", "slot"};
    options.resume->writer = ctx::WriterStamp{7, 9};
    options.resume->acquisition = ctx::AcquisitionProvenance{};
    options.resume->acquisition->host_id = "host";
    options.resume->acquisition->launcher_lock_id = "lock";
    options.ownership = options.resume->acquisition;
    auto session = client->open(ref(), {"agent", "slot"}, options);
    ASSERT_TRUE(session.ok());
    EXPECT_EQ((*session)->status().state, ctx::SessionState::NeedsReconcile);
    auto result = (*session)->remember(memory("new"));
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result->current.outcome, ctx::MemoryOutcome::Fenced);
    EXPECT_TRUE(peer.acquisitions.empty());
    EXPECT_TRUE(peer.appends.empty());
    auto reconciled = (*session)->reconcile();
    ASSERT_TRUE(reconciled.ok()) << reconciled.status();
    ASSERT_EQ(peer.acquisitions.size(), 1u);
    EXPECT_TRUE(peer.acquisitions[0].takeover());
    EXPECT_EQ(peer.acquisitions[0].expected_prior_incarnation(), 9u);
    for(const auto& append: peer.appends)
        for(const auto& item: append.items()) EXPECT_NE(item.incarnation(), 9u);
    options.resume->context = ref(2);
    EXPECT_EQ(client->open(ref(2), {"agent", "slot"}, options).status().code(), absl::StatusCode::kResourceExhausted);
    ASSERT_TRUE((*session)->close().ok());
    options.ownership->host_id = "foreign";
    auto foreign = client->open(ref(2), {"agent", "slot"}, options);
    ASSERT_TRUE(foreign.ok());
    EXPECT_EQ((*foreign)->status().state, ctx::SessionState::Fenced);
    EXPECT_TRUE((*foreign)->checkpoint().takeover_required);
    ASSERT_TRUE((*foreign)->close().ok());
    options.resume->context = ref(3);
    options.resume->acquisition_closed = true;
    ASSERT_TRUE(client->open(ref(3), {"agent", "slot"}, options).ok());
    ASSERT_EQ(peer.acquisitions.size(), 2u);
    EXPECT_FALSE(peer.acquisitions[1].takeover());
    EXPECT_EQ(peer.acquisitions[1].expected_prior_incarnation(), 9u);
}

TEST(ContextApi, RecallVerifiesTheCutAndPreservesFailures)
{
    Peer peer;
    peer.seal = 80;
    peer.read_reason = chronolog::IncompleteReason::LaggingWriters;
    peer.verify_below_seal = true;
    auto client = ctx::ContextClient::Connect(peer.options());
    ASSERT_TRUE(client.ok());
    ctx::OpenOptions ro;
    ro.access = ctx::Access::ReadOnly;
    ro.resume = ctx::Checkpoint{};
    ro.resume->context = ref();
    ro.resume->identity = {"agent", "slot"};
    ro.resume->causal_floor = {100, 0};
    auto session = client->open(ref(), {"agent", "slot"}, ro);
    ASSERT_TRUE(session.ok());
    auto page = (*session)->recall();
    ASSERT_TRUE(page.ok());
    ASSERT_TRUE(page->range);
    EXPECT_EQ(page->range->end, (chronolog::Hlc{80, 0}));
    EXPECT_TRUE(page->answer_complete);
    EXPECT_FALSE(page->cut_covers_causal_floor);
    ASSERT_EQ(peer.reads.size(), 3u);
    EXPECT_EQ(peer.reads[1].hlc().end().physical_ns(), 80);
    EXPECT_EQ(peer.reads[2].hlc().start().physical_ns(), 0);
    peer.read_error = true;
    ctx::RecallOptions options;
    options.end = chronolog::Hlc{80, 0};
    page = (*session)->recall(options);
    ASSERT_TRUE(page.ok());
    EXPECT_EQ(page->stream_status.code(), absl::StatusCode::kUnavailable);
    EXPECT_FALSE(page->completion);
    EXPECT_FALSE(page->after);
    EXPECT_FALSE(page->next_cursor);
}

TEST(ContextApi, OperationCapacityAndCompletedEviction)
{
    Peer peer;
    auto options = peer.options();
    options.max_completed_operations = 1;
    options.max_unresolved_operations = 1;
    auto client = ctx::ContextClient::Connect(options);
    ASSERT_TRUE(client.ok());
    auto a = client->open(ref(), {"agent", "slot"}, writable());
    auto b = client->open(ref(2), {"agent", "slot"}, writable());
    ASSERT_TRUE(a.ok());
    ASSERT_TRUE(b.ok());
    ASSERT_TRUE((*a)->remember(memory("one")).ok());
    ASSERT_TRUE((*a)->remember(memory("two")).ok());
    auto repeated = (*a)->remember(memory("one"));
    ASSERT_TRUE(repeated.ok());
    EXPECT_EQ(repeated->current.receipt->event_id.sequence, 3u);
    peer.append_code = absl::StatusCode::kUnavailable;
    ASSERT_TRUE((*a)->remember(memory("pending")).ok());
    EXPECT_EQ((*b)->remember(memory("new")).status().code(), absl::StatusCode::kResourceExhausted);
    peer.append_code = absl::StatusCode::kOk;
    ASSERT_TRUE((*a)->remember(memory("pending")).ok());
    ASSERT_TRUE((*b)->remember(memory("new")).ok());
}

TEST(ContextApi, FollowLimitsUseTheDeliveredPositionAndPreserveRemoteDeadline)
{
    Peer peer;
    auto client = ctx::ContextClient::Connect(peer.options());
    ASSERT_TRUE(client.ok());
    ctx::OpenOptions ro;
    ro.access = ctx::Access::ReadOnly;
    auto session = client->open(ref(), {"agent", "slot"}, ro);
    ASSERT_TRUE(session.ok());
    ctx::FollowInput input{*session, ctx::FollowFrom::Beginning, {}};
    ctx::FollowOptions options;
    options.limits.max_raw_bytes = 1;
    auto result = client->follow(std::span(&input, 1), options);
    ASSERT_TRUE(result.ok());
    ASSERT_TRUE(result->status.ok());
    ASSERT_EQ(result->pages[0].page.events.size(), 1u);
    EXPECT_EQ(result->pages[0].page.limited, ctx::DeliveryLimit::OversizedEvent);
    EXPECT_EQ(result->pages[0].resume->id.sequence, 1u);
    peer.tail_error = true;
    input.from = ctx::FollowFrom::Position;
    input.after = result->pages[0].resume;
    result = client->follow(std::span(&input, 1));
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result->status.code(), absl::StatusCode::kDeadlineExceeded);
    EXPECT_FALSE(result->idle);
    EXPECT_EQ(result->pages[0].resume->id, input.after->id);
    EXPECT_TRUE((*session)->acknowledgeProcessed(*input.after).ok());
    EXPECT_TRUE((*session)->acknowledgeProcessed(*input.after).ok());
    EXPECT_EQ((*session)->acknowledgeProcessed({{}, {1, 0, 0, 0}}).code(), absl::StatusCode::kInvalidArgument);
}

TEST(ContextApi, EnsureResolvesLiveGenerationsAndRejectsBadIdentity)
{
    Peer peer;
    auto client = ctx::ContextClient::Connect(peer.options());
    ASSERT_TRUE(client.ok());
    auto existing = client->ensureContext("team", "notes");
    ASSERT_TRUE(existing.ok()) << existing.status();
    EXPECT_EQ(existing->story_id, 1u);
    auto created = client->ensureContext("team", "old");
    ASSERT_TRUE(created.ok());
    EXPECT_EQ(created->story_id, 3u);
    auto contexts = client->listContexts("team");
    ASSERT_TRUE(contexts.ok());
    ASSERT_EQ(contexts->size(), 1u);
    auto wrong = ref();
    wrong.name = "reused";
    EXPECT_EQ(client->open(wrong, {"agent", "slot"}).status().code(), absl::StatusCode::kFailedPrecondition);
    EXPECT_EQ(client->open(ref(), {std::string(1, static_cast<char>(255)), "slot"}).status().code(),
              absl::StatusCode::kInvalidArgument);
    auto session = client->open(ref(), {"agent", "slot"});
    ASSERT_TRUE(session.ok());
    auto invalid = memory("bad");
    invalid.envelope.attributes["gen_ai.agent.id"] = "other";
    EXPECT_EQ((*session)->remember(invalid).status().code(), absl::StatusCode::kInvalidArgument);
    invalid = memory("bad trace");
    invalid.envelope.trace_id = "short";
    EXPECT_EQ((*session)->remember(invalid).status().code(), absl::StatusCode::kInvalidArgument);
    auto binary = memory("bytes", std::string("a\0\xff", 3));
    auto result = (*session)->remember(binary);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result->current.outcome, ctx::MemoryOutcome::Durable);
    ASSERT_EQ(peer.appends.size(), 1u);
    EXPECT_EQ(peer.appends[0].items(0).envelope().payload(), binary.envelope.payload);
    EXPECT_FALSE(peer.appends[0].items(0).envelope().attributes().at("gen_ai.conversation.id").empty());
}

TEST(ContextApi, FollowIdleRetainsItsBoundary)
{
    Peer peer;
    peer.idle_tail = true;
    auto client = ctx::ContextClient::Connect(peer.options());
    ASSERT_TRUE(client.ok());
    ctx::OpenOptions ro;
    ro.access = ctx::Access::ReadOnly;
    auto session = client->open(ref(), {"agent", "slot"}, ro);
    ASSERT_TRUE(session.ok());
    const ctx::FollowInput input{*session, ctx::FollowFrom::Position, ctx::Position{{30, 0}, {1, 2, 1, 9}}};
    ctx::FollowOptions options;
    options.wait = 0s;
    auto result = client->follow(std::span(&input, 1), options);
    ASSERT_TRUE(result.ok());
    EXPECT_TRUE(result->status.ok());
    EXPECT_TRUE(result->idle);
    ASSERT_TRUE(result->pages[0].resume);
    EXPECT_EQ(result->pages[0].resume->id, input.after->id);
    EXPECT_FALSE(result->pages[0].page.answer_complete);
    EXPECT_TRUE(result->pages[0].page.events.empty());
}

// A Replay over a fixed story: archived records of record_ns are admitted whole, hot events by HLC group, and
// a Read past the seal or over a failed source answers like the Player (I6.12 TRUNCATED, LAGGING_WRITERS, SOURCE_FAILED).
class StoryPeer: public Peer
{
public:
    struct Row
    {
        int64_t physical;
        uint32_t logical;
        uint64_t writer;
        std::string type;
    };
    static constexpr int64_t s = 1'000'000'000;
    StoryPeer()
    {
        uint64_t writer = 1;
        auto add = [&](int64_t physical, std::string type = "memory")
        { rows.push_back({physical, 0, writer++, std::move(type)}); };
        for(int64_t p = 3 * s; p < 900 * s; p += 7 * s) add(p, p == 304 * s ? "aggregate" : "memory");
        for(int i = 0; i < 3; ++i) add(450 * s);
        for(int i = 0; i < 25; ++i) add(500 * s + i * (s / 3) + 1);
        for(int64_t p = 900 * s; p < 1000 * s; p += s) add(p);
        for(int i = 0; i < 12; ++i) add(995 * s + s / 2);
        for(int i = 0; i < 30; ++i) add(999 * s + s / 2 + i * (s / 100));
        std::sort(rows.begin(),
                  rows.end(),
                  [](const Row& a, const Row& b)
                  { return std::tie(a.physical, a.logical, a.writer) < std::tie(b.physical, b.logical, b.writer); });
    }
    grpc::Status Read(grpc::ServerContext*,
                      const wire::ReadRequest* request,
                      grpc::ServerWriter<wire::ReadResponse>* stream) override
    {
        std::lock_guard lock(mutex);
        reads.push_back(*request);
        const chronolog::Hlc start{request->hlc().start().physical_ns(), request->hlc().start().logical()};
        const chronolog::Hlc end{request->hlc().end().physical_ns(), request->hlc().end().logical()};
        const size_t target = request->max_events() ? request->max_events() : 1000;
        const bool lagging = end > chronolog::Hlc{seal, 0};
        const bool failed = start.physical_ns < fail_end && end.physical_ns > fail_start;
        wire::ReadResponse response;
        std::optional<chronolog::Hlc> cut;
        std::optional<chronolog::Hlc> previous;
        size_t admitted = 0;
        for(const auto& row: rows)
        {
            const chronolog::Hlc hlc{row.physical, row.logical};
            if(hlc < start || hlc >= end || (lagging && hlc >= chronolog::Hlc{seal, 0}) ||
               (failed && row.physical >= fail_start && row.physical < fail_end))
                continue;
            const bool archived = row.physical < archive_end;
            if(!lagging && !failed && admitted >= target && previous && hlc != *previous &&
               (!archived || row.physical / record_ns != previous->physical_ns / record_ns))
            {
                cut = archived ? std::max(start, chronolog::Hlc{row.physical / record_ns * record_ns, 0}) : hlc;
                break;
            }
            auto* event = response.mutable_batch()->add_events();
            event->mutable_id()->set_story_id(request->story_id());
            event->mutable_id()->set_writer_id(row.writer);
            event->mutable_id()->set_incarnation(1);
            event->mutable_id()->set_sequence(1);
            event->mutable_hlc()->set_physical_ns(row.physical);
            event->mutable_hlc()->set_logical(row.logical);
            event->mutable_envelope()->set_content_type(row.type);
            event->mutable_envelope()->set_payload("payload");
            previous = hlc;
            ++admitted;
        }
        if(admitted)
            stream->Write(response);
        response.Clear();
        auto* completion = response.mutable_completion();
        auto reason = chronolog::IncompleteReason::None;
        chronolog::Hlc frontier = end;
        if(failed)
            reason = chronolog::IncompleteReason::SourceFailed, frontier = start;
        else if(lagging)
            reason = chronolog::IncompleteReason::LaggingWriters, frontier = {seal, 0};
        else if(cut)
            reason = chronolog::IncompleteReason::Truncated, frontier = *cut;
        completion->set_complete(reason == chronolog::IncompleteReason::None);
        completion->set_reason(static_cast<wire::IncompleteReason>(reason));
        completion->mutable_frontier()->set_physical_ns(frontier.physical_ns);
        completion->mutable_frontier()->set_logical(frontier.logical);
        stream->Write(response);
        return grpc::Status::OK;
    }
    std::vector<uint64_t> suffix(size_t n, chronolog::Hlc before = {1000 * s, 0}) const
    {
        std::vector<uint64_t> ids;
        for(const auto& row: rows)
            if(chronolog::Hlc{row.physical, row.logical} < before)
                ids.push_back(row.writer);
        ids.erase(ids.begin(), ids.end() - static_cast<ptrdiff_t>(std::min(n, ids.size())));
        return ids;
    }
    std::vector<Row> rows;
    int64_t seal{1000 * s};
    int64_t archive_end{900 * s};
    int64_t record_ns{10 * s};
    int64_t fail_start{-1};
    int64_t fail_end{-1};
};
std::vector<uint64_t> writers(const ctx::Page& page)
{
    std::vector<uint64_t> ids;
    for(const auto& event: page.events) ids.push_back(event.id.writer_id);
    return ids;
}
std::shared_ptr<ctx::ContextSession> reader(ctx::ContextClient& client)
{
    ctx::OpenOptions ro;
    ro.access = ctx::Access::ReadOnly;
    return *client.open(ref(), {"agent", "slot"}, ro);
}
std::vector<uint64_t> fullRead(ctx::ContextSession& session, chronolog::Hlc end)
{
    ctx::RecallOptions options;
    options.end = end;
    options.limits.max_events = 16;
    std::vector<uint64_t> ids;
    for(int call = 0; call < 200; ++call)
    {
        auto page = session.recall(options);
        EXPECT_TRUE(page.ok());
        for(auto id: writers(*page)) ids.push_back(id);
        if(page->answer_complete || !page->next_cursor)
        {
            EXPECT_TRUE(page->answer_complete);
            return ids;
        }
        options.cursor = page->next_cursor;
    }
    ADD_FAILURE() << "full read did not finish";
    return ids;
}

TEST(ContextApi, LatestAtVerifiedCut)
{
    StoryPeer peer;
    auto options = peer.options();
    options.cut_probe_width = 0s;
    EXPECT_EQ(ctx::ContextClient::Connect(options).status().code(), absl::StatusCode::kInvalidArgument);
    auto sdk = chronolog::client::Client::Connect(options.sdk);
    ASSERT_TRUE(sdk.ok());
    EXPECT_EQ(ctx::detail::latestAggregate(*sdk, options, 1, {}).status().code(), absl::StatusCode::kInvalidArgument);
    auto client = ctx::ContextClient::Connect(peer.options());
    ASSERT_TRUE(client.ok());
    auto session = reader(*client);
    ctx::LatestOptions latest;
    latest.limits.max_events = 4;
    EXPECT_EQ(session->latest(5, latest).status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_EQ(session->latest(0, latest).status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_TRUE(peer.reads.empty());
    // The realtime cut is past the seal: the lagging probe is re-verified below the seal, then as_of is the seal.
    auto result = session->latest(3);
    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_TRUE(result->as_of);
    EXPECT_EQ(*result->as_of, (chronolog::Hlc{peer.seal, 0}));
    EXPECT_TRUE(result->selection_complete);
    EXPECT_TRUE(result->page.answer_complete);
    EXPECT_EQ(writers(result->page), peer.suffix(3));
    EXPECT_EQ(result->page.range->end, *result->as_of);
    EXPECT_EQ(result->page.range->start, chronolog::Hlc{});
    ASSERT_GE(peer.reads.size(), 2u);
    EXPECT_GT(peer.reads[0].hlc().end().physical_ns(), peer.seal);
    EXPECT_LE(peer.reads[0].hlc().end().physical_ns() - peer.reads[0].hlc().start().physical_ns(), StoryPeer::s);
    EXPECT_EQ(peer.reads[1].hlc().end().physical_ns(), peer.seal);
    // An explicit before fixes as_of with no probe.
    peer.reads.clear();
    latest.before = chronolog::Hlc{950 * StoryPeer::s, 0};
    latest.limits.max_events = 1000;
    result = session->latest(2, latest);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result->as_of, latest.before);
    EXPECT_EQ(writers(result->page), peer.suffix(2, *latest.before));
    EXPECT_EQ(peer.reads[0].hlc().end().physical_ns(), 950 * StoryPeer::s);
    EXPECT_EQ(result->page.completion_range->end.physical_ns, peer.reads.back().hlc().end().physical_ns());
    EXPECT_LT(result->page.completion_range->end, *latest.before);
    latest.before = chronolog::Hlc{};
    result = session->latest(2, latest);
    ASSERT_TRUE(result.ok());
    EXPECT_TRUE(result->selection_complete);
    EXPECT_TRUE(result->page.events.empty());
}

TEST(ContextApi, LatestMatchesTheSuffixOfAFullReadOverHotAndArchivedHistory)
{
    StoryPeer peer;
    auto client = ctx::ContextClient::Connect(peer.options());
    ASSERT_TRUE(client.ok());
    auto session = reader(*client);
    const chronolog::Hlc seal{peer.seal, 0};
    for(chronolog::Hlc before: {seal,
                                chronolog::Hlc{995 * StoryPeer::s + StoryPeer::s / 2 + 1, 0},
                                chronolog::Hlc{505 * StoryPeer::s, 0},
                                chronolog::Hlc{451 * StoryPeer::s, 0}})
    {
        const auto full = fullRead(*session, before);
        ASSERT_EQ(full, peer.suffix(peer.rows.size(), before));
        for(size_t target: {8u, 1000u})
            for(size_t n: {1u, 2u, 5u, 8u, 13u, 40u, 120u, 400u})
            {
                if(n > target)
                    continue;
                ctx::LatestOptions latest;
                latest.before = before;
                latest.limits.max_events = target;
                auto result = session->latest(n, latest);
                ASSERT_TRUE(result.ok());
                ASSERT_TRUE(result->selection_complete) << n << " " << target << " " << before.physical_ns;
                const std::vector<uint64_t> expected(full.end() - static_cast<ptrdiff_t>(std::min(n, full.size())),
                                                     full.end());
                EXPECT_EQ(writers(result->page), expected) << n << " " << target << " " << before.physical_ns;
            }
    }
    // Fewer than n is proven only by a complete range reaching 0.
    peer.reads.clear();
    ctx::LatestOptions latest;
    latest.limits.max_events = 1000;
    auto result = session->latest(peer.rows.size() + 2, latest);
    ASSERT_TRUE(result.ok());
    EXPECT_TRUE(result->selection_complete);
    EXPECT_EQ(result->page.events.size(), peer.rows.size());
    EXPECT_EQ(result->page.completion_range->start, chronolog::Hlc{});
    EXPECT_LE(peer.reads.size(), 32u);
    // Search Reads walk backward: every window ends where the previous complete one began.
    for(size_t i = 3; i < peer.reads.size(); ++i)
        EXPECT_EQ(peer.reads[i].hlc().end().physical_ns(), peer.reads[i - 1].hlc().start().physical_ns());
}

TEST(ContextApi, LatestNarrowsTruncatedSuffixesAndKeepsEqualHlcGroupsWhole)
{
    StoryPeer peer;
    auto client = ctx::ContextClient::Connect(peer.options());
    ASSERT_TRUE(client.ok());
    auto session = reader(*client);
    ctx::LatestOptions latest;
    latest.before = chronolog::Hlc{peer.seal, 0};
    latest.limits.max_events = 4;
    auto result = session->latest(4, latest);
    ASSERT_TRUE(result.ok());
    EXPECT_TRUE(result->selection_complete);
    EXPECT_EQ(writers(result->page), peer.suffix(4));
    // The first suffix is TRUNCATED; the next Read raises m to the midpoint to max(c, midpoint).
    ASSERT_GE(peer.reads.size(), 2u);
    EXPECT_EQ(peer.reads[1].hlc().end().physical_ns(), peer.seal);
    EXPECT_GE(peer.reads[1].hlc().start().physical_ns(), 999 * StoryPeer::s + StoryPeer::s / 2);
    EXPECT_GT(peer.reads[1].hlc().start().physical_ns(), peer.reads[0].hlc().start().physical_ns());
    EXPECT_LE(peer.reads.size(), 12u);
    for(const auto& request: peer.reads) EXPECT_GE(request.hlc().start().physical_ns(), 999 * StoryPeer::s);
    // A 12-event group at one HLC exceeds the target of 4; it is read whole and its last members are selected.
    const chronolog::Hlc group{995 * StoryPeer::s + StoryPeer::s / 2, 0};
    peer.reads.clear();
    latest.before = chronolog::Hlc{group.physical_ns + 1, 0};
    result = session->latest(4, latest);
    ASSERT_TRUE(result.ok());
    EXPECT_TRUE(result->selection_complete);
    EXPECT_EQ(writers(result->page), peer.suffix(4, *latest.before));
    for(const auto& event: result->page.events) EXPECT_EQ(event.hlc, group);
    // Group members straddling the proven suffix are neither duplicated nor dropped.
    latest.limits.max_events = 16;
    for(size_t n: {13u, 14u, 16u})
    {
        latest.before = chronolog::Hlc{996 * StoryPeer::s + 1, 0};
        result = session->latest(n, latest);
        ASSERT_TRUE(result.ok());
        EXPECT_TRUE(result->selection_complete);
        EXPECT_EQ(writers(result->page), peer.suffix(n, *latest.before));
    }
}

TEST(ContextApi, LatestIncompleteSuffixIsProvisional)
{
    StoryPeer peer;
    peer.fail_start = 100 * StoryPeer::s;
    peer.fail_end = 200 * StoryPeer::s;
    auto client = ctx::ContextClient::Connect(peer.options());
    ASSERT_TRUE(client.ok());
    auto session = reader(*client);
    ctx::LatestOptions latest;
    latest.limits.max_events = 1000;
    auto result = session->latest(5, latest);
    ASSERT_TRUE(result.ok());
    EXPECT_TRUE(result->selection_complete);
    EXPECT_EQ(writers(result->page), peer.suffix(5));
    result = session->latest(peer.rows.size(), latest);
    ASSERT_TRUE(result.ok());
    EXPECT_FALSE(result->selection_complete);
    EXPECT_FALSE(result->page.answer_complete);
    EXPECT_TRUE(result->page.has_more);
    ASSERT_TRUE(result->page.completion);
    EXPECT_EQ(result->page.completion->reason, chronolog::IncompleteReason::SourceFailed);
    EXPECT_FALSE(result->page.completion_range->start == chronolog::Hlc{} &&
                 result->page.completion_range->end == *result->as_of);
    EXPECT_GT(result->page.events.size(), 100u);
    EXPECT_LT(result->page.events.size(), peer.rows.size());
    EXPECT_FALSE(result->page.after);
    EXPECT_FALSE(result->page.next_cursor);
    for(const auto& event: result->page.events)
        EXPECT_FALSE(event.hlc.physical_ns >= peer.fail_start && event.hlc.physical_ns < peer.fail_end);
    // A suffix past the seal is LAGGING_WRITERS: candidates are returned, the selection is not claimed.
    latest.before = chronolog::Hlc{peer.seal + 1, 0};
    result = session->latest(10, latest);
    ASSERT_TRUE(result.ok());
    EXPECT_FALSE(result->selection_complete);
    EXPECT_EQ(result->page.completion->reason, chronolog::IncompleteReason::LaggingWriters);
    EXPECT_EQ(writers(result->page), peer.suffix(10));
    // A failed verified-cut probe yields no as_of and no candidates.
    latest.before.reset();
    peer.fail_start = 0;
    peer.fail_end = 2000 * StoryPeer::s;
    result = session->latest(10, latest);
    ASSERT_TRUE(result.ok());
    EXPECT_FALSE(result->as_of);
    EXPECT_FALSE(result->selection_complete);
    EXPECT_TRUE(result->page.events.empty());
    EXPECT_EQ(result->page.completion->reason, chronolog::IncompleteReason::SourceFailed);
}

TEST(ContextApi, LatestReportsReadCallAndByteExhaustion)
{
    StoryPeer peer;
    auto client = ctx::ContextClient::Connect(peer.options());
    ASSERT_TRUE(client.ok());
    auto session = reader(*client);
    ctx::LatestOptions latest;
    latest.limits.max_events = 1000;
    latest.max_read_calls = 4;
    auto result = session->latest(200, latest);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(peer.reads.size(), 4u);
    EXPECT_EQ(result->page.limited, ctx::DeliveryLimit::ReadCalls);
    EXPECT_FALSE(result->selection_complete);
    EXPECT_FALSE(result->page.answer_complete);
    EXPECT_FALSE(result->page.events.empty());
    EXPECT_EQ(writers(result->page), peer.suffix(result->page.events.size()));
    latest.max_read_calls = 32;
    latest.limits.max_raw_bytes = 40;
    result = session->latest(10, latest);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result->page.limited, ctx::DeliveryLimit::Bytes);
    EXPECT_FALSE(result->selection_complete);
    EXPECT_FALSE(result->page.events.empty());
    EXPECT_LT(result->page.events.size(), 10u);
    EXPECT_LE(result->page.raw_bytes, latest.limits.max_raw_bytes);
    EXPECT_EQ(writers(result->page), peer.suffix(result->page.events.size()));
    latest.limits.max_raw_bytes = 1;
    result = session->latest(1, latest);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result->page.limited, ctx::DeliveryLimit::OversizedEvent);
    EXPECT_TRUE(result->selection_complete);
    EXPECT_EQ(writers(result->page), peer.suffix(1));
}

TEST(ContextApi, LatestAggregateLookupDoesNotDrainOldHistory)
{
    StoryPeer peer;
    auto sdk = chronolog::client::Client::Connect(peer.options().sdk);
    ASSERT_TRUE(sdk.ok());
    const auto config = peer.options();
    auto aggregate = [](const chronolog::Event& event) { return event.envelope.content_type == "aggregate"; };
    auto result = ctx::detail::latestAggregate(*sdk, config, 1, aggregate);
    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_TRUE(result->selection_complete);
    ASSERT_EQ(result->page.events.size(), 1u);
    EXPECT_EQ(result->page.events[0].hlc.physical_ns, 304 * StoryPeer::s);
    EXPECT_LE(peer.reads.size(), 14u);
    for(const auto& request: peer.reads) EXPECT_GT(request.hlc().end().physical_ns(), 304 * StoryPeer::s);
    {
        std::lock_guard lock(peer.mutex);
        peer.rows.insert(std::find_if(peer.rows.begin(),
                                      peer.rows.end(),
                                      [](const auto& row) { return row.physical > 998 * StoryPeer::s; }),
                         {998 * StoryPeer::s, 0, 9999, "aggregate"});
        peer.reads.clear();
    }
    result = ctx::detail::latestAggregate(*sdk, config, 1, aggregate);
    ASSERT_TRUE(result.ok());
    EXPECT_TRUE(result->selection_complete);
    ASSERT_EQ(result->page.events.size(), 1u);
    EXPECT_EQ(result->page.events[0].id.writer_id, 9999u);
    for(const auto& request: peer.reads) EXPECT_GE(request.hlc().start().physical_ns(), 990 * StoryPeer::s);
    ctx::LatestOptions bounded;
    bounded.max_read_calls = 3;
    {
        std::lock_guard lock(peer.mutex);
        std::erase_if(peer.rows, [](const auto& row) { return row.writer == 9999; });
    }
    result = ctx::detail::latestAggregate(*sdk, config, 1, aggregate, bounded);
    ASSERT_TRUE(result.ok());
    EXPECT_FALSE(result->selection_complete);
    EXPECT_EQ(result->page.limited, ctx::DeliveryLimit::ReadCalls);
    EXPECT_TRUE(result->page.events.empty());
}

} // namespace

// A Catalog and Keeper over one story log: Acquire checks expected_prior_incarnation, fenced incarnations answer
// their typed cause before dedupe (I3.7), a response can be lost after its event lands, and a Read over a failed
// source answers SOURCE_FAILED.
class LogPeer: public Peer
{
public:
    static constexpr uint64_t writer = 7;
    grpc::Status Acquire(grpc::ServerContext*, const wire::AcquireRequest* request, wire::AcquireResponse* p) override
    {
        std::lock_guard lock(mutex);
        acquisitions.push_back(*request);
        if(request->has_expected_prior_incarnation() && request->expected_prior_incarnation() != incarnation)
        {
            p->mutable_status()->set_code(static_cast<int>(absl::StatusCode::kFailedPrecondition));
            p->set_refusal_reason(wire::ACQUIRE_REFUSAL_REASON_PRIOR_MISMATCH);
            p->set_current_incarnation(incarnation);
            return grpc::Status::OK;
        }
        p->set_story_id(request->story_id());
        p->set_writer_id(writer);
        p->set_incarnation(++incarnation);
        *p->mutable_route() = route();
        *p->mutable_assigned_keeper() = p->route().keepers(0);
        return grpc::Status::OK;
    }
    grpc::Status
    AppendStream(grpc::ServerContext*,
                 grpc::ServerReaderWriter<wire::AppendStreamResponse, wire::AppendStreamRequest>* stream) override
    {
        wire::AppendStreamRequest request;
        while(stream->Read(&request))
        {
            std::lock_guard lock(mutex);
            appends.push_back(request);
            wire::AppendStreamResponse response;
            response.set_batch_id(request.batch_id());
            for(const auto& item: request.items())
            {
                auto* result = response.add_results();
                if(auto cause = fenced.find(item.incarnation()); cause != fenced.end())
                {
                    result->mutable_status()->set_code(static_cast<int>(absl::StatusCode::kFailedPrecondition));
                    result->set_rejection(static_cast<wire::AppendRejection>(cause->second));
                    continue;
                }
                auto stored = std::find_if(log.begin(),
                                           log.end(),
                                           [&](const wire::Event& e) {
                                               return e.id().incarnation() == item.incarnation() &&
                                                      e.id().sequence() == item.sequence();
                                           });
                if(stored == log.end())
                {
                    clock = std::max(clock, item.causal_floor().physical_ns()) + 1;
                    wire::Event event;
                    event.mutable_id()->set_story_id(request.story_id());
                    event.mutable_id()->set_writer_id(item.writer_id());
                    event.mutable_id()->set_incarnation(item.incarnation());
                    event.mutable_id()->set_sequence(item.sequence());
                    event.mutable_hlc()->set_physical_ns(clock);
                    *event.mutable_envelope() = item.envelope();
                    event.set_durability(request.durability());
                    log.push_back(event);
                    stored = log.end() - 1;
                }
                if(lose_next)
                {
                    lose_next = false;
                    result->mutable_status()->set_code(static_cast<int>(absl::StatusCode::kUnavailable));
                    continue;
                }
                *result->mutable_id() = stored->id();
                *result->mutable_assigned_hlc() = stored->hlc();
                result->set_achieved_durability(stored->durability());
            }
            if(!stream->Write(response))
                break;
        }
        return grpc::Status::OK;
    }
    grpc::Status Read(grpc::ServerContext*,
                      const wire::ReadRequest* request,
                      grpc::ServerWriter<wire::ReadResponse>* stream) override
    {
        std::lock_guard lock(mutex);
        reads.push_back(*request);
        wire::ReadResponse response;
        auto sorted = log;
        std::sort(sorted.begin(),
                  sorted.end(),
                  [](const wire::Event& a, const wire::Event& b)
                  {
                      return std::make_tuple(a.hlc().physical_ns(), a.id().incarnation(), a.id().sequence()) <
                             std::make_tuple(b.hlc().physical_ns(), b.id().incarnation(), b.id().sequence());
                  });
        if(!failed_source)
            for(const auto& event: sorted)
                if(event.id().story_id() == request->story_id() &&
                   event.hlc().physical_ns() >= request->hlc().start().physical_ns() &&
                   event.hlc().physical_ns() < request->hlc().end().physical_ns())
                    *response.mutable_batch()->add_events() = event;
        if(response.batch().events_size())
            stream->Write(response);
        response.Clear();
        auto* completion = response.mutable_completion();
        completion->set_complete(!failed_source);
        completion->set_reason(failed_source ? wire::INCOMPLETE_REASON_SOURCE_FAILED
                                             : wire::INCOMPLETE_REASON_UNSPECIFIED);
        *completion->mutable_frontier() = failed_source ? request->hlc().start() : request->hlc().end();
        stream->Write(response);
        return grpc::Status::OK;
    }
    // Another holder of the slot superseded incarnation old; the Catalog now names a newer one.
    void supersede(uint64_t old)
    {
        std::lock_guard lock(mutex);
        fenced[old] = chronolog::AppendRejection::FencedSuperseded;
        ++incarnation;
    }
    void fence(uint64_t old, chronolog::AppendRejection cause)
    {
        std::lock_guard lock(mutex);
        fenced[old] = cause;
    }
    void inject(uint64_t inc, int64_t hlc, const std::string& operation)
    {
        std::lock_guard lock(mutex);
        wire::Event event;
        event.mutable_id()->set_story_id(1);
        event.mutable_id()->set_writer_id(writer);
        event.mutable_id()->set_incarnation(inc);
        event.mutable_id()->set_sequence(1000 + log.size());
        event.mutable_hlc()->set_physical_ns(hlc);
        (*event.mutable_envelope()->mutable_attributes())["chronolog.operation.id"] = operation;
        event.set_durability(wire::DURABILITY_DURABLE);
        log.push_back(event);
        clock = std::max(clock, hlc);
    }
    size_t stored(const std::string& operation)
    {
        std::lock_guard lock(mutex);
        return std::count_if(log.begin(),
                             log.end(),
                             [&](const wire::Event& e)
                             {
                                 auto found = e.envelope().attributes().find("chronolog.operation.id");
                                 return found != e.envelope().attributes().end() && found->second == operation;
                             });
    }
    std::vector<uint64_t> incarnations()
    {
        std::lock_guard lock(mutex);
        std::vector<uint64_t> result;
        for(const auto& append: appends)
            for(const auto& item: append.items()) result.push_back(item.incarnation());
        return result;
    }
    std::vector<wire::Event> log;
    std::map<uint64_t, chronolog::AppendRejection> fenced;
    uint64_t incarnation{};
    int64_t clock{1000};
    bool lose_next{false};
    bool failed_source{false};
};
const ctx::ReconciledOperation* outcome(const ctx::ReconcileResult& result, const std::string& id)
{
    for(const auto& op: result.operations)
        if(op.operation_id == id)
            return &op;
    return nullptr;
}
ctx::ReconcileOptions takeover()
{
    ctx::ReconcileOptions options;
    options.takeover = true;
    return options;
}
ctx::RememberOptions resend()
{
    ctx::RememberOptions options;
    options.resend_after_absent = true;
    return options;
}

TEST(ContextApi, RememberAndReconcile)
{
    // An fsync'd append whose response was lost is retried after supersession, answers FENCED and stays UNKNOWN;
    // takeover proof finds it LANDED and repeating its id never appends again.
    {
        LogPeer peer;
        auto client = ctx::ContextClient::Connect(peer.options());
        ASSERT_TRUE(client.ok());
        auto session = client->open(ref(), {"agent", "slot"}, writable());
        ASSERT_TRUE(session.ok()) << session.status();
        auto before = (*session)->remember(memory("before"));
        ASSERT_TRUE(before.ok() && before->current.receipt);
        peer.lose_next = true;
        auto a = (*session)->remember(memory("A"));
        ASSERT_TRUE(a.ok());
        EXPECT_EQ(a->current.outcome, ctx::MemoryOutcome::Unknown);
        EXPECT_EQ(a->state, ctx::SessionState::TransportPending);
        peer.supersede(1);
        auto b = (*session)->remember(memory("B"));
        ASSERT_TRUE(b.ok());
        EXPECT_EQ(b->blocking_operation_id, "A");
        EXPECT_EQ(b->state, ctx::SessionState::Fenced);
        EXPECT_EQ((*session)->remember(memory("A")).value().current.outcome, ctx::MemoryOutcome::Unknown);
        EXPECT_EQ((*session)->reconcile().status().code(), absl::StatusCode::kFailedPrecondition);
        EXPECT_EQ(peer.acquisitions.size(), 1u);
        // CAS on the known prior meets the newer holder: PRIOR_MISMATCH names it and nothing changes.
        auto mismatch = (*session)->reconcile(takeover());
        ASSERT_TRUE(mismatch.ok()) << mismatch.status();
        EXPECT_FALSE(mismatch->attempted);
        auto refusal = chronolog::client::acquireRefusalOf(mismatch->status);
        ASSERT_TRUE(refusal);
        EXPECT_EQ(refusal->current_incarnation, 2u);
        EXPECT_EQ((*session)->status().state, ctx::SessionState::Fenced);
        auto reconciled = (*session)->reconcile(takeover());
        ASSERT_TRUE(reconciled.ok()) << reconciled.status();
        ASSERT_EQ(peer.acquisitions.size(), 3u);
        EXPECT_TRUE(peer.acquisitions[1].takeover());
        EXPECT_EQ(peer.acquisitions[1].expected_prior_incarnation(), 1u);
        EXPECT_TRUE(peer.acquisitions[2].takeover());
        EXPECT_EQ(peer.acquisitions[2].expected_prior_incarnation(), 2u);
        EXPECT_NE(peer.acquisitions[1].acquire_request_id(), peer.acquisitions[2].acquire_request_id());
        EXPECT_TRUE(reconciled->attempted);
        EXPECT_TRUE(reconciled->proof_complete);
        EXPECT_TRUE(reconciled->supply_all_unseen_operation_ids);
        EXPECT_TRUE(reconciled->omitted_operation_ids_may_duplicate);
        ASSERT_TRUE(reconciled->writer);
        EXPECT_EQ(reconciled->writer->incarnation, 3u);
        ASSERT_TRUE(reconciled->range);
        EXPECT_EQ(reconciled->range->start, before->current.receipt->hlc);
        const auto* landed = outcome(*reconciled, "A");
        ASSERT_TRUE(landed);
        EXPECT_EQ(landed->outcome, ctx::ReconcileOutcome::Landed);
        ASSERT_TRUE(landed->landed);
        EXPECT_EQ(landed->landed->id.incarnation, 1u);
        EXPECT_EQ(landed->observed_durability, chronolog::Durability::Durable);
        EXPECT_FALSE(outcome(*reconciled, "B"));
        auto repeated = (*session)->remember(memory("A"));
        ASSERT_TRUE(repeated.ok());
        EXPECT_EQ(repeated->current.outcome, ctx::MemoryOutcome::Landed);
        EXPECT_FALSE(repeated->current.receipt);
        EXPECT_EQ(peer.stored("A"), 1u);
        auto c = (*session)->remember(memory("C"));
        ASSERT_TRUE(c.ok() && c->current.receipt);
        EXPECT_EQ(c->current.receipt->event_id.incarnation, 3u);
        EXPECT_TRUE((*session)->status().reconcile_attempted);
        // A second restart reloads the LANDED disposition: the id is answered without another append.
        auto saved = ctx::encodeCheckpoint((*session)->checkpoint());
        ASSERT_TRUE(saved.ok()) << saved.status();
        ASSERT_TRUE((*session)->close().ok());
        auto restored = ctx::decodeCheckpoint(*saved);
        ASSERT_TRUE(restored.ok()) << restored.status();
        auto restart = ctx::ContextClient::Connect(peer.options());
        ASSERT_TRUE(restart.ok());
        auto options = writable();
        options.resume = *restored;
        options.resume->acquisition_closed = true;
        auto reopened = restart->open(ref(), {"agent", "slot"}, options);
        ASSERT_TRUE(reopened.ok()) << reopened.status();
        EXPECT_EQ(peer.acquisitions.back().expected_prior_incarnation(), 3u);
        EXPECT_EQ((*reopened)->remember(memory("A", "changed")).status().code(), absl::StatusCode::kFailedPrecondition);
        repeated = (*reopened)->remember(memory("A"));
        ASSERT_TRUE(repeated.ok());
        EXPECT_EQ(repeated->current.outcome, ctx::MemoryOutcome::Landed);
        EXPECT_EQ(peer.stored("A"), 1u);
    }
    // EXPIRED recovers by a plain conditional Acquire; a fenced marker extends the transition through an
    // intermediate incarnation, the lower bound stays the earliest, and a proven ABSENT id is resent explicitly.
    {
        LogPeer peer;
        auto client = ctx::ContextClient::Connect(peer.options());
        ASSERT_TRUE(client.ok());
        auto session = client->open(ref(), {"agent", "slot"}, writable());
        ASSERT_TRUE(session.ok());
        auto x = (*session)->remember(memory("x"));
        ASSERT_TRUE(x.ok() && x->current.receipt);
        peer.fence(1, chronolog::AppendRejection::FencedExpired);
        auto a = (*session)->remember(memory("A"));
        ASSERT_TRUE(a.ok());
        EXPECT_EQ(a->current.outcome, ctx::MemoryOutcome::Unknown);
        EXPECT_EQ(a->state, ctx::SessionState::NeedsReconcile);
        peer.fence(2, chronolog::AppendRejection::FencedOwnerRemoved);
        auto first = (*session)->reconcile();
        ASSERT_TRUE(first.ok()) << first.status();
        EXPECT_FALSE(first->attempted);
        EXPECT_EQ(chronolog::client::rejectionOf(first->status), chronolog::AppendRejection::FencedOwnerRemoved);
        EXPECT_EQ((*session)->status().state, ctx::SessionState::NeedsReconcile);
        EXPECT_EQ((*session)->remember(memory("new")).value().current.outcome, ctx::MemoryOutcome::Fenced);
        auto second = (*session)->reconcile();
        ASSERT_TRUE(second.ok()) << second.status();
        ASSERT_EQ(peer.acquisitions.size(), 3u);
        EXPECT_FALSE(peer.acquisitions[1].takeover());
        EXPECT_EQ(peer.acquisitions[1].expected_prior_incarnation(), 1u);
        EXPECT_FALSE(peer.acquisitions[2].takeover());
        EXPECT_EQ(peer.acquisitions[2].expected_prior_incarnation(), 2u);
        EXPECT_TRUE(second->proof_complete);
        ASSERT_TRUE(second->range && second->marker_hlc);
        EXPECT_EQ(second->range->start, x->current.receipt->hlc);
        EXPECT_EQ(second->range->end, *second->marker_hlc);
        ASSERT_TRUE(outcome(*second, "A"));
        EXPECT_EQ(outcome(*second, "A")->outcome, ctx::ReconcileOutcome::Absent);
        for(const auto& op: second->operations) EXPECT_FALSE(op.operation_id.starts_with("chronolog.reconcile/"));
        EXPECT_EQ((*session)->status().state, ctx::SessionState::Ready);
        auto refused = (*session)->remember(memory("A"));
        ASSERT_TRUE(refused.ok());
        EXPECT_EQ(refused->current.outcome, ctx::MemoryOutcome::Rejected);
        EXPECT_EQ(peer.stored("A"), 0u);
        auto resent = (*session)->remember(memory("A"), resend());
        ASSERT_TRUE(resent.ok() && resent->current.receipt);
        EXPECT_EQ(resent->current.receipt->event_id.incarnation, 3u);
        EXPECT_EQ(peer.stored("A"), 1u);
    }
    // Permanent SOURCE_FAILED leaves the old id permanently UNKNOWN and refused, and still admits new memories.
    {
        LogPeer peer;
        auto client = ctx::ContextClient::Connect(peer.options());
        ASSERT_TRUE(client.ok());
        auto session = client->open(ref(), {"agent", "slot"}, writable());
        ASSERT_TRUE(session.ok());
        peer.fence(1, chronolog::AppendRejection::FencedExpired);
        ASSERT_TRUE((*session)->remember(memory("A")).ok());
        peer.failed_source = true;
        ctx::ReconcileOptions options;
        options.operation_ids = {"A", "unseen"};
        auto reconciled = (*session)->reconcile(options);
        ASSERT_TRUE(reconciled.ok()) << reconciled.status();
        EXPECT_TRUE(reconciled->attempted);
        EXPECT_FALSE(reconciled->proof_complete);
        ASSERT_TRUE(reconciled->completion);
        EXPECT_EQ(reconciled->completion->reason, chronolog::IncompleteReason::SourceFailed);
        EXPECT_EQ(reconciled->permanently_unknown_operations, (std::vector<std::string>{"A", "unseen"}));
        EXPECT_EQ(outcome(*reconciled, "A")->outcome, ctx::ReconcileOutcome::Unknown);
        EXPECT_EQ((*session)->status().state, ctx::SessionState::Ready);
        auto fresh = (*session)->remember(memory("B"));
        ASSERT_TRUE(fresh.ok() && fresh->current.receipt);
        EXPECT_EQ(fresh->current.receipt->event_id.incarnation, 2u);
        for(const auto& again: {(*session)->remember(memory("A")), (*session)->remember(memory("A"), resend())})
        {
            ASSERT_TRUE(again.ok());
            EXPECT_EQ(again->current.outcome, ctx::MemoryOutcome::Unknown);
            EXPECT_EQ(again->current.status.code(), absl::StatusCode::kFailedPrecondition);
        }
        EXPECT_EQ(peer.stored("A"), 0u);
        EXPECT_EQ((*session)->status().permanently_unknown_operations, (std::vector<std::string>{"A", "unseen"}));
        auto checkpoint = (*session)->checkpoint();
        ASSERT_EQ(checkpoint.permanently_unknown_operations.size(), 2u);
        EXPECT_TRUE(checkpoint.permanently_unknown_operations[0].absence_provable);
        EXPECT_FALSE(checkpoint.permanently_unknown_operations[1].absence_provable);
        EXPECT_EQ(checkpoint.permanently_unknown_operations[0].window->end, *reconciled->marker_hlc);
        EXPECT_TRUE(checkpoint.reconcile_attempted);
    }
}

TEST(ContextApi, ReconcileProofWindowIsStrictlySeparated)
{
    // Before any own receipt the bound is the floor observed when the incarnation was acquired (I7.3); the proof
    // reads exactly [l, m) and the successor's own events never count as the old incarnation's.
    LogPeer peer;
    auto client = ctx::ContextClient::Connect(peer.options());
    ASSERT_TRUE(client.ok());
    auto options = writable();
    options.resume = ctx::Checkpoint{};
    options.resume->context = ref();
    options.resume->identity = {"agent", "slot"};
    options.resume->causal_floor = {5000, 3};
    auto session = client->open(ref(), {"agent", "slot"}, options);
    ASSERT_TRUE(session.ok()) << session.status();
    peer.fence(1, chronolog::AppendRejection::FencedExpired);
    ASSERT_TRUE((*session)->remember(memory("A")).ok());
    peer.inject(2, 5001, "A");
    peer.inject(1, 4999, "A");
    auto reconciled = (*session)->reconcile();
    ASSERT_TRUE(reconciled.ok()) << reconciled.status();
    ASSERT_TRUE(reconciled->range && reconciled->marker_hlc);
    EXPECT_EQ(reconciled->range->start, (chronolog::Hlc{5000, 3}));
    EXPECT_EQ(reconciled->range->end, *reconciled->marker_hlc);
    ASSERT_FALSE(peer.reads.empty());
    EXPECT_EQ(peer.reads.back().hlc().start().physical_ns(), 5000);
    EXPECT_EQ(peer.reads.back().hlc().start().logical(), 3u);
    EXPECT_EQ(peer.reads.back().hlc().end().physical_ns(), reconciled->marker_hlc->physical_ns);
    EXPECT_EQ(outcome(*reconciled, "A")->outcome, ctx::ReconcileOutcome::Absent);
    // After an own receipt the bound rises to it; an unknown prior state reads from 0.
    ASSERT_TRUE((*session)->remember(memory("own")).ok());
    const auto own = (*session)->checkpoint().last_own_receipt_hlc;
    ASSERT_TRUE(own);
    peer.fence(2, chronolog::AppendRejection::FencedExpired);
    ASSERT_TRUE((*session)->remember(memory("B")).ok());
    reconciled = (*session)->reconcile();
    ASSERT_TRUE(reconciled.ok()) << reconciled.status();
    EXPECT_EQ(reconciled->range->start, *own);
    EXPECT_GT(reconciled->range->end, *own);
    EXPECT_EQ(outcome(*reconciled, "B")->outcome, ctx::ReconcileOutcome::Absent);
    auto unknown = writable();
    unknown.resume = (*session)->checkpoint();
    unknown.resume->context = ref(2);
    unknown.resume->writer = ctx::WriterStamp{LogPeer::writer, peer.incarnation};
    unknown.resume->acquisition_closed = false;
    unknown.resume->prior_state_unknown_below = chronolog::Hlc{9000, 0};
    unknown.resume->dispositions.clear();
    unknown.resume->unresolved_operations = {"C"};
    auto other = client->open(ref(2), {"agent", "slot"}, unknown);
    ASSERT_TRUE(other.ok()) << other.status();
    EXPECT_EQ((*other)->status().state, ctx::SessionState::Fenced);
    reconciled = (*other)->reconcile(takeover());
    ASSERT_TRUE(reconciled.ok()) << reconciled.status();
    EXPECT_EQ(reconciled->range->start, chronolog::Hlc{});
    EXPECT_EQ(outcome(*reconciled, "C")->outcome, ctx::ReconcileOutcome::Absent);
}

TEST(ContextApi, RestoredCheckpointNeverReusesAnIdOrWritesARecordedIncarnation)
{
    LogPeer peer;
    peer.incarnation = 4;
    peer.inject(2, 1060, "A");
    peer.inject(5, 1070, "foreign");
    ctx::Checkpoint saved;
    saved.context = ref();
    saved.identity = {"agent", "slot"};
    saved.causal_floor = {1100, 0};
    saved.writer = ctx::WriterStamp{LogPeer::writer, 4};
    saved.acquisition = ctx::AcquisitionProvenance{"host", "lock", std::nullopt, chronolog::Hlc{1080, 0}};
    saved.recovery =
            ctx::ReconcileCheckpoint{"t1", {1050, 0}, {{{LogPeer::writer, 2}, {}}, {{LogPeer::writer, 3}, {}}}, {}, {}};
    saved.unresolved_operations = {"A"};
    auto encoded = ctx::encodeCheckpoint(saved);
    ASSERT_TRUE(encoded.ok()) << encoded.status();
    std::vector<std::string> ids;
    for(int restart = 0; restart < 2; ++restart)
    {
        auto client = ctx::ContextClient::Connect(peer.options());
        ASSERT_TRUE(client.ok());
        auto options = writable();
        options.resume = ctx::decodeCheckpoint(*encoded).value();
        options.ownership = options.resume->acquisition;
        auto session = client->open(ref(), {"agent", "slot"}, options);
        ASSERT_TRUE(session.ok()) << session.status();
        EXPECT_EQ((*session)->status().state, ctx::SessionState::NeedsReconcile);
        EXPECT_EQ((*session)->remember(memory("new")).value().current.outcome, ctx::MemoryOutcome::Fenced);
        const size_t before = peer.acquisitions.size();
        auto reconciled = (*session)->reconcile();
        ASSERT_TRUE(reconciled.ok()) << reconciled.status();
        ASSERT_EQ(peer.acquisitions.size(), before + 1);
        const auto& acquire = peer.acquisitions.back();
        EXPECT_TRUE(acquire.takeover());
        EXPECT_EQ(acquire.expected_prior_incarnation(), 4u);
        ids.push_back(acquire.acquire_request_id());
        EXPECT_FALSE(acquire.acquire_request_id().empty());
        EXPECT_EQ(encoded->find(acquire.acquire_request_id()), std::string::npos);
        if(restart == 0)
        {
            EXPECT_EQ(reconciled->writer->incarnation, 5u);
            EXPECT_EQ(reconciled->range->start, (chronolog::Hlc{1050, 0}));
            EXPECT_EQ(outcome(*reconciled, "A")->outcome, ctx::ReconcileOutcome::Landed);
            EXPECT_FALSE(outcome(*reconciled, "foreign"));
        }
        else
        {
            // The newer incarnation 5 is unknown to this checkpoint: no mutation, explicit takeover joins it.
            ASSERT_TRUE(chronolog::client::acquireRefusalOf(reconciled->status));
            EXPECT_EQ((*session)->status().state, ctx::SessionState::Fenced);
            reconciled = (*session)->reconcile(takeover());
            ASSERT_TRUE(reconciled.ok()) << reconciled.status();
            EXPECT_EQ(peer.acquisitions.back().expected_prior_incarnation(), 5u);
            ids.push_back(peer.acquisitions.back().acquire_request_id());
            EXPECT_EQ(reconciled->writer->incarnation, 6u);
            EXPECT_EQ(outcome(*reconciled, "A")->outcome, ctx::ReconcileOutcome::Landed);
            EXPECT_EQ(outcome(*reconciled, "foreign")->outcome, ctx::ReconcileOutcome::Landed);
        }
    }
    EXPECT_EQ(std::set<std::string>(ids.begin(), ids.end()).size(), ids.size());
    for(const auto inc: peer.incarnations()) EXPECT_GE(inc, 5u);
    EXPECT_EQ(peer.stored("A"), 1u);
}

TEST(ContextApi, LaterCompleteReadReleasesPermanentUnknown)
{
    LogPeer peer;
    auto client = ctx::ContextClient::Connect(peer.options());
    ASSERT_TRUE(client.ok());
    auto options = writable();
    options.ownership = ctx::AcquisitionProvenance{"host", "lock", {}, {}};
    auto session = client->open(ref(), {"agent", "slot"}, options);
    ASSERT_TRUE(session.ok());
    peer.fence(1, chronolog::AppendRejection::FencedExpired);
    ASSERT_TRUE((*session)->remember(memory("A")).ok());
    peer.failed_source = true;
    auto reconciled = (*session)->reconcile();
    ASSERT_TRUE(reconciled.ok());
    EXPECT_EQ(reconciled->permanently_unknown_operations, (std::vector<std::string>{"A"}));
    ASSERT_TRUE((*session)->remember(memory("B")).ok());
    // The old incarnation is fenced, so a complete Read of the recorded window releases the id (C5).
    peer.failed_source = false;
    const size_t acquisitions = peer.acquisitions.size();
    auto released = (*session)->reconcile();
    ASSERT_TRUE(released.ok()) << released.status();
    EXPECT_EQ(peer.acquisitions.size(), acquisitions);
    EXPECT_FALSE(released->attempted);
    EXPECT_TRUE(released->permanently_unknown_operations.empty());
    ASSERT_TRUE(outcome(*released, "A"));
    EXPECT_EQ(outcome(*released, "A")->outcome, ctx::ReconcileOutcome::Absent);
    ASSERT_TRUE((*session)->remember(memory("A"), resend()).value().current.receipt);
    // A permanent id restored from a checkpoint is released the same way, here as LANDED.
    peer.lose_next = true;
    ASSERT_TRUE((*session)->remember(memory("L")).ok());
    peer.fence(2, chronolog::AppendRejection::FencedExpired);
    ASSERT_TRUE((*session)->remember(memory("next")).ok());
    peer.failed_source = true;
    ASSERT_TRUE((*session)->reconcile().ok());
    auto saved = ctx::encodeCheckpoint((*session)->checkpoint());
    ASSERT_TRUE(saved.ok());
    peer.failed_source = false;
    auto restart = ctx::ContextClient::Connect(peer.options());
    ASSERT_TRUE(restart.ok());
    options.resume = ctx::decodeCheckpoint(*saved).value();
    auto restored = restart->open(ref(), {"agent", "slot"}, options);
    ASSERT_TRUE(restored.ok()) << restored.status();
    EXPECT_EQ((*restored)->status().permanently_unknown_operations, (std::vector<std::string>{"L"}));
    EXPECT_EQ((*restored)->remember(memory("L")).value().current.outcome, ctx::MemoryOutcome::Unknown);
    released = (*restored)->reconcile();
    ASSERT_TRUE(released.ok()) << released.status();
    ASSERT_TRUE(outcome(*released, "L"));
    EXPECT_EQ(outcome(*released, "L")->outcome, ctx::ReconcileOutcome::Landed);
    EXPECT_TRUE((*restored)->status().permanently_unknown_operations.empty());
    EXPECT_EQ(peer.stored("L"), 1u);
}

TEST(ContextApi, CheckpointEncodingRoundTripsAndIsBounded)
{
    ctx::Checkpoint value;
    value.identity = {"agent\"\\\n\xc3\xa9", "slot"};
    value.context = {UINT64_MAX, "team", "notes"};
    value.causal_floor = {INT64_MAX, UINT32_MAX};
    value.processed_after = ctx::Position{{10, 1}, {UINT64_MAX, 7, 2, 3}};
    value.writer = ctx::WriterStamp{7, 2};
    value.acquisition = ctx::AcquisitionProvenance{"host", "lock", 1, chronolog::Hlc{5, 0}};
    value.last_own_receipt_hlc = chronolog::Hlc{9, 0};
    value.recovery =
            ctx::ReconcileCheckpoint{"t", {5, 0}, {{{7, 1}, "chronolog.reconcile/t/1"}}, "m", chronolog::Hlc{11, 0}};
    const std::string long_id(128, 'x');
    value.unresolved_operations = {long_id};
    value.permanently_unknown_operations = {
            {"\"quoted\"", {{7, 1}}, chronolog::client::HlcRange{{5, 0}, {11, 0}}, true, std::string("\x00\xff", 2)}};
    value.dispositions = {{{"landed",
                            ctx::ReconcileOutcome::Landed,
                            ctx::Position{{6, 0}, {1, 7, 1, 4}},
                            chronolog::Durability::Durable},
                           {7, 1},
                           std::nullopt},
                          {{"absent", ctx::ReconcileOutcome::Absent, std::nullopt, chronolog::Durability::Unspecified},
                           {7, 1},
                           std::string(32, 'd')}};
    value.prior_state_unknown_below = chronolog::Hlc{3, 0};
    value.reconcile_attempted = true;
    value.takeover_required = true;
    auto encoded = ctx::encodeCheckpoint(value);
    ASSERT_TRUE(encoded.ok()) << encoded.status();
    auto decoded = ctx::decodeCheckpoint(*encoded);
    ASSERT_TRUE(decoded.ok()) << decoded.status();
    EXPECT_EQ(decoded->identity.agent_id, value.identity.agent_id);
    EXPECT_EQ(decoded->context.story_id, UINT64_MAX);
    EXPECT_EQ(decoded->causal_floor, value.causal_floor);
    EXPECT_EQ(decoded->processed_after->id, value.processed_after->id);
    EXPECT_EQ(decoded->writer, value.writer);
    EXPECT_EQ(decoded->acquisition->expected_prior_incarnation, 1u);
    EXPECT_EQ(decoded->acquisition->acquisition_record_receipt_hlc, value.acquisition->acquisition_record_receipt_hlc);
    EXPECT_EQ(decoded->recovery->recovered_incarnations[0].marker_operation_id, "chronolog.reconcile/t/1");
    EXPECT_EQ(decoded->recovery->marker_hlc, value.recovery->marker_hlc);
    EXPECT_EQ(decoded->unresolved_operations, value.unresolved_operations);
    ASSERT_EQ(decoded->permanently_unknown_operations.size(), 1u);
    EXPECT_EQ(decoded->permanently_unknown_operations[0].normalized_digest, std::string("\x00\xff", 2));
    EXPECT_EQ(decoded->permanently_unknown_operations[0].window->end, (chronolog::Hlc{11, 0}));
    ASSERT_EQ(decoded->dispositions.size(), 2u);
    EXPECT_EQ(decoded->dispositions[0].result.landed->id.sequence, 4u);
    EXPECT_EQ(decoded->dispositions[1].result.outcome, ctx::ReconcileOutcome::Absent);
    EXPECT_EQ(decoded->dispositions[1].normalized_digest, std::string(32, 'd'));
    EXPECT_TRUE(decoded->reconcile_attempted && decoded->takeover_required && !decoded->acquisition_closed);
    EXPECT_EQ(ctx::encodeCheckpoint(*decoded).value(), *encoded);
    EXPECT_EQ(ctx::encodeCheckpoint(value, encoded->size() - 1).status().code(), absl::StatusCode::kResourceExhausted);
    value.dispositions[0].result.outcome = ctx::ReconcileOutcome::Unknown;
    EXPECT_EQ(ctx::encodeCheckpoint(value).status().code(), absl::StatusCode::kInvalidArgument);
    for(const auto& bad: std::vector<std::string>{"{}", "[]", "not json", encoded->substr(0, encoded->size() / 2)})
        EXPECT_EQ(ctx::decodeCheckpoint(bad).status().code(), absl::StatusCode::kInvalidArgument);
    auto negative = *encoded;
    negative.replace(negative.find("\"causal_floor\":["), 16, "\"causal_floor\":[-");
    EXPECT_EQ(ctx::decodeCheckpoint(negative).status().code(), absl::StatusCode::kInvalidArgument);
    // A bounded disposition list is what a session persists: restoring more than the bound is refused.
    Peer peer;
    auto config = peer.options();
    config.max_persisted_dispositions = 1;
    auto client = ctx::ContextClient::Connect(config);
    ASSERT_TRUE(client.ok());
    auto options = writable();
    options.resume = *decoded;
    options.resume->context = ref();
    options.resume->processed_after.reset();
    EXPECT_EQ(client->open(ref(), decoded->identity, options).status().code(), absl::StatusCode::kResourceExhausted);
}
