#ifndef __HASHMAP_H_
#define __HASHMAP_H_

#include "block_map.h" 
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
#include "event.h"
#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <cassert>
#include "ClockSync.h"

namespace tl=thallium;

template<typename A>
void serialize(A &ar,event &e)
{
	ar & e.ts;
	ar & e.data;
}

template <class KeyT,
          class ValueT,
          class HashFcn=std::hash<KeyT>,
          class EqualFcn=std::equal_to<KeyT>>
class distributed_hashmap
{
   public :
      typedef BlockMap<KeyT,ValueT,HashFcn,EqualFcn> map_type;
      typedef memory_pool<KeyT,ValueT,HashFcn,EqualFcn> pool_type;

 private:
        std::vector<uint64_t> totalSizes;
	std::vector<uint64_t> maxSizes;
        uint32_t nservers;
        uint32_t serverid;
	int numcores;
	std::vector<std::pair<uint64_t,uint64_t>> range;
	std::vector<KeyT> emptyKeys;
	std::vector<pool_type *>pls;
	std::vector<map_type *>my_tables;
	BlockMap<std::string,int> *table_names;
	memory_pool<std::string,int> *t_pool;
	tl::engine *thallium_server;
	tl::engine *thallium_shm_server;
	tl::engine *thallium_client;
	tl::engine *thallium_shm_client;
	std::vector<tl::endpoint> serveraddrs;
	std::vector<std::string> ipaddrs;
	std::vector<std::string> shmaddrs;
	std::string myipaddr;
	std::string myhostname;
	ClockSynchronization<ClocksourceCPPStyle> *CM;
	std::atomic<int> dropped_events;
	double time_m;
	int maxtables;
	std::atomic<int> num_tables;
 public: 

   uint64_t serverLocation(KeyT &k,int i);
   void create_table(uint64_t n,KeyT maxKey);
   void remove_table(int index);

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
	std::function<void(const tl::request &, KeyT &, ValueT &, int &)> insertFunc(
        std::bind(&distributed_hashmap<KeyT, ValueT,HashFcn,EqualFcn>::ThalliumLocalInsert,
        this, std::placeholders::_1, std::placeholders::_2,std::placeholders::_3,std::placeholders::_4));

	std::function<void(const tl::request &,KeyT &,int &)> findFunc(
	std::bind(&distributed_hashmap<KeyT,ValueT,HashFcn,EqualFcn>::ThalliumLocalFind,
	this,std::placeholders::_1,std::placeholders::_2,std::placeholders::_3));

	std::function<void(const tl::request &,KeyT &,int &)> eraseFunc(
	std::bind(&distributed_hashmap<KeyT,ValueT,HashFcn,EqualFcn>::ThalliumLocalErase,
	this,std::placeholders::_1,std::placeholders::_2,std::placeholders::_3));

	std::function<void(const tl::request &,KeyT &,int &)> getValueFunc(
	std::bind(&distributed_hashmap<KeyT,ValueT,HashFcn,EqualFcn>::ThalliumLocalGetValue,
	this,std::placeholders::_1,std::placeholders::_2,std::placeholders::_3));

