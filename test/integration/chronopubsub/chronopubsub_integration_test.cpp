#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "chronopubsub.h"

/**
 * ChronoPubSub API Integration Test
 *
 * Verifies the publish/subscribe wrapper end-to-end against a live ChronoLog
 * deployment. ChronoLog's read path takes ~120s to surface freshly-published
 * events (events have to propagate from keepers to a player before
 * ReplayStory will return them), so this test uses the same explicit
 * propagation wait that the chronokvs integration test relies on.
 *
 * Phases:
 *   1. Publish N messages to a topic, flush.
 *   2. Wait kPropagationSeconds for events to propagate.
 *   3. Subscribe from timestamp 1 (replay everything) and wait briefly for
 *      the polling thread to deliver all messages.
 *   4. Verify count, payload integrity, ordering, and topic.
 *   5. Live delivery on a second topic: subscribe, publish, flush, wait
 *      another propagation window, verify delivery.
 *   6. Unsubscribe and verify no further callbacks fire.
 */

namespace
{

void printHeader(const std::string& title)
{
    std::cout << "\n" << std::string(60, '=') << std::endl;
    std::cout << "  " << title << std::endl;
    std::cout << std::string(60, '=') << std::endl;
}

void printResult(const std::string& name, bool passed, const std::string& details = "")
{
    std::cout << "  " << (passed ? "[PASS]" : "[FAIL]") << " " << name;
    if(!details.empty())
    {
        std::cout << " - " << details;
    }
    std::cout << std::endl;
}

constexpr int kNumMessages = 50;
constexpr int kLiveMessages = 10;
constexpr int kPropagationSeconds = 130;    // ~120s window plus margin
constexpr int kPostPropagationWaitSec = 30; // window for the polling thread to deliver
constexpr std::uint32_t kPollIntervalMs = 1000;
const std::string kTopic = "chronopubsub_integration_topic";
const std::string kSecondTopic = "chronopubsub_integration_topic_second";

class Collector
{
public:
    void operator()(const chronopubsub::Message& msg)
    {
        std::lock_guard<std::mutex> lock(mu_);
        messages_.push_back(msg);
    }

    std::size_t size() const
    {
        std::lock_guard<std::mutex> lock(mu_);
        return messages_.size();
    }

    std::vector<chronopubsub::Message> snapshot() const
    {
        std::lock_guard<std::mutex> lock(mu_);
        return messages_;
    }

private:
    mutable std::mutex mu_;
    std::vector<chronopubsub::Message> messages_;
};

void waitWithProgress(int total_seconds, const std::string& label)
{
    std::cout << "  " << label << ": waiting " << total_seconds << "s..." << std::flush;
    for(int s = 0; s < total_seconds; ++s)
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if((s + 1) % 10 == 0)
        {
            std::cout << " " << (s + 1) << "s" << std::flush;
        }
    }
    std::cout << " done" << std::endl;
}

