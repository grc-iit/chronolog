#include "chronolog/kvs/store.h"
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
    {
        std::cerr << client.status() << '\n';
        return 1;
    }
    chronolog::kvs::Store store(*client, "kvs-example");
    auto a = store.put("agent-state", "planning");
    auto b = store.put("agent-state", "finished");
    if(!a.ok() || !b.ok())
        return 1;
    auto current = store.get("agent-state", {.causal_floor = b->hlc});
    auto previous = store.get("agent-state", {.at = b->hlc});
    if(!current.ok() || !previous.ok() || !current->value || !previous->value || !current->completion.complete ||
       !previous->completion.complete || current->value->value != "finished" || previous->value->value != "planning")
        return 1;
    std::cout << previous->value->value << " -> " << current->value->value << '\n';
    auto end = b->hlc;
    ++end.logical;
    auto history = store.history("agent-state", {{}, end});
    if(!history.ok())
        return 1;
    size_t count = 0;
    for(size_t pull = 0; pull < 32; ++pull)
    {
        auto item = history->next();
        if(!item.ok() || !*item)
            return 1;
        for(const auto& value: (**item).versions)
        {
            std::cout << value.value << '\n';
            ++count;
        }
        if((**item).completion)
            return ((**item).completion->complete && count >= 2) ? 0 : 1;
    }
    return 1;
}
