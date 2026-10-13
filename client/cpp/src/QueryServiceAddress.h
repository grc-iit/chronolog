#ifndef CHRONOLOG_QUERY_SERVICE_ADDRESS_H
#define CHRONOLOG_QUERY_SERVICE_ADDRESS_H

#include <cstdint>
#include <optional>
#include <string>

namespace chronolog
{

/**
 * This host's IPv4 address on the route to visor_ip, as a dotted address, when
 * the visor is on another host; nothing when visor_ip is a loopback address,
 * does not resolve to an IPv4 address, or has no route. Found by connecting a
 * UDP socket to the visor's address and reading its local address; nothing is
 * sent. A client computes it once and passes it to the two functions below.
 */
std::optional<std::string> address_towards_remote_visor(std::string const& visor_ip);

struct QueryServiceIp
{
    std::string ip;        // the address to listen on and to announce
    bool replaced = false; // true when a loopback conf address was replaced by towards_visor
};

/**
 * The IPv4 address a reader-mode client's query service listens on. The client
 * also gives this address to the visor and the players as its own, and a player
 * sends replay results to it.
 *
 * conf_ip, the ClientQueryService service_ip of the client conf, is kept unless
 * it is a loopback address and towards_visor (from address_towards_remote_visor)
 * is set. A loopback address works only when every service is on this host; with
 * the visor elsewhere, every reader-mode client given the template's 127.0.0.1
 * would announce the same address. In that case the result is towards_visor,
 * which the services can reach and which differs between hosts, and replaced is
 * true.
 *
 * The result is a dotted address whenever conf_ip resolves, since the client's
 * ServiceId takes only dotted addresses: a host name such as "localhost" comes
 * back as "127.0.0.1", with replaced false. conf_ip is returned unchanged only
 * when it does not resolve to an IPv4 address.
 */
QueryServiceIp query_service_ip(std::string const& conf_ip, std::optional<std::string> const& towards_visor);

/**
 * The IPv4 address, in host byte order, that stands for this host in the
 * client's ClientId, which the visor uses to tell clients apart and which every
 * event carries as its producer.
 *
 * For a reader-mode client, listen_ip is its query service address, and the
 * ClientId uses it unless it is a loopback address, so that the ClientId names
 * the address the client can be reached at. Otherwise (a writer-only client,
 * which passes nothing, or a loopback listen address) it is towards_visor when
 * set. The host name is used only when neither is: on a stock Ubuntu or Debian
 * host it resolves to 127.0.1.1, the same on every host. 0 if nothing resolves.
 */
uint32_t client_host_ipv4(std::optional<std::string> const& listen_ip, std::optional<std::string> const& towards_visor);

} // namespace chronolog

#endif // CHRONOLOG_QUERY_SERVICE_ADDRESS_H
