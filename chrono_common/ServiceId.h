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

class ServiceId
{
public:
    ServiceId(std::string const& protocol, uint32_t addr = 0, uint16_t a_port = 0, uint16_t a_provider_id = 0)
        : protocol(protocol)
        , ip_addr(addr)
        , port(a_port)
        , provider_id(a_provider_id)
    {}

    ServiceId(uint32_t addr = 0, uint16_t a_port = 0, uint16_t a_provider_id = 0)
        : protocol("ofi+sockets")
        , ip_addr(addr)
        , port(a_port)
        , provider_id(a_provider_id)
    {}

    ~ServiceId() = default;

    std::string protocol;//protocol string : ex "ofi+sockets"
    uint32_t ip_addr;    //32int IP representation in host notation
    uint16_t port;       //16int port representation in host notation
    uint16_t provider_id;//thalium provider id

    inline service_endpoint get_service_endpoint() const
    {
        return service_endpoint(ip_addr,port);
    }

    template <typename SerArchiveT>
    void serialize(SerArchiveT& serT)
    {
        serT & protocol;
        serT & ip_addr;
        serT & port;
        serT & provider_id;
    }

    std::string& getIPasDottedString(std::string& a_string) const
    {

        char buffer[INET_ADDRSTRLEN];
        // convert ip from host to network byte order uint32_t
        uint32_t ip_net_order = htonl(ip_addr);
        // convert network byte order uint32_t to a dotted string
        if(NULL != inet_ntop(AF_INET, &ip_net_order, buffer, INET_ADDRSTRLEN)) { a_string += std::string(buffer); }
        return a_string;
    }
};

inline std::string to_string(ServiceId const& serviceId)
{
    std::string a_string;
    return std::string("ServiceId{") +serviceId.protocol + ":" + serviceId.getIPasDottedString(a_string) + ":" + std::to_string(serviceId.port) + ":" +
                std::to_string(serviceId.provider_id) + "}";
}

}//namespace chronolog


inline std::ostream& operator<<(std::ostream& out, chronolog::ServiceId const serviceId)
{
    std::string a_string;
    out << "ServiceId{" << serviceId.protocol<<":"<<serviceId.getIPasDottedString(a_string) << ":" << serviceId.port << ":" << serviceId.provider_id
        << "}";
    return out;
}

inline std::string& operator+= (std::string& a_string, chronolog::ServiceId const& serviceId)
{
    a_string += std::string("ServiceId{") + serviceId.protocol + ":" + serviceId.getIPasDottedString(a_string) + ":" + std::to_string(serviceId.port) + ":" +
                std::to_string(serviceId.provider_id) + "}";
    return a_string;
}

#endif
