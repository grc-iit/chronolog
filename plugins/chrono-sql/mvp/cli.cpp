#include "chronolog/sql/database.h"
#include <iostream>
int main(int argc, char** argv)
{
    if(argc < 4)
    {
        std::cerr << "usage: chronolog_sql VISOR PLAYER CHRONICLE [STATEMENT ...]\n";
        return 2;
    }
    chronolog::client::ClientOptions options;
    options.catalog_endpoint = argv[1];
    options.player_endpoint = argv[2];
    auto client = chronolog::client::Client::Connect(options);
    if(!client.ok())
    {
        std::cerr << client.status() << '\n';
        return 1;
    }
    chronolog::sql::Database database(*client, argv[3]);
    auto execute = [&](const std::string& statement)
    {
        auto result = database.execute(statement);
        if(!result.ok())
        {
            std::cerr << result.status() << '\n';
            return false;
        }
        for(const auto& row: result->rows) std::cout << row.dump() << '\n';
        for(const auto& receipt: result->receipts)
        {
            if(!receipt.ok())
            {
                std::cerr << receipt.status() << '\n';
                return false;
            }
            std::cout << chronolog::sql::Value(
                                 {{"acked", receipt->acked()},
                                  {"hlc",
                                   {{"physical_ns", receipt->hlc.physical_ns}, {"logical", receipt->hlc.logical}}},
                                  {"event_id",
                                   {{"story_id", receipt->event_id.story_id},
                                    {"writer_id", receipt->event_id.writer_id},
                                    {"incarnation", receipt->event_id.incarnation},
                                    {"sequence", receipt->event_id.sequence}}}})
                                 .dump()
                      << '\n';
        }
        std::cout << chronolog::sql::completionJson(*result).dump() << '\n';
        return true;
    };
    if(argc > 4)
    {
        for(int i = 4; i < argc; ++i)
            if(!execute(argv[i]))
                return 1;
    }
    else
    {
        std::string line;
        while(std::getline(std::cin, line))
            if(!line.empty() && !execute(line))
                return 1;
    }
    return 0;
}
