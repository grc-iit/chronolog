#include <cstring>
#include <optional>
#include <string>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "QueryServiceAddress.h"

namespace
{

// The first IPv4 address host resolves to, numeric or by name.
std::optional<struct in_addr> resolve_ipv4(std::string const& host)
{
    if(host.empty())
    {
        return std::nullopt;
    }
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    struct addrinfo* res = nullptr;
    if(getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || res == nullptr)
    {
        return std::nullopt;
    }
    struct in_addr addr = reinterpret_cast<struct sockaddr_in const*>(res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return addr;
}

bool is_loopback(struct in_addr const& addr) { return (ntohl(addr.s_addr) >> 24) == 127; }

// The source address the kernel picks for packets to dest. Connecting a UDP
// socket only selects the route; nothing is sent.
std::optional<std::string> source_address_towards(struct in_addr const& dest)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if(fd < 0)
    {
        return std::nullopt;
    }
    struct sockaddr_in peer;
    std::memset(&peer, 0, sizeof(peer));
    peer.sin_family = AF_INET;
    peer.sin_addr = dest;
    peer.sin_port = htons(9); // any port: the route depends on the address only
    struct sockaddr_in local;
    socklen_t local_len = sizeof(local);
    std::optional<std::string> result;
    char text[INET_ADDRSTRLEN] = {0};
    if(connect(fd, reinterpret_cast<struct sockaddr const*>(&peer), sizeof(peer)) == 0 &&
       getsockname(fd, reinterpret_cast<struct sockaddr*>(&local), &local_len) == 0 &&
       inet_ntop(AF_INET, &local.sin_addr, text, sizeof(text)) != nullptr)
    {
        result = text;
    }
    close(fd);
    return result;
}

} // namespace

std::string chronolog::query_service_ip(std::string const& conf_ip, std::string const& visor_ip)
{
    std::optional<struct in_addr> conf_addr = resolve_ipv4(conf_ip);
    if(!conf_addr || !is_loopback(*conf_addr))
    {
        return conf_ip;
    }
    std::optional<struct in_addr> visor_addr = resolve_ipv4(visor_ip);
    if(!visor_addr || is_loopback(*visor_addr))
    {
        return conf_ip;
    }
    return source_address_towards(*visor_addr).value_or(conf_ip);
}
