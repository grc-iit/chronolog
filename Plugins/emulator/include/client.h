#ifndef __CLIENT_H_
#define __CLIENT_H_
#include <thallium.hpp>
#include <thallium/serialization/proc_input_archive.hpp>
#include <thallium/serialization/proc_output_archive.hpp>
#include <thallium/serialization/serialize.hpp>
#include <thallium/serialization/stl/array.hpp>
#include <thallium/serialization/stl/complex.hpp>
#include <thallium/serialization/stl/deque.hpp>
#include <thallium/serialization/stl/forward_list.hpp>
#include <thallium/serialization/stl/list.hpp>
#include <thallium/serialization/stl/map.hpp>
#include <thallium/serialization/stl/multimap.hpp>
#include <thallium/serialization/stl/multiset.hpp>
#include <thallium/serialization/stl/pair.hpp>
#include <thallium/serialization/stl/set.hpp>
#include <thallium/serialization/stl/string.hpp>
#include <thallium/serialization/stl/tuple.hpp>
#include <thallium/serialization/stl/unordered_map.hpp>
#include <thallium/serialization/stl/unordered_multimap.hpp>
#include <thallium/serialization/stl/unordered_multiset.hpp>
#include <thallium/serialization/stl/unordered_set.hpp>
#include <thallium/serialization/stl/vector.hpp>
#include <vector>

namespace tl=thallium;


class metadata_client
{

   private:
	   std::string serveraddr;
	   std::vector<tl::endpoint> endpoint;
	   tl::engine *thallium_client; 

   public:
	   metadata_client(std::string &s) : serveraddr(s)
	   {

		thallium_client = new tl::engine("ofi+sockets",THALLIUM_CLIENT_MODE,true,1);
		tl::endpoint ep = thallium_client->lookup(serveraddr.c_str());
	        endpoint.push_back(ep);	
	   }

	   ~metadata_client()
	   {
	       delete thallium_client;
	   }

	   tl::engine *getClient()
	   {
		   return thallium_client;
	   }
	   std::vector<tl::endpoint> &getEndpoints()
	   {
		   return endpoint;
	   }
	   void Connect(std::string &client_id)
	   {
		tl::remote_procedure rp = thallium_client->define("connect");
		rp.on(endpoint[0])(client_id);
	   }

	   void Disconnect(std::string &client_id)
	   {
		tl::remote_procedure rp = thallium_client->define("disconnect");
		rp.on(endpoint[0])(client_id);
	   }
	
	   void CreateChronicle(std::string &client_id,std::string &chronicle_name)
	   {
		tl::remote_procedure rp = thallium_client->define("createchronicle");
		rp.on(endpoint[0])(client_id,chronicle_name);
	   }
	   void DestroyChronicle(std::string &client_id,std::string &chronicle_name)
	   {
		 tl::remote_procedure rp = thallium_client->define("destroychronicle");
		 rp.on(endpoint[0])(client_id,chronicle_name);
	   }
	   void AcquireChronicle(std::string &client_id,std::string &chronicle_name)
	   {
		tl::remote_procedure rp = thallium_client->define("acquirechronicle");
		rp.on(endpoint[0])(client_id,chronicle_name);
	   }
	   void ReleaseChronicle(std::string &client_id,std::string &chronicle_name)
	   {
		tl::remote_procedure rp = thallium_client->define("releasechronicle");
		rp.on(endpoint[0])(client_id,chronicle_name);
	   }
	   void CreateStory(std::string &client_id,std::string &chronicle_name,std::string &story_name)
	   {
		tl::remote_procedure rp = thallium_client->define("createstory");
		rp.on(endpoint[0])(client_id,chronicle_name,story_name);
	   }
	   void DestroyStory(std::string &client_id,std::string &chronicle_name,std::string &story_name)
	   {
		tl::remote_procedure rp = thallium_client->define("destroystory");
		rp.on(endpoint[0])(client_id,chronicle_name,story_name);
	   }
	   void AcquireStory(std::string &client_id,std::string &chronicle_name,std::string &story_name)
	   {
		tl::remote_procedure rp = thallium_client->define("acquirestory");
		rp.on(endpoint[0])(client_id,chronicle_name,story_name);
	   }
	   void ReleaseStory(std::string &client_id,std::string &chronicle_name,std::string &story_name)
	   {
		tl::remote_procedure rp = thallium_client->define("releasestory");
		rp.on(endpoint[0])(client_id,chronicle_name,story_name);
	   }
};


#endif
