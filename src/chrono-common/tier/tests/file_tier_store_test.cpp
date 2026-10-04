#include "../../../tests/contract/tier_store_contract_test.cpp"
#include "tier/FileTierStore.h"
#include "chronolog/v1/chronolog.pb.h"
#include <absl/crc/crc32c.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <thread>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/vfs.h>
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
    absl::StatusOr<std::vector<Event>> decode(std::span<unsigned char> bytes) const override
    {
        return codec_->decode(bytes);
    }

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
    harness->tombstone = [h = harness.get()](StoryId story)
    { ASSERT_TRUE(static_cast<FileTierStore*>(h->sut.get())->tombstone(story).ok()); };
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

class BlockingRead
{
public:
    std::string filename;
    absl::StatusOr<ChunkBytes> operator()(const fs::path& path)
    {
        if(!filename.empty() && path.filename() != filename)
            return LoadChunkFile(path);
        {
            std::unique_lock lock(mutex_);
            ++entered_;
            changed_.notify_all();
            if(!changed_.wait_for(lock, std::chrono::seconds(5), [&] { return released_; }))
                timed_out_ = true;
        }
        return LoadChunkFile(path);
    }
    bool waitEntered(int count)
    {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, std::chrono::seconds(5), [&] { return entered_ >= count; });
    }
    bool release()
    {
        std::lock_guard lock(mutex_);
        released_ = true;
        changed_.notify_all();
        return !timed_out_;
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    int entered_{};
    bool released_{}, timed_out_{};
};

TEST(FileTierStore, HungArchiveReadEndsWithinTheDeadline)
{
    auto directory = TestDirectory();
    auto writer = FileTierStore::Open(*directory, "primary", {{1, {100, 0}}}, std::make_shared<ProtoChunkCodec>());
    ASSERT_TRUE(writer.ok());
    auto record = (*writer)->publish(contract::Window());
    ASSERT_TRUE(record.ok());
    const auto timeout = std::chrono::milliseconds(100);
    const auto slack = std::chrono::seconds(2);
    auto release = std::make_shared<std::promise<void>>();
    auto gate = release->get_future().share();
    auto exited = std::make_shared<std::promise<void>>();
    auto done = exited->get_future();
    auto reader = FileTierStore::OpenReadOnly(
            *directory,
            std::chrono::hours(1),
            [gate, exited](const fs::path&) -> absl::StatusOr<ChunkBytes>
            {
                gate.wait();
                exited->set_value();
                return absl::UnavailableError("released hung load");
            },
            2,
            {},
            timeout);
    ASSERT_TRUE(reader.ok());
    auto read = std::async(std::launch::async,
                           [&] { return (*reader)->readRecord(*record, {Range::Axis::Hlc, {100, 0}, {200, 0}}); });
    const bool ended = read.wait_for(timeout + slack) == std::future_status::ready;
    release->set_value();
    EXPECT_TRUE(ended);
    EXPECT_EQ(read.get().status().code(), absl::StatusCode::kUnavailable);
    EXPECT_EQ(done.wait_for(timeout + slack), std::future_status::ready);
}

TEST(FileTierStore, HungArchiveReadDoesNotBlockOtherReadsOrDestruction)
{
    auto directory = TestDirectory();
    auto writer = FileTierStore::Open(*directory, "primary", {{1, {100, 0}}}, std::make_shared<ProtoChunkCodec>());
    ASSERT_TRUE(writer.ok());
    auto hung = (*writer)->publish(contract::Window());
    auto other = contract::Window();
    other.story_id = 2;
    for(auto& event: other.events) event.id.story_id = 2;
    ASSERT_TRUE((*writer)->registerStory(2, Hlc{100, 0}).ok());
    auto healthy = (*writer)->publish(other);
    ASSERT_TRUE(hung.ok());
    ASSERT_TRUE(healthy.ok());
    const auto timeout = std::chrono::milliseconds(100);
    const auto slack = std::chrono::seconds(2);
    auto release = std::make_shared<std::promise<void>>();
    auto gate = release->get_future().share();
    auto exited = std::make_shared<std::promise<void>>();
    auto done = exited->get_future();
    auto reader = FileTierStore::OpenReadOnly(
            *directory,
            std::chrono::hours(1),
            [gate, exited, filename = hung->file](const fs::path& path) -> absl::StatusOr<ChunkBytes>
            {
                if(path.generic_string().ends_with(filename))
                {
                    gate.wait();
                    exited->set_value();
                    return absl::UnavailableError("released hung load");
                }
                return LoadChunkFile(path);
            },
            2,
            {},
            timeout);
    ASSERT_TRUE(reader.ok());
    auto reads = std::async(std::launch::async,
                            [&]
                            {
                                auto failed = (*reader)->readRecord(*hung, {Range::Axis::Hlc, {100, 0}, {200, 0}});
                                EXPECT_EQ(failed.status().code(), absl::StatusCode::kUnavailable);
                                auto success = (*reader)->readRecord(*healthy, {Range::Axis::Hlc, {100, 0}, {200, 0}});
                                EXPECT_TRUE(success.ok()) << success.status();
                                if(success.ok())
                                {
                                    EXPECT_EQ(success->size(), 1u);
                                }
                                reader->reset();
                            });
    const bool ended = reads.wait_for(2 * timeout + slack) == std::future_status::ready;
    release->set_value();
    EXPECT_TRUE(ended);
    reads.get();
    EXPECT_EQ(done.wait_for(timeout + slack), std::future_status::ready);
}

TEST(FileTierStore, ConcurrentReadsDoNotSerialize)
{
    auto directory = TestDirectory();
    BlockingRead gate;
    auto store = FileTierStore::Open(*directory,
                                     "primary",
                                     {{1, {100, 0}}},
                                     std::make_shared<ProtoChunkCodec>(),
                                     {},
                                     std::ref(gate));
    ASSERT_TRUE(store.ok());
    ASSERT_TRUE((*store)->publish(contract::Window()).ok());
    ASSERT_TRUE((*store)->publish(contract::Window(200, 300)).ok());
    auto first =
            std::async(std::launch::async, [&] { return (*store)->read(1, {Range::Axis::Hlc, {100, 0}, {200, 0}}); });
    ASSERT_TRUE(gate.waitEntered(1));
    auto second =
            std::async(std::launch::async, [&] { return (*store)->read(1, {Range::Axis::Hlc, {200, 0}, {300, 0}}); });
    const bool concurrent = gate.waitEntered(2);
    EXPECT_TRUE(gate.release());
    EXPECT_TRUE(concurrent);
    auto a = first.get(), b = second.get();
    ASSERT_TRUE(a.ok()) << a.status();
    ASSERT_TRUE(b.ok()) << b.status();
    ASSERT_EQ(a->size(), 1u);
    ASSERT_EQ(b->size(), 1u);
    EXPECT_EQ(a->front().id.sequence, 100u);
    EXPECT_EQ(b->front().id.sequence, 200u);
}

TEST(FileTierStore, PublishIsNotBlockedByARead)
{
    auto directory = TestDirectory();
    BlockingRead gate;
    auto store = FileTierStore::Open(*directory,
                                     "primary",
                                     {{1, {100, 0}}},
                                     std::make_shared<ProtoChunkCodec>(),
                                     {},
                                     std::ref(gate));
    ASSERT_TRUE(store.ok());
    ASSERT_TRUE((*store)->publish(contract::Window()).ok());
    auto reading = std::async(std::launch::async, [&] { return (*store)->read(1, contract::WholeArchive()); });
    ASSERT_TRUE(gate.waitEntered(1));
    auto publishing = std::async(std::launch::async, [&] { return (*store)->publish(contract::Window(200, 300)); });
    const auto ready = publishing.wait_for(std::chrono::seconds(5));
    EXPECT_TRUE(gate.release());
    EXPECT_EQ(ready, std::future_status::ready);
    ASSERT_TRUE(publishing.get().ok());
    const auto events = reading.get();
    ASSERT_TRUE(events.ok()) << events.status();
    EXPECT_EQ(events->size(), 1u);
    EXPECT_EQ((*store)->contiguousWatermark(1).value(), (Hlc{300, 0}));
}

// A publish visits the same number of manifest records whether the story holds 16 files or 400: the story view takes
// the new record alone, the watermark walk resumes at the cached watermark, and the duplicate lookup is by key.
TEST(FileTierStore, PublishWorkDoesNotGrowWithTheManifest)
{
    auto directory = TestDirectory();
    constexpr int64_t Base = 1'000'000, Width = 100;
    auto store = FileTierStore::Open(*directory, "primary", {{1, {Base, 0}}}, std::make_shared<ProtoChunkCodec>());
    ASSERT_TRUE(store.ok()) << store.status();
    const auto publish = [&](int64_t index)
    {
        const auto before = (*store)->viewWorkForTesting();
        const auto published = (*store)->publish(contract::Window(Base + index * Width, Base + (index + 1) * Width));
        EXPECT_TRUE(published.ok()) << published.status();
        return (*store)->viewWorkForTesting() - before;
    };
    uint64_t small = 0, large = 0;
    for(int64_t index = 0; index < 400; ++index)
    {
        const auto work = publish(index);
        if(index == 15)
            small = work;
        large = work;
    }
    EXPECT_GT(small, 0u);
    EXPECT_EQ(large, small);
    EXPECT_EQ((*store)->contiguousWatermark(1).value(), (Hlc{Base + 400 * Width, 0}));
    const auto records = (*store)->manifest(1);
    ASSERT_TRUE(records.ok());
    EXPECT_EQ(records->size(), 400u);
}

// The view a store keeps while it publishes equals the view a fresh reader builds from the same manifest, after every
// publish: windows arrive out of order, leave gaps that later fill, and their names do not sort in window order.
TEST(FileTierStore, IncrementallyGrownViewEqualsARebuiltView)
{
    auto directory = TestDirectory();
    auto store = FileTierStore::Open(*directory, "primary", {{1, {10, 0}}}, std::make_shared<ProtoChunkCodec>());
    ASSERT_TRUE(store.ok()) << store.status();
    const std::vector<int64_t> order{3, 0, 1, 12, 2, 9, 10, 4, 11, 5, 150, 6, 7, 8, 13, 99, 14};
    std::set<int64_t> done;
    for(const auto index: order)
    {
        ASSERT_TRUE((*store)->publish(contract::Window((index + 1) * 10, (index + 2) * 10)).ok());
        auto reader = FileTierStore::OpenReadOnly(*directory);
        ASSERT_TRUE(reader.ok()) << reader.status();
        const auto grown = (*store)->manifest(1), rebuilt = (*reader)->manifest(1);
        ASSERT_TRUE(grown.ok() && rebuilt.ok());
        ASSERT_EQ(grown->size(), rebuilt->size());
        for(size_t i = 0; i < grown->size(); ++i)
        {
            EXPECT_EQ((*grown)[i].file, (*rebuilt)[i].file);
            EXPECT_EQ((*grown)[i].state, (*rebuilt)[i].state);
            EXPECT_EQ((*grown)[i].start, (*rebuilt)[i].start);
            EXPECT_EQ((*grown)[i].end, (*rebuilt)[i].end);
        }
        // The watermark is the end of the last window of the gap-free run that starts at the anchor.
        done.insert(index);
        int64_t contiguous = 0;
        while(done.contains(contiguous)) ++contiguous;
        EXPECT_EQ((*store)->contiguousWatermark(1).value(), (Hlc{(contiguous + 1) * 10, 0}));
    }
    EXPECT_EQ((*store)->contiguousWatermark(1).value(), (Hlc{160, 0}));
}

TEST(FileTierStore, AFileErasedDuringAReadNeverSilentlyDropsEvents)
{
    auto directory = TestDirectory();
    BlockingRead gate;
    auto store = FileTierStore::Open(*directory,
                                     "primary",
                                     {{1, {100, 0}}},
                                     std::make_shared<ProtoChunkCodec>(),
                                     {},
                                     std::ref(gate));
    ASSERT_TRUE(store.ok());
    auto erased = (*store)->publish(contract::Window());
    ASSERT_TRUE(erased.ok());
    ASSERT_TRUE((*store)->publish(contract::Window(200, 300)).ok());
    gate.filename = fs::path(erased->file).filename();
    auto reading = std::async(std::launch::async, [&] { return (*store)->read(1, contract::WholeArchive()); });
    ASSERT_TRUE(gate.waitEntered(1));
    // A destroy erases through Deleted records after the tombstone; an admitted read reaching it fails (I6.7).
    ASSERT_TRUE((*store)->tombstone(1).ok());
    auto erasing = std::async(std::launch::async, [&] { return (*store)->eraseFile(erased->file); });
    const auto ready = erasing.wait_for(std::chrono::seconds(5));
    EXPECT_TRUE(gate.release());
    EXPECT_EQ(ready, std::future_status::ready);
    ASSERT_TRUE(erasing.get().ok());
    EXPECT_TRUE(absl::IsUnavailable(reading.get().status()));
    EXPECT_EQ((*store)->contiguousWatermark(1).value(), (Hlc{300, 0}));
    EXPECT_TRUE(absl::IsUnavailable((*store)->readRecord(*erased, contract::WholeArchive()).status()));
}

// W10.15: "CLOCK_STATUS_UNSPECIFIED on the wire maps to Unavailable in every adapter, never to Synced; the proto
// comment states it." An archive chunk record that carries it decodes as Unavailable, with or without a bound.
TEST(ProtoChunkCodec, UnspecifiedClockStatusIsUnavailable)
{
    std::string bytes;
    for(const uint64_t sequence: {1, 2})
    {
        v1::Event event;
        event.mutable_id()->set_story_id(1);
        event.mutable_id()->set_writer_id(2);
        event.mutable_id()->set_incarnation(3);
        event.mutable_id()->set_sequence(sequence);
        event.mutable_hlc()->set_physical_ns(100 + static_cast<int64_t>(sequence));
        event.mutable_physical()->set_physical_ns(100);
        if(sequence == 2)
            event.mutable_physical()->set_uncertainty_ns(5);
        event.mutable_physical()->set_status(v1::CLOCK_STATUS_UNSPECIFIED);
        event.set_durability(v1::DURABILITY_DURABLE);
        const auto record = event.SerializeAsString();
        auto length = static_cast<uint32_t>(record.size());
        do {
            bytes.push_back(static_cast<char>((length & 0x7f) | (length > 0x7f ? 0x80 : 0)));
            length >>= 7;
        } while(length != 0);
        bytes += record;
    }
    std::vector<unsigned char> chunk(bytes.begin(), bytes.end());
    auto events = ProtoChunkCodec().decode(chunk);
    ASSERT_TRUE(events.ok()) << events.status();
    ASSERT_EQ(events->size(), 2u);
    for(const auto& event: *events) EXPECT_EQ(event.physical.status, ClockStatus::Unavailable) << event.id.sequence;
}

TEST(ChunkCodec, OnlyEnoentAndEstaleMeanTheFileVanished)
{
    EXPECT_TRUE(ArchiveFileVanished(ArchiveFileError("open archived chunk", ENOENT)));
    EXPECT_TRUE(ArchiveFileVanished(ArchiveFileError("read archived chunk", ESTALE)));
    EXPECT_FALSE(ArchiveFileVanished(ArchiveFileError("read archived chunk", EIO)));
    EXPECT_TRUE(absl::IsUnavailable(ArchiveFileError("stat archived chunk", ESTALE)));
    auto directory = TestDirectory();
    auto missing = LoadChunkFile(*directory / "1/missing.pb");
    EXPECT_TRUE(absl::IsUnavailable(missing.status()));
    EXPECT_TRUE(ArchiveFileVanished(missing.status()));
}

TEST(FileTierStore, VanishedFileAfterDeleteIsSkippedNotSourceFailed)
{
    for(const bool stale_after_open: {false, true})
        for(const int api: {0, 1, 2})
        {
            SCOPED_TRACE(std::to_string(stale_after_open) + "/" + std::to_string(api));
            auto directory = TestDirectory();
            auto writer = Open(*directory);
            ASSERT_TRUE(writer.ok());
            auto erased = (*writer)->publish(contract::Window());
            auto kept = (*writer)->publish(contract::Window(200, 300));
            ASSERT_TRUE(erased.ok());
            ASSERT_TRUE(kept.ok());
            std::atomic<int> erasures{0};
            auto load = [&](const fs::path& path) -> absl::StatusOr<ChunkBytes>
            {
                if(path.filename() != fs::path(erased->file).filename() || erasures++ != 0)
                    return LoadChunkFile(path);
                auto bytes = stale_after_open ? LoadChunkFile(path) : absl::StatusOr<ChunkBytes>();
                EXPECT_TRUE((*writer)->eraseFile(erased->file).ok());
                if(!stale_after_open)
                    return LoadChunkFile(path);
                EXPECT_TRUE(bytes.ok()) << bytes.status();
                return ArchiveFileError("read archived chunk", ESTALE);
            };
            auto reader = FileTierStore::OpenReadOnly(*directory, std::chrono::hours(1), load, 2);
            ASSERT_TRUE(reader.ok());
            if(api == 0)
            {
                auto events = (*reader)->readRecord(*erased, contract::WholeArchive());
                ASSERT_TRUE(events.ok()) << events.status();
                EXPECT_TRUE(events->empty());
            }
            else if(api == 1)
            {
                const std::vector<ManifestRecord> records{*erased, *kept};
                auto batch = (*reader)->readRecords(records, contract::WholeArchive());
                ASSERT_EQ(batch.size(), 2u);
                ASSERT_TRUE(batch[0].ok()) << batch[0].status();
                EXPECT_TRUE(batch[0]->empty());
                ASSERT_TRUE(batch[1].ok()) << batch[1].status();
                ASSERT_EQ(batch[1]->size(), 1u);
                EXPECT_EQ(batch[1]->front().id.sequence, 200u);
            }
            else
            {
                auto events = (*reader)->read(1, contract::WholeArchive());
                ASSERT_TRUE(events.ok()) << events.status();
                ASSERT_EQ(events->size(), 1u);
                EXPECT_EQ(events->front().id.sequence, 200u);
            }
            EXPECT_EQ(erasures.load(), 1);
            EXPECT_FALSE((*reader)->incomplete(1, contract::WholeArchive()).value());
        }
}

TEST(FileTierStore, VanishedEffectiveFileIsStillAFailure)
{
    for(const int cause: {0, 1, 2})
    {
        SCOPED_TRACE(cause);
        auto directory = TestDirectory();
        auto writer = Open(*directory);
        ASSERT_TRUE(writer.ok());
        auto record = (*writer)->publish(contract::Window());
        ASSERT_TRUE(record.ok());
        auto load = [&](const fs::path& path) -> absl::StatusOr<ChunkBytes>
        {
            if(cause == 1)
                return ArchiveFileError("stat archived chunk", ESTALE);
            return LoadChunkFile(path);
        };
        auto reader = FileTierStore::OpenReadOnly(*directory, std::chrono::hours(1), load, 2);
        ASSERT_TRUE(reader.ok());
        if(cause == 0)
        {
            ASSERT_TRUE(fs::remove(*directory / record->file));
        }
        if(cause == 2)
        {
            ASSERT_TRUE((*writer)->tombstone(1).ok());
            ASSERT_TRUE((*writer)->eraseFile(record->file).ok());
        }
        auto single = (*reader)->readRecord(*record, contract::WholeArchive());
        EXPECT_TRUE(absl::IsUnavailable(single.status())) << single.status();
        EXPECT_TRUE(ArchiveFileVanished(single.status()));
        auto batch = (*reader)->readRecords(std::span<const ManifestRecord>(&*record, 1), contract::WholeArchive());
        ASSERT_EQ(batch.size(), 1u);
        EXPECT_TRUE(absl::IsUnavailable(batch[0].status())) << batch[0].status();
        if(cause != 2)
        {
            EXPECT_TRUE(absl::IsUnavailable((*reader)->read(1, contract::WholeArchive()).status()));
        }
    }
}

TEST(FileTierStore, RecoveryDoesNotMarkADeletedFileLost)
{
    auto directory = TestDirectory();
    auto peer = Open(*directory, "peer");
    ASSERT_TRUE(peer.ok());
    auto erased = (*peer)->publish(contract::Window());
    ASSERT_TRUE(erased.ok());
    ASSERT_TRUE((*peer)->publish(contract::Window(200, 300)).ok());
    int erasures = 0;
    auto load = [&](const fs::path& path)
    {
        if(path.filename() == fs::path(erased->file).filename() && erasures++ == 0)
        {
            EXPECT_TRUE((*peer)->eraseFile(erased->file).ok());
        }
        return LoadChunkFile(path);
    };
    auto store =
            FileTierStore::Open(*directory, "primary", {{1, {100, 0}}}, std::make_shared<HDF5ChunkCodec>(), {}, load);
    ASSERT_TRUE(store.ok()) << store.status();
    EXPECT_EQ(erasures, 1);
    auto records = (*store)->manifest(1);
    ASSERT_TRUE(records.ok());
    for(const auto& record: *records)
    {
        EXPECT_NE(record.state, ManifestState::Lost) << record.file;
        if(record.file == erased->file)
        {
            EXPECT_EQ(record.state, ManifestState::Deleted);
        }
    }
    EXPECT_FALSE((*store)->incomplete(1, contract::WholeArchive()).value());
}

TEST(FileTierStore, RecordsFromOneSnapshotAreReadInParallel)
{
    for(const auto axis: {Range::Axis::Hlc, Range::Axis::Physical})
    {
        auto directory = TestDirectory();
        auto writer = Open(*directory);
        ASSERT_TRUE(writer.ok());
        ASSERT_TRUE((*writer)->publish(contract::Window(200, 300)).ok());
        ASSERT_TRUE((*writer)->publish(contract::Window()).ok());
        writer->reset();
        BlockingRead gate;
        auto store = FileTierStore::OpenReadOnly(*directory, std::chrono::hours(1), std::ref(gate), 2);
        ASSERT_TRUE(store.ok());
        auto reading = std::async(std::launch::async, [&] { return (*store)->read(1, {axis, {0, 0}, {1000, 0}}); });
        const bool concurrent = gate.waitEntered(2);
        EXPECT_TRUE(gate.release());
        EXPECT_TRUE(concurrent);
        const auto events = reading.get();
        ASSERT_TRUE(events.ok()) << events.status();
        ASSERT_EQ(events->size(), 2u);
        EXPECT_EQ(events->at(0).id.sequence, 100u);
        EXPECT_EQ(events->at(1).id.sequence, 200u);
    }
}

