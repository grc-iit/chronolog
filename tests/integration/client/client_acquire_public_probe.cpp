#include <ClientConfiguration.h>
#include <chronolog_client.h>

#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <unistd.h>

namespace
{
bool writeLine(int fd, std::string const& line)
{
    std::string bytes = line + '\n';
    std::size_t sent = 0;
    while(sent < bytes.size())
    {
        auto written = ::write(fd, bytes.data() + sent, bytes.size() - sent);
        if(written < 0 && errno == EINTR)
            continue;
        if(written <= 0)
            return false;
        sent += static_cast<std::size_t>(written);
    }
    return true;
}

bool readLine(int fd, std::string& line)
{
    line.clear();
    for(;;)
    {
        char byte = 0;
        auto readCount = ::read(fd, &byte, 1);
        if(readCount < 0 && errno == EINTR)
            continue;
        if(readCount <= 0)
            return false;
        if(byte == '\n')
            return true;
        line.push_back(byte);
        if(line.size() > 256)
            return false;
    }
}
} // namespace

int main(int argc, char** argv)
{
    if(argc != 7)
        return 2;
    int inputFd = std::stoi(argv[1]);
    int outputFd = std::stoi(argv[2]);

    chronolog::ClientPortalServiceConf portal;
    portal.PROTO_CONF = argv[3];
    portal.IP = argv[4];
    portal.PORT = static_cast<uint16_t>(std::stoul(argv[5]));
    portal.PROVIDER_ID = static_cast<uint16_t>(std::stoul(argv[6]));
    portal.RPC_POLICY = chronolog::RpcTimeoutPolicy{};

    auto client = std::make_unique<chronolog::Client>(portal);
    int status = client->Connect();
    if(status != chronolog::CL_SUCCESS)
    {
        writeLine(outputFd, "ERROR connect " + std::to_string(status));
        return 3;
    }
    status = client->CreateChronicle("public-chronicle");
    if(status != chronolog::CL_SUCCESS)
    {
        writeLine(outputFd, "ERROR create " + std::to_string(status));
        return 4;
    }
    if(!writeLine(outputFd, "READY " + std::to_string(client->client_id())))
        return 5;

    chronolog::StoryHandle* handle = nullptr;
    std::string command;
    while(readLine(inputFd, command))
    {
        if(command == "ACQUIRE" || command == "AGAIN")
        {
            auto started = std::chrono::steady_clock::now();
            auto result = client->AcquireStory("public-chronicle", "story");
            auto elapsed =
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started)
                            .count();
            handle = result.second;
            auto label = command == "ACQUIRE" ? "RESULT " : "AGAIN ";
            if(!writeLine(outputFd,
                          std::string(label) + std::to_string(result.first) + " " + std::to_string(elapsed) + " " +
                                  (handle ? "1" : "0")))
                return 6;
        }
        else if(command == "APPEND")
        {
            if(!handle)
            {
                writeLine(outputFd, "APPEND " + std::to_string(chronolog::CL_ERR_NOT_ACQUIRED) + " -1");
                continue;
            }
            auto result = handle->append_event("public-acquire-proof");
            if(!writeLine(outputFd,
                          "APPEND " + std::to_string(result.status) + " " +
                                  std::to_string(static_cast<int>(result.outcome))))
                return 7;
        }
        else if(command == "RELEASE")
        {
            status = client->ReleaseStory("public-chronicle", "story");
            handle = nullptr;
            if(!writeLine(outputFd, "RELEASE " + std::to_string(status)))
                return 8;
        }
        else if(command == "RETIRE")
        {
            status = client->Disconnect();
            handle = nullptr;
            client.reset();
            if(!writeLine(outputFd, "RETIRED " + std::to_string(status)))
                return 9;
            if(!readLine(inputFd, command) || command != "EXIT")
                return 10;
            return 0;
        }
        else if(command == "DROP")
        {
            handle = nullptr;
            client.reset();
            if(!writeLine(outputFd, "RETIRED 0"))
                return 11;
            if(!readLine(inputFd, command) || command != "EXIT")
                return 12;
            return 0;
        }
        else
        {
            writeLine(outputFd, "ERROR command");
            return 10;
        }
    }
    return 0;
}
