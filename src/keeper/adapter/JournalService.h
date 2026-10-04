#pragma once

#include <grpcpp/grpcpp.h>

#include "chronolog/v1/chronolog.grpc.pb.h"
#include "keeper/journal/RamJournal.h"
#include "common/worker/WorkerPool.h"

namespace chronolog::keeper
{

// chronolog.v1.Journal. Append and AppendStream share per-item semantics: results keep
// request order, stale epochs are payload failures with gRPC OK, and every Journal call
// runs on the WorkerPool. A stream has one batch in flight at a time and cancelling it
// keeps every item already accepted (I5.8).
class JournalService final: public v1::Journal::CallbackService
{
public:
    JournalService(RamJournal& journal, WorkerPool& pool);

    grpc::ServerUnaryReactor* Append(grpc::CallbackServerContext* context,
                                     const v1::AppendRequest* request,
                                     v1::AppendResponse* response) override;
    grpc::ServerBidiReactor<v1::AppendStreamRequest, v1::AppendStreamResponse>*
    AppendStream(grpc::CallbackServerContext* context) override;

private:
    RamJournal& journal_;
    WorkerPool& pool_;
};

} // namespace chronolog::keeper
