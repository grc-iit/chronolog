#pragma once

#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include "player/replay/RouteSource.h"
#include "chronolog/internal/v1/internal.grpc.pb.h"

namespace chronolog::player
{

// Keeps Cluster.WatchAcquisitions open per Keeper and maps registered writers to their
// assigned Keeper. It only names laggards; completeness never reads it (I6.11c).
class WriterDirectory final: public WriterSource
{
public:
    explicit WriterDirectory(std::shared_ptr<grpc::Channel> visor_internal);
    ~WriterDirectory() override;

    // Starts a subscription for this Keeper process id. Idempotent.
    void watch(const std::string& keeper_id);
    void stop();

    std::vector<WriterAssignment> writers(StoryId story) const override;

private:
    using Key = std::tuple<StoryId, uint64_t, uint64_t>;
    void run(std::string keeper_id);
    void apply(const std::string& keeper_id, const internal::v1::AcquisitionUpdate& update);

    std::unique_ptr<internal::v1::Cluster::Stub> stub_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    bool stopped_{};
    std::set<std::string> watched_;
    std::map<std::string, std::map<Key, WriterAssignment>> by_keeper_;
    std::map<std::string, grpc::ClientContext*> active_;
    std::vector<std::thread> threads_;
};

} // namespace chronolog::player
