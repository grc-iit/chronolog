#include "chronolog/sql/database.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <set>

namespace chronolog::sql
{
namespace
{
constexpr auto schemaType = "application/vnd.chronolog.sql-schema+json";
constexpr auto rowType = "application/vnd.chronolog.sql-row+json";
absl::StatusOr<Completion> snapshot(client::Client& client, StoryId id, client::Deadline deadline)
{
    auto stream = client.read(id, {{}, {}}, deadline);
    if(!stream.ok())
        return stream.status();
    while(std::chrono::system_clock::now() < *deadline)
    {
        auto item = stream->next(deadline);
        if(!item.ok())
            return item.status();
        if(!*item)
            break;
        if((**item).completion)
            return *(**item).completion;
    }
    return absl::DeadlineExceededError("frontier probe missing Replay Completion");
}
std::string schemaPayload(const std::vector<Column>& columns)
{
    Value cols = Value::array();
    for(const auto& c: columns) cols.push_back({{"name", c.name}, {"type", c.type}});
    return Value({{"v", 1}, {"columns", cols}}).dump();
}
bool typed(const Value& v, const std::string& type)
{
    if(v.is_null())
        return true;
    if(type == "integer")
        return v.is_number_integer() &&
               (!v.is_number_unsigned() || v.get<uint64_t>() <= static_cast<uint64_t>(INT64_MAX));
    if(type == "real")
        return v.is_number() && std::isfinite(v.get<double>());
    if(type == "text")
        return v.is_string();
    if(type == "blob")
        return v.is_binary();
    return type == "boolean" && v.is_boolean();
}
Value hidden(const Event& e, const std::string& name)
{
    if(name == "_physical_ns")
        return e.physical.physical_ns;
    if(name == "_hlc")
        return std::to_string(e.hlc.physical_ns) + ":" + std::to_string(e.hlc.logical);
    return std::to_string(e.id.story_id) + ":" + std::to_string(e.id.writer_id) + ":" +
           std::to_string(e.id.incarnation) + ":" + std::to_string(e.id.sequence);
}
bool match(const Value& a, const Predicate& p)
{
    const auto& b = p.value;
    if(a.is_null() || b.is_null())
        return false;
    if(p.op == "=")
        return a == b;
    if(p.op == "!=")
        return a != b;
    if(p.op == "<")
        return a < b;
    if(p.op == ">")
        return a > b;
    if(p.op == "<=")
        return a <= b;
    return a >= b;
}
} // namespace
Database::Database(client::Client& client, std::string chronicle)
    : client_(client)
    , chronicle_(std::move(chronicle))
{
    static std::atomic<uint64_t> serial{};
    identity_ = "sql-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + "-" +
                std::to_string(++serial);
}
absl::StatusOr<StoryId> Database::story(const std::string& table, bool create, client::Deadline deadline)
{
    if(create)
    {
        auto c = client_.createChronicle(chronicle_, deadline);
        if(!c.ok() && !absl::IsAlreadyExists(c.status()))
            return c.status();
        auto s = client_.createStory(chronicle_, table, deadline);
        if(s.ok())
            return s->id;
        if(!absl::IsAlreadyExists(s.status()))
            return s.status();
    }
    auto stories = client_.listStories(chronicle_, deadline);
    if(!stories.ok())
        return stories.status();
    for(const auto& s: *stories)
        if(s.name == table && !s.tombstoned)
            return s.id;
    return absl::NotFoundError("table has no story");
}
absl::StatusOr<std::vector<Column>> Database::schema(StoryId id, client::Deadline deadline)
{
    auto frontier = snapshot(client_, id, deadline);
    if(!frontier.ok())
        return frontier.status();
    if(!frontier->complete)
        return absl::UnavailableError("schema frontier Replay is incomplete");
    auto stream = client_.read(id, {{}, frontier->frontier}, deadline);
    if(!stream.ok())
        return stream.status();
    std::optional<Event> first;
    while(std::chrono::system_clock::now() < *deadline)
    {
        auto item = stream->next(deadline);
        if(!item.ok())
            return item.status();
        if(!*item)
            break;
        for(const auto& e: (**item).events)
            if(!first)
                first = e;
        if((**item).completion)
        {
            if(!(**item).completion->complete)
                return absl::UnavailableError("schema Replay is incomplete");
            if(!first)
                return absl::NotFoundError("schema event absent");
            if(first->envelope.content_type != schemaType)
                return absl::FailedPreconditionError("first event is not a SQL schema");
            try
            {
                auto j = Value::parse(first->envelope.payload);
                if(j.at("v") != 1)
                    return absl::DataLossError("unsupported schema version");
                std::vector<Column> columns;
                for(const auto& c: j.at("columns"))
                    columns.push_back({c.at("name").get<std::string>(), c.at("type").get<std::string>()});
                if(columns.empty() || columns.size() > 256)
                    return absl::DataLossError("invalid schema width");
                return columns;
            }
            catch(const std::exception& e)
            {
                return absl::DataLossError(e.what());
            }
        }
    }
    return absl::DeadlineExceededError("schema read missing Completion");
}
absl::StatusOr<Result> Database::execute(const std::string& sql, std::span<const Value> parameters, Options options)
{
    auto parsed = parse(sql, parameters);
    if(!parsed.ok())
        return parsed.status();
    const auto& s = *parsed;
    auto deadline = options.deadline.value_or(std::chrono::system_clock::now() + std::chrono::seconds(10));
    auto id = story(s.table, s.kind == Statement::Kind::Create, deadline);
    if(!id.ok())
        return id.status();
    Result result;
    if(s.kind == Statement::Kind::Create)
    {
        expected_[s.table] = s.schema;
        auto existing = schema(*id, deadline);
        if(existing.ok())
        {
            schemas_[s.table] = *existing;
            if(*existing != s.schema)
                return absl::AlreadyExistsError("different schema already exists");
            return result;
        }
        if(!absl::IsNotFound(existing.status()))
            return existing.status();
        auto writer = client_.acquire(*id, identity_, deadline);
        if(!writer.ok())
            return writer.status();
        client::AppendSpec spec;
        spec.envelope = options.metadata;
        spec.envelope.content_type = schemaType;
        spec.envelope.payload = schemaPayload(s.schema);
        spec.envelope.attributes["chronolog.sql.schema_version"] = "1";
        auto appended = writer->append(spec, deadline);
        auto released = writer->release(deadline);
        (void)released;
        if(!appended.ok())
            return appended.status();
        auto winner = schema(*id, deadline);
        if(!winner.ok())
            return winner.status();
        schemas_[s.table] = *winner;
        if(*winner != s.schema)
            return absl::AlreadyExistsError("concurrent CREATE schema lost");
        result.receipts.emplace_back(*appended);
        return result;
    }
    std::vector<Column> columns;
    if(s.kind == Statement::Kind::Select && schemas_.contains(s.table))
        columns = schemas_.at(s.table);
    else
    {
        auto loaded = schema(*id, deadline);
        if(!loaded.ok())
            return loaded.status();
        columns = *loaded;
        schemas_[s.table] = columns;
    }
    if(s.kind == Statement::Kind::Insert && expected_.contains(s.table) && expected_.at(s.table) != columns)
        return absl::FailedPreconditionError("INSERT uses losing CREATE schema");
    auto index = [&](const std::string& name) -> std::optional<size_t>
    {
        for(size_t i = 0; i < columns.size(); ++i)
            if(columns[i].name == name)
                return i;
        return {};
    };
    if(s.kind == Statement::Kind::Insert)
    {
        std::vector<std::string> names = s.columns;
        if(names.empty())
            for(const auto& c: columns) names.push_back(c.name);
        std::set<std::string> unique;
        for(const auto& name: names)
            if(!index(name) || !unique.insert(name).second)
                return absl::InvalidArgumentError("invalid column token '" + name + "'");
        std::vector<client::AppendSpec> batch;
        for(const auto& tuple: s.tuples)
        {
            if(tuple.size() != names.size())
                return absl::InvalidArgumentError("token VALUES width differs from columns");
            std::vector<Value> values(columns.size(), nullptr);
            for(size_t i = 0; i < tuple.size(); ++i)
            {
                auto col = *index(names[i]);
                if(!typed(tuple[i], columns[col].type))
                    return absl::InvalidArgumentError("type mismatch token '" + names[i] + "'");
                values[col] = tuple[i];
                if(columns[col].type == "real" && !values[col].is_null())
                    values[col] = values[col].get<double>();
            }
            client::AppendSpec spec;
            spec.envelope = options.metadata;
            spec.envelope.content_type = rowType;
            spec.envelope.payload = encodeRow(values);
            spec.envelope.attributes["chronolog.sql.schema_version"] = "1";
            spec.envelope.attributes["chronolog.sql.schema"] = schemaPayload(columns);
            batch.push_back(std::move(spec));
        }
        auto writer = client_.acquire(*id, identity_, deadline);
        if(!writer.ok())
            return writer.status();
        auto receipts = writer->appendBatch(batch, deadline);
        auto released = writer->release(deadline);
        (void)released;
        if(!receipts.ok())
            return receipts.status();
        result.receipts = std::move(*receipts);
        return result;
    }
    auto names = s.columns;
    if(names.empty())
        for(const auto& c: columns) names.push_back(c.name);
    for(const auto& name: names)
        if(!index(name) && name != "_hlc" && name != "_physical_ns" && name != "_event_id")
            return absl::InvalidArgumentError("unknown column token '" + name + "'");
    for(const auto& p: s.predicates)
    {
        auto col = index(p.column);
        if(!col)
            return absl::InvalidArgumentError("unknown predicate token '" + p.column + "'");
        if(!typed(p.value, columns[*col].type))
            return absl::InvalidArgumentError("type mismatch token '" + p.column + "'");
        if(p.op != "=" && p.op != "!=" && columns[*col].type != "integer" && columns[*col].type != "real" &&
           columns[*col].type != "text")
            return absl::InvalidArgumentError("unsupported comparison token '" + p.op + "'");
    }
    if(s.limit == 0)
    {
        result.limited = true;
        return result;
    }
    std::optional<Completion> frontier;
    client::HlcRange range;
    if(s.range)
        range = *s.range;
    else if(s.physical_range) {}
    else
    {
        auto probe = snapshot(client_, *id, deadline);
        if(!probe.ok())
            return probe.status();
        frontier = *probe;
        range = {{}, probe->frontier};
    }
    auto stream = s.physical_range ? client_.readPhysical(*id, *s.physical_range, deadline)
                                   : client_.read(*id, range, deadline);
    if(!stream.ok())
        return stream.status();
    struct Selected
    {
        Event event;
        Value row;
    };
    std::vector<Selected> selected;
    auto less = [&](const Selected& a, const Selected& b)
    {
        if(s.order == "_physical_ns")
        {
            if(a.event.physical.physical_ns != b.event.physical.physical_ns)
                return a.event.physical.physical_ns < b.event.physical.physical_ns;
        }
        else if(s.order == "_event_id")
            return a.event.id < b.event.id;
        return ReplayLess(a.event, b.event);
    };
    size_t count{}, selectedBytes{};
    bool orderedBuffer = s.descending || s.order == "_physical_ns" || s.order == "_event_id";
    while(std::chrono::system_clock::now() < deadline)
    {
        auto item = stream->next(deadline);
        if(!item.ok())
            return item.status();
        if(!*item)
            break;
        for(const auto& e: (**item).events)
        {
            if(e.envelope.content_type == schemaType)
                continue;
            if(e.envelope.content_type != rowType)
                return absl::DataLossError("unexpected SQL row content type");
            auto version = e.envelope.attributes.find("chronolog.sql.schema_version");
            auto fingerprint = e.envelope.attributes.find("chronolog.sql.schema");
            if(version == e.envelope.attributes.end() || version->second != "1" ||
               fingerprint == e.envelope.attributes.end() || fingerprint->second != schemaPayload(columns))
                return absl::FailedPreconditionError("row schema differs from first schema");
            auto values = decodeRow(e.envelope.payload);
            if(!values.ok())
                return values.status();
            if(values->size() != columns.size())
                return absl::DataLossError("row width differs from schema");
            bool matches = true;
            for(const auto& p: s.predicates) matches = matches && match((*values)[*index(p.column)], p);
            if(!matches)
                continue;
            ++count;
            if(!s.count)
            {
                Value row = Value::object();
                for(const auto& name: names)
                {
                    auto col = index(name);
                    row[name] = col ? (*values)[*col] : hidden(e, name);
                }
                if(orderedBuffer && s.limit)
                {
                    Event metadata;
                    metadata.id = e.id;
                    metadata.hlc = e.hlc;
                    metadata.physical = e.physical;
                    Selected candidate{std::move(metadata), std::move(row)};
                    auto best = [&](const Selected& a, const Selected& b)
                    { return s.descending ? less(b, a) : less(a, b); };
                    auto place = std::lower_bound(selected.begin(), selected.end(), candidate, best);
                    if(selected.size() < *s.limit)
                    {
                        selectedBytes += candidate.row.dump().size();
                        selected.insert(place, std::move(candidate));
                    }
                    else if(place != selected.end())
                    {
                        selectedBytes -= selected.back().row.dump().size();
                        selectedBytes += candidate.row.dump().size();
                        selected.pop_back();
                        place = std::lower_bound(selected.begin(), selected.end(), candidate, best);
                        selected.insert(place, std::move(candidate));
                    }
                }
                else
                {
                    if(selected.size() >= 10000)
                        return absl::ResourceExhaustedError("SELECT exceeds 10000 rows; use LIMIT");
                    selectedBytes += row.dump().size();
                    Event metadata;
                    metadata.id = e.id;
                    metadata.hlc = e.hlc;
                    metadata.physical = e.physical;
                    selected.push_back({std::move(metadata), std::move(row)});
                }
            }
            if(selectedBytes > (64u << 20))
                return absl::ResourceExhaustedError("SELECT exceeds 64 MiB; reduce LIMIT");
            if(s.limit && !orderedBuffer && !s.count && count >= *s.limit)
            {
                stream->cancel();
                result.limited = true;
                for(auto& r: selected) result.rows.push_back(std::move(r.row));
                return result;
            }
        }
        if((**item).completion)
        {
            result.completion = (**item).completion;
            break;
        }
    }
    if(!result.completion)
        return absl::DeadlineExceededError("SELECT missing Replay Completion");
    if(frontier && !frontier->complete)
        result.completion = *frontier;
    if(s.count)
        result.rows.push_back({{"count", count}});
    else
    {
        if(orderedBuffer && !s.limit)
            std::sort(selected.begin(), selected.end(), less);
        for(auto& r: selected) result.rows.push_back(std::move(r.row));
    }
    return result;
}
Value completionJson(const Result& result)
{
    Value out = {{"limited", result.limited}, {"completion", nullptr}};
    if(result.completion)
    {
        Value laggards = Value::array();
        for(const auto& l: result.completion->laggards)
            laggards.push_back(
                    {{"writer_id", l.writer_id},
                     {"incarnation", l.incarnation},
                     {"frontier", {{"physical_ns", l.frontier.physical_ns}, {"logical", l.frontier.logical}}}});
        out["completion"] = {{"complete", result.completion->complete},
                             {"reason", static_cast<int>(result.completion->reason)},
                             {"frontier",
                              {{"physical_ns", result.completion->frontier.physical_ns},
                               {"logical", result.completion->frontier.logical}}},
                             {"laggards", laggards}};
    }
    return out;
}
} // namespace chronolog::sql
