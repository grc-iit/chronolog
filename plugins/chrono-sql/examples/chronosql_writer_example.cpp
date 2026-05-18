#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

#include <cmd_arg_parse.h>

#include "chronosql.h"

int main(int argc, char** argv)
{
    // Optional ChronoLog client config file (-c/--config). Same convention as
    // the ChronoKVS examples; when omitted, ChronoSQL uses the localhost
    // defaults.
    std::string conf_file_path = parse_conf_path_arg(argc, argv);

    auto db = conf_file_path.empty() ? chronosql::ChronoSQL::Create() : chronosql::ChronoSQL::Create(conf_file_path);
    if(!db)
    {
        std::cerr << "Failed to initialize ChronoSQL\n";
        return EXIT_FAILURE;
    }

    // Programmatic API path: create a small "users" table.
    if(!db->getSchema("users").has_value())
    {
        db->createTable("users", {{"id", chronosql::ColumnType::INT}, {"name", chronosql::ColumnType::STRING}});
    }

    // SQL string facade path: insert via execute().
    auto r1 = db->execute("INSERT INTO users (id, name) VALUES (1, 'alice')");
    auto r2 = db->execute("INSERT INTO users VALUES (2, 'bob')");

    std::cout << "Inserted ts=" << r1.last_insert_timestamp.value_or(0)
              << " and ts=" << r2.last_insert_timestamp.value_or(0) << "\n";

    db->flush();
    return EXIT_SUCCESS;
}
