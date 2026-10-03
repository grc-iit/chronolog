#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <mutex>
#include <condition_variable>
#include "chronolog/context/context.h"
#include "chronolog/v1/chronolog.grpc.pb.h"

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

class Peer final
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
    EXPECT_EQ(client->open(ref(), {"agent\"\\\n", "slot"}, writable()).status().code(),
              absl::StatusCode::kUnimplemented);
    EXPECT_EQ(peer.acquisitions.size(), 2u);
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
            EXPECT_EQ((*session)->reconcile().status().code(), absl::StatusCode::kUnimplemented);
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
    EXPECT_EQ((*session)->reconcile().status().code(), absl::StatusCode::kUnimplemented);
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
    EXPECT_EQ(client->open(ref(3), {"agent", "slot"}, options).status().code(), absl::StatusCode::kUnimplemented);
    EXPECT_TRUE(peer.acquisitions.empty());
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

} // namespace
