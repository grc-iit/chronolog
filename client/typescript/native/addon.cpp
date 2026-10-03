#include "binding.h"
#include <set>

namespace binding
{
Js refusal(Napi::Env env, const AcquireRefusal& value)
{
    static const std::initializer_list<const char*> reasons = {"UNSPECIFIED", "HELD", "PRIOR_MISMATCH"};
    auto out = Napi::Object::New(env);
    out.Set("refusalReason", enumName(value.refusal_reason, reasons));
    if(value.current_incarnation)
        out.Set("currentIncarnation", Napi::BigInt::New(env, *value.current_incarnation));
    if(value.matched_incarnation)
        out.Set("matchedIncarnation", Napi::BigInt::New(env, *value.matched_incarnation));
    out.Set("remainingNs", Napi::BigInt::New(env, value.remaining_ns));
    if(value.termination_cause)
        out.Set("terminationCause", causeName(*value.termination_cause));
    return out;
}
void describe(Napi::Object out, const absl::Status& value)
{
    out.Set("code", codeName(value.code()));
    out.Set("message", std::string(value.message()));
    out.Set("rejection", rejectionName(sdk::rejectionOf(value)));
    if(auto detail = sdk::acquireRefusalOf(value))
        out.Set("acquireRefusal", refusal(out.Env(), *detail));
}
Js status(Napi::Env env, const absl::Status& value)
{
    auto out = Napi::Object::New(env);
    describe(out, value);
    return out;
}
Js error(Napi::Env env, const absl::Status& value)
{
    auto out = Napi::Error::New(env, std::string(value.message())).Value();
    describe(out, value);
    auto item = Napi::Object::New(env);
    item.Set("code", static_cast<int>(value.code()));
    item.Set("message", std::string(value.message()));
    out.Set("itemStatus", item);
    return out;
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
double number(Js value, double maximum)
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
sdk::Position position(Js value)
{
    auto input = object(value);
    return {hlc(input.Get("hlc")), eventId(input.Get("id"))};
}
sdk::HlcRange hlcRange(Js value)
{
    auto input = object(value);
    return {hlc(input.Get("start")), hlc(input.Get("end"))};
}
Envelope envelope(Js payload, Napi::Object options)
{
    Envelope result;
    result.payload = bytes(payload);
    if(has(options, "contentType"))
        result.content_type = text(options.Get("contentType"));
    if(has(options, "traceId"))
        result.trace_id = bytes(options.Get("traceId"));
    if(has(options, "spanId"))
        result.span_id = bytes(options.Get("spanId"));
    if(has(options, "attributes"))
    {
        auto attributes = object(options.Get("attributes"));
        auto keys = attributes.GetPropertyNames();
        for(uint32_t i = 0; i < keys.Length(); ++i)
        {
            auto key = text(keys.Get(i));
            result.attributes[key] = text(attributes.Get(key));
        }
    }
    return result;
}
TimeReading timeReading(Js value)
{
    auto input = object(value);
    TimeReading result;
    result.physical_ns = signed64(input.Get("physicalNs"));
    if(has(input, "uncertaintyNs"))
        result.uncertainty_ns = signed64(input.Get("uncertaintyNs"));
    auto clock = text(input.Get("status"));
    if(clock == "SYNCED")
        result.status = ClockStatus::Synced;
    else if(clock == "UNSYNCED")
        result.status = ClockStatus::Unsynced;
    else if(clock == "UNAVAILABLE")
        result.status = ClockStatus::Unavailable;
    else
        throw Napi::RangeError::New(value.Env(), "unknown clock status");
    return result;
}
Durability durability(Js value)
{
    auto level = number(value, 2);
    if(level == 0)
        throw Napi::RangeError::New(value.Env(), "durability must be ACCEPTED or DURABLE");
    return static_cast<Durability>(static_cast<int>(level));
}
sdk::AppendSpec spec(Js payload, Napi::Object options)
{
    sdk::AppendSpec result;
    result.envelope = envelope(payload, options);
    if(has(options, "durability"))
        result.durability = durability(options.Get("durability"));
    return result;
}
sdk::ClientOptions clientOptions(Napi::Object options)
{
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
    return config;
}
Js data(Napi::Env env, const std::string& input)
{
    auto result = Napi::Uint8Array::New(env, input.size());
    if(!input.empty())
        std::memcpy(result.Data(), input.data(), input.size());
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
    auto lease = Napi::Object::New(env);
    lease.Set("durationNs", Napi::BigInt::New(env, value.lease.duration_ns));
    lease.Set("remainingNs", Napi::BigInt::New(env, value.lease.remaining_ns));
    out.Set("lease", lease);
    if(value.keeper_preference)
    {
        static const std::initializer_list<const char*> names = {"UNSPECIFIED", "HONORED", "NOT_IN_ROUTE", "RETAINED"};
        out.Set("keeperPreference", enumName(*value.keeper_preference, names));
    }
    return out;
}
Js js(Napi::Env env, const sdk::WriterLease& value)
{
    auto out = Napi::Object::New(env);
    auto grant = Napi::Object::New(env);
    grant.Set("durationNs", Napi::BigInt::New(env, value.grant.duration_ns));
    grant.Set("remainingNs", Napi::BigInt::New(env, value.grant.remaining_ns));
    out.Set("grant", grant);
    out.Set("estimatedRemainingNs", Napi::BigInt::New(env, value.estimated_remaining_ns));
    out.Set("confirmed", value.confirmed);
    if(value.termination_cause)
        out.Set("terminationCause", causeName(*value.termination_cause));
    out.Set("renewals", Napi::BigInt::New(env, value.renewals));
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
Js js(Napi::Env env, const sdk::Position& value)
{
    auto out = Napi::Object::New(env);
    out.Set("hlc", js(env, value.hlc));
    out.Set("id", js(env, value.id));
    return out;
}
Js js(Napi::Env env, const sdk::HlcRange& value)
{
    auto out = Napi::Object::New(env);
    out.Set("start", js(env, value.start));
    out.Set("end", js(env, value.end));
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
    physical.Set("status", enumName(value.physical.status, {"SYNCED", "UNSYNCED", "UNAVAILABLE"}));
    if(value.physical.uncertainty_ns)
        physical.Set("uncertaintyNs", Napi::BigInt::New(env, *value.physical.uncertainty_ns));
    out.Set("physical", physical);
    auto envelope = Napi::Object::New(env);
    envelope.Set("contentType", value.envelope.content_type);
    envelope.Set("payload", data(env, value.envelope.payload));
    envelope.Set("traceId", data(env, value.envelope.trace_id));
    envelope.Set("spanId", data(env, value.envelope.span_id));
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
    static const std::initializer_list<const char*> names = {"NONE",
                                                             "LAGGING_WRITERS",
                                                             "PHYSICAL_AXIS_UNBOUNDED",
                                                             "SOURCE_FAILED",
                                                             "TRUNCATED"};
    out.Set("reason", enumName(value.reason, names));
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
    set(out, "completion", value.completion);
    set(out, "continuation", value.continuation);
    return out;
}
Js js(Napi::Env env, const std::optional<sdk::StreamItem>& value) { return value ? js(env, *value) : env.Null(); }
Js js(Napi::Env env, const std::string& value) { return Napi::String::New(env, value); }
Js js(Napi::Env env, bool value) { return Napi::Boolean::New(env, value); }
Js js(Napi::Env env, const sdk::BatchResult& values)
{
    auto out = Napi::Array::New(env, values.size());
    for(size_t i = 0; i < values.size(); ++i)
        out.Set(i, values[i].ok() ? js(env, *values[i]) : error(env, values[i].status()));
    return out;
}
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
    auto external = Napi::External<Held>::New(env,
                                              new Held(value),
                                              [](Napi::Env env, Held* pointer)
                                              {
                                                  std::unique_ptr<Held> owned(pointer);
                                                  runtime(env).bury(std::move(*owned));
                                              });
    if(value->kind == Handle::Writer)
    {
        auto out = Napi::Object::New(env);
        out.Set("handle", external);
        out.Set("acquisition", js(env, value->acquisition));
        return out;
    }
    if(value->kind == Handle::Session)
    {
        auto out = Napi::Object::New(env);
        auto context = Napi::Object::New(env);
        const auto& ref = value->session->context();
        context.Set("storyId", Napi::BigInt::New(env, ref.story_id));
        context.Set("chronicle", ref.chronicle);
        context.Set("name", ref.name);
        auto identity = Napi::Object::New(env);
        identity.Set("agentId", value->session->identity().agent_id);
        identity.Set("slot", value->session->identity().slot);
        out.Set("handle", external);
        out.Set("context", context);
        out.Set("identity", identity);
        return out;
    }
    return external;
}

namespace
{
std::mutex runtimes_mutex;
std::set<Runtime*>& runtimes()
{
    static auto* all = new std::set<Runtime*>;
    return *all;
}
// process.exit() skips environment cleanup, so exit() drains every live runtime before static destructors run.
void drainAtExit()
{
    std::vector<Runtime*> live;
    {
        std::lock_guard lock(runtimes_mutex);
        live.assign(runtimes().begin(), runtimes().end());
    }
    for(auto* item: live) item->shutdown();
}
} // namespace
Runtime::Runtime()
    : reaper_([this] { reap(); })
{
    static const bool registered = std::atexit(drainAtExit) == 0;
    static_cast<void>(registered);
    std::lock_guard lock(runtimes_mutex);
    runtimes().insert(this);
}
Runtime::~Runtime()
{
    shutdown();
    std::lock_guard lock(runtimes_mutex);
    runtimes().erase(this);
}
void Runtime::reap()
{
    std::unique_lock lock(mutex_);
    for(;;)
    {
        changed_.wait(lock, [&] { return closed_ || !graveyard_.empty(); });
        while(!graveyard_.empty())
        {
            auto value = std::move(graveyard_.front());
            graveyard_.pop_front();
            lock.unlock();
            value.reset();
            lock.lock();
        }
        if(closed_)
            return;
    }
}
void Runtime::bury(Held value)
{
    {
        std::lock_guard lock(mutex_);
        if(!closed_)
        {
            graveyard_.push_back(std::move(value));
            changed_.notify_all();
            return;
        }
    }
    // After teardown no thread is left to need this loop, so the handle drops here.
    value.reset();
}
void Runtime::track(const Held& value)
{
    std::lock_guard lock(mutex_);
    std::erase_if(streams_, [](const auto& item) { return item.expired(); });
    streams_.push_back(value);
}
void Runtime::enter()
{
    std::lock_guard lock(mutex_);
    ++inflight_;
}
void Runtime::leave()
{
    std::lock_guard lock(mutex_);
    --inflight_;
    changed_.notify_all();
}
void Runtime::shutdown()
{
    std::vector<Held> live;
    {
        std::lock_guard lock(mutex_);
        if(closed_)
            return;
        for(const auto& item: streams_)
            if(auto stream = item.lock())
                live.push_back(std::move(stream));
    }
    for(const auto& stream: live) stream->cancel();
    live.clear();
    {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [&] { return inflight_ == 0; });
        closed_ = true;
        changed_.notify_all();
    }
    if(reaper_.joinable() && reaper_.get_id() != std::this_thread::get_id())
        reaper_.join();
}
Runtime& runtime(Napi::Env env) { return *env.GetInstanceData<Runtime>(); }

namespace
{
Js connect(const Napi::CallbackInfo& info)
{
    auto options = object(info[0]);
    auto config = clientOptions(options);
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
absl::StatusOr<bool> done(const absl::Status& status)
{
    if(!status.ok())
        return status;
    return true;
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
        return work<bool>(env, [held, name, end] { return done(held->client->destroyChronicle(name, end)); });
    }
    if(method == "destroyStory")
    {
        auto id = unsigned64(args.Get(uint32_t{0}));
        return work<bool>(env, [held, id, end] { return done(held->client->destroyStory(id, end)); });
    }
    throw Napi::TypeError::New(env, "unknown catalog operation");
}
AcquireOptions acquireOptions(Napi::Object options)
{
    AcquireOptions result;
    if(has(options, "leaseDurationNs"))
        result.lease_duration_ns = signed64(options.Get("leaseDurationNs"));
    if(has(options, "preferredKeeperProcessId"))
        result.preferred_keeper_process_id = text(options.Get("preferredKeeperProcessId"));
    if(has(options, "takeover"))
    {
        if(!options.Get("takeover").IsBoolean())
            throw Napi::TypeError::New(options.Env(), "takeover must be a boolean");
        result.takeover = options.Get("takeover").As<Napi::Boolean>().Value();
    }
    if(has(options, "expectedPriorIncarnation"))
        result.expected_prior_incarnation = unsigned64(options.Get("expectedPriorIncarnation"));
    if(has(options, "acquireRequestId"))
        result.acquire_request_id = text(options.Get("acquireRequestId"));
    return result;
}
Js acquire(const Napi::CallbackInfo& info)
{
    auto held = handle(info[0], Handle::Client);
    auto story = unsigned64(info[1]);
    auto identity = text(info[2]);
    auto options = object(info[3]);
    auto request = acquireOptions(options);
    auto end = deadline(options);
    return work<Held>(info.Env(),
                      [held, story, identity, request = std::move(request), end]() mutable -> absl::StatusOr<Held>
                      {
                          auto result = held->client->acquire(story, identity, std::move(request), end);
                          if(!result.ok())
                              return result.status();
                          auto out = std::make_shared<Handle>(Handle::Writer);
                          out->writer = std::make_shared<sdk::Writer>(std::move(*result));
                          out->acquisition = out->writer->acquisition();
                          return out;
                      });
}
Js newAcquireRequestId(const Napi::CallbackInfo& info)
{
    auto result = handle(info[0], Handle::Client)->client->newAcquireRequestId();
    if(!result.ok())
        throw Napi::Error(info.Env(), error(info.Env(), result.status()));
    return Napi::String::New(info.Env(), *result);
}
Js lease(const Napi::CallbackInfo& info) { return js(info.Env(), handle(info[0], Handle::Writer)->writer->lease()); }
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
    auto* owner = &runtime(info.Env());
    auto opened = [owner](auto result, Handle::Kind kind) -> absl::StatusOr<Held>
    {
        if(!result.ok())
            return result.status();
        auto out = std::make_shared<Handle>(kind);
        if constexpr(std::is_same_v<std::decay_t<decltype(*result)>, sdk::ReadStream>)
            out->read = std::make_shared<sdk::ReadStream>(std::move(*result));
        else
            out->tail = std::make_shared<sdk::TailStream>(std::move(*result));
        owner->track(out);
        return out;
    };
    if(mode == "physical")
    {
        auto range = object(info[3]);
        sdk::PhysicalRange input{signed64(range.Get("startNs")), signed64(range.Get("endNs"))};
        return work<Held>(info.Env(),
                          [held, story, input, end, opened]
                          { return opened(held->client->readPhysical(story, input, end), Handle::Read); });
    }
    if(mode == "read")
    {
        auto input = hlcRange(info[3]);
        return work<Held>(info.Env(),
                          [held, story, input, end, opened]
                          { return opened(held->client->read(story, input, end), Handle::Read); });
    }
    std::optional<sdk::Position> after;
    if(!info[3].IsNull() && !info[3].IsUndefined())
        after = position(info[3]);
    return work<Held>(info.Env(),
                      [held, story, after, end, opened]
                      { return opened(held->client->tail(story, after, end), Handle::Tail); });
}
Js next(const Napi::CallbackInfo& info)
{
    auto kind = text(info[1]) == "read" ? Handle::Read : Handle::Tail;
    auto held = handle(info[0], kind);
    auto end = deadline(object(info[2]));
    if(held->pulling.exchange(true))
        throw Napi::Error::New(info.Env(), "stream already has a pending next");
    try
    {
        return thread<std::optional<sdk::StreamItem>>(info.Env(),
                                                      [held, end]
                                                      {
                                                          auto result = held->read ? held->read->next(end)
                                                                                   : held->tail->next(end);
                                                          held->pulling = false;
                                                          return result;
                                                      });
    }
    catch(...)
    {
        held->pulling = false;
        throw;
    }
}
Js cancel(const Napi::CallbackInfo& info)
{
    auto kind = text(info[1]) == "read" ? Handle::Read : Handle::Tail;
    handle(info[0], kind)->cancel();
    return info.Env().Undefined();
}
Napi::Object init(Napi::Env env, Napi::Object exports)
{
    auto* owner = new Runtime;
    env.SetInstanceData(owner);
    env.AddCleanupHook([owner] { owner->shutdown(); });
    exports.Set("connect", Napi::Function::New(env, connect));
    exports.Set("catalog", Napi::Function::New(env, catalog));
    exports.Set("acquire", Napi::Function::New(env, acquire));
    exports.Set("newAcquireRequestId", Napi::Function::New(env, newAcquireRequestId));
    exports.Set("lease", Napi::Function::New(env, lease));
    exports.Set("append", Napi::Function::New(env, append));
    exports.Set("appendBatch", Napi::Function::New(env, appendBatch));
    exports.Set("release", Napi::Function::New(env, release));
    exports.Set("stream", Napi::Function::New(env, stream));
    exports.Set("next", Napi::Function::New(env, next));
    exports.Set("cancel", Napi::Function::New(env, cancel));
    initContext(env, exports);
    return exports;
}
} // namespace
} // namespace binding
Napi::Object Init(Napi::Env env, Napi::Object exports) { return binding::init(env, exports); }
NODE_API_MODULE(chronolog_node, Init)
