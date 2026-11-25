#include <chronolog_client.h>
#include <ClientConfiguration.h>
#include <chrono>
#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <abt.h>
#include <thread>
int main(int argc, char** argv)
{
    bool const recovery = argc == 4 && std::string(argv[2]) == "--recovery";
    if(argc != 2 && !(argc == 3 && std::string(argv[2]) == "--existing") && !recovery)
        return 2;
    bool const existing = argc == 3;
    std::string const chronicle = recovery ? argv[3] : "LifecycleDeadline";
    chronolog::ClientConfiguration configuration;
    if(!configuration.load_from_file(argv[1]))
        return 2;
    auto clientOwner =
            recovery ? std::make_unique<chronolog::Client>(configuration.PORTAL_CONF, configuration.QUERY_CONF)
                     : std::make_unique<chronolog::Client>(configuration.PORTAL_CONF);
    auto& client = *clientOwner;
    uint64_t first = 0, last = 0;
    if(client.Connect() != chronolog::CL_SUCCESS)
        return 3;
    client.CreateChronicle(chronicle);
    auto acquired = client.AcquireStory(chronicle, "retained");
    if(acquired.first != chronolog::CL_SUCCESS)
    {
        std::cerr << "AcquireStory status " << acquired.first << std::endl;
        return 4;
    }
    if(!existing)
        for(unsigned i = 0; i != 8; ++i)
        {
            auto timestamp = acquired.second->log_event("lifecycle-event-" + std::to_string(i));
            if(!timestamp)
                return 5;
            if(!first)
                first = timestamp;
            last = timestamp;
        }
    std::cout << "READY 8";
    if(recovery)
        std::cout << " " << first << " " << last;
    std::cout << std::endl;
    for(std::string command; std::getline(std::cin, command);)
    {
        if(command == "QUIT")
            break;
        if(command != "TAIL" && !(recovery && command == "REPLAY"))
            return 6;
        std::vector<chronolog::Event> events;
        auto started = std::chrono::steady_clock::now();
        int status = command == "REPLAY" ? client.ReplayStory(chronicle, "retained", first, last + 1, events)
                                         : acquired.second->playback(8, events);
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started)
                               .count();
        bool exact = events.size() == 8;
        for(unsigned i = 0; i < events.size(); ++i)
            exact = exact && events[i].log_record() == "lifecycle-event-" + std::to_string(i);
        std::cout << command << " " << status << " " << elapsed << " " << events.size() << " " << exact << std::endl;
        if(recovery && command == "TAIL" && status == chronolog::CL_SUCCESS && exact)
            for(auto const& event: events)
            {
                std::cout << "EVENT_ID " << event.time() << " " << event.client_id() << " " << event.index() << " ";
                static char const digits[] = "0123456789abcdef";
                for(unsigned char byte: event.log_record()) std::cout << digits[byte >> 4] << digits[byte & 15];
                std::cout << std::endl;
            }
    }
    client.ReleaseStory(chronicle, "retained");
    client.Disconnect();
    if(recovery)
    {
        clientOwner.reset();
        auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while(ABT_initialized() != ABT_ERR_UNINITIALIZED && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        bool const clean = ABT_initialized() == ABT_ERR_UNINITIALIZED;
        std::cout << "CLIENT_NATIVE_CLEANUP " << clean << std::endl;
        if(!clean)
            return 7;
    }
    return 0;
}
