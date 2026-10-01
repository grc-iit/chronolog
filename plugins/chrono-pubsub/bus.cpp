#include "chronolog/pubsub/bus.h"
#include <algorithm>
#include <atomic>
#include <charconv>
#include <condition_variable>
#include <exception>
#include <string_view>

namespace chronolog::pubsub
{
namespace
{
using Clock = std::chrono::system_clock;
client::Deadline bounded(client::Deadline d) { return d.value_or(Clock::now() + std::chrono::seconds(10)); }
bool valid(const std::string& name)
{
    return !name.empty() && name.size() <= 255 &&
           std::all_of(name.begin(), name.end(), [](unsigned char c) { return c >= 0x20 && c != 0x7f; });
}
bool valid(client::Position p)
{
    const bool event = p.id.writer_id && p.id.incarnation && p.id.sequence;
    const bool frontier = !p.id.writer_id && !p.id.incarnation && !p.id.sequence;
    return p.hlc.physical_ns >= 0 && p.id.story_id && (event || frontier);
}
bool after(const Event& event, client::Position p)
{
    Event previous;
    previous.id = p.id;
    previous.hlc = p.hlc;
    return ReplayLess(previous, event);
}
bool retryable(const absl::Status& s)
{
    return absl::IsUnavailable(s) || absl::IsDeadlineExceeded(s) || absl::IsAborted(s) ||
           absl::IsFailedPrecondition(s) || absl::IsCancelled(s);
}
} // namespace
struct Subscription::State
{
    client::Client* client{};
    StoryId story{};
    std::string topic;
    Callback callback;
    SubscribeOptions options;
    mutable std::mutex mutex;
    std::condition_variable changed;
    bool stopping{};
    absl::Status terminal;
    std::optional<client::Position> cursor;
    std::optional<Completion> completion;
    std::unique_ptr<client::TailStream> tail;
    bool stopped()
    {
        std::lock_guard lock(mutex);
        return stopping;
    }
    void finish(absl::Status status)
    {
        std::lock_guard lock(mutex);
        terminal = std::move(status);
    }
    void pause()
    {
        std::unique_lock lock(mutex);
        changed.wait_until(lock,
                           std::min(*options.deadline, Clock::now() + options.retry_delay),
                           [&] { return stopping; });
    }
    void run()
    {
        while(Clock::now() < *options.deadline && !stopped())
        {
            std::optional<client::Position> resume;
            {
                std::lock_guard lock(mutex);
                resume = cursor;
            }
            auto stream = client->tail(story, resume, options.deadline);
            if(!stream.ok())
            {
                if(!retryable(stream.status()))
                {
                    finish(stream.status());
                    return;
                }
                pause();
                continue;
            }
            {
                std::lock_guard lock(mutex);
                if(stopping)
                    return;
                tail = std::make_unique<client::TailStream>(std::move(*stream));
            }
            while(Clock::now() < *options.deadline && !stopped())
            {
                auto pull_end = std::min(*options.deadline, Clock::now() + options.pull_timeout);
                auto item = tail->next(pull_end);
                if(!item.ok())
                {
                    if(stopped())
                        return;
                    if(!retryable(item.status()))
                    {
                        finish(item.status());
                        return;
                    }
                    break;
                }
                if(!*item)
                {
                    finish(absl::DataLossError("Tail ended without Completion"));
                    return;
                }
                if((**item).completion)
                {
                    std::lock_guard lock(mutex);
                    completion = (**item).completion;
                    // A terminal Tail is reopened from the committed cursor.
                    break;
                }
                for(auto& event: (**item).events)
                {
                    {
                        std::lock_guard lock(mutex);
                        if(cursor && !after(event, *cursor))
                            continue;
                    }
                    Message message{topic, std::move(event)};
                    bool delivered = false;
                    while(Clock::now() < *options.deadline && !stopped())
                    {
                        absl::Status result;
                        try
                        {
                            result = callback(message);
                        }
                        catch(const std::exception& e)
                        {
                            result = absl::UnknownError(e.what());
                        }
                        catch(...)
                        {
                            result = absl::UnknownError("subscriber callback threw");
                        }
                        if(result.ok())
                        {
                            std::lock_guard lock(mutex);
                            cursor = message.position();
                            delivered = true;
                            break;
                        }
                        pause();
                    }
                    if(!delivered)
                        break;
                }
            }
            {
                std::lock_guard lock(mutex);
                tail.reset();
            }
            if(!stopped())
                pause();
        }
        if(!stopped())
            finish(absl::DeadlineExceededError("subscription deadline"));
    }
};
Subscription::Subscription(std::shared_ptr<State> state)
    : state_(std::move(state))
    , worker_([state = state_] { state->run(); })
{}
Subscription::~Subscription() { stop(); }
void Subscription::requestStop()
{
    std::lock_guard lock(state_->mutex);
    state_->stopping = true;
    if(state_->tail)
        state_->tail->cancel();
    state_->changed.notify_all();
}
void Subscription::stop()
{
    requestStop();
    if(worker_.joinable())
    {
        // The worker owns State until it exits, including callback self-stop.
        if(worker_.get_id() == std::this_thread::get_id())
            worker_.detach();
        else
            worker_.join();
    }
}
std::optional<client::Position> Subscription::position() const
{
    std::lock_guard lock(state_->mutex);
    return state_->cursor;
}
absl::Status Subscription::status() const
{
    std::lock_guard lock(state_->mutex);
    return state_->terminal;
}
std::optional<Completion> Subscription::completion() const
{
    std::lock_guard lock(state_->mutex);
    return state_->completion;
}
Bus::Bus(client::Client& client, std::string chronicle)
    : client_(client)
    , chronicle_(std::move(chronicle))
{
    static std::atomic<uint64_t> sequence{};
    identity_ = "pubsub-" + std::to_string(Clock::now().time_since_epoch().count()) + "-" + std::to_string(++sequence);
}
absl::StatusOr<StoryId> Bus::topic(const std::string& name, client::Deadline deadline)
{
    if(!valid(name) || !valid(chronicle_))
        return absl::InvalidArgumentError("invalid Catalog topic or chronicle name");
    auto chronicle = client_.createChronicle(chronicle_, deadline);
    if(!chronicle.ok() && !absl::IsAlreadyExists(chronicle.status()))
        return chronicle.status();
    auto story = client_.createStory(chronicle_, name, deadline);
    if(story.ok())
        return story->id;
    if(!absl::IsAlreadyExists(story.status()))
        return story.status();
    auto stories = client_.listStories(chronicle_, deadline);
    if(!stories.ok())
        return stories.status();
    for(const auto& s: *stories)
        if(s.name == name && !s.tombstoned)
            return s.id;
    return absl::NotFoundError("topic disappeared during Catalog lookup");
}
absl::StatusOr<client::AppendResult> Bus::publish(const std::string& name, std::string payload, PublishOptions options)
{
    auto deadline = bounded(options.deadline);
    std::unique_lock lock(publish_mutex_, std::defer_lock);
    if(!lock.try_lock_until(*deadline))
        return absl::DeadlineExceededError("publisher busy");
    auto id = topic(name, deadline);
    if(!id.ok())
        return id.status();
    auto writer = client_.acquire(*id, identity_, deadline);
    if(!writer.ok())
        return writer.status();
    client::AppendSpec spec;
    spec.envelope = std::move(options.metadata);
    spec.envelope.payload = std::move(payload);
    if(spec.envelope.content_type.empty())
        spec.envelope.content_type = "application/octet-stream";
    spec.envelope.attributes["chronolog.pubsub.topic"] = name;
    spec.durability = options.durability;
    auto result = writer->append(spec, deadline);
    auto released = writer->release(deadline);
    (void)released;
    return result;
}
absl::StatusOr<std::unique_ptr<Subscription>>
Bus::subscribe(const std::string& name, Callback callback, SubscribeOptions options)
{
    options.deadline = options.deadline.value_or(Clock::now() + std::chrono::seconds(60));
    if(!callback || options.pull_timeout.count() <= 0 || options.retry_delay.count() <= 0 ||
       Clock::now() >= *options.deadline)
        return absl::InvalidArgumentError("invalid subscription options");
    if(options.start != Start::Earliest && options.start != Start::Latest && options.start != Start::Saved)
        return absl::InvalidArgumentError("unknown subscription start");
    if((options.start == Start::Saved) != options.position.has_value())
        return absl::InvalidArgumentError("Saved requires exactly one position");
    if(options.position && !valid(*options.position))
        return absl::InvalidArgumentError("invalid saved position");
    auto deadline = std::min(*options.deadline, *bounded({}));
    auto id = topic(name, deadline);
    if(!id.ok())
        return id.status();
    if(options.position && options.position->id.story_id != *id)
        return absl::InvalidArgumentError("saved position belongs to another topic");
    if(options.start == Start::Latest)
    {
        auto probe = client_.read(*id, {{}, {}}, deadline);
        if(!probe.ok())
            return probe.status();
        bool completed = false;
        while(Clock::now() < deadline)
        {
            auto item = probe->next(deadline);
            if(!item.ok())
                return item.status();
            if(!*item)
                break;
            if((**item).completion)
            {
                if(!(**item).completion->complete)
                    return absl::UnavailableError("latest topic frontier is incomplete");
                options.position = client::Position{(**item).completion->frontier, {*id, 0, 0, 0}};
                completed = true;
                break;
            }
        }
        if(!completed)
            return absl::DeadlineExceededError("latest topic frontier");
    }
    auto state = std::make_shared<Subscription::State>();
    state->client = &client_;
    state->story = *id;
    state->topic = name;
    state->callback = std::move(callback);
    state->cursor = options.position;
    state->options = std::move(options);
    return std::unique_ptr<Subscription>(new Subscription(std::move(state)));
}
absl::StatusOr<kvs::Version>
savePosition(kvs::Store& store, const std::string& key, client::Position position, client::Deadline deadline)
{
    if(!valid(position))
        return absl::InvalidArgumentError("invalid consumer position");
    std::string payload = std::to_string(position.hlc.physical_ns) + " " + std::to_string(position.hlc.logical) + " " +
                          std::to_string(position.id.story_id) + " " + std::to_string(position.id.writer_id) + " " +
                          std::to_string(position.id.incarnation) + " " + std::to_string(position.id.sequence);
    kvs::PutOptions options;
    options.metadata.content_type = "application/vnd.chronolog.pubsub.position";
    options.deadline = deadline;
    return store.put(key, std::move(payload), std::move(options));
}
absl::StatusOr<client::Position> loadPosition(kvs::Store& store, const std::string& key, kvs::GetOptions options)
{
    auto result = store.get(key, std::move(options));
    if(!result.ok())
        return result.status();
    if(!result->completion.complete)
        return absl::UnavailableError("consumer position read incomplete");
    if(!result->status.ok())
        return result->status;
    if(!result->value || result->value->envelope.content_type != "application/vnd.chronolog.pubsub.position")
        return absl::InvalidArgumentError("not a consumer position");
    std::string_view text(result->value->value);
    client::Position p;
    auto parse = [&](auto& number)
    {
        if(text.empty())
            return false;
        auto split = text.find(' ');
        auto part = text.substr(0, split);
        auto decoded = std::from_chars(part.data(), part.data() + part.size(), number);
        if(decoded.ec != std::errc{} || decoded.ptr != part.data() + part.size())
            return false;
        text = split == text.npos ? std::string_view{} : text.substr(split + 1);
        return true;
    };
    if(!parse(p.hlc.physical_ns) || !parse(p.hlc.logical) || !parse(p.id.story_id) || !parse(p.id.writer_id) ||
       !parse(p.id.incarnation) || !parse(p.id.sequence) || !text.empty() || !valid(p))
        return absl::InvalidArgumentError("malformed consumer position");
    return p;
}
} // namespace chronolog::pubsub
