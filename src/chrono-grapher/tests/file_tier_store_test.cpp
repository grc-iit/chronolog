#include "../../../tests/contract/tier_store_contract_test.cpp"
#include "chrono-grapher/tier/FileTierStore.h"
#include <atomic>
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace chronolog
{
namespace
{
namespace fs = std::filesystem;

std::shared_ptr<fs::path> TestDirectory()
{
    const auto* test = ::testing::UnitTest::GetInstance()->current_test_info();
    auto name = std::string(test->test_suite_name()) + "_" + test->name();
    std::replace(name.begin(), name.end(), '/', '_');
    auto directory = std::shared_ptr<fs::path>(
            new fs::path(fs::temp_directory_path() / ("chronolog_tier_" + std::to_string(::getpid()) + "_" + name)),
            [](fs::path* path)
            {
                fs::remove_all(*path);
                delete path;
            });
    fs::remove_all(*directory);
    fs::create_directories(*directory);
    return directory;
}

class FailingCodec final: public ChunkCodec
{
public:
    mutable std::atomic<bool> fail{false};
    absl::Status write(const fs::path& path, std::span<const Event> events) const override
    {
        if(fail.exchange(false))
            return absl::UnavailableError("injected chunk write failure");
        return proto_.write(path, events);
    }
    absl::StatusOr<std::vector<Event>> read(const fs::path& path) const override { return proto_.read(path); }

private:
    ProtoChunkCodec proto_;
};

std::unique_ptr<contract::TierStoreHarness> MakeFileStore()
{
    auto directory = TestDirectory();
    auto codec = std::make_shared<FailingCodec>();
    auto harness = std::make_unique<contract::TierStoreHarness>();
    auto opened = FileTierStore::Open(*directory, "primary", {{1, {100, 0}}}, codec);
    EXPECT_TRUE(opened.ok()) << opened.status();
    if(!opened.ok())
        return harness;
    harness->sut = *std::move(opened);
    harness->restart = [directory, codec, h = harness.get()]
    {
        h->sut.reset();
        auto reopened = FileTierStore::Open(*directory, "primary", {{1, {100, 0}}}, codec);
        ASSERT_TRUE(reopened.ok()) << reopened.status();
        h->sut = *std::move(reopened);
    };
    harness->failNextPublish = [codec] { codec->fail = true; };
    harness->injectTornRecord = [directory]
    {
        std::ofstream log(*directory / "manifest/primary.log", std::ios::app);
        log << "{\"story\":1,\"chunk\":\"torn";
    };
    harness->eraseFile = [h = harness.get()](std::string file)
    { ASSERT_TRUE(static_cast<FileTierStore*>(h->sut.get())->eraseFile(file).ok()); };
    harness->writerLogs = [directory]
    {
        std::vector<std::string> logs;
        for(const auto& file: fs::directory_iterator(*directory / "manifest"))
            if(file.path().extension() == ".log")
                logs.push_back(file.path().string());
        return logs;
    };
    harness->publishOtherWriter = [directory](Chunk chunk) -> absl::StatusOr<ManifestRecord>
    {
        auto other = FileTierStore::Open(*directory, "secondary", {{1, {100, 0}}});
        if(!other.ok())
            return other.status();
        return (*other)->publish(std::move(chunk));
    };
    return harness;
}

absl::StatusOr<std::unique_ptr<FileTierStore>> Open(const fs::path& root, const std::string& writer = "primary")
{
    return FileTierStore::Open(root, writer, {{1, {100, 0}}});
}

ManifestRecord Record(int64_t start, int64_t end, ManifestState state = ManifestState::Published)
{
    return {std::to_string(start),
            "primary",
            "1/" + std::to_string(start) + ".pb",
            1,
            {start, 0},
            {end, 0},
            state == ManifestState::Empty ? 0u : 1u,
            state,
            false};
}

std::string Bytes(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

TEST(FileTierStore, WatermarkStopsAtTheFirstGap)
{
    auto directory = TestDirectory();
    auto store = Open(*directory);
    ASSERT_TRUE(store.ok());
    ASSERT_TRUE((*store)->publish(contract::Window(300, 400)).ok());
    ASSERT_TRUE((*store)->publish(contract::Window()).ok());
    EXPECT_EQ((*store)->contiguousWatermark(1).value(), (Hlc{200, 0}));
    ASSERT_TRUE((*store)->publish(contract::Window(200, 300)).ok());
    EXPECT_EQ((*store)->contiguousWatermark(1).value(), (Hlc{400, 0}));
}

TEST(FileTierStore, AnEmptyWindowKeepsTheContiguousRunIntact)
{
    auto directory = TestDirectory();
    auto store = Open(*directory);
    ASSERT_TRUE(store.ok());
    ASSERT_TRUE((*store)->publish(contract::Window()).ok());
    ASSERT_TRUE((*store)->publish(contract::Window(200, 300, true)).ok());
    ASSERT_TRUE((*store)->publish(contract::Window(300, 400)).ok());
    EXPECT_EQ((*store)->contiguousWatermark(1).value(), (Hlc{400, 0}));
}

TEST(FileTierStore, ExemptRecordsDoNotAdvanceTheWatermark)
{
    auto directory = TestDirectory();
    auto store = Open(*directory);
    ASSERT_TRUE(store.ok());
    ASSERT_TRUE((*store)->publish(contract::Window()).ok());
    auto salvage = contract::Window(200, 300);
    salvage.exempt = true;
    ASSERT_TRUE((*store)->publish(salvage).ok());
    ASSERT_TRUE((*store)->publish(contract::Window(300, 400)).ok());
    EXPECT_EQ((*store)->contiguousWatermark(1).value(), (Hlc{200, 0}));
    EXPECT_EQ((*store)->read(1, contract::WholeArchive())->size(), 3u);
}

TEST(ManifestLog, EachWriterAppendsToItsOwnLog)
{
    auto directory = TestDirectory();
    auto first = ManifestLog::Open(*directory, "first");
    auto second = ManifestLog::Open(*directory, "second");
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());
    ASSERT_TRUE((*first)->append(Record(100, 200)).ok());
    const auto original = Bytes((*first)->logPath());
    ASSERT_TRUE((*second)->append(Record(200, 300)).ok());
    EXPECT_NE((*first)->logPath(), (*second)->logPath());
    EXPECT_EQ(Bytes((*first)->logPath()), original);
    EXPECT_EQ((*first)->load()->records.size(), 2u);
    EXPECT_FALSE(ManifestLog::Open(*directory, "first").ok());
}

TEST(ManifestLog, ATornFinalLineIsDiscardedAndEarlierRecordsSurvive)
{
    auto directory = TestDirectory();
    auto log = ManifestLog::Open(*directory, "primary");
    ASSERT_TRUE(log.ok());
    ASSERT_TRUE((*log)->append(Record(100, 200)).ok());
    {
        std::ofstream torn((*log)->logPath(), std::ios::app);
        torn << "{\"torn\":";
    }
    EXPECT_EQ((*log)->load()->records.size(), 1u);
    log->reset();
    log = ManifestLog::Open(*directory, "primary");
    ASSERT_TRUE(log.ok());
    ASSERT_TRUE((*log)->append(Record(200, 300)).ok());
    EXPECT_EQ((*log)->load()->records.size(), 2u);
}

TEST(ManifestLog, CompactionLeavesOtherWritersLogsAlone)
{
    auto directory = TestDirectory();
    auto first = ManifestLog::Open(*directory, "primary");
    auto second = ManifestLog::Open(*directory, "secondary");
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());
    ASSERT_TRUE((*first)->append(Record(100, 200)).ok());
    ASSERT_TRUE((*second)->append(Record(200, 300)).ok());
    const auto other = Bytes((*second)->logPath());
    ASSERT_TRUE((*first)->compact().ok());
    EXPECT_EQ(fs::file_size((*first)->logPath()), 0u);
    EXPECT_EQ(Bytes((*second)->logPath()), other);
    EXPECT_EQ((*first)->load()->records.size(), 2u);
    ASSERT_TRUE((*first)->compact().ok());
    EXPECT_EQ((*first)->load()->records.size(), 2u);
}

TEST(FileTierStore, PublishNeverOverwrites)
{
    auto directory = TestDirectory();
    auto store = Open(*directory);
    ASSERT_TRUE(store.ok());
    auto record = (*store)->publish(contract::Window());
    ASSERT_TRUE(record.ok());
    const auto original = Bytes(*directory / record->file);
    auto changed = contract::Window();
    changed.events[0].envelope.payload = "different";
    EXPECT_FALSE((*store)->publish(changed).ok());
    EXPECT_EQ(Bytes(*directory / record->file), original);
    fs::resize_file(*directory / "manifest/primary.log", 0);
    EXPECT_FALSE((*store)->publish(contract::Window()).ok());
    EXPECT_EQ(Bytes(*directory / record->file), original);
}

TEST(FileTierStore, CrashBetweenPublishAndRecordAdoptsTheFile)
{
    auto directory = TestDirectory();
    auto store = Open(*directory);
    ASSERT_TRUE(store.ok());
    auto record = (*store)->publish(contract::Window());
    ASSERT_TRUE(record.ok());
    fs::resize_file(*directory / "manifest/primary.log", 0);
    EXPECT_TRUE((*store)->read(1, contract::WholeArchive())->empty());
    auto live_reader = Open(*directory, "secondary");
    ASSERT_TRUE(live_reader.ok());
    EXPECT_TRUE((*live_reader)->read(1, contract::WholeArchive())->empty());
    live_reader->reset();
    store->reset();
    store = Open(*directory);
    ASSERT_TRUE(store.ok()) << store.status();
    EXPECT_EQ((*store)->read(1, contract::WholeArchive())->size(), 1u);
    EXPECT_EQ((*store)->contiguousWatermark(1).value(), (Hlc{200, 0}));
    EXPECT_EQ((*store)->manifest(1)->size(), 1u);
}

TEST(FileTierStore, RecoveryMarksMissingAndCorruptWindowsLostWithoutLoweringWatermark)
{
    auto directory = TestDirectory();
    auto store = Open(*directory);
    ASSERT_TRUE(store.ok());
    auto first = (*store)->publish(contract::Window());
    auto second = (*store)->publish(contract::Window(200, 300));
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());
    fs::remove(*directory / first->file);
    {
        std::ofstream corrupt(*directory / second->file, std::ios::binary);
        corrupt << '\x80';
    }
    store->reset();
    store = Open(*directory);
    ASSERT_TRUE(store.ok());
    EXPECT_EQ((*store)->contiguousWatermark(1).value(), (Hlc{300, 0}));
    EXPECT_TRUE((*store)->incomplete(1, contract::WholeArchive()).value());
    EXPECT_FALSE((*store)->incomplete(1, {Range::Axis::Hlc, {300, 0}, {400, 0}}).value());
    auto records = (*store)->manifest(1);
    ASSERT_TRUE(records.ok());
    ASSERT_EQ(records->size(), 2u);
    for(const auto& record: *records) EXPECT_EQ(record.state, ManifestState::Lost);
    EXPECT_TRUE((*store)->read(1, contract::WholeArchive())->empty());
    ASSERT_TRUE((*store)->compact().ok());
    store->reset();
    store = Open(*directory);
    ASSERT_TRUE(store.ok());
    EXPECT_EQ((*store)->contiguousWatermark(1).value(), (Hlc{300, 0}));
    EXPECT_TRUE((*store)->incomplete(1, contract::WholeArchive()).value());
}

