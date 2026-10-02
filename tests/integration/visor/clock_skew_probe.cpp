// Clock skew probe for tests/end-to-end/clock-skew/clock_skew_harness.sh (section 8).
//
// The probe reads Cluster.ReadClock the way a Cristian client does and checks the estimate against the injected
// skew. Its local time comes from the SDK ChronoClock over an injected TimeSource, so the skew is applied through
// the same seam the SDK offers production code. On one host the Visor authority tick and the probe share the
// CLOCK_MONOTONIC epoch, so a clock skewed by S must measure an offset of -S. The error must stay within the
// reported uncertainty (RTT/2) plus 2 ms of slack.
//
//   clock_skew_probe offset ENDPOINT SKEW_NS [STEP_NS]   exchange, optionally step the clock mid-run, exchange again
//   clock_skew_probe write CATALOG ENDPOINT SKEW_NS      append through a skewed SDK client, then exchange
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

#include <grpcpp/grpcpp.h>

#include "chronolog/client/client.h"
#include "chronolog/client/clock.h"
#include "chronolog/internal/v1/internal.grpc.pb.h"

namespace sdk = chronolog::client;
namespace iv1 = chronolog::internal::v1;
using namespace std::chrono_literals;

namespace
{
constexpr int64_t kSlackNs = 2'000'000;
constexpr int kExchanges = 30;

std::atomic<int64_t> g_skew{0};

int64_t steadyNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
}

chronolog::TimeReading skewedReading()
{
    return chronolog::TimeReading{steadyNs() + g_skew.load(), {}, chronolog::ClockStatus::Unsynced};
}

// Event timestamps are wall-clock readings, which the Keeper checks against its acceptance window, so the writer's
// source is skewed realtime. The exchange uses the monotonic source because the authority tick is monotonic.
chronolog::TimeReading skewedRealtime()
{
    const auto now =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
                    .count();
    return chronolog::TimeReading{now + g_skew.load(), {}, chronolog::ClockStatus::Unsynced};
}

struct Estimate
{
    int64_t offset_ns{};
    int64_t uncertainty_ns{};
};

// Best of kExchanges by round trip, with offset = authority + RTT/2 - t2 as documented on Cluster.ReadClock.
std::optional<Estimate> exchange(iv1::Cluster::Stub& stub, sdk::ChronoClock& clock)
{
    std::optional<Estimate> best;
    int64_t best_rtt = INT64_MAX;
    auto deadline = std::chrono::steady_clock::now() + 15s;
    for(int taken = 0; taken < kExchanges && std::chrono::steady_clock::now() < deadline;)
    {
        auto t1 = clock.now();
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + 2s);
        iv1::ReadClockResponse response;
        auto status = stub.ReadClock(&context, iv1::ReadClockRequest(), &response);
        auto t2 = clock.now();
        if(!status.ok() || !t1.ok() || !t2.ok())
        {
            // The leader lease takes a moment after the Visor starts.
            std::this_thread::sleep_for(50ms);
            continue;
        }
        ++taken;
        const int64_t rtt = t2->physical_ns - t1->physical_ns;
        if(rtt < best_rtt)
        {
            best_rtt = rtt;
            best = Estimate{response.authority_tick_ns() + rtt / 2 - t2->physical_ns, rtt / 2};
        }
    }
    return best;
}

bool check(const std::string& label, const std::optional<Estimate>& estimate, int64_t expected_ns)
{
    if(!estimate)
    {
        std::cout << "FAIL " << label << ": no ReadClock answer\n";
        return false;
    }
    const int64_t error = std::llabs(estimate->offset_ns - expected_ns);
    const int64_t bound = estimate->uncertainty_ns + kSlackNs;
    const bool ok = error <= bound;
    std::cout << (ok ? "PASS " : "FAIL ") << label << ": offset=" << estimate->offset_ns << " expected=" << expected_ns
              << " error=" << error << " uncertainty=" << estimate->uncertainty_ns << " bound=" << bound << '\n';
    return ok;
}

