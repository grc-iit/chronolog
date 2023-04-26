
#ifndef RPC_VISOR_CLIENT_H
#define RPC_VISOR_CLIENT_H

#include <string>
#include <unordered_map>
#include <iostream>
#include <thallium.hpp>
#include <thallium/serialization/stl/string.hpp>

#include "error.h"
//#include "../chrono_common/chronolog_types.h"

namespace tl = thallium;


namespace chronolog
{


class RpcVisorClient
{

public:
    static RpcVisorClient * CreateRpcVisorClient( tl::engine & tl_engine,
		    std::string const & service_addr, uint16_t provider_id )
    	{
	   try{
	      return new RpcVisorClient( tl_engine,service_addr, provider_id);
	   } catch( tl::exception const&)
	   {
		std::cout<<"KeeperRecordingClient: failed construction"<<std::endl;
		return nullptr;
	   }
        }


      int Connect(std::string const& uri, std::string const& client_id, int &flags)
		      //, uint64_t &clock_offset) 
      {
 //       LOGD("%s in ChronoLogAdminRPCProxy at addresss %p called in PID=%d, with args: uri=%s, client_id=%s",
   //          __FUNCTION__, this, getpid(), uri.c_str(), client_id.c_str());
        //return CHRONOLOG_RPC_CALL_WRAPPER("Connect", 0, int, uri, client_id, flags, clock_offset);
	visor_connect.on(service_ph)();
	return CL_SUCCESS;
      }

    int Disconnect(std::string const& client_id)
    {
     //   LOGD("%s is called in PID=%d, with args: client_id=%s, flags=%d",
       //      __FUNCTION__, getpid(), client_id.c_str(), flags);
	visor_disconnect.on(service_ph)();
	return CL_SUCCESS;
    }

    int CreateChronicle(std::string const& name,
                        const std::unordered_map<std::string, std::string> &attrs,
                        int &flags) {
//        LOGD("%s is called in PID=%d, with args: name=%s, flags=%d, attrs=",
  //           __FUNCTION__, getpid(), name.c_str(), flags);
        return CL_SUCCESS; //CHRONOLOG_RPC_CALL_WRAPPER("CreateChronicle", 0, int, name, attrs, flags);
    }

    int DestroyChronicle(std::string const& name)
    {
        //LOGD("%s is called in PID=%d, with args: name=%s, flags=%d", __FUNCTION__, getpid(), name.c_str(), flags);
        return CL_SUCCESS; //CHRONOLOG_RPC_CALL_WRAPPER("DestroyChronicle", 0, int, name, flags);
    }

    int DestroyStory(std::string const& chronicle_name, std::string const& story_name)
    {
        //LOGD("%s is called in PID=%d, with args: chronicle_name=%s, story_name=%s, flags=%d",
          //   __FUNCTION__, getpid(), chronicle_name.c_str(), story_name.c_str(), flags);
        return CL_SUCCESS; //CHRONOLOG_RPC_CALL_WRAPPER("DestroyStory", 0, int, chronicle_name, story_name, flags);
    }


    ~RpcVisorClient()
    {
           visor_connect.deregister(); 
           visor_disconnect.deregister(); 
	   create_chronicle.deregister();
	   destroy_chronicle.deregister();
	   acquire_story.deregister();
	   release_story.deregister();
	   destroy_story.deregister();
    }

    private:


    std::string service_addr;     // na address of ChronoVisor ClientService  
    uint16_t 	service_provider_id;          // ChronoVisor ClientService provider_id id
    tl::provider_handle  service_ph;  //provider_handle for client registry service
    tl::remote_procedure visor_connect;
    tl::remote_procedure visor_disconnect;
    tl::remote_procedure create_chronicle;
    tl::remote_procedure destroy_chronicle;
    tl::remote_procedure acquire_story;
    tl::remote_procedure release_story;
    tl::remote_procedure destroy_story;

    RpcVisorClient(RpcVisorClient const&) = delete;
    RpcVisorClient& operator= (RpcVisorClient const&) = delete;


    // constructor is private to make sure thalium rpc objects are created on the heap, not stack
    RpcVisorClient( tl::engine & tl_engine, std::string const& service_addr, uint16_t provider_id)
	    : service_addr(service_addr), service_provider_id(provider_id)
	    , service_ph(tl_engine.lookup( service_addr),provider_id)
	{
	 std::cout<<" RpcVisorClient created for Visor Service at {"<<service_addr<<"} provider_id {"<<service_provider_id<<"}"<<std::endl;
   	 visor_connect =tl_engine.define("Connect");
   	 visor_disconnect =tl_engine.define("Disconnect");
       
	}	
};

}

#endif
