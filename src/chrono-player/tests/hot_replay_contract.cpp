// Instantiates ReplayContract against HotReplay over an in-process fake HotSource.
#include <algorithm>
#include <condition_variable>
#include <map>
#include <tuple>
#include <mutex>
#include <filesystem>
#include <fstream>
#include <future>
#include <nlohmann/json.hpp>
#include <sys/vfs.h>
#include <unistd.h>
#include "replay_contract_test.cpp"
#include "chrono-player/replay/HotReplay.h"

namespace chronolog::contract
{
namespace
{

using player::HotFetch;
using player::HotReplay;
using player::HotReplayOptions;
using player::HotSource;
using player::KeeperFetch;
using player::WriterAssignment;

constexpr Epoch kEpoch = 7;

Event ev(uint64_t writer, uint64_t sequence, int64_t hlc)
{
    Event e;
    e.id = {1, writer, 3, sequence};
    e.hlc = {hlc, 0};
    e.physical.physical_ns = hlc;
    e.envelope.payload = "w" + std::to_string(writer) + "s" + std::to_string(sequence);
    e.durability = Durability::Durable;
    return e;
}

class FakeHotSource final: public HotSource
{
public:
    FakeHotSource()
    {
        // Writer 2 owns keeper-a, writer 4 owns keeper-b. The 150 event is unconfirmed hot data,
        // events at 50 and 300 lie outside the query, and each Keeper also holds one overlap duplicate.
        a_.events = {ev(2, 1, 50), ev(2, 2, 120), ev(2, 3, 150), ev(2, 4, 210), ev(2, 5, 250), ev(4, 1, 130)};
        b_.events = {ev(4, 1, 130), ev(4, 2, 210), ev(4, 3, 280), ev(4, 4, 300), ev(2, 4, 210)};
        a_.frontier = {"keeper-a", kEpoch, {200, 0}, true, false};
        b_.frontier = {"keeper-b", kEpoch, {200, 0}, true, false};
        writers_ = {{2, 3, "keeper-a"}, {4, 3, "keeper-b"}};
        view_ = writers_;
    }

    absl::StatusOr<HotFetch> fetch(StoryId story, const Range& range) const override
    {
        std::lock_guard lk(mu_);
        // A Read of a destroyed story fails at admission; a Tail's Keepers refuse it instead (fetchTail).
        if(tombstoned_)
            return absl::FailedPreconditionError("story is tombstoned");
        if(story != 1)
            return absl::NotFoundError("unknown story");
        return snapshot(range.start, nullptr);
    }

    absl::StatusOr<HotFetch> fetchTail(StoryId story, Hlc from, const player::TailStarts& starts) const override
    {
        std::lock_guard lk(mu_);
        if(story != 1)
            return absl::NotFoundError("unknown story");
        return snapshot(from, &starts);
    }

    absl::Status live(StoryId story) const
    {
        std::lock_guard lk(mu_);
        if(tombstoned_)
            return absl::FailedPreconditionError("story is tombstoned");
        return story == 1 ? absl::OkStatus() : absl::FailedPreconditionError("unknown story");
    }

    // Replaces what one Keeper holds and reports; a Keeper set this way answers from the lower bound it is asked for.
    void setKeeper(const std::string& name, const KeeperContents& contents)
    {
        std::lock_guard lk(mu_);
        KeeperFetch* k = name == "keeper-a" ? &a_ : name == "keeper-b" ? &b_ : &c_;
        k->events = contents.visible;
        k->frontier.sealed = contents.sealed;
        k->frontier.truncated = contents.truncated;
        k->frontier.instance = contents.instance;
        if(name == "keeper-c")
        {
            k->frontier.process_id = name;
            k->frontier.answered = true;
            k->frontier.predecessor = true;
            k->frontier.own_cut = contents.own_cut.value_or(Hlc{});
            k->frontier.epoch = k->frontier.expected_epoch = kEpoch - 1;
            has_c_ = true;
        }
        custom_[name] = contents.limit;
    }
    void awaitPolls(unsigned n) const
    {
        std::unique_lock lk(mu_);
        const unsigned target = polls_ + n;
        polled_.wait_for(lk, std::chrono::seconds(10), [&] { return polls_ >= target; });
    }
    Hlc lastPollStart() const
    {
        std::lock_guard lk(mu_);
        return last_from_;
    }
    Hlc lastSourceStart(const std::string& name) const
    {
        std::lock_guard lk(mu_);
        return source_start_.at(name);
    }
    void abandon(Hlc start, Hlc end)
    {
        std::lock_guard lk(mu_);
        abandoned_.push_back(Range{Range::Axis::Hlc, start, end});
    }

