// Reads back what store_chronolog (or the fake ldmsd) wrote: one story per schema and producer.
// usage: chronolog_ldms_example <catalog> <player> [container] [expected events]
#include <iostream>
#include <nlohmann/json.hpp>
#include "reader.h"

using namespace chronolog;

int main(int argc, char** argv)
{
    if(argc < 3)
    {
        std::cerr << "usage: chronolog_ldms_example <catalog> <player> [container] [expected events]\n";
        return 2;
    }
    client::ClientOptions options;
    options.catalog_endpoint = argv[1];
    options.player_endpoint = argv[2];
    const std::string container = argc > 3 ? argv[3] : "ldms";
    const size_t expected = argc > 4 ? std::stoull(argv[4]) : 0;
    auto sdk = client::Client::Connect(options);
    if(!sdk.ok())
    {
        std::cerr << "connect: " << sdk.status() << '\n';
        return 1;
    }
    auto stories = ldms::readChronicle(*sdk, container);
    if(!stories.ok())
    {
        std::cerr << "read: " << stories.status() << '\n';
        return 1;
    }
    size_t total = 0;
    bool ok = !stories->empty();
    for(const auto& s: *stories)
    {
        std::cout << s.story.name << " events=" << s.events.size() << " complete=" << s.complete << '\n';
        ok = ok && s.complete;
        for(const auto& event: s.events)
        {
            auto doc = nlohmann::json::parse(event.envelope.payload, nullptr, false);
            const auto& attrs = event.envelope.attributes;
            ok = ok && event.envelope.content_type == "application/vnd.chronolog.ldms-sample+json" && doc.is_object() &&
                 doc.contains("metrics") && doc["metrics"].is_object() && attrs.count("ldms.producer") == 1 &&
                 attrs.count("ldms.schema") == 1 && attrs.count("ldms.instance") == 1;
        }
        if(!s.events.empty())
            std::cout << "  first: " << s.events.front().envelope.payload << '\n';
        total += s.events.size();
    }
    std::cout << "total=" << total << '\n';
    if(expected != 0 && total != expected)
        ok = false;
    return ok ? 0 : 1;
}
