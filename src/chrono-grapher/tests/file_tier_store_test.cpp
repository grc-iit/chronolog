#include "../../../tests/contract/tier_store_contract_test.cpp"
#include "chrono-grapher/tier/FileTierStore.h"
#include <atomic>
#include <filesystem>
#include <fstream>
#include <set>
#include <thread>
#include <fcntl.h>
#include <sys/file.h>
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
    explicit FailingCodec(std::shared_ptr<const ChunkCodec> codec)
        : codec_(std::move(codec))
    {}
    mutable std::atomic<bool> fail{false};
    std::string extension() const override { return codec_->extension(); }
    absl::Status writeChunk(const fs::path& path, const Chunk& chunk) const override
    {
        if(fail.exchange(false))
            return absl::UnavailableError("injected chunk write failure");
        return codec_->writeChunk(path, chunk);
    }
    absl::Status write(const fs::path& path, std::span<const Event> events) const override
    {
        if(fail.exchange(false))
            return absl::UnavailableError("injected chunk write failure");
        return codec_->write(path, events);
    }
    absl::StatusOr<std::vector<Event>> read(const fs::path& path) const override { return codec_->read(path); }

private:
    std::shared_ptr<const ChunkCodec> codec_;
};

std::unique_ptr<contract::TierStoreHarness> MakeFileStore(std::shared_ptr<const ChunkCodec> implementation)
{
    auto directory = TestDirectory();
    auto codec = std::make_shared<FailingCodec>(implementation);
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
    harness->publishOtherWriter = [directory, codec](Chunk chunk) -> absl::StatusOr<ManifestRecord>
    {
        auto other = FileTierStore::Open(*directory, "secondary", {{1, {100, 0}}}, codec);
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

TEST(FileTierStore, ReRegisteringAStoryKeepsItsWatermarkAndRefusesAMovedAnchor)
{
    auto directory = TestDirectory();
    auto store = Open(*directory);
    ASSERT_TRUE(store.ok());
    ASSERT_TRUE((*store)->publish(contract::Window()).ok());
    ASSERT_TRUE((*store)->publish(contract::Window(200, 300)).ok());
    EXPECT_TRUE((*store)->registerStory(1, Hlc{100, 0}).ok());
    EXPECT_EQ((*store)->contiguousWatermark(1).value(), (Hlc{300, 0}));
    EXPECT_EQ((*store)->registerStory(1, Hlc{500, 0}).code(), absl::StatusCode::kFailedPrecondition);
    EXPECT_EQ((*store)->contiguousWatermark(1).value(), (Hlc{300, 0}));
}

TEST(FileTierStore, AWindowStartingBelowTheAnchorOrOverlappingTheRunExtendsIt)
{
    auto directory = TestDirectory();
    auto store = Open(*directory);
    ASSERT_TRUE(store.ok());
    ASSERT_TRUE((*store)->publish(contract::Window(50, 150)).ok());
    EXPECT_EQ((*store)->contiguousWatermark(1).value(), (Hlc{150, 0}));
    ASSERT_TRUE((*store)->publish(contract::Window(120, 250)).ok());
    EXPECT_EQ((*store)->contiguousWatermark(1).value(), (Hlc{250, 0}));
}

TEST(FileTierStore, AnEmptyExemptWindowDoesNotAdvanceTheWatermark)
{
    auto directory = TestDirectory();
    auto store = Open(*directory);
    ASSERT_TRUE(store.ok());
    ASSERT_TRUE((*store)->publish(contract::Window()).ok());
    auto salvage = contract::Window(200, 300, true);
    salvage.exempt = true;
    ASSERT_TRUE((*store)->publish(salvage).ok());
    EXPECT_EQ((*store)->contiguousWatermark(1).value(), (Hlc{200, 0}));
    ASSERT_TRUE((*store)->publish(contract::Window(300, 400)).ok());
    EXPECT_EQ((*store)->contiguousWatermark(1).value(), (Hlc{200, 0}));
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
    EXPECT_EQ((*first)->read(1, {Range::Axis::Physical, {9998, 0}, {9999, 0}})->size(), 1u);
    EXPECT_TRUE((*first)->read(1, {Range::Axis::Physical, {9991, 0}, {9992, 0}})->empty());
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
TEST(ManifestLog, EveryRecordStateRoundTripsAsOneJsonLineInAppendOrder)
{
    auto directory = TestDirectory();
    auto log = ManifestLog::Open(*directory, "primary");
    ASSERT_TRUE(log.ok());
    std::vector<ManifestRecord> written;
    int64_t start = 100;
    for(auto state: {ManifestState::Published,
                     ManifestState::Empty,
                     ManifestState::Deleted,
                     ManifestState::Failed,
                     ManifestState::Lost})
    {
        auto record = Record(start, start + 100, state);
        record.exempt = state == ManifestState::Failed;
        record.physical_policy = state == ManifestState::Lost;
        ASSERT_TRUE((*log)->append(record).ok());
        written.push_back(record);
        start += 100;
    }
    std::ifstream input((*log)->logPath());
    std::string line;
    size_t lines = 0;
    while(std::getline(input, line))
    {
        ++lines;
        EXPECT_FALSE(line.empty());
    }
    EXPECT_EQ(lines, written.size());
    auto index = (*log)->load();
    ASSERT_TRUE(index.ok());
    ASSERT_EQ(index->records.size(), written.size());
    for(size_t i = 0; i < written.size(); ++i)
    {
        const auto& actual = index->records[i];
        EXPECT_EQ(actual.chunk_id, written[i].chunk_id);
        EXPECT_EQ(actual.manifest_writer, "primary");
        EXPECT_EQ(actual.file, written[i].file);
        EXPECT_EQ(actual.story_id, written[i].story_id);
        EXPECT_EQ(actual.start, written[i].start);
        EXPECT_EQ(actual.end, written[i].end);
        EXPECT_EQ(actual.event_count, written[i].event_count);
        EXPECT_EQ(actual.state, written[i].state);
        EXPECT_EQ(actual.exempt, written[i].exempt);
        EXPECT_EQ(actual.physical_policy, written[i].physical_policy);
    }
}

TEST(ManifestLog, AMalformedCompleteLineFailsTheWholeLoad)
{
    for(const char* bad:
        {"not json",
         R"({"story":1})",
         R"({"chunk":"x","writer":"primary","file":"1/x.pb","story":1,"start":[5,0],"end":[5,0],"count":1,"state":0,"exempt":false})",
         R"({"chunk":"x","writer":"primary","file":"1/x.pb","story":1,"start":[5,0],"end":[9,0],"count":1,"state":9,"exempt":false})"})
    {
        auto directory = TestDirectory();
        auto log = ManifestLog::Open(*directory, "primary");
        ASSERT_TRUE(log.ok());
        ASSERT_TRUE((*log)->append(Record(100, 200)).ok());
        {
            std::ofstream out((*log)->logPath(), std::ios::app);
            out << bad << '\n';
        }
        EXPECT_FALSE((*log)->load().ok()) << bad;
    }
}

TEST(ManifestLog, AnAbsentManifestLoadsAsEmpty)
{
    auto directory = TestDirectory();
    auto log = ManifestLog::OpenReadOnly(*directory / "never-created");
    auto index = log->load();
    ASSERT_TRUE(index.ok()) << index.status();
    EXPECT_TRUE(index->records.empty());
    EXPECT_TRUE(index->watermarks.empty());
    EXPECT_TRUE(FileTierStore::OpenReadOnly(*directory / "never-created").ok());
}

TEST(ManifestLog, CompactionKeepsEveryRecordAndLaterAppendsLoadOnTopOfIt)
{
    auto directory = TestDirectory();
    auto log = ManifestLog::Open(*directory, "primary");
    ASSERT_TRUE(log.ok());
    auto chunks = [&]
    {
        std::set<std::string> ids;
        auto index = (*log)->load();
        EXPECT_TRUE(index.ok());
        if(index.ok())
            for(const auto& record: index->records) ids.insert(record.chunk_id);
        return ids;
    };
    for(int64_t start: {100, 200, 300}) ASSERT_TRUE((*log)->append(Record(start, start + 100)).ok());
    ASSERT_TRUE((*log)->compact().ok());
    EXPECT_EQ(fs::file_size((*log)->logPath()), 0u);
    EXPECT_GT(fs::file_size((*log)->snapshotPath()), 0u);
    EXPECT_EQ(chunks(), (std::set<std::string>{"100", "200", "300"}));
    for(int64_t start: {400, 500}) ASSERT_TRUE((*log)->append(Record(start, start + 100)).ok());
    EXPECT_EQ(chunks(), (std::set<std::string>{"100", "200", "300", "400", "500"}));
    ASSERT_TRUE((*log)->compact().ok());
    EXPECT_EQ(chunks().size(), 5u);
}

TEST(ManifestLog, ConcurrentAppendsAllSurvive)
{
    auto directory = TestDirectory();
    auto log = ManifestLog::Open(*directory, "primary");
    ASSERT_TRUE(log.ok());
    std::vector<std::thread> threads;
    std::atomic<int> failures{0};
    for(int64_t thread = 0; thread < 4; ++thread)
        threads.emplace_back(
                [&, thread]
                {
                    for(int64_t i = 0; i < 40; ++i)
                        if(!(*log)->append(Record(1000 * (thread + 1) + 10 * i, 1000 * (thread + 1) + 10 * i + 5)).ok())
                            ++failures;
                });
    for(auto& thread: threads) thread.join();
    EXPECT_EQ(failures.load(), 0);
    auto index = (*log)->load();
    ASSERT_TRUE(index.ok());
    std::set<std::string> ids;
    for(const auto& record: index->records) ids.insert(record.chunk_id);
    EXPECT_EQ(index->records.size(), 160u);
    EXPECT_EQ(ids.size(), 160u);
}

TEST(FileTierStore, WatermarksAreIndependentPerStory)
{
    auto directory = TestDirectory();
    auto store = FileTierStore::Open(*directory, "primary", {{1, {100, 0}}, {2, {100, 0}}});
    ASSERT_TRUE(store.ok());
    auto window = [](StoryId story, int64_t start, int64_t end)
    {
        auto chunk = contract::Window(start, end);
        chunk.story_id = story;
        chunk.events[0].id.story_id = story;
        return chunk;
    };
    ASSERT_TRUE((*store)->publish(window(1, 100, 200)).ok());
    ASSERT_TRUE((*store)->publish(window(1, 200, 300)).ok());
    ASSERT_TRUE((*store)->publish(window(2, 100, 200)).ok());
    ASSERT_TRUE((*store)->publish(window(2, 300, 400)).ok());
    EXPECT_EQ((*store)->contiguousWatermark(1).value(), (Hlc{300, 0}));
    EXPECT_EQ((*store)->contiguousWatermark(2).value(), (Hlc{200, 0}));
}

TEST(FileTierStore, AStoryIdAboveInt64MaxSurvivesTheManifestAndARestart)
{
    constexpr StoryId big = 0xfedcba9876543210ULL;
    auto directory = TestDirectory();
    auto store = FileTierStore::Open(*directory, "primary", {{big, {100, 0}}});
    ASSERT_TRUE(store.ok());
    auto chunk = contract::Window();
    chunk.story_id = big;
    chunk.events[0].id.story_id = big;
    ASSERT_TRUE((*store)->publish(chunk).ok());
    store->reset();
    store = FileTierStore::Open(*directory, "primary", {{big, {100, 0}}});
    ASSERT_TRUE(store.ok());
    EXPECT_EQ((*store)->contiguousWatermark(big).value(), (Hlc{200, 0}));
    auto events = (*store)->read(big, contract::WholeArchive());
    ASSERT_TRUE(events.ok());
    ASSERT_EQ(events->size(), 1u);
    EXPECT_EQ(events->front().id.story_id, big);
    auto manifest = (*store)->manifest(big);
    ASSERT_TRUE(manifest.ok());
    ASSERT_EQ(manifest->size(), 1u);
    EXPECT_EQ(manifest->front().story_id, big);
}

TEST(FileTierStore, DeletingOneWritersFileLeavesTheOtherWritersCopyOfTheWindow)
{
    auto directory = TestDirectory();
    auto first = Open(*directory);
    auto second = Open(*directory, "secondary");
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());
    auto mine = (*first)->publish(contract::Window());
    auto theirs = (*second)->publish(contract::Window());
    ASSERT_TRUE(mine.ok());
    ASSERT_TRUE(theirs.ok());
    ASSERT_NE(mine->file, theirs->file);
    ASSERT_TRUE((*first)->eraseFile(mine->file).ok());
    EXPECT_FALSE(fs::exists(*directory / mine->file));
    EXPECT_TRUE(fs::exists(*directory / theirs->file));
    EXPECT_EQ((*first)->read(1, contract::WholeArchive())->size(), 1u);
    EXPECT_EQ((*first)->contiguousWatermark(1).value(), (Hlc{200, 0}));
}

TEST(FileTierStore, APublishLeavesOnlyItsWindowFileAndAFailedOneLeavesNothing)
{
    auto directory = TestDirectory();
    auto codec = std::make_shared<FailingCodec>(std::make_shared<ProtoChunkCodec>());
    auto store = FileTierStore::Open(*directory, "primary", {{1, {100, 0}}}, codec);
    ASSERT_TRUE(store.ok());
    auto files = [&]
    {
        std::vector<std::string> names;
        for(const auto& entry: fs::recursive_directory_iterator(*directory / "1"))
            if(entry.is_regular_file())
                names.push_back(entry.path().filename().string());
        return names;
    };
    codec->fail = true;
    EXPECT_FALSE((*store)->publish(contract::Window()).ok());
    EXPECT_TRUE(!fs::exists(*directory / "1") || files().empty());
    auto record = (*store)->publish(contract::Window());
    ASSERT_TRUE(record.ok());
    const auto names = files();
    ASSERT_EQ(names.size(), 1u);
    EXPECT_EQ(names.front(), fs::path(record->file).filename().string());
}

TEST(FileTierStore, ConcurrentPublishesOfTheSameWindowsFromTwoWritersEachKeepTheirOwnFile)
{
    auto directory = TestDirectory();
    auto first = Open(*directory);
    auto second = Open(*directory, "secondary");
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());
    std::atomic<int> failures{0};
    auto publish = [&](FileTierStore& store)
    {
        for(int64_t i = 0; i < 8; ++i)
            if(!store.publish(contract::Window(100 + 100 * i, 200 + 100 * i)).ok())
                ++failures;
    };
    std::thread a([&] { publish(**first); });
    std::thread b([&] { publish(**second); });
    a.join();
    b.join();
    EXPECT_EQ(failures.load(), 0);
    size_t files = 0;
    for(const auto& entry: fs::recursive_directory_iterator(*directory / "1"))
        if(entry.is_regular_file())
            ++files;
    EXPECT_EQ(files, 16u);
    EXPECT_EQ((*first)->manifest(1)->size(), 16u);
    EXPECT_EQ((*first)->read(1, contract::WholeArchive())->size(), 8u);
    EXPECT_EQ((*first)->contiguousWatermark(1).value(), (Hlc{900, 0}));
}

TEST(FileTierStore, EventsOutsideTheWindowAreRejectedAndPublishedEventsComeBackSorted)
{
    auto directory = TestDirectory();
    auto store = FileTierStore::Open(*directory, "primary", {{1, {100, 0}}}, std::make_shared<ProtoChunkCodec>());
    ASSERT_TRUE(store.ok());
    auto rejected = [&](auto mutate)
    {
        auto chunk = contract::Window();
        mutate(chunk);
        return (*store)->publish(chunk).status().code() == absl::StatusCode::kInvalidArgument;
    };
    EXPECT_TRUE(rejected([](Chunk& chunk) { chunk.events[0].hlc = {99, 0}; }));
    EXPECT_TRUE(rejected([](Chunk& chunk) { chunk.events[0].id.story_id = 2; }));
    EXPECT_TRUE(rejected([](Chunk& chunk) { chunk.events[0].id.sequence = 0; }));
    EXPECT_TRUE(rejected([](Chunk& chunk) { chunk.events[0].envelope.payload.assign((1u << 20) + 1, 'x'); }));
    auto large = contract::Window();
    large.events[0].envelope.payload.assign(1u << 20, 'x');
    ASSERT_TRUE((*store)->publish(large).ok());
    auto unsorted = contract::Window(200, 300);
    unsorted.events.clear();
    for(int64_t at: {250, 210, 230})
    {
        Event event;
        event.id = {1, 2, 3, static_cast<uint64_t>(at)};
        event.hlc = {at, 0};
        unsorted.events.push_back(event);
    }
    ASSERT_TRUE((*store)->publish(unsorted).ok());
    auto events = (*store)->read(1, {Range::Axis::Hlc, {200, 0}, {300, 0}});
    ASSERT_TRUE(events.ok());
    ASSERT_EQ(events->size(), 3u);
    EXPECT_EQ(events->at(0).hlc, (Hlc{210, 0}));
    EXPECT_EQ(events->at(1).hlc, (Hlc{230, 0}));
    EXPECT_EQ(events->at(2).hlc, (Hlc{250, 0}));
}

TEST(HDF5ChunkCodec, ArchiveFilesAreReadableWhileAnotherHandleHoldsAnExclusiveLock)
{
    auto directory = TestDirectory();
    HDF5ChunkCodec codec;
    const auto path = *directory / "locked.h5";
    ASSERT_TRUE(codec.writeChunk(path, contract::Window()).ok());
    const int fd = ::open(path.c_str(), O_RDONLY);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(::flock(fd, LOCK_EX | LOCK_NB), 0);
    auto read = codec.read(path);
    ::close(fd);
    ASSERT_TRUE(read.ok()) << read.status();
    EXPECT_EQ(read->size(), 1u);
}

TEST(HDF5ChunkCodec, LosslessEveryEventField)
{
    auto directory = TestDirectory();
    auto chunk = contract::Window();
    auto& event = chunk.events.front();
    event.id = {1, 0xfedcba9876543210ULL, 0xffffffffffffffffULL, 42};
    event.hlc = {111, 37};
    event.physical = {-456, 0xffffffffffffffffULL, ClockStatus::Synced};
    event.durability = Durability::Durable;
    event.envelope = {"application/octet-stream",
                      std::string("a\0\xffz", 4),
                      std::string("\0", 1) + std::string(15, '\xff'),
                      std::string(8, '\0'),
                      {{"", ""}, {"host", "dragon"}, {std::string("a\0b", 3), "back\\slash"}}};
    auto second = event;
    second.id.sequence++;
    second.physical = {-999, std::nullopt, ClockStatus::Unavailable};
    second.durability = Durability::Accepted;
    second.envelope = {};
    chunk.events.push_back(second);
    auto third = second;
    third.id.sequence++;
    third.physical = {0, 0, ClockStatus::Unsynced};
    third.durability = Durability::Unspecified;
    chunk.events.push_back(third);
    HDF5ChunkCodec codec;
    const auto path = *directory / "fields.h5";
    ASSERT_TRUE(codec.writeChunk(path, chunk).ok());
    auto read = codec.read(path);
    ASSERT_TRUE(read.ok()) << read.status();
    ASSERT_EQ(read->size(), chunk.events.size());
    for(std::size_t i = 0; i < read->size(); ++i)
    {
        const auto& expected = chunk.events[i];
        const auto& actual = (*read)[i];
        EXPECT_EQ(actual.id, expected.id);
        EXPECT_EQ(actual.hlc, expected.hlc);
        EXPECT_EQ(actual.physical.physical_ns, expected.physical.physical_ns);
        EXPECT_EQ(actual.physical.uncertainty_ns, expected.physical.uncertainty_ns);
        EXPECT_EQ(actual.physical.status, expected.physical.status);
        EXPECT_EQ(actual.durability, expected.durability);
        EXPECT_EQ(actual.envelope.content_type, expected.envelope.content_type);
        EXPECT_EQ(actual.envelope.payload, expected.envelope.payload);
        EXPECT_EQ(actual.envelope.trace_id, expected.envelope.trace_id);
        EXPECT_EQ(actual.envelope.span_id, expected.envelope.span_id);
        EXPECT_EQ(actual.envelope.attributes, expected.envelope.attributes);
    }
}

TEST(FileTierStore, MixedCodecsRecoverOrphansAndReadByExtension)
{
    auto directory = TestDirectory();
    auto proto = FileTierStore::Open(*directory, "primary", {{1, {100, 0}}}, std::make_shared<ProtoChunkCodec>());
    ASSERT_TRUE(proto.ok());
    auto first = (*proto)->publish(contract::Window());
    ASSERT_TRUE(first.ok());
    EXPECT_EQ(fs::path(first->file).extension(), ".pb");
    proto->reset();
    auto hdf5 = Open(*directory);
    ASSERT_TRUE(hdf5.ok());
    auto retry = (*hdf5)->publish(contract::Window());
    ASSERT_TRUE(retry.ok());
    EXPECT_EQ(retry->file, first->file);
    auto changed = contract::Window();
    changed.events[0].envelope.payload = "changed";
    EXPECT_FALSE((*hdf5)->publish(changed).ok());
    auto second = (*hdf5)->publish(contract::Window(200, 300));
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(fs::path(second->file).extension(), ".h5");
    hdf5->reset();
    std::ofstream(*directory / "manifest/primary.log", std::ios::trunc).close();
    proto = FileTierStore::Open(*directory, "primary", {{1, {100, 0}}}, std::make_shared<ProtoChunkCodec>());
    ASSERT_TRUE(proto.ok());
    auto events = (*proto)->read(1, contract::WholeArchive());
    ASSERT_TRUE(events.ok()) << events.status();
    EXPECT_EQ(events->size(), 2u);
    EXPECT_EQ((*proto)->contiguousWatermark(1).value(), (Hlc{300, 0}));
}

} // namespace

