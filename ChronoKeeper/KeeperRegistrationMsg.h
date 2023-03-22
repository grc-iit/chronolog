#ifndef KEEPER_REGISTRATION_MSG_H
#define KEEPER_REGISTRATION_MSG_H

#include <arpa/inet.h>
#include <iostream>
#include "KeeperIdCard.h"


namespace chronolog
{

class ServiceId
{
public: 
	ServiceId( uint32_t addr, uint16_t a_port, uint16_t a_provider_id)
		: ip_addr(addr), port(a_port), provider_id(a_provider_id)
	{}
	~ServiceId() = default;

  uint32_t ip_addr;  	//32int IP representation in host notation
  uint16_t port;	//16int port representation in host notation
  uint16_t provider_id; //thalium provider id
  
  template <typename SerArchiveT>
  void serialize( SerArchiveT& serT)
  {
      serT & ip_addr;
      serT & port;
      serT & provider_id;
  }

  std::string & getIPasDottedString ( std::string & a_string ) const
  {
  
  char buffer[INET_ADDRSTRLEN];
  // convert ip from host to network byte order uint32_t
  uint32_t ip_net_order = htonl(ip_addr); 
  // convert network byte order uint32_t to a dotted string
  if (NULL != inet_ntop(AF_INET, &ip_net_order, buffer, INET_ADDRSTRLEN))
   {a_string += std::string(buffer); }
  return a_string;
  }
};

class KeeperRegistrationMsg
{

   KeeperIdCard	keeperIdCard; 
   ServiceId	adminServiceId; 

public:


  KeeperRegistrationMsg ( KeeperIdCard const & keeper_card = KeeperIdCard{0,0,0}
		    ,	ServiceId const & admin_service_id = ServiceId{0,0,0})
	: keeperIdCard(keeper_card)
        , adminServiceId(admin_service_id)
	{} 

  ~KeeperRegistrationMsg()=default;

  KeeperIdCard const& getKeeperIdCard() const 
  { return keeperIdCard; }

  ServiceId const& getAdminServiceId() const 
  { return adminServiceId; }

  template <typename SerArchiveT>
  void serialize( SerArchiveT& serT)
  {
      serT & keeperIdCard;
      serT & adminServiceId;
  }

};

}//namespace

std::ostream & operator << (std::ostream & out, chronolog::ServiceId const serviceId)
{
   std::string a_string;
   out <<"{"<< serviceId.getIPasDottedString(a_string)<<":"<<serviceId.port<<":"<<serviceId.provider_id<<"}";
   return out;
}

std::ostream & operator << (std::ostream & out, chronolog::KeeperRegistrationMsg const& msg)
{
  out<<"KeeperRegistrationMsg{"<<msg.getKeeperIdCard()<<"}{admin:"<<msg.getAdminServiceId()<<"}";
  return out;
}

#endif
