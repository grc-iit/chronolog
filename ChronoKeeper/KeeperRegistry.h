#ifndef KEEPER_REGISTRY_H
#define KEEPER_REGISTRY_H

#include <mutex>
#include <vector>
#include <map>
#include <chrono>

#include "KeeperIdCard.h"
#include "KeeperStatsMsg.h"


namespace chronolog
{

typedef uint64_t StoryId;

class KeeperRegistry
{
public:

struct KeeperProcessEntry
{
	KeeperProcessEntry(KeeperIdCard const& keeper_id_card)
		: idCard(keeper_id_card)
		, lastStatsTime(0)
		, activeStoryCount(0)
	{}

	KeeperIdCard idCard;
	uint64_t	lastStatsTime;
	uint32_t	activeStoryCount;
};

	KeeperRegistry(){}
	~KeeperRegistry()=default;

	int registerKeeperProcess( KeeperStatsMsg const& keeperStatsMsg)
	{
	   KeeperIdCard keeper_id_card = keeperStatsMsg.getKeeperIdCard();

	   std::lock_guard<std::mutex> lock_guard(registryLock);
	   auto insert_return = keeperProcessRegistry.insert( std::pair<std::pair<uint32_t,uint16_t>,KeeperProcessEntry>
		( std::pair<uint32_t,uint16_t>(keeper_id_card.getIPaddr(),keeper_id_card.getPort()),
			   KeeperProcessEntry(keeper_id_card) )
		);
	   if( false == insert_return.second)
	   { //log msg here , insert_return.first is the position to the current keeper entry the same KeeperIdCard...
		   return 0;
	   }

	   KeeperProcessEntry keeper_process = (*(insert_return.first)).second;
	   //keeper_process.lastStatsTime = std::chrono::steady_clock::now();
	   keeper_process.activeStoryCount = keeperStatsMsg.getActiveStoryCount();
	   return 1;
	}

	int unregisterKeeperProcess( KeeperIdCard const & keeper_id_card) 
       {

	   std::lock_guard<std::mutex> lock_guard(registryLock);
	   keeperProcessRegistry.erase(std::pair<uint32_t,uint16_t>(keeper_id_card.getIPaddr(),keeper_id_card.getPort()));
	   return 1;
	}

        void updateKeeperProcessStats(KeeperStatsMsg const & keeperStatsMsg) 
	{
	   KeeperIdCard keeper_id_card = keeperStatsMsg.getKeeperIdCard();

	   std::lock_guard<std::mutex> lock_guard(registryLock);
	   auto keeper_process_iter = keeperProcessRegistry.find(std::pair<uint32_t,uint16_t>(keeper_id_card.getIPaddr(),keeper_id_card.getPort()));
	   if(keeper_process_iter == keeperProcessRegistry.end())
	   {    // however unlikely it is that the stats msg would be delivered for the keeper that's already unregistered
		// we should probably log a warning here...
		return;
	   }
   	   KeeperProcessEntry keeper_process = (*keeper_process_iter).second;
	  // keeper_process.lastStatsTime = std::chrono::steady_clock::now();
	   keeper_process.activeStoryCount = keeperStatsMsg.getActiveStoryCount();

	}

	std::vector<KeeperIdCard> & getActiveKeepers( std::vector<KeeperIdCard> & keeper_id_cards) 
	{  //the process of keeper selection will probably get more nuanced; 
	   //for now just return all the keepers registered

	   keeper_id_cards.clear();
	   std::lock_guard<std::mutex> lock_guard(registryLock);
	   for (auto keeperProcess : keeperProcessRegistry )
		   keeper_id_cards.push_back(keeperProcess.second.idCard);

	   return keeper_id_cards;
	}

	int notifyKeepersOfStoryAcquisition(StoryId const&);
	int notifyKeepersOfStoryRelease(StoryId const&);

private:

	KeeperRegistry(KeeperRegistry const&) = delete; //disable copying
	KeeperRegistry & operator= (KeeperRegistry const&) = delete;

	std::mutex registryLock;
	std::map<std::pair<uint32_t,uint16_t>, KeeperProcessEntry> keeperProcessRegistry;
};

}

#endif
