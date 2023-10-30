#ifndef __KeyValueStoreMDS_H_
#define __KeyValueStoreMDS_H_

#include "blockmap.h"
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
#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <cassert>
#include "KeyValueStoreMetadata.h"
#include "data_server_client.h"
#include "util_t.h"




namespace tl=thallium;

class KeyValueStoreMDS
{
     private :
	      int myrank;
	      int numprocs;
	      BlockMap<std::string,KeyValueStoreMetadata*,stringhash,stringequal> *directory;
	      memory_pool<std::string,KeyValueStoreMetadata*,stringhash,stringequal> *t_pool;
              tl::engine *thallium_server;
              tl::engine *thallium_shm_server;
              tl::engine *thallium_client;
              tl::engine *thallium_shm_client;
              std::vector<tl::endpoint> serveraddrs;
              std::vector<std::string> ipaddrs;
              std::vector<std::string> shmaddrs;
              std::string myipaddr;
              std::string myhostname;
	      int nservers;
	      int serverid;

     public :

	    KeyValueStoreMDS(int np,int r) : numprocs(np),myrank(r)
	    {
		nservers = numprocs;
		serverid = myrank;
		if(serverid==0)
		{
		  t_pool = new memory_pool<std::string,KeyValueStoreMetadata*,stringhash,stringequal> (100);
		  std::string emptykey = "";
		  directory = new BlockMap<std::string,KeyValueStoreMetadata*,stringhash,stringequal> (1024,t_pool,emptykey);
		}
		else
		{
		   t_pool = nullptr;
		   directory = nullptr;

		}
	    }
	     void server_client_addrs(tl::engine *t_server,tl::engine *t_client,tl::engine *t_server_shm, tl::engine *t_client_shm,std::vector<std::string> &ips,std::vector<std::string> &shm_addrs,std::vector<tl::endpoint> &saddrs)
            {
           	thallium_server = t_server;
           	thallium_shm_server = t_server_shm;
           	thallium_client = t_client;
           	thallium_shm_client = t_client_shm;
           	ipaddrs.assign(ips.begin(),ips.end());
           	shmaddrs.assign(shm_addrs.begin(),shm_addrs.end());
           	myipaddr = ipaddrs[serverid];
           	serveraddrs.assign(saddrs.begin(),saddrs.end());
   	    }

	    void bind_functions()
	    {

	       std::function<void(const tl::request &,std::string &,std::vector<std::string> &)> insertFunc(
               std::bind(&KeyValueStoreMDS::ThalliumLocalInsert,this,std::placeholders::_1,std::placeholders::_2,std::placeholders::_3));
	       std::function<void(const tl::request &,std::string &)> findFunc(
	       std::bind(&KeyValueStoreMDS::ThalliumLocalFind,this,std::placeholders::_1,std::placeholders::_2));

	       std::function<void(const tl::request &,std::string &)> getFunc(
	       std::bind(&KeyValueStoreMDS::ThalliumLocalGet,this,std::placeholders::_1,std::placeholders::_2));	       

               thallium_server->define("RemoteInsertMDS",insertFunc);
               thallium_server->define("RemoteFindMDS",findFunc);
               thallium_server->define("RemoteGetMDS",getFunc);
               thallium_shm_server->define("RemoteInsertMDS",insertFunc);
               thallium_shm_server->define("RemoteFindMDS",findFunc);
               thallium_shm_server->define("RemoteGetMDS",getFunc);
	    }

	    bool LocalInsert(std::string &s,std::vector<std::string> &k)
	    {
		KeyValueStoreMetadata *m = new KeyValueStoreMetadata();
		m->unpackmetadata(k);
		int ret = directory->insert(s,m);
		if(ret == INSERTED) 
		{
			return true;
		}
		else 
		{
		   delete m;
		   return false;
		}
	    }

	    bool LocalFind(std::string &s)
	    {
		int k = 1;
		int ret = directory->find(s);
		if(ret != NOT_IN_TABLE) return true;
		else return false;
	    }
	    std::vector<std::string> LocalGet(std::string &s)
	    {
		KeyValueStoreMetadata *k;
		bool b = directory->get(s,&k);
		std::vector<std::string> metadata;
		if(b) k->packmetadata(metadata);
		return metadata;
	    }

	    void ThalliumLocalInsert(const tl::request &req,std::string &s,std::vector<std::string> &k)
	    {
		req.respond(LocalInsert(s,k));
	    }

	    void ThalliumLocalFind(const tl::request &req,std::string &s)
	    {
		req.respond(LocalFind(s));
	    }
	    
	    void ThalliumLocalGet(const tl::request &req,std::string &s)
	    {
		req.respond(LocalGet(s));
	    }

	    bool Insert(std::string &s,std::vector<std::string> &k)
	    {
		int destid = 0;
		if(ipaddrs[destid].compare(myipaddr)==0)
                {
                    tl::endpoint ep = thallium_shm_client->lookup(shmaddrs[destid]);
                    tl::remote_procedure rp = thallium_shm_client->define("RemoteInsertMDS");
                    return rp.on(ep)(s,k);
                }
                else
                {
                    tl::remote_procedure rp = thallium_client->define("RemoteInsertMDS");
                    return rp.on(serveraddrs[destid])(s,k);
		}
	    }

	    bool Find(std::string &s)
	    {
		int destid = 0;
		if(ipaddrs[destid].compare(myipaddr)==0)
		{
		   tl::endpoint ep = thallium_shm_client->lookup(shmaddrs[destid]);
		   tl::remote_procedure rp = thallium_shm_client->define("RemoteFindMDS");
		   return rp.on(ep)(s);
		}
		else
		{
		   tl::remote_procedure rp = thallium_client->define("RemoteFindMDS");
		   return rp.on(serveraddrs[destid])(s);
		}
	    }
	    std::vector<std::string> Get(std::string &s)
	    {
		int destid = 0;
		if(ipaddrs[destid].compare(myipaddr)==0)
		{
		    tl::endpoint ep = thallium_shm_client->lookup(shmaddrs[destid]);
		    tl::remote_procedure rp = thallium_shm_client->define("RemoteGetMDS");
		    return rp.on(ep)(s);
		}
		else
		{
		    tl::remote_procedure rp = thallium_client->define("RemoteGetMDS");
		    return rp.on(serveraddrs[destid])(s);
		}
	    }

	    ~KeyValueStoreMDS()
	    {
		if(t_pool != nullptr) delete t_pool;
		if(directory != nullptr) 
		{
			std::vector<KeyValueStoreMetadata*> values;
			directory->get_map(values);
			for(int i=0;i<values.size();i++)
				delete values[i];	
			delete directory;
		}
	    }


};

#endif
