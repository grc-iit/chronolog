#include "chronolog/sql/database.h"
#include <iostream>
int main(int argc, char** argv)
{
    if(argc != 3)
        return 2;
    chronolog::client::ClientOptions options;
    options.catalog_endpoint = argv[1];
    options.player_endpoint = argv[2];
    auto client = chronolog::client::Client::Connect(options);
    if(!client.ok())
        return 1;
    auto name = "sql-example-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    chronolog::sql::Database database(*client, name);
    auto created = database.execute("CREATE TABLE provenance (agent TEXT, step INTEGER, success BOOLEAN)");
    if(!created.ok())
        return 1;
    std::vector<chronolog::sql::Value> rows = {"planner", 1, true, "executor", 2, true};
    auto inserted = database.execute("INSERT INTO provenance VALUES (?, ?, ?), (?, ?, ?)", rows);
    if(!inserted.ok() || inserted->receipts.size() != 2)
        return 1;
    for(const auto& receipt: inserted->receipts)
        if(!receipt.ok() || !receipt->acked())
            return 1;
    std::vector<chronolog::sql::Value> filter = {true};
    auto selected = database.execute(
            "SELECT agent, step, _hlc, _event_id FROM provenance WHERE success = ? ORDER BY TIME DESC LIMIT 2",
            filter);
    if(!selected.ok() || !selected->completion || !selected->completion->complete || selected->rows.size() != 2 ||
       selected->rows[0]["step"] != 2)
        return 1;
    for(const auto& row: selected->rows) std::cout << row.dump() << '\n';
    std::cout << chronolog::sql::completionJson(*selected).dump() << '\n';
    return 0;
}