TEST(FileTierStore, BatchReadMatchesSequentialReadRecord)
{
    auto directory = TestDirectory();
    auto store = Open(*directory);
    ASSERT_TRUE(store.ok());
    std::vector<ManifestRecord> records;
    for(int i = 0; i < 7; ++i)
    {
        auto chunk = contract::Window(100 + i * 100, 200 + i * 100);
        auto second = chunk.events.front();
        ++second.id.sequence;
        ++second.hlc.logical;
        chunk.events.push_back(second);
        auto record = (*store)->publish(chunk);
        ASSERT_TRUE(record.ok());
        records.push_back(*record);
    }
    fs::remove(*directory / records[3].file);
    for(auto axis: {Range::Axis::Hlc, Range::Axis::Physical})
        for(size_t cap: {0u, 1u, 2u, 99u})
        {
            Range range{axis, {150, 0}, {650, 0}};
            auto batch = (*store)->readRecords(records, range, cap);
            ASSERT_EQ(batch.size(), records.size());
            for(size_t i = 0; i < records.size(); ++i)
            {
                auto sequential = (*store)->readRecord(records[i], range, cap);
                EXPECT_EQ(batch[i].status(), sequential.status());
                if(sequential.ok())
                {
                    ASSERT_TRUE(batch[i].ok());
                    ASSERT_EQ(batch[i]->size(), sequential->size());
                    for(size_t j = 0; j < sequential->size(); ++j)
                    {
                        EXPECT_EQ(batch[i]->at(j).id, sequential->at(j).id);
                        EXPECT_EQ(batch[i]->at(j).hlc, sequential->at(j).hlc);
                    }
                }
            }
            EXPECT_FALSE(batch[3].ok());
            EXPECT_TRUE(batch[2].ok());
            EXPECT_TRUE(batch[4].ok());
        }
}

TEST(FileTierStore, BatchReadRunsRecordsConcurrently)
{
    auto directory = TestDirectory();
    BlockingRead gate;
    auto store = FileTierStore::Open(*directory,
                                     "primary",
                                     {{1, {100, 0}}},
                                     std::make_shared<ProtoChunkCodec>(),
                                     {},
                                     std::ref(gate),
                                     2);
    ASSERT_TRUE(store.ok());
    std::vector<ManifestRecord> records;
    for(int i = 0; i < 5; ++i)
    {
        auto record = (*store)->publish(contract::Window(100 + i * 100, 200 + i * 100));
        ASSERT_TRUE(record.ok());
        records.push_back(*record);
    }
    auto pending =
            std::async(std::launch::async, [&] { return (*store)->readRecords(records, contract::WholeArchive()); });
    EXPECT_TRUE(gate.waitEntered(2));
    EXPECT_TRUE(gate.release());
    for(const auto& result: pending.get()) EXPECT_TRUE(result.ok());
}

TEST(FileTierStore, BatchReadOnReadOnlyStore)
{
    auto directory = TestDirectory();
    auto writer = Open(*directory);
    ASSERT_TRUE(writer.ok());
    auto record = (*writer)->publish(contract::Window());
    ASSERT_TRUE(record.ok());
    auto reader = FileTierStore::OpenReadOnly(*directory, std::chrono::hours(1), {}, 2);
    ASSERT_TRUE(reader.ok());
    auto results = (*reader)->readRecords(std::span<const ManifestRecord>(&*record, 1), contract::WholeArchive());
    ASSERT_EQ(results.size(), 1u);
    ASSERT_TRUE(results[0].ok());
    ASSERT_EQ(results[0]->size(), 1u);
    EXPECT_EQ(results[0]->front().id.sequence, 100u);
}

TEST(FileTierStore, BatchLoadDecodesOnCallerInInputOrder)
{
    for(const auto& codec: std::vector<std::shared_ptr<const ChunkCodec>>{std::make_shared<ProtoChunkCodec>(),
                                                                          std::make_shared<HDF5ChunkCodec>()})
    {
        auto directory = TestDirectory();
        auto writer = FileTierStore::Open(*directory, "primary", {{1, {100, 0}}}, codec);
        ASSERT_TRUE(writer.ok());
        for(int i = 0; i < 9; ++i) ASSERT_TRUE((*writer)->publish(contract::Window(100 + 100 * i, 200 + 100 * i)).ok());
        auto records = (*writer)->manifest(1);
        ASSERT_TRUE(records.ok());
        const auto caller = std::this_thread::get_id();
        std::atomic<size_t> loads{};
        std::vector<std::string> decoded;
        auto reader = FileTierStore::OpenReadOnly(
                *directory,
                std::chrono::hours(1),
                [&](const fs::path& file)
                {
                    EXPECT_NE(std::this_thread::get_id(), caller);
                    ++loads;
                    return LoadChunkFile(file);
                },
                2,
                [&](const fs::path& file, ChunkBytes& bytes)
                {
                    EXPECT_EQ(std::this_thread::get_id(), caller);
                    decoded.push_back(file.filename().string());
                    return DecodeChunkFile(file, bytes);
                });
        ASSERT_TRUE(reader.ok());
        auto events = (*reader)->read(1, {Range::Axis::Hlc, {0, 0}, {2000, 0}});
        ASSERT_TRUE(events.ok()) << events.status();
        EXPECT_EQ(events->size(), 9u);
        ASSERT_EQ(decoded.size(), records->size());
        for(size_t i = 0; i < records->size(); ++i)
            EXPECT_EQ(decoded[i], fs::path((*records)[i].file).filename().string());
        decoded.clear();
        std::reverse(records->begin(), records->end());
        auto batch = (*reader)->readRecords(*records, {Range::Axis::Hlc, {0, 0}, {2000, 0}}, 1);
        ASSERT_EQ(batch.size(), records->size());
        ASSERT_EQ(decoded.size(), records->size());
        for(size_t i = 0; i < records->size(); ++i)
        {
            ASSERT_TRUE(batch[i].ok());
            EXPECT_EQ(batch[i]->size(), 1u);
            EXPECT_EQ(decoded[i], fs::path((*records)[i].file).filename().string());
        }
        EXPECT_EQ(loads.load(), 18u);
    }
}

TEST(FileTierStore, LoadedBytesSurviveUnlinkAndDecodeFailureStaysInItsSlot)
{
    for(const auto& codec: std::vector<std::shared_ptr<const ChunkCodec>>{std::make_shared<ProtoChunkCodec>(),
                                                                          std::make_shared<HDF5ChunkCodec>()})
    {
        auto directory = TestDirectory();
        auto writer = FileTierStore::Open(*directory, "primary", {{1, {100, 0}}}, codec);
        ASSERT_TRUE(writer.ok());
        for(int i = 0; i < 3; ++i) ASSERT_TRUE((*writer)->publish(contract::Window(100 + 100 * i, 200 + 100 * i)).ok());
        auto records = (*writer)->manifest(1);
        ASSERT_TRUE(records.ok());
        auto reader = FileTierStore::OpenReadOnly(
                *directory,
                std::chrono::hours(1),
                [&](const fs::path& file)
                {
                    auto bytes = LoadChunkFile(file);
                    if(bytes.ok())
                    {
                        EXPECT_TRUE(fs::remove(file));
                        if(file.filename() == fs::path((*records)[1].file).filename())
                            std::fill_n(bytes->data.get(), bytes->size, 0xff);
                    }
                    return bytes;
                },
                2);
        ASSERT_TRUE(reader.ok());
        auto batch = (*reader)->readRecords(*records, contract::WholeArchive());
        ASSERT_EQ(batch.size(), 3u);
        ASSERT_TRUE(batch[0].ok());
        EXPECT_EQ(batch[0]->size(), 1u);
        EXPECT_FALSE(batch[1].ok());
        ASSERT_TRUE(batch[2].ok());
        EXPECT_EQ(batch[2]->size(), 1u);
    }
}

TEST(FileTierStore, ReadRecordErasedBeforeOpenIsUnavailable)
{
    auto directory = TestDirectory();
    auto writer = Open(*directory);
    ASSERT_TRUE(writer.ok());
    auto record = (*writer)->publish(contract::Window());
    ASSERT_TRUE(record.ok());
    BlockingRead gate;
    auto reader = FileTierStore::OpenReadOnly(*directory, std::chrono::hours(1), std::ref(gate));
    ASSERT_TRUE(reader.ok());
    auto reading =
            std::async(std::launch::async, [&] { return (*reader)->readRecord(*record, contract::WholeArchive()); });
    ASSERT_TRUE(gate.waitEntered(1));
    ASSERT_TRUE((*writer)->tombstone(1).ok());
    ASSERT_TRUE((*writer)->eraseFile(record->file).ok());
    EXPECT_TRUE(gate.release());
    EXPECT_TRUE(absl::IsUnavailable(reading.get().status()));
}

Chunk PhysicalChunk(int64_t hlc, TimeReading physical)
{
    auto chunk = contract::Window(hlc, hlc + 100);
    chunk.events.front().physical = physical;
    return chunk;
}

class CountingRead
{
public:
    absl::StatusOr<ChunkBytes> operator()(const fs::path& path)
    {
        {
            std::lock_guard lock(mutex_);
            files_.insert(path.filename().string());
        }
        return LoadChunkFile(path);
    }
    std::set<std::string> files()
    {
        std::lock_guard lock(mutex_);
        return files_;
    }
    void clear()
    {
        std::lock_guard lock(mutex_);
        files_.clear();
    }

private:
    std::mutex mutex_;
    std::set<std::string> files_;
};

TEST(FileTierStore, PhysicalReadSkipsFilesOutsideTheRange)
{
    for(const auto& codec: std::vector<std::shared_ptr<const ChunkCodec>>{std::make_shared<ProtoChunkCodec>(),
                                                                          std::make_shared<HDF5ChunkCodec>()})
    {
        auto directory = TestDirectory();
        CountingRead reads;
        auto store = FileTierStore::Open(*directory, "primary", {{1, {100, 0}}}, codec, {}, std::ref(reads));
        ASSERT_TRUE(store.ok());
        auto outside = (*store)->publish(PhysicalChunk(100, {10, 0, ClockStatus::Synced}));
        auto inside = (*store)->publish(PhysicalChunk(200, {105, 5, ClockStatus::Synced}));
        ASSERT_TRUE(outside.ok());
        ASSERT_TRUE(inside.ok());
        ASSERT_TRUE((*store)->publish(PhysicalChunk(300, {200, 0, ClockStatus::Synced})).ok());
        const Range range{Range::Axis::Physical, {100, 0}, {101, 0}};
        auto events = (*store)->read(1, range);
        ASSERT_TRUE(events.ok()) << events.status();
        ASSERT_EQ(events->size(), 1u);
        EXPECT_EQ(events->front().id.sequence, 200u);
        EXPECT_EQ(reads.files(), (std::set<std::string>{fs::path(inside->file).filename().string()}));
        reads.clear();
        events = (*store)->readRecord(*outside, range);
        ASSERT_TRUE(events.ok()) << events.status();
        EXPECT_TRUE(events->empty());
        EXPECT_TRUE(reads.files().empty());
    }
}

TEST(FileTierStore, PhysicalPruningNeverDropsAnIntersectingEvent)
{
    for(const auto& codec: std::vector<std::shared_ptr<const ChunkCodec>>{std::make_shared<ProtoChunkCodec>(),
                                                                          std::make_shared<HDF5ChunkCodec>()})
    {
        auto directory = TestDirectory();
        CountingRead reads;
        auto store = FileTierStore::Open(*directory, "primary", {{1, {100, 0}}}, codec, {}, std::ref(reads));
        ASSERT_TRUE(store.ok());
        std::vector<ManifestRecord> records;
        int64_t hlc = 100;
        for(const auto reading:
            std::vector<TimeReading>{{99, 0, ClockStatus::Synced},
                                     {99, 1, ClockStatus::Synced},
                                     {101, 1, ClockStatus::Synced},
                                     {102, 1, ClockStatus::Synced},
                                     {101, std::nullopt, ClockStatus::Synced},
                                     {100, 0, ClockStatus::Unsynced},
                                     {500, 0, ClockStatus::Unavailable},
                                     {100, PhysicalPolicy{}.uncertainty_cap_ns + 1, ClockStatus::Synced}})
        {
            auto record = (*store)->publish(PhysicalChunk(hlc, reading));
            ASSERT_TRUE(record.ok());
            records.push_back(*record);
            hlc += 100;
        }
        {
            auto legacy = ManifestLog::Open(*directory, "legacy");
            ASSERT_TRUE(legacy.ok());
            auto record = Record(900, 1000);
            record.file = "1/legacy" + codec->extension();
            std::ofstream(*directory / record.file).close();
            ASSERT_TRUE(codec->writeChunk(*directory / record.file, PhysicalChunk(900, {500, 0, ClockStatus::Synced}))
                                .ok());
            ASSERT_TRUE((*legacy)->append(record).ok());
            records.push_back(record);
        }
        {
            auto orphan = FileTierStore::Open(*directory, "orphan", {{1, {100, 0}}}, codec);
            ASSERT_TRUE(orphan.ok());
            for(const auto at: {1000, 1100})
            {
                auto record = (*orphan)->publish(PhysicalChunk(at, {at == 1000 ? 100 : 1000, 0, ClockStatus::Synced}));
                ASSERT_TRUE(record.ok());
                records.push_back(*record);
            }
        }
        fs::resize_file(*directory / "manifest/orphan.log", 0);
        store->reset();
        store = FileTierStore::Open(*directory, "primary", {{1, {100, 0}}}, codec, {}, std::ref(reads));
        ASSERT_TRUE(store.ok()) << store.status();
        reads.clear();
        const Range range{Range::Axis::Physical, {100, 0}, {101, 0}};
        auto events = (*store)->read(1, range);
        ASSERT_TRUE(events.ok()) << events.status();
        std::set<uint64_t> sequences;
        for(const auto& event: *events) sequences.insert(event.id.sequence);
        EXPECT_EQ(sequences, (std::set<uint64_t>{200, 300, 600, 800, 1000}));
        std::set<std::string> expected;
        for(const size_t i: {1u, 2u, 4u, 5u, 6u, 7u, 8u, 9u})
            expected.insert(fs::path(records[i].file).filename().string());
        EXPECT_EQ(reads.files(), expected);
        reads.clear();
        for(const auto& record: records) ASSERT_TRUE((*store)->readRecord(record, range).ok());
        EXPECT_EQ(reads.files(), expected);
    }
}

TEST(FileTierStore, PhysicalPruningSurvivesRestartAndCompaction)
{
    auto directory = TestDirectory();
    auto store = Open(*directory);
    ASSERT_TRUE(store.ok());
    auto inside = (*store)->publish(PhysicalChunk(100, {100, 0, ClockStatus::Synced}));
    ASSERT_TRUE(inside.ok());
    ASSERT_TRUE((*store)->publish(PhysicalChunk(200, {200, 0, ClockStatus::Synced})).ok());
    ASSERT_TRUE((*store)->publish(PhysicalChunk(300, {300, 0, ClockStatus::Synced})).ok());
    for(const bool compact: {false, true})
    {
        if(compact)
        {
            ASSERT_TRUE((*store)->compact().ok());
        }
        store->reset();
        CountingRead reads;
        auto reader = FileTierStore::OpenReadOnly(*directory, std::chrono::hours(1), std::ref(reads));
        ASSERT_TRUE(reader.ok());
        auto events = (*reader)->read(1, {Range::Axis::Physical, {100, 0}, {101, 0}});
        ASSERT_TRUE(events.ok()) << events.status();
        ASSERT_EQ(events->size(), 1u);
        EXPECT_EQ(reads.files(), (std::set<std::string>{fs::path(inside->file).filename().string()}));
        reader->reset();
        store = Open(*directory);
        ASSERT_TRUE(store.ok());
    }
}

TEST(FileTierStore, PhysicalPruningHandlesSaturatedIntervals)
{
    auto directory = TestDirectory();
    CountingRead reads;
    auto store = FileTierStore::Open(*directory,
                                     "primary",
                                     {{1, {100, 0}}},
                                     std::make_shared<HDF5ChunkCodec>(),
                                     {},
                                     std::ref(reads));
    ASSERT_TRUE(store.ok());
    const auto low = std::numeric_limits<int64_t>::min(), high = std::numeric_limits<int64_t>::max();
    auto first = (*store)->publish(PhysicalChunk(100, {low, 1, ClockStatus::Synced}));
    auto last = (*store)->publish(PhysicalChunk(200, {high, 1, ClockStatus::Synced}));
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(last.ok());
    for(const auto range:
        {Range{Range::Axis::Physical, {low, 0}, {low + 1, 0}}, Range{Range::Axis::Physical, {high - 1, 0}, {high, 0}}})
    {
        reads.clear();
        auto events = (*store)->read(1, range);
        ASSERT_TRUE(events.ok()) << events.status();
        ASSERT_EQ(events->size(), 1u);
        EXPECT_EQ(events->front().id.sequence, range.start.physical_ns == low ? 100u : 200u);
        EXPECT_EQ(reads.files().size(), 1u);
    }
}

TEST(FileTierStore, PhysicalBoundsCoverEveryEventInAFile)
{
    for(const auto& codec: std::vector<std::shared_ptr<const ChunkCodec>>{std::make_shared<ProtoChunkCodec>(),
                                                                          std::make_shared<HDF5ChunkCodec>()})
    {
        auto directory = TestDirectory();
        CountingRead reads;
        auto store = FileTierStore::Open(*directory, "primary", {{1, {100, 0}}}, codec, {}, std::ref(reads));
        ASSERT_TRUE(store.ok());
        auto chunk = PhysicalChunk(100, {500, 0, ClockStatus::Synced});
        for(const auto reading: std::vector<TimeReading>{{99, 1, ClockStatus::Synced},
                                                         {101, 1, ClockStatus::Synced},
                                                         {600, 0, ClockStatus::Synced}})
        {
            auto event = chunk.events.front();
            event.hlc.physical_ns += static_cast<int64_t>(chunk.events.size());
            event.id.sequence += chunk.events.size();
            event.physical = reading;
            chunk.events.push_back(std::move(event));
        }
        ASSERT_TRUE((*store)->publish(std::move(chunk)).ok());
        auto events = (*store)->read(1, {Range::Axis::Physical, {100, 0}, {101, 0}});
        ASSERT_TRUE(events.ok()) << events.status();
        ASSERT_EQ(events->size(), 2u);
        EXPECT_EQ(events->at(0).id.sequence, 101u);
        EXPECT_EQ(events->at(1).id.sequence, 102u);
        reads.clear();
        events = (*store)->read(1, {Range::Axis::Physical, {1000, 0}, {1001, 0}});
        ASSERT_TRUE(events.ok()) << events.status();
        EXPECT_TRUE(events->empty());
        EXPECT_TRUE(reads.files().empty());
    }
}

