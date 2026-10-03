#include <chrono>
#include <cstdlib>
#include <iostream>
#include <map>
#include <thread>
#include <grpcpp/grpcpp.h>
#include "chronolog/client/client.h"
#include "chronolog/v1/chronolog.grpc.pb.h"

namespace sdk = chronolog::client;
namespace wire = chronolog::v1;
using namespace chronolog;
using namespace std::chrono_literals;

namespace
{
constexpr auto lease_duration = 3s;
constexpr auto safety_margin = 500ms;
constexpr int64_t lease_ns = 3'000'000'000;

[[noreturn]] void fail(const std::string& message)
{
    std::cerr << "FAIL " << message << '\n';
    std::exit(1);
}

struct Row
{
    Hlc hlc;
    std::string payload;
};

struct Grant
{
    sdk::Writer writer;
    wire::AcquireRequest retry;
};

Grant acquire(sdk::Client& client, StoryId story)
{
    auto id = client.newAcquireRequestId();
    if(!id.ok())
        fail("request id: " + id.status().ToString());
    AcquireOptions options;
    options.acquire_request_id = *id;
    options.lease_duration_ns = lease_ns;
    auto writer = client.acquire(story, "lease-writer", options);
    if(!writer.ok())
        fail("acquire: " + writer.status().ToString());
    if(writer->acquisition().lease.duration_ns != lease_ns)
        fail("short lease was not granted");
    wire::AcquireRequest retry;
    retry.set_story_id(story);
    retry.set_writer_identity("lease-writer");
    retry.set_lease_duration_ns(lease_ns);
    retry.set_acquire_request_id(*id);
    if(writer->acquisition().incarnation > 1)
        retry.set_expected_prior_incarnation(writer->acquisition().incarnation - 1);
    return {std::move(*writer), std::move(retry)};
}

void append(sdk::Writer& writer, const std::string& payload, std::map<EventId, Row>& expected)
{
    sdk::AppendSpec spec;
    spec.envelope.payload = payload;
    auto result = writer.append(spec);
    if(!result.ok())
        fail("append " + payload + ": " + result.status().ToString());
    if(!result->acked() || !expected.emplace(result->event_id, Row{result->hlc, payload}).second)
        fail("append was not uniquely acknowledged DURABLE");
}

void expire(wire::Catalog::Stub& catalog, Grant& grant)
{
    // Last admission can reach Visor on the next 100 ms heartbeat. The configured lease safety
    // margin covers that delivery and the 100 ms service tick. Same-id Acquire samples, never renews.
    const auto end = std::chrono::steady_clock::now() + lease_duration + safety_margin;
    const auto acquisition = grant.writer.acquisition();
    bool expired = false;
    while(std::chrono::steady_clock::now() < end)
    {
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + 1s);
        wire::AcquireResponse response;
        auto status = catalog.Acquire(&context, grant.retry, &response);
        if(!status.ok())
            fail("idle expiry probe transport: " + status.error_message());
        if(response.status().code() == static_cast<int>(absl::StatusCode::kFailedPrecondition))
        {
            if(response.termination_cause() != wire::ACQUISITION_TERMINATION_CAUSE_EXPIRED ||
               response.incarnation() != acquisition.incarnation || response.writer_id() != acquisition.writer_id)
                fail("same-id terminal retry did not identify the expired tuple");
            expired = true;
            break;
        }
        if(response.status().code() != 0 || response.incarnation() != acquisition.incarnation)
            fail("idle retry changed the live grant");
        std::this_thread::sleep_for(20ms);
    }
    if(!expired)
        fail("idle writer did not expire within lease plus safety margin");

    // W10.12: poll the already terminal tuple until the real Keeper confirms its release revision.
    const auto fence_end = std::chrono::steady_clock::now() + lease_duration + safety_margin;
    bool fenced = false;
    while(std::chrono::steady_clock::now() < fence_end)
    {
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + 1s);
        wire::ReleaseRequest request;
        request.set_story_id(acquisition.story_id);
        request.set_writer_id(acquisition.writer_id);
        request.set_incarnation(acquisition.incarnation);
        wire::ReleaseResponse response;
        auto status = catalog.Release(&context, request, &response);
        if(!status.ok() || response.status().code() != 0)
            fail("expired release fence probe failed");
        if(response.fenced())
        {
            fenced = true;
            break;
        }
        std::this_thread::sleep_for(20ms);
    }
    if(!fenced)
        fail("Keeper never confirmed the expiry fence");
    if(grant.writer.lease().renewals != 0)
        fail("SDK renewal masked Keeper evidence or idle expiry");
    sdk::AppendSpec forbidden;
    forbidden.envelope.payload = "must-not-be-admitted-after-expiry";
    const auto result = grant.writer.append(forbidden);
    if(result.ok() || sdk::rejectionOf(result.status()) != sdk::AppendRejection::FencedExpired ||
       grant.writer.lease().termination_cause != AcquisitionTerminationCause::Expired)
        fail("SDK did not classify the next append as FENCED_EXPIRED: " + result.status().ToString());
}

