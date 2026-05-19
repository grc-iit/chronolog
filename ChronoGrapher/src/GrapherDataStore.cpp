#include <unistd.h>
#include <map>
#include <mutex>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>
#include <thallium.hpp>

#include <chronolog_errcode.h>
#include <GrapherDataStore.h>
#include <chrono_monitor.h>
#include <chronolog_profile.h>

namespace chl = chronolog;
namespace tl = thallium;

namespace
{
constexpr uint64_t NANOSECONDS_PER_SECOND = 1000000000ULL;

bool retirePipelineOnStopEnabled()
{
    char const* value = std::getenv("CHRONOLOG_GRAPHER_RETIRE_ON_STOP");
    if(value == nullptr || *value == '\0')
    {
        return false;
    }
    return std::string(value) != "0";
}

uint64_t stopRetireGraceUs()
{
    char const* value = std::getenv("CHRONOLOG_GRAPHER_STOP_RETIRE_GRACE_US");
    if(value == nullptr || *value == '\0')
    {
        return 0;
    }

    char* end = nullptr;
    unsigned long long parsed = std::strtoull(value, &end, 10);
    if(end == value)
    {
        return 0;
    }
    return static_cast<uint64_t>(parsed);
}

bool stopCompletionWaitEnabled()
{
    char const* value = std::getenv("CHRONOLOG_GRAPHER_STOP_DRAIN_COMPLETE_WAIT");
    return value != nullptr && *value != '\0' && std::string(value) != "0";
}

bool stopCompletionWaitAsyncEnabled()
{
    char const* value = std::getenv("CHRONOLOG_GRAPHER_STOP_DRAIN_COMPLETE_WAIT_ASYNC");
    return value != nullptr && *value != '\0' && std::string(value) != "0";
}

bool stopCompletionWaitOutsideLockEnabled()
{
    char const* value = std::getenv("CHRONOLOG_GRAPHER_STOP_DRAIN_WAIT_OUTSIDE_LOCK");
    return value != nullptr && *value != '\0' && std::string(value) != "0";
}

uint64_t stopCompletionWaitTimeoutMs()
{
    char const* value = std::getenv("CHRONOLOG_GRAPHER_STOP_DRAIN_COMPLETE_WAIT_TIMEOUT_MS");
    if(value == nullptr || *value == '\0')
    {
        return 30000;
    }

    char* end = nullptr;
    unsigned long long parsed = std::strtoull(value, &end, 10);
    if(end == value || parsed > std::numeric_limits<uint64_t>::max())
    {
        return 30000;
    }
    return static_cast<uint64_t>(parsed);
}
}

////////////////////////

int chronolog::GrapherDataStore::startStoryRecording(std::string const& chronicle,
                                                     std::string const& story,
                                                     chronolog::StoryId const& story_id,
                                                     uint64_t start_time)
{
    CL_PROFILE_REGION("grapher_start_story");
    CL_PROFILE_REGION("metadata_lookup");
    LOG_INFO("[GrapherDataStore] Start recording story: Chronicle={}, Story={}, StoryId={}",
             chronicle,
             story,
             story_id);

    // Get dataStoreMutex, check for story_id_presence & add new StoryPipeline if needed
    std::lock_guard storeLock(dataStoreMutex);
    auto pipeline_iter = theMapOfStoryPipelines.find(story_id);
    if(pipeline_iter != theMapOfStoryPipelines.end())
    {
        LOG_INFO("[GrapherDataStore] Story already being recorded. StoryId: {}", story_id);
        //check it the pipeline was put on the waitingForExit list by the previous acquisition
        // and remove it from there
        auto waiting_iter = pipelinesWaitingForExit.find(story_id);
        if(waiting_iter != pipelinesWaitingForExit.end())
        {
            pipelinesWaitingForExit.erase(waiting_iter);
        }
        pipelinesInStopRetireGrace.erase(story_id);
        pipelinesInStopCompletionWait.erase(story_id);

        return chronolog::CL_SUCCESS;
    }

    auto result = theMapOfStoryPipelines.emplace(
            std::pair<chl::StoryId, chl::StoryPipeline*>(story_id,
                                                         new chl::StoryPipeline(theExtractionQueue,
                                                                                chronicle,
                                                                                story,
                                                                                story_id,
                                                                                start_time,
                                                                                story_chunk_duration_secs,
                                                                                acceptance_window_secs)));

    if(result.second)
    {
        LOG_INFO("[GrapherDataStore] New StoryPipeline created successfully. StoryId {}", story_id);
        pipeline_iter = result.first;
        //engage StoryPipeline with the IngestionQueue
        chl::StoryChunkIngestionHandle* ingestionHandle = (*pipeline_iter).second->getActiveIngestionHandle();
        theIngestionQueue.addStoryIngestionHandle(story_id, ingestionHandle);
        return chronolog::CL_SUCCESS;
    }
    else
    {
        LOG_ERROR(
                "[GrapherDataStore] Failed to create StoryPipeline for StoryId: {}. Possible memory or resource issue.",
                story_id);
        return chronolog::CL_ERR_UNKNOWN;
    }
}
////////////////////////