TEST(FileTierStore, LostWindowAboveGapNeverAdvancesTheWatermark)
{
    auto directory = TestDirectory();
    auto store = Open(*directory);
    ASSERT_TRUE(store.ok());
    ASSERT_TRUE((*store)->publish(contract::Window()).ok());
    auto upper = (*store)->publish(contract::Window(300, 400));
    ASSERT_TRUE(upper.ok());
    fs::remove(*directory / upper->file);
    store->reset();
    store = Open(*directory);
    ASSERT_TRUE(store.ok());
    ASSERT_TRUE((*store)->publish(contract::Window(200, 300)).ok());
    EXPECT_EQ((*store)->contiguousWatermark(1).value(), (Hlc{300, 0}));
    store->reset();
    store = Open(*directory);
    ASSERT_TRUE(store.ok());
    EXPECT_EQ((*store)->contiguousWatermark(1).value(), (Hlc{300, 0}));
}

TEST(FileTierStore, RetentionDeletionPreservesWatermarkAcrossRestart)
{
    auto directory = TestDirectory();
    auto store = Open(*directory);
    ASSERT_TRUE(store.ok());
    auto record = (*store)->publish(contract::Window());
    ASSERT_TRUE(record.ok());
    ASSERT_TRUE((*store)->eraseFile(record->file).ok());
    EXPECT_EQ((*store)->contiguousWatermark(1).value(), (Hlc{200, 0}));
    EXPECT_TRUE((*store)->read(1, contract::WholeArchive())->empty());
    EXPECT_FALSE((*store)->incomplete(1, contract::WholeArchive()).value());
    store->reset();
    store = Open(*directory);
    ASSERT_TRUE(store.ok());
    EXPECT_EQ((*store)->contiguousWatermark(1).value(), (Hlc{200, 0}));
    EXPECT_EQ((*store)->manifest(1)->at(0).state, ManifestState::Deleted);
}

