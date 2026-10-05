#include "player/adapter/ReplayService.h"
#include <condition_variable>
#include <thread>
#include <type_traits>
#include "player/adapter/Convert.h"
#include "player/replay/HotReplay.h"

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

// Fills a response that may hold the previous batch: its event messages and their buffers are reused, and the
// events are moved in.
template <class Resp>
void fillEvents(Resp& response, std::vector<Event>& events)
{
    auto* out = response.mutable_batch()->mutable_events();
    out->Clear();
    out->Reserve(static_cast<int>(events.size()));
    for(auto& event: events) convert::toProto(std::move(event), *out->Add());
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

    // response_ is idle here: write() returns only after OnWriteDone or without starting a write.
    bool send(ReplayBatch& batch)
    {
        if(!batch.events.empty())
        {
            fillEvents(response_, batch.events);
            if(!write())
                return false;
        }
        if constexpr(std::is_same_v<Resp, v1::TailResponse>)
        {
            auto* tail = dynamic_cast<ProgressReplayStream*>(stream_.get());
            if(tail)
                if(auto progress = tail->progress())
                {
                    response_.Clear();
                    *response_.mutable_progress()->mutable_position()->mutable_hlc() = convert::toProto(*progress);
                    if(!write())
                        return false;
                }
        }
        if(batch.completion)
        {
            *response_.mutable_completion() = convert::toProto(*batch.completion);
            return write();
        }
        return true;
    }

    bool write()
    {
        std::unique_lock lk(mu_);
        if(cancelled_)
            return false;
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
                             size_t max_streams,
                             WriterStatusCall writer_status,
                             std::chrono::milliseconds await_max_wait)
    : replay_(std::move(replay))
    , catalog_(std::move(catalog))
    , max_streams_(max_streams)
    , writer_status_(std::move(writer_status))
    , await_max_wait_(await_max_wait)
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

absl::StatusOr<std::unique_ptr<ReplayStream>>
ReplayService::read(StoryId story, Range range, size_t max_events, const EventPredicate& predicate) const
{
    if(const auto* bounded = dynamic_cast<const HotReplay*>(replay_.get()))
        return bounded->read(story, range, max_events, predicate);
    if(max_events)
        return absl::UnimplementedError("replay does not support per-request event targets");
    if(!predicate.empty())
        return absl::UnimplementedError("replay does not support predicates");
    return replay_->read(story, range);
}

absl::StatusOr<std::unique_ptr<ReplayStream>>
ReplayService::tail(StoryId story, Event position, const EventPredicate& predicate, bool progress) const
{
    if(const auto* bounded = dynamic_cast<const HotReplay*>(replay_.get()))
        return bounded->tail(story, std::move(position), predicate, progress);
    if(!predicate.empty())
        return absl::UnimplementedError("replay does not support predicates");
    return replay_->tail(story, std::move(position));
}

grpc::ServerWriteReactor<v1::ReadResponse>* ReplayService::Read(grpc::CallbackServerContext*,
                                                                const v1::ReadRequest* request)
{
    auto range = convert::rangeFromProto(*request);
    if(!range.ok())
        return new FailedReactor<v1::ReadResponse>(convert::toGrpc(range.status()));
    auto predicate = convert::fromProto(request->predicate());
    if(auto valid = predicate.validate(); !valid.ok())
        return new FailedReactor<v1::ReadResponse>(convert::toGrpc(valid));
    const StoryId story = request->story_id();
    const size_t max_events = request->max_events();
    return open<v1::ReadResponse>(story,
                                  [this, story, range = *range, max_events, predicate = std::move(predicate)]
                                  { return read(story, range, max_events, predicate); });
}

grpc::ServerWriteReactor<v1::TailResponse>* ReplayService::Tail(grpc::CallbackServerContext*,
                                                                const v1::TailRequest* request)
{
    const StoryId story = request->story_id();
    auto position = convert::positionFromProto(story, *request);
    if(!position.ok())
        return new FailedReactor<v1::TailResponse>(convert::toGrpc(position.status()));
    auto predicate = convert::fromProto(request->predicate());
    if(auto valid = predicate.validate(); !valid.ok())
        return new FailedReactor<v1::TailResponse>(convert::toGrpc(valid));
    return open<v1::TailResponse>(
            story,
            [this, story, position = *position, predicate = std::move(predicate), progress = request->progress()]
            { return tail(story, position, predicate, progress); });
}

} // namespace chronolog::player