int chronolog::GrapherDataStore::stopStoryRecording(chronolog::StoryId const& story_id,
                                                    uint64_t extraction_drain_timeout_ms,
                                                    uint32_t expected_keeper_drains)
{
    auto const stop_profile_start = std::chrono::high_resolution_clock::now();
    double initial_lock_wait_us = 0.0;
    double initial_lock_hold_us = 0.0;
    double completion_wait_us = 0.0;
    double collect_erase_us = 0.0;
    LOG_DEBUG("[GrapherDataStore] Initiating stop recording for StoryId={}", story_id);
    LOG_INFO("[GrapherDataStore] Stop recording expected Keeper drains: StoryId={} expectedKeeperDrains={}",
             story_id,
             expected_keeper_drains);
    // we do not yet disengage the StoryPipeline from the IngestionQueue right away
    // but put it on the WaitingForExit list to be finalized, persisted to disk , and
    // removed from memory at exit_time = now+acceptance_window...
    // unless there's a new story acquisition request comes before that moment
    StoryPipeline* pipeline_to_retire = nullptr;
    uint64_t retire_grace_us = 0;
    uint64_t retire_grace_token = 0;
    {
        auto const lock_request = std::chrono::high_resolution_clock::now();
        std::unique_lock<std::mutex> storeLock(dataStoreMutex);
        auto lock_acquired = std::chrono::high_resolution_clock::now();
        initial_lock_wait_us =
                std::chrono::duration_cast<std::chrono::nanoseconds>(lock_acquired - lock_request).count() / 1000.0;
        auto pipeline_iter = theMapOfStoryPipelines.find(story_id);
        if(pipeline_iter != theMapOfStoryPipelines.end())
        {
            if(retirePipelineOnStopEnabled())
            {
                StoryPipeline* pipeline = pipeline_iter->second;
                retire_grace_us = stopRetireGraceUs();
                bool const completion_wait_enabled =
                        stopCompletionWaitEnabled() && expected_keeper_drains > 0 && retire_grace_us == 0;
                bool const completion_wait_async_enabled =
                        completion_wait_enabled && stopCompletionWaitAsyncEnabled();
                bool const completion_wait_outside_lock_enabled =
                        completion_wait_enabled && !completion_wait_async_enabled
                        && stopCompletionWaitOutsideLockEnabled();
                if(completion_wait_enabled)
                {
                    uint64_t const wait_timeout_ms = stopCompletionWaitTimeoutMs();
                    if(completion_wait_async_enabled)
                    {
                        retire_grace_token =
                                static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
                        pipelinesInStopCompletionWait[pipeline->getStoryId()] = retire_grace_token;
                        LOG_INFO("[GrapherDataStore] Async stop drain-complete wait started. StoryId={} "
                                 "expectedKeeperDrains={} timeout_ms={} token={}",
                                 pipeline->getStoryId(),
                                 expected_keeper_drains,
                                 wait_timeout_ms,
                                 retire_grace_token);

                        std::thread([this,
                                     story_id,
                                     expected_keeper_drains,
                                     wait_timeout_ms,
                                     retire_grace_token,
                                     extraction_drain_timeout_ms]() {
                            auto const async_profile_start = std::chrono::high_resolution_clock::now();
                            uint64_t const count_before = theIngestionQueue.storyDrainCompletionCount(story_id);
                            auto const wait_start = std::chrono::steady_clock::now();
                            bool const complete = theIngestionQueue.waitForStoryDrainCompletions(
                                    story_id, expected_keeper_drains, wait_timeout_ms);
                            auto const wait_end = std::chrono::steady_clock::now();
                            uint64_t const count_after = theIngestionQueue.storyDrainCompletionCount(story_id);
                            auto const wait_us =
                                    std::chrono::duration_cast<std::chrono::microseconds>(wait_end - wait_start)
                                            .count();
                            LOG_INFO("[GrapherDataStore] Stop drain-complete wait finished. StoryId={} enabled=1 "
                                     "async=1 expectedKeeperDrains={} complete={} countBefore={} countAfter={} "
                                     "timeout_ms={} wait_us={}",
                                     story_id,
                                     expected_keeper_drains,
                                     complete ? 1 : 0,
                                     count_before,
                                     count_after,
                                     wait_timeout_ms,
                                     wait_us);

                            StoryPipeline* async_pipeline_to_retire = nullptr;
                            double async_lock_wait_us = 0.0;
                            double async_collect_erase_us = 0.0;
                            if(complete)
                            {
                                auto const lock_request = std::chrono::high_resolution_clock::now();
                                std::unique_lock<std::mutex> storeLock(dataStoreMutex);
                                auto const lock_acquired = std::chrono::high_resolution_clock::now();
                                async_lock_wait_us =
                                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                lock_acquired - lock_request).count() / 1000.0;
                                auto token_iter = pipelinesInStopCompletionWait.find(story_id);
                                auto pipeline_iter = theMapOfStoryPipelines.find(story_id);
                                if(token_iter != pipelinesInStopCompletionWait.end()
                                   && token_iter->second == retire_grace_token
                                   && pipeline_iter != theMapOfStoryPipelines.end())
                                {
                                    auto const collect_erase_start = std::chrono::high_resolution_clock::now();
                                    StoryPipeline* pipeline = pipeline_iter->second;
                                    pipeline->collectIngestedEvents();
                                    theMapOfStoryPipelines.erase(pipeline->getStoryId());
                                    pipelinesWaitingForExit.erase(pipeline->getStoryId());
                                    pipelinesInStopRetireGrace.erase(pipeline->getStoryId());
                                    pipelinesInStopCompletionWait.erase(pipeline->getStoryId());
                                    theIngestionQueue.removeStoryIngestionHandle(pipeline->getStoryId());
                                    async_pipeline_to_retire = pipeline;
                                    auto const collect_erase_end = std::chrono::high_resolution_clock::now();
                                    async_collect_erase_us =
                                            std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                    collect_erase_end - collect_erase_start).count() / 1000.0;
                                }
                                else
                                {
                                    LOG_INFO("[GrapherDataStore] Async stop drain-complete wait cancelled. StoryId={} "
                                             "token={}",
                                             story_id,
                                             retire_grace_token);
                                }
                            }
                            else
                            {
                                std::lock_guard storeLock(dataStoreMutex);
                                auto token_iter = pipelinesInStopCompletionWait.find(story_id);
                                if(token_iter != pipelinesInStopCompletionWait.end()
                                   && token_iter->second == retire_grace_token)
                                {
                                    pipelinesInStopCompletionWait.erase(token_iter);
                                }
                                LOG_ERROR("[GrapherDataStore] Async stop drain-complete wait timed out. StoryId={} "
                                          "expectedKeeperDrains={} countAfter={} timeout_ms={} token={}",
                                          story_id,
                                          expected_keeper_drains,
                                          count_after,
                                          wait_timeout_ms,
                                          retire_grace_token);
                            }

                            if(async_pipeline_to_retire != nullptr)
                            {
                                auto const finalize_start = std::chrono::high_resolution_clock::now();
                                delete async_pipeline_to_retire;
                                auto const finalize_end = std::chrono::high_resolution_clock::now();

                                bool drained = true;
                                auto drain_start = finalize_end;
                                auto drain_end = finalize_end;
                                if(extraction_drain_timeout_ms > 0)
                                {
                                    drain_start = std::chrono::high_resolution_clock::now();
                                    drained = theExtractionQueue.waitForStoryDrain(story_id, extraction_drain_timeout_ms);
                                    drain_end = std::chrono::high_resolution_clock::now();
                                }

                                LOG_INFO("[GrapherDataStore] Async stop story retirement completed. StoryId={} "
                                         "drainEnabled={} drained={} queuedChunks={} inFlightChunks={} "
                                         "finalize_us={} drain_wait_us={} wait_us={} token={}",
                                         story_id,
                                         extraction_drain_timeout_ms > 0,
                                         drained,
                                         theExtractionQueue.queuedStoryChunkCount(story_id),
                                         theExtractionQueue.inFlightStoryChunkCount(story_id),
                                         std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                 finalize_end - finalize_start).count() / 1000.0,
                                         std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                 drain_end - drain_start).count() / 1000.0,
                                         wait_us,
                                         retire_grace_token);
                                auto const async_profile_end = std::chrono::high_resolution_clock::now();
                                LOG_INFO("[GrapherStopRetireProfile] story_id={} async=1 "
                                         "initial_lock_wait_us=0 initial_lock_hold_us=0 completion_wait_us={} "
                                         "collect_erase_us={} async_lock_wait_us={} finalize_us={} "
                                         "archive_drain_wait_us={} total_us={}",
                                         story_id,
                                         wait_us,
                                         async_collect_erase_us,
                                         async_lock_wait_us,
                                         std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                 finalize_end - finalize_start).count() / 1000.0,
                                         std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                 drain_end - drain_start).count() / 1000.0,
                                         std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                 async_profile_end - async_profile_start).count() / 1000.0);
                            }
                        }).detach();

                        auto const lock_released = std::chrono::high_resolution_clock::now();
                        initial_lock_hold_us +=
                                std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        lock_released - lock_acquired).count() / 1000.0;
                        return chronolog::CL_SUCCESS;
                    }
                    uint64_t const count_before = theIngestionQueue.storyDrainCompletionCount(story_id);
                    if(completion_wait_outside_lock_enabled)
                    {
                        auto const lock_release_for_wait = std::chrono::high_resolution_clock::now();
                        initial_lock_hold_us +=
                                std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        lock_release_for_wait - lock_acquired).count() / 1000.0;
                        storeLock.unlock();
                    }
                    auto const wait_start = std::chrono::steady_clock::now();
                    bool const complete = theIngestionQueue.waitForStoryDrainCompletions(
                            story_id, expected_keeper_drains, wait_timeout_ms);
                    auto const wait_end = std::chrono::steady_clock::now();
                    uint64_t const count_after = theIngestionQueue.storyDrainCompletionCount(story_id);
                    LOG_INFO("[GrapherDataStore] Stop drain-complete wait finished. StoryId={} enabled=1 "
                             "async=0 expectedKeeperDrains={} complete={} countBefore={} countAfter={} timeout_ms={} "
                             "wait_us={}",
                             story_id,
                             expected_keeper_drains,
                             complete ? 1 : 0,
                             count_before,
                             count_after,
                             wait_timeout_ms,
                             std::chrono::duration_cast<std::chrono::microseconds>(wait_end - wait_start).count());
                    completion_wait_us =
                            std::chrono::duration_cast<std::chrono::microseconds>(wait_end - wait_start).count();
                    if(!complete)
                    {
                        return chronolog::CL_ERR_UNKNOWN;
                    }
                    if(completion_wait_outside_lock_enabled)
                    {
                        auto const relock_request = std::chrono::high_resolution_clock::now();
                        storeLock.lock();
                        lock_acquired = std::chrono::high_resolution_clock::now();
                        initial_lock_wait_us +=
                                std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        lock_acquired - relock_request).count() / 1000.0;
                        pipeline_iter = theMapOfStoryPipelines.find(story_id);
                        if(pipeline_iter == theMapOfStoryPipelines.end())
                        {
                            LOG_INFO("[GrapherDataStore] Stop drain-complete wait outside lock cancelled. StoryId={}",
                                     story_id);
                            return chronolog::CL_SUCCESS;
                        }
                        pipeline = pipeline_iter->second;
                    }
                }
                retire_grace_token =
                        static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
                LOG_INFO("[GrapherDataStore] Retiring pipeline on stop: StoryId {} timeline {}-{} acceptanceWindow {}",
                         pipeline->getStoryId(),
                         pipeline->TimelineStart(),
                         pipeline->TimelineEnd(),
                         pipeline->getAcceptanceWindow());
                if(retire_grace_us > 0)
                {
                    pipelinesInStopRetireGrace[pipeline->getStoryId()] = retire_grace_token;
                    LOG_INFO("[GrapherDataStore] Stop-retire grace started. StoryId={} grace_us={} token={}",
                             pipeline->getStoryId(),
                             retire_grace_us,
                             retire_grace_token);
                }
                else
                {
                    auto const collect_erase_start = std::chrono::high_resolution_clock::now();
                    pipeline->collectIngestedEvents();
                    theMapOfStoryPipelines.erase(pipeline->getStoryId());
                    pipelinesWaitingForExit.erase(pipeline->getStoryId());
                    theIngestionQueue.removeStoryIngestionHandle(pipeline->getStoryId());
                    pipeline_to_retire = pipeline;
                    auto const collect_erase_end = std::chrono::high_resolution_clock::now();
                    collect_erase_us =
                            std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    collect_erase_end - collect_erase_start).count() / 1000.0;
                }
            }
            else
            {
                uint64_t now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
                uint64_t inactive_delay_ns = static_cast<uint64_t>(inactive_pipeline_delay_secs) * NANOSECONDS_PER_SECOND;
                uint64_t exit_time = now + inactive_delay_ns;
                // (*pipeline_iter).second->getAcceptanceWindow();
                pipelinesWaitingForExit[(*pipeline_iter).first] =
                        (std::pair<chl::StoryPipeline*, uint64_t>((*pipeline_iter).second, exit_time));
                LOG_INFO("[GrapherDataStore] Scheduled pipeline to retire: StoryId {} timeline {}-{} acceptanceWindow {} "
                         "inactiveDelaySecs {} inactiveDelayNs {} now {} retirementTime {}",
                         (*pipeline_iter).second->getStoryId(),
                         (*pipeline_iter).second->TimelineStart(),
                         (*pipeline_iter).second->TimelineEnd(),
                         (*pipeline_iter).second->getAcceptanceWindow(),
                         inactive_pipeline_delay_secs,
                         inactive_delay_ns,
                         now,
                         exit_time);
            }
        }
        else
        {
            LOG_WARNING("[GrapherDataStore] Attempt to stop recording for non-existent StoryId={}", story_id);
        }
        auto const lock_released = std::chrono::high_resolution_clock::now();
        initial_lock_hold_us +=
                std::chrono::duration_cast<std::chrono::nanoseconds>(lock_released - lock_acquired).count() / 1000.0;
    }

    if(retire_grace_us > 0)
    {
        auto const grace_start = std::chrono::steady_clock::now();
        auto const grace_deadline = grace_start + std::chrono::microseconds(retire_grace_us);
        uint64_t collection_passes = 0;
        while(std::chrono::steady_clock::now() < grace_deadline)
        {
            theIngestionQueue.drainOrphanChunks();
            {
                std::lock_guard storeLock(dataStoreMutex);
                auto grace_iter = pipelinesInStopRetireGrace.find(story_id);
                auto pipeline_iter = theMapOfStoryPipelines.find(story_id);
                if(grace_iter == pipelinesInStopRetireGrace.end() || grace_iter->second != retire_grace_token
                   || pipeline_iter == theMapOfStoryPipelines.end())
                {
                    LOG_INFO("[GrapherDataStore] Stop-retire grace cancelled. StoryId={} token={}",
                             story_id,
                             retire_grace_token);
                    return chronolog::CL_SUCCESS;
                }
                pipeline_iter->second->collectIngestedEvents();
                ++collection_passes;
            }

            auto const now = std::chrono::steady_clock::now();
            if(now >= grace_deadline)
            {
                break;
            }
            auto const remaining_us =
                    std::chrono::duration_cast<std::chrono::microseconds>(grace_deadline - now).count();
            useconds_t const sleep_us = static_cast<useconds_t>(remaining_us < 1000 ? remaining_us : 1000);
            if(sleep_us > 0)
            {
                usleep(sleep_us);
            }
        }

        {
            std::lock_guard storeLock(dataStoreMutex);
            auto grace_iter = pipelinesInStopRetireGrace.find(story_id);
            auto pipeline_iter = theMapOfStoryPipelines.find(story_id);
            if(grace_iter != pipelinesInStopRetireGrace.end() && grace_iter->second == retire_grace_token
               && pipeline_iter != theMapOfStoryPipelines.end())
            {
                auto const collect_erase_start = std::chrono::high_resolution_clock::now();
                StoryPipeline* pipeline = pipeline_iter->second;
                pipeline->collectIngestedEvents();
                theMapOfStoryPipelines.erase(pipeline->getStoryId());
                pipelinesWaitingForExit.erase(pipeline->getStoryId());
                pipelinesInStopRetireGrace.erase(pipeline->getStoryId());
                theIngestionQueue.removeStoryIngestionHandle(pipeline->getStoryId());
                pipeline_to_retire = pipeline;
                auto const collect_erase_end = std::chrono::high_resolution_clock::now();
                collect_erase_us =
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                                collect_erase_end - collect_erase_start).count() / 1000.0;
            }
            else
            {
                LOG_INFO("[GrapherDataStore] Stop-retire grace cancelled before finalize. StoryId={} token={}",
                         story_id,
                         retire_grace_token);
                return chronolog::CL_SUCCESS;
            }
        }

        auto const grace_end = std::chrono::steady_clock::now();
        LOG_INFO("[GrapherDataStore] Stop-retire grace completed. StoryId={} grace_us={} elapsed_us={} "
                 "collection_passes={}",
                 story_id,
                 retire_grace_us,
                 std::chrono::duration_cast<std::chrono::microseconds>(grace_end - grace_start).count(),
                 collection_passes);
    }

    if(pipeline_to_retire != nullptr)
    {
        auto const finalize_start = std::chrono::high_resolution_clock::now();
        delete pipeline_to_retire;
        auto const finalize_end = std::chrono::high_resolution_clock::now();

        bool drained = true;
        auto drain_start = finalize_end;
        auto drain_end = finalize_end;
        if(extraction_drain_timeout_ms > 0)
        {
            drain_start = std::chrono::high_resolution_clock::now();
            drained = theExtractionQueue.waitForStoryDrain(story_id, extraction_drain_timeout_ms);
            drain_end = std::chrono::high_resolution_clock::now();
        }

        LOG_INFO("[GrapherDataStore] Stop story archive drain completed. StoryId={} drainEnabled={} drained={} "
                 "queuedChunks={} inFlightChunks={} finalize_us={} drain_wait_us={}",
                 story_id,
                 extraction_drain_timeout_ms > 0,
                 drained,
                 theExtractionQueue.queuedStoryChunkCount(story_id),
                 theExtractionQueue.inFlightStoryChunkCount(story_id),
                 std::chrono::duration_cast<std::chrono::nanoseconds>(finalize_end - finalize_start).count() / 1000.0,
                 std::chrono::duration_cast<std::chrono::nanoseconds>(drain_end - drain_start).count() / 1000.0);
        auto const stop_profile_end = std::chrono::high_resolution_clock::now();
        LOG_INFO("[GrapherStopRetireProfile] story_id={} async=0 initial_lock_wait_us={} initial_lock_hold_us={} "
                 "completion_wait_us={} collect_erase_us={} async_lock_wait_us=0 finalize_us={} "
                 "archive_drain_wait_us={} total_us={}",
                 story_id,
                 initial_lock_wait_us,
                 initial_lock_hold_us,
                 completion_wait_us,
                 collect_erase_us,
                 std::chrono::duration_cast<std::chrono::nanoseconds>(finalize_end - finalize_start).count() / 1000.0,
                 std::chrono::duration_cast<std::chrono::nanoseconds>(drain_end - drain_start).count() / 1000.0,
                 std::chrono::duration_cast<std::chrono::nanoseconds>(
                         stop_profile_end - stop_profile_start).count() / 1000.0);
        return drained ? chronolog::CL_SUCCESS : chronolog::CL_ERR_UNKNOWN;
    }
    return chronolog::CL_SUCCESS;
}

