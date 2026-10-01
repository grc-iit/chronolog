#include "chronolog/kvs/store.h"
#include <algorithm>
#include <atomic>
#include <limits>

namespace chronolog::kvs
{
namespace
{
client::Deadline bounded(client::Deadline deadline)
{
    return deadline.value_or(std::chrono::system_clock::now() + std::chrono::seconds(10));
}
bool valid(const std::string& name)
{
    return !name.empty() && name.size() <= 255 &&
           std::all_of(name.begin(), name.end(), [](unsigned char c) { return c >= 0x20 && c != 0x7f; });
}
Hlc successor(Hlc h)
{
    if(h.logical == std::numeric_limits<uint32_t>::max())
    {
        if(h.physical_ns == std::numeric_limits<int64_t>::max())
            return h;
        ++h.physical_ns;
        h.logical = 0;
    }
    else
        ++h.logical;
    return h;
}
Value decode(const Event& e)
{
    auto marker = e.envelope.attributes.find("chronolog.kvs.deleted");
    return {e.envelope.payload,
            {e.id, e.hlc, e.durability},
            marker != e.envelope.attributes.end() && marker->second == "true",
            e.envelope};
}
client::AppendSpec spec(const std::string& key, std::string value, bool deleted, const PutOptions& options)
{
    client::AppendSpec result;
    result.envelope = options.metadata;
    result.envelope.payload = std::move(value);
    if(result.envelope.content_type.empty())
        result.envelope.content_type = "application/vnd.chronolog.kv+octet";
    result.envelope.attributes["chronolog.kvs.key"] = key;
    result.envelope.attributes["chronolog.kvs.deleted"] = deleted ? "true" : "false";
    result.durability = options.durability;
    return result;
}
} // namespace
absl::StatusOr<std::optional<HistoryItem>> History::next(client::Deadline deadline)
{
    auto item = stream_.next(bounded(deadline));
    if(!item.ok())
        return item.status();
    if(!*item)
        return std::optional<HistoryItem>{};
    HistoryItem result;
    for(const auto& e: (**item).events) result.versions.push_back(decode(e));
    result.completion = (**item).completion;
    result.continuation = (**item).continuation;
    return std::optional<HistoryItem>(std::move(result));
}
Store::Store(client::Client& client, std::string chronicle, StoreOptions options)
    : client_(client)
    , chronicle_(std::move(chronicle))
    , options_(options)
{
    static std::atomic<uint64_t> sequence{};
    identity_ = "kvs-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + "-" +
                std::to_string(++sequence);
    options_.max_keys = std::max(size_t{1}, options_.max_keys);
    options_.max_versions_per_key = std::max(size_t{1}, options_.max_versions_per_key);
}
absl::StatusOr<Store::Entry*> Store::entry(const std::string& key, bool create, client::Deadline deadline)
{
    if(!valid(key) || !valid(chronicle_))
        return absl::InvalidArgumentError("invalid Catalog key or chronicle name");
    if(auto found = entries_.find(key); found != entries_.end())
    {
        lru_.splice(lru_.begin(), lru_, found->second.lru);
        return &found->second;
    }
    StoryId id{};
    if(create)
    {
        auto chronicle = client_.createChronicle(chronicle_, deadline);
        if(!chronicle.ok() && !absl::IsAlreadyExists(chronicle.status()))
            return chronicle.status();
        auto story = client_.createStory(chronicle_, key, deadline);
        if(story.ok())
            id = story->id;
        else if(!absl::IsAlreadyExists(story.status()))
            return story.status();
    }
    if(!id)
    {
        auto stories = client_.listStories(chronicle_, deadline);
        if(!stories.ok())
            return stories.status();
        for(const auto& story: *stories)
            if(story.name == key && !story.tombstoned)
            {
                id = story.id;
                break;
            }
    }
    if(!id)
        return absl::NotFoundError("key has no story");
    if(entries_.size() >= options_.max_keys)
    {
        entries_.erase(lru_.back());
        lru_.pop_back();
    }
    lru_.push_front(key);
    auto& result = entries_[key];
    result.story = id;
    result.lru = lru_.begin();
    return &result;
}
void Store::insert(Entry& entry, Event event)
{
    if(std::any_of(entry.events.begin(), entry.events.end(), [&](const Event& e) { return e.id == event.id; }))
        return;
    auto place = std::lower_bound(entry.events.begin(), entry.events.end(), event, ReplayLess);
    entry.events.insert(place, std::move(event));
    size_t bytes{};
    for(const auto& e: entry.events)
    {
        bytes += e.envelope.payload.size() + e.envelope.content_type.size() + e.envelope.trace_id.size() +
                 e.envelope.span_id.size();
        for(const auto& [k, v]: e.envelope.attributes) bytes += k.size() + v.size();
    }
    while(entry.events.size() > options_.max_versions_per_key || bytes > options_.max_bytes_per_key)
    {
        // Recompute a conservative floor before removing the oldest version.
        entry.floor = std::max(entry.floor, successor(entry.events.front().hlc));
        entry.events.erase(entry.events.begin());
        bytes = 0;
        for(const auto& e: entry.events)
        {
            bytes += e.envelope.payload.size() + e.envelope.content_type.size() + e.envelope.trace_id.size() +
                     e.envelope.span_id.size();
            for(const auto& [k, v]: e.envelope.attributes) bytes += k.size() + v.size();
        }
    }
}
absl::StatusOr<Version> Store::append(const std::string& key, std::string value, bool deleted, PutOptions options)
{
    auto deadline = bounded(options.deadline);
    auto e = entry(key, true, deadline);
    if(!e.ok())
        return e.status();
    auto writer = client_.acquire((*e)->story, identity_, deadline);
    if(!writer.ok())
        return writer.status();
    auto item = spec(key, std::move(value), deleted, options);
    auto result = writer->append(item, deadline);
    auto released = writer->release(deadline);
    (void)released;
    if(!result.ok())
        return result.status();
    insert(**e, {result->event_id, {}, result->hlc, std::move(item.envelope), result->achieved});
    return Version{result->event_id, result->hlc, result->achieved, result->acked()};
}
absl::StatusOr<Version> Store::put(const std::string& key, std::string value, PutOptions options)
{
    return append(key, std::move(value), false, std::move(options));
}
absl::StatusOr<Version> Store::erase(const std::string& key, PutOptions options)
{
    return append(key, {}, true, std::move(options));
}
absl::StatusOr<std::vector<absl::StatusOr<Version>>>
Store::putBatch(const std::string& key, std::span<const std::string> values, PutOptions options)
{
    if(values.empty() || values.size() > 10000)
        return absl::InvalidArgumentError("batch size must be 1..10000");
    auto deadline = bounded(options.deadline);
    auto e = entry(key, true, deadline);
    if(!e.ok())
        return e.status();
    auto writer = client_.acquire((*e)->story, identity_, deadline);
    if(!writer.ok())
        return writer.status();
    std::vector<client::AppendSpec> specs;
    for(const auto& value: values) specs.push_back(spec(key, value, false, options));
    auto batch = writer->appendBatch(specs, deadline);
    auto released = writer->release(deadline);
    (void)released;
    if(!batch.ok())
        return batch.status();
    std::vector<absl::StatusOr<Version>> results;
    for(size_t i = 0; i < batch->size(); ++i)
    {
        const auto& r = (*batch)[i];
        if(!r.ok())
        {
            results.emplace_back(r.status());
            continue;
        }
        insert(**e, {r->event_id, {}, r->hlc, std::move(specs[i].envelope), r->achieved});
        results.emplace_back(Version{r->event_id, r->hlc, r->achieved, r->acked()});
    }
    return results;
}
absl::StatusOr<History> Store::history(const std::string& key, client::HlcRange range, client::Deadline deadline)
{
    deadline = bounded(deadline);
    auto e = entry(key, false, deadline);
    if(!e.ok())
        return e.status();
    auto stream = client_.read((*e)->story, range, deadline);
    if(!stream.ok())
        return stream.status();
    return History(std::move(*stream));
}
absl::Status Store::watch(const std::string& key, client::Deadline deadline)
{
    auto e = entry(key, false, bounded(deadline));
    if(!e.ok())
        return e.status();
    auto tail = client_.tail((*e)->story, {}, bounded(deadline));
    if(!tail.ok())
        return tail.status();
    (*e)->tail.emplace(std::move(*tail));
    return absl::OkStatus();
}
absl::StatusOr<HistoryItem> Store::follow(const std::string& key, client::Deadline deadline)
{
    auto e = entry(key, false, bounded(deadline));
    if(!e.ok())
        return e.status();
    if(!(*e)->tail)
        return absl::FailedPreconditionError("watch key before following");
    auto item = (*e)->tail->next(bounded(deadline));
    if(!item.ok())
        return item.status();
    if(!*item)
        return absl::OutOfRangeError("watch ended");
    HistoryItem result;
    result.completion = (**item).completion;
    for(auto& event: (**item).events)
    {
        result.versions.push_back(decode(event));
        insert(**e, std::move(event));
    }
    return result;
}
absl::StatusOr<GetResult> Store::get(const std::string& key, GetOptions options)
{
    auto deadline = bounded(options.deadline);
    auto found = entry(key, false, deadline);
    if(!found.ok())
        return found.status();
    auto& e = **found;
    Hlc end = options.at.value_or(Hlc{
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
                    .count(),
            0});
    if(options.causal_floor)
    {
        if(options.at && end <= *options.causal_floor)
            return absl::InvalidArgumentError("at must exceed causal floor");
        bool observed = std::any_of(e.events.begin(),
                                    e.events.end(),
                                    [&](const Event& v) { return v.hlc >= *options.causal_floor; });
        if(!observed)
        {
            auto tail = client_.tail(e.story, client::Position{*options.causal_floor, {e.story, 0, 0, 0}}, deadline);
            if(!tail.ok())
                return tail.status();
            while(!observed && std::chrono::system_clock::now() < *deadline)
            {
                auto item = tail->next(deadline);
                if(!item.ok())
                    return item.status();
                if(!*item || (**item).completion)
                    return absl::FailedPreconditionError("causal floor not observed");
                for(auto& event: (**item).events)
                {
                    observed = observed || event.hlc >= *options.causal_floor;
                    insert(e, std::move(event));
                }
            }
        }
        if(!observed)
            return absl::DeadlineExceededError("causal floor wait expired");
        if(!options.at)
            end = std::max(end, successor(*options.causal_floor));
    }
    std::optional<Completion> frontier_completion;
    if(!options.at)
    {
        auto probe = client_.read(e.story, {e.end, e.end}, deadline);
        if(!probe.ok())
            return probe.status();
        bool finished = false;
        while(std::chrono::system_clock::now() < *deadline)
        {
            auto item = probe->next(deadline);
            if(!item.ok())
                return item.status();
            if(!*item)
                break;
            if((**item).completion)
            {
                frontier_completion = (**item).completion;
                end = frontier_completion->frontier;
                if(options.causal_floor)
                    end = std::max(end, successor(*options.causal_floor));
                finished = true;
                break;
            }
        }
        if(!finished)
            return absl::DeadlineExceededError("frontier probe did not receive Completion");
    }
    const bool historical = end <= e.floor || (e.floor > Hlc{} && (e.events.empty() || end <= e.events.front().hlc));
    Hlc start = historical ? Hlc{} : std::min(e.end, end);
    std::optional<Event> best;
    if(!historical)
        for(const auto& event: e.events)
            if(event.hlc < end && (!best || ReplayLess(*best, event)))
                best = event;
    Completion completion;
    if(!historical && end <= e.end && e.completion.complete)
        completion = e.completion;
    else
    {
        auto stream = client_.read(e.story, {start, end}, deadline);
        if(!stream.ok())
            return stream.status();
        bool finished = false;
        while(std::chrono::system_clock::now() < *deadline)
        {
            auto item = stream->next(deadline);
            if(!item.ok())
                return item.status();
            if(!*item)
                break;
            for(auto& event: (**item).events)
            {
                if(!best || ReplayLess(*best, event))
                    best = event;
                if(!historical)
                    insert(e, std::move(event));
            }
            if((**item).completion)
            {
                completion = *(**item).completion;
                finished = true;
                break;
            }
        }
        if(!finished)
            return absl::DeadlineExceededError("get did not receive Completion");
        if(!historical && completion.complete)
        {
            e.end = end;
            e.completion = completion;
        }
    }
    if(frontier_completion && !frontier_completion->complete)
        completion = *frontier_completion;
    // For latest, only the sealed Replay prefix can be described as current.
    if(!options.at && !completion.complete)
    {
        if(best && best->hlc >= completion.frontier)
            best.reset();
        for(const auto& event: e.events)
            if(event.hlc < completion.frontier && (!best || ReplayLess(*best, event)))
                best = event;
    }
    GetResult result{absl::OkStatus(), {}, completion};
    if(best)
        result.value = decode(*best);
    if(!result.value || result.value->deleted)
        result.status = absl::NotFoundError("key absent at requested HLC");
    return result;
}
} // namespace chronolog::kvs
