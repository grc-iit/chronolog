#include "chronolog/kvs/store.h"
#include <charconv>
#include <iostream>
namespace
{
void version(const chronolog::kvs::Version& v)
{
    std::cout << v.hlc.physical_ns << ':' << v.hlc.logical << " id=" << v.event_id.story_id << ':'
              << v.event_id.writer_id << ':' << v.event_id.incarnation << ':' << v.event_id.sequence
              << " acked=" << v.acked() << '\n';
}
bool parse(const char* text, chronolog::Hlc& hlc)
{
    std::string_view value(text);
    auto colon = value.find(':');
    if(colon == value.npos)
        return false;
    auto p = std::from_chars(value.data(), value.data() + colon, hlc.physical_ns);
    auto l = std::from_chars(value.data() + colon + 1, value.data() + value.size(), hlc.logical);
    return p.ec == std::errc{} && p.ptr == value.data() + colon && l.ec == std::errc{} &&
           l.ptr == value.data() + value.size();
}
} // namespace
int main(int argc, char** argv)
{
    if(argc < 6)
    {
        std::cerr << "usage: chronolog_kvs CATALOG PLAYER CHRONICLE put|get|get-at|erase|history KEY [VALUE|HLC|START "
                     "END]\n";
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
    chronolog::kvs::Store store(*client, argv[3]);
    std::string action = argv[4], key = argv[5];
    if(action == "put" || action == "erase")
    {
        if((action == "put" && argc != 7) || (action == "erase" && argc != 6))
            return 2;
        auto result = action == "put" ? store.put(key, argv[6]) : store.erase(key);
        if(!result.ok())
        {
            std::cerr << result.status() << '\n';
            return 1;
        }
        version(*result);
        return 0;
    }
    if(action == "get" || action == "get-at")
    {
        chronolog::kvs::GetOptions get;
        if(action == "get-at")
        {
            chronolog::Hlc at;
            if(argc != 7 || !parse(argv[6], at))
                return 2;
            get.at = at;
        }
        else if(argc != 6)
            return 2;
        auto result = store.get(key, get);
        if(!result.ok())
        {
            std::cerr << result.status() << '\n';
            return 1;
        }
        std::cout << "complete=" << result->completion.complete
                  << " reason=" << static_cast<int>(result->completion.reason) << '\n';
        if(result->value)
        {
            version(result->value->version);
            std::cout << result->value->value << '\n';
        }
        if(!result->status.ok())
        {
            std::cerr << result->status << '\n';
            return 1;
        }
        return result->completion.complete ? 0 : 3;
    }
    if(action == "history")
    {
        chronolog::client::HlcRange range;
        if(argc != 8 || !parse(argv[6], range.start) || !parse(argv[7], range.end))
            return 2;
        auto history = store.history(key, range);
        if(!history.ok())
        {
            std::cerr << history.status() << '\n';
            return 1;
        }
        for(size_t pull = 0; pull < 10000; ++pull)
        {
            auto item = history->next();
            if(!item.ok())
            {
                std::cerr << item.status() << '\n';
                return 1;
            }
            if(!*item)
                return 1;
            for(const auto& v: (**item).versions)
            {
                version(v.version);
                std::cout << (v.deleted ? "<deleted>" : v.value) << '\n';
            }
            if((**item).completion)
            {
                std::cout << "complete=" << (**item).completion->complete
                          << " reason=" << static_cast<int>((**item).completion->reason) << '\n';
                return (**item).completion->complete ? 0 : 3;
            }
        }
        return 1;
    }
    return 2;
}