void verify(sdk::Client& client, StoryId story, const std::map<EventId, Row>& expected)
{
    Hlc end{};
    for(const auto& [id, row]: expected) end = std::max(end, row.hlc);
    ++end.logical;
    const auto deadline = std::chrono::steady_clock::now() + lease_duration + safety_margin;
    while(std::chrono::steady_clock::now() < deadline)
    {
        auto stream = client.read(story, {Hlc{}, end});
        if(!stream.ok())
            fail("read transport: " + stream.status().ToString());
        std::map<EventId, Row> observed;
        std::optional<Event> previous;
        bool complete = false;
        for(size_t frame = 0; frame < 256; ++frame)
        {
            auto item = stream->next(std::chrono::system_clock::now() + 2s);
            if(!item.ok())
                fail("read stream: " + item.status().ToString());
            if(!*item)
                break;
            for(const auto& event: (**item).events)
            {
                if(previous && !ReplayLess(*previous, event))
                    fail("read order regressed");
                previous = event;
                if(!observed.emplace(event.id, Row{event.hlc, event.envelope.payload}).second)
                    fail("read duplicated an event");
            }
            if((**item).completion)
                complete = (**item).completion->complete;
        }
        if(complete)
        {
            if(observed.size() != expected.size())
                fail("complete whole-range read contains a missing or unacknowledged event");
            for(const auto& [id, row]: expected)
            {
                const auto found = observed.find(id);
                if(found == observed.end() || found->second.hlc != row.hlc || found->second.payload != row.payload)
                    fail("complete read changed an acknowledged identity, HLC or payload");
            }
            return;
        }
        std::this_thread::sleep_for(20ms);
    }
    fail("whole-range read remained incomplete");
}
} // namespace

int main(int argc, char** argv)
{
    if(argc != 2)
        fail("expected Catalog endpoint");
    sdk::ClientOptions options;
    options.catalog_endpoint = argv[1];
    options.rpc_timeout = 2s;
    options.retry.max_retries = 0;
    // Only the SDK scheduler clock is held still. Real Visor and Keeper clocks advance normally;
    // successful appends beyond the original lease therefore require real heartbeat evidence.
    options.lease.boottime_ns = [] { return int64_t{1}; };
    options.lease.margin = 0ms;
    auto client = sdk::Client::Connect(options);
    if(!client.ok() || !client->createChronicle("lease-lifecycle").ok())
        fail("connect or create chronicle");
    auto story = client->createStory("lease-lifecycle", "events");
    if(!story.ok())
        fail("create story: " + story.status().ToString());
    auto catalog = wire::Catalog::NewStub(grpc::CreateChannel(argv[1], grpc::InsecureChannelCredentials()));
    auto first = acquire(*client, story->id);
    const auto prior = first.writer.acquisition();
    if(client->destroyStory(story->id).code() != absl::StatusCode::kFailedPrecondition ||
       client->destroyChronicle("lease-lifecycle").code() != absl::StatusCode::kFailedPrecondition)
        fail("I3.6 destroy did not refuse a live lease");
    std::map<EventId, Row> expected;
    // Three lease durations of traffic, bounded to 90 operations, without a Catalog renewal.
    for(size_t event = 0; event < 90; ++event)
    {
        append(first.writer, "active-" + std::to_string(event), expected);
        std::this_thread::sleep_for(100ms);
    }
    if(first.writer.lease().renewals != 0)
        fail("active writer used SDK renewal");
    std::cout << "PASS active writer renewed only through Keeper evidence\n";
    expire(*catalog, first);
    std::cout << "PASS idle expiry and SDK FENCED_EXPIRED\n";
    auto successor = acquire(*client, story->id);
    const auto next = successor.writer.acquisition();
    if(next.writer_id != prior.writer_id || next.incarnation <= prior.incarnation)
        fail("re-acquire did not preserve writer id and advance incarnation");
    append(successor.writer, "successor", expected);
    expire(*catalog, successor);
    verify(*client, story->id, expected);
    std::cout << "PASS new incarnation and complete exact read acknowledged=" << expected.size() << '\n';
    if(!client->destroyStory(story->id).ok() || !client->destroyChronicle("lease-lifecycle").ok())
        fail("I3.6 destroy refused after confirmed expiry");
    std::cout << "PASS story and chronicle destroy after expiry\n";
}
