#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "chronolog/types.h"

namespace chronolog::visor
{

enum class AcquisitionState
{
    Acquired,
    Released
};

// One committed change to the set of acquisitions. Revisions are persisted, so
// they keep increasing across Visor restarts and a Keeper's applied revision is
// never ahead of a restarted Visor.
struct AcquisitionChange
{
    uint64_t revision{};
    StoryId story_id{};
    uint64_t writer_id{};
    uint64_t incarnation{};
    std::string assigned_keeper;
    AcquisitionState state{AcquisitionState::Acquired};
};

struct AcquisitionSnapshot
{
    // Every change with revision <= this value is reflected in `active`.
    uint64_t revision{};
    std::vector<AcquisitionChange> active;
};

class AcquisitionObserver
{
public:
    virtual ~AcquisitionObserver() = default;
    // Called after the change is committed, while the store still serializes
    // mutations, so observers see changes in revision order. Must not call back
    // into the store and must not block.
    virtual void onAcquisitionChange(const AcquisitionChange& change) = 0;
};

// Blocks until the Keeper serving `keeper_endpoint` has applied `revision` or the
// configured fence timeout passes. Returns true only on confirmation. A store built
// without one reports fenced=false from every release.
using FenceWaiter = std::function<bool(const std::string& keeper_endpoint, uint64_t revision)>;

// Visor-local extension of MetadataStore. It exposes what the Cluster service needs
// to tell Keepers about registered writers and to fence a released incarnation.
class AcquisitionLedger
{
public:
    virtual ~AcquisitionLedger() = default;
    virtual absl::StatusOr<AcquisitionSnapshot> snapshotAcquisitions() const = 0;
    // The observer must outlive the ledger or be reset to nullptr first.
    virtual void setObserver(AcquisitionObserver* observer) = 0;
};

// Name validation shared by every MetadataStore implementation.
inline bool validName(const std::string& name)
{
    if(name.empty() || name.size() > 255)
        return false;
    for(unsigned char c: name)
        if(c < 0x20 || c == 0x7f)
            return false;
    return true;
}

} // namespace chronolog::visor
