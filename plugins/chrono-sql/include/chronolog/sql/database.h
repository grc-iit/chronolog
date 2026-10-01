#pragma once
#include "chronolog/client/client.h"
#include <nlohmann/json.hpp>

namespace chronolog::sql
{
using Value = nlohmann::json;
struct Column
{
    std::string name, type;
    auto operator<=>(const Column&) const = default;
};
struct Predicate
{
    std::string column, op;
    Value value;
};
struct Statement
{
    enum class Kind
    {
        Create,
        Insert,
        Select
    } kind;
    std::string table;
    std::vector<Column> schema;
    std::vector<std::string> columns;
    std::vector<std::vector<Value>> tuples;
    std::vector<Predicate> predicates;
    std::optional<client::HlcRange> range;
    std::string order{"time"};
    bool count{}, descending{};
    std::optional<size_t> limit;
};
absl::StatusOr<Statement> parse(const std::string&, std::span<const Value> parameters = {});
std::string encodeRow(const std::vector<Value>&);
absl::StatusOr<std::vector<Value>> decodeRow(const std::string&);
struct Result
{
    std::vector<Value> rows;
    client::BatchResult receipts;
    std::optional<Completion> completion;
    bool limited{};
};
struct Options
{
    client::Deadline deadline;
    Envelope metadata;
};
class Database
{
public:
    Database(client::Client& client, std::string chronicle);
    absl::StatusOr<Result> execute(const std::string&, std::span<const Value> parameters = {}, Options = {});

private:
    client::Client& client_;
    std::string chronicle_, identity_;
    std::map<std::string, std::vector<Column>> expected_, schemas_;
    absl::StatusOr<StoryId> story(const std::string&, bool, client::Deadline);
    absl::StatusOr<std::vector<Column>> schema(StoryId, client::Deadline);
};
Value completionJson(const Result&);
} // namespace chronolog::sql
