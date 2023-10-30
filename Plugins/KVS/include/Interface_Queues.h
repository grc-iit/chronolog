#ifndef __INTERFACE_QUEUES_H_
#define __INTERFACE_QUEUES_H_

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
#include "query_request.h"
#include "query_response.h"
#include <boost/lockfree/queue.hpp>
#include "data_server_client.h"
#include <fstream>

using namespace std::chrono_literals;
namespace tl=thallium;


class Interface_Queues
{
  private: 
	  boost::lockfree::queue<struct query_req*> *request_queue;
	  boost::lockfree::queue<struct query_resp*> *response_queue;
	  tl::engine *thallium_server;
          tl::engine *thallium_shm_server;
          tl::engine *thallium_client;
          tl::engine *thallium_shm_client;
          std::vector<tl::endpoint> serveraddrs;
          std::vector<std::string> ipaddrs;
          std::vector<std::string> shmaddrs;
	  std::vector<std::string> remoteshmaddrs;
	  std::vector<std::string> remoteipaddrs;
	  std::vector<std::string> remoteserveraddrs;
          std::string myipaddr;
          std::string myhostname;
          uint32_t nservers;
          uint32_t serverid;


  public :
	Interface_Queues(int n,int p) : nservers(n), serverid(p)
	{
	   request_queue = new boost::lockfree::queue<struct query_req*> (128);
	   response_queue = new boost::lockfree::queue<struct query_resp*> (128);


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

	void get_remote_addrs(std::string &filename)
	{
	   std::ifstream filest(filename.c_str(),std::ios_base::in);

	   std::vector<std::string> lines;
	   if(filest.is_open())
	   {
		std::string line;
		while(getline(filest,line))
		{
		   lines.push_back(line);
		   line.clear();
		}

		int nremoteservers = std::stoi(lines[0]);
		int n = 1;
		for(int i=0;i<nremoteservers;i++)
		{
		   remoteshmaddrs.push_back(lines[n]);
		   n++;
		}
		for(int i=0;i<nremoteservers;i++)
		{
		   remoteipaddrs.push_back(lines[n]);
		   n++;
		}
		for(int i=0;i<nremoteservers;i++)
		{
		   remoteserveraddrs.push_back(lines[n]);
		   n++;
		}

		filest.close();
	   }

	}

	void bind_functions()
	{
	     std::function<void(const tl::request &, struct query_req &r)> PutRequestFunc(
             std::bind(&Interface_Queues::ThalliumLocalPutRequest,this, std::placeholders::_1, std::placeholders::_2));

	     thallium_server->define("KVSRemotePutRequest",PutRequestFunc);
             thallium_shm_server->define("KVSRemotePutRequest",PutRequestFunc);

	}


	bool LocalPutRequest(struct query_req &r)
	{
	   struct query_req * rq = new struct query_req();
	   rq->name.assign(r.name.begin(),r.name.end());
	   rq->minkey = r.minkey;
	   rq->maxkey = r.maxkey;
	   rq->id = r.id;
	   rq->sender = r.sender;
	   rq->from_nvme = r.from_nvme;
	   rq->sorted = r.sorted;
	   rq->collective = r.collective;
	   rq->single_point = r.single_point;
	   rq->output_file = r.output_file;
	   rq->op = r.op;
	   return request_queue->push(rq);
	}

	bool LocalPutResponse(struct query_resp &r)
	{
	   struct query_resp *rs = new struct query_resp();
	   rs->id = r.id;
	   rs->response_id = r.response_id;
	   rs->minkey = r.minkey;
	   rs->maxkey = r.maxkey;
	   rs->sender = r.sender;
	   rs->response.assign(r.response.begin(),r.response.end());
	   rs->output_file.assign(r.output_file.begin(),r.output_file.end());
	   rs->complete = r.complete;
	   return response_queue->push(rs);
	}

	void ThalliumLocalPutRequest(const tl::request &req, struct query_req &r)
        {
            req.respond(LocalPutRequest(r));
        }


	std::vector<uint64_t> PutEmulatorEvent(std::string &s,std::string &data,int s_id)
	{
	    if(remoteipaddrs[s_id].compare(myipaddr)==0)
	    {
		tl::endpoint ep = thallium_shm_client->lookup(remoteshmaddrs[s_id]);
		tl::remote_procedure rp = thallium_shm_client->define("EmulatorAddEvent");
		std::chrono::duration<double,std::ratio<100>> second;
		return rp.on(ep).timed(second,s,data);
	    }
	    else
	    {
		tl::endpoint ep = thallium_client->lookup(remoteserveraddrs[s_id]);
		tl::remote_procedure rp = thallium_client->define("EmulatorAddEvent");
		std::chrono::duration<double,std::ratio<100>> second;
		return rp.on(ep).timed(second,s,data);
	    }
	}

	bool CreateEmulatorStream(std::string &s,std::vector<std::string> &metadata,int s_id,int &n1,int &n2)
	{
	   bool b = false;
	   if(remoteipaddrs[s_id].compare(myipaddr)==0)
	   {
		tl::endpoint ep = thallium_shm_client->lookup(remoteshmaddrs[s_id]);
		tl::remote_procedure rp = thallium_shm_client->define("EmulatorPrepareStream");
		std::chrono::duration<double,std::ratio<20>> second;
		b = rp.on(ep).timed(second,s,metadata,n1,n2);
	   }
           else
	   {
		tl::endpoint ep = thallium_client->lookup(remoteserveraddrs[s_id]);
		tl::remote_procedure rp = thallium_client->define("EmulatorPrepareStream");
		std::chrono::duration<double,std::ratio<20>> second;
		b = rp.on(ep).timed(second,s,metadata,n1,n2);
	   }	
	   return b;

	}	
	bool CreateEmulatorBuffer(int numevents,std::string &s,int s_id)
	{
	    bool b = false;
	    if(remoteipaddrs[s_id].compare(myipaddr)==0)
	    {
		tl::endpoint ep = thallium_shm_client->lookup(remoteshmaddrs[s_id]);
		tl::remote_procedure rp = thallium_shm_client->define("EmulatorCreateBuffer");
		std::chrono::duration<double,std::ratio<20>> second;
		b = rp.on(ep).timed(second,numevents,s);
	    }
	    else
	    {
		tl::endpoint ep = thallium_client->lookup(remoteserveraddrs[s_id]);
		tl::remote_procedure rp = thallium_client->define("EmulatorCreateBuffer");
		std::chrono::duration<double,std::ratio<20>> second;
		b = rp.on(ep).timed(second,numevents,s);
	    }
	    return  b;
	}

	bool EndEmulatorSession(std::string &s,int s_id)
	{
	   bool b = false;
	   if(remoteipaddrs[s_id].compare(myipaddr)==0)
	   {
	     tl::endpoint ep = thallium_shm_client->lookup(remoteshmaddrs[s_id]);
             tl::remote_procedure rp = thallium_shm_client->define("EmulatorEndSessions");
	     std::chrono::duration<double> second;
	     b = rp.on(ep).timed(second,s);
	   }
	   else
	   {
	     tl::endpoint ep = thallium_client->lookup(remoteserveraddrs[s_id]);
	     tl::remote_procedure rp = thallium_client->define("EmulatorEndSessions");
	     std::chrono::duration<double> second;
	     b = rp.on(ep).timed(second,s);
	   }
	   return b;
	}

	bool ShutDownEmulator(std::string &s,int s_id)
	{
	   bool b = false;
	   if(remoteipaddrs[s_id].compare(myipaddr)==0)
	   {
	     tl::endpoint ep = thallium_shm_client->lookup(remoteshmaddrs[s_id]);
	     tl::remote_procedure rp = thallium_shm_client->define("ShutDownEmulator");
	     b = rp.on(ep)(s);
	   }
	   else
	   {
	      tl::endpoint ep = thallium_client->lookup(remoteserveraddrs[s_id]);
	      tl::remote_procedure rp = thallium_client->define("ShutDownEmulator");
	      b = rp.on(ep)(s);
	   }
	   return b;
	}

	bool PutKVSAddresses(int s_id)
	{
	   std::vector<std::string> lines;
	   
	   lines.push_back(std::to_string(nservers));

	   for(int i=0;i<shmaddrs.size();i++)
		lines.push_back(shmaddrs[i]);
	   for(int i=0;i<ipaddrs.size();i++)
		lines.push_back(ipaddrs[i]);
	   for(int i=0;i<serveraddrs.size();i++)
		lines.push_back(serveraddrs[i]);
	   bool b = false;
	   if(remoteipaddrs[s_id].compare(myipaddr)==0)
	   {
	      tl::endpoint ep = thallium_shm_client->lookup(remoteshmaddrs[s_id]);
	      tl::remote_procedure rp = thallium_shm_client->define("EmulatorPutRemoteAddresses");
	      std::chrono::duration<double> second;
	      b = rp.on(ep).timed(second,lines);

	   }
	   else
	   {
	     tl::endpoint ep = thallium_client->lookup(remoteserveraddrs[s_id]);
	     tl::remote_procedure rp = thallium_client->define("EmulatorPutRemoteAddresses");
	     std::chrono::duration<double> second;
	     b = rp.on(ep).timed(second,lines);
	   }
	   return b;
	}

	std::string GetEmulatorEvent(std::string &s,uint64_t &ts,int s_id)
	{
	   if(remoteipaddrs[s_id].compare(myipaddr)==0)
   	   {
		tl::endpoint ep = thallium_shm_client->lookup(remoteshmaddrs[s_id]);
		tl::remote_procedure rp = thallium_shm_client->define("EmulatorFindEventFile");
		std::chrono::duration<double,std::ratio<100>> second;
		return rp.on(ep).timed(second,s,ts);
	   }		   
	   else
	   {
		tl::endpoint ep = thallium_client->lookup(remoteserveraddrs[s_id]);
		tl::remote_procedure rp = thallium_client->define("EmulatorFindEventFile");
		std::chrono::duration<double,std::ratio<100>> second;
		return rp.on(ep).timed(second,s,ts);
	   }
	}
	bool CheckFileExistence(std::string &s,int s_id)
	{
	   if(remoteipaddrs[s_id].compare(myipaddr)==0)
	   {
		tl::endpoint ep = thallium_shm_client->lookup(remoteshmaddrs[s_id]);
		tl::remote_procedure rp = thallium_shm_client->define("EmulatorCheckFile");
		std::chrono::duration<double> second;
		return rp.on(ep).timed(second,s);
	   }
	   else
	   {
		tl::endpoint ep = thallium_client->lookup(remoteserveraddrs[s_id]);
		tl::remote_procedure rp = thallium_client->define("EmulatorCheckFile");
		std::chrono::duration<double> second;
		return rp.on(ep).timed(second,s);
	   }
	}

	~Interface_Queues()
	{
	   delete request_queue;
	   delete response_queue;
	
	}


};

#endif
