#ifndef CHRONOLOG_WATERMARK_REPORT_PUBLISHER_H
#define CHRONOLOG_WATERMARK_REPORT_PUBLISHER_H
#include <private/RpcBulkOwner.h>
#include <StoryWatermarkRegistry.h>
#include <ServiceId.h>
#include <memory>
namespace chronolog
{
// Best-effort one-way reports. Local send completion is not Keeper application
// or an archive receipt. Unknown attempts retain their owner and fixed charge.
class WatermarkReportPublisher
{
public:
    WatermarkReportPublisher(std::shared_ptr<rpc::RpcBulkOwner> const& owner,
                             std::shared_ptr<StoryWatermarkRegistry> registry,
                             uint32_t report_interval_secs = 1);
    ~WatermarkReportPublisher();
    WatermarkReportPublisher(WatermarkReportPublisher const&) = delete;
    WatermarkReportPublisher& operator=(WatermarkReportPublisher const&) = delete;
    void
    recordContributor(StoryId story, ServiceId const& reporter, StoryWatermarkRegistry::Incarnation incarnation = {});
    int publish(bool force = false);
    int publish(rpc::RpcDeadline const& deadline, bool force = false);
    void quiesce() noexcept;

private:
    struct Core;
    std::shared_ptr<Core> core_;
};
} // namespace chronolog
#endif
