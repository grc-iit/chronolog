#ifndef CHRONOLOG_WATERMARK_REPORT_PUBLISHER_H
#define CHRONOLOG_WATERMARK_REPORT_PUBLISHER_H

#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <thallium.hpp>

#include <chronolog_types.h>
#include <ServiceId.h>

namespace tl = thallium;

namespace chronolog
{

class StoryWatermarkRegistry;

// Pushes per-story persisted watermarks W back to the keepers that
// contributed chunks. The recording service records each drained chunk's
// reporter identity (the keeper's DataStoreAdminService, carried with the
// chunk); publish() — called from the data-collection loop — takes the
// registry's dirty snapshot and sends one coalesced one-way
// report_story_watermarks(map<StoryId, W>) per contributing keeper.
//
// Reports are best-effort: they are one-way, a failed send drops the cached
// keeper handle (lazily rebuilt — the keeper may have restarted), and a lost
// report only delays eviction until the next W advance or the keeper's stall
// re-send; it can never cause data loss.
class WatermarkReportPublisher
{
public:
    WatermarkReportPublisher(tl::engine& tl_engine, StoryWatermarkRegistry& registry,
                             uint32_t report_interval_secs = 1);
    ~WatermarkReportPublisher();

    WatermarkReportPublisher(WatermarkReportPublisher const&) = delete;
    WatermarkReportPublisher& operator=(WatermarkReportPublisher const&) = delete;

    // Called by the recording service for every received chunk.
    void recordContributor(StoryId const& story_id, ServiceId const& reporter);

    // Rate-limited internally to one report round per report_interval_secs;
    // safe to call from multiple data-collection ULTs.
    void publish();

private:
    void sendReport(std::string const& endpoint_key, ServiceId const& keeper_id,
                    std::map<StoryId, uint64_t> const& watermarks);

    static std::string endpointKey(ServiceId const& service_id)
    {
        std::string key;
        service_id.get_service_as_string(key);
        key += "@" + std::to_string(service_id.getProviderId());
        return key;
    }

    tl::engine& theEngine;
    StoryWatermarkRegistry& theRegistry;
    std::chrono::seconds reportInterval;
    std::chrono::steady_clock::time_point lastPublish;
    tl::remote_procedure report_story_watermarks;

    std::mutex publisherMutex;
    // story -> contributing keepers, keyed by endpoint string (ServiceId has
    // no ordering). Entries persist for the process lifetime; a report for a
    // story a keeper no longer retains is a no-op on the keeper.
    std::map<StoryId, std::map<std::string, ServiceId>> contributors;
    // lazily built per-keeper provider handles; dropped on send failure
    std::map<std::string, tl::provider_handle> keeperHandles;
};

} // namespace chronolog

#endif // CHRONOLOG_WATERMARK_REPORT_PUBLISHER_H
