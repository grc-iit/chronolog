#include <cstdlib>
#include <iostream>
#include <string>

#include <cmd_arg_parse.h>

#include "chronosql.h"

int main(int argc, char** argv)
{
    std::string conf_file_path = parse_conf_path_arg(argc, argv);

    auto db = conf_file_path.empty() ? chronosql::ChronoSQL::Create() : chronosql::ChronoSQL::Create(conf_file_path);
    if(!db)
    {
        std::cerr << "Failed to initialize ChronoSQL\n";
        return EXIT_FAILURE;
    }

    auto schema = db->getSchema("users");
    if(!schema)
    {
        std::cerr << "Table 'users' not found. Run the writer example first.\n";
        return EXIT_FAILURE;
    }

    auto result = db->execute("SELECT * FROM users");
    std::cout << "Found " << result.rows.size() << " rows in 'users':\n";
    for(const auto& row: result.rows)
    {
        std::cout << "  ts=" << row.timestamp;
        for(const auto& col: result.columns)
        {
            auto it = row.values.find(col);
            std::cout << "  " << col << "=" << (it == row.values.end() ? "NULL" : it->second);
        }
        std::cout << "\n";
    }

    return EXIT_SUCCESS;
}
