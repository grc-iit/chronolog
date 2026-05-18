#ifndef CHRONOSQL_MAPPER_H_
#define CHRONOSQL_MAPPER_H_

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "chronosql_client_adapter.h"
#include "chronosql_metadata.h"
#include "chronosql_logger.h"
#include "chronosql_types.h"

namespace chronosql
{

/**
 * @brief Translates SQL-shaped operations (table-level CRUD) into ChronoLog
 * events on top of the ChronoSQLClientAdapter.
 *
 * Owns the in-memory MetadataState and is responsible for keeping it in sync
 * with the metadata story.
 */
class ChronoSQLMapper
{
public:
    explicit ChronoSQLMapper(LogLevel level);
    explicit ChronoSQLMapper(const std::string& config_path, LogLevel level);
    ~ChronoSQLMapper() = default;

    void createTable(const std::string& name, const std::vector<Column>& columns);
    void dropTable(const std::string& name);
    std::vector<std::string> listTables();
    std::optional<Schema> getSchema(const std::string& name);

    std::uint64_t insert(const std::string& table, const std::map<std::string, std::string>& row);

    std::vector<Row> selectByKey(const std::string& table, const std::string& column, const std::string& value);
    std::vector<Row> selectRange(const std::string& table, std::uint64_t start_ts, std::uint64_t end_ts);
    std::vector<Row> selectAll(const std::string& table);

    void flush();

private:
    LogLevel logLevel_;
    std::unique_ptr<ChronoSQLClientAdapter> adapter_;
    MetadataState metadata_;
    bool metadata_loaded_{false};

    static constexpr const char* kMetadataStory = "__chronosql_metadata";
    static constexpr const char* kDefaultChronicle = "ChronoSQLChronicle";

    void ensureMetadataLoaded();
    void appendMetadata(const std::string& payload);
    void requireSchema(const std::string& table) const;
    Row eventToRow(std::uint64_t ts, const std::string& payload) const;
    void validateColumnsAgainstSchema(const Schema& schema, const std::map<std::string, std::string>& row) const;
};

} // namespace chronosql

#endif // CHRONOSQL_MAPPER_H_
