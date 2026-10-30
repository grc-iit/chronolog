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
class MetadataOperationState
{
public:
    enum class Phase
    {
        Pending,
        Unconfirmed
    };
    struct Ticket
    {
        std::pair<std::string, std::string> name;
        uint64_t generation;
    };
    bool unresolved() const noexcept { return !records_.empty(); }
    bool conflicts(std::string const& chronicle, std::string const& story) const
    {
        for(auto const& item: records_)
            if(chronicle.empty() || item.first.first.empty() ||
               (chronicle == item.first.first &&
                (story.empty() || item.first.second.empty() || story == item.first.second)))
                return true;
        return false;
    }
    std::optional<Ticket> begin(std::string const& chronicle, std::string const& story)
    {
        if(conflicts(chronicle, story))
            return std::nullopt;
        if(next_ == UINT64_MAX)
            return std::nullopt;
        Ticket ticket{{chronicle, story}, ++next_};
        records_.emplace(ticket.name, Record{ticket.generation, Phase::Pending});
        return ticket;
    }
    void finish(Ticket const& ticket, bool confirmed)
    {
        auto position = records_.find(ticket.name);
        if(confirmed && position != records_.end() && position->second.generation == ticket.generation)
            records_.erase(position);
        else if(!confirmed && position != records_.end() && position->second.generation == ticket.generation)
            position->second.phase = Phase::Unconfirmed;
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
    struct Record
    {
        uint64_t generation;
        Phase phase;
    };
    uint64_t next_ = 0;
    std::map<std::pair<std::string, std::string>, Record> records_;
};
} // namespace chronolog
#endif
