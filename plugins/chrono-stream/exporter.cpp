#include "chronolog/stream/stream.h"
#include <set>
#include <thread>

namespace chronolog::stream
{
namespace
{
client::Deadline bounded(client::Deadline deadline)
{
    return deadline.value_or(std::chrono::system_clock::now() + std::chrono::seconds(10));
}
Json positionJson(client::Position p)
{
    return {{"v", 1},
            {"physical_ns", p.hlc.physical_ns},
            {"logical", p.hlc.logical},
            {"story_id", p.id.story_id},
            {"writer_id", p.id.writer_id},
            {"incarnation", p.id.incarnation},
            {"sequence", p.id.sequence}};
}
bool after(const Event& event, client::Position position)
{
    Event previous;
    previous.hlc = position.hlc;
    previous.id = position.id;
    return ReplayLess(previous, event);
}
} // namespace
Exporter::Exporter(client::Client& client,
                   kvs::Store& store,
                   std::string chronicle,
                   std::vector<std::string> stories,
                   InfluxSink& sink,
                   ExportOptions options)
    : client_(client)
    , positions_(store)
    , chronicle_(std::move(chronicle))
    , stories_(std::move(stories))
    , sink_(sink)
    , options_(std::move(options))
{}
absl::StatusOr<std::optional<client::Position>> Exporter::saved(const std::string& story, client::Deadline deadline)
{
    auto value = positions_.get(options_.consumer + "/" + story, {.deadline = bounded(deadline)});
    if(!value.ok())
    {
        if(absl::IsNotFound(value.status()))
            return std::optional<client::Position>{};
        return value.status();
    }
    if(!value->completion.complete)
        return absl::UnavailableError("saved export position read is incomplete");
    if(!value->status.ok())
    {
        if(absl::IsNotFound(value->status))
            return std::optional<client::Position>{};
        return value->status;
    }
    if(!value->value)
        return absl::DataLossError("missing saved export position");
    try
    {
        auto j = Json::parse(value->value->value);
        if(j.at("v") != 1)
            return absl::DataLossError("unknown export position version");
        client::Position p{{j.at("physical_ns").get<int64_t>(), j.at("logical").get<uint32_t>()},
                           {j.at("story_id").get<uint64_t>(),
                            j.at("writer_id").get<uint64_t>(),
                            j.at("incarnation").get<uint64_t>(),
                            j.at("sequence").get<uint64_t>()}};
        if(!p.id.story_id || !p.id.writer_id || !p.id.incarnation || !p.id.sequence)
            return absl::DataLossError("invalid saved EventId");
        return std::optional<client::Position>{p};
    }
    catch(const std::exception& e)
    {
        return absl::DataLossError(e.what());
    }
}
absl::Status Exporter::flush(Batch& batch, ExportStats& stats, client::Deadline deadline)
{
    if(!batch.count())
        return absl::OkStatus();
    auto sent = sink_.post(batch.body(), deadline);
    if(!sent.ok())
        return sent;
    for(const auto& [story, position]: batch.positions())
    {
        auto saved = positions_.put(options_.consumer + "/" + story,
                                    positionJson(position).dump(),
                                    {.metadata = {}, .deadline = bounded(deadline)});
        if(!saved.ok())
            return saved.status();
        if(!saved->acked())
            return absl::FailedPreconditionError("export cursor is not DURABLE");
    }
    stats.events += batch.count();
    ++stats.batches;
    batch.clear();
    return absl::OkStatus();
}
absl::StatusOr<ExportStats> Exporter::run(std::function<bool()> stop, client::Deadline deadline)
{
    if(stories_.empty() || stories_.size() > 64 || options_.batch_count == 0 || options_.batch_count > 10000 ||
       options_.batch_age.count() <= 0 || options_.batch_age > std::chrono::seconds(30) ||
       options_.pull_timeout.count() <= 0 || options_.pull_timeout > std::chrono::seconds(1) ||
       options_.consumer.empty())
        return absl::InvalidArgumentError("invalid exporter bounds");
    struct Source
    {
        StoryId id;
        std::string name;
        std::optional<client::Position> consumed;
        std::unique_ptr<client::TailStream> tail;
    };
    std::vector<Source> sources;
    std::set<std::string> names;
    for(const auto& name: stories_)
    {
        if(!names.insert(name).second)
            return absl::InvalidArgumentError("duplicate export story");
        auto id = findStory(client_, chronicle_, name, false, bounded(deadline));
        if(!id.ok())
            return id.status();
        auto position = saved(name, bounded(deadline));
        if(!position.ok())
            return position.status();
        if(*position && (**position).id.story_id != *id)
            return absl::FailedPreconditionError("saved position belongs to another story");
        sources.push_back({*id, name, *position, {}});
    }
    Batch batch(options_.batch_count, options_.batch_age);
    ExportStats stats;
    while(!stop() && (!deadline || std::chrono::system_clock::now() < *deadline))
    {
        for(auto& source: sources)
        {
            if(stop() || (deadline && std::chrono::system_clock::now() >= *deadline))
                break;
            if(!source.tail)
            {
                auto tail = client_.tail(source.id, source.consumed);
                if(!tail.ok())
                    return tail.status();
                source.tail = std::make_unique<client::TailStream>(std::move(*tail));
            }
            auto pull_end = std::chrono::system_clock::now() + std::min(options_.pull_timeout, options_.batch_age);
            if(deadline)
                pull_end = std::min(pull_end, *deadline);
            auto item = source.tail->next(pull_end);
            if(!item.ok())
            {
                if(!absl::IsDeadlineExceeded(item.status()) && !absl::IsUnavailable(item.status()) &&
                   !absl::IsCancelled(item.status()))
                    return item.status();
                source.tail.reset();
            }
            else if(!*item || (**item).completion)
                source.tail.reset();
            else
                for(const auto& event: (**item).events)
                {
                    if(stop() || (deadline && std::chrono::system_clock::now() >= *deadline))
                        break;
                    if(source.consumed && !after(event, *source.consumed))
                        continue;
                    auto line = lineProtocol(event);
                    if(!line.ok())
                        return line.status();
                    if(batch.full(line->size()))
                    {
                        auto status = flush(batch, stats, bounded(deadline));
                        if(!status.ok())
                            return status;
                    }
                    auto added = batch.add(source.name, {event.hlc, event.id}, std::move(*line));
                    if(!added.ok())
                        return added;
                    source.consumed = client::Position{event.hlc, event.id};
                    if(batch.due(std::chrono::steady_clock::now()))
                    {
                        auto status = flush(batch, stats, bounded(deadline));
                        if(!status.ok())
                            return status;
                    }
                }
            if(batch.due(std::chrono::steady_clock::now()))
            {
                auto status = flush(batch, stats, bounded(deadline));
                if(!status.ok())
                    return status;
            }
        }
    }
    auto drained = flush(batch, stats, bounded({}));
    if(!drained.ok())
        return drained;
    return stats;
}
} // namespace chronolog::stream