	thallium_server->define("RemoteInsert",insertFunc);
	thallium_server->define("RemoteFind",findFunc);
	thallium_server->define("RemoteErase",eraseFunc);
	thallium_server->define("RemoteGetValue",getValueFunc);
	thallium_shm_server->define("RemoteInsert",insertFunc);
	thallium_shm_server->define("RemoteFind",findFunc);
	thallium_shm_server->define("RemoteErase",eraseFunc);
	thallium_shm_server->define("RemoteGetValue",getValueFunc);
	
   }

  distributed_hashmap(uint64_t np,uint64_t nc, int rank)
  {
	nservers = np;
	numcores = nc;
	serverid = rank;
	maxtables = 1000;
	num_tables.store(0);
	maxSizes.resize(maxtables);
	totalSizes.resize(maxtables);
	emptyKeys.resize(maxtables);
	my_tables.resize(maxtables);
	pls.resize(maxtables);
	range.resize(maxtables);
	dropped_events.store(0);
	for(int i=0;i<maxtables;i++)
	{
	  my_tables[i] = nullptr;
	  pls[i] = nullptr;
	}
	std::string emptystring = "NULL";
	t_pool = new memory_pool<std::string,int> (100);
	table_names = new BlockMap<std::string,int> (maxtables,t_pool,emptystring);
  }
 ~distributed_hashmap()
  {
	  for(int i=0;i<maxtables;i++)
	  {
		if(my_tables[i] != nullptr)  delete my_tables[i];
		if(pls[i] != nullptr) delete pls[i];
	  }
	  delete table_names;
	  delete t_pool;
  }

   void setClock(ClockSynchronization<ClocksourceCPPStyle> *C)
   {
	 CM = C;
   }
   int LocalInsert(KeyT &k,ValueT &v,int index)
  {
      if(!CM->NearTime(k) || !(k>=range[index].first && k <= range[index].second))
      {
	 dropped_events.fetch_add(1);
	 std::cout <<" dropped event"<<std::endl;
	 return 2;
      }
        
      uint32_t b = my_tables[index]->insert(k,v);
      if(b == INSERTED) return 1;
      else return 0;
  }
  bool LocalFind(KeyT &k,int index)
  {
    if(my_tables[index]->find(k) != NOT_IN_TABLE) return true;
    else return false;
  }
  bool LocalErase(KeyT &k,int index)
  {
     return my_tables[index]->erase(k);
  }
  bool LocalUpdate(KeyT &k,ValueT &v,int index)
  {
       return my_tables[index]->update(k,v);
  }
  bool LocalGet(KeyT &k,ValueT* v,int index)
  {
       return my_tables[index]->get(k,v);
  }

  ValueT LocalGetValue(KeyT &k,int index)
  {
        ValueT v;
        new (&v) ValueT();
	v = -1;
        bool b = LocalGet(k,&v,index);
        return v;
  }
  
  bool LocalGetMap(std::vector<ValueT> &values,int index)
  {
	my_tables[index]->get_map(values);
	return true;
  }
  void set_valid_range(int index,uint64_t &min_k,uint64_t &max_k)
  {
	range[index].first = min_k;
	range[index].second = max_k;
  }
  bool LocalClearMap(int index)
  {
	my_tables[index]->clear_map();
	dropped_events.store(0);
	return true;
  }

  template<typename... Args>
  bool LocalUpdateField(KeyT &k,int index,void(*f)(ValueT*,Args&&... args),Args&&...args_)
  {
     return my_tables[index]->update_field(k,f,std::forward<Args>(args_)...);
  }

  uint64_t allocated(int index)
  {
     return my_tables[index]->allocated_nodes();
  }

  uint64_t removed(int index)
  {
     return my_tables[index]->removed_nodes();
  }

  int num_dropped()
  {
	 return dropped_events;
  }
  void ThalliumLocalInsert(const tl::request &req, KeyT &k, ValueT &v, int &s)
  {
	req.respond(LocalInsert(k,v,s));
  }

  void ThalliumLocalFind(const tl::request &req, KeyT &k,int &s)
  {
	  req.respond(LocalFind(k,s));
  }

  void ThalliumLocalErase(const tl::request &req, KeyT &k,int &s)
  {
	  req.respond(LocalErase(k,s));
  }
  void ThalliumLocalGetValue(const tl::request &req,KeyT &k,int &s)
  {
	req.respond(LocalGetValue(k,s));
  }
  int Insert(KeyT &k, ValueT &v,int index)
  {
    int destid = serverLocation(k,index);
    assert (destid >= 0 && destid < nservers);
    if(ipaddrs[destid].compare(myipaddr)==0)
    {
	tl::endpoint ep = thallium_shm_client->lookup(shmaddrs[destid]);
	tl::remote_procedure rp = thallium_shm_client->define("RemoteInsert");
	return rp.on(ep)(k,v,index);
    }
    else
    {
      tl::remote_procedure rp = thallium_client->define("RemoteInsert");
      return rp.on(serveraddrs[destid])(k,v,index);
    }
  }
  bool Find(KeyT &k,int index)
  {
    int destid = serverLocation(k,index);
    if(ipaddrs[destid].compare(myipaddr)==0)
    {
	tl::endpoint ep = thallium_shm_client->lookup(shmaddrs[destid]);
	tl::remote_procedure rp = thallium_shm_client->define("RemoteFind");
	return rp.on(ep)(k,index);
    }
    else
    {
      tl::remote_procedure rp = thallium_client->define("RemoteFind");
      return rp.on(serveraddrs[destid])(k,index);
    }
  }
  bool Erase(KeyT &k,int index)
  {
     int destid = serverLocation(k,index);
     if(ipaddrs[destid].compare(myipaddr)==0)
     {
	tl::endpoint ep = thallium_shm_client->lookup(shmaddrs[destid]);
	tl::remote_procedure rp = thallium_shm_client->define("RemoteErase");
	return rp.on(ep)(k,index);
     }
     else
     {
       tl::remote_procedure rp = thallium_client->define("RemoteErase");
       return rp.on(serveraddrs[destid])(k,index);
     }
  } 
  ValueT GetValue(KeyT &k,int index)
  {
     int destid = serverLocation(k,index);
     if(ipaddrs[destid].compare(myipaddr)==0)
     {
	tl::endpoint ep = thallium_shm_client->lookup(shmaddrs[destid]);
	tl::remote_procedure rp = thallium_shm_client->define("RemoteGetValue");
	return rp.on(ep)(k,index);
     }
     else
     {
	tl::remote_procedure rp = thallium_client->define("RemoteGetValue");
	return rp.on(serveraddrs[destid])(k,index);
     }

  } 
};

#include "../srcs/distributed_map.cpp"

#endif
