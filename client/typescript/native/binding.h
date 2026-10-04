#pragma once
#include <napi.h>
#include "enum_names.h"
#include "chronolog/client/client.h"
#include "chronolog/context/context.h"
#include <atomic>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <mutex>
#include <thread>

namespace binding
{
using namespace chronolog;
namespace sdk = chronolog::client;
namespace ctx = chronolog::context;
using Js = Napi::Value;
constexpr double max_safe_integer = 9007199254740991.0;

Js status(Napi::Env, const absl::Status&);
Js error(Napi::Env, const absl::Status&);
Napi::Object object(Js);
std::string text(Js);
uint64_t unsigned64(Js);
int64_t signed64(Js);
double number(Js, double maximum = UINT32_MAX);
bool has(Napi::Object, const char* key);
sdk::Deadline deadline(Napi::Object options);
std::string bytes(Js);
Hlc hlc(Js);
EventId eventId(Js);
sdk::Position position(Js);
sdk::HlcRange hlcRange(Js);
Envelope envelope(Js payload, Napi::Object options);
TimeReading timeReading(Js);
sdk::ClientOptions clientOptions(Napi::Object);
Js data(Napi::Env, const std::string&);
Js js(Napi::Env, const Hlc&);
Js js(Napi::Env, const EventId&);
Js js(Napi::Env, const KeeperRef&);
Js js(Napi::Env, const Chronicle&);
Js js(Napi::Env, const Story&);
Js js(Napi::Env, const Acquisition&);
Js js(Napi::Env, const sdk::AppendResult&);
Js js(Napi::Env, const sdk::Position&);
Js js(Napi::Env, const sdk::HlcRange&);
Js js(Napi::Env, const Event&);
Js js(Napi::Env, const Completion&);
Js js(Napi::Env, const sdk::StreamItem&);
Js js(Napi::Env, const std::optional<sdk::StreamItem>&);
Js js(Napi::Env, const sdk::WriterLease&);
Js js(Napi::Env, const sdk::BatchResult&);
Js js(Napi::Env, const std::string&);
Js js(Napi::Env, bool);
Js js(Napi::Env, const ctx::ContextRef&);
Js js(Napi::Env, const ctx::MemoryResult&);
Js js(Napi::Env, const ctx::Page&);
Js js(Napi::Env, const ctx::LatestResult&);
Js js(Napi::Env, const ctx::ReconcileResult&);
Js js(Napi::Env, const ctx::CloseResult&);
Js js(Napi::Env, const ctx::FollowResult&);
template <class T>
Js array(Napi::Env env, const std::vector<T>& values)
{
    auto out = Napi::Array::New(env, values.size());
    for(size_t i = 0; i < values.size(); ++i) out.Set(i, js(env, values[i]));
    return out;
}
template <class T>
Js js(Napi::Env env, const std::vector<T>& values)
{
    return array(env, values);
}
template <class T>
void set(Napi::Object out, const char* key, const std::optional<T>& value)
{
    if(value)
        out.Set(key, js(out.Env(), *value));
}

struct Handle
{
    enum Kind
    {
        Client,
        Writer,
        Read,
        Tail,
        ContextClient,
        Session,
        Lanes
    } kind;
    std::shared_ptr<sdk::Client> client;
    std::shared_ptr<sdk::Writer> writer;
    std::shared_ptr<sdk::LaneWriter> lanes;
    std::shared_ptr<sdk::ReadStream> read;
    std::shared_ptr<sdk::TailStream> tail;
    std::shared_ptr<ctx::ContextClient> context;
    std::shared_ptr<ctx::ContextSession> session;
    Acquisition acquisition;
    std::atomic<bool> pulling{false};
    explicit Handle(Kind type)
        : kind(type)
    {}
    void cancel()
    {
        if(read)
            read->cancel();
        if(tail)
            tail->cancel();
    }
    ~Handle() { cancel(); }
};
using Held = std::shared_ptr<Handle>;
Held handle(Js, Handle::Kind);
Js js(Napi::Env, const Held&);

// Owns native teardown for one environment. Finalizers hand handles to a reaper thread instead of destroying them on
// the JS thread, so no finalizer joins SDK scheduler work. Teardown cancels live streams, waits for native threads and
// joins the reaper, which needs no event loop.
class Runtime
{
public:
    Runtime();
    ~Runtime();
    void bury(Held);
    void track(const Held&);
    void enter();
    void leave();
    void shutdown();

private:
    void reap();
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<Held> graveyard_;
    std::vector<std::weak_ptr<Handle>> streams_;
    size_t inflight_{0};
    bool closed_{false};
    std::thread reaper_;
};
Runtime& runtime(Napi::Env);

template <class T>
class Work final: public Napi::AsyncWorker
{
    Napi::Promise::Deferred promise_;
    std::function<absl::StatusOr<T>()> call_;
    absl::StatusOr<T> result_ = absl::UnknownError("not executed");
    Runtime& runtime_;

public:
    Work(Napi::Env env, std::function<absl::StatusOr<T>()> call)
        : Napi::AsyncWorker(env)
        , promise_(Napi::Promise::Deferred::New(env))
        , call_(std::move(call))
        , runtime_(runtime(env))
    {
        runtime_.enter();
    }
    void Execute() override
    {
        try
        {
            result_ = call_();
        }
        catch(const std::exception& e)
        {
            result_ = absl::InternalError(e.what());
        }
        // Captured handles drop here, off the JS thread.
        call_ = nullptr;
        runtime_.leave();
    }
    void OnOK() override
    {
        if(result_.ok())
            promise_.Resolve(js(Env(), *result_));
        else
            promise_.Reject(error(Env(), result_.status()));
    }
    Napi::Promise promise() const { return promise_.Promise(); }
};
template <class T, class F>
Js work(Napi::Env env, F call)
{
    auto* worker = new Work<T>(env, std::move(call));
    auto promise = worker->promise();
    worker->Queue();
    return promise;
}
// Runs a call that may wait long on its own thread so it never occupies the libuv pool.
template <class T>
Js thread(Napi::Env env, std::function<absl::StatusOr<T>()> call)
{
    auto& owner = runtime(env);
    auto promise = Napi::Promise::Deferred::New(env);
    auto function = Napi::Function::New(env, [](const Napi::CallbackInfo&) {});
    auto tsfn = Napi::ThreadSafeFunction::New(env, function, "chronolog native call", 1, 1);
    owner.enter();
    try
    {
        std::thread(
                [&owner, call = std::move(call), promise, tsfn]() mutable
                {
                    using Result = absl::StatusOr<T>;
                    auto result = std::make_unique<Result>(absl::UnknownError("not executed"));
                    try
                    {
                        *result = call();
                    }
                    catch(const std::exception& e)
                    {
                        *result = absl::InternalError(e.what());
                    }
                    call = nullptr;
                    auto status = tsfn.BlockingCall(result.get(),
                                                    [promise](Napi::Env env, Napi::Function, Result* input)
                                                    {
                                                        std::unique_ptr<Result> value(input);
                                                        if(!env)
                                                            return;
                                                        if(!value->ok())
                                                            promise.Reject(error(env, value->status()));
                                                        else
                                                            promise.Resolve(js(env, **value));
                                                    });
                    if(status == napi_ok)
                        (void)result.release();
                    tsfn.Release();
                    result.reset();
                    owner.leave();
                })
                .detach();
    }
    catch(...)
    {
        owner.leave();
        tsfn.Release();
        throw;
    }
    return promise.Promise();
}
void initContext(Napi::Env, Napi::Object exports);
} // namespace binding
