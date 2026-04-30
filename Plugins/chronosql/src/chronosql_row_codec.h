#ifndef CHRONOSQL_ROW_CODEC_H_
#define CHRONOSQL_ROW_CODEC_H_

#include <map>
#include <string>

namespace chronosql
{

/**
 * @brief Encode a row's column-value map as a compact JSON object.
 *
 * Values are always serialized as JSON strings; ChronoSQL keeps stored values
 * as strings regardless of the schema's declared column type. NULL columns
 * (absent from the map) are omitted from the JSON output.
 */
std::string encodeRow(const std::map<std::string, std::string>& row);

/**
 * @brief Decode a JSON object emitted by encodeRow back into a column-value
 * map. Non-string JSON values are stringified.
 *
 * @throws std::runtime_error on invalid JSON or non-object root.
 */
std::map<std::string, std::string> decodeRow(const std::string& payload);

} // namespace chronosql

#endif // CHRONOSQL_ROW_CODEC_H_
