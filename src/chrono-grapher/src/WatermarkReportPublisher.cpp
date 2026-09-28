#include <thallium/serialization/stl/map.hpp>

#include <chrono_monitor.h>
#include <StoryWatermarkRegistry.h>
#include <WatermarkReportPublisher.h>

namespace chl = chronolog;
namespace tl = thallium;

chronolog::WatermarkReportPublisher::WatermarkReportPublisher(tl::engine& tl_engine,
                                                              chl::StoryWatermarkRegistry& registry,
                                                              uint32_t report_interval_secs)
    : theEngine(tl_engine)
    , theRegistry(registry)
    , reportInterval(report_interval_secs)
    , lastPublish(std::chrono::steady_clock::now())
    , report_story_watermarks(tl_engine.define("report_story_watermarks").disable_response())
{
    LOG_INFO("[WatermarkReportPublisher] created, report interval {} s", report_interval_secs);
}

chronolog::WatermarkReportPublisher::~WatermarkReportPublisher()
{
    LOG_DEBUG("[WatermarkReportPublisher] Destructor called. Cleaning up...");
    report_story_watermarks.deregister();
}

void chronolog::WatermarkReportPublisher::recordContributor(chl::StoryId const& story_id,
                                                            chl::ServiceId const& reporter)
{
    if(!reporter.is_valid())
    {
        return; // sender that does not want reports (e.g. a player-bound leg)
    }
    std::lock_guard<std::mutex> lock(publisherMutex);
    auto emplaced = contributors[story_id].emplace(endpointKey(reporter), reporter);
    if(emplaced.second)
    {
        LOG_INFO("[WatermarkReportPublisher] StoryId={} gains contributor {}", story_id, chl::to_string(reporter));
    }
}

void chronolog::WatermarkReportPublisher::publish()
{
    // endpoint key -> (keeper identity, coalesced story->W report)
    std::map<std::string, std::pair<ServiceId, std::map<StoryId, uint64_t>>> reports;
    {
        std::lock_guard<std::mutex> lock(publisherMutex);
        auto const now = std::chrono::steady_clock::now();
        if(now - lastPublish < reportInterval)
        {
            return;
        }
        lastPublish = now;

        std::map<StoryId, uint64_t> dirty = theRegistry.snapshotDirty();
        if(dirty.empty())
        {
            return;
        }
        for(auto const& story_watermark: dirty)
        {
            auto contributor_iter = contributors.find(story_watermark.first);
            if(contributor_iter == contributors.end())
            {
                // W moved before any keeper chunk arrived (fresh registration):
                // nobody to tell, and nothing on any keeper waits for it
                continue;
            }
            for(auto const& keeper_entry: contributor_iter->second)
            {
                auto& report_slot = reports[keeper_entry.first];
                report_slot.first = keeper_entry.second;
                report_slot.second[story_watermark.first] = story_watermark.second;
            }
        }
    }
    // RPC sends happen outside the mutex: a slow lookup must not block the
    // recording service's recordContributor calls
    for(auto const& report_entry: reports)
    {
        sendReport(report_entry.first, report_entry.second.first, report_entry.second.second);
    }
}

void chronolog::WatermarkReportPublisher::sendReport(std::string const& endpoint_key,
                                                     chl::ServiceId const& keeper_id,
                                                     std::map<chl::StoryId, uint64_t> const& watermarks)
{
    try
    {
        tl::provider_handle keeper_handle;
        bool cached = false;
        {
            std::lock_guard<std::mutex> lock(publisherMutex);
            auto handle_iter = keeperHandles.find(endpoint_key);
            if(handle_iter != keeperHandles.end())
            {
                keeper_handle = handle_iter->second;
                cached = true;
            }
        }
        if(!cached)
        {
            std::string keeper_addr;
            keeper_id.get_service_as_string(keeper_addr);
            keeper_handle = tl::provider_handle(theEngine.lookup(keeper_addr), keeper_id.getProviderId());
            std::lock_guard<std::mutex> lock(publisherMutex);
            keeperHandles[endpoint_key] = keeper_handle;
        }

        report_story_watermarks.on(keeper_handle)(watermarks);
        LOG_DEBUG("[WatermarkReportPublisher] reported {} story watermark(s) to {}",
                  watermarks.size(),
                  chl::to_string(keeper_id));
    }
    catch(tl::exception const& ex)
    {
        // drop the cached handle: the keeper may have restarted at a new
        // margo instance; it is lazily rebuilt on the next report round
        LOG_WARNING("[WatermarkReportPublisher] failed to report to {} : {} — dropping cached handle",
                    chl::to_string(keeper_id),
                    ex.what());
        std::lock_guard<std::mutex> lock(publisherMutex);
        keeperHandles.erase(endpoint_key);
    }
    catch(...)
    {
        LOG_WARNING("[WatermarkReportPublisher] failed to report to {} (unknown exception) — dropping cached handle",
                    chl::to_string(keeper_id));
        std::lock_guard<std::mutex> lock(publisherMutex);
        keeperHandles.erase(endpoint_key);
    }
}