////////////////////////

void chronolog::GrapherDataStore::collectIngestedEvents()
{
    CL_PROFILE_REGION("grapher_collect_ingested");
    LOG_DEBUG("[GrapherDataStore] Initiating collection of ingested story chunks. Current state={}, Active "
              "StoryPipelines={}, PipelinesWaitingForExit={}, ThreadID={}",
              state,
              theMapOfStoryPipelines.size(),
              pipelinesWaitingForExit.size(),
              tl::thread::self_id());
    theIngestionQueue.drainOrphanChunks();

    std::lock_guard storeLock(dataStoreMutex);
    for(auto pipeline_iter = theMapOfStoryPipelines.begin(); pipeline_iter != theMapOfStoryPipelines.end();
        ++pipeline_iter)
    {
        //INNA: this can be delegated to different threads handling individual storylines...
        (*pipeline_iter).second->collectIngestedEvents();
    }
}

////////////////////////
void chronolog::GrapherDataStore::extractDecayedStoryChunks()
{
    CL_PROFILE_REGION("grapher_extract_decayed");
    LOG_DEBUG("[GrapherDataStore] Initiating extraction of decayed story chunks. Current state={}, Active "
              "StoryPipelines={}, PipelinesWaitingForExit={}, ThreadID={}",
              state,
              theMapOfStoryPipelines.size(),
              pipelinesWaitingForExit.size(),
              tl::thread::self_id());

    uint64_t current_time = std::chrono::high_resolution_clock::now().time_since_epoch().count();

    std::lock_guard storeLock(dataStoreMutex);
    for(auto pipeline_iter = theMapOfStoryPipelines.begin(); pipeline_iter != theMapOfStoryPipelines.end();
        ++pipeline_iter)
    {
        (*pipeline_iter).second->extractDecayedStoryChunks(current_time);
    }
}
////////////////////////