TEST(ManifestLog, MalformedPhysicalBoundsFailTheWholeLoad)
{
    for(const char* bounds: {R"({"min_lo":2,"max_hi":1,"unbounded":false})",
                             R"({"min_lo":18446744073709551615,"max_hi":18446744073709551615,"unbounded":false})",
                             R"({"min_lo":0.5,"max_hi":1,"unbounded":false})",
                             R"({"min_lo":0,"max_hi":1})"})
    {
        auto directory = TestDirectory();
        auto log = ManifestLog::Open(*directory, "primary");
        ASSERT_TRUE(log.ok());
        ASSERT_TRUE((*log)->append(Record(100, 200)).ok());
        log->reset();
        auto line = Bytes(*directory / "manifest/primary.log");
        line.erase(line.rfind('}'));
        {
            std::ofstream output(*directory / "manifest/primary.log");
            output << line << ",\"physical_bounds\":" << bounds << "}\n";
        }
        auto reader = FileTierStore::OpenReadOnly(*directory);
        EXPECT_TRUE(absl::IsUnavailable(reader.status())) << bounds;
    }
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

TEST(FileTierStore, ADeletedFileLeftOnDiskIsUnlinkedOnOpen)
{
    auto directory = TestDirectory();
    auto store = Open(*directory);
    ASSERT_TRUE(store.ok());
    auto record = (*store)->publish(contract::Window());
    ASSERT_TRUE(record.ok());
    store->reset();
    auto log = ManifestLog::Open(*directory, "primary");
    ASSERT_TRUE(log.ok());
    ASSERT_TRUE((*log)->rememberWatermark(1, {200, 0}).ok());
    record->state = ManifestState::Deleted;
    ASSERT_TRUE((*log)->append(*record).ok());
    log->reset();
    const auto manifest_bytes = Bytes(*directory / "manifest/primary.log");
    ASSERT_TRUE(fs::exists(*directory / record->file));
    auto reader = FileTierStore::OpenReadOnly(*directory);
    ASSERT_TRUE(reader.ok());
    EXPECT_TRUE(fs::exists(*directory / record->file));
    reader->reset();
    for(int restart = 0; restart < 2; ++restart)
    {
        store = Open(*directory);
        ASSERT_TRUE(store.ok()) << store.status();
        EXPECT_FALSE(fs::exists(*directory / record->file));
        EXPECT_EQ((*store)->manifest(1)->front().state, ManifestState::Deleted);
        EXPECT_TRUE((*store)->read(1, contract::WholeArchive())->empty());
        EXPECT_EQ((*store)->contiguousWatermark(1).value(), (Hlc{200, 0}));
        EXPECT_EQ(Bytes(*directory / "manifest/primary.log"), manifest_bytes);
        store->reset();
    }
}

TEST(FileTierStore, DeletedFileCleanupLeavesPublishedAndAdoptedFilesUntouched)
{
    auto directory = TestDirectory();
    auto store = Open(*directory);
    ASSERT_TRUE(store.ok());
    auto deleted = (*store)->publish(contract::Window());
    auto published = (*store)->publish(contract::Window(200, 300));
    ASSERT_TRUE(deleted.ok());
    ASSERT_TRUE(published.ok());
    auto orphan_writer = Open(*directory, "orphan-writer");
    ASSERT_TRUE(orphan_writer.ok());
    auto orphan = (*orphan_writer)->publish(contract::Window(300, 400));
    ASSERT_TRUE(orphan.ok());
    orphan_writer->reset();
    fs::resize_file(*directory / "manifest/orphan-writer.log", 0);
    const auto published_bytes = Bytes(*directory / published->file);
    const auto orphan_bytes = Bytes(*directory / orphan->file);
    store->reset();
    auto log = ManifestLog::Open(*directory, "primary");
    ASSERT_TRUE(log.ok());
    ASSERT_TRUE((*log)->rememberWatermark(1, {300, 0}).ok());
    deleted->state = ManifestState::Deleted;
    ASSERT_TRUE((*log)->append(*deleted).ok());
    log->reset();
    store = Open(*directory);
    ASSERT_TRUE(store.ok()) << store.status();
    EXPECT_FALSE(fs::exists(*directory / deleted->file));
    EXPECT_EQ(Bytes(*directory / published->file), published_bytes);
    EXPECT_EQ(Bytes(*directory / orphan->file), orphan_bytes);
    EXPECT_EQ((*store)->read(1, contract::WholeArchive())->size(), 2u);
    EXPECT_EQ((*store)->contiguousWatermark(1).value(), (Hlc{400, 0}));
    auto records = (*store)->manifest(1);
    ASSERT_TRUE(records.ok());
    EXPECT_EQ(std::count_if(records->begin(),
                            records->end(),
                            [](const auto& record) { return record.state == ManifestState::Published; }),
              2);
}

TEST(FileTierStore, FailedOpenUnlinkRemainsPendingAcrossCompaction)
{
    auto directory = TestDirectory();
    bool fail = true;
    int attempts = 0;
    const auto unlink = [&](const fs::path& path)
    {
        ++attempts;
        if(fail)
        {
            errno = EACCES;
            return -1;
        }
        return ::unlink(path.c_str());
    };
    auto store =
            FileTierStore::Open(*directory, "primary", {{1, {100, 0}}}, std::make_shared<ProtoChunkCodec>(), unlink);
    ASSERT_TRUE(store.ok());
    auto deleted = (*store)->publish(contract::Window());
    ASSERT_TRUE(deleted.ok());
    EXPECT_FALSE((*store)->eraseFile(deleted->file).ok());
    store->reset();
    store = FileTierStore::Open(*directory, "primary", {{1, {100, 0}}}, std::make_shared<ProtoChunkCodec>(), unlink);
    ASSERT_TRUE(store.ok()) << store.status();
    EXPECT_EQ(attempts, 2);
    EXPECT_TRUE(fs::exists(*directory / deleted->file));
    EXPECT_TRUE((*store)->hasPendingUnlinks(1).value());
    EXPECT_EQ((*store)->contiguousWatermark(1).value(), (Hlc{200, 0}));
    EXPECT_TRUE((*store)->read(1, contract::WholeArchive())->empty());
    auto published = (*store)->publish(contract::Window(200, 300));
    ASSERT_TRUE(published.ok());
    const auto published_bytes = Bytes(*directory / published->file);
    ASSERT_TRUE((*store)->compact().ok());
    const auto log_bytes = Bytes(*directory / "manifest/primary.log");
    const auto snapshot_bytes = Bytes(*directory / "manifest/primary.snap");
    EXPECT_FALSE((*store)->eraseFile(deleted->file).ok());
    EXPECT_FALSE((*store)->retryDeletedFiles().ok());
    EXPECT_TRUE((*store)->hasPendingUnlinks(1).value());
    fail = false;
    ASSERT_TRUE((*store)->retryDeletedFiles().ok());
    ASSERT_TRUE((*store)->eraseFile(deleted->file).ok());
    EXPECT_FALSE((*store)->hasPendingUnlinks(1).value());
    ASSERT_TRUE((*store)->retryDeletedFiles().ok());
    EXPECT_FALSE(fs::exists(*directory / deleted->file));
    EXPECT_EQ(Bytes(*directory / published->file), published_bytes);
    EXPECT_EQ((*store)->contiguousWatermark(1).value(), (Hlc{300, 0}));
    EXPECT_EQ(Bytes(*directory / "manifest/primary.log"), log_bytes);
    EXPECT_EQ(Bytes(*directory / "manifest/primary.snap"), snapshot_bytes);
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

TEST(ManifestLog, SyncSeesAnotherWritersAppendsAndSurvivesItsCompaction)
{
    auto directory = TestDirectory();
    auto mine = ManifestLog::Open(*directory, "mine");
    auto theirs = ManifestLog::Open(*directory, "theirs");
    ASSERT_TRUE(mine.ok());
    ASSERT_TRUE(theirs.ok());
    auto count = [&]
    {
        auto index = (*mine)->sync();
        EXPECT_TRUE(index.ok());
        return index.ok() ? (*index)->records.size() : size_t{0};
    };
    ASSERT_TRUE((*mine)->append(Record(100, 200)).ok());
    EXPECT_EQ(count(), 1u);
    ASSERT_TRUE((*theirs)->append(Record(200, 300)).ok());
    EXPECT_EQ(count(), 2u);
    ASSERT_TRUE((*theirs)->append(Record(300, 400)).ok());
    ASSERT_TRUE((*theirs)->compact().ok());
    EXPECT_EQ(count(), 3u);
    ASSERT_TRUE((*theirs)->append(Record(400, 500)).ok());
    ASSERT_TRUE((*mine)->compact().ok());
    EXPECT_EQ(count(), 4u);
    EXPECT_EQ((*mine)->sync().value()->by_story.at(1).size(), 4u);
}

TEST(ManifestLog, PollDetectsAppendWithoutPathStat)
{
    auto directory = TestDirectory();
    auto writer = ManifestLog::Open(*directory, "primary");
    ASSERT_TRUE(writer.ok());
    ASSERT_TRUE((*writer)->append(Record(100, 200)).ok());
    std::mutex mutex;
    std::map<std::string, struct stat> cached;
    auto lagging = [&](const fs::path& path, struct stat& info)
    {
        std::lock_guard lock(mutex);
        auto [it, inserted] = cached.try_emplace(path.string());
        if(inserted && ::stat(path.c_str(), &it->second) != 0)
        {
            const int error = errno;
            cached.erase(it);
            errno = error;
            return -1;
        }
        info = it->second;
        return 0;
    };
    auto reader = ManifestLog::OpenReadOnly(*directory, lagging);
    auto index = reader->sync();
    ASSERT_TRUE(index.ok()) << index.status();
    EXPECT_EQ((*index)->records.size(), 1u);
    for(const int64_t start: {200, 300})
    {
        ASSERT_TRUE((*writer)->append(Record(start, start + 100)).ok());
        index = reader->sync();
        ASSERT_TRUE(index.ok()) << index.status();
        ASSERT_FALSE((*index)->records.empty());
        EXPECT_EQ((*index)->records.back().start, (Hlc{start, 0}));
    }
    EXPECT_EQ(reader->sync().value()->records.size(), 3u);
}

TEST(ManifestLog, TruncatedAndRegrownLogIsReadFromTheStart)
{
    auto directory = TestDirectory();
    auto writer = ManifestLog::Open(*directory, "primary");
    ASSERT_TRUE(writer.ok());
    for(const int64_t start: {100, 200, 300}) ASSERT_TRUE((*writer)->append(Record(start, start + 100)).ok());
    auto reader = ManifestLog::OpenReadOnly(*directory);
    auto index = reader->sync();
    ASSERT_TRUE(index.ok()) << index.status();
    ASSERT_EQ((*index)->records.size(), 3u);
    const auto log = *directory / "manifest/primary.log";
    const auto consumed = fs::file_size(log);
    struct stat before
    {
    };
    ASSERT_EQ(::stat(log.c_str(), &before), 0);
    fs::resize_file(log, 0);
    for(const int64_t start: {10000000, 20000000, 30000000})
        ASSERT_TRUE((*writer)->append(Record(start, start + 100)).ok());
    ASSERT_GT(fs::file_size(log), consumed);
    struct stat after
    {
    };
    ASSERT_EQ(::stat(log.c_str(), &after), 0);
    ASSERT_EQ(after.st_ino, before.st_ino);
    index = reader->sync();
    ASSERT_TRUE(index.ok()) << index.status();
    std::vector<int64_t> starts;
    for(const auto& record: (*index)->records) starts.push_back(record.start.physical_ns);
    EXPECT_EQ(starts, (std::vector<int64_t>{10000000, 20000000, 30000000}));
    ASSERT_TRUE((*writer)->append(Record(40000000, 40000100)).ok());
    EXPECT_EQ(reader->sync().value()->records.size(), 4u);
}

TEST(ManifestLog, FailedRefreshKeepsThePreviousIndex)
{
    auto directory = TestDirectory();
    auto writer = ManifestLog::Open(*directory, "primary");
    ASSERT_TRUE(writer.ok());
    ASSERT_TRUE((*writer)->append(Record(100, 200)).ok());
    auto reader = ManifestLog::OpenReadOnly(*directory);
    auto store = FileTierStore::OpenReadOnly(*directory, std::chrono::hours(1));
    ASSERT_TRUE(store.ok());
    auto index = reader->sync();
    ASSERT_TRUE(index.ok());
    const auto generation = (*index)->generation;
    const auto log = *directory / "manifest/primary.log";
    const auto good = Bytes(log);
    std::ofstream(log, std::ios::app) << "{\"chunk\":1}\n";
    for(int attempt = 0; attempt < 2; ++attempt)
    {
        EXPECT_FALSE(reader->sync().ok());
        EXPECT_EQ(reader->current()->records.size(), 1u);
        EXPECT_EQ(reader->current()->generation, generation);
        EXPECT_FALSE((*store)->refreshNow().ok());
        auto records = (*store)->manifest(1);
        ASSERT_TRUE(records.ok()) << records.status();
        EXPECT_EQ(records->size(), 1u);
    }
    std::ofstream(log, std::ios::trunc) << good;
    ASSERT_TRUE((*writer)->append(Record(200, 300)).ok());
    index = reader->sync();
    ASSERT_TRUE(index.ok()) << index.status();
    EXPECT_EQ((*index)->records.size(), 2u);
    ASSERT_TRUE((*store)->refreshNow().ok());
    EXPECT_EQ((*store)->manifest(1)->size(), 2u);
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

TEST(HDF5ChunkCodec, RejectsOversizedFileBeforeAllocating)
{
    auto directory = TestDirectory();
    const auto path = *directory / "oversized.h5";
    std::ofstream(path).close();
    fs::resize_file(path, 512 * 1024 * 1024 + 1);
    auto read = HDF5ChunkCodec().read(path);
    EXPECT_EQ(read.status().code(), absl::StatusCode::kUnavailable);
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

TEST(FileTierStore, ATombstoneIsIdempotentSurvivesCompactionAndNeedsNoKnownStory)
{
    auto directory = TestDirectory();
    auto store = Open(*directory);
    ASSERT_TRUE(store.ok());
    ASSERT_TRUE((*store)->publish(contract::Window()).ok());
    EXPECT_EQ((*store)->tombstoned(1).value(), false);
    EXPECT_EQ((*store)->liveStories().value(), (std::vector<StoryId>{1}));
    ASSERT_TRUE((*store)->tombstone(1).ok());
    ASSERT_TRUE((*store)->tombstone(1).ok());
    ASSERT_TRUE((*store)->tombstone(7).ok());
    EXPECT_FALSE((*store)->tombstone(0).ok());
    std::ifstream before(*directory / "manifest/primary.log");
    EXPECT_EQ(std::count(std::istreambuf_iterator<char>(before), std::istreambuf_iterator<char>(), '\n'), 3)
            << "one record and two tombstone lines";
    ASSERT_TRUE((*store)->compact().ok());
    store->reset();
    store = Open(*directory);
    ASSERT_TRUE(store.ok());
    EXPECT_EQ((*store)->tombstoned(1).value(), true);
    EXPECT_EQ((*store)->tombstoned(7).value(), true);
    EXPECT_EQ((*store)->tombstoned(2).value(), false);
    EXPECT_EQ((*store)->tombstonedStories().value(), (std::vector<StoryId>{1, 7}));
    EXPECT_TRUE((*store)->liveStories().value().empty());
    EXPECT_EQ((*store)->publish(contract::Window(200, 300)).status().code(), absl::StatusCode::kFailedPrecondition);
    auto reader = FileTierStore::OpenReadOnly(*directory);
    ASSERT_TRUE(reader.ok());
    EXPECT_EQ((*reader)->tombstoned(1).value(), true);
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

namespace chronolog
{
namespace
{
const Range kAll{Range::Axis::Hlc, {0, 0}, {int64_t{1} << 40, 0}};

// Window `index` is [100 + 50 * index, 150 + 50 * index) with every Event field set and some unbounded readings.
Chunk Rich(int index, int events = 2)
{
    const int64_t start = 100 + 50 * index;
    Chunk chunk{"w" + std::to_string(index), 1, {start, 0}, {start + 50, 0}, {}, false, false};
    for(int k = 0; k < events; ++k)
    {
        Event event;
        event.id = {1, static_cast<uint64_t>(2 + k % 2), 3, static_cast<uint64_t>(1000 * index + k + 1)};
        event.hlc = {start + k, static_cast<uint32_t>(k)};
        event.physical.physical_ns = start * 10 + k;
        event.physical.status = k % 2 ? ClockStatus::Unsynced : ClockStatus::Synced;
        if(!(k % 2))
            event.physical.uncertainty_ns = 5;
        event.durability = k % 2 ? Durability::Accepted : Durability::Durable;
        event.envelope.content_type = "application/test";
        event.envelope.payload = std::string(64, static_cast<char>('a' + k)) + std::to_string(index);
        event.envelope.trace_id = std::string(16, 't');
        event.envelope.span_id = std::string(8, 's');
        event.envelope.attributes = {{"window", std::to_string(index)}, {"k", std::to_string(k)}};
        chunk.events.push_back(event);
    }
    return chunk;
}

CompactionPolicy Eager(size_t min_files = 2)
{
    CompactionPolicy policy;
    policy.min_files = min_files;
    policy.min_age = std::chrono::seconds(0);
    policy.io_bytes_per_sec = policy.io_burst_bytes = uint64_t{1} << 30;
    return policy;
}

absl::StatusOr<std::unique_ptr<FileTierStore>>
OpenStore(const fs::path& root,
          FileTierStore::Hooks hooks = {},
          std::shared_ptr<const ChunkCodec> codec = std::make_shared<HDF5ChunkCodec>(),
          std::string writer = "primary",
          FileTierStore::LoadFile load = {})
{
    return FileTierStore::Open(root, writer, {{1, {100, 0}}}, std::move(codec), {}, std::move(load), 2, {}, hooks);
}

bool SameEvents(const std::vector<Event>& a, const std::vector<Event>& b)
{
    return std::equal(
            a.begin(),
            a.end(),
            b.begin(),
            b.end(),
            [](const Event& x, const Event& y)
            {
                return x.id == y.id && x.hlc == y.hlc && x.durability == y.durability &&
                       x.physical.physical_ns == y.physical.physical_ns && x.physical.status == y.physical.status &&
                       x.physical.uncertainty_ns == y.physical.uncertainty_ns &&
                       x.envelope.content_type == y.envelope.content_type && x.envelope.payload == y.envelope.payload &&
                       x.envelope.trace_id == y.envelope.trace_id && x.envelope.span_id == y.envelope.span_id &&
                       x.envelope.attributes == y.envelope.attributes;
            });
}

std::vector<ManifestRecord> PublishWindows(FileTierStore& store, int count, int first = 0)
{
    std::vector<ManifestRecord> records;
    for(int i = first; i < first + count; ++i)
    {
        auto record = store.publish(Rich(i));
        EXPECT_TRUE(record.ok()) << record.status();
        if(record.ok())
            records.push_back(*record);
    }
    return records;
}

// Compaction outputs and temporaries in the story directory.
std::vector<std::string> CompactionFiles(const fs::path& root)
{
    std::vector<std::string> files;
    for(const auto& file: fs::directory_iterator(root / "1"))
        if(file.path().filename().string().starts_with("compact-") ||
           file.path().filename().string().starts_with(".compact-"))
            files.push_back(file.path().filename().string());
    return files;
}

std::vector<ManifestRecord> Effective(FileTierStore& store)
{
    auto records = store.manifest(1);
    EXPECT_TRUE(records.ok()) << records.status();
    return records.ok() ? *records : std::vector<ManifestRecord>{};
}

FileTierStore::Hooks CrashAt(std::string step)
{
    FileTierStore::Hooks hooks;
    hooks.compaction_step = [step](std::string_view at)
    { return at == step ? absl::AbortedError("injected crash at " + step) : absl::OkStatus(); };
    return hooks;
}
} // namespace

TEST(FileTierStore, PayloadBitFlipFailsReadAndRecoveryMarksLost)
{
    auto directory = TestDirectory();
    auto store = OpenStore(*directory, {}, std::make_shared<ProtoChunkCodec>());
    ASSERT_TRUE(store.ok());
    auto chunk = Rich(0);
    auto record = (*store)->publish(chunk);
    ASSERT_TRUE(record.ok());
    auto bytes = Bytes(*directory / record->file);
    const auto at = bytes.find(chunk.events.front().envelope.payload);
    ASSERT_NE(at, std::string::npos);
    bytes[at] ^= 1;
    std::ofstream(*directory / record->file, std::ios::binary) << bytes;
    ASSERT_TRUE(ReadChunkFile(*directory / record->file).ok());
    EXPECT_EQ((*store)->read(1, kAll).status().code(), absl::StatusCode::kUnavailable);
    EXPECT_EQ((*store)->readRecord(*record, kAll).status().code(), absl::StatusCode::kUnavailable);
    EXPECT_EQ((*store)->publish(chunk).status().code(), absl::StatusCode::kUnavailable);
    store->reset();
    store = OpenStore(*directory, {}, std::make_shared<ProtoChunkCodec>());
    ASSERT_TRUE(store.ok());
    const auto records = Effective(**store);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records.front().state, ManifestState::Lost);
    EXPECT_TRUE((*store)->incomplete(1, kAll).value());
}

TEST(FileTierStore, ReadOnlyCorruptFileIsUnavailableAndNeverLost)
{
    auto directory = TestDirectory();
    auto owner = OpenStore(*directory, {}, std::make_shared<ProtoChunkCodec>());
    ASSERT_TRUE(owner.ok());
    auto record = (*owner)->publish(Rich(0));
    ASSERT_TRUE(record.ok());
    owner->reset();
    const auto manifest_before = Bytes(*directory / "manifest/primary.log");
    auto bytes = Bytes(*directory / record->file);
    bytes.push_back('x');
    std::ofstream(*directory / record->file, std::ios::binary) << bytes;
    auto player = FileTierStore::OpenReadOnly(*directory);
    ASSERT_TRUE(player.ok());
    EXPECT_EQ((*player)->read(1, kAll).status().code(), absl::StatusCode::kUnavailable);
    EXPECT_EQ((*player)->readRecord(*record, kAll).status().code(), absl::StatusCode::kUnavailable);
    EXPECT_EQ(Effective(**player).front().state, ManifestState::Published);
    player->reset();
    auto foreign = FileTierStore::Open(*directory, "peer", {{1, {100, 0}}});
    ASSERT_TRUE(foreign.ok());
    EXPECT_EQ(Effective(**foreign).front().state, ManifestState::Published);
    EXPECT_EQ(Bytes(*directory / "manifest/primary.log"), manifest_before);
}

TEST(ManifestLog, ChecksumKeysAreIgnoredByDecode)
{
    auto directory = TestDirectory();
    auto log = ManifestLog::Open(*directory, "primary");
    ASSERT_TRUE(log.ok());
    auto record = Record(100, 150);
    ASSERT_TRUE((*log)->append(record).ok());
    auto old = nlohmann::json::parse(Bytes((*log)->logPath()));
    auto added = old;
    added["bytes"] = uint64_t{100};
    added["crc32c"] = uint32_t{123};
    added["future_key"] = "ignored";
    std::ofstream((*log)->logPath()) << old.dump() << '\n' << added.dump() << '\n';
    auto reader = ManifestLog::OpenReadOnly(*directory);
    auto index = reader->load();
    ASSERT_TRUE(index.ok()) << index.status();
    ASSERT_EQ(index->records.size(), 2u);
    EXPECT_EQ(index->records[0].file, index->records[1].file);
    EXPECT_EQ(index->records[0].event_count, index->records[1].event_count);
    EXPECT_EQ(index->checksums.at(record.file).bytes, 100u);
    EXPECT_EQ(index->checksums.at(record.file).crc32c, 123u);
}

TEST(FileTierStore, CompactionOutputCarriesItsChecksum)
{
    for(const auto& codec: std::vector<std::shared_ptr<const ChunkCodec>>{std::make_shared<ProtoChunkCodec>(),
                                                                          std::make_shared<HDF5ChunkCodec>()})
    {
        auto directory = TestDirectory();
        auto store = OpenStore(*directory, {}, codec);
        ASSERT_TRUE(store.ok());
        const auto inputs = PublishWindows(**store, 3);
        auto result = (*store)->compactOnce(Eager());
        ASSERT_TRUE(result.ok()) << result.status();
        ASSERT_EQ(result->inputs, 3u);
        auto reader = ManifestLog::OpenReadOnly(*directory);
        auto index = reader->load();
        ASSERT_TRUE(index.ok());
        ASSERT_TRUE(index->checksums.contains(result->output));
        const auto expected = index->checksums.at(result->output);
        const auto bytes = Bytes(*directory / result->output);
        EXPECT_EQ(expected.bytes, bytes.size());
        EXPECT_EQ(expected.crc32c, static_cast<uint32_t>(absl::ComputeCrc32c(bytes)));
        ASSERT_TRUE((*store)->compact().ok());
        store->reset();
        store = OpenStore(*directory, {}, codec);
        ASSERT_TRUE(store.ok());
        EXPECT_TRUE((*store)->read(1, kAll).ok());
        auto corrupted = bytes;
        corrupted.back() ^= 1;
        std::ofstream(*directory / result->output, std::ios::binary) << corrupted;
        EXPECT_EQ((*store)->readRecord(inputs.front(), kAll).status().code(), absl::StatusCode::kUnavailable);
        EXPECT_EQ((*store)->read(1, kAll).status().code(), absl::StatusCode::kUnavailable);
        store->reset();
        store = OpenStore(*directory, {}, codec);
        ASSERT_TRUE(store.ok());
        EXPECT_EQ(Effective(**store).front().state, ManifestState::Lost);
    }
}

TEST(FileTierStore, CompactionCoverageAndEveryEventFieldArePreserved)
{
    for(const auto& codec: std::vector<std::shared_ptr<const ChunkCodec>>{std::make_shared<HDF5ChunkCodec>(),
                                                                          std::make_shared<ProtoChunkCodec>()})
    {
        SCOPED_TRACE(codec->extension());
        auto directory = TestDirectory();
        auto store = OpenStore(*directory, {}, codec);
        ASSERT_TRUE(store.ok());
        const auto inputs = PublishWindows(**store, 6);
        const auto before = (*store)->read(1, kAll);
        ASSERT_TRUE(before.ok());
        ASSERT_EQ(before->size(), 12u);
        const auto w = (*store)->contiguousWatermark(1).value();
        EXPECT_EQ(w, (Hlc{400, 0}));
        auto result = (*store)->compactOnce(Eager());
        ASSERT_TRUE(result.ok()) << result.status();
        EXPECT_EQ(result->inputs, 6u);
        const auto check = [&](FileTierStore& sut)
        {
            const auto records = Effective(sut);
            ASSERT_EQ(records.size(), 1u);
            EXPECT_EQ(records[0].file, result->output);
            EXPECT_EQ(records[0].state, ManifestState::Published);
            EXPECT_EQ(records[0].manifest_writer, "primary");
            EXPECT_EQ(records[0].start, (Hlc{100, 0}));
            EXPECT_EQ(records[0].end, (Hlc{400, 0}));
            EXPECT_EQ(records[0].event_count, 12u);
            EXPECT_EQ(fs::path(records[0].file).extension(), codec->extension());
            for(const auto& input: inputs) EXPECT_FALSE(fs::exists(*directory / input.file)) << input.file;
            auto after = sut.read(1, kAll);
            ASSERT_TRUE(after.ok()) << after.status();
            EXPECT_TRUE(SameEvents(*after, *before));
            EXPECT_EQ(sut.contiguousWatermark(1).value(), w);
            EXPECT_FALSE(sut.incomplete(1, kAll).value());
        };
        check(**store);
        // B2 bounds of the output cover every event with the saturating interval convention.
        int64_t min_lo = std::numeric_limits<int64_t>::max(), max_hi = std::numeric_limits<int64_t>::min();
        for(const auto& event: *before)
        {
            const int64_t u = event.physical.status == ClockStatus::Synced ? 5 : 0;
            min_lo = std::min(min_lo, event.physical.physical_ns - u);
            max_hi = std::max(max_hi, event.physical.physical_ns + u);
        }
        auto index = ManifestLog::OpenReadOnly(*directory)->load();
        ASSERT_TRUE(index.ok());
        const auto& bounds = index->physical_bounds.at(result->output);
        EXPECT_EQ(bounds.bounds.min_lo, min_lo);
        EXPECT_EQ(bounds.bounds.max_hi, max_hi);
        EXPECT_TRUE(bounds.bounds.unbounded);
        EXPECT_EQ(bounds.event_count, 12u);
        // The switch survives the snapshot and log truncation and a restart.
        ASSERT_TRUE((*store)->compact().ok());
        store->reset();
        auto reopened = OpenStore(*directory, {}, codec);
        ASSERT_TRUE(reopened.ok()) << reopened.status();
        check(**reopened);
        auto again = (*reopened)->compactOnce(Eager());
        ASSERT_TRUE(again.ok());
        EXPECT_EQ(again->inputs, 0u);
    }
}

TEST(FileTierStore, CompactionRefusesGapEmptyLostExemptForeignAndAboveW)
{
    auto directory = TestDirectory();
    {
        auto peer = OpenStore(*directory, {}, std::make_shared<HDF5ChunkCodec>(), "secondary");
        ASSERT_TRUE(peer.ok());
        PublishWindows(**peer, 2);
    }
    auto store = OpenStore(*directory);
    ASSERT_TRUE(store.ok());
    PublishWindows(**store, 2);
    ASSERT_TRUE((*store)->publish(Rich(2, 0)).ok());
    const auto lost = PublishWindows(**store, 2, 3);
    auto exempt = Rich(5);
    exempt.exempt = true;
    ASSERT_TRUE((*store)->publish(exempt).ok());
    PublishWindows(**store, 1, 6);
    PublishWindows(**store, 2, 8);
    store->reset();
    ASSERT_TRUE(fs::remove(*directory / lost[0].file));
    store = OpenStore(*directory);
    ASSERT_TRUE(store.ok());
    // The exempt window stops W (I13.4); the Lost window keeps the floor recorded when it was found.
    const auto w = (*store)->contiguousWatermark(1).value();
    EXPECT_EQ(w, (Hlc{350, 0}));
    auto result = (*store)->compactOnce(Eager());
    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(result->inputs, 2u);
    auto again = (*store)->compactOnce(Eager());
    ASSERT_TRUE(again.ok());
    EXPECT_EQ(again->inputs, 0u);
    size_t outputs = 0, foreign = 0;
    for(const auto& record: Effective(**store))
    {
        if(record.file == result->output)
        {
            ++outputs;
            EXPECT_EQ(record.start, (Hlc{100, 0}));
            EXPECT_EQ(record.end, (Hlc{200, 0}));
        }
        if(record.manifest_writer == "secondary")
        {
            ++foreign;
            EXPECT_EQ(record.state, ManifestState::Published);
            EXPECT_TRUE(fs::exists(*directory / record.file));
        }
        if(record.file == lost[0].file)
        {
            EXPECT_EQ(record.state, ManifestState::Lost);
        }
    }
    EXPECT_EQ(outputs, 1u);
    EXPECT_EQ(foreign, 2u);
    EXPECT_EQ((*store)->contiguousWatermark(1).value(), w);
}

TEST(FileTierStore, CompactionOutputHonorsEveryBound)
{
    auto directory = TestDirectory();
    auto store = OpenStore(*directory);
    ASSERT_TRUE(store.ok());
    PublishWindows(**store, 12);
    for(const auto& [policy, empty]: std::vector<std::pair<CompactionPolicy, bool>>{{[]
                                                                                     {
                                                                                         auto p = Eager();
                                                                                         p.small_file_bytes = 1;
                                                                                         return p;
                                                                                     }(),
                                                                                     true},
                                                                                    {[]
                                                                                     {
                                                                                         auto p = Eager();
                                                                                         p.min_age =
                                                                                                 std::chrono::hours(1);
                                                                                         return p;
                                                                                     }(),
                                                                                     true},
                                                                                    {[]
                                                                                     {
                                                                                         auto p = Eager();
                                                                                         p.max_output_bytes = 1;
                                                                                         return p;
                                                                                     }(),
                                                                                     true}})
    {
        auto result = (*store)->compactOnce(policy);
        ASSERT_TRUE(result.ok()) << result.status();
        EXPECT_EQ(result->inputs == 0, empty);
    }
    auto files = Eager();
    files.max_files = 4;
    auto events = Eager();
    events.max_events = 6;
    auto span = Eager();
    span.max_span_ns = 120;
    const std::vector<std::tuple<CompactionPolicy, size_t, Hlc, Hlc>> expected{{files, 4, {100, 0}, {300, 0}},
                                                                               {events, 3, {300, 0}, {450, 0}},
                                                                               {span, 2, {450, 0}, {550, 0}}};
    for(const auto& [policy, inputs, start, end]: expected)
    {
        auto result = (*store)->compactOnce(policy);
        ASSERT_TRUE(result.ok()) << result.status();
        ASSERT_EQ(result->inputs, inputs);
        for(const auto& record: Effective(**store))
            if(record.file == result->output)
            {
                EXPECT_EQ(record.start, start);
                EXPECT_EQ(record.end, end);
                EXPECT_EQ(record.event_count, 2 * inputs);
            }
    }
    // An output is never an input again, and the rest stays below min_files only if the bounds forbid it.
    auto rest = (*store)->compactOnce(Eager());
    ASSERT_TRUE(rest.ok());
    EXPECT_EQ(rest->inputs, 3u);
    auto none = (*store)->compactOnce(Eager());
    ASSERT_TRUE(none.ok());
    EXPECT_EQ(none->inputs, 0u);
    EXPECT_EQ((*store)->contiguousWatermark(1).value(), (Hlc{700, 0}));
}

TEST(FileTierStore, CompactionCrashBeforeOrAfterOutputLinkNeverAdoptsTheOutput)
{
    for(const std::string step: {"link", "switch"})
    {
        SCOPED_TRACE(step);
        auto directory = TestDirectory();
        auto store = OpenStore(*directory, CrashAt(step));
        ASSERT_TRUE(store.ok());
        const auto inputs = PublishWindows(**store, 4);
        const auto before = (*store)->read(1, kAll).value();
        EXPECT_FALSE((*store)->compactOnce(Eager()).ok());
        const auto left = CompactionFiles(*directory);
        ASSERT_EQ(left.size(), 1u);
        EXPECT_EQ(left[0].starts_with(".compact-"), step == "link");
        EXPECT_EQ(Effective(**store).size(), 4u);
        store->reset();
        auto reopened = OpenStore(*directory);
        ASSERT_TRUE(reopened.ok()) << reopened.status();
        EXPECT_TRUE(CompactionFiles(*directory).empty());
        const auto records = Effective(**reopened);
        ASSERT_EQ(records.size(), 4u);
        for(const auto& record: records) EXPECT_EQ(record.state, ManifestState::Published);
        EXPECT_TRUE(SameEvents((*reopened)->read(1, kAll).value(), before));
        EXPECT_FALSE((*reopened)->incomplete(1, kAll).value());
    }
}

TEST(FileTierStore, CompactionCrashAfterSwitchCleansInputsOnlyAfterTheOwnLogFsync)
{
    auto directory = TestDirectory();
    auto store = OpenStore(*directory, CrashAt("cleanup"));
    ASSERT_TRUE(store.ok());
    const auto inputs = PublishWindows(**store, 4);
    const auto before = (*store)->read(1, kAll).value();
    const auto w = (*store)->contiguousWatermark(1).value();
    EXPECT_FALSE((*store)->compactOnce(Eager()).ok());
    ASSERT_EQ(Effective(**store).size(), 1u);
    for(const auto& input: inputs) EXPECT_TRUE(fs::exists(*directory / input.file));
    EXPECT_TRUE(SameEvents((*store)->read(1, kAll).value(), before));
    store->reset();
    // The restarted writer sees the switch, but cannot fsync its own log: it must not unlink the inputs.
    FileTierStore::Hooks failing;
    failing.manifest_sync = [](int)
    {
        errno = EIO;
        return -1;
    };
    store = OpenStore(*directory, failing);
    ASSERT_TRUE(store.ok()) << store.status();
    (void)(*store)->retryDeletedFiles();
    for(const auto& input: inputs) EXPECT_TRUE(fs::exists(*directory / input.file));
    EXPECT_TRUE(absl::IsFailedPrecondition((*store)->compactOnce(Eager()).status()));
    store->reset();
    store = OpenStore(*directory);
    ASSERT_TRUE(store.ok()) << store.status();
    for(const auto& input: inputs) EXPECT_FALSE(fs::exists(*directory / input.file));
    const auto records = Effective(**store);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].state, ManifestState::Published);
    EXPECT_TRUE(SameEvents((*store)->read(1, kAll).value(), before));
    EXPECT_EQ((*store)->contiguousWatermark(1).value(), w);
    EXPECT_FALSE((*store)->incomplete(1, kAll).value());
}

TEST(FileTierStore, CompactionFailedSwitchFsyncStopsCompactionAndBothRecoveryImagesAreSafe)
{
    for(const bool line_survives: {true, false})
    {
        SCOPED_TRACE(line_survives);
        auto directory = TestDirectory();
        auto fail = std::make_shared<std::atomic<bool>>(false);
        FileTierStore::Hooks hooks;
        hooks.manifest_sync = [fail](int fd)
        {
            if(fail->load())
            {
                errno = EIO;
                return -1;
            }
            return ::fsync(fd);
        };
        auto store = OpenStore(*directory, hooks);
        ASSERT_TRUE(store.ok());
        const auto inputs = PublishWindows(**store, 4);
        const auto before = (*store)->read(1, kAll).value();
        *fail = true;
        EXPECT_FALSE((*store)->compactOnce(Eager()).ok());
        *fail = false;
        ASSERT_EQ(CompactionFiles(*directory).size(), 1u);
        EXPECT_TRUE(absl::IsFailedPrecondition((*store)->compactOnce(Eager()).status()));
        (void)(*store)->retryDeletedFiles();
        for(const auto& input: inputs) EXPECT_TRUE(fs::exists(*directory / input.file));
        EXPECT_TRUE(SameEvents((*store)->read(1, kAll).value(), before));
        store->reset();
        if(!line_survives)
        {
            // The complete but undurable line is lost with the page cache.
            auto log = Bytes(*directory / "manifest/primary.log");
            ASSERT_EQ(log.back(), '\n');
            log.pop_back();
            log.resize(log.rfind('\n') + 1);
            std::ofstream(*directory / "manifest/primary.log", std::ios::binary | std::ios::trunc) << log;
        }
        store = OpenStore(*directory);
        ASSERT_TRUE(store.ok()) << store.status();
        const auto records = Effective(**store);
        ASSERT_EQ(records.size(), line_survives ? 1u : 4u);
        for(const auto& input: inputs) EXPECT_EQ(fs::exists(*directory / input.file), !line_survives);
        EXPECT_EQ(CompactionFiles(*directory).size(), line_survives ? 1u : 0u);
        EXPECT_TRUE(SameEvents((*store)->read(1, kAll).value(), before));
        EXPECT_FALSE((*store)->incomplete(1, kAll).value());
    }
}

TEST(FileTierStore, CompactionCleanupBelongsToTheCommittingWriterUntilATombstone)
{
    auto directory = TestDirectory();
    auto store = OpenStore(*directory, CrashAt("cleanup"));
    ASSERT_TRUE(store.ok());
    const auto inputs = PublishWindows(**store, 4);
    EXPECT_FALSE((*store)->compactOnce(Eager()).ok());
    auto peer = OpenStore(*directory, {}, std::make_shared<HDF5ChunkCodec>(), "secondary");
    ASSERT_TRUE(peer.ok()) << peer.status();
    EXPECT_TRUE((*peer)->retryDeletedFiles().ok());
    EXPECT_EQ((*peer)->compactOnce(Eager()).value().inputs, 0u);
    for(const auto& input: inputs) EXPECT_TRUE(fs::exists(*directory / input.file));
    EXPECT_FALSE((*peer)->hasPendingUnlinks(1).value());
    // In a tombstoned story any writer frees what the compacting writer left behind (I13.11).
    ASSERT_TRUE((*peer)->tombstone(1).ok());
    EXPECT_TRUE((*peer)->hasPendingUnlinks(1).value());
    EXPECT_TRUE((*peer)->retryDeletedFiles().ok());
    for(const auto& input: inputs) EXPECT_FALSE(fs::exists(*directory / input.file));
    EXPECT_FALSE((*peer)->hasPendingUnlinks(1).value());
}

TEST(FileTierStore, CompactionRecoveryKeepsAnUnreferencedOutputWhoseInputsAreMissing)
{
    auto directory = TestDirectory();
    auto store = OpenStore(*directory, CrashAt("switch"));
    ASSERT_TRUE(store.ok());
    const auto inputs = PublishWindows(**store, 4);
    EXPECT_FALSE((*store)->compactOnce(Eager()).ok());
    const auto output = CompactionFiles(*directory);
    ASSERT_EQ(output.size(), 1u);
    store->reset();
    ASSERT_TRUE(fs::remove(*directory / inputs[1].file));
    store = OpenStore(*directory);
    ASSERT_TRUE(store.ok()) << store.status();
    EXPECT_TRUE(fs::exists(*directory / "1" / output[0]));
    for(const auto& record: Effective(**store))
    {
        EXPECT_EQ(record.state, record.file == inputs[1].file ? ManifestState::Lost : ManifestState::Published);
    }
}

TEST(FileTierStore, CompactionRecoveryRollsBackACorruptOutputWhoseInputsAreIntact)
{
    auto directory = TestDirectory();
    auto store = OpenStore(*directory, CrashAt("cleanup"));
    ASSERT_TRUE(store.ok());
    const auto inputs = PublishWindows(**store, 4);
    const auto before = (*store)->read(1, kAll).value();
    const auto w = (*store)->contiguousWatermark(1).value();
    EXPECT_FALSE((*store)->compactOnce(Eager()).ok());
    const auto output = Effective(**store).at(0).file;
    store->reset();
    std::ofstream(*directory / output, std::ios::binary | std::ios::trunc) << "corrupt";
    store = OpenStore(*directory);
    ASSERT_TRUE(store.ok()) << store.status();
    const auto records = Effective(**store);
    ASSERT_EQ(records.size(), 4u);
    for(const auto& record: records) EXPECT_EQ(record.state, ManifestState::Published);
    EXPECT_FALSE(fs::exists(*directory / output));
    EXPECT_NE(Bytes(*directory / "manifest/primary.log").find("compact_rollback_v1"), std::string::npos);
    EXPECT_TRUE(SameEvents((*store)->read(1, kAll).value(), before));
    EXPECT_EQ((*store)->contiguousWatermark(1).value(), w);
    EXPECT_FALSE((*store)->incomplete(1, kAll).value());
    store->reset();
    store = OpenStore(*directory);
    ASSERT_TRUE(store.ok()) << store.status();
    EXPECT_EQ(Effective(**store).size(), 4u);
}

TEST(FileTierStore, PeerRecoveryDuringCompactionCleanupWritesNoLost)
{
    auto directory = TestDirectory();
    auto store = OpenStore(*directory);
    ASSERT_TRUE(store.ok());
    const auto inputs = PublishWindows(**store, 4);
    const auto before = (*store)->read(1, kAll).value();
    std::atomic<int> compactions{0};
    // The peer's recovery read its index before the switch; the switch and the unlinks land while it validates.
    auto load = [&](const fs::path& path)
    {
        if(path.filename() == fs::path(inputs[0].file).filename() && compactions++ == 0)
        {
            auto result = (*store)->compactOnce(Eager());
            EXPECT_TRUE(result.ok()) << result.status();
            EXPECT_EQ(result.ok() ? result->inputs : 0, 4u);
        }
        return LoadChunkFile(path);
    };
    auto peer = OpenStore(*directory, {}, std::make_shared<HDF5ChunkCodec>(), "secondary", load);
    ASSERT_TRUE(peer.ok()) << peer.status();
    EXPECT_EQ(compactions.load(), 1);
    const auto records = Effective(**peer);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].state, ManifestState::Published);
    EXPECT_FALSE((*peer)->incomplete(1, kAll).value());
    EXPECT_TRUE(SameEvents((*peer)->read(1, kAll).value(), before));
    EXPECT_EQ(Bytes(*directory / "manifest/secondary.log").find("\"state\":4"), std::string::npos);
}

TEST(FileTierStore, CompactionReaderThatPlannedBeforeTheSwitchReadsIdenticalEvents)
{
    auto directory = TestDirectory();
    auto store = OpenStore(*directory);
    ASSERT_TRUE(store.ok());
    PublishWindows(**store, 6);
    auto reader = FileTierStore::OpenReadOnly(*directory, std::chrono::hours(1), {}, 2);
    ASSERT_TRUE(reader.ok());
    const auto plan = (*reader)->manifest(1).value();
    ASSERT_EQ(plan.size(), 6u);
    const std::vector<Range> ranges{kAll,
                                    {Range::Axis::Hlc, {130, 0}, {260, 0}},
                                    {Range::Axis::Physical, {1500, 0}, {2600, 0}}};
    struct Results
    {
        std::vector<std::vector<Event>> single, batch, whole;
    };
    const auto collect = [&](FileTierStore& sut)
    {
        Results results;
        for(const auto& range: ranges)
            for(const size_t cap: {SIZE_MAX, size_t{1}})
            {
                for(const auto& record: plan)
                {
                    auto events = sut.readRecord(record, range, cap);
                    EXPECT_TRUE(events.ok()) << events.status();
                    results.single.push_back(events.ok() ? *events : std::vector<Event>{});
                }
                for(auto& events: sut.readRecords(plan, range, cap))
                {
                    EXPECT_TRUE(events.ok()) << events.status();
                    results.batch.push_back(events.ok() ? *events : std::vector<Event>{});
                }
            }
        for(const auto& range: ranges)
        {
            auto events = sut.read(1, range);
            EXPECT_TRUE(events.ok()) << events.status();
            results.whole.push_back(events.ok() ? *events : std::vector<Event>{});
        }
        return results;
    };
    const auto before = collect(**reader);
    auto result = (*store)->compactOnce(Eager());
    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(result->inputs, 6u);
    for(const auto& record: plan) ASSERT_FALSE(fs::exists(*directory / record.file));
    for(auto* sut: {reader->get(), store->get()})
    {
        const auto after = collect(*sut);
        ASSERT_EQ(after.single.size(), before.single.size());
        for(size_t i = 0; i < before.single.size(); ++i)
            EXPECT_TRUE(SameEvents(after.single[i], before.single[i])) << i;
        ASSERT_EQ(after.batch.size(), before.batch.size());
        for(size_t i = 0; i < before.batch.size(); ++i) EXPECT_TRUE(SameEvents(after.batch[i], before.batch[i])) << i;
        ASSERT_EQ(after.whole.size(), before.whole.size());
        for(size_t i = 0; i < before.whole.size(); ++i) EXPECT_TRUE(SameEvents(after.whole[i], before.whole[i])) << i;
    }
}

TEST(FileTierStore, CompactionDoesNotChangeWatermarkAcrossRestartRetentionOrOutputLoss)
{
    {
        auto directory = TestDirectory();
        auto store = OpenStore(*directory);
        ASSERT_TRUE(store.ok());
        PublishWindows(**store, 4);
        const auto w = (*store)->contiguousWatermark(1).value();
        EXPECT_EQ(w, (Hlc{300, 0}));
        auto result = (*store)->compactOnce(Eager());
        ASSERT_TRUE(result.ok());
        EXPECT_EQ((*store)->contiguousWatermark(1).value(), w);
        store->reset();
        store = OpenStore(*directory);
        ASSERT_TRUE(store.ok());
        EXPECT_EQ((*store)->contiguousWatermark(1).value(), w);
        ASSERT_TRUE((*store)->eraseFile(result->output).ok());
        EXPECT_EQ((*store)->contiguousWatermark(1).value(), w);
        EXPECT_TRUE((*store)->read(1, kAll).value().empty());
        EXPECT_FALSE(fs::exists(*directory / result->output));
    }
    {
        auto directory = TestDirectory();
        auto store = OpenStore(*directory);
        ASSERT_TRUE(store.ok());
        PublishWindows(**store, 4);
        const auto w = (*store)->contiguousWatermark(1).value();
        auto result = (*store)->compactOnce(Eager());
        ASSERT_TRUE(result.ok());
        store->reset();
        ASSERT_TRUE(fs::remove(*directory / result->output));
        store = OpenStore(*directory);
        ASSERT_TRUE(store.ok()) << store.status();
        const auto records = Effective(**store);
        ASSERT_EQ(records.size(), 1u);
        EXPECT_EQ(records[0].state, ManifestState::Lost);
        EXPECT_EQ((*store)->contiguousWatermark(1).value(), w);
        EXPECT_TRUE((*store)->incomplete(1, kAll).value());
    }
}

TEST(FileTierStore, CompactionAndTombstoneRespectBothCommitOrders)
{
    {
        auto directory = TestDirectory();
        FileTierStore* sut = nullptr;
        FileTierStore::Hooks hooks;
        hooks.compaction_step = [&sut](std::string_view step)
        { return step == "switch" ? sut->tombstone(1) : absl::OkStatus(); };
        auto store = OpenStore(*directory, hooks);
        ASSERT_TRUE(store.ok());
        sut = store->get();
        const auto inputs = PublishWindows(**store, 4);
        EXPECT_TRUE(absl::IsFailedPrecondition((*store)->compactOnce(Eager()).status()));
        EXPECT_TRUE(CompactionFiles(*directory).empty());
        for(const auto& input: inputs) EXPECT_TRUE(fs::exists(*directory / input.file));
        EXPECT_EQ(Effective(**store).size(), 4u);
        EXPECT_EQ((*store)->compactOnce(Eager()).value().inputs, 0u);
    }
    {
        auto directory = TestDirectory();
        auto store = OpenStore(*directory);
        ASSERT_TRUE(store.ok());
        PublishWindows(**store, 4);
        auto result = (*store)->compactOnce(Eager());
        ASSERT_TRUE(result.ok());
        ASSERT_TRUE((*store)->tombstone(1).ok());
        for(const auto& record: Effective(**store))
            if(record.state == ManifestState::Published)
            {
                ASSERT_TRUE((*store)->eraseFile(record.file).ok());
            }
        EXPECT_TRUE((*store)->retryDeletedFiles().ok());
        EXPECT_FALSE((*store)->hasPendingUnlinks(1).value());
        EXPECT_TRUE(fs::is_empty(*directory / "1"));
    }
}

TEST(FileTierStore, CompactionAndInputEraseRespectBothCommitOrders)
{
    {
        auto directory = TestDirectory();
        FileTierStore* sut = nullptr;
        std::string erased;
        FileTierStore::Hooks hooks;
        hooks.compaction_step = [&](std::string_view step)
        { return step == "switch" ? sut->eraseFile(erased) : absl::OkStatus(); };
        auto store = OpenStore(*directory, hooks);
        ASSERT_TRUE(store.ok());
        sut = store->get();
        const auto inputs = PublishWindows(**store, 4);
        erased = inputs[1].file;
        EXPECT_TRUE(absl::IsAborted((*store)->compactOnce(Eager()).status()));
        EXPECT_TRUE(CompactionFiles(*directory).empty());
        for(const auto& record: Effective(**store))
        {
            EXPECT_EQ(record.state, record.file == erased ? ManifestState::Deleted : ManifestState::Published);
        }
    }
    {
        auto directory = TestDirectory();
        auto store = OpenStore(*directory);
        ASSERT_TRUE(store.ok());
        const auto inputs = PublishWindows(**store, 4);
        const auto w = (*store)->contiguousWatermark(1).value();
        auto result = (*store)->compactOnce(Eager());
        ASSERT_TRUE(result.ok());
        EXPECT_TRUE(absl::IsFailedPrecondition((*store)->eraseFile(inputs[1].file)));
        EXPECT_EQ((*store)->read(1, kAll).value().size(), 8u);
        ASSERT_TRUE((*store)->eraseFile(result->output).ok());
        EXPECT_EQ((*store)->contiguousWatermark(1).value(), w);
        EXPECT_TRUE((*store)->read(1, kAll).value().empty());
    }
}

TEST(FileTierStore, CompactionRetriedPublicationUsesTheOutputWithoutRecreatingTheInput)
{
    auto directory = TestDirectory();
    auto store = OpenStore(*directory);
    ASSERT_TRUE(store.ok());
    const auto inputs = PublishWindows(**store, 4);
    auto result = (*store)->compactOnce(Eager());
    ASSERT_TRUE(result.ok());
    auto retry = (*store)->publish(Rich(2));
    ASSERT_TRUE(retry.ok()) << retry.status();
    EXPECT_EQ(retry->state, ManifestState::Published);
    EXPECT_EQ(retry->file, result->output);
    EXPECT_FALSE(fs::exists(*directory / inputs[2].file));
    auto changed = Rich(2);
    changed.events[0].envelope.payload = "changed";
    EXPECT_TRUE(absl::IsUnavailable((*store)->publish(changed).status()));
    EXPECT_FALSE(fs::exists(*directory / inputs[2].file));
    EXPECT_EQ(Effective(**store).size(), 1u);
}

TEST(ManifestLog, CompactionSwitchIsOneFramedLineThatOldParsersRefuse)
{
    auto directory = TestDirectory();
    auto store = OpenStore(*directory);
    ASSERT_TRUE(store.ok());
    PublishWindows(**store, 3);
    ASSERT_TRUE((*store)->compactOnce(Eager()).ok());
    std::istringstream log(Bytes(*directory / "manifest/primary.log"));
    std::vector<std::string> switches;
    for(std::string line; std::getline(log, line);)
        if(line.find("compact_v1") != std::string::npos)
            switches.push_back(line);
    ASSERT_EQ(switches.size(), 1u);
    // The output is nested under its own key: a pre-B3 parser finds no top-level record fields and fails closed.
    const auto json = nlohmann::json::parse(switches[0]);
    EXPECT_FALSE(json.contains("chunk") || json.contains("watermark") || json.contains("tombstoned"));
    EXPECT_TRUE(json.at("compact_v1").at("output").contains("chunk"));
    EXPECT_TRUE(json.contains("length") && json.contains("crc"));
    EXPECT_EQ(json.at("compact_v1").at("inputs").size(), 3u);
}

TEST(ManifestLog, AnIncompleteSwitchLineIsRetriedAtTheNextPoll)
{
    auto directory = TestDirectory();
    auto store = OpenStore(*directory);
    ASSERT_TRUE(store.ok());
    PublishWindows(**store, 3);
    ASSERT_TRUE((*store)->compactOnce(Eager()).ok());
    store->reset();
    const auto path = *directory / "manifest/primary.log";
    const auto log = Bytes(path);
    const auto begin = log.find("{\"length\":");
    ASSERT_NE(begin, std::string::npos);
    const auto length = log.find('\n', begin) - begin;
    // An NFS client can see the final page and newline of a long line while an earlier page is still a hole.
    auto holed = log;
    std::fill(holed.begin() + static_cast<long>(begin + 20),
              holed.begin() + static_cast<long>(begin + length - 20),
              '\0');
    std::ofstream(path, std::ios::binary | std::ios::trunc) << holed;
    auto reader = ManifestLog::OpenReadOnly(*directory);
    auto index = reader->sync();
    ASSERT_TRUE(index.ok()) << index.status();
    EXPECT_TRUE((*index)->switches.empty());
    EXPECT_EQ((*index)->records.size(), 3u);
    {
        std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
        file.seekp(static_cast<std::streamoff>(begin));
        file.write(log.data() + begin, static_cast<std::streamsize>(length));
    }
    index = reader->sync();
    ASSERT_TRUE(index.ok()) << index.status();
    EXPECT_EQ((*index)->switches.size(), 1u);
    EXPECT_EQ((*index)->superseded.size(), 3u);
}
} // namespace chronolog

