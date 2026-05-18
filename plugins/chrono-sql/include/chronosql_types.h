#ifndef CHRONOSQL_TYPES_H_
#define CHRONOSQL_TYPES_H_

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace chronosql
{

/**
 * @brief Declared column types. Stored values are always strings; types are
 * advisory metadata that the engine records on CREATE TABLE and surfaces back
 * to the caller. v0.1 does not enforce types at INSERT time beyond column-name
 * validation.
 */
enum class ColumnType : std::uint8_t
{
    INT,
    DOUBLE,
    STRING,
    BOOL
};

const char* columnTypeToString(ColumnType t);
std::optional<ColumnType> columnTypeFromString(const std::string& s);

struct Column
{
    std::string name;
    ColumnType type{ColumnType::STRING};
};

/**
 * @brief Schema for a single table: ordered list of columns.
 *
 * Columns named with the reserved `__` prefix are not allowed. The implicit
 * `__ts` pseudo-column (event timestamp) is always available for filtering
 * but is never part of the persisted row payload.
 */
struct Schema
{
    std::string table;
    std::vector<Column> columns;
};

/**
 * @brief A single row materialized from a table.
 *
 * `timestamp` is the ChronoLog event timestamp (the implicit `__ts` column).
 * `values` holds string-form column values keyed by column name; columns
 * absent from the map are treated as NULL.
 */
struct Row
{
    std::uint64_t timestamp{0};
    std::map<std::string, std::string> values;
};

/**
 * @brief Result of executing a SQL statement via execute().
 *
 * For SELECT, `columns` is the projected column order and `rows` holds the
 * matching rows. For INSERT, `last_insert_timestamp` is the event timestamp
 * assigned by ChronoLog. For CREATE/DROP TABLE, the result carries no rows.
 */
struct ResultSet
{
    std::vector<std::string> columns;
    std::vector<Row> rows;
    std::optional<std::uint64_t> last_insert_timestamp;
};

} // namespace chronosql

#endif // CHRONOSQL_TYPES_H_
