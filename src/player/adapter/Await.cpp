#include "player/adapter/ReplayService.h"

#include <atomic>
#include <condition_variable>
#include <thread>
#include "player/adapter/Convert.h"
#include "player/replay/HotReplay.h"
#include "player/replay/KeeperHotSource.h"

namespace chronolog::player
{
namespace
{
class AwaitReactor final
    : public grpc::ServerUnaryReactor
    , public ReplayService::Stream
{
public:
    AwaitReactor(ReplayService& service,
                 grpc::CallbackServerContext* context,
                 v1::AwaitRequest request,
                 v1::AwaitResponse* response,
                 std::function<absl::StatusOr<v1::AwaitResponse>(std::chrono::system_clock::time_point)> answer,
                 std::chrono::milliseconds cap)
        : service_(service)
        , context_(context)
        , request_(std::move(request))
        , response_(response)
        , answer_(std::move(answer))
        , cap_(cap)
    {}
    void begin()
    {
        auto status = service_.admit(this);
        if(!status.ok())
        {
            Finish(convert::toGrpc(status));
            return;
        }
        admitted_ = true;
        worker_ = std::thread([this] { run(); });
    }
    void drain() override
    {
        stopped_ = true;
        cv_.notify_all();
    }
    void OnCancel() override { drain(); }
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
        const auto wait = std::min(std::chrono::nanoseconds(request_.wait_bound_ns()),
                                   std::chrono::duration_cast<std::chrono::nanoseconds>(cap_));
        const auto end = std::min(context_->deadline(), std::chrono::system_clock::now() + wait);
        bool first = true;
        for(;;)
        {
            if(!first && std::chrono::system_clock::now() >= end)
            {
                Finish(grpc::Status::OK);
                return;
            }
            first = false;
            if(stopped_)
            {
                Finish(grpc::Status(grpc::StatusCode::CANCELLED, "Await cancelled"));
                return;
            }
            auto result = answer_(request_.wait_bound_ns() ? end : context_->deadline());
            if(!result.ok())
            {
                Finish(convert::toGrpc(result.status()));
                return;
            }
            *response_ = std::move(*result);
            if(response_->answer() != v1::AWAIT_ANSWER_ABSENT || std::chrono::system_clock::now() >= end)
            {
                Finish(grpc::Status::OK);
                return;
            }
            std::unique_lock lock(mu_);
            cv_.wait_until(lock,
                           std::min(end, std::chrono::system_clock::now() + std::chrono::milliseconds(20)),
                           [this] { return stopped_.load(); });
        }
    }
    ReplayService& service_;
    grpc::CallbackServerContext* context_;
    v1::AwaitRequest request_;
    v1::AwaitResponse* response_;
    std::function<absl::StatusOr<v1::AwaitResponse>(std::chrono::system_clock::time_point)> answer_;
    std::chrono::milliseconds cap_;
    std::thread worker_;
    std::atomic<bool> stopped_{};
    std::mutex mu_;
    std::condition_variable cv_;
    bool admitted_{};
};
} // namespace

absl::StatusOr<std::unique_ptr<ReplayStream>>
HotReplay::readAwait(StoryId story, Range range, std::chrono::system_clock::time_point deadline) const
{
    if(const auto* keeper = dynamic_cast<const KeeperHotSource*>(source_.get()))
        return HotReplay(keeper->bounded(deadline), options_).read(story, range);
    return read(story, range);
}