std::unique_ptr<iv1::Cluster::Stub> clusterStub(const std::string& endpoint)
{
    return iv1::Cluster::NewStub(grpc::CreateChannel(endpoint, grpc::InsecureChannelCredentials()));
}

int offsetMode(const std::string& endpoint, int64_t skew, std::optional<int64_t> step)
{
    g_skew = skew;
    sdk::ChronoClock clock(skewedReading);
    auto stub = clusterStub(endpoint);
    bool ok = check("skew=" + std::to_string(skew), exchange(*stub, clock), -skew);
    if(step)
    {
        g_skew = skew + *step;
        ok &= check("after step=" + std::to_string(*step), exchange(*stub, clock), -(skew + *step));
    }
    return ok ? 0 : 1;
}

int writeMode(const std::string& catalog, const std::string& endpoint, int64_t skew)
{
    g_skew = skew;
    sdk::ClientOptions options;
    options.catalog_endpoint = catalog;
    options.rpc_timeout = 10s;
    options.time_source = skewedRealtime;
    auto client = sdk::Client::Connect(options);
    if(!client.ok())
    {
        std::cout << "FAIL connect: " << client.status() << '\n';
        return 1;
    }
    const std::string name = "skew" + std::to_string(std::llabs(skew)) + (skew < 0 ? "n" : "p");
    if(!client->createChronicle(name).ok())
    {
        std::cout << "FAIL createChronicle\n";
        return 1;
    }
    auto story = client->createStory(name, "events");
    if(!story.ok())
    {
        std::cout << "FAIL createStory: " << story.status() << '\n';
        return 1;
    }
    auto writer = client->acquire(story->id, "skewed-writer");
    if(!writer.ok())
    {
        std::cout << "FAIL acquire: " << writer.status() << '\n';
        return 1;
    }
    // A story created a moment ago reaches the Keeper through the route stream, so early appends may be refused as
    // UNAVAILABLE; each retry appends only the events still unacknowledged.
    constexpr size_t kEvents = 50;
    size_t acked = 0;
    std::string last_error;
    for(auto deadline = std::chrono::steady_clock::now() + 20s;
        acked < kEvents && std::chrono::steady_clock::now() < deadline;)
    {
        std::vector<sdk::AppendSpec> specs(kEvents - acked);
        for(size_t i = 0; i < specs.size(); ++i) specs[i].envelope.payload = "skewed-" + std::to_string(acked + i + 1);
        auto appended = writer->appendBatch(specs);
        if(!appended.ok())
        {
            last_error = appended.status().ToString();
            std::this_thread::sleep_for(100ms);
            continue;
        }
        size_t round = 0;
        for(const auto& result: *appended)
        {
            if(result.ok() && result->acked())
                ++round;
            else if(!result.ok())
                last_error = result.status().ToString();
            if(!result.ok() || !result->acked())
                break;
        }
        acked += round;
        if(round == 0)
            std::this_thread::sleep_for(100ms);
    }
    const bool wrote = acked == kEvents;
    std::cout << (wrote ? "PASS " : "FAIL ") << "skewed writer appended " << acked << "/" << kEvents
              << (wrote ? "" : " " + last_error) << '\n';
    sdk::ChronoClock clock(skewedReading);
    auto stub = clusterStub(endpoint);
    return check("write skew=" + std::to_string(skew), exchange(*stub, clock), -skew) && wrote ? 0 : 1;
}
} // namespace

int main(int argc, char** argv)
{
    const std::string mode = argc > 1 ? argv[1] : "";
    if(mode == "offset" && (argc == 4 || argc == 5))
        return offsetMode(argv[2],
                          std::atoll(argv[3]),
                          argc == 5 ? std::optional<int64_t>(std::atoll(argv[4])) : std::nullopt);
    if(mode == "write" && argc == 5)
        return writeMode(argv[2], argv[3], std::atoll(argv[4]));
    std::cerr << "usage: clock_skew_probe offset ENDPOINT SKEW_NS [STEP_NS] | write CATALOG ENDPOINT SKEW_NS\n";
    return 2;
}
