#include <napi.h>
#include "chronolog/client/client.h"
#include <atomic>
#include <cmath>
#include <cstring>
#include <climits>
#include <functional>
#include <limits>
#include <thread>

namespace
{
using namespace chronolog;
namespace sdk = chronolog::client;
using Js = Napi::Value;

const char* codeName(absl::StatusCode code)
{
    static const char* names[] = {"OK",
                                  "CANCELLED",
                                  "UNKNOWN",
                                  "INVALID_ARGUMENT",
                                  "DEADLINE_EXCEEDED",
                                  "NOT_FOUND",
                                  "ALREADY_EXISTS",
                                  "PERMISSION_DENIED",
                                  "RESOURCE_EXHAUSTED",
                                  "FAILED_PRECONDITION",
                                  "ABORTED",
                                  "OUT_OF_RANGE",
                                  "UNIMPLEMENTED",
                                  "INTERNAL",
                                  "UNAVAILABLE",
                                  "DATA_LOSS",
                                  "UNAUTHENTICATED"};
    auto index = static_cast<unsigned>(code);
    return index < std::size(names) ? names[index] : "UNKNOWN";
}
Js error(Napi::Env env, const absl::Status& status)
{
    auto value = Napi::Error::New(env, std::string(status.message())).Value();
    value.Set("code", codeName(status.code()));
    auto item = Napi::Object::New(env);
    item.Set("code", static_cast<int>(status.code()));
    item.Set("message", std::string(status.message()));
    value.Set("itemStatus", item);
    return value;
}
Napi::Object object(Js value)
{
    if(!value.IsObject() || value.IsNull())
        throw Napi::TypeError::New(value.Env(), "expected an object");
    return value.As<Napi::Object>();
}
std::string text(Js value)
{
    if(!value.IsString())
        throw Napi::TypeError::New(value.Env(), "expected a string");
    return value.As<Napi::String>().Utf8Value();
}
uint64_t unsigned64(Js value)
{
    if(!value.IsBigInt())
        throw Napi::TypeError::New(value.Env(), "uint64 values must be bigint");
    bool lossless;
    auto result = value.As<Napi::BigInt>().Uint64Value(&lossless);
    if(!lossless)
        throw Napi::RangeError::New(value.Env(), "bigint outside uint64 range");
    return result;
}
int64_t signed64(Js value)
{
    if(!value.IsBigInt())
        throw Napi::TypeError::New(value.Env(), "int64 values must be bigint");
    bool lossless;
    auto result = value.As<Napi::BigInt>().Int64Value(&lossless);
    if(!lossless)
        throw Napi::RangeError::New(value.Env(), "bigint outside int64 range");
    return result;
}
double number(Js value, double maximum = UINT32_MAX)
{
    if(!value.IsNumber())
        throw Napi::TypeError::New(value.Env(), "expected a number");
    auto result = value.As<Napi::Number>().DoubleValue();
    if(!std::isfinite(result) || result < 0 || std::floor(result) != result || result > maximum)
        throw Napi::RangeError::New(value.Env(), "number outside integer range");
    return result;
}
bool has(Napi::Object value, const char* key) { return value.Has(key) && !value.Get(key).IsUndefined(); }
int channelInteger(Js value)
{
    if(!value.IsNumber())
        throw Napi::TypeError::New(value.Env(), "channel argument must be a string or int32");
    auto result = value.As<Napi::Number>().DoubleValue();
    if(!std::isfinite(result) || std::floor(result) != result || result < INT_MIN || result > INT_MAX)
        throw Napi::RangeError::New(value.Env(), "channel argument outside int32 range");
    return static_cast<int>(result);
}
sdk::Deadline deadline(Napi::Object options)
{
    if(!has(options, "timeoutMs"))
        return {};
    return std::chrono::system_clock::now() +
           std::chrono::milliseconds(static_cast<int64_t>(number(options.Get("timeoutMs"))));
}
std::string bytes(Js value)
{
    if(!value.IsTypedArray() || value.As<Napi::TypedArray>().TypedArrayType() != napi_uint8_array)
        throw Napi::TypeError::New(value.Env(), "payload and trace ids must be Uint8Array");
    auto array = value.As<Napi::Uint8Array>();
    return std::string(reinterpret_cast<const char*>(array.Data()), array.ElementLength());
}
Hlc hlc(Js value)
{
    auto input = object(value);
    return {signed64(input.Get("physicalNs")), static_cast<uint32_t>(number(input.Get("logical")))};
}
EventId eventId(Js value)
{
    auto input = object(value);
    return {unsigned64(input.Get("storyId")),
            unsigned64(input.Get("writerId")),
            unsigned64(input.Get("incarnation")),
            unsigned64(input.Get("sequence"))};
}
sdk::AppendSpec spec(Js payload, Napi::Object options)
{
    sdk::AppendSpec result;
    result.envelope.payload = bytes(payload);
    if(has(options, "contentType"))
        result.envelope.content_type = text(options.Get("contentType"));
    if(has(options, "traceId"))
        result.envelope.trace_id = bytes(options.Get("traceId"));
    if(has(options, "spanId"))
        result.envelope.span_id = bytes(options.Get("spanId"));
    if(has(options, "attributes"))
    {
        auto attributes = object(options.Get("attributes"));
        auto keys = attributes.GetPropertyNames();
        for(uint32_t i = 0; i < keys.Length(); ++i)
        {
            auto key = text(keys.Get(i));
            result.envelope.attributes[key] = text(attributes.Get(key));
        }
    }
    if(has(options, "durability"))
    {
        auto value = number(options.Get("durability"), 2);
        if(value == 0)
            throw Napi::RangeError::New(options.Env(), "durability must be ACCEPTED or DURABLE");
        result.durability = static_cast<Durability>(static_cast<int>(value));
    }
    return result;
}
Js js(Napi::Env env, const Hlc& value)
{
    auto out = Napi::Object::New(env);
    out.Set("physicalNs", Napi::BigInt::New(env, value.physical_ns));
    out.Set("logical", value.logical);
    return out;
}
Js js(Napi::Env env, const EventId& value)
{
    auto out = Napi::Object::New(env);
    out.Set("storyId", Napi::BigInt::New(env, value.story_id));
    out.Set("writerId", Napi::BigInt::New(env, value.writer_id));
    out.Set("incarnation", Napi::BigInt::New(env, value.incarnation));
    out.Set("sequence", Napi::BigInt::New(env, value.sequence));
    return out;
}
Js js(Napi::Env env, const KeeperRef& value)
{
    auto out = Napi::Object::New(env);
    out.Set("processId", value.process_id);
    out.Set("endpoint", value.endpoint);
    return out;
}
Js js(Napi::Env env, const Chronicle& value);
Js js(Napi::Env env, const Story& value);
Js js(Napi::Env env, const Event& value);
template <class T>
Js array(Napi::Env env, const std::vector<T>& values)
{
    auto out = Napi::Array::New(env, values.size());
    for(size_t i = 0; i < values.size(); ++i) out.Set(i, js(env, values[i]));
    return out;
}
Js js(Napi::Env env, const Chronicle& value)
{
    auto out = Napi::Object::New(env);
    out.Set("name", value.name);
    out.Set("tombstoned", value.tombstoned);
    return out;
}
Js js(Napi::Env env, const Story& value)
{
    auto out = Napi::Object::New(env);
    out.Set("id", Napi::BigInt::New(env, value.id));
    out.Set("epoch", Napi::BigInt::New(env, value.epoch));
    out.Set("chronicle", value.chronicle);
    out.Set("name", value.name);
    out.Set("tombstoned", value.tombstoned);
    return out;
}
Js js(Napi::Env env, const Acquisition& value)
{
    auto out = Napi::Object::New(env);
    out.Set("storyId", Napi::BigInt::New(env, value.story_id));
    out.Set("writerId", Napi::BigInt::New(env, value.writer_id));
    out.Set("incarnation", Napi::BigInt::New(env, value.incarnation));
    out.Set("assignedKeeper", js(env, value.assigned_keeper));
    auto route = Napi::Object::New(env);
    route.Set("epoch", Napi::BigInt::New(env, value.route.epoch));
    route.Set("keepers", array(env, value.route.keepers));
    route.Set("grapher", value.route.grapher);
    route.Set("player", value.route.player);
    out.Set("route", route);
    return out;
}
Js js(Napi::Env env, const sdk::AppendResult& value)
{
    auto out = Napi::Object::New(env);
    out.Set("eventId", js(env, value.event_id));
    out.Set("hlc", js(env, value.hlc));
    out.Set("achieved", static_cast<int>(value.achieved));
    out.Set("acked", value.acked());
    return out;
}
Js js(Napi::Env env, const Event& value)
{
    auto out = Napi::Object::New(env);
    out.Set("id", js(env, value.id));
    out.Set("hlc", js(env, value.hlc));
    out.Set("durability", static_cast<int>(value.durability));
    auto physical = Napi::Object::New(env);
    physical.Set("physicalNs", Napi::BigInt::New(env, value.physical.physical_ns));
    physical.Set("status",
                 value.physical.status == ClockStatus::Synced     ? "SYNCED"
                 : value.physical.status == ClockStatus::Unsynced ? "UNSYNCED"
                                                                  : "UNAVAILABLE");
    if(value.physical.uncertainty_ns)
        physical.Set("uncertaintyNs", Napi::BigInt::New(env, *value.physical.uncertainty_ns));
    out.Set("physical", physical);
    auto envelope = Napi::Object::New(env);
    envelope.Set("contentType", value.envelope.content_type);
    auto data = [&](const std::string& input)
    {
        auto result = Napi::Uint8Array::New(env, input.size());
        if(!input.empty())
            std::memcpy(result.Data(), input.data(), input.size());
        return result;
    };
    envelope.Set("payload", data(value.envelope.payload));
    envelope.Set("traceId", data(value.envelope.trace_id));
    envelope.Set("spanId", data(value.envelope.span_id));
    auto attributes = Napi::Object::New(env);
    for(const auto& [key, item]: value.envelope.attributes)
        attributes.DefineProperty(Napi::PropertyDescriptor::Value(key, Napi::String::New(env, item), napi_enumerable));
    envelope.Set("attributes", attributes);
    out.Set("envelope", envelope);
    return out;
}
Js js(Napi::Env env, const Completion& value)
{
    auto out = Napi::Object::New(env);
    out.Set("complete", value.complete);
    out.Set("frontier", js(env, value.frontier));
    static const char* names[] = {"NONE", "LAGGING_WRITERS", "PHYSICAL_AXIS_UNBOUNDED", "SOURCE_FAILED", "TRUNCATED"};
    out.Set("reason", names[static_cast<int>(value.reason)]);
    auto laggards = Napi::Array::New(env, value.laggards.size());
    for(size_t i = 0; i < value.laggards.size(); ++i)
    {
        auto item = Napi::Object::New(env);
        item.Set("writerId", Napi::BigInt::New(env, value.laggards[i].writer_id));
        item.Set("incarnation", Napi::BigInt::New(env, value.laggards[i].incarnation));
        item.Set("frontier", js(env, value.laggards[i].frontier));
        laggards.Set(i, item);
    }
    out.Set("laggards", laggards);
    return out;
}
Js js(Napi::Env env, const sdk::StreamItem& value)
{
    auto out = Napi::Object::New(env);
    out.Set("events", array(env, value.events));
    if(value.completion)
        out.Set("completion", js(env, *value.completion));
    if(value.continuation)
        out.Set("continuation", js(env, *value.continuation));
    return out;
}
Js js(Napi::Env env, bool value) { return Napi::Boolean::New(env, value); }

struct Handle
{
    enum Kind
    {
        Client,
        Writer,
        Read,
        Tail
    } kind;
    std::shared_ptr<sdk::Client> client;
    std::shared_ptr<sdk::Writer> writer;
    std::shared_ptr<sdk::ReadStream> read;
    std::shared_ptr<sdk::TailStream> tail;
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
Held handle(Js value, Handle::Kind kind)
{
    if(!value.IsExternal())
        throw Napi::TypeError::New(value.Env(), "invalid native handle");
    auto result = *value.As<Napi::External<Held>>().Data();
    if(result->kind != kind)
        throw Napi::TypeError::New(value.Env(), "wrong native handle type");
    return result;
}
Js js(Napi::Env env, const Held& value)
{
    auto external = Napi::External<Held>::New(env, new Held(value), [](Napi::Env, Held* pointer) { delete pointer; });
    if(value->kind != Handle::Writer)
        return external;
    auto out = Napi::Object::New(env);
    out.Set("handle", external);
    out.Set("acquisition", js(env, value->acquisition));
    return out;
}
Js js(Napi::Env env, const sdk::BatchResult& values)
{
    auto out = Napi::Array::New(env, values.size());
    for(size_t i = 0; i < values.size(); ++i)
        out.Set(i, values[i].ok() ? js(env, *values[i]) : error(env, values[i].status()));
    return out;
}

template <class T>
Js js(Napi::Env env, const std::vector<T>& values)
{
    return array(env, values);
}

template <class T>
class Work final: public Napi::AsyncWorker
{
    Napi::Promise::Deferred promise_;
    std::function<absl::StatusOr<T>()> call_;
    absl::StatusOr<T> result_ = absl::UnknownError("not executed");

public:
    Work(Napi::Env env, std::function<absl::StatusOr<T>()> call)
        : Napi::AsyncWorker(env)
        , promise_(Napi::Promise::Deferred::New(env))
        , call_(std::move(call))
    {}
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
Js connect(const Napi::CallbackInfo& info)
{
    auto options = object(info[0]);
    sdk::ClientOptions config;
    config.catalog_endpoint = text(options.Get("catalog"));
    if(has(options, "player"))
        config.player_endpoint = text(options.Get("player"));
    if(has(options, "rpcTimeoutMs"))
        config.rpc_timeout = std::chrono::milliseconds(static_cast<int64_t>(number(options.Get("rpcTimeoutMs"))));
    if(has(options, "maxInFlight"))
        config.max_in_flight = static_cast<size_t>(number(options.Get("maxInFlight")));
    if(has(options, "batchSize"))
        config.batch_size = static_cast<size_t>(number(options.Get("batchSize")));
    if(has(options, "maxBatchItems"))
        config.max_batch_items = static_cast<size_t>(number(options.Get("maxBatchItems")));
    if(has(options, "maxBatchBytes"))
        config.max_batch_bytes = static_cast<size_t>(number(options.Get("maxBatchBytes")));
    if(has(options, "retry"))
    {
        auto retry = object(options.Get("retry"));
        if(has(retry, "maxRetries"))
            config.retry.max_retries = static_cast<size_t>(number(retry.Get("maxRetries")));
        if(has(retry, "backoffMs"))
            config.retry.backoff = std::chrono::milliseconds(static_cast<int64_t>(number(retry.Get("backoffMs"))));
    }
    if(has(options, "channelArgs"))
    {
        auto args = object(options.Get("channelArgs"));
        auto keys = args.GetPropertyNames();
        for(uint32_t i = 0; i < keys.Length(); ++i)
        {
            auto key = text(keys.Get(i));
            auto value = args.Get(key);
            if(value.IsString())
                config.channel_args[key] = text(value);
            else
                config.channel_args[key] = channelInteger(value);
        }
    }
    auto end = deadline(options);
    return work<Held>(info.Env(),
                      [config = std::move(config), end]() mutable -> absl::StatusOr<Held>
                      {
                          auto result = sdk::Client::Connect(std::move(config), end);
                          if(!result.ok())
                              return result.status();
                          auto out = std::make_shared<Handle>(Handle::Client);
                          out->client = std::make_shared<sdk::Client>(std::move(*result));
                          return out;
                      });
}
Js catalog(const Napi::CallbackInfo& info)
{
    auto held = handle(info[0], Handle::Client);
    auto method = text(info[1]);
    auto args = info[2].As<Napi::Array>();
    auto end = deadline(object(info[3]));
    auto env = info.Env();
    if(method == "createChronicle" || method == "getChronicle")
    {
        auto name = text(args.Get(uint32_t{0}));
        return work<Chronicle>(env,
                               [held, method, name, end]
                               {
                                   return method == "createChronicle" ? held->client->createChronicle(name, end)
                                                                      : held->client->getChronicle(name, end);
                               });
    }
    if(method == "listChronicles")
        return work<std::vector<Chronicle>>(env, [held, end] { return held->client->listChronicles(end); });
    if(method == "createStory")
    {
        auto chronicle = text(args.Get(uint32_t{0})), name = text(args.Get(uint32_t{1}));
        return work<Story>(env,
                           [held, chronicle, name, end] { return held->client->createStory(chronicle, name, end); });
    }
    if(method == "getStory")
    {
        auto id = unsigned64(args.Get(uint32_t{0}));
        return work<Story>(env, [held, id, end] { return held->client->getStory(id, end); });
    }
    if(method == "listStories")
    {
        auto name = text(args.Get(uint32_t{0}));
        return work<std::vector<Story>>(env, [held, name, end] { return held->client->listStories(name, end); });
    }
    if(method == "destroyChronicle")
    {
        auto name = text(args.Get(uint32_t{0}));
        return work<bool>(env,
                          [held, name, end]() -> absl::StatusOr<bool>
                          {
                              auto status = held->client->destroyChronicle(name, end);
                              if(!status.ok())
                                  return status;
                              return true;
                          });
    }
    if(method == "destroyStory")
    {
        auto id = unsigned64(args.Get(uint32_t{0}));
        return work<bool>(env,
                          [held, id, end]() -> absl::StatusOr<bool>
                          {
                              auto status = held->client->destroyStory(id, end);
                              if(!status.ok())
                                  return status;
                              return true;
                          });
    }
    throw Napi::TypeError::New(env, "unknown catalog operation");
}
Js acquire(const Napi::CallbackInfo& info)
{
    auto held = handle(info[0], Handle::Client);
    auto story = unsigned64(info[1]);
    auto identity = text(info[2]);
    auto end = deadline(object(info[3]));
    return work<Held>(info.Env(),
                      [held, story, identity, end]() -> absl::StatusOr<Held>
                      {
                          auto result = held->client->acquire(story, identity, end);
                          if(!result.ok())
                              return result.status();
                          auto out = std::make_shared<Handle>(Handle::Writer);
                          out->writer = std::make_shared<sdk::Writer>(std::move(*result));
                          out->acquisition = out->writer->acquisition();
                          return out;
                      });
}
Js append(const Napi::CallbackInfo& info)
{
    auto held = handle(info[0], Handle::Writer);
    auto options = object(info[2]);
    auto input = spec(info[1], options);
    auto end = deadline(options);
    return work<sdk::AppendResult>(info.Env(),
                                   [held, input = std::move(input), end] { return held->writer->append(input, end); });
}
Js appendBatch(const Napi::CallbackInfo& info)
{
    auto held = handle(info[0], Handle::Writer);
    if(!info[1].IsArray())
        throw Napi::TypeError::New(info.Env(), "batch must be an array");
    auto values = info[1].As<Napi::Array>();
    std::vector<sdk::AppendSpec> specs;
    specs.reserve(values.Length());
    for(uint32_t i = 0; i < values.Length(); ++i)
    {
        auto item = object(values.Get(i));
        specs.push_back(spec(item.Get("payload"), item));
    }
    auto end = deadline(object(info[2]));
    return work<sdk::BatchResult>(info.Env(),
                                  [held, specs = std::move(specs), end]
                                  { return held->writer->appendBatch(specs, end); });
}
Js release(const Napi::CallbackInfo& info)
{
    auto held = handle(info[0], Handle::Writer);
    auto end = deadline(object(info[1]));
    return work<bool>(info.Env(), [held, end] { return held->writer->release(end); });
}
Js stream(const Napi::CallbackInfo& info)
{
    auto held = handle(info[0], Handle::Client);
    auto story = unsigned64(info[1]);
    auto mode = text(info[2]);
    auto end = deadline(object(info[4]));
    if(mode == "physical")
    {
        auto range = object(info[3]);
        sdk::PhysicalRange input{signed64(range.Get("startNs")), signed64(range.Get("endNs"))};
        return work<Held>(info.Env(),
                          [held, story, input, end]() -> absl::StatusOr<Held>
                          {
                              auto result = held->client->readPhysical(story, input, end);
                              if(!result.ok())
                                  return result.status();
                              auto out = std::make_shared<Handle>(Handle::Read);
                              out->read = std::make_shared<sdk::ReadStream>(std::move(*result));
                              return out;
                          });
    }
    if(mode == "read")
    {
        auto range = object(info[3]);
        sdk::HlcRange input{hlc(range.Get("start")), hlc(range.Get("end"))};
        return work<Held>(info.Env(),
                          [held, story, input, end]() -> absl::StatusOr<Held>
                          {
                              auto result = held->client->read(story, input, end);
                              if(!result.ok())
                                  return result.status();
                              auto out = std::make_shared<Handle>(Handle::Read);
                              out->read = std::make_shared<sdk::ReadStream>(std::move(*result));
                              return out;
                          });
    }
    std::optional<sdk::Position> after;
    if(!info[3].IsNull() && !info[3].IsUndefined())
    {
        auto input = object(info[3]);
        after = sdk::Position{hlc(input.Get("hlc")), eventId(input.Get("id"))};
    }
    return work<Held>(info.Env(),
                      [held, story, after, end]() -> absl::StatusOr<Held>
                      {
                          auto result = held->client->tail(story, after, end);
                          if(!result.ok())
                              return result.status();
                          auto out = std::make_shared<Handle>(Handle::Tail);
                          out->tail = std::make_shared<sdk::TailStream>(std::move(*result));
                          return out;
                      });
}
Js next(const Napi::CallbackInfo& info)
{
    auto kind = text(info[1]) == "read" ? Handle::Read : Handle::Tail;
    auto held = handle(info[0], kind);
    auto end = deadline(object(info[2]));
    if(held->pulling.exchange(true))
        throw Napi::Error::New(info.Env(), "stream already has a pending next");
    auto promise = Napi::Promise::Deferred::New(info.Env());
    auto function = Napi::Function::New(info.Env(), [](const Napi::CallbackInfo&) {});
    auto tsfn = Napi::ThreadSafeFunction::New(info.Env(), function, "chronolog stream pull", 1, 1);
    try
    {
        std::thread(
                [held, end, promise, tsfn]() mutable
                {
                    using Result = absl::StatusOr<std::optional<sdk::StreamItem>>;
                    auto result = std::make_unique<Result>(absl::UnknownError("not executed"));
                    try
                    {
                        *result = held->read ? held->read->next(end) : held->tail->next(end);
                    }
                    catch(const std::exception& e)
                    {
                        *result = absl::InternalError(e.what());
                    }
                    held->pulling = false;
                    auto status = tsfn.BlockingCall(result.get(),
                                                    [promise](Napi::Env env, Napi::Function, Result* input)
                                                    {
                                                        std::unique_ptr<Result> result(input);
                                                        if(!env)
                                                            return;
                                                        if(!result->ok())
                                                            promise.Reject(error(env, result->status()));
                                                        else if(!**result)
                                                            promise.Resolve(env.Null());
                                                        else
                                                            promise.Resolve(js(env, ***result));
                                                    });
                    if(status == napi_ok)
                        result.release();
                    tsfn.Release();
                })
                .detach();
    }
    catch(...)
    {
        held->pulling = false;
        tsfn.Release();
        throw;
    }
    return promise.Promise();
}
Js cancel(const Napi::CallbackInfo& info)
{
    auto kind = text(info[1]) == "read" ? Handle::Read : Handle::Tail;
    handle(info[0], kind)->cancel();
    return info.Env().Undefined();
}
Napi::Object init(Napi::Env env, Napi::Object exports)
{
    exports.Set("connect", Napi::Function::New(env, connect));
    exports.Set("catalog", Napi::Function::New(env, catalog));
    exports.Set("acquire", Napi::Function::New(env, acquire));
    exports.Set("append", Napi::Function::New(env, append));
    exports.Set("appendBatch", Napi::Function::New(env, appendBatch));
    exports.Set("release", Napi::Function::New(env, release));
    exports.Set("stream", Napi::Function::New(env, stream));
    exports.Set("next", Napi::Function::New(env, next));
    exports.Set("cancel", Napi::Function::New(env, cancel));
    return exports;
}
} // namespace
NODE_API_MODULE(chronolog_node, init)
