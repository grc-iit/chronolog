#pragma once

#include <cstddef>
#include <string>
#include <vector>
#include <absl/status/status.h>
#include "chronolog/types.h"

namespace chronolog
{

// I6.17: a conjunction of equality terms over the indexed fields of I3.9. A set term is satisfied by any member, and a
// predicate with no term matches every event.
struct EventPredicate
{
    struct Attribute
    {
        std::string key;
        std::string value;
    };
    struct LinkTerm
    {
        // Empty matches any link type.
        std::string type;
        EventId target;
    };

    // Set members and terms, counted together.
    static constexpr size_t kMaxValues = 256;

    std::vector<std::string> kinds;
    std::vector<std::string> actors;
    std::vector<Attribute> attributes;
    std::vector<LinkTerm> links;
    std::vector<EventId> event_ids;

    bool empty() const;
    // INVALID_ARGUMENT for an attribute term without a key, a link or event id term whose EventId is incomplete (a field
    // is zero), or more than kMaxValues values.
    absl::Status validate() const;
    // A link term matches on the target EventId and optionally the type, never on the hlc a link carries.
    bool matches(const Event& event) const;
};

} // namespace chronolog
