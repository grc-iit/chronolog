#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include "common/predicate/Predicate.h"
#include "player/adapter/PrefixReplay.h"
#include "player/adapter/StoryCatalog.h"
#include "chronolog/replay.h"
#include "chronolog/v1/chronolog.grpc.pb.h"

namespace chronolog::player
{

class ReplayService final: public v1::Replay::CallbackService
{
public:
    // Every stream runs on its own worker thread, so the cap bounds threads. Beyond it a call
    // fails with RESOURCE_EXHAUSTED.
    ReplayService(std::shared_ptr<const Replay> replay,
                  std::shared_ptr<const StoryCatalog> catalog,
                  size_t max_streams = 256,
                  PrefixOptions prefix = {});

    grpc::ServerWriteReactor<v1::ReadResponse>* Read(grpc::CallbackServerContext*, const v1::ReadRequest*) override;
    grpc::ServerWriteReactor<v1::TailResponse>* Tail(grpc::CallbackServerContext*, const v1::TailRequest*) override;

    absl::StatusOr<std::unique_ptr<ReplayStream>>
    read(StoryId story, Range range, size_t max_events, const EventPredicate& predicate = {}) const;
    absl::StatusOr<std::unique_ptr<ReplayStream>>
    tail(StoryId story, Event position, const EventPredicate& predicate = {}, bool progress = false) const;
    // I6.15: UNIMPLEMENTED unless the replay is a HotReplay and the Catalog resolves prefixes.
    absl::StatusOr<std::unique_ptr<ReplayStream>>
    readPrefix(const std::string& prefix, Range range, size_t max_events, const EventPredicate& predicate = {}) const;
    absl::StatusOr<std::unique_ptr<ReplayStream>>
    tailPrefix(const std::string& prefix, Event position, const EventPredicate& predicate = {}) const;

    // Drains: refuses new calls and cancels open streams, so tails finish with a
    // complete=false Completion.
    void shutdown();

    // Every stream the service has not finished.
    size_t activeStreams() const;

    class Stream
    {
    public:
        virtual ~Stream() = default;
        virtual void drain() = 0;
    };

    // Used by stream reactors. admit fails with UNAVAILABLE after shutdown and with
    // RESOURCE_EXHAUSTED at the stream cap.
    absl::Status admit(Stream* stream);
    void forget(Stream* stream);

private:
    template <class Resp>
    grpc::ServerWriteReactor<Resp>* open(std::function<absl::Status()> precheck,
                                         std::function<absl::StatusOr<std::unique_ptr<ReplayStream>>()> open);

    std::shared_ptr<const Replay> replay_;
    std::shared_ptr<const StoryCatalog> catalog_;
    const size_t max_streams_;
    const PrefixOptions prefix_;
    mutable std::mutex mu_;
    std::set<Stream*> streams_;
    bool closed_{};
};

} // namespace chronolog::player