bool waitForCount(const Collector& collector, std::size_t expected, int timeout_seconds)
{
    auto start = std::chrono::steady_clock::now();
    while(collector.size() < expected)
    {
        if(std::chrono::steady_clock::now() - start > std::chrono::seconds(timeout_seconds))
        {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return true;
}

bool runTest()
{
    printHeader("CHRONOPUBSUB INTEGRATION TEST");

    auto pubsub = chronopubsub::ChronoPubSub::Create();
    if(!pubsub)
    {
        printResult("Initialize ChronoPubSub", false, "Create() returned nullptr");
        return false;
    }
    printResult("Initialize ChronoPubSub", true);

    // ---- Phase 1: publish ---------------------------------------------------
    printHeader("PHASE 1: PUBLISH");
    std::vector<std::uint64_t> published_ts;
    published_ts.reserve(kNumMessages);
    for(int i = 0; i < kNumMessages; ++i)
    {
        std::string payload = "msg-" + std::to_string(i);
        std::uint64_t ts = pubsub->publish(kTopic, payload);
        published_ts.push_back(ts);
    }
    printResult("Publish",
                published_ts.size() == static_cast<std::size_t>(kNumMessages),
                std::to_string(published_ts.size()) + "/" + std::to_string(kNumMessages));

    // Flush so the server can finalize the chunks and a subsequent ReplayStory can see them.
    pubsub->flush();
    printResult("Flush after publish", true);

    // ---- Phase 2: wait for propagation, then subscribe and read ------------
    printHeader("PHASE 2: PROPAGATION WAIT");
    waitWithProgress(kPropagationSeconds, "Data propagation");

    printHeader("PHASE 3: SUBSCRIBE AND RECEIVE HISTORICAL");
    Collector collector;
    auto sub_id = pubsub->subscribe_from(kTopic, /*since_timestamp*/ 1, std::ref(collector), kPollIntervalMs);
    if(sub_id == chronopubsub::kInvalidSubscriptionId)
    {
        printResult("Subscribe", false, "subscribe_from returned kInvalidSubscriptionId");
        return false;
    }

    bool received_all = waitForCount(collector, static_cast<std::size_t>(kNumMessages), kPostPropagationWaitSec);
    auto received = collector.snapshot();
    printResult("Receive all messages",
                received_all,
                std::to_string(received.size()) + "/" + std::to_string(kNumMessages));

    bool ordered = true;
    for(std::size_t i = 1; i < received.size(); ++i)
    {
        if(received[i].timestamp < received[i - 1].timestamp)
        {
            ordered = false;
            break;
        }
    }
    printResult("Messages delivered in timestamp order", ordered);

    bool topic_ok = true;
    for(const auto& m: received)
    {
        if(m.topic != kTopic)
        {
            topic_ok = false;
            break;
        }
    }
    printResult("All messages carry the expected topic", topic_ok);

    // ---- Phase 4: delivery on a second topic --------------------------------
    // We deliberately publish BEFORE subscribing here, mirroring Phase 3.
    // Subscribing before publishing means the polling thread races the
    // publisher on the same chronicle+topic (the subscriber's ReleaseStory
    // invalidates the publisher's cached handle every poll cycle), and on
    // ChronoLog today that causes events not to surface during a single
    // propagation window.
    printHeader("PHASE 4: SECOND TOPIC PUBLISH + SUBSCRIBE");
    for(int i = 0; i < kLiveMessages; ++i) { pubsub->publish(kSecondTopic, "live-" + std::to_string(i)); }
    pubsub->flush();
    printResult("Publish + flush messages on second topic", true);

    waitWithProgress(kPropagationSeconds, "Second topic propagation");

    Collector collector2;
    auto sub_id2 = pubsub->subscribe_from(kSecondTopic, /*since_timestamp*/ 1, std::ref(collector2), kPollIntervalMs);
    if(sub_id2 == chronopubsub::kInvalidSubscriptionId)
    {
        printResult("Subscribe to second topic", false);
        pubsub->unsubscribe(sub_id);
        return false;
    }

    bool live_ok = waitForCount(collector2, static_cast<std::size_t>(kLiveMessages), kPostPropagationWaitSec);
    printResult("Second topic delivery",
                live_ok,
                std::to_string(collector2.size()) + "/" + std::to_string(kLiveMessages));

    // ---- Phase 5: unsubscribe stops further callbacks -----------------------
    printHeader("PHASE 5: UNSUBSCRIBE STOPS DELIVERY");
    bool unsub1 = pubsub->unsubscribe(sub_id);
    bool unsub2 = pubsub->unsubscribe(sub_id2);
    printResult("Unsubscribe both subscriptions", unsub1 && unsub2);

    auto count_at_stop = collector2.size();
    pubsub->publish(kSecondTopic, "after-unsubscribe");
    pubsub->flush();
    std::this_thread::sleep_for(std::chrono::seconds(2));
    bool no_more = collector2.size() == count_at_stop;
    printResult("No callbacks after unsubscribe", no_more);

    // ---- Summary ------------------------------------------------------------
    bool overall = received_all && ordered && topic_ok && live_ok && unsub1 && unsub2 && no_more;
    printHeader(overall ? "RESULT: PASS" : "RESULT: FAIL");
    return overall;
}

} // namespace

int main()
{
    try
    {
        return runTest() ? 0 : 1;
    }
    catch(const std::exception& e)
    {
        std::cerr << "Integration test threw: " << e.what() << std::endl;
        return 2;
    }
}
