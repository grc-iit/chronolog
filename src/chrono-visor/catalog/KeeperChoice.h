#pragma once

#include <absl/status/statusor.h>
#include <chronolog/types.h>

#include <optional>
#include <string>
#include <vector>

namespace chronolog::visor
{

struct KeeperChoice
{
    KeeperRef keeper;
    std::optional<KeeperPreferenceResult> preference;
};

// The committed-Route Keeper for one (story, writer) acquisition (I7.5, I4.11). A prior row on the story pins the
// assignment: a surviving owner is retained with the Route's endpoint, a removed owner remaps by writer_id. Only a
// writer without a prior row can be placed by the preference hint, and the hint resolves to the Route's own endpoint.
// Deterministic over its inputs so Raft apply recomputes exactly the choice the proposal carried.
inline absl::StatusOr<KeeperChoice> chooseKeeper(const std::vector<KeeperRef>& keepers,
                                                 uint64_t writer_id,
                                                 const std::optional<std::string>& prior_owner,
                                                 const std::optional<std::string>& hint)
{
    if(keepers.empty())
        return absl::FailedPreconditionError("route has no keepers");
    auto listed = [&](const std::optional<std::string>& id) -> const KeeperRef*
    {
        if(id)
            for(const auto& k: keepers)
                if(k.process_id == *id)
                    return &k;
        return nullptr;
    };
    KeeperChoice choice{keepers[writer_id % keepers.size()], std::nullopt};
    const KeeperRef* preferred = listed(hint);
    if(prior_owner)
    {
        if(const KeeperRef* owner = listed(prior_owner))
            choice.keeper = *owner;
        if(hint)
            choice.preference = preferred ? KeeperPreferenceResult::Retained : KeeperPreferenceResult::NotInRoute;
        return choice;
    }
    if(preferred)
        return KeeperChoice{*preferred, KeeperPreferenceResult::Honored};
    if(hint)
        choice.preference = KeeperPreferenceResult::NotInRoute;
    return choice;
}

} // namespace chronolog::visor
