#ifndef DataStoreAdmin_SERVICE_H
#define DataStoreAdmin_SERVICE_H

#include <iostream>
#include <string>
#include <cstdint>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <thread>
#include <margo.h>
#include <thallium.hpp>
#include <thallium/serialization/stl/string.hpp>

#include <chronolog_types.h>
#include <chronolog_errcode.h>

#include "KeeperAppendStats.h"
#include "KeeperDataStore.h"
#include "KeeperLocalJournal.h"
#include "KeeperRecordingService.h"

namespace tl = thallium;

namespace chronolog
{

class DataStoreAdminService: public tl::provider<DataStoreAdminService>
{
public:
    // Service should be created on the heap not the stack thus the constructor is private...
    static DataStoreAdminService*
    CreateDataStoreAdminService(tl::engine& tl_engine, uint16_t service_provider_id, KeeperDataStore& dataStoreInstance)
    {
        return new DataStoreAdminService(tl_engine, service_provider_id, dataStoreInstance);
    }

    ~DataStoreAdminService()
    {
        LOG_DEBUG("[DataStoreAdminService] Destructor called. Cleaning up...");
        //remove provider finalization callback from the engine's list
        get_engine().pop_finalize_callback(this);
    }

    void collection_service_available(tl::request const& request) { request.respond(1); }

    void shutdown_data_collection(tl::request const& request)
    {
        int status = 1;
        theDataStore.shutdownDataCollection();
        request.respond(status);
    }

    void StartStoryRecording(tl::request const& request,
                             std::string const& chronicle_name,
                             std::string const& story_name,
                             StoryId const& story_id,
                             uint64_t start_time)
    {
        LOG_INFO("[DataStoreAdminService] Starting Story Recording: StoryName={}, StoryID={}", story_name, story_id);
        int return_code = theDataStore.startStoryRecording(chronicle_name, story_name, story_id, start_time);
        request.respond(return_code);
    }

    void StopStoryRecording(tl::request const& request, StoryId const& story_id)
    {
        auto const total_start = std::chrono::steady_clock::now();
        auto elapsed_us = [](std::chrono::steady_clock::time_point start) {
            return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start)
                    .count();
        };
        LOG_INFO("[DataStoreAdminService] Stopping Story Recording: StoryID={}", story_id);
        int return_code = chronolog::CL_SUCCESS;
        auto const async_drain_start = std::chrono::steady_clock::now();
        if(!KeeperRecordingService::WaitForAsyncDrainIdle(stopStoryStatsWaitMs()))
        {
            LOG_WARNING("[DataStoreAdminService] Timed out waiting for Keeper async journal drain before StopStory. "
                        "StoryID={}",
                        story_id);
            return_code = chronolog::CL_ERR_UNKNOWN;
        }
        int64_t const async_drain_idle_us = elapsed_us(async_drain_start);
        auto const datastore_stop_start = std::chrono::steady_clock::now();
        int const stop_return = theDataStore.stopStoryRecording(story_id);
        int64_t const datastore_stop_us = elapsed_us(datastore_stop_start);
        if(stop_return != chronolog::CL_SUCCESS)
        {
            return_code = stop_return;
        }
        auto const journal_sync_start = std::chrono::steady_clock::now();
        if(!KeeperLocalJournal::instance().syncAll())
        {
            return_code = chronolog::CL_ERR_UNKNOWN;
        }
        int64_t const journal_sync_all_us = elapsed_us(journal_sync_start);
        auto const stats_settle_start = std::chrono::steady_clock::now();
        waitForAppendStatsToSettle();
        int64_t const stats_settle_us = elapsed_us(stats_settle_start);
        int64_t flush_story_us = 0;
        int flush_return = chronolog::CL_SUCCESS;
        if(stopStoryFlushEnabled())
        {
            auto const flush_start = std::chrono::steady_clock::now();
            flush_return = theDataStore.flushStoryRecording(story_id,
                                                            stopStoryFlushTimeoutMs(),
                                                            stopStoryFlushAsyncCompleteEnabled());
            flush_story_us = elapsed_us(flush_start);
            if(flush_return != chronolog::CL_SUCCESS)
            {
                return_code = flush_return;
            }
        }
        KeeperAppendStats::instance().logSummary("stop_story_recording");
        LOG_INFO("[KeeperStopStoryProfile] story_id={} return_code={} stop_return_code={} flush_return_code={} "
                 "flush_enabled={} flush_async_complete={} async_drain_idle_us={} datastore_stop_us={} journal_sync_all_us={} "
                 "stats_settle_us={} flush_story_us={} total_us={}",
                 story_id,
                 return_code,
                 stop_return,
                 flush_return,
                 stopStoryFlushEnabled() ? 1 : 0,
                 stopStoryFlushAsyncCompleteEnabled() ? 1 : 0,
                 async_drain_idle_us,
                 datastore_stop_us,
                 journal_sync_all_us,
                 stats_settle_us,
                 flush_story_us,
                 elapsed_us(total_start));
        request.respond(return_code);
    }

private:
    static bool stopStoryFlushEnabled()
    {
        char const* value = std::getenv("CHRONOLOG_KEEPER_STOP_STORY_FLUSH_DRAIN");
        if(value == nullptr || *value == '\0')
        {
            return false;
        }
        return std::string(value) != "0";
    }

