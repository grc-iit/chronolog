#pragma once

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>

#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <grpcpp/grpcpp.h>

namespace chronolog::rpc
{

// One endpoint, or a comma separated list of Visor replicas, as one gRPC target. A list becomes a
// single pick_first channel over the resolved IPv4 addresses, so a call or watch stream that loses
// the replica it was attached to reconnects to the next one (I9.1, section 12).
inline std::string visorTarget(const std::string& endpoints)
{
    std::vector<std::string> list;
    std::stringstream stream(endpoints);
    for(std::string item; std::getline(stream, item, ',');)
        if(!item.empty())
            list.push_back(item);
    if(list.size() < 2)
        return endpoints;
    std::string target = "ipv4:";
    bool resolved = false;
    for(const auto& item: list)
    {
        const auto colon = item.rfind(':');
        if(colon == std::string::npos)
            continue;
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* found = nullptr;
        if(::getaddrinfo(item.substr(0, colon).c_str(), nullptr, &hints, &found) != 0 || !found)
            continue;
        char text[INET_ADDRSTRLEN] = {};
        ::inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(found->ai_addr)->sin_addr, text, sizeof(text));
        ::freeaddrinfo(found);
        target += (resolved ? "," : "") + std::string(text) + item.substr(colon);
        resolved = true;
    }
    return resolved ? target : list.front();
}

// A failed lookup, such as aardvark-dns answering late while a container joins the network, must not
// keep the channel dark: gRPC otherwise waits dns_min_time_between_resolutions (30 s) before it resolves
// again, far beyond every call deadline here.
inline std::shared_ptr<grpc::Channel> visorChannel(const std::string& endpoints, grpc::ChannelArguments args = {})
{
    args.SetInt(GRPC_ARG_DNS_MIN_TIME_BETWEEN_RESOLUTIONS_MS, 1000);
    args.SetInt(GRPC_ARG_INITIAL_RECONNECT_BACKOFF_MS, 100);
    args.SetInt(GRPC_ARG_MIN_RECONNECT_BACKOFF_MS, 100);
    args.SetInt(GRPC_ARG_MAX_RECONNECT_BACKOFF_MS, 1000);
    return grpc::CreateCustomChannel(visorTarget(endpoints), grpc::InsecureChannelCredentials(), args);
}

} // namespace chronolog::rpc
