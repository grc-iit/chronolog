// The grapher publishes watermark reports from its data-collection loop, which
// runs two threads on each execution stream. A report send waits for the
// network and resumes only when its stream schedules it again, so the thread
// next to it has to yield. If it does not, the rest of the round never goes
// out, and a keeper learns that a window was written only when another round
// happens to unblock the send, minutes later.
//
// The GrapherDataStore and the WatermarkReportPublisher are real; the keepers
// are stand-in providers that record the reports they receive.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>

#include <thallium.hpp>
#include <thallium/serialization/stl/map.hpp>

#include <chrono_monitor.h>
#include <ChunkIngestionQueue.h>
#include <ChunkReceipt.h>
#include <GrapherDataStore.h>
#include <ServiceId.h>
#include <StoryChunkExtractionQueue.h>
#include <StoryWatermarkRegistry.h>
#include <WatermarkReportPublisher.h>

namespace chl = chronolog;
namespace tl = thallium;

namespace
{
constexpr char kProtocol[] = "ofi+sockets";
constexpr uint16_t kKeeper1Provider = 11;
constexpr uint16_t kKeeper2Provider = 12;
constexpr chl::StoryId kStory = 5;

void ensureLogger()
{
    static bool done = false;
    if(!done)
    {
        chl::chrono_monitor::initialize("console",
                                        "",
                                        chl::LogLevel::err,
                                        "grapher_data_collection_report_test_logger");
        done = true;
    }
}

bool waitFor(std::function<bool()> const& condition, std::chrono::milliseconds timeout)
{
    auto const deadline = std::chrono::steady_clock::now() + timeout;
    while(!condition())
    {
        if(std::chrono::steady_clock::now() > deadline)
        {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

// Stands in for a keeper's DataStoreAdminService: remembers the last watermark
// reported for the story.
class RecordingKeeper: public tl::provider<RecordingKeeper>
{
public:
    RecordingKeeper(tl::engine& engine, uint16_t provider_id)
        : tl::provider<RecordingKeeper>(engine, provider_id)
    {
        define("report_story_watermarks", &RecordingKeeper::report_story_watermarks, tl::ignore_return_value());
    }

    void report_story_watermarks(tl::request const&, std::map<chl::StoryId, chl::StoryWatermarkReport> const& reports)
    {
        auto const story_report = reports.find(kStory);
        if(story_report != reports.end())
        {
            lastWatermark = story_report->second.watermark;
        }
    }

    std::atomic<uint64_t> lastWatermark{0};
};

class GrapherDataCollectionReport: public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureLogger();
        keeperEngine =
                std::make_unique<tl::engine>(std::string(kProtocol) + "://127.0.0.1", THALLIUM_SERVER_MODE, true, 1);
        grapherEngine =
                std::make_unique<tl::engine>(std::string(kProtocol) + "://127.0.0.1", THALLIUM_SERVER_MODE, true, 1);
        keeper1 = std::make_unique<RecordingKeeper>(*keeperEngine, kKeeper1Provider);
        keeper2 = std::make_unique<RecordingKeeper>(*keeperEngine, kKeeper2Provider);
    }

    void TearDown() override
    {
        publisher.reset();
        grapherEngine->finalize();
        keeper1.reset();
        keeper2.reset();
        keeperEngine->finalize();
    }

    chl::ServiceId keeperServiceId(uint16_t provider_id)
    {
        std::string const self = keeperEngine->self();
        auto const port = static_cast<uint16_t>(std::stoul(self.substr(self.rfind(':') + 1)));
        return chl::ServiceId(kProtocol, "127.0.0.1", port, provider_id);
    }

    std::unique_ptr<tl::engine> keeperEngine;
    std::unique_ptr<tl::engine> grapherEngine;
    std::unique_ptr<RecordingKeeper> keeper1;
    std::unique_ptr<RecordingKeeper> keeper2;
    chl::StoryWatermarkRegistry registry;
    std::unique_ptr<chl::WatermarkReportPublisher> publisher;
};
} // namespace

TEST_F(GrapherDataCollectionReport, ReportRoundFromTheLoopReachesEveryKeeper)
{
    publisher = std::make_unique<chl::WatermarkReportPublisher>(*grapherEngine, registry, 0);
    registry.registerStory(kStory, 100);
    publisher->recordContributor(kStory, keeperServiceId(kKeeper1Provider));
    publisher->recordContributor(kStory, keeperServiceId(kKeeper2Provider));
    registry.advancePersisted(kStory, 100, 200);

    chl::ChunkIngestionQueue ingestion_queue;
    chl::StoryChunkExtractionQueue extraction_queue;
    chl::GrapherDataStore data_store(ingestion_queue, extraction_queue, 4096, 60, 180, 300, nullptr, &registry);
    data_store.attachWatermarkPublisher(publisher.get());
    data_store.startDataCollection(1); // one execution stream, two loop threads

    EXPECT_TRUE(waitFor([&] { return keeper1->lastWatermark == 200 && keeper2->lastWatermark == 200; },
                        std::chrono::seconds(10)))
            << "keeper1 W=" << keeper1->lastWatermark << " keeper2 W=" << keeper2->lastWatermark;
    data_store.shutdownDataCollection();
}
