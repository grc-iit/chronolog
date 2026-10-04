#include "common/predicate/Predicate.h"

#include <algorithm>

namespace chronolog
{
namespace
{

bool complete(const EventId& id) { return id.story_id && id.writer_id && id.incarnation && id.sequence; }

} // namespace

bool EventPredicate::empty() const
{
    return kinds.empty() && actors.empty() && attributes.empty() && links.empty() && event_ids.empty();
}

absl::Status EventPredicate::validate() const
{
    if(kinds.size() + actors.size() + attributes.size() + links.size() + event_ids.size() > kMaxValues)
        return absl::InvalidArgumentError("predicate holds more than 256 values");
    for(const auto& term: attributes)
        if(term.key.empty())
            return absl::InvalidArgumentError("predicate attribute term needs a key");
    for(const auto& term: links)
        if(!complete(term.target))
            return absl::InvalidArgumentError("predicate link term needs a complete target EventId");
    for(const auto& id: event_ids)
        if(!complete(id))
            return absl::InvalidArgumentError("predicate event id term needs a complete EventId");
    return absl::OkStatus();
}

bool EventPredicate::matches(const Event& event) const
{
    const auto& envelope = event.envelope;
    if(!kinds.empty() && std::find(kinds.begin(), kinds.end(), envelope.kind) == kinds.end())
        return false;
    if(!actors.empty() && std::find(actors.begin(), actors.end(), envelope.actor) == actors.end())
        return false;
    for(const auto& term: attributes)
    {
        auto it = envelope.attributes.find(term.key);
        if(it == envelope.attributes.end() || it->second != term.value)
            return false;
    }
    for(const auto& term: links)
        if(std::none_of(envelope.links.begin(),
                        envelope.links.end(),
                        [&](const Link& link)
                        { return link.target == term.target && (term.type.empty() || link.type == term.type); }))
            return false;
    return event_ids.empty() || std::find(event_ids.begin(), event_ids.end(), event.id) != event_ids.end();
}

} // namespace chronolog
