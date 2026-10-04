#include "chrono-grapher/server/GrapherConfig.h"
#include "chrono-grapher/server/WorkerPool.h"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <future>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

namespace chronolog::grapher
{
namespace
{
using Json = nlohmann::json;
namespace fs = std::filesystem;
struct Archive
{
    fs::path root, local, slow;
    Json configuration;
    std::unique_ptr<FileTierStore> store;
    ManifestRecord record;
    std::vector<Event> events;
    explicit Archive(FileTierStore::Hooks hooks = {})
    {
        const auto* nfs = std::getenv("TIER2G_NFS_ROOT");
        root = fs::temp_directory_path() / ("tier2g-" + std::to_string(::getpid()) + "-" +
                                            ::testing::UnitTest::GetInstance()->current_test_info()->name());
        local = root / "local";
        slow = nfs ? fs::path(nfs) / ::testing::UnitTest::GetInstance()->current_test_info()->name() : root / "slow";
        fs::create_directories(local);
        fs::create_directories(slow);
        Json tiers = Json::array();
        for(const auto& [name, path, rank]: {std::tuple{"local", local, 0}, std::tuple{"slow", slow, 1}})
        {
            struct statfs info
            {
            };
            if(::statfs(path.c_str(), &info) != 0)
                throw std::runtime_error("statfs failed");
            Json tier{{"name", name},
                      {"kind", "posix"},
                      {"root", path.string()},
                      {"rank", rank},
                      {"tier_uuid", rank ? "22222222222222222222222222222222" : "11111111111111111111111111111111"},
                      {"f_type", info.f_type},
                      {"st_dev", 0},
                      {"f_fsid", {info.f_fsid.__val[0], info.f_fsid.__val[1]}}};
            tiers.push_back(tier);
            tier["deployment_id"] = "tier2g";
            std::ofstream(path / ".chronolog-tier.json") << tier.dump();
        }
        configuration = {{"deployment_id", "tier2g"},
                         {"tiers", tiers},
                         {"archive_root", local.string()},
                         {"manifest_writer", "writer"},
                         {"migrate_enabled", true},
                         {"archive_codec", "proto"},
                         {"tier_status_file", (root / "status.json").string()}};
        auto config = load();
        auto opened = FileTierStore::Open(local,
                                          "writer",
                                          {{1, {100, 0}}},
                                          std::make_shared<ProtoChunkCodec>(),
                                          {},
                                          {},
                                          0,
                                          {},
                                          std::move(hooks),
                                          config.tierChain());
        if(!opened.ok())
            throw std::runtime_error(std::string(opened.status().message()));
        store = *std::move(opened);
        Event event;
        event.id = {1, 7, 1, 1};
        event.hlc = {110, 0};
        event.envelope.payload = "tier event";
        events = {event};
        Chunk chunk;
        chunk.id = "window";
        chunk.story_id = 1;
        chunk.start = {100, 0};
        chunk.end = {200, 0};
        chunk.events = events;
        auto published = store->publish(chunk);
        if(!published.ok())
            throw std::runtime_error(std::string(published.status().message()));
        record = *published;
        // Migration moves only files the scrubber's mark covers (I13.13, I13.17).
        if(auto scrubbed = store->scrubOnce(0); !scrubbed.ok())
            throw std::runtime_error(std::string(scrubbed.status().message()));
    }
    GrapherConfig load()
    {
        const auto path = root / "config.json";
        std::ofstream(path) << configuration.dump();
        auto config = GrapherConfig::load(path.string());
        if(!config.ok())
            throw std::runtime_error(std::string(config.status().message()));
        return *config;
    }
    ~Archive()
    {
        store.reset();
        fs::remove_all(root);
        if(std::getenv("TIER2G_NFS_ROOT"))
            fs::remove_all(slow);
    }
    absl::Status pass()
    {
        MigrationWorker worker(*store, load().migration);
        std::promise<absl::Status> result;
        WorkerPool pool(1, 1);
        if(!pool.submit([&] { result.set_value(worker.pass()); }))
            throw std::runtime_error("submit failed");
        auto future = result.get_future();
        if(future.wait_for(std::chrono::seconds(20)) != std::future_status::ready)
            throw std::runtime_error("worker pass deadline");
        return future.get();
    }
    void intact()
    {
        auto records = store->manifest(1);
        ASSERT_TRUE(records.ok());
        for(const auto& item: *records) EXPECT_NE(item.state, ManifestState::Lost);
        auto location = store->location(record.file);
        ASSERT_TRUE(location.ok());
        if(*location)
        {
            EXPECT_TRUE(fs::exists(slow / record.file));
        }
    }
    void readable()
    {
        auto read = store->read(1, {Range::Axis::Hlc, {100, 0}, {200, 0}});
        ASSERT_TRUE(read.ok()) << read.status();
        ASSERT_EQ(read->size(), events.size());
        EXPECT_EQ(read->front().id, events.front().id);
        EXPECT_EQ(read->front().hlc, events.front().hlc);
        EXPECT_EQ(read->front().envelope.payload, events.front().envelope.payload);
    }
    void unavailable()
    {
        EXPECT_FALSE(store->probeTiers().ok());
        auto read = store->read(1, {Range::Axis::Hlc, {100, 0}, {200, 0}});
        EXPECT_FALSE(read.ok());
        EXPECT_TRUE(absl::IsUnavailable(read.status())) << read.status();
        auto records = store->manifest(1);
        ASSERT_TRUE(records.ok());
        for(const auto& item: *records) EXPECT_NE(item.state, ManifestState::Lost);
    }
};

TEST(GrapherTiers, WorkerMigratesAndReadsIdenticalEvents)
{
    Archive archive;
    ASSERT_TRUE(archive.pass().ok());
    auto location = archive.store->location(archive.record.file);
    ASSERT_TRUE(location.ok());
    ASSERT_TRUE(location->has_value());
    EXPECT_EQ((*location)->tier, "slow");
    EXPECT_FALSE(fs::exists(archive.local / archive.record.file));
    archive.readable();
    archive.intact();
    std::ifstream input(archive.root / "status.json");
    Json status;
    input >> status;
    EXPECT_EQ(status["writer"], "writer");
    EXPECT_EQ(status["tiers"][1]["available"], true);
    EXPECT_GT(status["tiers"][1]["used_bytes"].get<uint64_t>(), 0u);
}

// Cleanup, sweeps, the manifest replica and the status file follow events (a start, a migration, a failed attempt, a
// tier's return), never the probe interval: a pass with nothing to do rewrites no file on any tier.
TEST(GrapherTiers, AnIdlePassRewritesNeitherTheReplicaNorTheStatusFile)
{
    Archive archive;
    MigrationWorker worker(*archive.store, archive.load().migration);
    WorkerPool pool(1, 1);
    const auto pass = [&]
    {
        std::promise<absl::Status> result;
        auto future = result.get_future();
        if(!pool.submit([&] { result.set_value(worker.pass()); }))
            return absl::Status(absl::UnavailableError("submit failed"));
        return future.get();
    };
    ASSERT_TRUE(pass().ok());
    ASSERT_TRUE(archive.store->location(archive.record.file).value().has_value());
    const std::vector<fs::path> files{archive.slow / "manifest-replica" / "writer.snap",
                                      archive.local / "manifest" / "writer.snap",
                                      archive.root / "status.json"};
    const auto identities = [&]
    {
        std::vector<std::tuple<ino_t, time_t, long>> result;
        for(const auto& file: files)
        {
            struct stat info
            {
            };
            EXPECT_EQ(::stat(file.c_str(), &info), 0) << file;
            result.emplace_back(info.st_ino, info.st_mtim.tv_sec, info.st_mtim.tv_nsec);
        }
        return result;
    };
    const auto written = identities();
    ASSERT_TRUE(pass().ok());
    ASSERT_TRUE(pass().ok());
    EXPECT_EQ(identities(), written);
    archive.readable();
    archive.intact();
}

TEST(GrapherTiers, UnavailableDestinationKeepsTheLocalFile)
{
    Archive archive;
    fs::rename(archive.slow, archive.slow.string() + ".away");
    ASSERT_TRUE(archive.pass().ok());
    auto location = archive.store->location(archive.record.file);
    ASSERT_TRUE(location.ok());
    EXPECT_FALSE(location->has_value());
    archive.readable();
    fs::rename(archive.slow.string() + ".away", archive.slow);
    archive.intact();
}

TEST(GrapherTierFaults, Permissions)
{
    Archive archive;
    ASSERT_TRUE(archive.pass().ok());
    ASSERT_EQ(::chmod(archive.slow.c_str(), 0000), 0);
    archive.unavailable();
    EXPECT_FALSE(archive.store->migrateOnce("slow").ok());
    ASSERT_EQ(::chmod(archive.slow.c_str(), 0700), 0);
    ASSERT_TRUE(archive.store->probeTiers().ok());
    archive.intact();
    archive.readable();
}

TEST(GrapherTierFaults, RootRenamed)
{
    Archive archive;
    ASSERT_TRUE(archive.pass().ok());
    fs::rename(archive.slow, archive.slow.string() + ".away");
    archive.unavailable();
    EXPECT_FALSE(archive.store->migrateOnce("slow").ok());
    EXPECT_FALSE(fs::exists(archive.slow));
    fs::rename(archive.slow.string() + ".away", archive.slow);
    ASSERT_TRUE(archive.store->probeTiers().ok());
    archive.intact();
    archive.readable();
}

TEST(GrapherTierFaults, ReaddedUuidRefused)
{
    Archive archive;
    ASSERT_TRUE(archive.pass().ok());
    fs::rename(archive.slow, archive.slow.string() + ".away");
    fs::create_directory(archive.slow);
    auto marker = archive.configuration["tiers"][1];
    marker["deployment_id"] = "tier2g";
    marker["tier_uuid"] = "33333333333333333333333333333333";
    std::ofstream(archive.slow / ".chronolog-tier.json") << marker.dump();
    archive.unavailable();
    EXPECT_FALSE(archive.store->migrateOnce("slow").ok());
    EXPECT_EQ(std::distance(fs::directory_iterator(archive.slow), fs::directory_iterator{}), 1);
    fs::remove_all(archive.slow);
    fs::rename(archive.slow.string() + ".away", archive.slow);
    ASSERT_TRUE(archive.store->probeTiers().ok());
    archive.intact();
    archive.readable();
}

TEST(GrapherTierFaults, ExecutorHang)
{
    auto entered = std::make_shared<std::promise<void>>();
    auto released = std::make_shared<std::promise<void>>();
    auto done = std::make_shared<std::promise<void>>();
    auto release = released->get_future().share();
    auto completion = done->get_future();
    auto armed = std::make_shared<std::atomic<bool>>(false);
    FileTierStore::Hooks hooks;
    hooks.tier_step = [entered, release, done, armed](std::string_view step)
    {
        if(step == "usage" && armed->exchange(false))
        {
            entered->set_value();
            release.wait();
            done->set_value();
        }
        return absl::OkStatus();
    };
    Archive archive(hooks);
    ASSERT_TRUE(archive.pass().ok());
    armed->store(true);
    auto usage = std::async(std::launch::async, [&] { return archive.store->tierUsage("slow"); });
    ASSERT_EQ(entered->get_future().wait_for(std::chrono::seconds(5)), std::future_status::ready);
    auto result = usage.get();
    ASSERT_TRUE(result.ok());
    EXPECT_FALSE(result->available);
    auto read = archive.store->read(1, {Range::Axis::Hlc, {100, 0}, {200, 0}});
    EXPECT_TRUE(absl::IsUnavailable(read.status())) << read.status();
    released->set_value();
    ASSERT_EQ(completion.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    ASSERT_TRUE(archive.store->probeTiers().ok());
    archive.intact();
    archive.readable();
}

TEST(GrapherTiers, DestroyRequeuesUnavailableTierAndCompletesAfterRecovery)
{
    auto durable = std::make_shared<std::promise<void>>();
    auto second_durable = std::make_shared<std::promise<void>>();
    auto deleted_signaled = std::make_shared<std::atomic<bool>>(false);
    auto second_signaled = std::make_shared<std::atomic<bool>>(false);
    FileTierStore::Hooks hooks;
    hooks.manifest_sync = [durable, second_durable, deleted_signaled, second_signaled](int fd)
    {
        const auto result = ::fsync(fd);
        char bytes[65536];
        const auto size = ::pread(fd, bytes, sizeof(bytes), 0);
        const std::string_view contents(bytes, size > 0 ? static_cast<size_t>(size) : 0);
        if(result == 0 && contents.find("\"state\":2") != std::string_view::npos && !deleted_signaled->exchange(true))
            durable->set_value();
        if(result == 0 && contents.find("{\"story\":2,\"tombstoned\":true}") != std::string_view::npos &&
           !second_signaled->exchange(true))
            second_durable->set_value();
        return result;
    };
    Archive archive(hooks);
    ASSERT_TRUE(archive.pass().ok());
    fs::rename(archive.slow, archive.slow.string() + ".away");
    EXPECT_FALSE(archive.store->probeTiers().ok());
    ArchiveService service(*archive.store, "destroy-test");
    service.tombstone(1);
    ASSERT_EQ(durable->get_future().wait_for(std::chrono::seconds(5)), std::future_status::ready);
    EXPECT_FALSE(service.waitDestroyed(1, std::chrono::milliseconds(1)));
    // Synchronize with the destroy worker through its durable Deleted record, not a sleep.
    auto deleted = archive.store->manifest(1);
    ASSERT_TRUE(deleted.ok());
    bool has_deleted = false;
    for(const auto& record: *deleted) has_deleted |= record.state == ManifestState::Deleted;
    EXPECT_TRUE(has_deleted);
    EXPECT_TRUE(*archive.store->hasPendingUnlinks(1));
    ASSERT_TRUE(archive.store->registerStory(2, Hlc{100, 0}).ok());
    service.tombstone(2);
    ASSERT_EQ(second_durable->get_future().wait_for(std::chrono::seconds(5)), std::future_status::ready);
    EXPECT_TRUE(*archive.store->tombstoned(2));
    fs::rename(archive.slow.string() + ".away", archive.slow);
    ASSERT_TRUE(archive.store->probeTiers().ok());
    EXPECT_TRUE(service.waitDestroyed(1, std::chrono::seconds(5)));
    EXPECT_FALSE(*archive.store->hasPendingUnlinks(1));
    for(const auto& path: {archive.local / "1", archive.slow / "1"})
        if(fs::exists(path))
        {
            EXPECT_TRUE(fs::is_empty(path));
        }
}

TEST(GrapherTierInterleavings, MigrationThroughAReplacedRootNeverCommits)
{
    auto slow = std::make_shared<fs::path>();
    auto replaced = std::make_shared<std::atomic<bool>>(false);
    FileTierStore::Hooks hooks;
    hooks.migration_step = [slow, replaced](int step)
    {
        if(step == 2 && !replaced->exchange(true))
        {
            fs::rename(*slow, slow->string() + ".away");
            fs::create_directory(*slow);
        }
        return absl::OkStatus();
    };
    Archive archive(hooks);
    *slow = archive.slow;
    ASSERT_TRUE(archive.store->probeTiers().ok());
    EXPECT_FALSE(archive.store->migrateOnce("slow").ok());
    ASSERT_TRUE(replaced->load());
    EXPECT_TRUE(fs::is_empty(archive.slow));
    EXPECT_TRUE(fs::exists(archive.local / archive.record.file));
    auto location = archive.store->location(archive.record.file);
    ASSERT_TRUE(location.ok());
    EXPECT_FALSE(location->has_value());
    archive.readable();
    fs::remove(archive.slow);
    fs::rename(archive.slow.string() + ".away", archive.slow);
    ASSERT_TRUE(archive.pass().ok());
    archive.intact();
    archive.readable();
}

TEST(GrapherTierInterleavings, EraseThroughAReplacedRootStaysPending)
{
    auto slow = std::make_shared<fs::path>();
    auto replaced = std::make_shared<std::atomic<bool>>(false);
    FileTierStore::Hooks hooks;
    hooks.tier_step = [slow, replaced](std::string_view step)
    {
        if(step == "erase" && !replaced->exchange(true))
        {
            fs::rename(*slow, slow->string() + ".away");
            fs::create_directory(*slow);
        }
        return absl::OkStatus();
    };
    Archive archive(hooks);
    *slow = archive.slow;
    ASSERT_TRUE(archive.pass().ok());
    ASSERT_TRUE(archive.store->tombstone(1).ok());
    EXPECT_FALSE(archive.store->eraseFile(archive.record.file).ok());
    (void)archive.store->awaitTierUnlinksForTesting(std::chrono::seconds(5));
    ASSERT_TRUE(replaced->load());
    EXPECT_TRUE(fs::is_empty(archive.slow));
    EXPECT_TRUE(fs::exists(fs::path(archive.slow.string() + ".away") / archive.record.file));
    EXPECT_TRUE(*archive.store->hasPendingUnlinks(1));
    auto records = archive.store->manifest(1);
    ASSERT_TRUE(records.ok());
    bool deleted = false;
    for(const auto& record: *records)
    {
        EXPECT_NE(record.state, ManifestState::Lost);
        deleted |= record.state == ManifestState::Deleted;
    }
    EXPECT_TRUE(deleted);
    ArchiveService service(*archive.store, "replaced-root-destroy");
    service.tombstone(1);
    EXPECT_FALSE(service.waitDestroyed(1, std::chrono::milliseconds(1)));
    fs::remove(archive.slow);
    fs::rename(archive.slow.string() + ".away", archive.slow);
    ASSERT_TRUE(archive.store->probeTiers().ok());
    EXPECT_TRUE(service.waitDestroyed(1, std::chrono::seconds(5)));
    EXPECT_FALSE(*archive.store->hasPendingUnlinks(1));
    for(const auto& path: {archive.local / "1", archive.slow / "1"})
        if(fs::exists(path))
        {
            EXPECT_TRUE(fs::is_empty(path));
        }
}

TEST(GrapherTierConfig, ShapeValidationAndLegacyDefaults)
{
    Archive archive;
    const auto valid = archive.configuration;
    auto expect_bad = [&](Json config)
    {
        archive.configuration = std::move(config);
        std::ofstream(archive.root / "bad.json") << archive.configuration.dump();
        EXPECT_FALSE(GrapherConfig::load((archive.root / "bad.json").string()).ok());
    };
    ASSERT_TRUE(archive.load().validate().ok());
    auto bad = valid;
    bad["tiers"][1]["unknown"] = 1;
    expect_bad(bad);
    bad = valid;
    bad["tiers"][1]["kind"] = "s3";
    expect_bad(bad);
    bad = valid;
    bad["tiers"][1]["name"] = "local";
    expect_bad(bad);
    bad = valid;
    bad["tiers"][1]["rank"] = 0;
    expect_bad(bad);
    bad = valid;
    bad.erase("deployment_id");
    expect_bad(bad);
    for(auto age: {359, 360})
    {
        bad = valid;
        bad["migrate_after_s"] = age;
        expect_bad(bad);
    }
    bad = valid;
    bad["tier_status_file"] = (archive.slow / "status").string();
    expect_bad(bad);
    auto legacy = GrapherConfig::load(std::nullopt);
    ASSERT_TRUE(legacy.ok());
    EXPECT_TRUE(legacy->migration.tiers.empty());
    EXPECT_TRUE(legacy->tierChain().deployment_id.empty());
    EXPECT_FALSE(legacy->migration.enabled);
    EXPECT_EQ(legacy->archive_root, "./archive");
}
} // namespace
} // namespace chronolog::grapher
