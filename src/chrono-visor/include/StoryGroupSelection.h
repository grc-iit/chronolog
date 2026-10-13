#ifndef CHRONOLOG_STORY_GROUP_SELECTION_H
#define CHRONOLOG_STORY_GROUP_SELECTION_H

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

#include <ServiceId.h>

namespace chronolog
{

// SplitMix64's finalizer: spreads every input bit over the whole result.
inline uint64_t mix64(uint64_t z)
{
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

/**
 * The recording group that records story_id, out of group_ids (the active
 * groups, in any order), by rendezvous hashing: each group gets a weight from
 * the story id and the group id, and the heaviest group wins (the lower id on a
 * tie). The result depends only on the story id and the set of groups:
 * - a story released and acquired again goes back to the same group, whose
 *   keepers still hold its recent events, and so does it after a visor restart;
 * - when a group leaves, only its stories move; when one joins, stories move
 *   only to it, about 1/N of them;
 * - stories spread evenly over the groups.
 * Returns nothing when group_ids is empty.
 */
inline std::optional<RecordingGroupId> select_recording_group(uint64_t story_id,
                                                              std::vector<RecordingGroupId> const& group_ids)
{
    std::optional<RecordingGroupId> selected;
    uint64_t selected_weight = 0;
    for(RecordingGroupId group_id: group_ids)
    {
        uint64_t weight = mix64(story_id ^ mix64(0x9e3779b97f4a7c15ULL + group_id));
        if(!selected || weight > selected_weight || (weight == selected_weight && group_id < *selected))
        {
            selected = group_id;
            selected_weight = weight;
        }
    }
    return selected;
}

/**
 * Add group to the visor's list of active groups, unless it is listed already.
 * The selection above draws from that list, so a group must be in it at most
 * once: a copy left behind after the group is removed keeps receiving the
 * stories that hash to it, with no grapher or no keepers to record them.
 */
template <typename Group>
void add_active_group(std::vector<Group*>& active_groups, Group* group)
{
    if(std::find(active_groups.begin(), active_groups.end(), group) == active_groups.end())
    {
        active_groups.push_back(group);
    }
}

/**
 * Remove every copy of group from the list of active groups; a group that is
 * not listed leaves the list unchanged.
 */
template <typename Group>
void remove_active_group(std::vector<Group*>& active_groups, Group* group)
{
    active_groups.erase(std::remove(active_groups.begin(), active_groups.end(), group), active_groups.end());
}

} // namespace chronolog

#endif // CHRONOLOG_STORY_GROUP_SELECTION_H
