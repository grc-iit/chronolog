#include "chronolog/kvs/store.h"
#include <gtest/gtest.h>
#include <limits>

using namespace chronolog;
namespace
{
std::string catalog, player;
client::Client connect()
{
    client::ClientOptions options;
    options.catalog_endpoint = catalog;
    options.player_endpoint = player;
    auto result = client::Client::Connect(options);
    if(!result.ok())
        throw std::runtime_error(result.status().ToString());
    return std::move(*result);
}
Hlc after(Hlc h)
{
    ++h.logical;
    return h;
}
TEST(Kvs, VersionsAsOfBatchAndTombstone)
{
    auto c = connect();
    kvs::Store store(c, "kvs-versions");
    auto a = store.put("workflow", "pending");
    ASSERT_TRUE(a.ok()) << a.status();
    ASSERT_TRUE(a->acked());
    std::vector<std::string> values{"running", "done"};
    auto batch = store.putBatch("workflow", values);
    ASSERT_TRUE(batch.ok()) << batch.status();
    ASSERT_EQ(batch->size(), 2u);
    ASSERT_TRUE((*batch)[0].ok());
    ASSERT_TRUE((*batch)[1].ok());
    auto b = *(*batch)[0];
    auto d = *(*batch)[1];
    EXPECT_LT(a->hlc, b.hlc);
    EXPECT_LT(b.hlc, d.hlc);
    auto at = store.get("workflow", {.at = b.hlc});
    ASSERT_TRUE(at.ok()) << at.status();
    ASSERT_TRUE(at->status.ok());
    ASSERT_TRUE(at->value);
    EXPECT_EQ(at->value->value, "pending");
    EXPECT_TRUE(at->completion.complete);
    auto latest = store.get("workflow", {.causal_floor = d.hlc});
    ASSERT_TRUE(latest.ok()) << latest.status();
    ASSERT_TRUE(latest->value);
    EXPECT_EQ(latest->value->value, "done");
    EXPECT_TRUE(latest->completion.complete);
    auto history = store.history("workflow", {{}, after(d.hlc)});
    ASSERT_TRUE(history.ok());
    std::vector<kvs::Version> versions;
    bool complete = false;
    for(int i = 0; i < 32; ++i)
    {
        auto item = history->next();
        ASSERT_TRUE(item.ok()) << item.status();
        if(!*item)
            break;
        for(const auto& v: (**item).versions) versions.push_back(v.version);
        if((**item).completion)
        {
            complete = (**item).completion->complete;
            break;
        }
    }
    ASSERT_EQ(versions.size(), 3u);
    EXPECT_EQ(versions[0].event_id, a->event_id);
    EXPECT_TRUE(complete);
    auto erased = store.erase("workflow");
    ASSERT_TRUE(erased.ok());
    auto gone = store.get("workflow", {.causal_floor = erased->hlc});
    ASSERT_TRUE(gone.ok()) << gone.status();
    EXPECT_TRUE(absl::IsNotFound(gone->status));
    ASSERT_TRUE(gone->value);
    EXPECT_TRUE(gone->value->deleted);
    EXPECT_EQ(gone->value->version.event_id, erased->event_id);
    auto empty = store.put("empty", "");
    ASSERT_TRUE(empty.ok());
    auto read = store.get("empty", {.causal_floor = empty->hlc});
    ASSERT_TRUE(read.ok());
    EXPECT_TRUE(read->status.ok());
    ASSERT_TRUE(read->value);
    EXPECT_TRUE(read->value->value.empty());
}
TEST(Kvs, CrossClientFloorWatchAndEviction)
{
    auto c = connect();
    auto other = connect();
    kvs::Store first(c, "kvs-cross");
    kvs::Store second(other, "kvs-cross", {1, 2, 1024});
    auto v = first.put("agent", "one");
    ASSERT_TRUE(v.ok());
    auto r = second.get("agent", {.causal_floor = v->hlc});
    ASSERT_TRUE(r.ok()) << r.status();
    ASSERT_TRUE(r->value);
    EXPECT_EQ(r->value->value, "one");
    EXPECT_TRUE(r->completion.complete);
    ASSERT_TRUE(second.watch("agent").ok());
    auto two = first.put("agent", "two");
    ASSERT_TRUE(two.ok());
    bool observed = false;
    for(int i = 0; i < 8 && !observed; ++i)
    {
        auto item = second.follow("agent");
        ASSERT_TRUE(item.ok()) << item.status();
        for(const auto& value: item->versions)
            if(value.version.event_id == two->event_id)
                observed = true;
    }
    ASSERT_TRUE(observed);
    auto three = first.put("agent", "three");
    ASSERT_TRUE(three.ok());
    auto four = first.put("agent", "four");
    ASSERT_TRUE(four.ok());
    auto current = second.get("agent", {.causal_floor = four->hlc});
    ASSERT_TRUE(current.ok());
    ASSERT_TRUE(current->value);
    EXPECT_EQ(current->value->value, "four");
    auto old = second.get("agent", {.at = two->hlc});
    ASSERT_TRUE(old.ok());
    ASSERT_TRUE(old->value);
    EXPECT_EQ(old->value->value, "one");
    ASSERT_TRUE(second.put("evict", "x").ok());
    EXPECT_EQ(second.cachedKeys(), 1u);
    auto cold = second.get("agent", {.causal_floor = four->hlc});
    ASSERT_TRUE(cold.ok()) << cold.status();
    ASSERT_TRUE(cold->value);
    EXPECT_EQ(cold->value->value, "four");
    EXPECT_EQ(second.cachedKeys(), 1u);
}
TEST(Kvs, ValidationMetadataAndIncompleteCompletion)
{
    auto c = connect();
    kvs::Store store(c, "kvs-errors");
    EXPECT_TRUE(absl::IsInvalidArgument(store.put("", "v").status()));
    EXPECT_TRUE(absl::IsInvalidArgument(store.get(std::string(256, 'x')).status()));
    EXPECT_TRUE(absl::IsInvalidArgument(store.put("bad\nkey", "v").status()));
    kvs::PutOptions opts;
    opts.metadata.content_type = "application/json";
    opts.metadata.trace_id = std::string(16, 't');
    opts.metadata.span_id = std::string(8, 's');
    opts.metadata.attributes["gen_ai.agent.name"] = "agent";
    auto v = store.put("valid", "{}", opts);
    ASSERT_TRUE(v.ok());
    auto r = store.get("valid", {.at = Hlc{std::numeric_limits<int64_t>::max(), 0}});
    ASSERT_TRUE(r.ok()) << r.status();
    EXPECT_FALSE(r->completion.complete);
    EXPECT_NE(r->completion.reason, IncompleteReason::None);
    ASSERT_TRUE(r->value);
    EXPECT_EQ(r->value->envelope.trace_id, opts.metadata.trace_id);
    EXPECT_EQ(r->value->envelope.content_type, "application/json");
    auto missing = store.get("missing");
    EXPECT_TRUE(absl::IsNotFound(missing.status()));
    auto deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(100);
    auto waiting = store.get("valid", {.causal_floor = Hlc{v->hlc.physical_ns + 1000000000, 0}, .deadline = deadline});
    EXPECT_FALSE(waiting.ok());
    EXPECT_TRUE(absl::IsDeadlineExceeded(waiting.status())) << waiting.status();
}
} // namespace
int main(int argc, char** argv)
{
    if(argc != 3)
        return 2;
    catalog = argv[1];
    player = argv[2];
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