TEST(FileTierStore, ReadersMergeNewWriterLogsAndDeduplicateInReplayOrder)
{
    auto directory = TestDirectory();
    auto first = Open(*directory);
    auto second = Open(*directory, "secondary");
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());
    auto chunk = contract::Window();
    auto earlier = chunk.events[0];
    earlier.id.sequence = 99;
    earlier.id.writer_id = 1;
    earlier.envelope.content_type = "application/octet-stream";
    earlier.envelope.payload = std::string("a\0b", 3);
    earlier.envelope.trace_id = std::string(16, 't');
    earlier.envelope.span_id = std::string(8, 's');
    earlier.envelope.attributes = {{"key", "value"}};
    earlier.physical = {9999, 7, ClockStatus::Synced};
    earlier.durability = Durability::Durable;
    chunk.events.push_back(earlier);
    ASSERT_TRUE((*second)->publish(chunk).ok());
    ASSERT_TRUE((*first)->publish(contract::Window()).ok());
    auto events = (*first)->read(1, contract::WholeArchive());
    ASSERT_TRUE(events.ok());
    ASSERT_EQ(events->size(), 2u);
    EXPECT_EQ(events->at(0).id, earlier.id);
    EXPECT_EQ(events->at(0).physical.physical_ns, 9999);
    EXPECT_EQ(events->at(0).physical.uncertainty_ns, 7u);
    EXPECT_EQ(events->at(0).physical.status, ClockStatus::Synced);
    EXPECT_EQ(events->at(0).durability, Durability::Durable);
    EXPECT_EQ(events->at(0).envelope.content_type, earlier.envelope.content_type);
    EXPECT_EQ(events->at(0).envelope.payload, earlier.envelope.payload);
    EXPECT_EQ(events->at(0).envelope.trace_id, earlier.envelope.trace_id);
    EXPECT_EQ(events->at(0).envelope.span_id, earlier.envelope.span_id);
    EXPECT_EQ(events->at(0).envelope.attributes, earlier.envelope.attributes);
    EXPECT_EQ(events->at(1).id.writer_id, 2u);
    EXPECT_EQ((*first)->read(1, {Range::Axis::Physical, {9999, 0}, {10000, 0}})->size(), 1u);
    EXPECT_TRUE((*first)->read(1, {Range::Axis::Physical, {9998, 0}, {9999, 0}})->empty());
}

