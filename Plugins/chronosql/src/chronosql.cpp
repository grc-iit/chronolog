#include "chronosql.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <stdexcept>
#include <variant>

#include "chronosql_mapper.h"
#include "chronosql_sql_parser.h"

namespace chronosql
{

const char* columnTypeToString(ColumnType t)
{
    switch(t)
    {
        case ColumnType::INT:
            return "INT";
        case ColumnType::DOUBLE:
            return "DOUBLE";
        case ColumnType::STRING:
            return "STRING";
        case ColumnType::BOOL:
            return "BOOL";
    }
    return "STRING";
}

std::optional<ColumnType> columnTypeFromString(const std::string& s)
{
    std::string up;
    up.reserve(s.size());
    for(char c: s) { up += static_cast<char>(std::toupper(static_cast<unsigned char>(c))); }
    if(up == "INT" || up == "INTEGER" || up == "BIGINT")
    {
        return ColumnType::INT;
    }
    if(up == "DOUBLE" || up == "FLOAT" || up == "REAL")
    {
        return ColumnType::DOUBLE;
    }
    if(up == "STRING" || up == "TEXT" || up == "VARCHAR" || up == "CHAR")
    {
        return ColumnType::STRING;
    }
    if(up == "BOOL" || up == "BOOLEAN")
    {
        return ColumnType::BOOL;
    }
    return std::nullopt;
}

ChronoSQL::ChronoSQL(LogLevel level)
    : mapper(std::make_unique<ChronoSQLMapper>(level))
    , logLevel_(level)
{}

ChronoSQL::ChronoSQL(const std::string& config_path, LogLevel level)
    : mapper(std::make_unique<ChronoSQLMapper>(config_path, level))
    , logLevel_(level)
{}

ChronoSQL::~ChronoSQL() = default;

void ChronoSQL::createTable(const std::string& name, const std::vector<Column>& columns)
{
    mapper->createTable(name, columns);
}

void ChronoSQL::dropTable(const std::string& name) { mapper->dropTable(name); }

std::vector<std::string> ChronoSQL::listTables() { return mapper->listTables(); }

std::optional<Schema> ChronoSQL::getSchema(const std::string& table) { return mapper->getSchema(table); }

std::uint64_t ChronoSQL::insert(const std::string& table, const std::map<std::string, std::string>& row)
{
    return mapper->insert(table, row);
}

std::vector<Row> ChronoSQL::selectByKey(const std::string& table, const std::string& column, const std::string& value)
{
    return mapper->selectByKey(table, column, value);
}

std::vector<Row> ChronoSQL::selectRange(const std::string& table, std::uint64_t start_ts, std::uint64_t end_ts)
{
    return mapper->selectRange(table, start_ts, end_ts);
}

std::vector<Row> ChronoSQL::selectAll(const std::string& table) { return mapper->selectAll(table); }

void ChronoSQL::flush() { mapper->flush(); }

namespace
{

std::vector<std::string> projectionColumns(const ParsedSelect& sel, const Schema& schema)
{
    if(sel.projection.empty())
    {
        std::vector<std::string> all;
        all.reserve(schema.columns.size());
        for(const auto& c: schema.columns) { all.push_back(c.name); }
        return all;
    }
    return sel.projection;
}

std::vector<Row> projectRows(const std::vector<std::string>& cols, std::vector<Row>&& rows)
{
    bool wants_ts = std::find(cols.begin(), cols.end(), "__ts") != cols.end();
    for(auto& r: rows)
    {
        std::map<std::string, std::string> kept;
        for(const auto& c: cols)
        {
            if(c == "__ts")
            {
                continue; // already on Row::timestamp
            }
            auto it = r.values.find(c);
            if(it != r.values.end())
            {
                kept.emplace(c, it->second);
            }
        }
        r.values = std::move(kept);
        (void)wants_ts;
    }
    return std::move(rows);
}

} // namespace

ResultSet ChronoSQL::execute(const std::string& sql)
{
    ParsedStatement stmt = parseStatement(sql);
    ResultSet result;

    if(auto* p = std::get_if<ParsedCreateTable>(&stmt))
    {
        mapper->createTable(p->table, p->columns);
        return result;
    }
    if(auto* p = std::get_if<ParsedDropTable>(&stmt))
    {
        mapper->dropTable(p->table);
        return result;
    }
    if(auto* p = std::get_if<ParsedInsert>(&stmt))
    {
        auto schema = mapper->getSchema(p->table);
        if(!schema)
        {
            throw std::runtime_error("ChronoSQL: cannot insert into unknown table '" + p->table + "'");
        }
        std::vector<std::string> col_names;
        if(p->columns.empty())
        {
            for(const auto& c: schema->columns) { col_names.push_back(c.name); }
        }
        else
        {
            col_names = p->columns;
        }
        if(col_names.size() != p->values.size())
        {
            throw std::invalid_argument("ChronoSQL: INSERT column count does not match value count");
        }
        std::map<std::string, std::string> row;
        for(std::size_t i = 0; i < col_names.size(); ++i)
        {
            if(p->is_null[i])
            {
                continue; // NULL → omit column
            }
            row.emplace(col_names[i], p->values[i]);
        }
        std::uint64_t ts = mapper->insert(p->table, row);
        result.last_insert_timestamp = ts;
        return result;
    }
    if(auto* p = std::get_if<ParsedSelect>(&stmt))
    {
        auto schema = mapper->getSchema(p->table);
        if(!schema)
        {
            throw std::runtime_error("ChronoSQL: cannot SELECT from unknown table '" + p->table + "'");
        }
        std::vector<Row> rows;
        if(std::holds_alternative<WhereTsBetween>(p->where))
        {
            auto w = std::get<WhereTsBetween>(p->where);
            // BETWEEN is inclusive in SQL; ReplayStory range is half-open.
            std::uint64_t end = (w.hi == UINT64_MAX) ? UINT64_MAX : w.hi + 1;
            rows = mapper->selectRange(p->table, w.lo, end);
        }
        else if(std::holds_alternative<WhereEq>(p->where))
        {
            auto w = std::get<WhereEq>(p->where);
            if(w.is_null)
            {
                rows = mapper->selectAll(p->table);
                rows.erase(std::remove_if(rows.begin(),
                                          rows.end(),
                                          [&](const Row& r) { return r.values.find(w.column) != r.values.end(); }),
                           rows.end());
            }
            else
            {
                rows = mapper->selectByKey(p->table, w.column, w.value);
            }
        }
        else
        {
            rows = mapper->selectAll(p->table);
        }
        result.columns = projectionColumns(*p, *schema);
        result.rows = projectRows(result.columns, std::move(rows));
        return result;
    }

    throw std::invalid_argument("ChronoSQL: unsupported statement");
}

} // namespace chronosql
