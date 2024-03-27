#ifndef GRAPHER_ID_CARD_H
#define GRAPHER_ID_CARD_H

#include <arpa/inet.h>

#include <iostream>

// this class wrapps ChronoGrapher Process identification 
// that will be used by all the ChronoLog Processes 
// to both identofy the process and create RPC client channels 
// to send the data to the RecordingService it contains

namespace chronolog
{

class GrapherIdCard
{

    uint32_t ip_addr; //IP address as uint32_t in host byte order
    uint16_t port;    //port number as uint16_t in host byte order
    uint16_t tl_provider_id; // id of thallium service provider

public:


    GrapherIdCard( uint32_t addr = 0, uint16_t a_port=0, uint16_t provider_id=0)
        : ip_addr(addr), port(a_port),tl_provider_id(provider_id)
    {}

    GrapherIdCard( GrapherIdCard const& other)
          : ip_addr(other.getIPaddr()), port(other.getPort()),tl_provider_id(other.getProviderId())
      {}

    ~GrapherIdCard()=default;

    uint32_t getIPaddr() const {return ip_addr; }
    uint16_t getPort() const { return port;}
    uint16_t getProviderId () const { return tl_provider_id; }


    // serialization function used by thallium RPC providers
    // to serialize/deserialize 
    template <typename SerArchiveT>
    void serialize( SerArchiveT & serT)
    {
        serT & ip_addr;
        serT & port;
        serT & tl_provider_id;
    }

    std::string & getIPasDottedString ( std::string & a_string ) const
    {

        char buffer[INET_ADDRSTRLEN];
        // convert ip from host to network byte order uint32_t
        uint32_t ip_net_order = htonl(ip_addr);
        // convert network byte order uint32_t to a dotted string
        if (NULL != inet_ntop(AF_INET, &ip_net_order, buffer, INET_ADDRSTRLEN))
        {   a_string += std::string(buffer); }
     return a_string;
    }

};

} //namespace chronolog

inline bool operator==(chronolog::GrapherIdCard const& card1, chronolog::GrapherIdCard const& card2)
{
    return ( (card1.getIPaddr()==card2.getIPaddr() && card1.getPort() == card2.getPort()
                && card1.getProviderId() == card2.getProviderId()) ? true : false );

}
inline std::ostream & operator<< (std::ostream & out , chronolog::GrapherIdCard const & id_card)
{
    std::string a_string;
    out << "GrapherIdCard{"
       <<":"<<id_card.getIPasDottedString(a_string)<<":"<<id_card.getPort()
       <<":"<<id_card.getProviderId()<<"}";
    return out;
}


#endif
