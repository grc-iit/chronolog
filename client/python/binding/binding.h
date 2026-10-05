#pragma once
#include "chronolog/client/client.h"
#include "chronolog/context/context.h"
#include <nanobind/nanobind.h>
#include <nanobind/stl/map.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>
#include <unistd.h>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace binding
{
namespace nb = nanobind;
using namespace nb::literals;
using namespace chronolog;
namespace sdk = chronolog::client;
namespace ctx = chronolog::context;

template <class... Args>
nb::dict fields(Args&&... args)
{
    return nb::cast<nb::dict>(nb::module_::import_("builtins").attr("dict")(std::forward<Args>(args)...));
}
inline nb::object value(const char* name, nb::dict fields)
{
    return nb::module_::import_("chronolog").attr(name)(**fields);
}
inline nb::object none() { return nb::none(); }
template <class T, class F>
nb::object maybe(const std::optional<T>& input, F convert)
{
    return input ? nb::object(convert(*input)) : nb::none();
}
template <class T, class F>
nb::tuple tuple(const std::vector<T>& values, F convert)
{
    nb::list out;
    for(const auto& item: values) out.append(convert(item));
    return nb::tuple(out);
}
nb::object status(const absl::Status&);
nb::object error(const absl::Status&);
[[noreturn]] void raise(const absl::Status&);
inline void check(const absl::Status& status)
{
    if(!status.ok())
        raise(status);
}
template <class T>
T unwrap(absl::StatusOr<T> result)
{
    check(result.status());
    return std::move(*result);
}
sdk::Deadline deadline(std::optional<double> timeout);
uint64_t u64(nb::handle);
int64_t i64(nb::handle);
uint32_t u32(nb::handle);
std::string text(nb::handle);
std::string binary(nb::handle);
template <class T, class F>
std::optional<T> optional(nb::handle input, F convert)
{
    if(input.is_none())
        return std::nullopt;
    return convert(input);
}
template <class T, class F>
std::vector<T> list(nb::handle input, F convert)
{
    std::vector<T> out;
    for(auto item: input) out.push_back(convert(item));
    return out;
}

nb::object pack(const Hlc&);
nb::object pack(const EventId&);
nb::object pack(const sdk::Position&);
nb::object pack(const sdk::HlcRange&);
nb::object pack(const sdk::AppendResult&);
nb::object pack(const Event&);
nb::object pack(const sdk::AwaitResult&);
nb::object pack(const Completion&);
Hlc hlc(nb::handle);
EventId eventId(nb::handle);
sdk::Position position(nb::handle);
sdk::HlcRange hlcRange(nb::handle);
Envelope envelope(nb::handle);
TimeReading timeReading(nb::handle);
sdk::ClientOptions clientOptions(nb::handle);

// Owns native teardown for this process. Finalizers run with the GIL held, so they hand native objects to a reaper
// thread instead of destroying them inline, and no finalizer joins SDK work. At interpreter exit shutdown() refuses new
// calls, cancels live streams, waits for in-flight calls with the GIL released and joins the reaper. A forked child
// gets its own Runtime and never touches the parent's locks or threads.
class Runtime
{
public:
    static Runtime& get();
    void enter();
    void leave();
    void bury(std::shared_ptr<void>);
    void track(std::weak_ptr<sdk::ReadStream>);
    void track(std::weak_ptr<sdk::TailStream>);
    void shutdown();

private:
    Runtime();
    void reap();
    const pid_t pid_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<std::shared_ptr<void>> graveyard_;
    std::vector<std::weak_ptr<sdk::ReadStream>> reads_;
    std::vector<std::weak_ptr<sdk::TailStream>> tails_;
    size_t inflight_{0};
    bool closing_{false};
    bool closed_{false};
    std::thread reaper_;
};

// A native object owned by the process that created it. A forked child refuses to use it and never destroys it.
template <class T>
class Held
{
public:
    explicit Held(std::shared_ptr<T> value)
        : value_(std::move(value))
        , pid_(getpid())
    {}
    Held(const Held&) = delete;
    Held& operator=(const Held&) = delete;
    ~Held()
    {
        if(!value_)
            return;
        if(pid_ != getpid())
            new std::shared_ptr<T>(std::move(value_));
        else
            Runtime::get().bury(std::move(value_));
    }
    const std::shared_ptr<T>& shared() const
    {
        if(pid_ != getpid())
            raise(absl::FailedPreconditionError("client used after fork; create a new Client in this process"));
        return value_;
    }
    T& operator*() const { return *shared(); }
    T* operator->() const { return shared().get(); }

private:
    std::shared_ptr<T> value_;
    const pid_t pid_;
};

// Runs a blocking native call with the GIL released, counted so interpreter exit can wait for it.
template <class F>
auto call(F&& f)
{
    auto& runtime = Runtime::get();
    runtime.enter();
    struct Leave
    {
        Runtime& runtime;
        ~Leave() { runtime.leave(); }
    } leave{runtime};
    nb::gil_scoped_release release;
    return f();
}

void initContext(nb::module_&);
} // namespace binding
