#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <chrono>

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

    std::cout << "Publishing 5 messages to topic '" << topic << "'...\n";
    for(int i = 1; i <= 5; ++i)
    {
        std::string payload = "message-" + std::to_string(i);
        std::uint64_t ts = pubsub->publish(topic, payload);
        std::cout << "  [" << i << "] payload='" << payload << "' timestamp=" << ts << "\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Required for cross-process subscribers to see the messages.
    pubsub->flush();
    std::cout << "Flushed. Subscribers in other processes can now replay these messages.\n";

    return 0;
}
