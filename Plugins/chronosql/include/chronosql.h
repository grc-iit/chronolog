#ifndef CHRONOSQL_H_
#define CHRONOSQL_H_

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "chronosql_logger.h"
#include "chronosql_types.h"

namespace chronosql
{

class ChronoSQLMapper;

/**
 * @brief SQL-style facade over a single ChronoLog chronicle.
 *
 * A ChronoSQL "database" is one chronicle. Each table is one story inside that
 * chronicle, and each row is one event whose `log_record` is a JSON object of
 * column values. A reserved metadata story persists CREATE/DROP TABLE
 * operations so the schema and table list are recoverable across processes.
 *
 * The implicit `__ts` pseudo-column exposes the ChronoLog event timestamp and
 * can be used in time-range queries (e.g. WHERE __ts BETWEEN a AND b).
 */
class ChronoSQL
{
private:
    std::unique_ptr<ChronoSQLMapper> mapper;
    LogLevel logLevel_;

public:
    /**
     * @brief Construct a ChronoSQL instance using built-in defaults.
     *
     * Uses the localhost ChronoLog client defaults and connects to the default
     * chronicle.
     */
    explicit ChronoSQL(LogLevel level = getDefaultLogLevel());

    /**
     * @brief Construct a ChronoSQL instance loading a ChronoLog client config.
     *
     * @param config_path
     *     Path to a ChronoLog client configuration JSON file. Empty means
     *     "use defaults" (same as the no-arg constructor).
     * @param level
     *     The logging level.
     *
     * @throws std::runtime_error if the config file cannot be loaded or the
     *     ChronoLog client fails to connect.
     */
    explicit ChronoSQL(const std::string& config_path, LogLevel level = getDefaultLogLevel());

    ~ChronoSQL();

    LogLevel getLogLevel() const { return logLevel_; }

    // ---- Programmatic engine API ------------------------------------------

    /**
     * @brief Create a new table with the given schema.
     *
     * @throws std::invalid_argument on empty/duplicate columns or reserved
     *     column names (any name starting with `__`).
     * @throws std::runtime_error if the table already exists or the metadata
     *     append fails.
     */
    void createTable(const std::string& name, const std::vector<Column>& columns);

    /**
     * @brief Drop a table. The underlying story is not destroyed; only the
     * metadata mapping is removed (an event log is append-only).
     *
     * @throws std::runtime_error if the table does not exist.
     */
    void dropTable(const std::string& name);

    /// List all currently known table names (in CREATE order).
    std::vector<std::string> listTables();

    /// Return the schema for a table, or std::nullopt if unknown.
    std::optional<Schema> getSchema(const std::string& table);

    /**
     * @brief Insert a row into @p table. Column names not in the schema are
     * rejected. Missing columns are treated as NULL (omitted from the
     * persisted JSON).
     *
     * @return The ChronoLog event timestamp assigned to this row.
     */
    std::uint64_t insert(const std::string& table, const std::map<std::string, std::string>& row);

    /// Equality lookup on an arbitrary column. O(events) — no auxiliary index.
    std::vector<Row> selectByKey(const std::string& table, const std::string& column, const std::string& value);

    /// Time-range scan on the implicit `__ts` column.
    /// Returns rows with `__ts` in [start_ts, end_ts).
    std::vector<Row> selectRange(const std::string& table, std::uint64_t start_ts, std::uint64_t end_ts);

    /// Full-table scan.
    std::vector<Row> selectAll(const std::string& table);

    // ---- SQL string facade ------------------------------------------------

    /**
     * @brief Parse and execute a single SQL statement.
     *
     * Supported grammar (case-insensitive keywords):
     *   - CREATE TABLE name (col [type], col [type], ...)
     *   - DROP TABLE name
     *   - INSERT INTO name [(col, col, ...)] VALUES (v, v, ...)
     *   - SELECT * | col, ... FROM name
     *         [WHERE col = value | WHERE __ts BETWEEN n AND n]
     *
     * Types: INT | DOUBLE | STRING | BOOL (synonyms: VARCHAR, TEXT → STRING).
     *
     * @throws std::invalid_argument on parse errors.
     * @throws std::runtime_error on execution errors.
     */
    ResultSet execute(const std::string& sql);

    /**
     * @brief Flush all cached write story handles, releasing them so writes
     * become visible to subsequent reads. Equivalent to the chronokvs flush().
     */
    void flush();
};

} // namespace chronosql

#endif // CHRONOSQL_H_
