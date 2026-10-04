#include "chrono-grapher/server/ClusterWorker.h"

#include <absl/log/log.h>
#include <algorithm>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <optional>
#include <utility>

#include "rpc/Channel.h"

namespace chronolog::grapher
{
namespace
{
template <class Response>
ClockAuditDecision auditReply(VisorClockAudit& audit,
                              const VisorClockAudit::Bracket& bracket,
                              const grpc::Status& status,
                              const Response& response)
{
    std::optional<TimeReading> visor;
    if(status.ok() && response.has_physical())
    {
        const auto& wire = response.physical();
        visor.emplace();
        visor->physical_ns = wire.physical_ns();
        if(wire.has_uncertainty_ns())
            visor->uncertainty_ns = wire.uncertainty_ns();
        visor->status = wire.status() == v1::CLOCK_STATUS_SYNCED     ? ClockStatus::Synced
                        : wire.status() == v1::CLOCK_STATUS_UNSYNCED ? ClockStatus::Unsynced
                                                                     : ClockStatus::Unavailable;
    }
    return audit.record(bracket,
                        status.ok(),
                        std::move(visor),
                        {response.clock_responder().replica_id(), response.clock_responder().instance()});
}
} // namespace

ClockAuditDecision auditVisorReply(VisorClockAudit& audit,
                                   const VisorClockAudit::Bracket& bracket,
                                   const grpc::Status& status,
                                   const internal::v1::RegisterResponse& response)
{
    return auditReply(audit, bracket, status, response);
}

ClockAuditDecision auditVisorReply(VisorClockAudit& audit,
                                   const VisorClockAudit::Bracket& bracket,
                                   const grpc::Status& status,
                                   const internal::v1::HeartbeatResponse& response)
{
    return auditReply(audit, bracket, status, response);
}

void runClusterWorker(std::stop_token stop,
                      internal::v1::Cluster::StubInterface& stub,
                      VisorClockAudit& audit,
                      const ClusterWorkerOptions& options)
{
    bool registered = false;
    size_t policy_cursor = 0;
    std::mutex mutex;
    std::condition_variable_any pause;
    while(!stop.stop_requested())
    {
        grpc::ClientContext context;
        rpc::withTimeout(context, options.rpc_timeout);
        std::stop_callback cancelled(stop, [&] { context.TryCancel(); });
        if(!registered)
        {
            internal::v1::RegisterRequest request;
            auto* process = request.mutable_process();
            process->set_process_id(options.process_id);
            process->set_instance(options.instance);
            process->set_endpoint(options.endpoint);
            process->set_role(internal::v1::PROCESS_ROLE_GRAPHER);
            internal::v1::RegisterResponse response;
            const auto bracket = audit.begin();
            const auto status = stub.Register(&context, request, &response);
            auditVisorReply(audit, bracket, status, response);
            registered = status.ok() && response.status().code() == 0;
            if(registered)
            {
                audit.retain({response.visor_replicas().begin(), response.visor_replicas().end()});
                LOG(INFO) << "grapher registered "
                          << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                                   options.process_start)
                                     .count()
                          << " ms after start";
                std::cout << "grapher registered instance=" << options.instance << std::endl;
            }
            if(!registered && !stop.stop_requested())
                LOG(WARNING) << "grapher registration failed";
        }
        else
        {
            internal::v1::HeartbeatRequest request;
            request.set_process_id(options.process_id);
            request.set_instance(options.instance);
            if(options.stories_without_physical_policy)
            {
                auto legacy = options.stories_without_physical_policy();
                if(legacy.ok() && !legacy->empty())
                {
                    policy_cursor %= legacy->size();
                    const auto count = std::min<size_t>(65536, legacy->size() - policy_cursor);
                    for(size_t n = 0; n < count; ++n)
                        request.add_stories_without_physical_policy((*legacy)[policy_cursor + n]);
                    policy_cursor += count;
                }
            }
            internal::v1::HeartbeatResponse response;
            const auto bracket = audit.begin();
            const auto status = stub.Heartbeat(&context, request, &response);
            auditVisorReply(audit, bracket, status, response);
            registered = status.ok() && response.status().code() == 0;
            if(!registered && !stop.stop_requested())
                LOG_EVERY_N_SEC(WARNING, 5) << "grapher heartbeat failed";
        }
        audit.report();
        std::unique_lock lock(mutex);
        pause.wait_for(lock, stop, options.heartbeat_interval, [] { return false; });
    }
}

} // namespace chronolog::grapher
