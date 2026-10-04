// B45 Part 1 gates for the Player: the clock audit on Register and Heartbeat is observational.
#include <absl/log/log_sink.h>
#include <absl/log/log_sink_registry.h>
#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>
#include <google/protobuf/util/message_differencer.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include "clock/FakeClock.h"
#include "rpc/Channel.h"
#include "chrono-player/adapter/ClusterClient.h"
#include "chrono-player/adapter/ReplayService.h"
#include "rpc/VisorClockAudit.h"
#include "chrono-player/replay/HotReplay.h"
#include "chrono-player/replay/KeeperHotSource.h"

namespace chronolog::player
{
namespace
{
using namespace std::chrono_literals;
namespace wire = internal::v1;

constexpr StoryId kStory = 1;
constexpr int64_t kNow = 1'700'000'000'000'000'000;
constexpr uint64_t kBound = 1'000'000;

class CapturingSink final: public absl::LogSink
{
public:
    CapturingSink() { absl::AddLogSink(this); }
    ~CapturingSink() override { absl::RemoveLogSink(this); }
    void Send(const absl::LogEntry& entry) override
    {
        std::lock_guard lock(mu_);
        messages_.emplace_back(entry.text_message());
    }
    size_t count(const std::string& needle)
    {
        std::lock_guard lock(mu_);
        return std::count_if(messages_.begin(),
                             messages_.end(),
                             [&](const std::string& text) { return text.find(needle) != std::string::npos; });
    }

private:
    std::mutex mu_;
    std::vector<std::string> messages_;
};

std::shared_ptr<FakeClock> syncedClock()
{
    auto clock = std::make_shared<FakeClock>(kNow, kBound);
    clock->setStatus(ClockStatus::Synced);
    return clock;
}

template <class Response>
void stamp(Response& response, int64_t physical_ns, const std::string& replica, v1::ClockStatus status)
{
    auto* physical = response.mutable_physical();
    physical->set_physical_ns(physical_ns);
    physical->set_status(status);
    if(status == v1::CLOCK_STATUS_SYNCED)
        physical->set_uncertainty_ns(kBound);
    if(!replica.empty())
    {
        response.mutable_clock_responder()->set_replica_id(replica);
        response.mutable_clock_responder()->set_instance(replica + "-1");
    }
}

v1::Event event(uint64_t writer, uint64_t sequence, int64_t hlc)
{
    v1::Event e;
    e.mutable_id()->set_story_id(kStory);
    e.mutable_id()->set_writer_id(writer);
    e.mutable_id()->set_incarnation(1);
    e.mutable_id()->set_sequence(sequence);
    e.mutable_hlc()->set_physical_ns(hlc);
    e.mutable_physical()->set_physical_ns(hlc);
    e.set_durability(v1::DURABILITY_ACCEPTED);
    return e;
}

// A Keeper holding fixed events sealed at 200 under epoch 7.
class FakeKeeper final: public wire::Archive::Service
{
public:
    explicit FakeKeeper(std::vector<v1::Event> events)
        : events_(std::move(events))
    {}
    grpc::Status FetchHot(grpc::ServerContext*,
                          const wire::FetchHotRequest* request,
                          grpc::ServerWriter<wire::FetchHotResponse>* writer) override
    {
        wire::FetchHotResponse batch;
        for(const auto& e: events_)
        {
            const Hlc h{e.hlc().physical_ns(), e.hlc().logical()};
            const Hlc start{request->hlc().start().physical_ns(), request->hlc().start().logical()};
            const Hlc end{request->hlc().end().physical_ns(), request->hlc().end().logical()};
            if(h >= start && h < end)
                *batch.mutable_batch()->add_events() = e;
        }
        if(batch.batch().events_size())
            writer->Write(batch);
        wire::FetchHotResponse trailer;
        trailer.mutable_trailer()->set_epoch(7);
        trailer.mutable_trailer()->set_instance("keeper-instance");
        trailer.mutable_trailer()->mutable_sealed_frontier()->set_physical_ns(200);
        writer->Write(trailer);
        return grpc::Status::OK;
    }

private:
    const std::vector<v1::Event> events_;
};

// Routes kStory to keeper-a and keeper-b and stamps every reply with kNow + offset from replica "visor-a".
class FakeVisor final: public wire::Cluster::Service
{
public:
    grpc::Status Register(grpc::ServerContext*, const wire::RegisterRequest*, wire::RegisterResponse* r) override
    {
        auto* update = r->add_routes();
        update->set_story_id(kStory);
        update->set_revision(1);
        update->mutable_route()->set_epoch(7);
        for(const std::string keeper: {"keeper-a", "keeper-b"})
        {
            auto* k = update->mutable_route()->add_keepers();
            k->set_process_id(keeper);
            k->set_endpoint(keeper);
        }
        stamp(*r, kNow + offset, "visor-a", v1::CLOCK_STATUS_SYNCED);
        return grpc::Status::OK;
    }
    grpc::Status Heartbeat(grpc::ServerContext*, const wire::HeartbeatRequest*, wire::HeartbeatResponse* r) override
    {
        stamp(*r, kNow + offset, "visor-a", v1::CLOCK_STATUS_SYNCED);
        return grpc::Status::OK;
    }
    std::atomic<int64_t> offset{0};
};

struct FakeCatalog final: StoryCatalog
{
    absl::Status ensureLive(StoryId) const override { return absl::OkStatus(); }
};

struct FakeWriters final: WriterSource
{
    std::vector<WriterAssignment> writers(StoryId) const override { return {{2, 1, "keeper-a"}, {4, 1, "keeper-b"}}; }
};

std::unique_ptr<grpc::Server> serve(grpc::Service& service, std::string& address)
{
    grpc::ServerBuilder builder;
    rpc::applyServerPolicy(builder);
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    auto server = builder.BuildAndStart();
    address = "127.0.0.1:" + std::to_string(port);
    return server;
}

std::optional<ClockAuditState> stateOf(const VisorClockAudit& audit, const std::string& replica)
{
    for(const auto& entry: audit.entries())
        if(entry.key.replica_id == replica)
            return entry.state;
    return std::nullopt;
}

struct Observed
{
    std::vector<v1::Event> events;
    std::vector<v1::Completion> completions;
    std::vector<v1::Event> tailed;
};

bool same(const google::protobuf::Message& a, const google::protobuf::Message& b)
{
    return google::protobuf::util::MessageDifferencer::Equals(a, b);
}

TEST(PlayerClockAudit, AlarmLeavesReadAndTailUnchanged)
{
    CapturingSink sink;
    FakeKeeper a({event(2, 1, 110), event(2, 2, 130), event(2, 3, 150)});
    FakeKeeper b({event(4, 1, 120), event(4, 2, 140), event(4, 3, 160)});
    FakeVisor visor;
    std::string a_addr, b_addr, visor_addr;
    auto a_server = serve(a, a_addr);
    auto b_server = serve(b, b_addr);
    auto visor_server = serve(visor, visor_addr);
    ASSERT_TRUE(a_server && b_server && visor_server);

    auto audit = std::make_shared<VisorClockAudit>("player",
                                                   "player-1/i1",
                                                   syncedClock(),
                                                   [] { return int64_t{5'000'000'000}; });
    auto cluster = std::make_shared<ClusterClient>(rpc::peerChannel(visor_addr),
                                                   Process{"player-1", "i1", "player:1", ProcessRole::Player},
                                                   2000ms,
                                                   ClusterClient::TombstoneLookup{},
                                                   audit);
    ASSERT_TRUE(cluster->registerSelf().ok());
    ASSERT_EQ(stateOf(*audit, "visor-a"), ClockAuditState::Ok);

    KeeperHotSourceOptions options;
    options.deadline = 2000ms;
    auto source = std::make_shared<KeeperHotSource>(
            cluster,
            std::make_shared<FakeWriters>(),
            [&](const KeeperRef& k) { return k.process_id == "keeper-a" ? a_addr : b_addr; },
            options);
    HotReplayOptions replay_options;
    replay_options.batch_size = 2;
    replay_options.tail_poll = 20ms;
    auto catalog = std::make_shared<FakeCatalog>();
    ReplayService service(std::make_shared<HotReplay>(source, replay_options), catalog);
    grpc::ServerBuilder builder;
    rpc::applyServerPolicy(builder);
    builder.RegisterService(&service);
    auto server = builder.BuildAndStart();
    ASSERT_NE(server, nullptr);
    auto stub = v1::Replay::NewStub(server->InProcessChannel(grpc::ChannelArguments()));

    auto observe = [&]
    {
        Observed seen;
        {
            grpc::ClientContext context;
            context.set_deadline(std::chrono::system_clock::now() + 10s);
            v1::ReadRequest request;
            request.set_story_id(kStory);
            request.mutable_hlc()->mutable_start()->set_physical_ns(100);
            request.mutable_hlc()->mutable_end()->set_physical_ns(200);
            auto reader = stub->Read(&context, request);
            v1::ReadResponse response;
            for(int i = 0; i < 100 && reader->Read(&response); ++i)
            {
                for(const auto& e: response.batch().events()) seen.events.push_back(e);
                if(response.has_completion())
                    seen.completions.push_back(response.completion());
            }
            EXPECT_TRUE(reader->Finish().ok());
        }
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + 10s);
        v1::TailRequest request;
        request.set_story_id(kStory);
        const auto from = event(2, 2, 130);
        *request.mutable_from()->mutable_hlc() = from.hlc();
        *request.mutable_from()->mutable_id() = from.id();
        auto reader = stub->Tail(&context, request);
        v1::TailResponse response;
        for(int i = 0; i < 100 && seen.tailed.size() < 3 && reader->Read(&response); ++i)
            for(const auto& e: response.batch().events()) seen.tailed.push_back(e);
        context.TryCancel();
        while(reader->Read(&response)) {}
        reader->Finish();
        return seen;
    };

    const auto before = observe();
    // Offset 10 ms against a threshold of local 1 ms + Visor 1 ms + RTT/2 0; the next Heartbeat alarms.
    visor.offset = 10'000'000;
    for(int i = 0; i < 1000 && sink.count("clock audit ALARM") == 0; ++i) std::this_thread::sleep_for(10ms);
    ASSERT_EQ(stateOf(*audit, "visor-a"), ClockAuditState::Alarm);
    const auto after = observe();

    EXPECT_EQ(sink.count("clock audit ALARM role=player identity=player-1/i1 replica=visor-a instance=visor-a-1 "
                         "offset_ns=10000000 local_bound_ns=1000000 visor_bound_ns=1000000 rtt_ns=0 "
                         "threshold_ns=2000000"),
              1u);
    ASSERT_EQ(before.events.size(), 6u);
    ASSERT_FALSE(before.completions.empty());
    ASSERT_EQ(before.tailed.size(), 3u);
    ASSERT_EQ(after.events.size(), before.events.size());
    for(size_t i = 0; i < before.events.size(); ++i) EXPECT_TRUE(same(before.events[i], after.events[i])) << i;
    ASSERT_EQ(after.completions.size(), before.completions.size());
    for(size_t i = 0; i < before.completions.size(); ++i)
        EXPECT_TRUE(same(before.completions[i], after.completions[i])) << i;
    ASSERT_EQ(after.tailed.size(), before.tailed.size());
    for(size_t i = 0; i < before.tailed.size(); ++i) EXPECT_TRUE(same(before.tailed[i], after.tailed[i])) << i;

    service.shutdown();
    server->Shutdown(std::chrono::system_clock::now() + 3s);
    cluster.reset();
    visor_server->Shutdown(std::chrono::system_clock::now() + 1s);
    a_server->Shutdown(std::chrono::system_clock::now() + 1s);
    b_server->Shutdown(std::chrono::system_clock::now() + 1s);
}

} // namespace
} // namespace chronolog::player