namespace chronolog
{
namespace
{
TierConfig MakeSlowTier(const fs::path& root,
                        const std::string& name = "slow",
                        uint32_t rank = 1,
                        const std::string& uuid = "tier-uuid")
{
    fs::create_directories(root);
    struct statfs info
    {
    };
    EXPECT_EQ(::statfs(root.c_str(), &info), 0);
    nlohmann::json marker{{"deployment_id", "test"},
                          {"name", name},
                          {"rank", rank},
                          {"kind", "posix"},
                          {"tier_uuid", uuid},
                          {"f_type", info.f_type}};
    std::ofstream(root / ".chronolog-tier.json") << marker.dump();
    return {name, "posix", root, rank, uuid};
}
void AttachTier(FileTierStore& store, const fs::path& local, const TierConfig& tier, std::string writer = "primary")
{
    // Migration moves only files the scrubber's mark covers (I13.13, I13.17).
    (void)local;
    (void)writer;
    ASSERT_TRUE(store.scrubOnce(0).ok());
    ASSERT_TRUE(store.configureTiers("test", {tier}).ok());
    ASSERT_TRUE(store.probeTiers().ok());
}
} // namespace

TEST(FileTierStore, MigrationCrashAtEveryStepLeavesOneEffectiveCopy)
{
    for(int crash = 1; crash <= 6; ++crash)
    {
        SCOPED_TRACE(crash);
        auto directory = TestDirectory();
        const auto tier = MakeSlowTier(*directory / "slow");
        FileTierStore::Hooks hooks;
        hooks.migration_step = [crash](int step)
        { return step == crash ? absl::AbortedError("injected migration crash") : absl::OkStatus(); };
        auto store = OpenStore(*directory / "local", hooks, std::make_shared<ProtoChunkCodec>());
        ASSERT_TRUE(store.ok()) << store.status();
        const auto records = PublishWindows(**store, 1);
        ASSERT_EQ(records.size(), 1u);
        const auto original = Bytes(*directory / "local" / records[0].file);
        AttachTier(**store, *directory / "local", tier);
        EXPECT_FALSE((*store)->migrateOnce("slow").ok());
        store->reset();
        store = OpenStore(*directory / "local", {}, std::make_shared<ProtoChunkCodec>());
        ASSERT_TRUE(store.ok()) << store.status();
        AttachTier(**store, *directory / "local", tier);
        auto location = (*store)->location(records[0].file);
        ASSERT_TRUE(location.ok());
        EXPECT_EQ(location->has_value(), crash == 6);
        const auto effective = (location->has_value() ? tier.root : *directory / "local") / records[0].file;
        EXPECT_EQ(Bytes(effective), original);
        EXPECT_EQ(Effective(**store).front().state, ManifestState::Published);
        EXPECT_EQ((*store)->contiguousWatermark(1).value(), (Hlc{150, 0}));
        if(crash == 6)
        {
            // The stale copy of a committed migration is the cleanup pass's work, never a migration attempt's.
            auto recovered = (*store)->cleanupMigrations();
            ASSERT_TRUE(recovered.ok()) << recovered;
            EXPECT_FALSE(fs::exists(*directory / "local" / records[0].file));
        }
        else
        {
            ASSERT_TRUE((*store)->sweepTiers().ok());
            EXPECT_FALSE(fs::exists(tier.root / records[0].file));
        }
    }
}

TEST(FileTierStore, MigrationCommitRechecksUnderTheStoreMutex)
{
    auto directory = TestDirectory();
    const auto tier = MakeSlowTier(*directory / "slow");
    FileTierStore* sut = nullptr;
    FileTierStore::Hooks hooks;
    hooks.migration_step = [&sut](int step) { return step == 5 ? sut->tombstone(1) : absl::OkStatus(); };
    auto store = OpenStore(*directory / "local", hooks, std::make_shared<ProtoChunkCodec>());
    ASSERT_TRUE(store.ok());
    sut = store->get();
    const auto records = PublishWindows(**store, 1);
    AttachTier(**store, *directory / "local", tier);
    EXPECT_EQ((*store)->migrateOnce("slow").status().code(), absl::StatusCode::kAborted);
    EXPECT_FALSE((*store)->location(records[0].file).value().has_value());
    EXPECT_TRUE(fs::exists(*directory / "local" / records[0].file));
    EXPECT_FALSE(fs::exists(tier.root / records[0].file));
}

TEST(FileTierStore, CompactionNeverTakesAMigratedOrMigratingInput)
{
    for(bool committed: {false, true})
    {
        auto directory = TestDirectory();
        const auto tier = MakeSlowTier(*directory / "slow");
        FileTierStore* sut = nullptr;
        FileTierStore::Hooks hooks;
        if(!committed)
            hooks.migration_step = [&sut](int step)
            {
                if(step == 3)
                {
                    auto compacted = sut->compactOnce(Eager());
                    EXPECT_TRUE(compacted.ok()) << compacted.status();
                    if(compacted.ok())
                    {
                        EXPECT_EQ(compacted->inputs, 2u);
                    }
                }
                return absl::OkStatus();
            };
        auto store = OpenStore(*directory / "local", hooks, std::make_shared<ProtoChunkCodec>());
        ASSERT_TRUE(store.ok());
        sut = store->get();
        const auto records = PublishWindows(**store, 3);
        AttachTier(**store, *directory / "local", tier);
        auto moved = (*store)->migrateOnce("slow");
        ASSERT_TRUE(moved.ok()) << moved.status();
        EXPECT_EQ(*moved, 1u);
        if(committed)
        {
            auto compacted = (*store)->compactOnce(Eager());
            ASSERT_TRUE(compacted.ok()) << compacted.status();
            EXPECT_EQ(compacted->inputs, 2u);
        }
        const auto manifest = Effective(**store);
        EXPECT_EQ(manifest.size(), 2u);
        EXPECT_EQ(manifest.front().file, records.front().file);
        EXPECT_TRUE(fs::exists(tier.root / records.front().file));
    }
}

TEST(FileTierStore, MigrationThroughAReplacedRootNeverCommits)
{
    auto directory = TestDirectory();
    const auto tier = MakeSlowTier(*directory / "slow");
    FileTierStore::Hooks hooks;
    hooks.migration_step = [tier](int step)
    {
        if(step == 2)
        {
            fs::rename(tier.root, tier.root.string() + "-original");
            fs::create_directory(tier.root);
        }
        return absl::OkStatus();
    };
    auto store = OpenStore(*directory / "local", hooks, std::make_shared<ProtoChunkCodec>());
    ASSERT_TRUE(store.ok());
    const auto records = PublishWindows(**store, 1);
    AttachTier(**store, *directory / "local", tier);
    EXPECT_FALSE((*store)->migrateOnce("slow").ok());
    EXPECT_FALSE((*store)->location(records[0].file).value().has_value());
    EXPECT_TRUE(fs::is_empty(tier.root));
    EXPECT_TRUE(fs::exists(*directory / "local" / records[0].file));
    fs::remove(tier.root);
    fs::rename(tier.root.string() + "-original", tier.root);
    EXPECT_EQ(Effective(**store).front().state, ManifestState::Published);
}

TEST(FileTierStore, EraseThroughAReplacedRootStaysPending)
{
    auto directory = TestDirectory();
    const auto tier = MakeSlowTier(*directory / "slow");
    std::atomic<bool> replace{false};
    FileTierStore::Hooks hooks;
    hooks.tier_step = [tier, &replace](std::string_view step)
    {
        if(step == "erase" && replace.exchange(false))
        {
            fs::rename(tier.root, tier.root.string() + "-original");
            fs::create_directory(tier.root);
        }
        return absl::OkStatus();
    };
    auto store = OpenStore(*directory / "local", hooks, std::make_shared<ProtoChunkCodec>());
    ASSERT_TRUE(store.ok());
    const auto records = PublishWindows(**store, 1);
    AttachTier(**store, *directory / "local", tier);
    ASSERT_TRUE((*store)->migrateOnce("slow").ok());
    replace = true;
    EXPECT_FALSE((*store)->eraseFile(records[0].file).ok());
    EXPECT_FALSE((*store)->awaitTierUnlinksForTesting(std::chrono::seconds(5)).ok());
    EXPECT_TRUE((*store)->hasPendingUnlinks(1).value());
    EXPECT_TRUE(fs::is_empty(tier.root));
    EXPECT_TRUE(fs::exists(fs::path(tier.root.string() + "-original") / records[0].file));
    fs::remove(tier.root);
    fs::rename(tier.root.string() + "-original", tier.root);
    ASSERT_TRUE((*store)->probeTiers().ok());
    EXPECT_FALSE((*store)->retryDeletedFiles().ok());
    ASSERT_TRUE((*store)->awaitTierUnlinksForTesting(std::chrono::seconds(5)).ok());
    EXPECT_FALSE((*store)->hasPendingUnlinks(1).value());
    EXPECT_FALSE(fs::exists(tier.root / records[0].file));
}

TEST(FileTierStore, PeerSweepNeverRemovesAnotherWritersInFlightDestination)
{
    auto directory = TestDirectory();
    const auto tier = MakeSlowTier(*directory / "slow");
    std::string file;
    FileTierStore::Hooks hooks;
    hooks.migration_step = [&](int step)
    {
        if(step == 3)
        {
            auto peer = OpenStore(*directory / "local", {}, std::make_shared<ProtoChunkCodec>(), "peer");
            EXPECT_TRUE(peer.ok()) << peer.status();
            if(peer.ok())
            {
                AttachTier(**peer, *directory / "local", tier, "peer");
                EXPECT_TRUE((*peer)->sweepTiers().ok());
                EXPECT_TRUE(fs::exists(tier.root / file));
            }
        }
        return absl::OkStatus();
    };
    auto store = OpenStore(*directory / "local", hooks, std::make_shared<ProtoChunkCodec>());
    ASSERT_TRUE(store.ok());
    const auto records = PublishWindows(**store, 1);
    file = records[0].file;
    const auto bytes = Bytes(*directory / "local" / file);
    AttachTier(**store, *directory / "local", tier);
    auto migrated = (*store)->migrateOnce("slow");
    ASSERT_TRUE(migrated.ok()) << migrated.status();
    EXPECT_EQ(Bytes(tier.root / file), bytes);
    auto read = (*store)->readRecord(records[0], kAll);
    ASSERT_TRUE(read.ok()) << read.status();
    EXPECT_TRUE(SameEvents(*read, Rich(0).events));
    EXPECT_TRUE((*store)->location(file).value().has_value());
}

TEST(FileTierStore, EmptyMountPointIsUnavailableNeverLost)
{
    auto directory = TestDirectory();
    const auto tier = MakeSlowTier(*directory / "slow");
    auto store = OpenStore(*directory / "local", {}, std::make_shared<ProtoChunkCodec>());
    ASSERT_TRUE(store.ok());
    const auto records = PublishWindows(**store, 1);
    AttachTier(**store, *directory / "local", tier);
    ASSERT_TRUE((*store)->migrateOnce("slow").ok());
    store->reset();
    fs::rename(tier.root, tier.root.string() + "-original");
    fs::create_directory(tier.root);
    store = OpenStore(*directory / "local", {}, std::make_shared<ProtoChunkCodec>());
    ASSERT_TRUE(store.ok());
    ASSERT_TRUE((*store)->configureTiers("test", {tier}).ok());
    EXPECT_FALSE((*store)->probeTiers().ok());
    EXPECT_FALSE((*store)->migrateOnce("slow").ok());
    EXPECT_EQ(Effective(**store).front().state, ManifestState::Published);
    EXPECT_TRUE(fs::is_empty(tier.root));
    EXPECT_TRUE((*store)->location(records[0].file).value().has_value());
}

// I13.15: an availability epoch ends when a probe or an operation fails, never because a probe ran. A reader or a
// migration that holds the epoch's descriptor stays current across probes of a healthy root.
TEST(FileTierStore, AProbeOfAnAvailableTierKeepsItsEpoch)
{
    auto directory = TestDirectory();
    const auto config = MakeSlowTier(*directory / "slow");
    auto tier = std::make_shared<PosixTier>(config, "test", 2, std::chrono::milliseconds(1000));
    ASSERT_TRUE(tier->probe().ok());
    const auto first = tier->directory();
    ASSERT_NE(first, nullptr);
    ASSERT_TRUE(tier->probe().ok());
    EXPECT_TRUE(tier->current(first));
    // The root is replaced by an empty directory: the epoch ends and nothing is available.
    fs::rename(config.root, config.root.string() + "-away");
    fs::create_directory(config.root);
    EXPECT_FALSE(tier->probe().ok());
    EXPECT_FALSE(tier->current(first));
    EXPECT_EQ(tier->directory(), nullptr);
    fs::remove(config.root);
    fs::rename(config.root.string() + "-away", config.root);
    ASSERT_TRUE(tier->probe().ok());
    const auto second = tier->directory();
    ASSERT_NE(second, nullptr);
    EXPECT_GT(second->epoch, first->epoch);
    EXPECT_FALSE(tier->current(first));
    tier->stop();
}

namespace
{
// Counts the chunk files a store loads, by file name; the validation of Open and of the scrubber goes through it.
struct LoadCounter
{
    std::shared_ptr<std::mutex> mutex = std::make_shared<std::mutex>();
    std::shared_ptr<std::map<std::string, int>> loads = std::make_shared<std::map<std::string, int>>();
    FileTierStore::LoadFile hook() const
    {
        return [mutex = mutex, loads = loads](const fs::path& path)
        {
            {
                std::lock_guard lock(*mutex);
                ++(*loads)[path.filename().string()];
            }
            return LoadChunkFile(path);
        };
    }
    int of(const ManifestRecord& record) const
    {
        std::lock_guard lock(*mutex);
        const auto found = loads->find(fs::path(record.file).filename().string());
        return found == loads->end() ? 0 : found->second;
    }
    int total() const
    {
        std::lock_guard lock(*mutex);
        int sum = 0;
        for(const auto& [file, count]: *loads) sum += count;
        return sum;
    }
};

void Corrupt(const fs::path& file)
{
    std::ofstream corrupt(file, std::ios::binary);
    corrupt << '\x80';
}

uint64_t SeqOf(const fs::path& root, const std::string& file)
{
    auto reader = ManifestLog::OpenReadOnly(root);
    auto index = reader->sync();
    EXPECT_TRUE(index.ok()) << index.status();
    if(!index.ok())
        return 0;
    const auto found = (*index)->record_sequences.find(file);
    return found == (*index)->record_sequences.end() ? 0 : found->second;
}

ManifestState StateOf(FileTierStore& store, const std::string& file)
{
    for(const auto& record: Effective(store))
        if(record.file == file)
            return record.state;
    ADD_FAILURE() << "no effective record for " << file;
    return ManifestState::Failed;
}
} // namespace

TEST(FileTierStore, OpenValidatesOnlyRecordsAboveTheValidatedMark)
{
    auto directory = TestDirectory();
    auto store = OpenStore(*directory);
    ASSERT_TRUE(store.ok());
    const auto covered = PublishWindows(**store, 3);
    const auto scrubbed = (*store)->scrubOnce(0);
    ASSERT_TRUE(scrubbed.ok()) << scrubbed.status();
    EXPECT_EQ(scrubbed->validated, 3u);
    EXPECT_EQ(scrubbed->through, 3u);
    EXPECT_TRUE(scrubbed->marked);
    EXPECT_EQ(ManifestLog::ValidatedMark(*directory, "primary"), std::optional<uint64_t>(3));
    const auto above = PublishWindows(**store, 1, 3);
    store->reset();
    LoadCounter counter;
    store = OpenStore(*directory, {}, std::make_shared<HDF5ChunkCodec>(), "primary", counter.hook());
    ASSERT_TRUE(store.ok()) << store.status();
    for(const auto& record: covered) EXPECT_EQ(counter.of(record), 0) << record.file;
    EXPECT_EQ(counter.of(above[0]), 1);
    store->reset();
    // A covered file that goes bad stays Published at Open and fails its read; one above the mark is Lost at Open.
    Corrupt(*directory / covered[0].file);
    Corrupt(*directory / above[0].file);
    store = OpenStore(*directory);
    ASSERT_TRUE(store.ok()) << store.status();
    EXPECT_EQ(StateOf(**store, covered[0].file), ManifestState::Published);
    EXPECT_EQ(StateOf(**store, above[0].file), ManifestState::Lost);
    EXPECT_FALSE((*store)->read(1, kAll).ok());
    // A store without a mark recovers as it always did: every effective record is read.
    auto unmarked = TestDirectory();
    auto plain = OpenStore(*unmarked / "plain");
    ASSERT_TRUE(plain.ok());
    const auto records = PublishWindows(**plain, 2);
    plain->reset();
    LoadCounter all;
    plain = OpenStore(*unmarked / "plain", {}, std::make_shared<HDF5ChunkCodec>(), "primary", all.hook());
    ASSERT_TRUE(plain.ok());
    for(const auto& record: records) EXPECT_EQ(all.of(record), 1) << record.file;
}

TEST(FileTierStore, ValidatedMarkSurvivesManifestCompaction)
{
    auto directory = TestDirectory();
    auto store = OpenStore(*directory);
    ASSERT_TRUE(store.ok());
    const auto covered = PublishWindows(**store, 3);
    ASSERT_TRUE((*store)->scrubOnce(0).ok());
    // The snapshot takes every line and the log is truncated; the seq rides in the lines and the mark outside them.
    ASSERT_TRUE((*store)->compact().ok());
    EXPECT_EQ(fs::file_size(*directory / "manifest/primary.log"), 0u);
    store->reset();
    LoadCounter counter;
    store = OpenStore(*directory, {}, std::make_shared<HDF5ChunkCodec>(), "primary", counter.hook());
    ASSERT_TRUE(store.ok()) << store.status();
    EXPECT_EQ(counter.total(), 0);
    for(const auto& record: covered) EXPECT_EQ(SeqOf(*directory, record.file), 1u + (&record - covered.data()));
    const auto above = PublishWindows(**store, 1, 3);
    EXPECT_EQ(SeqOf(*directory, above[0].file), 4u);
    store->reset();
    LoadCounter after;
    store = OpenStore(*directory, {}, std::make_shared<HDF5ChunkCodec>(), "primary", after.hook());
    ASSERT_TRUE(store.ok()) << store.status();
    EXPECT_EQ(after.total(), 1);
    EXPECT_EQ(after.of(above[0]), 1);
}

TEST(FileTierStore, RecordsPublishedDuringAScrubAreNotCovered)
{
    auto directory = TestDirectory();
    FileTierStore* sut = nullptr;
    std::optional<ManifestRecord> during;
    FileTierStore::Hooks hooks;
    hooks.scrub_step = [&](std::string_view)
    {
        if(!during)
        {
            auto record = sut->publish(Rich(2));
            EXPECT_TRUE(record.ok()) << record.status();
            if(record.ok())
                during = *record;
        }
        return absl::OkStatus();
    };
    auto store = OpenStore(*directory, hooks);
    ASSERT_TRUE(store.ok());
    sut = store->get();
    const auto before = PublishWindows(**store, 2);
    const auto scrubbed = (*store)->scrubOnce(0);
    ASSERT_TRUE(scrubbed.ok()) << scrubbed.status();
    ASSERT_TRUE(during.has_value());
    EXPECT_EQ(scrubbed->through, 2u);
    EXPECT_EQ(SeqOf(*directory, during->file), 3u);
    store->reset();
    LoadCounter counter;
    store = OpenStore(*directory, {}, std::make_shared<HDF5ChunkCodec>(), "primary", counter.hook());
    ASSERT_TRUE(store.ok()) << store.status();
    for(const auto& record: before) EXPECT_EQ(counter.of(record), 0);
    EXPECT_EQ(counter.of(*during), 1);
}

TEST(FileTierStore, SeqIsNeverReusedAfterATornTail)
{
    auto directory = TestDirectory();
    auto store = OpenStore(*directory);
    ASSERT_TRUE(store.ok());
    const auto records = PublishWindows(**store, 2);
    ASSERT_TRUE((*store)->scrubOnce(0).ok());
    ASSERT_EQ(ManifestLog::ValidatedMark(*directory, "primary"), std::optional<uint64_t>(2));
    store->reset();
    // The line that carried seq 2 was visible and is lost with the tail; the mark already covers seq 2.
    const auto log = *directory / "manifest/primary.log";
    auto text = Bytes(log);
    const auto last = text.rfind("\"seq\":2");
    ASSERT_NE(last, std::string::npos);
    const auto line = text.rfind('\n', last);
    ASSERT_NE(line, std::string::npos);
    fs::resize_file(log, line + 1 + (last - line) / 2);
    LoadCounter counter;
    store = OpenStore(*directory, {}, std::make_shared<HDF5ChunkCodec>(), "primary", counter.hook());
    ASSERT_TRUE(store.ok()) << store.status();
    // Its file is adopted as an orphan under a new seq above the mark, so this Open and any later one validate it.
    EXPECT_EQ(SeqOf(*directory, records[1].file), 3u);
    EXPECT_GE(counter.of(records[1]), 1);
    EXPECT_EQ(SeqOf(*directory, PublishWindows(**store, 1, 2)[0].file), 4u);
    store->reset();
    LoadCounter again;
    store = OpenStore(*directory, {}, std::make_shared<HDF5ChunkCodec>(), "primary", again.hook());
    ASSERT_TRUE(store.ok()) << store.status();
    EXPECT_EQ(again.of(records[0]), 0);
    EXPECT_EQ(again.of(records[1]), 1);
}

TEST(FileTierStore, OpenValidatesInputsOfAnUnreferencedOutputBeforeRemovingIt)
{
    auto directory = TestDirectory();
    auto store = OpenStore(*directory, CrashAt("switch"));
    ASSERT_TRUE(store.ok());
    const auto inputs = PublishWindows(**store, 4);
    ASSERT_TRUE((*store)->scrubOnce(0).ok());
    // The output is linked and no switch names it; the mark covers every input.
    EXPECT_FALSE((*store)->compactOnce(Eager()).ok());
    const auto outputs = CompactionFiles(*directory);
    ASSERT_EQ(outputs.size(), 1u);
    store->reset();
    Corrupt(*directory / inputs[1].file);
    store = OpenStore(*directory);
    ASSERT_TRUE(store.ok()) << store.status();
    // Removing the output needs inputs validated by this Open: the bad input is found and the output stays.
    EXPECT_TRUE(fs::exists(*directory / "1" / outputs[0]));
    EXPECT_EQ(StateOf(**store, inputs[1].file), ManifestState::Lost);
    EXPECT_EQ(StateOf(**store, inputs[0].file), ManifestState::Published);
}

TEST(FileTierStore, OpenValidatesACommittedOutputBeforeUnlinkingItsInputs)
{
    auto directory = TestDirectory();
    auto store = OpenStore(*directory, CrashAt("cleanup"));
    ASSERT_TRUE(store.ok());
    const auto inputs = PublishWindows(**store, 4);
    const auto before = (*store)->read(1, kAll).value();
    EXPECT_FALSE((*store)->compactOnce(Eager()).ok());
    const auto committed = Effective(**store);
    ASSERT_EQ(committed.size(), 1u);
    // The scrubber covers the output while its inputs still await cleanup.
    const auto scrubbed = (*store)->scrubOnce(0);
    ASSERT_TRUE(scrubbed.ok()) << scrubbed.status();
    EXPECT_EQ(scrubbed->through, 5u);
    store->reset();
    Corrupt(*directory / committed[0].file);
    store = OpenStore(*directory);
    ASSERT_TRUE(store.ok()) << store.status();
    (void)(*store)->retryDeletedFiles();
    // The mark covered the output, yet Open read it before any input could be unlinked and rolled the switch back.
    for(const auto& input: inputs) EXPECT_TRUE(fs::exists(*directory / input.file)) << input.file;
    EXPECT_EQ(Effective(**store).size(), 4u);
    EXPECT_TRUE(SameEvents((*store)->read(1, kAll).value(), before));
    EXPECT_FALSE((*store)->incomplete(1, kAll).value());
}

TEST(FileTierStore, ScrubberMarksMissingAndCorruptWindowsLostWithoutLoweringWatermark)
{
    auto directory = TestDirectory();
    auto store = OpenStore(*directory);
    ASSERT_TRUE(store.ok());
    const auto records = PublishWindows(**store, 3);
    const auto w = (*store)->contiguousWatermark(1).value();
    fs::remove(*directory / records[0].file);
    Corrupt(*directory / records[1].file);
    const auto scrubbed = (*store)->scrubOnce(0);
    ASSERT_TRUE(scrubbed.ok()) << scrubbed.status();
    EXPECT_EQ(scrubbed->lost, 2u);
    EXPECT_EQ(scrubbed->validated, 1u);
    EXPECT_TRUE(scrubbed->marked);
    EXPECT_EQ(StateOf(**store, records[0].file), ManifestState::Lost);
    EXPECT_EQ(StateOf(**store, records[1].file), ManifestState::Lost);
    EXPECT_EQ(StateOf(**store, records[2].file), ManifestState::Published);
    EXPECT_EQ((*store)->contiguousWatermark(1).value(), w);
    EXPECT_TRUE((*store)->incomplete(1, kAll).value());
    store->reset();
    store = OpenStore(*directory);
    ASSERT_TRUE(store.ok()) << store.status();
    EXPECT_EQ((*store)->contiguousWatermark(1).value(), w);
    EXPECT_TRUE((*store)->incomplete(1, kAll).value());
}

namespace
{
// A store whose unlinks fail until `allow` is set, so a committed switch keeps its inputs pending.
absl::StatusOr<std::unique_ptr<FileTierStore>> OpenWithHeldUnlinks(const fs::path& root,
                                                                   std::shared_ptr<std::atomic<bool>> allow,
                                                                   std::function<void(const fs::path&)> before = {},
                                                                   FileTierStore::LoadFile load = {})
{
    return FileTierStore::Open(
            root,
            "primary",
            {{1, {100, 0}}},
            std::make_shared<HDF5ChunkCodec>(),
            [allow, before](const fs::path& path)
            {
                if(before)
                    before(path);
                if(!allow->load())
                {
                    errno = EBUSY;
                    return -1;
                }
                return ::unlink(path.c_str());
            },
            std::move(load),
            2);
}
} // namespace

TEST(FileTierStore, ScrubberRollbackWithdrawsPendingInputUnlinks)
{
    auto directory = TestDirectory();
    auto allow = std::make_shared<std::atomic<bool>>(false);
    auto store = OpenWithHeldUnlinks(*directory, allow);
    ASSERT_TRUE(store.ok());
    const auto inputs = PublishWindows(**store, 4);
    const auto before = (*store)->read(1, kAll).value();
    // The switch commits; the inputs cannot be unlinked yet and stay pending.
    (void)(*store)->compactOnce(Eager());
    const auto committed = Effective(**store);
    ASSERT_EQ(committed.size(), 1u);
    for(const auto& input: inputs) ASSERT_TRUE(fs::exists(*directory / input.file));
    EXPECT_TRUE((*store)->hasPendingUnlinks(1).value());
    Corrupt(*directory / committed[0].file);
    const auto scrubbed = (*store)->scrubOnce(0);
    ASSERT_TRUE(scrubbed.ok()) << scrubbed.status();
    EXPECT_EQ(scrubbed->rolled_back, 1u);
    EXPECT_EQ(scrubbed->lost, 0u);
    // Unlinks work again: only the rolled back output goes, the inputs were withdrawn.
    *allow = true;
    EXPECT_TRUE((*store)->retryDeletedFiles().ok());
    for(const auto& input: inputs) EXPECT_TRUE(fs::exists(*directory / input.file)) << input.file;
    EXPECT_FALSE(fs::exists(*directory / committed[0].file));
    EXPECT_EQ(Effective(**store).size(), 4u);
    EXPECT_TRUE(SameEvents((*store)->read(1, kAll).value(), before));
    EXPECT_FALSE((*store)->hasPendingUnlinks(1).value());
}

// Opus O6(d): an unlinker that copied the pending set before the rollback must not unlink a withdrawn input.
TEST(FileTierStore, RollbackBetweenPendingCopyAndUnlinkKeepsTheInputs)
{
    auto directory = TestDirectory();
    auto allow = std::make_shared<std::atomic<bool>>(false);
    FileTierStore* sut = nullptr;
    std::string erased;
    bool rolled = false;
    // retryDeletedFiles has copied the pending set and reaches its first unlink, of the erased file, which sorts
    // before every input; the scrubber rolls the switch back at that moment.
    auto store = OpenWithHeldUnlinks(*directory,
                                     allow,
                                     [&](const fs::path& path)
                                     {
                                         if(!allow->load() || rolled || path.filename() != fs::path(erased).filename())
                                             return;
                                         rolled = true;
                                         const auto scrubbed = sut->scrubOnce(0);
                                         EXPECT_TRUE(scrubbed.ok()) << scrubbed.status();
                                         if(scrubbed.ok())
                                         {
                                             EXPECT_EQ(scrubbed->rolled_back, 1u);
                                         }
                                     });
    ASSERT_TRUE(store.ok());
    sut = store->get();
    const auto first = PublishWindows(**store, 1);
    const auto inputs = PublishWindows(**store, 4, 1);
    erased = first[0].file;
    for(const auto& input: inputs) ASSERT_LT(erased, input.file);
    // Erased first, so the compaction run starts at the next window and the erased file stays pending.
    (void)(*store)->eraseFile(erased);
    ASSERT_TRUE(fs::exists(*directory / erased));
    (void)(*store)->compactOnce(Eager());
    std::string output;
    for(const auto& record: Effective(**store))
        if(record.state == ManifestState::Published)
        {
            ASSERT_TRUE(output.empty()) << "one output replaces the four inputs";
            output = record.file;
        }
    ASSERT_FALSE(output.empty());
    for(const auto& input: inputs)
    {
        ASSERT_NE(output, input.file);
        ASSERT_TRUE(fs::exists(*directory / input.file));
    }
    Corrupt(*directory / output);
    *allow = true;
    (void)(*store)->retryDeletedFiles();
    EXPECT_TRUE(rolled);
    for(const auto& input: inputs)
    {
        EXPECT_TRUE(fs::exists(*directory / input.file)) << input.file;
        EXPECT_EQ(StateOf(**store, input.file), ManifestState::Published);
    }
    EXPECT_FALSE(fs::exists(*directory / erased));
    auto events = (*store)->read(1, kAll);
    ASSERT_TRUE(events.ok()) << events.status();
    EXPECT_EQ(events->size(), 8u);
}

namespace
{
// Holds the scrubber inside the validation of a rollback's inputs: the load of the last input returns only once
// release() runs, so the test acts between that validation and the scrubber's re-check.
struct HeldInputLoad
{
    std::shared_ptr<std::atomic<int>> remaining = std::make_shared<std::atomic<int>>(0);
    std::shared_ptr<std::set<std::string>> inputs = std::make_shared<std::set<std::string>>();
    std::shared_ptr<std::promise<void>> entered = std::make_shared<std::promise<void>>();
    std::shared_ptr<std::promise<void>> freed = std::make_shared<std::promise<void>>();
    std::shared_ptr<std::once_flag> once = std::make_shared<std::once_flag>();
    std::shared_future<void> released = freed->get_future().share();
    FileTierStore::LoadFile hook() const
    {
        return [remaining = remaining, inputs = inputs, entered = entered, released = released](const fs::path& path)
        {
            const bool last =
                    remaining->load() > 0 && inputs->contains(path.filename().string()) && remaining->fetch_sub(1) == 1;
            auto bytes = LoadChunkFile(path);
            if(last)
            {
                entered->set_value();
                released.wait_for(std::chrono::seconds(60));
            }
            return bytes;
        };
    }
    void arm(const std::vector<ManifestRecord>& records)
    {
        for(const auto& record: records) inputs->insert(fs::path(record.file).filename().string());
        remaining->store(static_cast<int>(records.size()));
    }
    bool held() { return entered->get_future().wait_for(std::chrono::seconds(60)) == std::future_status::ready; }
    void release()
    {
        std::call_once(*once, [this] { freed->set_value(); });
    }
};

size_t RollbackLines(const fs::path& root)
{
    std::ifstream log(root / "manifest/primary.log", std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(log)), std::istreambuf_iterator<char>());
    size_t count = 0;
    for(auto at = text.find("compact_rollback_v1"); at != std::string::npos;
        at = text.find("compact_rollback_v1", at + 1))
        ++count;
    return count;
}
} // namespace

