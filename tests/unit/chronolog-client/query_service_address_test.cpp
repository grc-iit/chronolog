#include <climits>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "QueryServiceAddress.h"

using chronolog::address_towards_remote_visor;
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

// Names under .invalid never resolve (RFC 6761). The trailing dot keeps the
// resolver from appending a search domain, where a wildcard record could answer.
constexpr char const* kNoSuchVisor = "no-such-visor.invalid.";
constexpr char const* kNoSuchHost = "no-such-host.invalid.";

// A documentation address (RFC 5737) standing for a visor on another host.
constexpr char const* kRemote = "192.0.2.7";

TEST(QueryServiceAddressTest, TheRouteToAVisorAtOneOfThisHostsAddressesLeavesFromIt)
{
    std::string const own_ip = first_non_loopback_ipv4();
    if(own_ip.empty())
    {
        GTEST_SKIP() << "no IPv4 interface other than loopback";
    }
    EXPECT_EQ(address_towards_remote_visor(own_ip), own_ip);
}

TEST(QueryServiceAddressTest, NoRouteIsTakenForAVisorOnLoopbackOrUnknown)
{
    EXPECT_EQ(address_towards_remote_visor("127.0.0.1"), std::nullopt);
    EXPECT_EQ(address_towards_remote_visor("127.0.1.1"), std::nullopt);
    EXPECT_EQ(address_towards_remote_visor("localhost"), std::nullopt);
    EXPECT_EQ(address_towards_remote_visor(kNoSuchVisor), std::nullopt);
    EXPECT_EQ(address_towards_remote_visor(""), std::nullopt);
}

TEST(QueryServiceAddressTest, ExplicitAddressIsKept)
{
    for(std::optional<std::string> const& towards: {std::optional<std::string>(kRemote), std::optional<std::string>()})
    {
        auto const result = query_service_ip("10.1.2.3", towards);
        EXPECT_EQ(result.ip, "10.1.2.3");
        EXPECT_FALSE(result.replaced);
    }
}

TEST(QueryServiceAddressTest, LoopbackIsKeptWithoutARemoteVisor)
{
    auto const result = query_service_ip("127.0.0.1", std::nullopt);
    EXPECT_EQ(result.ip, "127.0.0.1");
    EXPECT_FALSE(result.replaced);
}

// With the visor on another host, every reader-mode client given the template's
// 127.0.0.1 would listen, and announce itself, under that one address.
TEST(QueryServiceAddressTest, LoopbackIsReplacedByTheAddressTowardsARemoteVisor)
{
    for(char const* conf_ip: {"127.0.0.1", "127.0.1.1", "localhost"})
    {
        auto const result = query_service_ip(conf_ip, std::string(kRemote));
        EXPECT_EQ(result.ip, kRemote) << conf_ip;
        EXPECT_TRUE(result.replaced) << conf_ip;
    }
}

// ServiceId takes only dotted addresses: a host name given back unchanged would
// be announced to the players as address 0. Resolving a name is not a
// replacement, so the client does not log that the visor is on another host.
TEST(QueryServiceAddressTest, AHostNameIsReturnedAsItsAddressWithoutCountingAsReplaced)
{
    auto const result = query_service_ip("localhost", std::nullopt);
    EXPECT_EQ(result.ip, "127.0.0.1");
    EXPECT_FALSE(result.replaced);
}

TEST(QueryServiceAddressTest, AConfNameThatDoesNotResolveIsKept)
{
    auto const result = query_service_ip(kNoSuchHost, std::string(kRemote));
    EXPECT_EQ(result.ip, kNoSuchHost);
    EXPECT_FALSE(result.replaced);
}

// A conf address on a fast network while the visor is reached over another
// one: the ClientId must name the address the client listens on.
TEST(QueryServiceAddressTest, AReaderIsIdentifiedByItsListenAddress)
{
    EXPECT_EQ(client_host_ipv4(std::string("10.10.0.5"), std::string(kRemote)), host_order("10.10.0.5"));
    EXPECT_EQ(client_host_ipv4(std::string("10.10.0.5"), std::nullopt), host_order("10.10.0.5"));
}

// On a stock Ubuntu or Debian host the host name resolves to 127.0.1.1, so a
// ClientId taken from it would be the same on every host.
TEST(QueryServiceAddressTest, AWriterOrLoopbackReaderIsIdentifiedByTheRouteToARemoteVisor)
{
    EXPECT_EQ(client_host_ipv4(std::nullopt, std::string(kRemote)), host_order(kRemote));
    EXPECT_EQ(client_host_ipv4(std::string("127.0.0.1"), std::string(kRemote)), host_order(kRemote));
}

TEST(QueryServiceAddressTest, WithoutEitherTheClientIsIdentifiedByItsHostName)
{
    EXPECT_EQ(client_host_ipv4(std::nullopt, std::nullopt), host_name_ipv4());
    EXPECT_EQ(client_host_ipv4(std::string("127.0.0.1"), std::nullopt), host_name_ipv4());
}
