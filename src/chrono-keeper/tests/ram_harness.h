#pragma once

#include <memory>

#include "clock/FakeClock.h"
#include "journal/RamJournal.h"

namespace chronolog::test
{

// Story 1 at epoch 7, served by the single Keeper "self". Every other story is unknown.
class FakeMembership final: public Membership
{
public:
    absl::StatusOr<Route> route(StoryId id) const override
    {
        if(id != 1)
            return absl::NotFoundError("unknown story");
        return Route{7, {KeeperRef{"self", "self:1"}}, "grapher:1", "player:1"};
    }

    absl::Status validateEpoch(StoryId id, Epoch epoch) const override
    {
        if(id != 1)
            return absl::NotFoundError("unknown story");
        if(epoch != 7)
            return absl::FailedPreconditionError("stale epoch");
        return absl::OkStatus();
    }

    absl::Status registerProcess(Process) override { return absl::OkStatus(); }
    absl::Status heartbeat(std::string, std::string, uint64_t) override { return absl::OkStatus(); }
};

// Fresh RamJournal on a FakeClock at physical 100 with writer 2 incarnation 3 registered on story 1.
struct RamRig
{
    std::shared_ptr<FakeClock> clock;
    std::shared_ptr<FakeMembership> membership;
    RamJournal* journal{};

    explicit RamRig(RamJournalConfig config = {})
    {
        clock = std::make_shared<FakeClock>(100);
        clock->setStatus(ClockStatus::Synced);
        membership = std::make_shared<FakeMembership>();
        owned_ = std::make_unique<RamJournal>(clock, membership, config);
        journal = owned_.get();
        (void)journal->registerWriter(1, 2, 3);
    }

    std::unique_ptr<RamJournal> release() { return std::move(owned_); }

private:
    std::unique_ptr<RamJournal> owned_;
};

} // namespace chronolog::test
