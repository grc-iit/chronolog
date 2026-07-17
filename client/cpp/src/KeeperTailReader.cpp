#include <cstddef>
#include <map>
#include <vector>

#include <chrono_monitor.h>
#include <chronolog_types.h>
#include <client_errcode.h>

#include "KeeperRecordingClient.h"
#include "KeeperTailReader.h"

// Tail read: two-phase scatter/gather across the story's assigned keepers.
int chronolog::gather_story_tail(std::vector<KeeperRecordingClient*> const& keepers,
                                 StoryId const& story_id,
                                 std::size_t n,
                                 std::vector<Event>& events)
{
    events.clear();
    if(keepers.empty())
    {
        LOG_WARNING("[KeeperTailReader] gather_story_tail: no keepers assigned to story {}", story_id);
        return CL_ERR_NO_KEEPERS;
    }
    if(n == 0)
    {
        return CL_SUCCESS;
    }

    // Phase 1: collect each keeper's last-N event sequences, remembering which
    // keeper reported each sequence (so phase 2 asks the right keeper for it).
    std::map<EventSequence, KeeperRecordingClient*> seqToKeeper;
    for(auto* keeper: keepers)
    {
        if(keeper == nullptr)
        {
            continue;
        }
        std::vector<EventSequence> seqs = keeper->getTailSequences(story_id, n);
        for(auto const& seq: seqs) { seqToKeeper[seq] = keeper; }
    }
    if(seqToKeeper.empty())
    {
        LOG_DEBUG("[KeeperTailReader] gather_story_tail: story {} has no tail events yet", story_id);
        return CL_SUCCESS;
    }

    // Select the global last-N sequences (the largest N keys), grouped per keeper.
    std::map<KeeperRecordingClient*, std::vector<EventSequence>> perKeeper;
    std::size_t take = (n < seqToKeeper.size()) ? n : seqToKeeper.size();
    auto seq_iter = seqToKeeper.end();
    for(std::size_t i = 0; i < take; ++i)
    {
        --seq_iter;
        perKeeper[seq_iter->second].push_back(seq_iter->first);
    }

    // Phase 2: fetch payloads for the selected sequences from each keeper and
    // assemble Events keyed by sequence so the result is sorted ascending.
    std::map<EventSequence, Event> assembled;
    for(auto& keeper_entry: perKeeper)
    {
        std::vector<LogEvent> logEvents = keeper_entry.first->getTailEvents(story_id, keeper_entry.second);
        for(auto const& le: logEvents)
        {
            EventSequence seq{le.time(), le.getClientId(), le.index()};
            assembled[seq] = Event(le.time(), le.getClientId(), le.index(), le.getRecord());
        }
    }

    events.reserve(assembled.size());
    for(auto const& entry: assembled) { events.push_back(entry.second); }

    LOG_DEBUG("[KeeperTailReader] gather_story_tail(n={}) for story {} returned {} events", n, story_id, events.size());
    return CL_SUCCESS;
}
