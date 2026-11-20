#ifndef CHRONOLOG_METADATA_OPERATION_STATE_H
#define CHRONOLOG_METADATA_OPERATION_STATE_H
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
namespace chronolog
{
// Caller holds the owning context's metadata mutex. Records survive caller
// expiry; only the matching confirmed generation may clear an unknown mutation.
//
// A mutation whose outcome is unknown blocks every later mutation of the same
// name, with two exceptions where the Visor's answer settles it safely:
// - After an unconfirmed AcquireStory, the next AcquireStory or ReleaseStory of
//   that story is sent (a retry). The Visor answers a repeated acquire by the
//   client that holds the story with its current acquisition, and a release
//   with whether this client held it, so either answer is a confirmed outcome.
//   No handle exists for the story, so nothing local answers for the Visor.
// - After an unconfirmed ReleaseStory, the next ReleaseStory is sent. An
//   AcquireStory stays refused: the handle from before the release still exists
//   and may name keepers the story no longer uses.
// Disconnect is sent while the only unknown outcomes are those of AcquireStory
// and ReleaseStory: the Visor releases every story this client holds, so a
// confirmed Disconnect settles all of them (settleAcquisitions).
class MetadataOperationState
{
public:
    enum class Phase
    {
        Pending,
        Unconfirmed
    };
    enum class Kind
    {
        Acquire,
        Release,
        Disconnect,
        Other
    };
    struct Ticket
    {
        std::pair<std::string, std::string> name;
        uint64_t generation;
        // Sent to settle an earlier unconfirmed acquire or release of this
        // name. Only a Visor reply may clear it; a local answer, or a failure
        // before it is sent, restores the earlier record (keepEarlier).
        bool retry = false;
        Kind earlier = Kind::Other;
    };
    bool unresolved() const noexcept { return !records_.empty(); }
    // True when a record other than an unconfirmed acquire or release is left;
    // a confirmed Disconnect settles those (see the class comment).
    bool unresolvedBeyondAcquisitions() const noexcept
    {
        for(auto const& item: records_)
            if(!settledByDisconnect(item.second))
                return true;
        return false;
    }
    bool conflicts(std::string const& chronicle, std::string const& story, Kind kind = Kind::Other) const
    {
        return admission(chronicle, story, kind) == Admission::Refused;
    }
    std::optional<Ticket> begin(std::string const& chronicle, std::string const& story, Kind kind = Kind::Other)
    {
        auto admitted = admission(chronicle, story, kind);
        if(admitted == Admission::Refused)
            return std::nullopt;
        if(next_ == UINT64_MAX)
            return std::nullopt;
        Ticket ticket{{chronicle, story}, ++next_, admitted == Admission::Retry};
        if(auto position = records_.find(ticket.name); ticket.retry && position != records_.end())
            ticket.earlier = position->second.kind;
        records_.insert_or_assign(ticket.name, Record{ticket.generation, Phase::Pending, kind});
        return ticket;
    }
    // A retry that was answered locally or never sent changed nothing: the
    // earlier unconfirmed outcome stands as it was.
    void keepEarlier(Ticket const& ticket)
    {
        auto position = records_.find(ticket.name);
        if(ticket.retry && position != records_.end() && position->second.generation == ticket.generation)
            position->second = Record{ticket.generation, Phase::Unconfirmed, ticket.earlier};
    }
    void finish(Ticket const& ticket, bool confirmed)
    {
        auto position = records_.find(ticket.name);
        if(confirmed && position != records_.end() && position->second.generation == ticket.generation)
            records_.erase(position);
        else if(!confirmed && position != records_.end() && position->second.generation == ticket.generation)
            position->second.phase = Phase::Unconfirmed;
    }
    // A confirmed Disconnect released every story this client held at the
    // Visor: forget the unknown outcomes of its acquires and releases.
    void settleAcquisitions()
    {
        for(auto position = records_.begin(); position != records_.end();)
            position = settledByDisconnect(position->second) ? records_.erase(position) : std::next(position);
    }

    bool matches(Ticket const& ticket) const
    {
        auto position = records_.find(ticket.name);
        return position != records_.end() && position->second.generation == ticket.generation;
    }
    std::optional<Phase> phase(Ticket const& ticket) const
    {
        auto position = records_.find(ticket.name);
        if(position == records_.end() || position->second.generation != ticket.generation)
            return std::nullopt;
        return position->second.phase;
    }

private:
    enum class Admission
    {
        Fresh,
        Retry,
        Refused
    };
    struct Record
    {
        uint64_t generation;
        Phase phase;
        Kind kind;
    };
    static bool settledByDisconnect(Record const& record)
    {
        return record.phase == Phase::Unconfirmed && (record.kind == Kind::Acquire || record.kind == Kind::Release);
    }
    Admission admission(std::string const& chronicle, std::string const& story, Kind kind) const
    {
        bool retry = false;
        for(auto const& item: records_)
        {
            bool overlaps = chronicle.empty() || item.first.first.empty() ||
                            (chronicle == item.first.first &&
                             (story.empty() || item.first.second.empty() || story == item.first.second));
            if(!overlaps)
                continue;
            auto const& record = item.second;
            bool sameName = item.first.first == chronicle && item.first.second == story;
            if(kind == Kind::Disconnect && settledByDisconnect(record))
                continue;
            if(sameName && record.phase == Phase::Unconfirmed &&
               ((record.kind == Kind::Acquire && (kind == Kind::Acquire || kind == Kind::Release)) ||
                (record.kind == Kind::Release && kind == Kind::Release)))
            {
                retry = true;
                continue;
            }
            return Admission::Refused;
        }
        return retry ? Admission::Retry : Admission::Fresh;
    }
    uint64_t next_ = 0;
    std::map<std::pair<std::string, std::string>, Record> records_;
};
} // namespace chronolog
#endif
