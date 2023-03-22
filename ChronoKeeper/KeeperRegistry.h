#ifndef KEEPER_REGISTRY_H
#define KEEPER_REGISTRY_H

#include <mutex>
#include <vector>
#include <map>
#include <chrono>
#include <thallium.hpp>

#include "chronolog_types.h"
#include "KeeperIdCard.h"
#include "KeeperStatsMsg.h"
#include "KeeperRegistrationMsg.h"

namespace chronolog
{

struct RegistryConfiguration
{
	std::string REGISTRY_SERVICE_PROTOCOL;
	std::string REGISTRY_SERVICE_IP;
	std::string    REGISTRY_SERVICE_PORT;
	uint16_t    REGISTRY_SERVICE_PROVIDER_ID;
	uint16_t    SERVICE_THREAD_COUNT;
};

class DataStoreAdminClient;
class KeeperRegistryService;

class KeeperRegistry
{

struct KeeperProcessEntry
{
public:
	KeeperProcessEntry(KeeperIdCard const& keeper_id_card, ServiceId const& admin_service_id)
		: idCard(keeper_id_card)
		, adminServiceId(admin_service_id)
		, keeperAdminClient(nullptr)  
		, lastStatsTime(0)
		, activeStoryCount(0)

	{}
	
	KeeperProcessEntry(KeeperProcessEntry const& other)  = default;

	void reset()
	{
	    keeperAdminClient=nullptr;  
	    lastStatsTime=0;
	    activeStoryCount=0;
	}

	~KeeperProcessEntry() = default;   // Registry is reponsible for creating & deleting keeperAdminClient

	KeeperIdCard 	idCard;
	ServiceId 	adminServiceId;
	DataStoreAdminClient * keeperAdminClient;
	uint64_t	lastStatsTime;
	uint32_t	activeStoryCount;
};

enum RegistryState
{
	UNKNOWN = 0,
	INITIALIZED =1, // RegistryService is initialized, no active keepers 
	RUNNING	=2,	// RegistryService and active Keepers
	SHUTTING_DOWN=3	// Shutting down services
};

public:
	KeeperRegistry( thallium::engine & the_engine)
	   : registryState(UNKNOWN)
	   , registryEngine(the_engine)
	   , keeperRegistryService(nullptr)
	{}

	~KeeperRegistry();

	bool is_initialized() const 
	{ return (INITIALIZED == registryState); }

	bool is_running() const 
	{ return (RUNNING == registryState); }

	bool is_shutting_down() const 
	{ return (SHUTTING_DOWN == registryState); }

	int InitializeRegistryService(uint16_t service_provider_id);

	int ShutdownRegistryService();

	int registerKeeperProcess( KeeperRegistrationMsg const& keeper_reg_msg);

	int unregisterKeeperProcess( KeeperIdCard const & keeper_id_card);

        void updateKeeperProcessStats(KeeperStatsMsg const & keeperStatsMsg); 

	std::vector<KeeperIdCard> & getActiveKeepers( std::vector<KeeperIdCard> & keeper_id_cards);

	int notifyKeepersOfStoryRecordingStart( std::vector<KeeperIdCard> const&,
			ChronicleName const&, StoryName const&, StoryId const&);

	int notifyKeepersOfStoryRecordingStop(std::vector<KeeperIdCard> const&, StoryId const&);


private:

	KeeperRegistry(KeeperRegistry const&) = delete; //disable copying
	KeeperRegistry & operator= (KeeperRegistry const&) = delete;
	
	RegistryState	registryState;
	std::mutex registryLock;
	std::map<std::pair<uint32_t,uint16_t>, KeeperProcessEntry> keeperProcessRegistry;
	thallium::engine & registryEngine;
        KeeperRegistryService	* keeperRegistryService;
};

}

#endif
