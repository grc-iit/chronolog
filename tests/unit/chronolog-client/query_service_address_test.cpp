#include <string>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>

#include <gtest/gtest.h>

#include "QueryServiceAddress.h"

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
