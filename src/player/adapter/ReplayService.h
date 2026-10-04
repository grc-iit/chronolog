#pragma once

#include <chrono>
#include <functional>
#include "chronolog/internal/v1/internal.pb.h"
#include <memory>
#include <mutex>
#include <set>
#include "player/adapter/StoryCatalog.h"
#include "chronolog/replay.h"
#include "chronolog/v1/chronolog.grpc.pb.h"

namespace chronolog::player
{

class ReplayService final: public v1::Replay::CallbackService
{
public:
    using WriterStatusCall =
            std::function<absl::StatusOr<internal::v1::WriterStatusResponse>(EventId,
                                                                             std::chrono::system_clock::time_point)>;

    // Every stream runs on its own worker thread, so the cap bounds threads. Beyond it a call
    // fails with RESOURCE_EXHAUSTED.
    ReplayService(std::shared_ptr<const Replay> replay,
                  std::shared_ptr<const StoryCatalog> catalog,
                  size_t max_streams = 256,
                  WriterStatusCall writer_status = {},
                  std::chrono::milliseconds await_max_wait = std::chrono::milliseconds(300000));

    grpc::ServerWriteReactor<v1::ReadResponse>* Read(grpc::CallbackServerContext*, const v1::ReadRequest*) override;
    grpc::ServerWriteReactor<v1::TailResponse>* Tail(grpc::CallbackServerContext*, const v1::TailRequest*) override;

    grpc::ServerUnaryReactor* Await(grpc::CallbackServerContext*, const v1::AwaitRequest*, v1::AwaitResponse*) override;
    absl::StatusOr<v1::AwaitResponse> awaitAnswer(const v1::AwaitRequest&,
                                                  std::chrono::system_clock::time_point deadline) const;

    absl::StatusOr<std::unique_ptr<ReplayStream>> read(StoryId story, Range range, size_t max_events) const;

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
    grpc::ServerWriteReactor<Resp>* open(StoryId story,
                                         std::function<absl::StatusOr<std::unique_ptr<ReplayStream>>()> open);

    std::shared_ptr<const Replay> replay_;
    std::shared_ptr<const StoryCatalog> catalog_;
    const size_t max_streams_;
    WriterStatusCall writer_status_;
    std::chrono::milliseconds await_max_wait_;
    mutable std::mutex mu_;
    std::set<Stream*> streams_;
    bool closed_{};
};

} // namespace chronolog::player
