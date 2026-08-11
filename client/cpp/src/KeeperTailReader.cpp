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
    // Per-phase outcome tallies. A tail read is best-effort across the group, so ONE
    // keeper failing degrades the result but is still a successful read; every keeper
    // failing is not a read at all, and must not be reported as an empty tail.
    std::size_t timeouts = 0;
    std::size_t errors = 0;
    std::size_t responded = 0;

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
            ++errors;
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
            ++responded;
            for(auto const& seq: seqs) { seqToKeeper[seq] = keeper; }
        }
        catch(thallium::timeout const&)
        {
            ++timeouts;
            LOG_WARNING("[KeeperTailReader] tail_get_sequences to {} timed out; skipping this keeper",
                        to_string(keeper->getRecordingServiceId()));
        }
        catch(thallium::exception const& ex)
        {
            ++errors;
            LOG_ERROR("[KeeperTailReader] tail_get_sequences to {} failed: {}",
                      to_string(keeper->getRecordingServiceId()),
                      ex.what());
        }
    }

    // No keeper answered at all: the tail is unknown, not empty. Returning CL_SUCCESS
    // here would be indistinguishable from "this story has no events yet", and callers
    // do read it that way -- the e2e test and perf_bench both treat an empty result as
    // "not visible yet" and keep polling.
    if(responded == 0 && (timeouts + errors) > 0)
    {
        LOG_ERROR("[KeeperTailReader] gather_story_tail: story {} - no keeper answered phase 1 ({} timed out, {} "
                  "errored); the tail is unknown",
                  story_id,
                  timeouts,
                  errors);
        return (timeouts > 0) ? CL_ERR_QUERY_TIMED_OUT : CL_ERR_UNKNOWN;
    }
    if(timeouts + errors > 0)
    {
        LOG_WARNING("[KeeperTailReader] gather_story_tail: story {} - {} of {} keeper(s) did not answer phase 1; the "
                    "tail may be missing their most recent events",
                    story_id,
                    timeouts + errors,
                    responded + timeouts + errors);
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
    timeouts = 0;
    errors = 0;
    responded = 0;

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
            ++errors;
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
            ++responded;
            for(auto const& le: logEvents)
            {
                Event event(le.time(), le.getClientId(), le.index(), le.getRecord());
                assembled[event.sequence()] = event;
            }
        }
        catch(thallium::timeout const&)
        {
            ++timeouts;
            LOG_WARNING("[KeeperTailReader] tail_get_events to {} timed out; skipping this keeper",
                        to_string(keeper->getRecordingServiceId()));
        }
        catch(thallium::exception const& ex)
        {
            ++errors;
            LOG_ERROR("[KeeperTailReader] tail_get_events to {} failed: {}",
                      to_string(keeper->getRecordingServiceId()),
                      ex.what());
        }
    }

    // Phase 1 found sequences, so there ARE events to fetch. If no keeper delivered
    // any payload, the read failed -- do not report that as an empty tail.
    if(responded == 0 && (timeouts + errors) > 0)
    {
        LOG_ERROR("[KeeperTailReader] gather_story_tail: story {} - no keeper answered phase 2 ({} timed out, {} "
                  "errored); {} selected event(s) could not be fetched",
                  story_id,
                  timeouts,
                  errors,
                  take);
        return (timeouts > 0) ? CL_ERR_QUERY_TIMED_OUT : CL_ERR_UNKNOWN;
    }
    if(timeouts + errors > 0)
    {
        LOG_WARNING("[KeeperTailReader] gather_story_tail: story {} - {} of {} keeper(s) did not answer phase 2; "
                    "returning {} of {} selected event(s)",
                    story_id,
                    timeouts + errors,
                    responded + timeouts + errors,
                    assembled.size(),
                    take);
    }

    events.reserve(assembled.size());
    for(auto const& entry: assembled) { events.push_back(entry.second); }

    LOG_DEBUG("[KeeperTailReader] gather_story_tail(n={}) for story {} returned {} events", n, story_id, events.size());
    return CL_SUCCESS;
}