void chronolog::GrapherDataStore::retireDecayedPipelines()
{
    CL_PROFILE_REGION("grapher_retire_pipelines");
    LOG_TRACE("[GrapherDataStore] Initiating retirement of decayed pipelines. Current state={}, Active "
              "StoryPipelines={}, PipelinesWaitingForExit={}, ThreadID={}",
              state,
              theMapOfStoryPipelines.size(),
              pipelinesWaitingForExit.size(),
              tl::thread::self_id());

    if(!theMapOfStoryPipelines.empty())
    {
        std::lock_guard storeLock(dataStoreMutex);

        uint64_t current_time = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        for(auto pipeline_iter = pipelinesWaitingForExit.begin(); pipeline_iter != pipelinesWaitingForExit.end();)
        {
            if(current_time >= (*pipeline_iter).second.second)
            {
                //current_time >= pipeline exit_time
                StoryPipeline* pipeline = (*pipeline_iter).second.first;
                LOG_DEBUG("[GrapherDataStore] retiring pipeline StoryId {} timeline {}-{} acceptanceWindow {} "
                          "retirementTime {}",
                          pipeline->getStoryId(),
                          pipeline->TimelineStart(),
                          pipeline->TimelineEnd(),
                          pipeline->getAcceptanceWindow(),
                          (*pipeline_iter).second.second);
                theMapOfStoryPipelines.erase(pipeline->getStoryId());
                theIngestionQueue.removeStoryIngestionHandle(pipeline->getStoryId());
                pipeline_iter = pipelinesWaitingForExit.erase(pipeline_iter);
                delete pipeline;
            }
            else
            {
                pipeline_iter++;
            }
        }
    }

    LOG_TRACE("[GrapherDataStore] Completed retirement of decayed pipelines. Current state={}, Active "
              "StoryPipelines={}, PipelinesWaitingForExit={}, ThreadID={}",
              state,
              theMapOfStoryPipelines.size(),
              pipelinesWaitingForExit.size(),
              tl::thread::self_id());
}