namespace contract
{
INSTANTIATE_TEST_SUITE_P(
        File,
        TierStoreContract,
        ::testing::Values(TierStoreFactory([] { return MakeFileStore(std::make_shared<ProtoChunkCodec>()); })),
        [](const auto&) { return "FileTier"; });
INSTANTIATE_TEST_SUITE_P(
        HDF5,
        TierStoreContract,
        ::testing::Values(TierStoreFactory([] { return MakeFileStore(std::make_shared<HDF5ChunkCodec>()); })),
        [](const auto&) { return "HDF5Tier"; });
} // namespace contract
} // namespace chronolog

namespace chronolog
{
TEST(FileTierStore, PhysicalPolicySurvivesBothCodecsAndManifestRecovery)
{
    for(const auto& codec: std::vector<std::shared_ptr<const ChunkCodec>>{std::make_shared<ProtoChunkCodec>(),
                                                                          std::make_shared<HDF5ChunkCodec>()})
    {
        auto directory = TestDirectory();
        auto store = FileTierStore::Open(*directory, "primary", {{1, {100, 0}}}, codec);
        ASSERT_TRUE(store.ok());
        Event event;
        event.id = {1, 2, 3, 1};
        event.hlc = {150, 0};
        event.physical = {150, 1, ClockStatus::Synced};
        event.durability = Durability::Durable;
        Chunk chunk{"policy", 1, {100, 0}, {200, 0}, {event}, false, true};
        auto record = (*store)->publish(chunk);
        ASSERT_TRUE(record.ok()) << record.status();
        EXPECT_TRUE(record->physical_policy);
        store->reset();
        auto reopened = FileTierStore::Open(*directory, "primary", {{1, {100, 0}}}, codec);
        ASSERT_TRUE(reopened.ok());
        auto manifest = (*reopened)->manifest(1);
        ASSERT_TRUE(manifest.ok());
        ASSERT_FALSE(manifest->empty());
        EXPECT_TRUE(manifest->front().physical_policy);
        reopened->reset();
        std::filesystem::remove_all(*directory / "manifest");
        reopened = FileTierStore::Open(*directory, "primary", {{1, {100, 0}}}, codec);
        ASSERT_TRUE(reopened.ok());
        manifest = (*reopened)->manifest(1);
        ASSERT_TRUE(manifest.ok());
        ASSERT_FALSE(manifest->empty());
        EXPECT_TRUE(manifest->front().physical_policy);
    }
}
} // namespace chronolog
