#ifndef _KEEPER_ID_CARD_H
#define _KEEPER_ID_CARD_H

#include <arpa/inet.h>

#include <iostream>

#include "ServiceId.h"

// this class wrapps ChronoKeeper Process identification 
// that will be used by all the ChronoLog Processes 
// to both identify the Keepr process and create RPC client channels 
// to send the data to the Keeper RecordingService and Keeper DataStoreAdminService

namespace chronolog
{


class KeeperIdCard
{

    RecordingGroupId keeper_group_id;
    uint32_t ip_addr; //IP address as uint32_t in host byte order
    uint16_t port;    //port number as uint16_t in host byte order
    uint16_t tl_provider_id; // id of thallium service provider

public:


    KeeperIdCard( uint32_t group_id = 0, uint32_t addr = 0, uint16_t a_port=0, uint16_t provider_id=0)
        : keeper_group_id(group_id), ip_addr(addr), port(a_port),tl_provider_id(provider_id)
    {}

    KeeperIdCard( KeeperIdCard const& other)
          : keeper_group_id(other.getGroupId()), ip_addr(other.getIPaddr()), port(other.getPort()),tl_provider_id(other.getProviderId())
      {}

    ~KeeperIdCard()=default;

    RecordingGroupId getGroupId() const { return keeper_group_id; }
    uint32_t getIPaddr() const {return ip_addr; }
    uint16_t getPort() const { return port;}
    uint16_t getProviderId () const { return tl_provider_id; }


    // serialization function used by thallium RPC providers
    // to serialize/deserialize KeeperIdCard
    template <typename SerArchiveT>
    void serialize( SerArchiveT & serT)
    {
        serT & keeper_group_id;
        serT & ip_addr;
        serT & port;
        serT & tl_provider_id;
    }

    std::string & get_ip_as_dotted_string ( std::string & a_string ) const
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

inline std::string to_string(chronolog::KeeperIdCard const& keeper_id_card)
{
    std::string a_string;
    return std::string("KeeperIdCard{") + std::to_string(keeper_id_card.getGroupId()) + ":" +
                keeper_id_card.get_ip_as_dotted_string(a_string) + ":" + std::to_string(keeper_id_card.getPort()) + ":" +
                std::to_string(keeper_id_card.getProviderId()) + "}";
}

} //namespace chronolog


inline bool operator==(chronolog::KeeperIdCard const& card1, chronolog::KeeperIdCard const& card2)
{
    return ( (card1.getIPaddr()==card2.getIPaddr() && card1.getPort() == card2.getPort()
                && card1.getProviderId() == card2.getProviderId()) ? true : false );

}

inline std::ostream & operator<< (std::ostream & out , chronolog::KeeperIdCard const & keeper_id_card)
{
    std::string a_string;
    out << "KeeperIdCard{"<<keeper_id_card.getGroupId()
       <<":"<<keeper_id_card.get_ip_as_dotted_string(a_string)<<":"<<keeper_id_card.getPort()
       <<":"<<keeper_id_card.getProviderId()<<"}";
    return out;
}

inline std::string & operator+= (std::string & a_string , chronolog::KeeperIdCard const & keeper_id_card)
{
    a_string += chronolog::to_string(keeper_id_card);
    return a_string;
}

#endif