    static uint64_t stopStoryFlushTimeoutMs()
    {
        char const* value = std::getenv("CHRONOLOG_KEEPER_STOP_STORY_FLUSH_DRAIN_TIMEOUT_MS");
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

    static bool stopStoryFlushAsyncCompleteEnabled()
    {
        char const* value = std::getenv("CHRONOLOG_KEEPER_STOP_STORY_FLUSH_DRAIN_ASYNC_COMPLETE");
        return value != nullptr && *value != '\0' && std::string(value) != "0";
    }

    static uint64_t stopStoryStatsWaitMs()
    {
        char const* value = std::getenv("CHRONOLOG_KEEPER_STOP_STORY_STATS_WAIT_MS");
        if(value == nullptr || *value == '\0')
        {
            return 0;
        }
        char* end = nullptr;
        unsigned long long parsed = std::strtoull(value, &end, 10);
        if(end == value || parsed > std::numeric_limits<uint64_t>::max())
        {
            return 0;
        }
        return static_cast<uint64_t>(parsed);
    }

    static void waitForAppendStatsToSettle()
    {
        uint64_t const wait_ms = stopStoryStatsWaitMs();
        if(wait_ms == 0)
        {
            return;
        }

        uint64_t waited_ms = 0;
        while(waited_ms < wait_ms)
        {
            KeeperAppendStats const& stats = KeeperAppendStats::instance();
            if(stats.currentRecordEventCount() >= stats.currentJournalAppendCount())
            {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            ++waited_ms;
        }
    }

    DataStoreAdminService(tl::engine& tl_engine, uint16_t service_provider_id, KeeperDataStore& data_store_instance)
        : tl::provider<DataStoreAdminService>(tl_engine, service_provider_id)
        , theDataStore(data_store_instance)
    {
        define("collection_service_available", &DataStoreAdminService::collection_service_available);
        define("shutdown_data_collection", &DataStoreAdminService::shutdown_data_collection);
        define("start_story_recording", &DataStoreAdminService::StartStoryRecording);
        define("stop_story_recording", &DataStoreAdminService::StopStoryRecording);
        //set up callback for the case when the engine is being finalized while this provider is still alive
        get_engine().push_finalize_callback(this, [p = this]() { delete p; });

        std::stringstream ss;
        ss << get_engine().self();
        LOG_INFO("[DataStoreAdminService] Constructed at {}. ProviderID={}", ss.str(), service_provider_id);
    }

    DataStoreAdminService(DataStoreAdminService const&) = delete;

    DataStoreAdminService& operator=(DataStoreAdminService const&) = delete;

    KeeperDataStore& theDataStore;
};

} // namespace chronolog

#endif