// M11.7, I13.17: the scrubber reads and checksums a rollback's inputs with the store mutex released, so a publish and
// an erase complete while that validation is held; the rollback still follows.
TEST(FileTierStore, ScrubberValidatesRollbackInputsWithoutTheStoreMutex)
{
    auto directory = TestDirectory();
    auto allow = std::make_shared<std::atomic<bool>>(false);
    HeldInputLoad load;
    auto store = OpenWithHeldUnlinks(*directory, allow, {}, load.hook());
    ASSERT_TRUE(store.ok());
    const auto inputs = PublishWindows(**store, 4);
    (void)(*store)->compactOnce(Eager());
    const auto committed = Effective(**store);
    ASSERT_EQ(committed.size(), 1u);
    Corrupt(*directory / committed[0].file);
    load.arm(inputs);
    auto scrub = std::async(std::launch::async, [&] { return (*store)->scrubOnce(0); });
    std::string published;
    absl::Status erased = absl::UnknownError("erase did not run");
    std::future_status writer = std::future_status::timeout;
    if(load.held())
    {
        auto work = std::async(std::launch::async,
                               [&]
                               {
                                   auto record = (*store)->publish(Rich(4));
                                   if(!record.ok())
                                       return;
                                   published = record->file;
                                   erased = (*store)->eraseFile(published);
                               });
        writer = work.wait_for(std::chrono::seconds(60));
        load.release();
    }
    else
        ADD_FAILURE() << "the scrubber never validated the rollback inputs";
    load.release();
    const auto scrubbed = scrub.get();
    EXPECT_EQ(writer, std::future_status::ready) << "publish and erase waited for the scrubber's input validation";
    ASSERT_FALSE(published.empty());
    EXPECT_EQ(StateOf(**store, published), ManifestState::Deleted) << erased;
    ASSERT_TRUE(scrubbed.ok()) << scrubbed.status();
    EXPECT_EQ(scrubbed->rolled_back, 1u);
    EXPECT_EQ(scrubbed->lost, 0u);
    EXPECT_TRUE(scrubbed->marked);
    for(const auto& input: inputs) EXPECT_EQ(StateOf(**store, input.file), ManifestState::Published);
}

