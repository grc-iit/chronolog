#ifndef KEEPER_REGISTRY_H
#define KEEPER_REGISTRY_H

#include <mutex>
#include <vector>
#include <map>

#include "KeeperIdCard.h"
#include "KeeperStatsMsg.h"


namespace chronolog
{


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

	int registerKeeperProcess( KeeperIdCard const& keeper_id_card)
	{ 
	   std::lock_guard<std::mutex> lock_guard(registryLock);
	   keeperProcessRegistry.insert( std::pair<std::pair<uint32_t,uint16_t>,KeeperProcessEntry>
		( std::pair<uint32_t,uint16_t>(keeper_id_card.getIPaddr(),keeper_id_card.getPort()),
			   KeeperProcessEntry(keeper_id_card) )
		);
	   return 1;
	}

	int unregisterKeeperProcess( KeeperIdCard const & keeper_id_card) 
       {

	   std::lock_guard<std::mutex> lock_guard(registryLock);
	   keeperProcessRegistry.erase(std::pair<uint32_t,uint16_t>(keeper_id_card.getIPaddr(),keeper_id_card.getPort()));
	   return 1;
	}

        void updateKeeperProcessStats(KeeperStatsMsg const &) {}

	std::vector<KeeperIdCard> const& getActiveKeepers() const
	{ return std::vector<KeeperIdCard>(); }	


private:

	KeeperRegistry(KeeperRegistry const&) = delete; //disable copying
	KeeperRegistry & operator= (KeeperRegistry const&) = delete;

	std::mutex registryLock;
	std::map<std::pair<uint32_t,uint16_t>, KeeperProcessEntry> keeperProcessRegistry;
};

}

#endif
