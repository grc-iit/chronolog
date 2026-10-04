#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>
#include <cstring>
#include <map>
#include <mutex>
#include "chronolog_ldms_bridge.h"
#include "chronolog/v1/chronolog.grpc.pb.h"

namespace
{
namespace wire = chronolog::v1;
using namespace std::chrono_literals;
constexpr int64_t lease_ns = 300'000'000'000;

// Finite grants; the first Release is refused unapplied and a plain Acquire of a live incarnation answers HELD, as the
// Catalog does once leases are enforced (RFC-G section 7). The Keeper can fail whole append calls.
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
        server_->Shutdown(std::chrono::system_clock::now() + 2s);
        server_->Wait();
    }
    const std::string& endpoint() const { return endpoint_; }
    struct Slot
    {
        uint64_t writer_id{};
        uint64_t incarnation{};
        bool live{};
    };
    std::mutex mutex;
    std::map<std::string, uint64_t> stories;
    std::map<std::pair<uint64_t, std::string>, Slot> slots;
    std::vector<wire::AcquireRequest> acquires;
    size_t releases{};
    std::vector<uint64_t> appended;
    bool fail_appends{};
    bool failing()
    {
        std::lock_guard lock(mutex);
        return fail_appends;
    }
    // Another process takes the slot over; its incarnation stays live.
    void foreignTakeover()
    {
        std::lock_guard lock(mutex);
        for(auto& [key, slot]: slots)
        {
            ++slot.incarnation;
            slot.live = true;
        }
    }
    grpc::Status CreateChronicle(grpc::ServerContext*,
                                 const wire::CreateChronicleRequest* r,
                                 wire::CreateChronicleResponse* p) override
    {
        p->mutable_chronicle()->set_name(r->name());
        return grpc::Status::OK;
    }
    grpc::Status
    CreateStory(grpc::ServerContext*, const wire::CreateStoryRequest* r, wire::CreateStoryResponse* p) override
    {
        std::lock_guard lock(mutex);
        auto [it, created] = stories.emplace(r->name(), stories.size() + 1);
        if(!created)
            p->mutable_status()->set_code(static_cast<int>(absl::StatusCode::kAlreadyExists));
        story(r->chronicle(), it->first, it->second, *p->mutable_story());
        return grpc::Status::OK;
    }
    grpc::Status
    ListStories(grpc::ServerContext*, const wire::ListStoriesRequest* r, wire::ListStoriesResponse* p) override
    {
        std::lock_guard lock(mutex);
        for(const auto& [name, id]: stories) story(r->chronicle(), name, id, *p->add_stories());
        return grpc::Status::OK;
    }
    grpc::Status Acquire(grpc::ServerContext*, const wire::AcquireRequest* r, wire::AcquireResponse* p) override
    {
        std::lock_guard lock(mutex);
        acquires.push_back(*r);
        auto& slot = slots[{r->story_id(), r->writer_identity()}];
        if(!slot.writer_id)
            slot.writer_id = slots.size();
        auto refuse = [&](wire::AcquireRefusalReason reason)
        {
            p->mutable_status()->set_code(static_cast<int>(absl::StatusCode::kFailedPrecondition));
            p->set_refusal_reason(reason);
            return grpc::Status::OK;
        };
        if(r->has_expected_prior_incarnation() && r->expected_prior_incarnation() != slot.incarnation)
        {
            p->set_current_incarnation(slot.incarnation);
            return refuse(wire::ACQUIRE_REFUSAL_REASON_PRIOR_MISMATCH);
        }
        if(slot.live && !r->takeover())
        {
            p->set_remaining_ns(lease_ns / 2);
            return refuse(wire::ACQUIRE_REFUSAL_REASON_HELD);
        }
        ++slot.incarnation;
        slot.live = true;
        p->set_story_id(r->story_id());
        p->set_writer_id(slot.writer_id);
        p->set_incarnation(slot.incarnation);
        route(*p->mutable_route());
        *p->mutable_assigned_keeper() = p->route().keepers(0);
        p->mutable_lease()->set_duration_ns(lease_ns);
        p->mutable_lease()->set_remaining_ns(lease_ns);
        return grpc::Status::OK;
    }
    grpc::Status Release(grpc::ServerContext*, const wire::ReleaseRequest* r, wire::ReleaseResponse* p) override
    {
        std::lock_guard lock(mutex);
        if(++releases == 1)
            return {grpc::StatusCode::UNAVAILABLE, "release refused"};
        for(auto& [key, slot]: slots)
            if(slot.writer_id == r->writer_id() && slot.incarnation == r->incarnation())
                slot.live = false;
        p->set_fenced(true);
        return grpc::Status::OK;
    }
    grpc::Status Append(grpc::ServerContext*, const wire::AppendRequest* r, wire::AppendResponse* p) override
    {
        if(failing())
            return {grpc::StatusCode::UNAVAILABLE, "keeper unreachable"};
        answer(*r, *p);
        return grpc::Status::OK;
    }
    grpc::Status
    AppendStream(grpc::ServerContext*,
                 grpc::ServerReaderWriter<wire::AppendStreamResponse, wire::AppendStreamRequest>* stream) override
    {
        if(failing())
            return {grpc::StatusCode::UNAVAILABLE, "keeper unreachable"};
        wire::AppendStreamRequest request;
        while(stream->Read(&request))
        {
            wire::AppendStreamResponse response;
            answer(request, response);
            if(!stream->Write(response))
                break;
        }
        return grpc::Status::OK;
    }

