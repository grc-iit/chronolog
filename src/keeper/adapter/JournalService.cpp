#include "adapter/JournalService.h"

#include "adapter/Convert.h"

namespace chronolog::keeper
{

namespace
{

// Runs on a worker thread.
template <class Req, class Resp>
void process(RamJournal& journal, const Req& request, Resp& response, std::function<void(grpc::Status)> done)
{
    CHRONOLOG_ASSERT_WORKER_THREAD();
    auto parsed = convert::parse(request);
    if(!parsed.ok())
        return done(convert::toGrpc(parsed.status()));
    auto input = std::make_shared<convert::ParsedAppend>(std::move(*parsed));
    journal.appendAsync(input->batch,
                        input->durability,
                        [input, &response, done = std::move(done)](auto results)
                        {
                            if(!results.ok())
                                return done(convert::toGrpc(results.status()));
                            convert::fill(response, *input, *results);
                            done(grpc::Status::OK);
                        });
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
        process(journal_,
                request_,
                response_,
                [this](grpc::Status status)
                {
                    if(!status.ok())
                        Finish(status);
                    else
                        StartWrite(&response_);
                });
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
    bool queued = pool_.submit(
            [this, reactor, request, response]
            { process(journal_, *request, *response, [reactor](grpc::Status status) { reactor->Finish(status); }); });
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
