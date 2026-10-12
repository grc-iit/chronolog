#ifndef CHRONOLOG_QUERY_SERVICE_ADDRESS_H
#define CHRONOLOG_QUERY_SERVICE_ADDRESS_H

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
 * hosts. conf_ip is returned when either name does not resolve to an IPv4 address
 * or the route cannot be found.
 */
std::string query_service_ip(std::string const& conf_ip, std::string const& visor_ip);

} // namespace chronolog

#endif // CHRONOLOG_QUERY_SERVICE_ADDRESS_H