void chronolog::GrapherDataStore::dataCollectionTask()
{
    //run dataCollectionTask as long as the state == RUNNING
    // or there're still events left to collect and
    // storyPipelines left to retire...
    tl::xstream es = tl::xstream::self();
    LOG_DEBUG("[GrapherDataStore] Initiating DataCollectionTask. ESrank={}, ThreadID={}",
              es.get_rank(),
              tl::thread::self_id());

    while(!is_shutting_down() || !theIngestionQueue.is_empty() || !theMapOfStoryPipelines.empty())
    {
        LOG_DEBUG("[GrapherDataStore] Running DataCollection iteration. ESrank={}, ThreadID={}",
                  es.get_rank(),
                  tl::thread::self_id());
        for(int i = 0; i < 1; ++i)
        {
            collectIngestedEvents();
            usleep(data_collection_poll_interval_us);
        }
        extractDecayedStoryChunks();
        retireDecayedPipelines();
        if(is_shutting_down())
        {
            bool no_active_pipelines = false;
            {
                std::lock_guard storeLock(dataStoreMutex);
                no_active_pipelines = theMapOfStoryPipelines.empty();
            }
            if(no_active_pipelines)
            {
                theIngestionQueue.discardOrphanChunks("shutdown_no_active_story_pipelines");
            }
        }
    }
    LOG_DEBUG("[GrapherDataStore] Exiting DataCollectionTask thread {}", tl::thread::self_id());
}

