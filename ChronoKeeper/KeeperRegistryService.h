#ifndef KEEPER_REGISTRY_SERVICE_H
#define KEEPER_REGISTRY_SERVICE_H

#include <iostream>
#include <margo.h>
#include <thallium.hpp>
#include <thallium/serialization/stl/string.hpp>

#include "KeeperIdCard.h"
#include "KeeperStatsMsg.h"

#include "KeeperRegistry.h"

namespace tl = thallium;


namespace chronolog
{


class KeeperRegistryService : public tl::provider<KeeperRegistryService> 
{

private:
    
    void register_keeper(tl::request const& request, chronolog::KeeperStatsMsg const& keeperStatsMsg)
    {
	
	int return_code = 0;

        std::cout << "register_keeper:"<< keeperStatsMsg <<std::endl;

	return_code = theKeeperProcessRegistry.registerKeeperProcess(keeperStatsMsg);

	request.respond(return_code);
    }

    void unregister_keeper(tl::request const& request, chronolog::KeeperIdCard const& keeper_id_card)
    {
	
	int return_code = 0;

        std::cout << "unregister_keeper:" << keeper_id_card<<std::endl;
	return_code = theKeeperProcessRegistry.unregisterKeeperProcess(keeper_id_card);
	
	request.respond(return_code);
    }

    void handle_stats_msg( chronolog::KeeperStatsMsg const& keeper_stats_msg) 
    {
        std::cout << "handle_keeper_stats " << keeper_stats_msg<< std::endl;
	theKeeperProcessRegistry.updateKeeperProcessStats(keeper_stats_msg);
    }

    KeeperRegistry &  theKeeperProcessRegistry;


    KeeperRegistryService(tl::engine& tl_engine, uint16_t service_provider_id
		    , KeeperRegistry & keeperRegistry)
       : tl::provider<KeeperRegistryService>(tl_engine, service_provider_id)
       , theKeeperProcessRegistry(keeperRegistry)
    {
	define("register_keeper", &KeeperRegistryService::register_keeper);
	define("unregister_keeper", &KeeperRegistryService::unregister_keeper);
        define("handle_stats_msg", &KeeperRegistryService::handle_stats_msg, tl::ignore_return_value());
	//setup finalization callback in case this ser vice provider is still alive when the engine is finalized 
	get_engine().push_finalize_callback(this, [p=this]() {delete p;});
    }

    KeeperRegistryService(KeeperRegistryService const&) =delete;
    KeeperRegistryService & operator= (KeeperRegistryService const&) =delete;


    public:

    static KeeperRegistryService * CreateKeeperRegistryService(tl::engine& tl_engine, uint16_t service_provider_id
		    , KeeperRegistry & keeperRegistry)
    {
	    return new KeeperRegistryService(tl_engine, service_provider_id, keeperRegistry);
    }

    ~KeeperRegistryService() {
        std::cout<<"KeeperRegistryService::destructor"<<std::endl;
        get_engine().pop_finalize_callback(this);
    }
};


}// namespace chronolog

#endif
