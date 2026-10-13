#ifndef CHRONOLOG_QUERY_SERVICE_ADDRESS_H
#define CHRONOLOG_QUERY_SERVICE_ADDRESS_H

#include <cstdint>
#include <string>

namespace chronolog
{

/**
 * The IPv4 address a reader-mode client's query service listens on. The client
 * also gives this address to the visor and the players as its own, and a player
 * sends replay results to it.
 *
 * conf_ip, the ClientQueryService service_ip of the client conf, is returned as
 * it is unless it is a loopback address and visor_ip is not. A loopback address
 * works only when every service is on this host; with the visor elsewhere, every
 * reader-mode client given the template's 127.0.0.1 would announce the same
 * address. In that case the result is the address of this host's interface on
 * the route to visor_ip, which the services can reach and which differs between
 * hosts.
 *
 * The result is always a dotted address when conf_ip resolves, since the
 * client's ServiceId takes only dotted addresses: a host name such as
 * "localhost" comes back as "127.0.0.1". conf_ip is returned unchanged only when
 * it does not resolve to an IPv4 address.
 */
std::string query_service_ip(std::string const& conf_ip, std::string const& visor_ip);

/**
 * The IPv4 address, in host byte order, that stands for this host in the
 * client's ClientId, which the visor uses to tell clients apart and which every
 * event carries as its producer.
 *
 * With the visor on another host, it is this host's address on the route to
 * visor_ip, the same address query_service_ip picks. The host name is not used
 * then: on a stock Ubuntu or Debian host it resolves to 127.0.1.1, the same on
 * every host. Otherwise (visor on loopback, or no route) it is the address the
 * host name resolves to, and 0 if it resolves to none.
 */
uint32_t client_host_ipv4(std::string const& visor_ip);

} // namespace chronolog

#endif // CHRONOLOG_QUERY_SERVICE_ADDRESS_H