// I13.17: what the scrubber validated without the mutex decides nothing once an input was claimed for unlink or the
// switch changed before the re-check; no rollback line follows, and a later pass takes the decision.
TEST(FileTierStore, ScrubberRollbackRechecksAfterValidation)
{
    for(const bool claimed: {true, false})
    {
        SCOPED_TRACE(claimed ? "an input claimed for unlink" : "the switch rolled back by another pass");
        auto directory = TestDirectory();
        auto allow = std::make_shared<std::atomic<bool>>(false);
        HeldInputLoad load;
        auto store = OpenWithHeldUnlinks(*directory, allow, {}, load.hook());
        ASSERT_TRUE(store.ok());
        const auto inputs = PublishWindows(**store, 4);
        (void)(*store)->compactOnce(Eager());
        const auto committed = Effective(**store);
        ASSERT_EQ(committed.size(), 1u);
        const auto output = committed[0].file;
        Corrupt(*directory / output);
        load.arm(inputs);
        auto scrub = std::async(std::launch::async, [&] { return (*store)->scrubOnce(0); });
        absl::StatusOr<ScrubResult> other = absl::UnknownError("no other pass");
        if(load.held())
        {
            auto work = std::async(std::launch::async,
                                   [&]
                                   {
                                       if(claimed)
                                       {
                                           *allow = true;
                                           (void)(*store)->retryDeletedFiles();
                                       }
                                       else
                                           other = (*store)->scrubOnce(0);
                                   });
            EXPECT_EQ(work.wait_for(std::chrono::seconds(60)), std::future_status::ready);
            load.release();
        }
        else
            ADD_FAILURE() << "the scrubber never validated the rollback inputs";
        load.release();
        const auto scrubbed = scrub.get();
        ASSERT_TRUE(scrubbed.ok()) << scrubbed.status();
        EXPECT_EQ(scrubbed->rolled_back, 0u);
        EXPECT_EQ(scrubbed->lost, 0u);
        EXPECT_EQ(scrubbed->skipped, 1u);
        if(claimed)
        {
            for(const auto& input: inputs) EXPECT_FALSE(fs::exists(*directory / input.file)) << input.file;
            EXPECT_EQ(RollbackLines(*directory), 0u);
            EXPECT_FALSE(scrubbed->marked);
            EXPECT_EQ(StateOf(**store, output), ManifestState::Published);
            // The next pass finds the inputs gone and records the output Lost.
            const auto next = (*store)->scrubOnce(0);
            ASSERT_TRUE(next.ok()) << next.status();
            EXPECT_EQ(next->lost, 1u);
            EXPECT_EQ(next->rolled_back, 0u);
            EXPECT_TRUE(next->marked);
            EXPECT_EQ(StateOf(**store, output), ManifestState::Lost);
            EXPECT_EQ(RollbackLines(*directory), 0u);
        }
        else
        {
            ASSERT_TRUE(other.ok()) << other.status();
            EXPECT_EQ(other->rolled_back, 1u);
            EXPECT_EQ(RollbackLines(*directory), 1u);
            EXPECT_TRUE(scrubbed->marked);
            for(const auto& input: inputs) EXPECT_EQ(StateOf(**store, input.file), ManifestState::Published);
        }
    }
}

