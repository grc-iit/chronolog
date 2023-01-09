#ifndef KEEPER_ID_CARD_H
#define KEEPER_ID_CARD_H



namespace chronolog
{

class KeeperIdCard
{

   uint64_t keeperId;
   uint32_t ip_addr;
   uint16_t port;

public:


  KeeperIdCard( uint64_t uid = 0, uint32_t addr = 0, uint16_t a_port=0)
	: keeperId(uid), ip_addr(addr), port(a_port)
	{} 

  KeeperIdCard( KeeperIdCard const& other)
  	: keeperId(other.getId()), ip_addr(other.getIPaddr()), port(other.getPort())
  	{}

  ~KeeperIdCard()=default;

  uint64_t getId() const { return keeperId; }
  uint32_t getIPaddr() const {return ip_addr; }
  uint16_t getPort() const { return port;}

  template <typename SerArchiveT>
  void serialize( SerArchiveT & serT)
  {
      serT & keeperId;
      serT & ip_addr;
      serT & port;	
  }

};


}

#endif
