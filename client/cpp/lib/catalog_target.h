#pragma once

#include <arpa/inet.h>
#include <netdb.h>

#include <charconv>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/statusor.h>

namespace chronolog::client::detail
{
// The IPv4 list resolution from src/common/rpc/Channel.h, kept here because the SDK cannot include src/.
inline absl::StatusOr<std::string> catalogTarget(const std::string& endpoints)
{
    if(endpoints.empty())
        return absl::InvalidArgumentError("invalid Catalog endpoint");
    const bool ipv4 = endpoints.starts_with("ipv4:");
    if(!ipv4 &&
       (endpoints.find("://") != std::string::npos || endpoints.starts_with("dns:") || endpoints.starts_with("unix:") ||
        endpoints.starts_with("unix-abstract:") || endpoints.starts_with("ipv6:")))
    {
        if(endpoints.find(',') != std::string::npos && !endpoints.starts_with("ipv6:"))
            return absl::InvalidArgumentError("Catalog replica lists require host:port endpoints");
        return endpoints;
    }
    if(endpoints.find_first_of(" \t\r\n") != std::string::npos)
        return absl::InvalidArgumentError("invalid Catalog endpoint");
    auto addresses = ipv4 ? endpoints.substr(5) : endpoints;
    if(ipv4 && addresses.starts_with("///"))
        addresses.erase(0, 3);
    if(addresses.empty() || addresses.back() == ',')
        return absl::InvalidArgumentError("empty Catalog replica endpoint");
    std::vector<std::string> list;
    std::stringstream stream(addresses);
    for(std::string item; std::getline(stream, item, ',');)
    {
        const auto colon = item.rfind(':');
        if(colon == std::string::npos || colon == 0 || colon + 1 == item.size())
            return absl::InvalidArgumentError("Catalog endpoint requires host:port");
        unsigned port = 0;
        const auto parsed = std::from_chars(item.data() + colon + 1, item.data() + item.size(), port);
        if(parsed.ec != std::errc{} || parsed.ptr != item.data() + item.size() || port == 0 || port > 65535)
            return absl::InvalidArgumentError("invalid Catalog endpoint port");
        const auto host = item.substr(0, colon);
        in_addr literal{};
        if(ipv4)
        {
            if(::inet_pton(AF_INET, host.c_str(), &literal) != 1)
                return absl::InvalidArgumentError("ipv4 Catalog target requires IPv4 literals");
        }
        else if(host.front() == '[' && host.back() == ']' && addresses.find(',') == std::string::npos)
        {
            in6_addr literal6{};
            if(::inet_pton(AF_INET6, host.substr(1, host.size() - 2).c_str(), &literal6) != 1)
                return absl::InvalidArgumentError("invalid Catalog IPv6 endpoint");
        }
        else if(host.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") !=
                std::string::npos)
            return absl::InvalidArgumentError("invalid Catalog endpoint host");
        list.push_back(std::move(item));
    }
    if(ipv4 || list.size() == 1)
        return endpoints;
    std::string target = "ipv4:";
    bool resolved = false;
    for(const auto& item: list)
    {
        const auto colon = item.rfind(':');
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* found = nullptr;
        if(::getaddrinfo(item.substr(0, colon).c_str(), nullptr, &hints, &found) != 0 || !found)
            continue;
        char address[INET_ADDRSTRLEN] = {};
        ::inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(found->ai_addr)->sin_addr, address, sizeof(address));
        ::freeaddrinfo(found);
        target += (resolved ? "," : "") + std::string(address) + item.substr(colon);
        resolved = true;
    }
    return resolved ? target : list.front();
}
} // namespace chronolog::client::detail