TEST(FileTierStore, ScrubberDuringMigrationOrCompactionCleanupWritesNoLost)
{
    for(const bool migrate: {false, true})
    {
        SCOPED_TRACE(migrate);
        auto directory = TestDirectory();
        const auto tier = MakeSlowTier(*directory / "slow");
        FileTierStore* sut = nullptr;
        bool moved = false;
        FileTierStore::Hooks hooks;
        // The pass has planned every file; they are compacted away, or the first one migrates, before it reads them.
        hooks.scrub_step = [&](std::string_view)
        {
            if(std::exchange(moved, true))
                return absl::OkStatus();
            if(migrate)
            {
                auto migrated = sut->migrateOnce("slow");
                EXPECT_TRUE(migrated.ok()) << migrated.status();
                EXPECT_EQ(migrated.ok() ? *migrated : 0, 1u);
            }
            else
            {
                auto compacted = sut->compactOnce(Eager());
                EXPECT_TRUE(compacted.ok()) << compacted.status();
                EXPECT_EQ(compacted.ok() ? compacted->inputs : 0, 3u);
            }
            return absl::OkStatus();
        };
        auto store = OpenStore(*directory / "local", {}, std::make_shared<ProtoChunkCodec>());
        ASSERT_TRUE(store.ok());
        const auto records = PublishWindows(**store, 3);
        const auto before = (*store)->read(1, kAll).value();
        AttachTier(**store, *directory / "local", tier);
        store->reset();
        store = OpenStore(*directory / "local", hooks, std::make_shared<ProtoChunkCodec>());
        ASSERT_TRUE(store.ok()) << store.status();
        sut = store->get();
        ASSERT_TRUE((*store)->configureTiers("test", {tier}).ok());
        ASSERT_TRUE((*store)->probeTiers().ok());
        const auto scrubbed = (*store)->scrubOnce(0);
        ASSERT_TRUE(scrubbed.ok()) << scrubbed.status();
        EXPECT_TRUE(moved);
        EXPECT_EQ(scrubbed->lost, 0u);
        EXPECT_EQ(scrubbed->rolled_back, 0u);
        EXPECT_TRUE(scrubbed->marked);
        for(const auto& record: Effective(**store)) EXPECT_EQ(record.state, ManifestState::Published);
        EXPECT_EQ(Bytes(*directory / "local/manifest/primary.log").find("\"state\":4"), std::string::npos);
        EXPECT_TRUE(SameEvents((*store)->read(1, kAll).value(), before));
    }
}

TEST(FileTierStore, PeerOpenDuringMigrationWritesNoLost)
{
    auto directory = TestDirectory();
    const auto tier = MakeSlowTier(*directory / "slow");
    auto store = OpenStore(*directory / "local", {}, std::make_shared<ProtoChunkCodec>());
    ASSERT_TRUE(store.ok());
    const auto records = PublishWindows(**store, 1);
    AttachTier(**store, *directory / "local", tier);
    // The owner's mark is withdrawn so the peer's recovery reads the file; the migration lands while it does.
    fs::remove(*directory / "local/manifest/primary.validated");
    std::atomic<int> migrations{0};
    auto load = [&](const fs::path& path) -> absl::StatusOr<ChunkBytes>
    {
        if(path.filename() == fs::path(records[0].file).filename() && migrations++ == 0)
        {
            std::ofstream(*directory / "local/manifest/primary.validated") << R"({"writer":"primary","through":1})";
            auto migrated = (*store)->migrateOnce("slow");
            EXPECT_TRUE(migrated.ok()) << migrated.status();
            EXPECT_EQ(migrated.ok() ? *migrated : 0, 1u);
        }
        return LoadChunkFile(path);
    };
    auto peer = OpenStore(*directory / "local", {}, std::make_shared<ProtoChunkCodec>(), "secondary", load);
    ASSERT_TRUE(peer.ok()) << peer.status();
    EXPECT_EQ(migrations.load(), 1);
    EXPECT_FALSE(fs::exists(*directory / "local" / records[0].file));
    const auto effective = Effective(**peer);
    ASSERT_EQ(effective.size(), 1u);
    EXPECT_EQ(effective[0].state, ManifestState::Published);
    EXPECT_EQ(Bytes(*directory / "local/manifest/secondary.log").find("\"state\":4"), std::string::npos);
    EXPECT_TRUE((*store)->location(records[0].file).value().has_value());
}

TEST(FileTierStore, FileCorruptedAfterItsMarkIsFoundByTheScrubberAndFailsARead)
{
    auto directory = TestDirectory();
    auto store = OpenStore(*directory);
    ASSERT_TRUE(store.ok());
    const auto records = PublishWindows(**store, 2);
    const auto w = (*store)->contiguousWatermark(1).value();
    ASSERT_TRUE((*store)->scrubOnce(0).ok());
    ASSERT_TRUE((*store)->compact().ok());
    store->reset();
    Corrupt(*directory / records[1].file);
    LoadCounter counter;
    store = OpenStore(*directory, {}, std::make_shared<HDF5ChunkCodec>(), "primary", counter.hook());
    ASSERT_TRUE(store.ok()) << store.status();
    // Open trusted the mark: nothing was read and the record is still Published. The read fails all the same, which
    // the Player reports SOURCE_FAILED (I6.14).
    EXPECT_EQ(counter.total(), 0);
    EXPECT_EQ(StateOf(**store, records[1].file), ManifestState::Published);
    EXPECT_EQ((*store)->read(1, kAll).status().code(), absl::StatusCode::kUnavailable);
    const auto scrubbed = (*store)->scrubOnce(0);
    ASSERT_TRUE(scrubbed.ok()) << scrubbed.status();
    EXPECT_EQ(scrubbed->lost, 1u);
    EXPECT_EQ(scrubbed->validated, 1u);
    EXPECT_EQ(StateOf(**store, records[1].file), ManifestState::Lost);
    EXPECT_EQ((*store)->contiguousWatermark(1).value(), w);
    EXPECT_TRUE((*store)->incomplete(1, kAll).value());
}

namespace
{
// The files of story 1 a migrate_v1 line names.
std::vector<ManifestRecord> Migrated(FileTierStore& store)
{
    std::vector<ManifestRecord> moved;
    for(const auto& record: Effective(store))
        if(auto location = store.location(record.file); location.ok() && location->has_value())
            moved.push_back(record);
    return moved;
}

// migrateOnce moves one file per call; this drains every eligible file to the tier.
void MigrateAll(FileTierStore& store)
{
    for(;;)
    {
        auto moved = store.migrateOnce("slow");
        ASSERT_TRUE(moved.ok()) << moved.status();
        if(!*moved)
            return;
    }
}

// The `local` marker a store opened with a deployment id or a Player's tier table requires.
TierConfig MakeLocalTier(const fs::path& root)
{
    fs::create_directories(root);
    struct statfs info
    {
    };
    EXPECT_EQ(::statfs(root.c_str(), &info), 0);
    nlohmann::json marker{{"deployment_id", "test"},
                          {"name", "local"},
                          {"rank", 0},
                          {"kind", "posix"},
                          {"tier_uuid", "local-uuid"},
                          {"f_type", info.f_type}};
    std::ofstream(root / ".chronolog-tier.json") << marker.dump();
    return {"local", "posix", root, 0, "local-uuid"};
}

// A blocking fault seam for tier executor operations: every tier step waits until release().
struct TierHang
{
    std::shared_ptr<std::promise<void>> released = std::make_shared<std::promise<void>>();
    std::shared_future<void> gate = released->get_future().share();
    std::shared_ptr<std::atomic<int>> entered = std::make_shared<std::atomic<int>>(0);
    std::shared_ptr<std::atomic<int>> left = std::make_shared<std::atomic<int>>(0);
    FileTierStore::Hooks hooks() const
    {
        FileTierStore::Hooks hooks;
        hooks.tier_step = [gate = gate, entered = entered, left = left](std::string_view)
        {
            ++*entered;
            gate.wait();
            ++*left;
            return absl::OkStatus();
        };
        return hooks;
    }
    bool waitEntered(int count) const
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while(entered->load() < count && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return entered->load() >= count;
    }
    // Releases every blocked operation and waits until each one left the seam.
    bool release() const
    {
        released->set_value();
        return settled();
    }
    bool settled() const
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while(left->load() < entered->load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return left->load() == entered->load();
    }
};
} // namespace

// I13.14 (RFC-I 3.4, O2): a fresh reader applies a writer's log before its snapshot, a polling reader the snapshot
// before the log, and manifest compaction moves lines from one to the other. The effective location is the slowest
// rank any line names, whatever the order.
TEST(ManifestLog, MultiHopLocationIsOrderIndependentAcrossSnapshotCompaction)
{
    auto directory = TestDirectory();
    std::string file;
    {
        auto store = OpenStore(*directory, {}, std::make_shared<ProtoChunkCodec>());
        ASSERT_TRUE(store.ok());
        file = PublishWindows(**store, 1).at(0).file;
    }
    auto log = ManifestLog::Open(*directory, "primary");
    ASSERT_TRUE(log.ok()) << log.status();
    const auto hop = [&file](std::string tier, uint32_t rank)
    {
        MigrationLocation location;
        location.writer = "primary";
        location.file = file;
        location.tier = tier;
        location.tier_uuid = "uuid-" + tier;
        location.token = "token-" + tier;
        location.story_id = 1;
        location.rank = rank;
        location.checksum = {10, 20};
        return location;
    };
    const auto resolved = [&file](ManifestLog& reader, bool fresh) -> std::string
    {
        if(fresh)
        {
            auto index = reader.load();
            EXPECT_TRUE(index.ok()) << index.status();
            return index.ok() && index->locations.contains(file) ? index->locations.at(file).tier : "local";
        }
        auto index = reader.sync();
        EXPECT_TRUE(index.ok()) << index.status();
        return index.ok() && (*index)->locations.contains(file) ? (*index)->locations.at(file).tier : "local";
    };
    // Three polling readers that first sync at different points of the history.
    auto before = ManifestLog::OpenReadOnly(*directory);
    EXPECT_EQ(resolved(*before, false), "local");
    ASSERT_TRUE((*log)->appendMigration(hop("nfs", 1)).ok());
    auto between = ManifestLog::OpenReadOnly(*directory);
    EXPECT_EQ(resolved(*between, false), "nfs");
    // The first hop moves into the snapshot; the second hop is the only line in the log.
    ASSERT_TRUE((*log)->sync().ok());
    ASSERT_TRUE((*log)->compact().ok());
    ASSERT_TRUE((*log)->appendMigration(hop("s3", 2)).ok());
    auto after = ManifestLog::OpenReadOnly(*directory);
    for(auto* reader: {before.get(), between.get(), after.get()}) EXPECT_EQ(resolved(*reader, false), "s3");
    EXPECT_EQ(resolved(*ManifestLog::OpenReadOnly(*directory), true), "s3");
    ASSERT_TRUE((*log)->sync().ok());
    EXPECT_EQ((*log)->location(file).value().tier, "s3");
    // Reversed arrangement: the slower hop is in the snapshot and a line of the faster rank follows it in the log
    // (a retried append of the first hop). The file never moves back to a faster tier.
    ASSERT_TRUE((*log)->sync().ok());
    ASSERT_TRUE((*log)->compact().ok());
    ASSERT_TRUE((*log)->appendMigration(hop("nfs", 1)).ok());
    for(auto* reader: {before.get(), between.get(), after.get()}) EXPECT_EQ(resolved(*reader, false), "s3");
    EXPECT_EQ(resolved(*ManifestLog::OpenReadOnly(*directory), true), "s3");
    auto reopened = ManifestLog::OpenReadOnly(*directory);
    EXPECT_EQ(resolved(*reopened, false), "s3");
    ASSERT_TRUE((*log)->sync().ok());
    EXPECT_EQ((*log)->location(file).value().tier, "s3");
    EXPECT_EQ((*log)->location(file).value().rank, 2u);
    // Everything in the snapshot.
    ASSERT_TRUE((*log)->compact().ok());
    for(auto* reader: {before.get(), between.get(), after.get(), reopened.get()})
        EXPECT_EQ(resolved(*reader, false), "s3");
    EXPECT_EQ(resolved(*ManifestLog::OpenReadOnly(*directory), true), "s3");
}

// I13.14 (RFC-I 3.4, O10): a Keeper resending a window whose holder migrated is compared against the holder at its
// effective location. While that tier is unavailable the transfer is UNAVAILABLE and writes nothing, so the one
// receipt settles when the tier returns.
TEST(FileTierStore, PublishDuplicateOfAMigratedWindowSettles)
{
    auto directory = TestDirectory();
    const auto tier = MakeSlowTier(*directory / "slow");
    auto store = OpenStore(*directory / "local", {}, std::make_shared<ProtoChunkCodec>());
    ASSERT_TRUE(store.ok());
    const auto records = PublishWindows(**store, 1);
    AttachTier(**store, *directory / "local", tier);
    MigrateAll(**store);
    ASSERT_EQ(Migrated(**store).size(), 1u);
    ASSERT_FALSE(fs::exists(*directory / "local" / records[0].file));
    const auto manifest = Bytes(*directory / "local/manifest/primary.log");
    const auto settled = (*store)->publish(Rich(0));
    ASSERT_TRUE(settled.ok()) << settled.status();
    EXPECT_EQ(settled->file, records[0].file);
    EXPECT_EQ(settled->state, ManifestState::Published);
    // A resend with other events for the same window is refused against the migrated holder.
    auto other = Rich(0);
    other.events.front().envelope.payload = "different";
    EXPECT_EQ((*store)->publish(other).status().code(), absl::StatusCode::kUnavailable);
    // The tier goes away: the duplicate cannot be compared, nothing is written, the Keeper keeps its chunk.
    fs::rename(tier.root, tier.root.string() + "-away");
    EXPECT_FALSE((*store)->probeTiers().ok());
    EXPECT_EQ((*store)->publish(Rich(0)).status().code(), absl::StatusCode::kUnavailable);
    // It returns: the same transfer settles on the migrated holder.
    fs::rename(tier.root.string() + "-away", tier.root);
    ASSERT_TRUE((*store)->probeTiers().ok());
    const auto again = (*store)->publish(Rich(0));
    ASSERT_TRUE(again.ok()) << again.status();
    EXPECT_EQ(again->file, records[0].file);
    EXPECT_FALSE(fs::exists(*directory / "local" / records[0].file));
    EXPECT_EQ(Bytes(*directory / "local/manifest/primary.log"), manifest);
    EXPECT_EQ(Effective(**store).size(), 1u);
    EXPECT_EQ((*store)->contiguousWatermark(1).value(), (Hlc{150, 0}));
}

// I13.15 (RFC-I 3.5, O4): a probe that has not answered keeps the tier unavailable and starts no second probe, so a
// dead server costs one probe thread per tier. The marker is a FIFO without a writer: opening it blocks as a hard
// mount does.
TEST(FileTierStore, AtMostOneProbePerTier)
{
    auto directory = TestDirectory();
    const auto config = MakeSlowTier(*directory / "slow");
    const auto marker = config.root / ".chronolog-tier.json";
    const auto identity = Bytes(marker);
    fs::rename(marker, config.root / "marker.saved");
    ASSERT_EQ(::mkfifo(marker.c_str(), 0600), 0);
    auto tier = std::make_shared<PosixTier>(config, "test", 2, std::chrono::milliseconds(100));
    EXPECT_FALSE(tier->probe().ok());
    EXPECT_EQ(tier->directory(), nullptr);
    for(int i = 0; i < 8; ++i)
    {
        const auto status = tier->probe();
        EXPECT_EQ(status.code(), absl::StatusCode::kUnavailable);
        EXPECT_NE(status.message().find("already outstanding"), std::string::npos) << status;
        EXPECT_EQ(tier->directory(), nullptr);
    }
    EXPECT_EQ(tier->probesStarted(), 1u);
    // Two workers and one of them lost to the probe: the executor still runs work, which a second hung probe would
    // have made impossible.
    EXPECT_TRUE(tier->run([] { return absl::OkStatus(); }).ok());
    // The server answers: the one outstanding probe returns, and it does not start an epoch it was abandoned in.
    {
        tier_detail::Fd writer(::open(marker.c_str(), O_WRONLY | O_CLOEXEC));
        ASSERT_GE(writer.get(), 0);
        fs::rename(config.root / "marker.saved", marker);
        ASSERT_TRUE(tier_detail::WriteAll(writer.get(), identity).ok());
    }
    absl::Status status;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        status = tier->probe();
    } while(!status.ok() && status.message().find("already outstanding") != std::string::npos &&
            std::chrono::steady_clock::now() < deadline);
    EXPECT_TRUE(status.ok()) << status;
    EXPECT_NE(tier->directory(), nullptr);
    EXPECT_EQ(tier->probesStarted(), 2u);
    tier->stop();
}