    void physicalState(bool policy, int64_t frontier, uint64_t uncertainty)
    {
        std::lock_guard lk(mu_);
        policy_ = policy;
        for(auto* k: {&a_, &b_})
        {
            k->frontier.physical_frontier = frontier;
            for(auto& e: k->events)
            {
                e.physical.status = uncertainty == UINT64_MAX ? ClockStatus::Unsynced : ClockStatus::Synced;
                e.physical.uncertainty_ns =
                        uncertainty == UINT64_MAX ? std::nullopt : std::optional<uint64_t>(uncertainty);
            }
        }
    }
    void setKeeperFrontiers(const std::vector<KeeperFrontier>& frontiers)
    {
        std::lock_guard lk(mu_);
        for(const auto& kf: frontiers)
        {
            auto& k = kf.keeper == "keeper-a" ? a_ : b_;
            k.frontier.epoch = kf.epoch;
            k.frontier.sealed = kf.frontier;
            k.frontier.answered = kf.answered;
        }
    }

    void setWriterFrontiers(const std::vector<Frontier>& frontiers)
    {
        std::lock_guard lk(mu_);
        // A Keeper seal is shared by its writers, so it is the minimum over their entries.
        std::map<std::string, Hlc> seal;
        for(const auto& f: frontiers)
            for(const auto& w: writers_)
                if(w.writer_id == f.writer_id && w.incarnation == f.incarnation)
                {
                    auto [it, fresh] = seal.emplace(w.keeper, f.frontier);
                    if(!fresh)
                        it->second = std::min(it->second, f.frontier);
                }
        for(const auto& [keeper, hlc]: seal) (keeper == "keeper-a" ? a_ : b_).frontier.sealed = hlc;
    }

    void tombstone()
    {
        std::lock_guard lk(mu_);
        tombstoned_ = true;
    }
    void requireArchive()
    {
        std::lock_guard lk(mu_);
        a_.frontier.evicted_below = {200, 0};
    }
    void hideWriter()
    {
        std::lock_guard lk(mu_);
        std::erase_if(view_, [](const WriterAssignment& w) { return w.writer_id == 4; });
    }
    void failB()
    {
        std::lock_guard lk(mu_);
        b_.frontier.answered = false;
    }
    void truncateA()
    {
        std::lock_guard lk(mu_);
        a_.frontier.truncated = true;
    }
    void close()
    {
        std::lock_guard lk(mu_);
        closed_ = true;
    }
    void addIdleWriter(uint64_t writer, uint64_t incarnation)
    {
        std::lock_guard lk(mu_);
        writers_.push_back({writer, incarnation, "keeper-a"});
        view_.push_back({writer, incarnation, "keeper-a"});
    }

private:
    // One consultation: every Keeper answers from the bound the Tail named for it (or `from`), cut at its own limit.
    HotFetch snapshot(Hlc from, const player::TailStarts* starts) const
    {
        ++polls_;
        last_from_ = from;
        HotFetch f;
        f.route_epoch = kEpoch;
        f.writers = view_;
        f.closed = closed_;
        f.physical_policy = policy_;
        f.abandoned = abandoned_;
        std::vector<const KeeperFetch*> held{&a_, &b_};
        if(has_c_)
            held.push_back(&c_);
        for(const auto* source: held)
        {
            KeeperFetch k = *source;
            const auto& id = k.frontier.process_id;
            Hlc start = from;
            if(starts)
                if(auto it = starts->find(
                           player::SourceId{id, k.frontier.predecessor ? k.frontier.expected_epoch : Epoch{}});
                   it != starts->end())
                    start = std::max(from, it->second);
            source_start_[id] = start;
            if(k.frontier.predecessor && start >= k.frontier.own_cut)
                continue;
            if(auto custom = custom_.find(id); custom != custom_.end())
            {
                std::erase_if(k.events,
                              [&](const Event& e)
                              { return e.hlc < start || (k.frontier.predecessor && e.hlc >= k.frontier.own_cut); });
                std::stable_sort(k.events.begin(), k.events.end(), ReplayLess);
                if(custom->second && k.events.size() > custom->second)
                {
                    k.events.resize(custom->second);
                    k.frontier.truncated = true;
                }
            }
            if(tombstoned_)
            {
                k.events.clear();
                k.frontier.answered = false;
                k.frontier.status = absl::StatusCode::kFailedPrecondition;
            }
            f.keepers.push_back(std::move(k));
        }
        polled_.notify_all();
        return f;
    }

