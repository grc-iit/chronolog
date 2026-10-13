#include <climits>
#include <cstdint>
#include <cstring>
#include <string>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "QueryServiceAddress.h"

using chronolog::client_host_ipv4;
using chronolog::query_service_ip;

namespace
{

// The first IPv4 address of an interface that is up and is not loopback, or
// an empty string when this host has none.
std::string first_non_loopback_ipv4()
{
    struct ifaddrs* list = nullptr;
    if(getifaddrs(&list) != 0)
    {
        return "";
    }
    std::string found;
    for(struct ifaddrs* ifa = list; ifa != nullptr && found.empty(); ifa = ifa->ifa_next)
    {
        if(ifa->ifa_addr == nullptr || ifa->ifa_addr->sa_family != AF_INET || !(ifa->ifa_flags & IFF_UP) ||
           (ifa->ifa_flags & IFF_LOOPBACK))
        {
            continue;
        }
        char text[INET_ADDRSTRLEN] = {0};
        auto const* sin = reinterpret_cast<struct sockaddr_in const*>(ifa->ifa_addr);
        if(inet_ntop(AF_INET, &sin->sin_addr, text, sizeof(text)) != nullptr)
        {
            found = text;
        }
    }
    freeifaddrs(list);
    return found;
}

// A dotted IPv4 address in host byte order.
uint32_t host_order(std::string const& dotted)
{
    struct in_addr addr;
    if(inet_pton(AF_INET, dotted.c_str(), &addr) != 1)
    {
        return 0;
    }
    return ntohl(addr.s_addr);
}

// What this host's name resolves to, in host byte order, or 0.
uint32_t host_name_ipv4()
{
    char name[HOST_NAME_MAX + 1] = {0};
    if(gethostname(name, sizeof(name) - 1) != 0)
    {
        return 0;
    }
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    struct addrinfo* res = nullptr;
    if(getaddrinfo(name, nullptr, &hints, &res) != 0 || res == nullptr)
    {
        return 0;
    }
    uint32_t ip = ntohl(reinterpret_cast<struct sockaddr_in const*>(res->ai_addr)->sin_addr.s_addr);
    freeaddrinfo(res);
    return ip;
}

} // namespace

TEST(QueryServiceAddressTest, ExplicitAddressIsKept)
{
    EXPECT_EQ(query_service_ip("10.1.2.3", "192.0.2.10"), "10.1.2.3");
}

TEST(QueryServiceAddressTest, LoopbackIsKeptWhenTheVisorIsOnLoopback)
{
    EXPECT_EQ(query_service_ip("127.0.0.1", "127.0.0.1"), "127.0.0.1");
    EXPECT_EQ(query_service_ip("127.0.0.1", "127.0.1.1"), "127.0.0.1");
    EXPECT_EQ(query_service_ip("127.0.0.1", "localhost"), "127.0.0.1");
}

// With the visor on another host, every reader-mode client given the template's
// 127.0.0.1 would listen, and announce itself, under that one address.
TEST(QueryServiceAddressTest, LoopbackIsReplacedByTheAddressOnTheRouteToTheVisor)
{
    std::string const own_ip = first_non_loopback_ipv4();
    if(own_ip.empty())
    {
        GTEST_SKIP() << "no IPv4 interface other than loopback";
    }
    // the route to one of this host's own addresses leaves from that address
    EXPECT_EQ(query_service_ip("127.0.0.1", own_ip), own_ip);
    EXPECT_EQ(query_service_ip("localhost", own_ip), own_ip);
}

TEST(QueryServiceAddressTest, ConfAddressIsKeptWhenTheVisorDoesNotResolve)
{
    EXPECT_EQ(query_service_ip("127.0.0.1", "no-such-visor.invalid"), "127.0.0.1");
    EXPECT_EQ(query_service_ip("127.0.0.1", ""), "127.0.0.1");
}

// ServiceId takes only dotted addresses: a host name given back unchanged would
// be announced to the players as address 0.
TEST(QueryServiceAddressTest, AHostNameIsReturnedAsItsAddress)
{
    EXPECT_EQ(query_service_ip("localhost", "127.0.0.1"), "127.0.0.1");
    EXPECT_EQ(query_service_ip("localhost", "localhost"), "127.0.0.1");
    EXPECT_EQ(query_service_ip("localhost", "no-such-visor.invalid"), "127.0.0.1");
}

TEST(QueryServiceAddressTest, AConfNameThatDoesNotResolveIsKept)
{
    EXPECT_EQ(query_service_ip("no-such-host.invalid", "192.0.2.10"), "no-such-host.invalid");
}

// On a stock Ubuntu or Debian host the host name resolves to 127.0.1.1, so a
// ClientId taken from it would be the same on every host.
TEST(QueryServiceAddressTest, ClientIdentityUsesTheAddressOnTheRouteToARemoteVisor)
{
    std::string const own_ip = first_non_loopback_ipv4();
    if(own_ip.empty())
    {
        GTEST_SKIP() << "no IPv4 interface other than loopback";
    }
    EXPECT_EQ(client_host_ipv4(own_ip), host_order(own_ip));
}

TEST(QueryServiceAddressTest, ClientIdentityFallsBackToTheHostNameWithoutARemoteVisor)
{
    EXPECT_EQ(client_host_ipv4("127.0.0.1"), host_name_ipv4());
    EXPECT_EQ(client_host_ipv4("no-such-visor.invalid"), host_name_ipv4());
    EXPECT_EQ(client_host_ipv4(""), host_name_ipv4());
}