absl::StatusOr<v1::AwaitResponse> ReplayService::awaitAnswer(const v1::AwaitRequest& request,
                                                             std::chrono::system_clock::time_point deadline) const
{
    const auto& ref = request.ref();
    if(auto status = catalog_->ensureLive(ref.story_id()); !status.ok())
        return status;
    v1::AwaitResponse out;
    out.set_answer(v1::AWAIT_ANSWER_UNKNOWN);
    auto point = [&](Hlc h) -> absl::StatusOr<v1::AwaitResponse>
    {
        v1::AwaitResponse result;
        result.set_answer(v1::AWAIT_ANSWER_UNKNOWN);
        Hlc end = h;
        if(end.logical == UINT32_MAX)
        {
            if(end.physical_ns == INT64_MAX)
                return result;
            ++end.physical_ns;
            end.logical = 0;
        }
        else
            ++end.logical;
        const Range range{Range::Axis::Hlc, h, end};
        const auto* hot = dynamic_cast<const HotReplay*>(replay_.get());
        auto opened = hot ? hot->readAwait(ref.story_id(), range, deadline) : read(ref.story_id(), range, 0);
        if(!opened.ok())
        {
            if(absl::IsFailedPrecondition(opened.status()))
                return opened.status();
            return result;
        }
        for(;;)
        {
            auto batch = (*opened)->next();
            if(!batch.ok())
                return result;
            if(!*batch)
                return result;
            for(const auto& event: (**batch).events)
                if(event.id == EventId{ref.story_id(), ref.writer_id(), ref.incarnation(), ref.sequence()})
                {
                    result.set_answer(v1::AWAIT_ANSWER_VISIBLE);
                    *result.mutable_event() = convert::toProto(event);
                    *result.mutable_hlc() = convert::toProto(event.hlc);
                    return result;
                }
            if((**batch).completion)
            {
                const auto& c = *(**batch).completion;
                if(c.reason == IncompleteReason::SourceFailed)
                    return result;
                if(c.frontier <= h)
                {
                    result.set_answer(v1::AWAIT_ANSWER_ABSENT);
                    *result.mutable_frontier() = convert::toProto(c.frontier);
                }
                else
                    *result.mutable_frontier() = convert::toProto(c.frontier);
                return result;
            }
        }
    };
    if(request.has_hlc())
    {
        auto result = point(convert::fromProto(request.hlc()));
        if(!result.ok() || result->answer() != v1::AWAIT_ANSWER_UNKNOWN)
            return result;
        // A miss above the supplied HLC must consult the checkpoint. A failed source cannot certify it.
        // point's UNKNOWN distinguishes these below through its frontier marker.
        if(!result->has_frontier())
            return result;
    }
    if(!writer_status_)
        return out;
    auto status = writer_status_({ref.story_id(), ref.writer_id(), ref.incarnation(), ref.sequence()}, deadline);
    if(!status.ok() || !status->known())
        return out;
    if(ref.sequence() >= status->next_sequence())
    {
        if(status->released())
            out.set_answer(v1::AWAIT_ANSWER_WILL_NEVER_EXIST);
        else
        {
            out.set_answer(v1::AWAIT_ANSWER_ABSENT);
            *out.mutable_frontier() = status->sealed_frontier();
        }
    }
    else if(status->has_recorded_rejection() &&
            status->recorded_rejection().code() == static_cast<int>(grpc::StatusCode::OUT_OF_RANGE))
        out.set_answer(v1::AWAIT_ANSWER_SEQUENCE_CONSUMED);
    else if(status->has_recorded_hlc())
    {
        auto result = point(convert::fromProto(status->recorded_hlc()));
        if(result.ok() && result->answer() == v1::AWAIT_ANSWER_UNKNOWN)
            result->clear_frontier();
        return result;
    }
    return out;
}

grpc::ServerUnaryReactor*
ReplayService::Await(grpc::CallbackServerContext* context, const v1::AwaitRequest* request, v1::AwaitResponse* response)
{
    const auto& id = request->ref();
    if(request->wait_bound_ns() < 0 || !id.story_id() || !id.writer_id() || !id.incarnation() || !id.sequence())
    {
        auto* reactor = context->DefaultReactor();
        reactor->Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                     "complete EventId and nonnegative wait bound required"));
        return reactor;
    }
    auto* reactor = new AwaitReactor(
            *this,
            context,
            *request,
            response,
            [this, request = *request](auto deadline) { return awaitAnswer(request, deadline); },
            await_max_wait_);
    reactor->begin();
    return reactor;
}
} // namespace chronolog::player