// I13.15 (RFC-I 3.5, O4): workers abandoned on a hung tier are capped at the tier's thread count. At the cap new work
// fails at once, never runs, and the tier stays unavailable until one abandoned worker returns.
TEST(FileTierStore, AbandonedWorkersAreCapped)
{
    auto directory = TestDirectory();
    const auto config = MakeSlowTier(*directory / "slow");
    auto tier = std::make_shared<PosixTier>(config, "test", 2, std::chrono::milliseconds(50));
    ASSERT_TRUE(tier->probe().ok());
    std::array<std::promise<void>, 2> release;
    auto entered = std::make_shared<std::atomic<int>>(0);
    auto left = std::make_shared<std::atomic<int>>(0);
    for(auto& promise: release)
    {
        const auto status = tier->run([gate = promise.get_future().share(), entered, left]
        {
            ++*entered;
            gate.wait();
            ++*left;
            return absl::OkStatus();
        });
        EXPECT_EQ(status.code(), absl::StatusCode::kUnavailable);
        // The first hang ends the epoch; a probe of the healthy root starts the next while one worker remains.
        if(&promise == &release.front())
        {
            EXPECT_EQ(tier->directory(), nullptr);
            EXPECT_TRUE(tier->probe().ok());
        }
    }
    ASSERT_EQ(entered->load(), 2);
    // At the cap: nothing more is started, and the healthy root cannot become available.
    auto extra = std::make_shared<std::atomic<int>>(0);
    for(int i = 0; i < 8; ++i)
    {
        const auto status = tier->run([extra]
        {
            ++*extra;
            return absl::OkStatus();
        });
        EXPECT_EQ(status.code(), absl::StatusCode::kUnavailable);
        EXPECT_NE(status.message().find("tier executor is full"), std::string::npos) << status;
        EXPECT_FALSE(tier->probe().ok());
        EXPECT_EQ(tier->directory(), nullptr);
    }
    EXPECT_EQ(extra->load(), 0);
    EXPECT_EQ(entered->load(), 2);
    // One abandoned worker returns: the tier takes work again.
    release[0].set_value();
    absl::Status status;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        status = tier->probe();
    } while(!status.ok() && std::chrono::steady_clock::now() < deadline);
    EXPECT_TRUE(status.ok()) << status;
    EXPECT_NE(tier->directory(), nullptr);
    EXPECT_TRUE(tier->run([extra]
    {
        ++*extra;
        return absl::OkStatus();
    }).ok());
    EXPECT_EQ(extra->load(), 1);
    release[1].set_value();
    while(left->load() < 2 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    EXPECT_EQ(left->load(), 2);
    tier->stop();
}

// I13.15 (RFC-I 3.5, O4): nothing at Open, destroy, compaction or shutdown waits on a slow tier. Every tier
// operation hangs in the executor seam and tier_io_timeout is an hour, so a path that awaited the tier would never
// return; the test ends only because none does.
TEST(FileTierStore, HungTierDoesNotBlockOpenDestroyCompactionOrShutdown)
{
    auto directory = TestDirectory();
    const auto tier = MakeSlowTier(*directory / "slow");
    const auto hour = std::chrono::milliseconds(3600 * 1000);
    std::vector<ManifestRecord> migrated;
    {
        auto store = OpenStore(*directory / "local", {}, std::make_shared<ProtoChunkCodec>());
        ASSERT_TRUE(store.ok());
        PublishWindows(**store, 2);
        AttachTier(**store, *directory / "local", tier);
        MigrateAll(**store);
        migrated = Migrated(**store);
        ASSERT_EQ(migrated.size(), 2u);
        PublishWindows(**store, 3, 2);
        ASSERT_TRUE((*store)->scrubOnce(0).ok());
    }
    MakeLocalTier(*directory / "local");
    TierHang hang;
    const auto open = [&]
    {
        return FileTierStore::Open(*directory / "local",
                                   "primary",
                                   {{1, {100, 0}}},
                                   std::make_shared<ProtoChunkCodec>(),
                                   {},
                                   {},
                                   2,
                                   {},
                                   hang.hooks(),
                                   TierChain{"test", {tier}, 2, hour});
    };
    auto store = open();
    ASSERT_TRUE(store.ok()) << store.status();
    ASSERT_TRUE((*store)->probeTiers().ok());
    // Destroy of a migrated file: the Deleted record is durable at once and its unlink hangs on the tier.
    EXPECT_FALSE((*store)->eraseFile(migrated[0].file).ok());
    ASSERT_TRUE(hang.waitEntered(1));
    EXPECT_EQ(StateOf(**store, migrated[0].file), ManifestState::Deleted);
    EXPECT_TRUE((*store)->hasPendingUnlinks(1).value());
    EXPECT_FALSE((*store)->retryDeletedFiles().ok());
    EXPECT_TRUE(fs::exists(tier.root / migrated[0].file));
    // Local reads and compaction proceed beside the hung operation.
    const auto local = Effective(**store).back();
    ASSERT_FALSE((*store)->location(local.file).value().has_value());
    auto read = (*store)->readRecord(local, kAll);
    ASSERT_TRUE(read.ok()) << read.status();
    EXPECT_TRUE(SameEvents(*read, Rich(4).events));
    auto compacted = (*store)->compactOnce(Eager());
    ASSERT_TRUE(compacted.ok()) << compacted.status();
    EXPECT_EQ(compacted->inputs, 3u);
    // Shutdown with the tier worker still inside the seam.
    store->reset();
    EXPECT_EQ(hang.left->load(), 0);
    // Open with a pending slow-tier unlink, then the hung tier again: Open, the destroy of the story and its tier
    // sweep all return.
    store = open();
    ASSERT_TRUE(store.ok()) << store.status();
    EXPECT_TRUE((*store)->hasPendingUnlinks(1).value());
    ASSERT_TRUE((*store)->probeTiers().ok());
    EXPECT_EQ(StateOf(**store, migrated[0].file), ManifestState::Deleted);
    EXPECT_EQ(StateOf(**store, migrated[1].file), ManifestState::Published);
    EXPECT_FALSE((*store)->retryDeletedFiles().ok());
    ASSERT_TRUE((*store)->tombstone(1).ok());
    // The destroy worker erases every remaining file: each Deleted record is durable at once, the migrated file's
    // unlink queues behind the hung tier and a file that never left `local` is unlinked at once (I13.14).
    for(const auto& record: Effective(**store))
        if(record.state == ManifestState::Published || record.state == ManifestState::Empty)
            (void)(*store)->eraseFile(record.file);
    for(const auto& record: Effective(**store)) EXPECT_EQ(record.state, ManifestState::Deleted) << record.file;
    EXPECT_FALSE((*store)->retryDeletedFiles().ok());
    EXPECT_TRUE((*store)->hasPendingUnlinks(1).value());
    EXPECT_GE((*store)->pendingTierDeletions()[1], 1u);
    EXPECT_TRUE(fs::exists(tier.root / migrated[0].file));
    EXPECT_TRUE(fs::exists(tier.root / migrated[1].file));
    EXPECT_EQ(hang.left->load(), 0);
    // The tier answers: the destroy worker's retries complete every pending deletion and the tier sweep.
    ASSERT_TRUE(hang.release());
    absl::Status drained;
    bool pending = true;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        drained = (*store)->awaitTierUnlinksForTesting(std::chrono::seconds(5));
        pending = (*store)->hasPendingUnlinks(1).value();
    } while((!drained.ok() || pending) && std::chrono::steady_clock::now() < deadline);
    EXPECT_TRUE(drained.ok()) << drained;
    EXPECT_FALSE(pending);
    EXPECT_FALSE(fs::exists(tier.root / migrated[0].file));
    EXPECT_FALSE(fs::exists(tier.root / migrated[1].file));
    store->reset();
    EXPECT_TRUE(hang.settled());
}

// I13.14, I13.15, I13.16: an erase targets the file's effective location and the faster tiers a stale copy may
// remain on, so a file that never left `local` is unlinked in its erase call while one slow tier hangs and another is
// unavailable, and a copy an abandoned attempt left on the unavailable tier is the sweep's once that tier returns.
TEST(FileTierStore, LocalEraseDoesNotWaitOnAHungOrUnavailableSlowTier)
{
    auto directory = TestDirectory();
    const auto local = *directory / "local";
    const auto slow = MakeSlowTier(*directory / "slow");
    // A root without its marker is unavailable (I13.15).
    fs::create_directories(*directory / "cold");
    const TierConfig cold{"cold", "posix", *directory / "cold", 2, "cold-uuid"};
    TierHang hang;
    auto store = OpenStore(local, hang.hooks(), std::make_shared<ProtoChunkCodec>());
    ASSERT_TRUE(store.ok()) << store.status();
    const auto records = PublishWindows(**store, 2);
    ASSERT_TRUE((*store)->scrubOnce(0).ok());
    ASSERT_TRUE((*store)->configureTiers("test", {slow, cold}, 2, std::chrono::hours(1)).ok());
    EXPECT_FALSE((*store)->probeTiers().ok());
    auto migrated = (*store)->migrateOnce("slow");
    ASSERT_TRUE(migrated.ok()) << migrated.status();
    ASSERT_EQ(*migrated, 1u);
    const auto moved = Migrated(**store);
    ASSERT_EQ(moved.size(), 1u);
    const auto kept = moved[0].file == records[0].file ? records[1] : records[0];
    ASSERT_FALSE((*store)->location(kept.file).value().has_value());
    fs::create_directories((cold.root / kept.file).parent_path());
    fs::copy_file(local / kept.file, cold.root / kept.file);
    // The migrated file's erase waits on its own tier, which hangs.
    EXPECT_FALSE((*store)->eraseFile(moved[0].file).ok());
    ASSERT_TRUE(hang.waitEntered(1));
    EXPECT_TRUE(fs::exists(slow.root / moved[0].file));
    // The local-only file is unlinked in its erase call with no slow-tier work.
    auto erased = (*store)->eraseFile(kept.file);
    EXPECT_TRUE(erased.ok()) << erased;
    EXPECT_FALSE(fs::exists(local / kept.file));
    EXPECT_EQ(StateOf(**store, kept.file), ManifestState::Deleted);
    EXPECT_EQ(hang.entered->load(), 1);
    ASSERT_TRUE(hang.release());
    auto drained = (*store)->awaitTierUnlinksForTesting(std::chrono::seconds(5));
    EXPECT_TRUE(drained.ok()) << drained;
    EXPECT_FALSE((*store)->hasPendingUnlinks(1).value());
    EXPECT_FALSE(fs::exists(slow.root / moved[0].file));
    EXPECT_TRUE(fs::exists(cold.root / kept.file));
    MakeSlowTier(cold.root, cold.name, cold.rank, cold.tier_uuid);
    ASSERT_TRUE((*store)->probeTiers().ok());
    auto swept = (*store)->sweepTiers();
    EXPECT_TRUE(swept.ok()) << swept;
    EXPECT_FALSE(fs::exists(cold.root / kept.file));
    store->reset();
    EXPECT_TRUE(hang.settled());
}

// I13.15 (RFC-I 3.5, F1, O3c, O3d): a slow-tier file is recorded Lost only by its owner, when it is missing or
// corrupt through the epoch's descriptor with the marker intact, and only when the verdict repeats in a later pass
// under a probe that started after the first.
TEST(FileTierStore, SlowTierFileIsLostOnlyOnASecondVerdictUnderAFreshProbe)
{
    for(const bool corrupt: {false, true})
    {
        SCOPED_TRACE(corrupt);
        auto directory = TestDirectory();
        const auto tier = MakeSlowTier(*directory / "slow");
        auto store = OpenStore(*directory / "local", {}, std::make_shared<ProtoChunkCodec>());
        ASSERT_TRUE(store.ok());
        PublishWindows(**store, 2);
        AttachTier(**store, *directory / "local", tier);
        MigrateAll(**store);
        const auto migrated = Migrated(**store);
        ASSERT_EQ(migrated.size(), 2u);
        const auto w = (*store)->contiguousWatermark(1).value();
        auto intact = (*store)->scrubOnce(0, true);
        ASSERT_TRUE(intact.ok()) << intact.status();
        EXPECT_EQ(intact->validated, 2u);
        EXPECT_EQ(intact->slow_failed, 0u);
        if(corrupt)
            Corrupt(tier.root / migrated[1].file);
        else
            fs::remove(tier.root / migrated[1].file);
        // Neither a peer Grapher nor a Player records anything for another writer's slow-tier file.
        {
            auto peer = OpenStore(*directory / "local", {}, std::make_shared<ProtoChunkCodec>(), "peer");
            ASSERT_TRUE(peer.ok()) << peer.status();
            ASSERT_TRUE((*peer)->configureTiers("test", {tier}).ok());
            for(int pass = 0; pass < 3; ++pass)
            {
                ASSERT_TRUE((*peer)->probeTiers().ok());
                auto scrubbed = (*peer)->scrubOnce(0, true);
                ASSERT_TRUE(scrubbed.ok()) << scrubbed.status();
                EXPECT_EQ(scrubbed->lost, 0u);
            }
            TierChain chain{"test", {MakeLocalTier(*directory / "local"), tier}};
            auto player = FileTierStore::OpenReadOnly(*directory / "local",
                                                      std::chrono::milliseconds(1),
                                                      {},
                                                      0,
                                                      {},
                                                      std::chrono::milliseconds(30000),
                                                      chain);
            ASSERT_TRUE(player.ok()) << player.status();
            ASSERT_TRUE((*player)->probeTiers().ok());
            EXPECT_FALSE((*player)->readRecord(migrated[1], kAll).ok());
            EXPECT_FALSE((*player)->scrubOnce(0, true).ok());
        }
        EXPECT_EQ(StateOf(**store, migrated[1].file), ManifestState::Published);
        // First verdict: reported, nothing recorded, and it does not repeat without a later probe.
        for(int pass = 0; pass < 3; ++pass)
        {
            auto scrubbed = (*store)->scrubOnce(0, true);
            ASSERT_TRUE(scrubbed.ok()) << scrubbed.status();
            EXPECT_EQ(scrubbed->lost, 0u);
            EXPECT_EQ(scrubbed->slow_failed, 1u);
            EXPECT_EQ(scrubbed->validated, 1u);
            EXPECT_EQ(StateOf(**store, migrated[1].file), ManifestState::Published);
        }
        // A later probe, but the verdict interval has not passed.
        ASSERT_TRUE((*store)->probeTiers().ok());
        auto early = (*store)->scrubOnce(0, true, std::chrono::hours(1));
        ASSERT_TRUE(early.ok()) << early.status();
        EXPECT_EQ(early->lost, 0u);
        EXPECT_EQ(StateOf(**store, migrated[1].file), ManifestState::Published);
        // Second verdict under the later probe.
        auto second = (*store)->scrubOnce(0, true);
        ASSERT_TRUE(second.ok()) << second.status();
        EXPECT_EQ(second->lost, 1u);
        EXPECT_EQ(second->slow_failed, 0u);
        EXPECT_EQ(second->validated, 1u);
        EXPECT_EQ(StateOf(**store, migrated[1].file), ManifestState::Lost);
        EXPECT_EQ(StateOf(**store, migrated[0].file), ManifestState::Published);
        EXPECT_EQ((*store)->contiguousWatermark(1).value(), w);
        EXPECT_TRUE((*store)->incomplete(1, kAll).value());
        // The verdict survives a restart and is not repeated.
        store->reset();
        store = OpenStore(*directory / "local", {}, std::make_shared<ProtoChunkCodec>());
        ASSERT_TRUE(store.ok()) << store.status();
        EXPECT_EQ(StateOf(**store, migrated[1].file), ManifestState::Lost);
        ASSERT_TRUE((*store)->configureTiers("test", {tier}).ok());
        ASSERT_TRUE((*store)->probeTiers().ok());
        auto after = (*store)->scrubOnce(0, true);
        ASSERT_TRUE(after.ok()) << after.status();
        EXPECT_EQ(after->lost, 0u);
        EXPECT_EQ(after->slow_failed, 0u);
    }
}

// I13.15 (O3c): one miss between two probes (automount expiry, a remount) is not a loss, and a miss after the file
// was seen intact again starts over as a first verdict.
TEST(FileTierStore, TransientMissIsNotLost)
{
    auto directory = TestDirectory();
    const auto tier = MakeSlowTier(*directory / "slow");
    auto store = OpenStore(*directory / "local", {}, std::make_shared<ProtoChunkCodec>());
    ASSERT_TRUE(store.ok());
    PublishWindows(**store, 1);
    AttachTier(**store, *directory / "local", tier);
    MigrateAll(**store);
    const auto migrated = Migrated(**store);
    ASSERT_EQ(migrated.size(), 1u);
    const auto file = tier.root / migrated[0].file;
    const auto away = fs::path(file.string() + ".away");
    for(int round = 0; round < 3; ++round)
    {
        SCOPED_TRACE(round);
        fs::rename(file, away);
        auto missed = (*store)->scrubOnce(0, true);
        ASSERT_TRUE(missed.ok()) << missed.status();
        EXPECT_EQ(missed->slow_failed, 1u);
        EXPECT_EQ(missed->lost, 0u);
        fs::rename(away, file);
        ASSERT_TRUE((*store)->probeTiers().ok());
        auto seen = (*store)->scrubOnce(0, true);
        ASSERT_TRUE(seen.ok()) << seen.status();
        EXPECT_EQ(seen->validated, 1u);
        EXPECT_EQ(seen->lost, 0u);
        ASSERT_TRUE((*store)->probeTiers().ok());
    }
    EXPECT_EQ(StateOf(**store, migrated[0].file), ManifestState::Published);
    auto read = (*store)->readRecord(migrated[0], kAll);
    ASSERT_TRUE(read.ok()) << read.status();
    EXPECT_TRUE(SameEvents(*read, Rich(0).events));
}

// I13.15 (F1): the marker probe succeeds, the file read returns ENOENT and the marker re-read through the same
// descriptor fails. That is an unavailable tier, in every pass, never a miss.
TEST(FileTierStore, ProbeOkThenMissThenProbeFailWritesNoLost)
{
    auto directory = TestDirectory();
    const auto tier = MakeSlowTier(*directory / "slow");
    auto fail = std::make_shared<std::atomic<bool>>(false);
    FileTierStore::Hooks hooks;
    hooks.tier_errno = [fail](std::string_view step)
    { return !fail->load() ? 0 : step == "lost-open" ? ENOENT : step == "lost-marker" ? EIO : 0; };
    auto store = OpenStore(*directory / "local", hooks, std::make_shared<ProtoChunkCodec>());
    ASSERT_TRUE(store.ok());
    PublishWindows(**store, 1);
    AttachTier(**store, *directory / "local", tier);
    MigrateAll(**store);
    const auto migrated = Migrated(**store);
    ASSERT_EQ(migrated.size(), 1u);
    const auto manifest = Bytes(*directory / "local/manifest/primary.log");
    *fail = true;
    for(int pass = 0; pass < 4; ++pass)
    {
        SCOPED_TRACE(pass);
        ASSERT_TRUE((*store)->probeTiers().ok());
        auto scrubbed = (*store)->scrubOnce(0, true);
        ASSERT_TRUE(scrubbed.ok()) << scrubbed.status();
        EXPECT_EQ(scrubbed->lost, 0u);
        EXPECT_EQ(scrubbed->slow_failed, 1u);
        // The failed marker re-read ended the epoch.
        EXPECT_FALSE((*store)->tierUsage("slow").value().available);
    }
    EXPECT_EQ(Bytes(*directory / "local/manifest/primary.log"), manifest);
    *fail = false;
    ASSERT_TRUE((*store)->probeTiers().ok());
    auto scrubbed = (*store)->scrubOnce(0, true);
    ASSERT_TRUE(scrubbed.ok()) << scrubbed.status();
    EXPECT_EQ(scrubbed->validated, 1u);
    EXPECT_EQ(StateOf(**store, migrated[0].file), ManifestState::Published);
}

// I13.15: ESTALE and EIO at the open mark the tier unavailable and never count as missing, however often they
// repeat under separate probes.
TEST(FileTierStore, EstaleIsNeverMissing)
{
    for(const int error: {ESTALE, EIO})
    {
        SCOPED_TRACE(error);
        auto directory = TestDirectory();
        const auto tier = MakeSlowTier(*directory / "slow");
        auto fail = std::make_shared<std::atomic<bool>>(false);
        FileTierStore::Hooks hooks;
        hooks.tier_errno = [fail, error](std::string_view step)
        { return fail->load() && step == "lost-open" ? error : 0; };
        auto store = OpenStore(*directory / "local", hooks, std::make_shared<ProtoChunkCodec>());
        ASSERT_TRUE(store.ok());
        PublishWindows(**store, 1);
        AttachTier(**store, *directory / "local", tier);
        MigrateAll(**store);
        const auto migrated = Migrated(**store);
        ASSERT_EQ(migrated.size(), 1u);
        // The file is really gone as well: the errno alone decides.
        fs::rename(tier.root / migrated[0].file, tier.root / (migrated[0].file + ".away"));
        *fail = true;
        for(int pass = 0; pass < 4; ++pass)
        {
            SCOPED_TRACE(pass);
            ASSERT_TRUE((*store)->probeTiers().ok());
            auto scrubbed = (*store)->scrubOnce(0, true);
            ASSERT_TRUE(scrubbed.ok()) << scrubbed.status();
            EXPECT_EQ(scrubbed->lost, 0u);
            EXPECT_EQ(scrubbed->slow_failed, 1u);
            EXPECT_FALSE((*store)->tierUsage("slow").value().available);
        }
        EXPECT_EQ(StateOf(**store, migrated[0].file), ManifestState::Published);
    }
}

TEST(ManifestLog, ForeignMigrateLineFailsTheRefreshClosed)
{
    auto directory = TestDirectory();
    auto store = OpenStore(*directory, {}, std::make_shared<ProtoChunkCodec>());
    ASSERT_TRUE(store.ok());
    const auto records = PublishWindows(**store, 1);
    auto reader = ManifestLog::OpenReadOnly(*directory);
    ASSERT_TRUE(reader->sync().ok());
    nlohmann::json body{{"writer", "primary"},
                        {"story", 1},
                        {"file", records[0].file},
                        {"tier", "slow"},
                        {"rank", 1},
                        {"tier_uuid", "uuid"},
                        {"bytes", 0u},
                        {"crc32c", 0u},
                        {"token", "token"}};
    const auto text = body.dump();
    std::ofstream(*directory / "manifest/foreign.log")
            << "{\"length\":" << text.size() << ",\"crc\":" << static_cast<uint32_t>(absl::ComputeCrc32c(text))
            << ",\"migrate_v1\":" << text << "}\n";
    EXPECT_FALSE(reader->sync().ok());
    EXPECT_TRUE(reader->current()->locations.empty());
}
} // namespace chronolog
