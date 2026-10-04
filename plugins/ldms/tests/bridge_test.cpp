#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <set>
#include <thread>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <unistd.h>
#include "chronolog_ldms_bridge.h"
#include "core.h"
#include "reader.h"
using namespace chronolog;
using Json = nlohmann::json;
using namespace std::chrono_literals;
namespace
{
std::string catalog, player;
std::atomic<int> serial{0};
std::string unique(const char* prefix)
{
    return std::string(prefix) + "-" + std::to_string(::getpid()) + "-" + std::to_string(++serial);
}
int64_t nowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
}
chrono_ldms_t* openBridge(uint32_t capacity, uint32_t batch, uint32_t flushMs)
{
    chrono_ldms_config cfg{};
    cfg.catalog_endpoint = catalog.c_str();
    cfg.player_endpoint = player.c_str();
    cfg.queue_capacity = capacity;
    cfg.batch_size = batch;
    cfg.flush_interval_ms = flushMs;
    chrono_ldms_t* bridge = nullptr;
    char error[256];
    EXPECT_EQ(chrono_ldms_open(&cfg, &bridge, error, sizeof(error)), CHRONO_LDMS_OK) << error;
    return bridge;
}
std::string sampleJson(uint64_t ts, int n)
{
    return Json({{"producer", "p"},
                 {"instance", "p/set"},
                 {"schema", "s"},
                 {"component_id", 1},
                 {"timestamp_ns", ts},
                 {"metrics", {{"n", n}}}})
            .dump();
}
std::vector<ldms::StoryEvents> readBack(const std::string& container)
{
    client::ClientOptions options;
    options.catalog_endpoint = catalog;
    options.player_endpoint = player;
    auto sdk = client::Client::Connect(options);
    EXPECT_TRUE(sdk.ok());
    auto stories = ldms::readChronicle(*sdk, container);
    EXPECT_TRUE(stories.ok()) << stories.status();
    return stories.ok() ? *stories : std::vector<ldms::StoryEvents>{};
}
} // namespace

