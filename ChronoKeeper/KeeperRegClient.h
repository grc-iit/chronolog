
#include <iostream>
#include <thallium/serialization/stl/string.hpp>
#include <thallium.hpp>

#include 
namespace tl = thallium;


namespace chronolog
{


class KeeperRegistryClient
{

    static KeeperRegistryClient * CreateKeeperRegistryClient( std::string const & registry_service_addr = "ofi+sockets://127.0.0.1:1234"
		       uint16_t registry_provider_id = 25)
    	{
	      return new KeeperRegistryClient( registry_service_addr, registry_provider_id);
        }


    

    private:

    // constructor is private to make sure thalium rpc objects are created on the heap, not stack
    KeeperRegistryClient( std::string const& registry_addr, uint16_t registry_provider_id)
	    : registry_addr(registry_addr), provider_id(registry_provider_id)
    {
	    initialize_rpc_client();
    }


    //TODO : INNA add exception catching and error checks here ...
    void initialize_rpc_client()
    {
    tl::engine myEngine("ofi+sockets", THALLIUM_CLIENT_MODE);
    tl::remote_procedure register_keeper  = myEngine.define("register_keeper");
    tl::remote_procedure unregister_keeper  = myEngine.define("unregister_keeper");
    tl::remote_procedure handle_stats_msg = myEngine.define("handle_stats_msg").disable_response();
    tl::endpoint server = myEngine.lookup(registry_addr, provider_id);
    tl::provider_handle ph(server, provider_id);
    }


    std::string na_registry_addr;     // na address of Keeper Registry Service 
    uint16_t 	provider_id;          // KeeperRegistryService provider id
};

}
