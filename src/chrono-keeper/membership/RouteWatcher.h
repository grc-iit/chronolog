#pragma once

#include <memory>
#include <map>
#include <string>

#include <grpcpp/grpcpp.h>

#include "chronolog/internal/v1/internal.grpc.pb.h"
#include "membership/ConfigMembership.h"
#include "membership/Watcher.h"

namespace chronolog::keeper
{

// Holds Cluster.WatchRoutes open and replaces each story's route in the membership as the
// Visor sends it. The Visor sends one full snapshot on every connect.
class RouteWatcher
{
public:
    RouteWatcher(ConfigMembership& membership,
                 std::shared_ptr<grpc::Channel> channel,
                 std::string process_id,
                 std::string instance);

private:
    bool session(std::stop_token stop);

    std::map<StoryId, std::pair<uint64_t, Epoch>> applied_;
    ConfigMembership& membership_;
    std::unique_ptr<internal::v1::Cluster::Stub> stub_;
    const std::string process_id_;
    const std::string instance_;
    std::unique_ptr<Watcher> watcher_;
};

} // namespace chronolog::keeper