TEST(LdmsQueue, BoundsAndOrderAcrossProducers)
{
    ldms::BoundedQueue<int*> queue(5);
    EXPECT_EQ(queue.slots(), 8u);
    std::vector<int> values(8);
    int pushed = 0;
    for(auto& v: values) pushed += queue.push(&v) ? 1 : 0;
    EXPECT_EQ(pushed, 8);
    int extra = 0;
    EXPECT_FALSE(queue.push(&extra));
    int* out = nullptr;
    for(auto& v: values)
    {
        ASSERT_TRUE(queue.pop(out));
        EXPECT_EQ(out, &v);
    }
    EXPECT_FALSE(queue.pop(out));
    EXPECT_TRUE(queue.push(&extra));
}
TEST(LdmsQueue, ManyProducersOneConsumerLoseNothing)
{
    ldms::BoundedQueue<uintptr_t> queue(256);
    constexpr int kProducers = 4, kEach = 5000;
    std::vector<std::thread> producers;
    for(int p = 0; p < kProducers; ++p)
        producers.emplace_back(
                [&, p]
                {
                    for(int i = 0; i < kEach; ++i)
                        while(!queue.push(static_cast<uintptr_t>(p) * kEach + i + 1)) std::this_thread::yield();
                });
    std::set<uintptr_t> seen;
    const auto stop = std::chrono::steady_clock::now() + 20s;
    while(seen.size() < kProducers * kEach && std::chrono::steady_clock::now() < stop)
    {
        uintptr_t v = 0;
        if(queue.pop(v))
            EXPECT_TRUE(seen.insert(v).second);
        else
            std::this_thread::yield();
    }
    for(auto& t: producers) t.join();
    EXPECT_EQ(seen.size(), static_cast<size_t>(kProducers * kEach));
}
TEST(LdmsCore, NamesAndKeeperWindow)
{
    EXPECT_EQ(ldms::sanitizeName("node1.example.org"), "node1_example_org");
    EXPECT_EQ(ldms::sanitizeName("a/b c-d_e"), "a_b_c-d_e");
    const int64_t now = 1'000'000'000'000;
    EXPECT_TRUE(ldms::inKeeperWindow(now, now));
    EXPECT_TRUE(ldms::inKeeperWindow(now - 9'000'000'000, now));
    EXPECT_FALSE(ldms::inKeeperWindow(now - 11'000'000'000, now));
    EXPECT_TRUE(ldms::inKeeperWindow(now + 29'000'000'000, now));
    EXPECT_FALSE(ldms::inKeeperWindow(now + 31'000'000'000, now));
}
TEST(LdmsEncode, OneSampleIsOneJsonObject)
{
    chrono_ldms_metric metrics[4] = {};
    metrics[0] = {"a", CHRONO_LDMS_U64, {}};
    metrics[0].v.u64 = 18446744073709551615ull;
    metrics[1] = {"b", CHRONO_LDMS_I64, {}};
    metrics[1].v.i64 = -5;
    metrics[2] = {"c", CHRONO_LDMS_F64, {}};
    metrics[2].v.f64 = 1.5;
    metrics[3] = {"d", CHRONO_LDMS_F64, {}};
    metrics[3].v.f64 = std::nan("");
    chrono_ldms_sample sample{"host.example", "host.example/meminfo", "meminfo", 7, 123456789, metrics, 4};
    char buffer[512];
    const int64_t len = chrono_ldms_encode(&sample, buffer, sizeof(buffer));
    ASSERT_GT(len, 0);
    auto doc = Json::parse(buffer);
    EXPECT_EQ(doc["producer"], "host.example");
    EXPECT_EQ(doc["instance"], "host.example/meminfo");
    EXPECT_EQ(doc["schema"], "meminfo");
    EXPECT_EQ(doc["component_id"], 7);
    EXPECT_EQ(doc["timestamp_ns"], 123456789);
    EXPECT_EQ(doc["metrics"]["a"].get<uint64_t>(), 18446744073709551615ull);
    EXPECT_EQ(doc["metrics"]["b"], -5);
    EXPECT_EQ(doc["metrics"]["c"], 1.5);
    EXPECT_TRUE(doc["metrics"]["d"].is_null());
    char tiny[8];
    EXPECT_EQ(chrono_ldms_encode(&sample, tiny, sizeof(tiny)), len);
    EXPECT_EQ(strlen(tiny), 7u);
    sample.metrics = nullptr;
    EXPECT_LT(chrono_ldms_encode(&sample, buffer, sizeof(buffer)), 0);
}
TEST(LdmsBridge, OpenRejectsMissingEndpoint)
{
    chrono_ldms_config cfg{};
    chrono_ldms_t* bridge = reinterpret_cast<chrono_ldms_t*>(1);
    char error[256];
    EXPECT_EQ(chrono_ldms_open(&cfg, &bridge, error, sizeof(error)), CHRONO_LDMS_INVALID_ARGUMENT);
    EXPECT_EQ(bridge, nullptr);
    EXPECT_NE(std::string(error).find("catalog_endpoint"), std::string::npos);
}

TEST(LdmsStack, FullQueueDropsWithoutBlockingAndDrainsTheRest)
{
    const std::string container = unique("ldms-full");
    auto* bridge = openBridge(8, 1000, 5000);
    ASSERT_NE(bridge, nullptr);
    chrono_ldms_stream_t* stream = nullptr;
    ASSERT_EQ(chrono_ldms_stream(bridge, container.c_str(), "s", "p", &stream), CHRONO_LDMS_OK);
    int accepted = 0, dropped = 0;
    std::chrono::nanoseconds slowest{0};
    for(int i = 0; i < 50; ++i)
    {
        const auto json = sampleJson(nowNs(), i);
        const auto begin = std::chrono::steady_clock::now();
        const int rc = chrono_ldms_store(stream, nowNs(), json.data(), json.size());
        slowest = std::max(slowest, std::chrono::steady_clock::now() - begin);
        (rc == CHRONO_LDMS_OK ? accepted : dropped) += 1;
        EXPECT_TRUE(rc == CHRONO_LDMS_OK || rc == CHRONO_LDMS_DROPPED);
    }
    EXPECT_EQ(accepted, 8);
    EXPECT_EQ(dropped, 42);
    EXPECT_LT(slowest, 50ms);
    chrono_ldms_stats stats;
    chrono_ldms_stats_get(bridge, &stats);
    EXPECT_EQ(stats.queued, 8u);
    EXPECT_EQ(stats.dropped, 42u);
    ASSERT_EQ(chrono_ldms_flush(bridge, 20000), CHRONO_LDMS_OK);
    chrono_ldms_stats_get(bridge, &stats);
    EXPECT_EQ(stats.queued, 0u);
    EXPECT_EQ(stats.appended, 8u);
    EXPECT_EQ(stats.failed, 0u);
    EXPECT_EQ(chrono_ldms_close(bridge, 10000), CHRONO_LDMS_OK);
    auto stories = readBack(container);
    ASSERT_EQ(stories.size(), 1u);
    EXPECT_TRUE(stories[0].complete);
    EXPECT_EQ(stories[0].events.size(), 8u);
    for(size_t i = 0; i < stories[0].events.size(); ++i)
        EXPECT_EQ(Json::parse(stories[0].events[i].envelope.payload)["metrics"]["n"], static_cast<int>(i));
}
TEST(LdmsStack, InvalidSampleIsCountedAndTheRestStillLands)
{
    const std::string container = unique("ldms-bad");
    auto* bridge = openBridge(16, 16, 20);
    chrono_ldms_stream_t* stream = nullptr;
    ASSERT_EQ(chrono_ldms_stream(bridge, container.c_str(), "s", "p", &stream), CHRONO_LDMS_OK);
    const auto good = sampleJson(nowNs(), 1);
    EXPECT_EQ(chrono_ldms_store(stream, nowNs(), "not json", 8), CHRONO_LDMS_OK);
    EXPECT_EQ(chrono_ldms_store(stream, nowNs(), good.data(), good.size()), CHRONO_LDMS_OK);
    EXPECT_EQ(chrono_ldms_store(stream, nowNs(), "", 0), CHRONO_LDMS_INVALID_ARGUMENT);
    ASSERT_EQ(chrono_ldms_flush(bridge, 20000), CHRONO_LDMS_OK);
    chrono_ldms_stats stats;
    chrono_ldms_stats_get(bridge, &stats);
    EXPECT_EQ(stats.failed, 1u);
    EXPECT_EQ(stats.appended, 1u);
    EXPECT_NE(std::string(stats.last_error).find("JSON"), std::string::npos);
    EXPECT_EQ(chrono_ldms_close(bridge, 10000), CHRONO_LDMS_OK);
    auto stories = readBack(container);
    ASSERT_EQ(stories.size(), 1u);
    EXPECT_EQ(stories[0].events.size(), 1u);
}
TEST(LdmsStack, CloseDrainsQueuedSamples)
{
    const std::string container = unique("ldms-close");
    auto* bridge = openBridge(64, 1000, 5000);
    for(int p = 0; p < 2; ++p)
    {
        chrono_ldms_stream_t* stream = nullptr;
        ASSERT_EQ(chrono_ldms_stream(bridge, container.c_str(), "s", p == 0 ? "p0" : "p1", &stream), CHRONO_LDMS_OK);
        for(int i = 0; i < 10; ++i)
        {
            const auto json = sampleJson(nowNs(), i);
            ASSERT_EQ(chrono_ldms_store(stream, nowNs(), json.data(), json.size()), CHRONO_LDMS_OK);
        }
    }
    EXPECT_EQ(chrono_ldms_close(bridge, 20000), CHRONO_LDMS_OK);
    auto stories = readBack(container);
    ASSERT_EQ(stories.size(), 2u);
    for(const auto& s: stories)
    {
        EXPECT_TRUE(s.complete);
        EXPECT_EQ(s.events.size(), 10u);
    }
}
TEST(LdmsStack, PhysicalTimeFollowsTheKeeperWindowRule)
{
    const std::string container = unique("ldms-time");
    auto* bridge = openBridge(16, 16, 20);
    chrono_ldms_stream_t* stream = nullptr;
    ASSERT_EQ(chrono_ldms_stream(bridge, container.c_str(), "s", "p", &stream), CHRONO_LDMS_OK);
    const uint64_t inside = static_cast<uint64_t>(nowNs()) - 2'000'000'000ull;
    const uint64_t old = static_cast<uint64_t>(nowNs()) - 3600'000'000'000ull;
    const uint64_t future = static_cast<uint64_t>(nowNs()) + 3600'000'000'000ull;
    for(uint64_t ts: {inside, old, future})
    {
        const auto json = sampleJson(ts, 0);
        ASSERT_EQ(chrono_ldms_store(stream, ts, json.data(), json.size()), CHRONO_LDMS_OK);
    }
    ASSERT_EQ(chrono_ldms_flush(bridge, 20000), CHRONO_LDMS_OK);
    chrono_ldms_stats stats;
    chrono_ldms_stats_get(bridge, &stats);
    EXPECT_EQ(stats.appended, 3u) << stats.last_error;
    EXPECT_EQ(stats.failed, 0u);
    EXPECT_EQ(chrono_ldms_close(bridge, 10000), CHRONO_LDMS_OK);
    auto stories = readBack(container);
    ASSERT_EQ(stories.size(), 1u);
    ASSERT_EQ(stories[0].events.size(), 3u);
    const auto& e = stories[0].events;
    EXPECT_EQ(e[0].physical.physical_ns, static_cast<int64_t>(inside));
    EXPECT_EQ(e[0].physical.status, ClockStatus::Unsynced);
    EXPECT_FALSE(e[0].physical.uncertainty_ns.has_value());
    EXPECT_EQ(e[0].envelope.attributes.count("ldms.timestamp_ns"), 0u);
    const std::pair<size_t, uint64_t> outside[] = {{1, old}, {2, future}};
    for(const auto& [i, ts]: outside)
    {
        EXPECT_EQ(e[i].envelope.attributes.at("ldms.timestamp_ns"), std::to_string(ts));
        EXPECT_NE(e[i].physical.physical_ns, static_cast<int64_t>(ts));
        EXPECT_LT(std::llabs(e[i].physical.physical_ns - nowNs()), 60'000'000'000ll);
        EXPECT_EQ(Json::parse(e[i].envelope.payload)["timestamp_ns"].get<uint64_t>(), ts);
    }
    for(const auto& event: e)
    {
        EXPECT_EQ(event.envelope.content_type, "application/vnd.chronolog.ldms-sample+json");
        EXPECT_EQ(event.envelope.attributes.at("ldms.producer"), "p");
        EXPECT_EQ(event.envelope.attributes.at("ldms.schema"), "s");
        EXPECT_EQ(event.envelope.attributes.at("ldms.instance"), "p/set");
        EXPECT_EQ(event.durability, Durability::Durable);
    }
}
TEST(LdmsStack, FakeLdmsdThreadsMapProducersToStories)
{
    const char* fake = std::getenv("CHRONOLOG_LDMS_FAKE");
    ASSERT_NE(fake, nullptr);
    const std::string container = unique("ldms-fake");
    const std::string command = std::string(fake) + " --catalog " + catalog + " --player " + player + " --container " +
                                container + " --producers 3 --samples 20 --stale-every 5";
    ASSERT_EQ(std::system(command.c_str()), 0);
    auto stories = readBack(container);
    ASSERT_EQ(stories.size(), 3u);
    std::set<std::string> names;
    for(const auto& s: stories)
    {
        names.insert(s.story.name);
        EXPECT_TRUE(s.complete);
        EXPECT_EQ(s.events.size(), 20u);
        size_t stale = 0;
        for(const auto& event: s.events)
        {
            stale += event.envelope.attributes.count("ldms.timestamp_ns");
            auto doc = Json::parse(event.envelope.payload);
            EXPECT_EQ(doc["metrics"].size(), 4u);
            EXPECT_EQ(event.envelope.attributes.at("ldms.producer"), doc["producer"].get<std::string>());
        }
        EXPECT_EQ(stale, 4u);
    }
    EXPECT_EQ(names,
              (std::set<std::string>{"meminfo_node0_example_org",
                                     "meminfo_node1_example_org",
                                     "meminfo_node2_example_org"}));
}
TEST(LdmsStack, FakeLdmsdOverflowDropsInsteadOfBlocking)
{
    const char* fake = std::getenv("CHRONOLOG_LDMS_FAKE");
    ASSERT_NE(fake, nullptr);
    const std::string container = unique("ldms-overflow");
    const std::string command = std::string(fake) + " --catalog " + catalog + " --player " + player + " --container " +
                                container +
                                " --producers 2 --samples 100 --interval-us 0 --queue 4 --batch 1000 --flush-ms 5000"
                                " --allow-drops";
    ASSERT_EQ(std::system(command.c_str()), 0);
}

int main(int argc, char** argv)
{
    if(argc == 3 && argv[1][0] != '-')
    {
        catalog = argv[1];
        player = argv[2];
        argc = 1;
        ::testing::GTEST_FLAG(filter) = "LdmsStack.*";
    }
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
