#ifndef KEEPER_STATS_MSG_H
#define KEEPER_STATS_MSG_H

#include "KeeperIdCard.h"


namespace chronolog
{

class KeeperStatsMsg
{

   KeeperIdCard	keeperIdCard; 
   uint32_t active_story_count;

public:


  KeeperIdStatsMsg ( KeeperIdCard const & keeper_card=KeeperIdCard(0,0,0)
		    ,	uint32_t count=0)
	: keeperIdCard(keeper_card) 
        , active_story_count(count)
	{} 

  ~KeeperStatsMsg()=default;

  typename <SerializerT>
  void serialize( SerializerT& serT)
  {
      serT & keeperIdCard;
      serT & ip_addr;
      serT & port;	
  }

};


}

#endif