////////////////////////
void chronolog::GrapherDataStore::startDataCollection(int stream_count, int threads_per_stream)
{
    std::lock_guard storeLock(dataStoreStateMutex);
    if(is_running() || is_shutting_down())
    {
        LOG_INFO("[GrapherDataStore] Data collection is already running or shutting down. Ignoring request.");
        return;
    }

    if(stream_count <= 0)
    {
        stream_count = 1;
    }
    if(threads_per_stream <= 0)
    {
        threads_per_stream = 1;
    }

    LOG_INFO("[GrapherDataStore] Starting data collection. StreamCount={} ThreadsPerStream={} ThreadID={}",
             stream_count,
             threads_per_stream,
             tl::thread::self_id());
    state = RUNNING;

    for(int i = 0; i < stream_count; ++i)
    {
        tl::managed<tl::xstream> es = tl::xstream::create();
        dataStoreStreams.push_back(std::move(es));
    }

    for(int i = 0; i < threads_per_stream * stream_count; ++i)
    {
        tl::managed<tl::thread> th =
                dataStoreStreams[i % (dataStoreStreams.size())]->make_thread([p = this]() { p->dataCollectionTask(); });
        dataStoreThreads.push_back(std::move(th));
    }
    LOG_INFO("[GrapherDataStore] Data collection started successfully. StreamCount={} ThreadsPerStream={} ThreadID={}",
             stream_count,
             threads_per_stream,
             tl::thread::self_id());
}
//////////////////////////////

