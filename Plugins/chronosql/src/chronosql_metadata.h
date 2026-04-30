#ifndef CHRONOSQL_METADATA_H_
#define CHRONOSQL_METADATA_H_

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "chronosql_types.h"

namespace chronosql
{

/**
 * @brief Encode/decode metadata operations stored in the reserved
 * `__chronosql_metadata` story.
 *
 * Each metadata event is a JSON object describing exactly one schema
 * operation (CREATE TABLE, DROP TABLE). Replaying the events in timestamp
 * order reconstructs the current schema/table-list state.
 */
struct MetaCreateOp
{
    std::string table;
    std::vector<Column> columns;
};

struct MetaDropOp
{
    std::string table;
};

std::string encodeCreateOp(const MetaCreateOp& op);
std::string encodeDropOp(const MetaDropOp& op);

/**
 * @brief In-memory metadata state: known tables and their schemas, in CREATE
 * order. dropTable() removes a table from the live set.
 */
class MetadataState
{
public:
    /// Apply a single metadata payload (encoded JSON). Unknown ops are ignored
    /// to keep forward compatibility cheap.
    void apply(const std::string& payload);

    bool has(const std::string& table) const;
    std::optional<Schema> get(const std::string& table) const;
    std::vector<std::string> listTables() const;
    void clear();

    void recordCreate(const MetaCreateOp& op);
    void recordDrop(const MetaDropOp& op);

private:
    std::vector<std::string> order_;        // CREATE-time insertion order
    std::map<std::string, Schema> schemas_; // live tables
};

} // namespace chronosql

#endif // CHRONOSQL_METADATA_H_
