#pragma once

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <unistd.h>

#include "ram_harness.h"
#include "wal/Record.h"
#include "wal/WalJournal.h"

namespace chronolog::test
{

struct WalControl
{
    WalControl()
    {
        std::string pattern = (std::filesystem::temp_directory_path() / "chronolog-wal-XXXXXX").string();
        if(!::mkdtemp(pattern.data()))
            throw std::runtime_error("mkdtemp failed");
        directory = pattern;
    }
    ~WalControl() { std::filesystem::remove_all(directory); }
    void block()
    {
        std::lock_guard lock(mu);
        blocked = true;
        pending.reset();
    }
    void release()
    {
        {
            std::lock_guard lock(mu);
            blocked = false;
        }
        cv.notify_all();
    }
    Hlc waitPending()
    {
        std::unique_lock lock(mu);
        if(!cv.wait_for(lock, std::chrono::seconds(5), [this] { return pending.has_value(); }))
            throw std::runtime_error("WAL did not reach fsync");
        return *pending;
    }
    std::string directory;
    std::mutex mu;
    std::condition_variable cv;
    bool blocked{};
    bool fail{};
    std::optional<Hlc> pending;
};

class ControlledSink final: public FileSink
{
public:
    ControlledSink(std::shared_ptr<WalControl> control, const std::string& path)
        : control_(std::move(control))
        , sink_(openFileSink(path))
    {}
    absl::Status write(std::string_view bytes) override
    {
        if(bytes.size() > 8 && bytes[8] == 'E')
        {
            auto event = wal::decode(bytes.substr(9));
            {
                std::lock_guard lock(control_->mu);
                control_->pending = event.hlc;
            }
            control_->cv.notify_all();
        }
        return sink_->write(bytes);
    }
    absl::Status sync() override
    {
        std::unique_lock lock(control_->mu);
        if(!control_->cv.wait_for(lock, std::chrono::seconds(10), [this] { return !control_->blocked; }))
            return absl::UnavailableError("test fsync block timed out");
        if(control_->fail)
            return absl::UnavailableError("injected fsync failure");
        lock.unlock();
        return sink_->sync();
    }

private:
    std::shared_ptr<WalControl> control_;
    std::unique_ptr<FileSink> sink_;
};

struct WalRig
{
    std::shared_ptr<WalControl> control = std::make_shared<WalControl>();
    std::shared_ptr<FakeClock> clock;
    std::shared_ptr<FakeMembership> membership = std::make_shared<FakeMembership>();
    RamJournalConfig ram_config;
    WalJournalConfig config;
    WalJournal* current{};
    std::unique_ptr<WalJournal> journal;

    WalRig()
    {
        ram_config.causal_floor_skew_limit_ns = 1000;
        config.wal_dir = control->directory;
        reopen();
    }
    WalJournal::SinkFactory factory() const
    {
        return [control = control](const std::string& path) { return std::make_unique<ControlledSink>(control, path); };
    }
    void reopen()
    {
        journal.reset();
        clock = std::make_shared<FakeClock>(100);
        clock->setStatus(ClockStatus::Synced);
        journal = std::make_unique<WalJournal>(clock, membership, ram_config, config, factory());
        current = journal.get();
        (void)current->registerWriter(1, 2, 3);
    }
};

} // namespace chronolog::test