TEST(FileTierStore, InvalidInputsAndUnknownStoriesAreRejected)
{
    auto directory = TestDirectory();
    auto store = Open(*directory);
    ASSERT_TRUE(store.ok());
    auto chunk = contract::Window();
    chunk.story_id = 2;
    chunk.events[0].id.story_id = 2;
    EXPECT_EQ((*store)->publish(chunk).status().code(), absl::StatusCode::kNotFound);
    EXPECT_EQ((*store)->manifest(2).status().code(), absl::StatusCode::kNotFound);
    chunk = contract::Window();
    chunk.events[0].hlc = chunk.end;
    EXPECT_EQ((*store)->publish(chunk).status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_EQ((*store)->read(1, {Range::Axis::Hlc, {200, 0}, {100, 0}}).status().code(),
              absl::StatusCode::kInvalidArgument);
    EXPECT_EQ((*store)->read(1, {static_cast<Range::Axis>(99), {100, 0}, {200, 0}}).status().code(),
              absl::StatusCode::kInvalidArgument);
}
} // namespace

namespace contract
{
INSTANTIATE_TEST_SUITE_P(File,
                         TierStoreContract,
                         ::testing::Values(TierStoreFactory(MakeFileStore)),
                         [](const auto&) { return "FileTier"; });
} // namespace contract
} // namespace chronolog