    bool policy_{};
    mutable std::mutex mu_;
    mutable std::condition_variable polled_;
    mutable unsigned polls_{};
    mutable Hlc last_from_;
    mutable std::map<std::string, Hlc> source_start_;
    std::map<std::string, size_t> custom_;
    std::vector<Range> abandoned_;
    bool has_c_{};
    KeeperFetch c_;
    KeeperFetch a_, b_;
    std::vector<WriterAssignment> writers_, view_;
    bool closed_{};
    bool tombstoned_{};
};

struct ArchiveWindow
{
    std::filesystem::path root =
            std::filesystem::temp_directory_path() / ("replay-contract-" + std::to_string(::getpid()));
    std::shared_ptr<std::promise<void>> release;
    ~ArchiveWindow()
    {
        if(release)
            release->set_value();
        std::filesystem::remove_all(root);
    }
};

std::unique_ptr<ReplayHarness> makeHarness()
{
    auto src = std::make_shared<FakeHotSource>();
    auto h = std::make_unique<ReplayHarness>();
    HotReplayOptions options;
    options.batch_size = 3;
    options.tail_poll = std::chrono::milliseconds(5);
    options.story_live = [src](StoryId story) { return src->live(story); };
    h->sut = std::make_unique<HotReplay>(src, options);
    h->physicalState = [src](bool p, int64_t f, uint64_t u) { src->physicalState(p, f, u); };
    h->setKeeperFrontiers = [src](std::vector<KeeperFrontier> f) { src->setKeeperFrontiers(f); };
    h->hideWriterFromAcquisitionView = [src] { src->hideWriter(); };
    h->setFrontiers = [src](std::vector<Frontier> f) { src->setWriterFrontiers(f); };
    h->failSource = [src] { src->failB(); };
    h->truncateSource = [src] { src->truncateA(); };
    h->finishTail = [src] { src->close(); };
    h->registerIdleWriter = [src](uint64_t w, uint64_t i) { src->addIdleWriter(w, i); };
    // Seeded events are DURABLE and the fake keeps them, so a crash restart is a reopen.
    h->crashRestart = [] {};
    h->tombstoneStory = [src] { src->tombstone(); };
    h->setKeeper = [src](const std::string& name, KeeperContents contents) { src->setKeeper(name, contents); };
    h->awaitPolls = [src](unsigned n) { src->awaitPolls(n); };
    h->lastPollStart = [src] { return src->lastPollStart(); };
    h->lastSourceStart = [src](const std::string& name) { return src->lastSourceStart(name); };
    h->abandonRange = [src](Hlc start, Hlc end) { src->abandon(start, end); };
    h->loseWindowBelowWatermark = [src, harness = h.get(), options]
    {
        auto window = std::make_shared<ArchiveWindow>();
        std::filesystem::remove_all(window->root);
        auto opened = FileTierStore::Open(window->root, "writer", {{1, {100, 0}}});
        ASSERT_TRUE(opened.ok()) << opened.status();
        auto writer = *std::move(opened);
        auto record = writer->publish({"lost", 1, {100, 0}, {200, 0}, {ev(2, 2, 120)}, false});
        ASSERT_TRUE(record.ok()) << record.status();
        std::filesystem::remove(window->root / record->file);
        writer.reset();
        opened = FileTierStore::Open(window->root, "writer", {{1, {100, 0}}});
        ASSERT_TRUE(opened.ok()) << opened.status();
        writer = *std::move(opened);
        auto watermark = writer->contiguousWatermark(1);
        ASSERT_TRUE(watermark.ok());
        EXPECT_EQ(*watermark, (Hlc{200, 0}));
        auto archive = FileTierStore::OpenReadOnly(window->root, std::chrono::hours(1));
        ASSERT_TRUE(archive.ok()) << archive.status();
        auto lost_options = options;
        lost_options.archive =
                std::shared_ptr<FileTierStore>(archive->release(), [window](FileTierStore* store) { delete store; });
        src->requireArchive();
        harness->sut = std::make_unique<HotReplay>(src, lost_options);
    };
    h->failArchiveFile = [src, harness = h.get(), options](ArchiveFault fault)
    {
        auto window = std::make_shared<ArchiveWindow>();
        std::filesystem::remove_all(window->root);
        auto opened = FileTierStore::Open(window->root, "writer", {{1, {100, 0}}});
        ASSERT_TRUE(opened.ok()) << opened.status();
        auto writer = *std::move(opened);
        auto e = ev(2, 2, 120);
        e.physical = {120, 0, ClockStatus::Synced};
        Chunk chunk{"fault", 1, {100, 0}, {200, 0}, {e}, false};
        chunk.physical_policy = true;
        auto record = writer->publish(chunk);
        ASSERT_TRUE(record.ok()) << record.status();
        TierChain chain;
        FileTierStore::Hooks hooks;
        if(fault == ArchiveFault::TierUnavailable || fault == ArchiveFault::Hang)
        {
            chain = {"test",
                     {{"local", "posix", window->root, 0, "local-uuid"},
                      {"slow", "posix", window->root / "slow", 1, "slow-uuid"}},
                     2,
                     std::chrono::milliseconds(20)};
            for(const auto& tier: chain.tiers)
            {
                std::filesystem::create_directories(tier.root);
                struct statfs info
                {
                };
                ASSERT_EQ(::statfs(tier.root.c_str(), &info), 0);
                std::ofstream(tier.root / ".chronolog-tier.json") << nlohmann::json{{"deployment_id", "test"},
                                                                                    {"name", tier.name},
                                                                                    {"rank", tier.rank},
                                                                                    {"kind", "posix"},
                                                                                    {"tier_uuid", tier.tier_uuid},
                                                                                    {"f_type", info.f_type}};
            }
            ASSERT_TRUE(writer->scrubOnce(0).ok());
            ASSERT_TRUE(writer->configureTiers("test", {chain.tiers[1]}).ok());
            ASSERT_TRUE(writer->probeTiers().ok());
            auto migrated = writer->migrateOnce("slow");
            ASSERT_TRUE(migrated.ok()) << migrated.status();
            ASSERT_EQ(*migrated, 1u);
            if(fault == ArchiveFault::Hang)
            {
                window->release = std::make_shared<std::promise<void>>();
                auto gate = window->release->get_future().share();
                hooks.tier_step = [gate](std::string_view)
                {
                    gate.wait();
                    return absl::UnavailableError("released hang");
                };
            }
        }
        FileTierStore::LoadFile load;
        std::chrono::milliseconds read_timeout = std::chrono::seconds(30);
        if(fault == ArchiveFault::LocalHang)
        {
            // The per-file load hook holds this file on `local`, where the Read's deadline is the archive read timeout,
            // 20 ms here like the slow tier's.
            window->release = std::make_shared<std::promise<void>>();
            load = [gate = window->release->get_future().share(),
                    held = std::filesystem::path(record->file).filename()](const std::filesystem::path& path)
            {
                if(path.filename() != held)
                    return LoadChunkFile(path);
                gate.wait();
                return absl::StatusOr<ChunkBytes>(absl::UnavailableError("released hang"));
            };
            read_timeout = std::chrono::milliseconds(20);
        }
        auto archive = FileTierStore::OpenReadOnly(window->root,
                                                   std::chrono::hours(1),
                                                   load,
                                                   0,
                                                   {},
                                                   read_timeout,
                                                   chain,
                                                   hooks);
        ASSERT_TRUE(archive.ok()) << archive.status();
        if(fault == ArchiveFault::TierUnavailable)
            std::filesystem::remove(window->root / "slow/.chronolog-tier.json");
        if(!chain.tiers.empty())
        {
            EXPECT_EQ((*archive)->probeTiers().ok(), fault != ArchiveFault::TierUnavailable);
        }
        if(fault == ArchiveFault::Missing)
            std::filesystem::remove(window->root / record->file);
        if(fault == ArchiveFault::Undecodable)
            std::ofstream(window->root / record->file, std::ios::trunc) << "not an archive";
        if(fault == ArchiveFault::ChecksumMismatch)
        {
            chunk.events.front().envelope.payload = "changed";
            ASSERT_TRUE(HDF5ChunkCodec().writeChunk(window->root / record->file, chunk).ok());
        }
        auto failed_options = options;
        failed_options.archive =
                std::shared_ptr<FileTierStore>(archive->release(), [window](FileTierStore* store) { delete store; });
        src->requireArchive();
        harness->sut = std::make_unique<HotReplay>(src, failed_options);
    };
    h->retireArchiveFile = [src, harness = h.get(), options](ArchiveRetirement retirement)
    {
        std::vector<Event> archived;
        [&]
        {
            auto window = std::make_shared<ArchiveWindow>();
            std::filesystem::remove_all(window->root);
            auto opened = FileTierStore::Open(window->root, "writer", {{1, {100, 0}}});
            ASSERT_TRUE(opened.ok()) << opened.status();
            std::shared_ptr<FileTierStore> writer = *std::move(opened);
            // Writer 8 has no Keeper, so its events reach a Read only from the archive.
            std::vector<std::string> files;
            for(const auto& [sequence, start, end]: {std::tuple{1, 100, 150}, std::tuple{2, 150, 200}})
            {
                auto e = ev(8, sequence, start + 10);
                e.physical = {start + 10, 0, ClockStatus::Synced};
                Chunk chunk{"retired-" + std::to_string(sequence), 1, {start, 0}, {end, 0}, {e}, false};
                chunk.physical_policy = true;
                auto record = writer->publish(chunk);
                ASSERT_TRUE(record.ok()) << record.status();
                files.push_back(record->file);
                archived.push_back(e);
            }
            auto retired = std::make_shared<std::once_flag>();
            auto load = [writer, root = window->root, files, retired, retirement](const std::filesystem::path& path)
            {
                const auto planned = [&](const std::string& file)
                { return std::filesystem::path(file).filename() == path.filename(); };
                if(std::any_of(files.begin(), files.end(), planned))
                    std::call_once(*retired,
                                   [&]
                                   {
                                       if(retirement == ArchiveRetirement::Compacted)
                                       {
                                           CompactionPolicy policy;
                                           policy.min_files = 2;
                                           policy.min_age = std::chrono::seconds(0);
                                           auto compacted = writer->compactOnce(policy);
                                           ASSERT_TRUE(compacted.ok()) << compacted.status();
                                           EXPECT_EQ(compacted->inputs, 2u);
                                       }
                                       else
                                           for(const auto& file: files) EXPECT_TRUE(writer->eraseFile(file).ok());
                                       for(const auto& file: files) EXPECT_FALSE(std::filesystem::exists(root / file));
                                   });
                return LoadChunkFile(path);
            };
            auto archive = FileTierStore::OpenReadOnly(window->root, std::chrono::hours(1), load);
            ASSERT_TRUE(archive.ok()) << archive.status();
            auto retired_options = options;
            retired_options.archive = std::shared_ptr<FileTierStore>(archive->release(),
                                                                     [window](FileTierStore* store) { delete store; });
            src->requireArchive();
            harness->sut = std::make_unique<HotReplay>(src, retired_options);
        }();
        return archived;
    };
    return h;
}

INSTANTIATE_TEST_SUITE_P(Hot,
                         ReplayContract,
                         ::testing::Values(ReplayFactory(makeHarness)),
                         [](const ::testing::TestParamInfo<ReplayFactory>&) { return std::string("Fake"); });

} // namespace
} // namespace chronolog::contract
