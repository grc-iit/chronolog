#include "adapter/JournalService.h"

#include "adapter/Convert.h"

namespace chronolog::keeper
{

namespace
{

// Runs on a worker thread.
template <class Req, class Resp>
grpc::Status process(RamJournal& journal, const Req& request, Resp& response)
{
    CHRONOLOG_ASSERT_WORKER_THREAD();
    auto parsed = convert::parse(request);
    if(!parsed.ok())
        return convert::toGrpc(parsed.status());
    auto results = journal.append(parsed->batch, parsed->durability);
    if(!results.ok())
        return convert::toGrpc(results.status());
    convert::fill(response, *parsed, *results);
    return grpc::Status::OK;
}

class AppendStreamReactor final: public grpc::ServerBidiReactor<v1::AppendStreamRequest, v1::AppendStreamResponse>
{
public:
    AppendStreamReactor(RamJournal& journal, WorkerPool& pool)
        : journal_(journal)
        , pool_(pool)
    {
        StartRead(&request_);
    }

    void OnReadDone(bool ok) override
    {
        if(!ok)
        {
            // Client half-close or cancellation. Accepted items stay in the journal.
            Finish(grpc::Status::OK);
            return;
        }
        if(!pool_.submit([this] { run(); }))
            Finish(grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED, "keeper is saturated"));
    }

    void OnWriteDone(bool ok) override
    {
        if(!ok)
        {
            Finish(grpc::Status::CANCELLED);
            return;
        }
        request_.Clear();
        StartRead(&request_);
    }

    void OnDone() override { delete this; }

private:
    void run()
    {
        response_.Clear();
        grpc::Status status = process(journal_, request_, response_);
        if(!status.ok())
        {
            Finish(status);
            return;
        }
        StartWrite(&response_);
    }

    RamJournal& journal_;
    WorkerPool& pool_;
    v1::AppendStreamRequest request_;
    v1::AppendStreamResponse response_;
};

} // namespace

JournalService::JournalService(RamJournal& journal, WorkerPool& pool)
    : journal_(journal)
    , pool_(pool)
{}

grpc::ServerUnaryReactor* JournalService::Append(grpc::CallbackServerContext* context,
                                                 const v1::AppendRequest* request,
                                                 v1::AppendResponse* response)
{
    grpc::ServerUnaryReactor* reactor = context->DefaultReactor();
    bool queued = pool_.submit([this, reactor, request, response]
                               { reactor->Finish(process(journal_, *request, *response)); });
    if(!queued)
        reactor->Finish(grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED, "keeper is saturated"));
    return reactor;
}

grpc::ServerBidiReactor<v1::AppendStreamRequest, v1::AppendStreamResponse>*
JournalService::AppendStream(grpc::CallbackServerContext*)
{
    return new AppendStreamReactor(journal_, pool_);
}

} // namespace chronolog::keeper
