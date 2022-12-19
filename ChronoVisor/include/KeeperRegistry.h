#ifndef CHRONOLOG_KEEPER_REGISTRY_H
#define CHRONOLOG_KEEPER_REGISTRY_H


#include <ChronoKeeperIdCard.h>


namespace chronolog
{


struct KeeperStats
{
public:
   uint64_t    lastHeartbeatTime;
   uint64_t    keeperTime;
   uint64_t    localStorageCapacity;
   uint64_t    usedStorage;
   uint64_t    activeStories;
};


template <typename RPCChannel>
class KeeperRecord
{
public:
	KeeperRecord( KeeperIdCard const&, KeeperStats const &);
	~KeeperRecord();	

private:
	KeeperProcessId 	keeperProcess;
	KeeperInstanceId	keeperGroup;
        KeeperStats keeperStatistics;
        //ip*port for responses or an RPC channel instance 
	RPCChannel		keeperAdminRPCChannel;
};



template <typename RPCChannel>
class ChronoKeeperRegistry
{




private:
	std::map<KeeperProcessId, KeeperRecord<RPCChannel> > keeperRegistryMap;
 
};

}//namespace cronolog

#endif CHRONOLOG_KEEPER_REGISTRY_H
