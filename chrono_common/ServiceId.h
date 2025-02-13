#ifndef CHRONO_SERVICE_ID_H
#define CHRONO_SERVICE_ID_H

#include <arpa/inet.h>
#include <iostream>


namespace chronolog
{

typedef uint32_t RecordingGroupId;

typedef uint32_t        in_addr_t;
typedef uint16_t        in_port_t;
typedef std::pair <in_addr_t, in_port_t> service_endpoint;

static int ip_addr_from_dotted_string_to_uint32(std::string const &ip_string, uint32_t & ip_address)
{
    struct sockaddr_in sa;
    // translate the recording service dotted IP string into 32bit network byte order representation
    int inet_pton_return = inet_pton(AF_INET, ip_string.c_str(), &sa.sin_addr.s_addr); //returns 1 on success
    if(1 != inet_pton_return)
    {
        ip_address=0;
        return(-1);
    }

    // translate 32bit ip from network byte order into the host byte order
    ip_address = ntohl(sa.sin_addr.s_addr);

    return 1;
}

class ServiceId
{
// we are using a combination of the uint32_t representation of the service IP address
// and uint16_t representation of the port number to identify service endpoint
// NOTE: both IP and port values ServiceId are kept in the host byte order, not the network order)

    std::string protocol;//protocol string : ex "ofi+sockets"
    uint32_t ip_addr;    //32int IP representation in host notation
    uint16_t port;       //16int port representation in host notation
    uint16_t provider_id;//thalium provider id

public:
    ServiceId(std::string const & protocol=std::string(), uint32_t addr = 0, uint16_t a_port = 0, uint16_t a_provider_id = 0)
        : protocol(protocol)
        , ip_addr(addr)
        , port(a_port)
        , provider_id(a_provider_id)
    {}

    ServiceId(std::string const & protocol, std::string const & ip_string, uint16_t a_port, uint16_t a_provider_id)
        : protocol(protocol)
        , ip_addr(0)
        , port(a_port)
        , provider_id(a_provider_id)
    {
        ip_addr_from_dotted_string_to_uint32(ip_string, ip_addr);
    }

    ServiceId( std::string const & protocol, service_endpoint const & endpoint, uint16_t a_provider_id)
        : protocol(protocol)
        , ip_addr(endpoint.first)
        , port(endpoint.second)
        , provider_id(a_provider_id)
    {}

    ServiceId(ServiceId const&) = default;
    ServiceId & operator=(ServiceId const&) = default;
    ~ServiceId() = default;

    std::string const& getProtocol() const { return protocol; }
    uint32_t getIPaddr() const { return ip_addr; }
    uint16_t getPort() const { return port;}
    uint16_t getProviderId () const { return provider_id; }

    inline service_endpoint get_service_endpoint() const
    {
        return service_endpoint(ip_addr,port);
    }

    bool is_valid() const
    {
        return (!protocol.empty() && (ip_addr != 0) && (port != 0) && (provider_id != 0));
    }

    template <typename SerArchiveT>
    void serialize(SerArchiveT& serT)
    {
        serT & protocol;
        serT & ip_addr;
        serT & port;
        serT & provider_id;
    }

    std::string & get_ip_as_dotted_string(std::string & a_string) const
    {

        char buffer[INET_ADDRSTRLEN];
        // convert ip from host to network byte order uint32_t
        uint32_t ip_net_order = htonl(ip_addr);
        // convert network byte order uint32_t to a dotted string
        if(NULL != inet_ntop(AF_INET, &ip_net_order, buffer, INET_ADDRSTRLEN)) { a_string += std::string(buffer); }
        return a_string;
    }

    std::string & get_service_as_string(std::string & service_as_string) const
    {
        service_as_string =  protocol + "://" + get_ip_as_dotted_string(service_as_string) + ":"+ std::to_string(port);
        
        return service_as_string;

    }
};

inline std::string to_string(ServiceId const& serviceId)
{
    std::string a_string;
    return std::string("ServiceId{") +serviceId.getProtocol() + ":" + serviceId.get_ip_as_dotted_string(a_string) + ":" + std::to_string(serviceId.getPort()) + ":" +
                std::to_string(serviceId.getProviderId()) + "}";
}

}//namespace chronolog

inline bool operator==(chronolog::ServiceId const& service_id_1, chronolog::ServiceId const& service_id_2)
{
    return ( (service_id_1.getProtocol() == service_id_2.getProtocol()) && (service_id_1.getIPaddr() == service_id_2.getIPaddr())
            && (service_id_1.getPort() == service_id_2.getPort()) && (service_id_1.getProviderId() == service_id_2.getProviderId()));
}


inline std::ostream& operator<<(std::ostream& out, chronolog::ServiceId const serviceId)
{
    std::string a_string;
    out << "ServiceId{" << serviceId.getProtocol()<<":"<<serviceId.get_ip_as_dotted_string(a_string) << ":" << serviceId.getPort() << ":" 
        << serviceId.getProviderId() << "}";
    return out;
}

inline std::string& operator+= (std::string& a_string, chronolog::ServiceId const& serviceId)
{
    a_string += std::string("ServiceId{") + serviceId.getProtocol() + ":" + serviceId.get_ip_as_dotted_string(a_string) + ":" + std::to_string(serviceId.getPort()) + ":" +
                std::to_string(serviceId.getProviderId()) + "}";
    return a_string;
}

#endif





