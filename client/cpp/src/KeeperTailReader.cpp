#include <cstddef>
#include <map>
#include <utility>
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

    // Phase 1 (scatter): fan out tail_get_sequences to every keeper concurrently,
    // then collect. Issuing all RPCs before waiting on any means total phase-1
    // latency is ~the slowest keeper (bounded by the per-RPC timeout) instead of
    // the sum, and one slow or hung keeper can neither serialize nor block the
    // others. Each response is bounded by kTailReadRpcTimeoutMs; a timeout or RPC
    // error simply drops that keeper's contribution (empty tail).
    std::vector<std::pair<KeeperRecordingClient*, tl::async_response>> inflight;
    inflight.reserve(keepers.size());
    for(auto* keeper: keepers)
    {
        if(keeper == nullptr)
        {
            continue;
        }
        try
        {
            inflight.emplace_back(keeper, keeper->getTailSequencesAsync(story_id, n));
        }
        catch(thallium::exception const& ex)
        {
            LOG_ERROR("[KeeperTailReader] tail_get_sequences issue to {} failed: {}",
                      to_string(keeper->getRecordingServiceId()),
                      ex.what());
        }
    }

    std::map<EventSequence, KeeperRecordingClient*> seqToKeeper;
    for(auto& entry: inflight)
    {
        KeeperRecordingClient* keeper = entry.first;
        try
        {
            std::vector<EventSequence> seqs = entry.second.wait();
            for(auto const& seq: seqs) { seqToKeeper[seq] = keeper; }
        }
        catch(thallium::timeout const&)
        {
            LOG_WARNING("[KeeperTailReader] tail_get_sequences to {} timed out; skipping this keeper",
                        to_string(keeper->getRecordingServiceId()));
        }
        catch(thallium::exception const& ex)
        {
            LOG_ERROR("[KeeperTailReader] tail_get_sequences to {} failed: {}",
                      to_string(keeper->getRecordingServiceId()),
                      ex.what());
        }
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

    // Phase 2 (gather): fetch payloads for the selected sequences from each keeper
    // concurrently — same rationale as phase 1. Issue all tail_get_events first,
    // then wait on each; per-keeper timeout/error drops that keeper's payloads.
    // perKeeper stays in scope through the waits so the seqs each request
    // references remain valid.
    std::vector<std::pair<KeeperRecordingClient*, tl::async_response>> inflight_events;
    inflight_events.reserve(perKeeper.size());
    for(auto& keeper_entry: perKeeper)
    {
        try
        {
            inflight_events.emplace_back(keeper_entry.first,
                                         keeper_entry.first->getTailEventsAsync(story_id, keeper_entry.second));
        }
        catch(thallium::exception const& ex)
        {
            LOG_ERROR("[KeeperTailReader] tail_get_events issue to {} failed: {}",
                      to_string(keeper_entry.first->getRecordingServiceId()),
                      ex.what());
        }
    }

    // Assemble Events keyed by sequence so the result comes out sorted ascending.
    std::map<EventSequence, Event> assembled;
    for(auto& entry: inflight_events)
    {
        KeeperRecordingClient* keeper = entry.first;
        try
        {
            std::vector<LogEvent> logEvents = entry.second.wait();
            for(auto const& le: logEvents)
            {
                EventSequence seq{le.time(), le.getClientId(), le.index()};
                assembled[seq] = Event(le.time(), le.getClientId(), le.index(), le.getRecord());
            }
        }
        catch(thallium::timeout const&)
        {
            LOG_WARNING("[KeeperTailReader] tail_get_events to {} timed out; skipping this keeper",
                        to_string(keeper->getRecordingServiceId()));
        }
        catch(thallium::exception const& ex)
        {
            LOG_ERROR("[KeeperTailReader] tail_get_events to {} failed: {}",
                      to_string(keeper->getRecordingServiceId()),
                      ex.what());
        }
    }

    events.reserve(assembled.size());
    for(auto const& entry: assembled) { events.push_back(entry.second); }

    LOG_DEBUG("[KeeperTailReader] gather_story_tail(n={}) for story {} returned {} events", n, story_id, events.size());
    return CL_SUCCESS;
}
