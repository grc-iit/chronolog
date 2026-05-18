#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include <cmd_arg_parse.h>

#include "chronopubsub.h"

int main(int argc, char** argv)
{
    std::string conf_file_path = parse_conf_path_arg(argc, argv);

    auto pubsub = conf_file_path.empty() ? chronopubsub::ChronoPubSub::Create()
                                         : chronopubsub::ChronoPubSub::Create(conf_file_path);
    if(!pubsub)
    {
        std::cerr << "Failed to initialize ChronoPubSub\n";
        return 1;
    }

    const std::string topic = "demo_topic";

    std::atomic<int> received{0};
    auto sub_id = pubsub->subscribe_from(topic,
                                         /*since_timestamp*/ 1,
                                         [&received](const chronopubsub::Message& msg)
                                         {
                                             std::cout << "  received: topic='" << msg.topic << "' ts=" << msg.timestamp
                                                       << " payload='" << msg.payload << "'\n";
                                             ++received;
                                         });

    std::cout << "Subscribed (id=" << sub_id << ") to topic '" << topic << "'. Listening for 10 seconds...\n";
    std::this_thread::sleep_for(std::chrono::seconds(10));

    pubsub->unsubscribe(sub_id);
    std::cout << "Done. Received " << received.load() << " messages.\n";

    return 0;
}