private:
    void route(wire::Route& r)
    {
        r.set_epoch(1);
        r.set_player(endpoint_);
        auto* keeper = r.add_keepers();
        keeper->set_process_id("keeper");
        keeper->set_endpoint(endpoint_);
    }
    void story(const std::string& chronicle, const std::string& name, uint64_t id, wire::Story& s)
    {
        s.set_story_id(id);
        s.set_chronicle(chronicle);
        s.set_name(name);
        s.set_epoch(1);
        route(*s.mutable_route());
    }
    template <class Request, class Response>
    void answer(const Request& r, Response& p)
    {
        std::lock_guard lock(mutex);
        p.set_batch_id(r.batch_id());
        for(const auto& item: r.items())
        {
            auto* result = p.add_results();
            bool live = false;
            for(const auto& [key, slot]: slots)
                live |= slot.writer_id == item.writer_id() && slot.incarnation == item.incarnation() && slot.live;
            if(!live)
            {
                result->mutable_status()->set_code(static_cast<int>(absl::StatusCode::kFailedPrecondition));
                result->set_rejection(wire::APPEND_REJECTION_FENCED_SUPERSEDED);
                continue;
            }
            appended.push_back(item.incarnation());
            result->mutable_id()->set_story_id(r.story_id());
            result->mutable_id()->set_writer_id(item.writer_id());
            result->mutable_id()->set_incarnation(item.incarnation());
            result->mutable_id()->set_sequence(item.sequence());
            result->mutable_assigned_hlc()->set_physical_ns(static_cast<int64_t>(appended.size()));
            result->set_achieved_durability(r.durability());
        }
    }
    std::unique_ptr<grpc::Server> server_;
    std::string endpoint_;
};

std::string sample(int n)
{
    return R"({"producer":"p","instance":"p/set","schema":"s","component_id":1,"timestamp_ns":1,"metrics":{"n":)" +
           std::to_string(n) + "}}";
}
chrono_ldms_stats stats(chrono_ldms_t* bridge)
{
    chrono_ldms_stats out;
    chrono_ldms_stats_get(bridge, &out);
    return out;
}

// RFC-G r4 finding 5: a whole-call append failure releases the abandoned Writer; when that Release is refused the
// re-acquire of the same identity retries it instead of meeting HELD, and a live foreign holder is a typed conflict.
TEST(LdmsLease, FailedAppendAndReleaseThenNextBatchCompletes)
{
    LeasePeer peer;
    chrono_ldms_config cfg{};
    cfg.catalog_endpoint = peer.endpoint().c_str();
    cfg.batch_size = 1;
    cfg.flush_interval_ms = 5;
    cfg.rpc_timeout_ms = 2000;
    chrono_ldms_t* bridge = nullptr;
    char error[256];
    ASSERT_EQ(chrono_ldms_open(&cfg, &bridge, error, sizeof(error)), CHRONO_LDMS_OK) << error;
    chrono_ldms_stream_t* stream = nullptr;
    ASSERT_EQ(chrono_ldms_stream(bridge, "ldms-lease", "s", "p", &stream), CHRONO_LDMS_OK);
    auto store = [&](int n)
    {
        const auto json = sample(n);
        ASSERT_EQ(chrono_ldms_store(stream, 1, json.data(), json.size()), CHRONO_LDMS_OK);
        ASSERT_EQ(chrono_ldms_flush(bridge, 10000), CHRONO_LDMS_OK);
    };
    {
        std::lock_guard lock(peer.mutex);
        peer.fail_appends = true;
    }
    store(1);
    EXPECT_EQ(stats(bridge).failed, 1u);
    EXPECT_EQ(stats(bridge).appended, 0u);
    {
        std::lock_guard lock(peer.mutex);
        peer.fail_appends = false;
        EXPECT_EQ(peer.releases, 1u);
    }
    store(2);
    EXPECT_EQ(stats(bridge).appended, 1u);
    EXPECT_EQ(stats(bridge).failed, 1u);
    {
        std::lock_guard lock(peer.mutex);
        EXPECT_EQ(peer.appended, (std::vector<uint64_t>{2}));
        // The refused Release was retried by the re-acquire, which then acquired plainly.
        EXPECT_EQ(peer.releases, 2u);
        EXPECT_EQ(peer.acquires.size(), 2u);
        for(const auto& r: peer.acquires) EXPECT_FALSE(r.takeover() || r.has_expected_prior_incarnation());
    }

    // The superseded Writer is dropped after its fenced batch; the live foreign holder then refuses the re-acquire with
    // HELD instead of every later batch failing on the fence.
    peer.foreignTakeover();
    store(3);
    store(4);
    const auto after = stats(bridge);
    EXPECT_EQ(after.appended, 1u);
    EXPECT_EQ(after.failed, 3u);
    EXPECT_NE(std::strstr(after.last_error, "acquire"), nullptr) << after.last_error;
    EXPECT_NE(std::strstr(after.last_error, "FAILED_PRECONDITION"), nullptr) << after.last_error;
    {
        std::lock_guard lock(peer.mutex);
        EXPECT_EQ(peer.appended, (std::vector<uint64_t>{2}));
        for(const auto& r: peer.acquires) EXPECT_FALSE(r.takeover() && !r.has_expected_prior_incarnation());
    }
    EXPECT_EQ(chrono_ldms_close(bridge, 10000), CHRONO_LDMS_OK);
}
} // namespace
