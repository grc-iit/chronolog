#include "chronosql_mapper.h"

#include <algorithm>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <utility>

#include "chronosql_row_codec.h"

namespace chronosql
{

namespace
{
constexpr std::uint64_t MIN_TS = 1;
constexpr std::uint64_t MAX_TS = UINT64_MAX;

bool isReservedName(const std::string& s) { return s.size() >= 2 && s[0] == '_' && s[1] == '_'; }
} // namespace

ChronoSQLMapper::ChronoSQLMapper(LogLevel level)
    : logLevel_(level)
    , adapter_(std::make_unique<ChronoSQLClientAdapter>(kDefaultChronicle, level))
{}

ChronoSQLMapper::ChronoSQLMapper(const std::string& config_path, LogLevel level)
    : logLevel_(level)
    , adapter_(std::make_unique<ChronoSQLClientAdapter>(kDefaultChronicle, config_path, level))
{}

void ChronoSQLMapper::ensureMetadataLoaded()
{
    if(metadata_loaded_)
    {
        return;
    }
    auto events = adapter_->replayEvents(kMetadataStory, MIN_TS, MAX_TS, /*tolerate_timeout=*/true);
    std::sort(events.begin(),
              events.end(),
              [](const ChronoSQLClientAdapter::EventPayload& a, const ChronoSQLClientAdapter::EventPayload& b)
              { return a.timestamp < b.timestamp; });
    for(const auto& e: events) { metadata_.apply(e.payload); }
    metadata_loaded_ = true;
}

void ChronoSQLMapper::appendMetadata(const std::string& payload) { adapter_->appendEvent(kMetadataStory, payload); }

void ChronoSQLMapper::requireSchema(const std::string& table) const
{
    auto s = metadata_.get(table);
    if(!s)
    {
        throw std::runtime_error("ChronoSQL: unknown table '" + table + "'");
    }
}

void ChronoSQLMapper::createTable(const std::string& name, const std::vector<Column>& columns)
{
    if(name.empty())
    {
        throw std::invalid_argument("ChronoSQL: table name cannot be empty");
    }
    if(isReservedName(name))
    {
        throw std::invalid_argument("ChronoSQL: table names cannot start with '__'");
    }
    if(columns.empty())
    {
        throw std::invalid_argument("ChronoSQL: table must declare at least one column");
    }
    std::set<std::string> seen;
    for(const auto& c: columns)
    {
        if(c.name.empty())
        {
            throw std::invalid_argument("ChronoSQL: column name cannot be empty");
        }
        if(isReservedName(c.name))
        {
            throw std::invalid_argument("ChronoSQL: column names cannot start with '__' (reserved)");
        }
        if(!seen.insert(c.name).second)
        {
            throw std::invalid_argument("ChronoSQL: duplicate column '" + c.name + "'");
        }
    }

    ensureMetadataLoaded();
    if(metadata_.has(name))
    {
        throw std::runtime_error("ChronoSQL: table '" + name + "' already exists");
    }

    MetaCreateOp op{name, columns};
    appendMetadata(encodeCreateOp(op));
    metadata_.recordCreate(op);
    CHRONOSQL_INFO(logLevel_, "Created table", name, "with", columns.size(), "columns");
}

void ChronoSQLMapper::dropTable(const std::string& name)
{
    ensureMetadataLoaded();
    if(!metadata_.has(name))
    {
        throw std::runtime_error("ChronoSQL: cannot drop unknown table '" + name + "'");
    }
    appendMetadata(encodeDropOp(MetaDropOp{name}));
    metadata_.recordDrop(MetaDropOp{name});
    CHRONOSQL_INFO(logLevel_, "Dropped table", name);
}

std::vector<std::string> ChronoSQLMapper::listTables()
{
    ensureMetadataLoaded();
    return metadata_.listTables();
}

std::optional<Schema> ChronoSQLMapper::getSchema(const std::string& name)
{
    ensureMetadataLoaded();
    return metadata_.get(name);
}

void ChronoSQLMapper::validateColumnsAgainstSchema(const Schema& schema,
                                                   const std::map<std::string, std::string>& row) const
{
    std::set<std::string> declared;
    for(const auto& c: schema.columns) { declared.insert(c.name); }
    for(const auto& [k, v]: row)
    {
        (void)v;
        if(declared.find(k) == declared.end())
        {
            throw std::invalid_argument("ChronoSQL: unknown column '" + k + "' for table '" + schema.table + "'");
        }
    }
}

std::uint64_t ChronoSQLMapper::insert(const std::string& table, const std::map<std::string, std::string>& row)
{
    ensureMetadataLoaded();
    auto schema = metadata_.get(table);
    if(!schema)
    {
        throw std::runtime_error("ChronoSQL: cannot insert into unknown table '" + table + "'");
    }
    validateColumnsAgainstSchema(*schema, row);
    std::string payload = encodeRow(row);
    return adapter_->appendEvent(table, payload);
}

Row ChronoSQLMapper::eventToRow(std::uint64_t ts, const std::string& payload) const
{
    Row r;
    r.timestamp = ts;
    r.values = decodeRow(payload);
    return r;
}

std::vector<Row> ChronoSQLMapper::selectAll(const std::string& table)
{
    ensureMetadataLoaded();
    requireSchema(table);
    auto events = adapter_->replayEvents(table, MIN_TS, MAX_TS);
    std::vector<Row> out;
    out.reserve(events.size());
    for(const auto& e: events)
    {
        try
        {
            out.push_back(eventToRow(e.timestamp, e.payload));
        }
        catch(const std::exception& ex)
        {
            CHRONOSQL_WARNING(logLevel_, "Skipping malformed event in table", table, "ts=", e.timestamp);
        }
    }
    return out;
}

std::vector<Row> ChronoSQLMapper::selectRange(const std::string& table, std::uint64_t start_ts, std::uint64_t end_ts)
{
    if(start_ts >= end_ts)
    {
        throw std::invalid_argument("ChronoSQL: invalid range, start_ts must be < end_ts");
    }
    ensureMetadataLoaded();
    requireSchema(table);
    auto events = adapter_->replayEvents(table, start_ts, end_ts);
    std::vector<Row> out;
    out.reserve(events.size());
    for(const auto& e: events)
    {
        try
        {
            out.push_back(eventToRow(e.timestamp, e.payload));
        }
        catch(const std::exception&)
        {
            CHRONOSQL_WARNING(logLevel_, "Skipping malformed event in table", table, "ts=", e.timestamp);
        }
    }
    return out;
}

std::vector<Row>
ChronoSQLMapper::selectByKey(const std::string& table, const std::string& column, const std::string& value)
{
    auto rows = selectAll(table);
    rows.erase(std::remove_if(rows.begin(),
                              rows.end(),
                              [&](const Row& r)
                              {
                                  auto it = r.values.find(column);
                                  return it == r.values.end() || it->second != value;
                              }),
               rows.end());
    return rows;
}

void ChronoSQLMapper::flush() { adapter_->flush(); }

} // namespace chronosql
