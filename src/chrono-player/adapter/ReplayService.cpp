#include "chrono-player/adapter/ReplayService.h"
#include <condition_variable>
#include <thread>
#include "chrono-player/adapter/Convert.h"

namespace chronolog::player
{
namespace
{

// W10.3: an unknown story is FAILED_PRECONDITION on the wire.
grpc::Status wireStatus(const absl::Status& status)
{
    if(status.code() == absl::StatusCode::kNotFound)
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, std::string(status.message()));
    return convert::toGrpc(status);
}

template <class Resp>
class FailedReactor final: public grpc::ServerWriteReactor<Resp>
{
public:
    explicit FailedReactor(grpc::Status status) { this->Finish(std::move(status)); }
    void OnDone() override { delete this; }
};

void fill(v1::ReadResponse& response, const ReplayBatch& batch, bool completion)
{
    if(completion)
        *response.mutable_completion() = convert::toProto(*batch.completion);
    else
        for(const auto& event: batch.events) *response.mutable_batch()->add_events() = convert::toProto(event);
}

void fill(v1::TailResponse& response, const ReplayBatch& batch, bool completion)
{
    if(completion)
        *response.mutable_completion() = convert::toProto(*batch.completion);
    else
        for(const auto& event: batch.events) *response.mutable_batch()->add_events() = convert::toProto(event);
}

// One RPC. A worker thread owns the blocking work: the Catalog check, the fan-out to Keepers
// that opens the stream, and the pump from ReplayStream::next() to StartWrite.
template <class Resp>
class StreamReactor final
    : public grpc::ServerWriteReactor<Resp>
    , public ReplayService::Stream
{
public:
    using Open = std::function<absl::StatusOr<std::unique_ptr<ReplayStream>>()>;

    StreamReactor(ReplayService& service, std::function<absl::Status()> precheck, Open open)
        : service_(service)
        , precheck_(std::move(precheck))
        , open_(std::move(open))
    {}

    void begin()
    {
        if(auto admitted = service_.admit(this); !admitted.ok())
        {
            admitted_ = false;
            this->Finish(wireStatus(admitted));
            return;
        }
        worker_ = std::thread([this] { run(); });
    }

    void drain() override
    {
        std::lock_guard lk(mu_);
        draining_ = true;
        if(stream_)
            stream_->cancel();
    }

    void OnWriteDone(bool ok) override
    {
        {
            std::lock_guard lk(mu_);
            writing_ = false;
            write_failed_ |= !ok;
        }
        cv_.notify_all();
    }

    void OnCancel() override
    {
        {
            std::lock_guard lk(mu_);
            cancelled_ = true;
            if(stream_)
                stream_->cancel();
        }
        cv_.notify_all();
    }

    void OnDone() override
    {
        if(worker_.joinable())
            worker_.join();
        if(admitted_)
            service_.forget(this);
        delete this;
    }

private:
    void run()
    {
        if(auto status = precheck_(); !status.ok())
            return finish(wireStatus(status));
        auto opened = open_();
        if(!opened.ok())
            return finish(wireStatus(opened.status()));
        ReplayStream* stream;
        {
            std::lock_guard lk(mu_);
            stream_ = std::move(*opened);
            stream = stream_.get();
            if(cancelled_ || draining_)
                stream->cancel();
        }
        for(;;)
        {
            auto next = stream->next();
            if(!next.ok())
                return finish(wireStatus(next.status()));
            if(!*next)
                return finish(grpc::Status::OK);
            if(!send(**next))
                return finish(grpc::Status(grpc::StatusCode::CANCELLED, "client went away"));
        }
    }

    bool send(const ReplayBatch& batch)
    {
        if(!batch.events.empty())
        {
            Resp response;
            fill(response, batch, false);
            if(!write(std::move(response)))
                return false;
        }
        if(batch.completion)
        {
            Resp response;
            fill(response, batch, true);
            return write(std::move(response));
        }
        return true;
    }

    bool write(Resp response)
    {
        std::unique_lock lk(mu_);
        if(cancelled_)
            return false;
        response_ = std::move(response);
        writing_ = true;
        this->StartWrite(&response_);
        cv_.wait(lk, [&] { return !writing_; });
        return !write_failed_ && !cancelled_;
    }

    void finish(grpc::Status status)
    {
        // Finish must run unlocked: OnDone joins this worker and may follow at once.
        this->Finish(std::move(status));
    }

    ReplayService& service_;
    std::function<absl::Status()> precheck_;
    Open open_;
    std::thread worker_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::unique_ptr<ReplayStream> stream_;
    Resp response_;
    bool admitted_{true};
    bool writing_{};
    bool write_failed_{};
    bool cancelled_{};
    bool draining_{};
};

} // namespace

ReplayService::ReplayService(std::shared_ptr<const Replay> replay,
                             std::shared_ptr<const StoryCatalog> catalog,
                             size_t max_streams)
    : replay_(std::move(replay))
    , catalog_(std::move(catalog))
    , max_streams_(max_streams)
{}

absl::Status ReplayService::admit(Stream* stream)
{
    std::lock_guard lk(mu_);
    if(closed_)
        return absl::UnavailableError("player is shutting down");
    if(streams_.size() >= max_streams_)
        return absl::ResourceExhaustedError("too many open replay streams");
    streams_.insert(stream);
    return absl::OkStatus();
}

void ReplayService::forget(Stream* stream)
{
    std::lock_guard lk(mu_);
    streams_.erase(stream);
}

size_t ReplayService::activeStreams() const
{
    std::lock_guard lk(mu_);
    return streams_.size();
}

void ReplayService::shutdown()
{
    std::lock_guard lk(mu_);
    closed_ = true;
    for(Stream* stream: streams_) stream->drain();
}

template <class Resp>
grpc::ServerWriteReactor<Resp>* ReplayService::open(StoryId story,
                                                    std::function<absl::StatusOr<std::unique_ptr<ReplayStream>>()> open)
{
    auto* reactor = new StreamReactor<Resp>(
            *this,
            [catalog = catalog_, story] { return catalog->ensureLive(story); },
            std::move(open));
    reactor->begin();
    return reactor;
}

grpc::ServerWriteReactor<v1::ReadResponse>* ReplayService::Read(grpc::CallbackServerContext*,
                                                                const v1::ReadRequest* request)
{
    auto range = convert::rangeFromProto(*request);
    if(!range.ok())
        return new FailedReactor<v1::ReadResponse>(convert::toGrpc(range.status()));
    const StoryId story = request->story_id();
    return open<v1::ReadResponse>(story,
                                  [replay = replay_, story, range = *range] { return replay->read(story, range); });
}

grpc::ServerWriteReactor<v1::TailResponse>* ReplayService::Tail(grpc::CallbackServerContext*,
                                                                const v1::TailRequest* request)
{
    const StoryId story = request->story_id();
    auto position = convert::positionFromProto(story, *request);
    if(!position.ok())
        return new FailedReactor<v1::TailResponse>(convert::toGrpc(position.status()));
    return open<v1::TailResponse>(story,
                                  [replay = replay_, story, position = *position]
                                  { return replay->tail(story, position); });
}

} // namespace chronolog::player
