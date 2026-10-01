#include "chronolog/stream/stream.h"
#include <cstdlib>
#include <iostream>
int main(int argc, char** argv)
{
    if(argc != 4)
        return 2;
    chronolog::client::ClientOptions options;
    options.catalog_endpoint = argv[1];
    options.player_endpoint = argv[2];
    auto client = chronolog::client::Client::Connect(options);
    if(!client.ok())
        return 1;
    auto chronicle = "metrics-example-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    chronolog::stream::Collector collector(*client, chronicle, "example-host", {}, true);
    for(int i = 0; i < 2; ++i)
    {
        auto sample = collector.sample();
        if(!sample.ok())
            return 1;
        for(const auto& r: *sample)
            if(!r.ok() || !r->acked())
                return 1;
    }
    chronolog::stream::SinkOptions sinkOptions;
    sinkOptions.url = argv[3];
    if(auto token = std::getenv("INFLUX_TOKEN"))
        sinkOptions.token = token;
    chronolog::stream::InfluxSink sink(sinkOptions);
    chronolog::kvs::Store cursors(*client, chronicle + ".cursors");
    chronolog::stream::Exporter exporter(*client,
                                         cursors,
                                         chronicle,
                                         {"system.cpu", "system.memory", "system.network"},
                                         sink,
                                         {.batch_count = 3});
    auto result = exporter.run([] { return false; }, std::chrono::system_clock::now() + std::chrono::seconds(2));
    if(!result.ok())
    {
        std::cerr << result.status() << '\n';
        return 1;
    }
    std::cout << "exported " << result->events << " metric samples\n";
    return result->events >= 6 ? 0 : 1;
}