void chronolog::GrapherDataStore::shutdownDataCollection()
{
    LOG_INFO("[GrapherDataStore] Initiating shutdown of DataCollection. CurrentState={}, Active StoryPipelines={}, "
             "PipelinesWaitingForExit={}",
             state,
             theMapOfStoryPipelines.size(),
             pipelinesWaitingForExit.size());

    // switch the state to shuttingDown
    std::lock_guard storeLock(dataStoreStateMutex);
    if(is_shutting_down())
    {
        LOG_INFO("[GrapherDataStore] Data collection is already shutting down. Ignoring additional shutdown request.");
        return;
    }
    state = SHUTTING_DOWN;

    if(!theMapOfStoryPipelines.empty())
    {
        // label all existing Pipelines as waiting to exit
        std::lock_guard storeLock(dataStoreMutex);
        uint64_t current_time = std::chrono::high_resolution_clock::now().time_since_epoch().count();

        for(auto pipeline_iter = theMapOfStoryPipelines.begin(); pipeline_iter != theMapOfStoryPipelines.end();
            ++pipeline_iter)
        {
            if(pipelinesWaitingForExit.find((*pipeline_iter).first) == pipelinesWaitingForExit.end())
            {
                uint64_t exit_time = current_time + (*pipeline_iter).second->getAcceptanceWindow();
                pipelinesWaitingForExit[(*pipeline_iter).first] =
                        (std::pair<chl::StoryPipeline*, uint64_t>((*pipeline_iter).second, exit_time));
            }
        }
    }

    // Join threads & execution streams while holding stateMutex
    // and just wait until all the events are collected and
    // all the storyPipelines decay and retire
    for(auto& th: dataStoreThreads) { th->join(); }
    LOG_INFO("[GrapherDataStore] All data collection threads have been joined.");

    for(auto& es: dataStoreStreams) { es->join(); }
    LOG_INFO("[GrapherDataStore] All data collection streams have been joined.");
    LOG_INFO("[GrapherDataStore] DataCollection shutdown completed.");
}

///////////////////////

//
chronolog::GrapherDataStore::~GrapherDataStore()
{
    LOG_INFO("[GrapherDataStore] Destructor called. Initiating shutdown. Active StoryPipelines count={}",
             theMapOfStoryPipelines.size());
    shutdownDataCollection();
    LOG_INFO("[GrapherDataStore] Shutdown completed successfully. Active StoryPipelines count={}",
             theMapOfStoryPipelines.size());
}
