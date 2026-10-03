#include "chrono-grapher/server/ClusterWorker.h"

#include <absl/log/log.h>
#include <algorithm>
#include <condition_variable>
#include <iostream>
#include <mutex>

#include "rpc/Channel.h"

namespace chronolog::grapher
{

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
            audit.finish(bracket, status, response);
            registered = status.ok() && response.status().code() == 0;
            if(registered)
            {
                audit.retain(response.visor_replicas());
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
            audit.finish(bracket, status, response);
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
