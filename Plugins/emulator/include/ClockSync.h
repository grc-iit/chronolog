#ifndef __CLOCK_SYNC_
#define __CLOCK_SYNC_

#include <chrono>
#include <time.h>
#include <unistd.h>
#include <climits>
#include <mpi.h>
#include <vector>
#include <iostream>
#include <cassert>
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
#include <netdb.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <boost/type_traits.hpp>
#include <boost/atomic.hpp>

namespace tl=thallium;

class ClocksourceCStyle
{
public:
    ClocksourceCStyle()
    {}
    ~ClocksourceCStyle()
    {}    
    uint64_t getTimestamp()
    {
        struct timespec t{};
        clock_gettime(CLOCK_TAI, &t);
        return t.tv_sec;
    }
};
 
class ClocksourceCPPStyle
{
public:
    ClocksourceCPPStyle()
    {}
    ~ClocksourceCPPStyle()
    {}    
    uint64_t getTimestamp()
    {
        auto t = std::chrono::high_resolution_clock::now().time_since_epoch().count();
	return t;
    }
};


template<class Clocksource>
class ClockSynchronization
{
 
   private:
     Clocksource *clock;
     std::atomic<uint64_t> maxError;
     int myrank;
     int numprocs;
     std::atomic<uint64_t> myoffset;
     std::atomic<uint64_t> unit;
     std::atomic<uint64_t> epsilon;
     std::atomic<uint64_t> delay;
     std::atomic<bool> is_less;
     tl::engine *thallium_server;
     tl::engine *thallium_shm_server;
     tl::engine *thallium_client;
     tl::engine *thallium_shm_client;
     std::vector<tl::endpoint> serveraddrs;
     std::vector<std::string> ipaddrs;
     std::vector<std::string> shmaddrs;
     std::string myipaddr;
     std::string myhostname;
     uint32_t nservers;
     uint32_t serverid;
     boost::atomic<boost::uint128_type> offset_error;
   public:
     ClockSynchronization(int n1,int n2,std::string &q) : myrank(n1), numprocs(n2)
     {
	 if(q.compare("second")==0) unit.store(1000000000);
	 else if(q.compare("microsecond")==0) unit.store(1000);
         else if(q.compare("millisecond")==0) unit.store(1000000);
	 else unit.store(1);
	 myoffset.store(0);
	 maxError.store(0);
	 epsilon.store(200000000);  //400 microseconds (scheduling, measurement errors)
	 delay.store(500000); // 200 microseconds network delay for reasonably large messages
	 uint64_t dd = delay.load()/unit.load();
	 delay.store(dd);
	 uint64_t en = epsilon.load()/unit.load();
	 epsilon.store(en);
	 is_less.store(false);
	 nservers = numprocs;
	 serverid = myrank;
     }
     ~ClockSynchronization()
     {}

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
	 std::function<void(const tl::request &,int &)> TimeStampFunc(
         std::bind(&ClockSynchronization<Clocksource>::ThalliumLocalTimeStamp,this,std::placeholders::_1, std::placeholders::_2));
         thallium_server->define("RemoteTimeStamp",TimeStampFunc);
         thallium_shm_server->define("RemoteTimeStamp",TimeStampFunc);
      }

     uint64_t Timestamp()
     {
	 boost::uint128_type eoffset = offset_error.load();
	 eoffset = eoffset >> 64;
	 uint64_t mb = (uint64_t)eoffset;
	 uint64_t mbit = mb >> 63;
	 uint64_t den = mb << 1;
	 den = den >> 1;
	 if(!mbit)
	 return clock->getTimestamp()/unit.load()+den;
	 else
	  return clock->getTimestamp()/unit.load()-den;
     }

     bool NearTime(uint64_t ts)
     {
	boost::uint128_type eoffset = offset_error.load();
	uint64_t errorm = (uint64_t)eoffset;
	eoffset = eoffset >> 64;
	uint64_t mb = (uint64_t)eoffset;
	uint64_t mbit = mb >> 63;
	uint64_t den = mb << 1;
	den = den >> 1;
	uint64_t myts = clock->getTimestamp()/unit.load();
	if(!mbit) myts += den; else myts -= den;
	uint64_t diff = UINT64_MAX;
	if(myts < ts) diff = ts-myts;
	else diff = myts-ts;
	if(diff <= 2*errorm+delay.load()+epsilon.load()) return true;
	else 
	{
		//throw std::runtime_error("Timestamp out of range");
		return false;
	}
     }
     void ThalliumLocalTimeStamp(const tl::request &req, int &pid)
     {
           req.respond(Timestamp());
     }

     uint64_t RequestTimeStamp(int server_id)
     {
           uint64_t b = UINT64_MAX;
           if(ipaddrs[server_id].compare(myipaddr)==0)
           {
                  tl::endpoint ep = thallium_shm_client->lookup(shmaddrs[server_id]);
                  tl::remote_procedure rp = thallium_shm_client->define("RemoteTimeStamp");
                  b = rp.on(ep)(myrank);
           }
           else
           {
                   tl::remote_procedure rp = thallium_client->define("RemoteTimeStamp");
                   b = rp.on(serveraddrs[server_id])(myrank);
           }
           return b;
     }

     std::pair<uint64_t,uint64_t> SynchronizeClocks();
     void ComputeErrorInterval(uint64_t,uint64_t);

};

#include "../srcs/clocksync.cpp"

#endif
