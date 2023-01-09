#include <iostream>
#include <margo.h>
#include <thallium.hpp>
#include <thallium/serialization/stl/string.hpp>

#include "KeeperIdCard.h"
#include "KeeperStatsMsg.h"

namespace tl = thallium;


namespace chronolog
{


class KeeperRegistryService : public tl::provider<KeeperRegistryService> 
{

    private:


    void register_keeper(tl::request const& request, chronolog::KeeperIdCard const&)
    {
	
	int return_code = 0;

        std::cout << "register_keeper"<<std::endl;
	
	request.respond(return_code);
    }
    void unregister_keeper(tl::request const& request, chronolog::KeeperIdCard const&)
    {
	
	int return_code = 0;

        std::cout << "unregister_keeper"<<std::endl;
	
	request.respond(return_code);
    }

    void handle_stats_msg(tl::request const& request, const std::string& keeper_stats_msg) 
    {
	int return_code = 0;
        std::cout << "handle_keeper_stats " << keeper_stats_msg<< std::endl;
    }

    public:

    KeeperRegistryService(tl::engine& tl_engine, uint16_t service_provider_id)
    : tl::provider<KeeperRegistryService>(tl_engine, service_provider_id) {
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
int main(int argc, char** argv) {

    uint16_t provider_id = 22;

    margo_instance_id margo_id=margo_init("ofi+sockets",MARGO_SERVER_MODE, 1, 0);
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

