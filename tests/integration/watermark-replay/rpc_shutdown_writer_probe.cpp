// Write a named, retained story and report the acknowledged event identities.
#include <chronolog_client.h>
#include <abt.h>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>

int main(int argc, char** argv)
{
    if(argc != 3)
        return 2;
    chronolog::ClientConfiguration config;
    if(!config.load_from_file(argv[1]))
        return 2;
    auto client = std::make_unique<chronolog::Client>(config.PORTAL_CONF);
    if(client->Connect() != chronolog::CL_SUCCESS || client->CreateChronicle(argv[2]) != chronolog::CL_SUCCESS)
        return 3;
    auto acquired = client->AcquireStory(argv[2], "retained");
    if(acquired.first != chronolog::CL_SUCCESS || !acquired.second)
        return 4;
    for(unsigned i = 0; i < 8; ++i)
    {
        auto payload = "shutdown-event-" + std::to_string(i);
        auto result = acquired.second->append_event(payload);
        if(result.status != chronolog::CL_SUCCESS || result.outcome != chronolog::AppendOutcome::Acknowledged ||
           result.timestamp == 0)
            return 5;
        std::cout << "EVENT_ID " << result.timestamp << " " << client->client_id() << " " << result.event_index << " ";
        static char const digits[] = "0123456789abcdef";
        for(unsigned char byte: payload) std::cout << digits[byte >> 4] << digits[byte & 15];
        std::cout << std::endl;
    }
    auto releaseStarted =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
                    .count();
    std::cout << "RELEASE_START " << releaseStarted << std::endl;
    if(client->ReleaseStory(argv[2], "retained") != chronolog::CL_SUCCESS ||
       client->Disconnect() != chronolog::CL_SUCCESS)
        return 6;
    client.reset();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while(ABT_initialized() != ABT_ERR_UNINITIALIZED && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    bool const retired = ABT_initialized() == ABT_ERR_UNINITIALIZED;
    std::cout << "CLIENT_NATIVE_CLEANUP " << retired << std::endl;
    return retired ? 0 : 7;
}
