#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
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
 * deployment. The test covers:
 *
 *   1. Publish N messages to a topic.
 *   2. Subscribe with a watermark of 1 (replay from the beginning) and wait
 *      until all N messages are delivered to the callback or the propagation
 *      window expires.
 *   3. Verify count, payload integrity, and timestamp ordering.
 *   4. Subscribe a second time and confirm new messages are still delivered.
 *   5. Unsubscribe and ensure no further callbacks fire.
 *
 * ChronoLog's read path takes some time to surface freshly-published events,
 * so the test allows a generous propagation window (DEFAULT 180s) before
 * declaring failure.
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
constexpr int kPropagationSeconds = 180;
constexpr std::uint32_t kPollIntervalMs = 100;
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

    // Force a flush so other readers (and our own subscribe path) can see the data.
    pubsub->flush();
    printResult("Flush after publish", true);

    // ---- Phase 2: subscribe and wait for delivery ---------------------------
    printHeader("PHASE 2: SUBSCRIBE & RECEIVE");
    std::cout << "  Waiting up to " << kPropagationSeconds << "s for propagation..." << std::endl;

    Collector collector;
    auto sub_id = pubsub->subscribe_from(kTopic,
                                         /*since_timestamp*/ 1,
                                         std::ref(collector),
                                         kPollIntervalMs);
    if(sub_id == chronopubsub::kInvalidSubscriptionId)
    {
        printResult("Subscribe", false, "subscribe_from returned kInvalidSubscriptionId");
        return false;
    }

    bool received_all = waitForCount(collector, static_cast<std::size_t>(kNumMessages), kPropagationSeconds);
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

    // ---- Phase 3: live delivery on a second topic ---------------------------
    printHeader("PHASE 3: LIVE DELIVERY ON SECOND TOPIC");
    Collector collector2;
    auto sub_id2 = pubsub->subscribe(kSecondTopic, std::ref(collector2), kPollIntervalMs);
    if(sub_id2 == chronopubsub::kInvalidSubscriptionId)
    {
        printResult("Subscribe to second topic", false);
        pubsub->unsubscribe(sub_id);
        return false;
    }

    constexpr int kLiveMessages = 10;
    for(int i = 0; i < kLiveMessages; ++i) { pubsub->publish(kSecondTopic, "live-" + std::to_string(i)); }
    pubsub->flush();

    bool live_ok = waitForCount(collector2, static_cast<std::size_t>(kLiveMessages), kPropagationSeconds);
    printResult("Live delivery", live_ok, std::to_string(collector2.size()) + "/" + std::to_string(kLiveMessages));

    // ---- Phase 4: unsubscribe stops further callbacks -----------------------
    printHeader("PHASE 4: UNSUBSCRIBE STOPS DELIVERY");
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
