#pragma once

#include <grpcpp/grpcpp.h>

#include "chronolog/internal/v1/internal.grpc.pb.h"
#include "chronolog/membership.h"
#include "chronolog/message_limits.h"
#include "keeper/journal/RamJournal.h"
#include "common/worker/WorkerPool.h"

namespace chronolog::keeper
{

// chronolog.internal.v1.Archive. Only FetchHot is served; TransferChunk and
// WatchWatermarks stay UNIMPLEMENTED until the Grapher port. FetchHot ticks the sealed
// frontier BEFORE it scans, so every event below the trailer's frontier is in the
// stream (I6.11a).
class ArchiveService final: public internal::v1::Archive::CallbackService
{
public:
    struct Options
    {
        // Applied when the request leaves max_events at zero.
        uint64_t default_max_events{100000};
        // One response message holds at most this many events or encoded event bytes.
        size_t batch_events{512};
        size_t batch_bytes{kEventBatchBytes};
        // Total encoded event bytes in one FetchHot reply; the stream is truncated beyond it.
        size_t max_bytes{64u << 20};
    };

    ArchiveService(RamJournal& journal, const Membership& membership, WorkerPool& pool, Options options);
    ArchiveService(RamJournal& journal, const Membership& membership, WorkerPool& pool)
        : ArchiveService(journal, membership, pool, Options{})
    {}

    grpc::ServerWriteReactor<internal::v1::FetchHotResponse>*
    FetchHot(grpc::CallbackServerContext* context, const internal::v1::FetchHotRequest* request) override;

    grpc::ServerUnaryReactor* WriterStatus(grpc::CallbackServerContext*,
                                           const internal::v1::WriterStatusRequest*,
                                           internal::v1::WriterStatusResponse*) override;

private:
    RamJournal& journal_;
    const Membership& membership_;
    WorkerPool& pool_;
    const Options options_;
};

} // namespace chronolog::keeper
