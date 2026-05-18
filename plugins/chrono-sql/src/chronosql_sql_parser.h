#ifndef CHRONOSQL_SQL_PARSER_H_
#define CHRONOSQL_SQL_PARSER_H_

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "chronosql_types.h"

namespace chronosql
{

/**
 * @brief Parsed AST for the SQL subset accepted by ChronoSQL::execute().
 *
 * The grammar is intentionally small and targeted at the YCSB workload shape.
 * See ChronoSQL::execute() in chronosql.h for the supported productions.
 */

struct ParsedCreateTable
{
    std::string table;
    std::vector<Column> columns;
};

struct ParsedDropTable
{
    std::string table;
};

struct ParsedInsert
{
    std::string table;
    std::vector<std::string> columns; // empty means "use schema order"
    std::vector<std::string> values;  // string-form of each literal (NULL → "")
    std::vector<bool> is_null;        // parallel to values
};

struct WhereEq
{
    std::string column;
    std::string value;
    bool is_null{false};
};

struct WhereTsBetween
{
    std::uint64_t lo{0};
    std::uint64_t hi{0}; // inclusive
};

struct ParsedSelect
{
    std::string table;
    std::vector<std::string> projection; // empty means SELECT *
    std::variant<std::monostate, WhereEq, WhereTsBetween> where;
};

using ParsedStatement = std::variant<ParsedCreateTable, ParsedDropTable, ParsedInsert, ParsedSelect>;

/**
 * @brief Parse a single SQL statement.
 * @throws std::invalid_argument with a descriptive message on parse failure.
 */
ParsedStatement parseStatement(const std::string& sql);

} // namespace chronosql

#endif // CHRONOSQL_SQL_PARSER_H_
