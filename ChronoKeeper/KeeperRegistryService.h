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
    
    KeeperRegistryService(KeeperRegistryService const&) =delete;
    KeeperRegistryService & operator= (KeeperRegistryService const&) =delete;


    void register_keeper(tl::request const& request, chronolog::KeeperIdCard const& keeper_id_card)
    {
	
	int return_code = 0;

        std::cout << "register_keeper:"<< keeper_id_card <<std::endl;

	return_code = theKeeperProcessRegistry.registerKeeperProcess(keeper_id_card);

	request.respond(return_code);
    }
    void unregister_keeper(tl::request const& request, chronolog::KeeperIdCard const& keeper_id_card)
    {
	
	int return_code = 0;

        std::cout << "unregister_keeper:" << keeper_id_card<<std::endl;
	return_code = theKeeperProcessRegistry.unregisterKeeperProcess(keeper_id_card);
	
	request.respond(return_code);
    }

    void handle_stats_msg(tl::request const& request, chronolog::KeeperStatsMsg const& keeper_stats_msg) 
    {
        std::cout << "handle_keeper_stats " << keeper_stats_msg<< std::endl;
	theKeeperProcessRegistry.updateKeeperProcessStats(keeper_stats_msg);
    }

    KeeperRegistry &  theKeeperProcessRegistry;

    public:

    KeeperRegistryService(tl::engine& tl_engine, uint16_t service_provider_id
		    , KeeperRegistry & keeperRegistry)
       : tl::provider<KeeperRegistryService>(tl_engine, service_provider_id)
       , theKeeperProcessRegistry(keeperRegistry)
    {
	define("register_keeper", &KeeperRegistryService::register_keeper);
	define("unregister_keeper", &KeeperRegistryService::unregister_keeper);
        define("handle_stats_msg", &KeeperRegistryService::handle_stats_msg, tl::ignore_return_value());
    }

    ~KeeperRegistryService() {
        std::cout<<"KeeperRegistryService::destructor"<<std::endl;
        get_engine().wait_for_finalize();
    }
};

/*INNA: TODO list : 
1. make constructor private and add static create function , so that service is always created on the HEAP and nothte stack
2. add a way to either pass in or locate the instance of KeeperRegistry instance 
*/

}// namespace chronolog

// uncomment to test this class as standalone process
#ifdef INNA
int main(int argc, char** argv) {

    uint16_t provider_id = 25;

    margo_instance_id margo_id=margo_init("ofi+sockets://127.0.0.1:1234",MARGO_SERVER_MODE, 1, 0);
    //margo_instance_id margo_id=margo_init("tcp",MARGO_SERVER_MODE, 1, 0);

    if(MARGO_INSTANCE_NULL == margo_id)
    {
      std::cout<<"FAiled to initialise margo_instance"<<std::endl;
      return 1;
    }
    std::cout<<"KeeperRegistryService:margo_instance initialized"<<std::endl;

   tl::engine keeper_reg_engine(margo_id);
 
    std::cout << "Starting KeeperRegService  at address " << keeper_reg_engine.self()
        << " with provider id " << provider_id << std::endl;

    chronolog::KeeperRegistryService keeperRegistryService(keeper_reg_engine,provider_id);
    
    return 0;
}
#endif

#endif
