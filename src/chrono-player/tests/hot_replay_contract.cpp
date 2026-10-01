// Instantiates ReplayContract against HotReplay over an in-process fake HotSource.
#include <algorithm>
#include <mutex>
#include <filesystem>
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

    absl::StatusOr<HotFetch> fetch(StoryId story, const Range&) const override
    {
        std::lock_guard lk(mu_);
        if(tombstoned_)
            return absl::FailedPreconditionError("story is tombstoned");
        if(story != 1)
            return absl::NotFoundError("unknown story");
        HotFetch f;
        f.route_epoch = kEpoch;
        f.keepers = {a_, b_};
        f.writers = view_;
        f.closed = closed_;
        return f;
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
    mutable std::mutex mu_;
    KeeperFetch a_, b_;
    std::vector<WriterAssignment> writers_, view_;
    bool closed_{};
    bool tombstoned_{};
};

struct ArchiveWindow
{
    std::filesystem::path root =
            std::filesystem::temp_directory_path() / ("replay-contract-" + std::to_string(::getpid()));
    ~ArchiveWindow() { std::filesystem::remove_all(root); }
};

std::unique_ptr<ReplayHarness> makeHarness()
{
    auto src = std::make_shared<FakeHotSource>();
    auto h = std::make_unique<ReplayHarness>();
    HotReplayOptions options;
    options.batch_size = 3;
    options.tail_poll = std::chrono::milliseconds(5);
    h->sut = std::make_unique<HotReplay>(src, options);
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
    return h;
}

INSTANTIATE_TEST_SUITE_P(Hot,
                         ReplayContract,
                         ::testing::Values(ReplayFactory(makeHarness)),
                         [](const ::testing::TestParamInfo<ReplayFactory>&) { return std::string("Fake"); });

} // namespace
} // namespace chronolog::contract
