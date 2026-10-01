#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>

#include <grpcpp/grpcpp.h>

#include "chronolog/internal/v1/internal.grpc.pb.h"
#include "journal/RamJournal.h"
#include "membership/AcquisitionWatcher.h"
#include "membership/ConfigMembership.h"

namespace chronolog::keeper
{

// Registers this Keeper with the Visor and heartbeats it. A heartbeat carries the
// watcher's applied_revision, which is how the Visor learns a release fence took effect.
class ClusterClient
{
public:
    struct Options
    {
        std::string process_id;
        std::string instance;
        std::string endpoint;
        std::chrono::milliseconds interval{5000};
        std::string recovered_instance{};
    };

    ClusterClient(std::shared_ptr<grpc::Channel> channel,
                  Options options,
                  RamJournal& journal,
                  ConfigMembership& membership,
                  const AcquisitionWatcher& acquisitions);
    ~ClusterClient();

    absl::Status registerNow();
    absl::Status heartbeatNow();
    absl::Status extendNow();

    // Starts the loop: register with backoff, then heartbeat every interval or when kicked.
    void start();
    // Heartbeat now instead of waiting for the interval.
    void kick();

    bool registered() const { return registered_; }

private:
    void loop(std::stop_token stop);
    void applyRoutes(const google::protobuf::RepeatedPtrField<internal::v1::RouteUpdate>& routes);

    template <class Call>
    grpc::Status invoke(Call call)
    {
        std::lock_guard lock(rpc_mutex_);
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
        auto status = call(*stub_, context);
        if(status.error_code() != grpc::StatusCode::UNAVAILABLE)
            return status;
        for(auto& replica: replicas_)
        {
            grpc::ClientContext retry;
            retry.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
            status = call(*replica, retry);
            if(status.ok())
            {
                stub_.swap(replica);
                return status;
            }
            if(status.error_code() != grpc::StatusCode::UNAVAILABLE)
                return status;
        }
        return status;
    }
    std::mutex rpc_mutex_;
    std::vector<std::unique_ptr<internal::v1::Cluster::Stub>> replicas_;
    std::unique_ptr<internal::v1::Cluster::Stub> stub_;
    const Options options_;
    RamJournal& journal_;
    ConfigMembership& membership_;
    const AcquisitionWatcher& acquisitions_;
    std::atomic<bool> registered_{false};
    std::mutex mutex_;
    std::condition_variable_any cv_;
    bool kicked_{};
    std::jthread thread_;
};

} // namespace chronolog::keeper
