#ifndef CHRONOLOG_WATERMARK_REPORT_PUBLISHER_H
#define CHRONOLOG_WATERMARK_REPORT_PUBLISHER_H
#include <private/RpcBulkOwner.h>
#include <StoryWatermarkRegistry.h>
#include <ServiceId.h>
#include <private/StopPublicationProof.h>
namespace chronolog::rpc
{
class StopDependencyEvidence;
}
#include <memory>
namespace chronolog
{
// Best-effort one-way reports. Local send completion is not Keeper application
// or an archive receipt. An Unknown attempt keeps its target from sending
// again; its slot is given back once the forward has returned.
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
    // Local completion only. Repeated final passes do not resend an unchanged
    // known envelope; a changed envelope still uses the original actual C2.
    int publishFinal(rpc::RpcDeadline const& deadline);
    int finish(rpc::RpcDeadline const& deadline, rpc::StopDependencyEvidence const& evidence);
    // ACK requires native cleanup of every original send to this exact target.
    int park(rpc::PublisherParkProof const& proof, rpc::RpcDeadline const& deadline);
    void quiesce() noexcept;

private:
    int publishImpl(rpc::RpcDeadline const&, bool force, bool final, rpc::StopDependencyEvidence const*);
    struct Core;
    std::shared_ptr<Core> core_;
};
} // namespace chronolog
#endif
