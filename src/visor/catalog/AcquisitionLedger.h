#pragma once

#include <cstdint>
#include <functional>
#include <map>
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
    KeeperRef assigned_keeper;
    AcquisitionState state{AcquisitionState::Acquired};
    int64_t duration_ns{};
    AcquisitionTerminationCause termination_cause{AcquisitionTerminationCause::Unspecified};
    uint64_t applied_index{};
};

struct AcquisitionSnapshot
{
    // Every change with revision <= this value is reflected in `active`.
    uint64_t revision{};
    uint64_t applied_index{};
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

// Blocks until the Keeper `keeper` has applied `revision` or the
// configured fence timeout passes. Returns true only on confirmation. A store built
// without one reports fenced=false from every release.
using FenceWaiter = std::function<bool(const KeeperRef& keeper, uint64_t revision)>;

// Visor-local extension of MetadataStore. It exposes what the Cluster service needs
// to tell Keepers about registered writers and to fence a released incarnation.
class AcquisitionLedger
{
public:
    virtual ~AcquisitionLedger() = default;
    virtual absl::StatusOr<AcquisitionSnapshot> snapshotAcquisitions() const = 0;
    // The observer must outlive the ledger or be reset to nullptr first.
    virtual void setObserver(AcquisitionObserver* observer) = 0;
    virtual absl::StatusOr<Acquisition> requestGrant(const std::string& request_id) const = 0;
};

// Observational per-Keeper count of the committed active assigned rows (RFC-G section 9). It makes no admission,
// assignment or fence decision.
inline std::map<std::string, uint64_t> activeAcquisitionsPerKeeper(const AcquisitionSnapshot& snapshot)
{
    std::map<std::string, uint64_t> counts;
    for(const auto& row: snapshot.active)
        if(row.state == AcquisitionState::Acquired)
            ++counts[row.assigned_keeper.process_id];
    return counts;
}

// Name validation shared by every MetadataStore implementation. Lookups use this alone, so an object created before
// the I3.11 rules keeps its name and stays readable.
inline bool validName(const std::string& name)
{
    if(name.empty() || name.size() > 255)
        return false;
    for(unsigned char c: name)
        if(c < 0x20 || c == 0x7f)
            return false;
    return true;
}

// I3.11: nonempty segments separated by single `/`, none beginning with `@` unless reserved is allowed.
inline bool validSegments(const std::string& path, bool allow_reserved)
{
    size_t start = 0;
    while(true)
    {
        const size_t end = path.find('/', start);
        const size_t length = (end == std::string::npos ? path.size() : end) - start;
        if(length == 0 || (!allow_reserved && path[start] == '@'))
            return false;
        if(end == std::string::npos)
            return true;
        start = end + 1;
    }
}

// I3.11, checked at createChronicle: one segment, not reserved.
inline bool validNewChronicleName(const std::string& name)
{
    return validName(name) && name.find('/') == std::string::npos && validSegments(name, false);
}

// I3.11, checked at createStory.
inline bool validNewStoryName(const std::string& name) { return validName(name) && validSegments(name, false); }

// I3.11, I9.3: whole segments, reserved ones allowed because a prefix may name them. A story path is at most
// 255 + 1 + 255 bytes.
inline bool validPrefix(const std::string& prefix)
{
    if(prefix.size() > 511)
        return false;
    for(unsigned char c: prefix)
        if(c < 0x20 || c == 0x7f)
            return false;
    return validSegments(prefix, true);
}

// I9.3: the path equals the prefix or lies below it by whole segments, and any segment beginning with `@` is one
// the prefix names.
inline bool underPrefix(const std::string& chronicle, const std::string& story, const std::string& prefix)
{
    const std::string path = chronicle + '/' + story;
    if(path.size() < prefix.size() || path.compare(0, prefix.size(), prefix) != 0)
        return false;
    if(path.size() > prefix.size() && path[prefix.size()] != '/')
        return false;
    for(size_t i = prefix.size(); i < path.size(); ++i)
        if(path[i] == '@' && path[i - 1] == '/')
            return false;
    return true;
}

// I9.2: negative retention and a granularity outside the enum are INVALID_ARGUMENT. Zero retention means none and is
// stored unset.
inline absl::Status checkProperties(Properties& properties)
{
    if(properties.retention_ns && *properties.retention_ns < 0)
        return absl::InvalidArgumentError("retention_ns must not be negative");
    if(properties.retention_ns == 0)
        properties.retention_ns.reset();
    if(static_cast<uint32_t>(properties.granularity) > static_cast<uint32_t>(Granularity::S))
        return absl::InvalidArgumentError("unknown granularity");
    return absl::OkStatus();
}

} // namespace chronolog::visor
