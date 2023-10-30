#ifndef __KeyValueStore_H_
#define __KeyValueStore_H_

#include "KeyValueStoreMetadata.h"
#include "KeyValueStoreAccessorRepository.h"
#include "KeyValueStoreMDS.h"
#include "KeyValueStoreIO.h"
#include "data_server_client.h"
#include "util_t.h"
#include <hdf5.h>
#include <thread>
#include <mutex>
#include "Interface_Queues.h"

struct kstream_args
{
  std::string tname;
  std::string attr_name;
  int ffreq;
  int wdelay;
  int ifreq;
  int maxsize;
  int tid;
};

class KeyValueStore
{
    private:
	    int numprocs;
	    int myrank;
	    KeyValueStoreMDS *mds;
	    data_server_client *ds;
	    KeyValueStoreIO *io_layer;
	    Interface_Queues *if_q;
	    KeyValueStoreAccessorRepository *tables;
	    int io_count;
	    int tag;
	    std::unordered_map<std::string,int> kvindex;
	    std::vector<struct kstream_args> k_args;
	    std::vector<std::thread> kstreams;
	    std::atomic<int> *stream_flags;
	    std::atomic<int> nstreams;

    public:
	    KeyValueStore(int np,int r) : numprocs(np), myrank(r)
	   {
		H5open();
		io_count=0;
		int base_port = 2000;
		tag = 20000;
		nstreams.store(0);
		ds = new data_server_client(numprocs,myrank,base_port);
		mds = new KeyValueStoreMDS(numprocs,myrank);
		tl::engine *t_server = ds->get_thallium_server();
           	tl::engine *t_server_shm = ds->get_thallium_shm_server();
           	tl::engine *t_client = ds->get_thallium_client();
           	tl::engine *t_client_shm = ds->get_thallium_shm_client();
           	std::vector<tl::endpoint> server_addrs = ds->get_serveraddrs();
           	std::vector<std::string> ipaddrs = ds->get_ipaddrs();
           	std::vector<std::string> shmaddrs = ds->get_shm_addrs();
           	mds->server_client_addrs(t_server,t_client,t_server_shm,t_client_shm,ipaddrs,shmaddrs,server_addrs);
		mds->bind_functions();
		io_layer = new KeyValueStoreIO(numprocs,myrank);
		io_layer->server_client_addrs(t_server,t_client,t_server_shm,t_client_shm,ipaddrs,shmaddrs,server_addrs);
		io_layer->io_service();
		if_q = new Interface_Queues(numprocs,myrank);
		if_q->server_client_addrs(t_server,t_client,t_server_shm,t_client_shm,ipaddrs,shmaddrs,server_addrs);
		std::string remfilename = "emulatoraddrs";
		if_q->get_remote_addrs(remfilename);
		if_q->bind_functions();
		bool b = if_q->PutKVSAddresses(myrank);
		tables = new KeyValueStoreAccessorRepository(numprocs,myrank,io_layer,if_q,ds);
		k_args.resize(MAXSTREAMS);
	        kstreams.resize(MAXSTREAMS);
		stream_flags = (std::atomic<int>*)std::malloc(MAXSTREAMS*sizeof(std::atomic<int>));
		MPI_Request *reqs = new MPI_Request[2*numprocs];

		int send_v = 1;
		std::vector<int> recvv(numprocs);
		std::fill(recvv.begin(),recvv.end(),0);

		int nreq = 0;
		for(int i=0;i<numprocs;i++)
		{
		   MPI_Isend(&send_v,1,MPI_INT,i,tag,MPI_COMM_WORLD,&reqs[nreq]);
		   nreq++;
		   MPI_Irecv(&recvv[i],1,MPI_INT,i,tag,MPI_COMM_WORLD,&reqs[nreq]);
		   nreq++;
		}
		MPI_Waitall(nreq,reqs,MPI_STATUS_IGNORE);

		delete reqs;
	   }
	   void createKeyValueStoreEntry(std::string &,KeyValueStoreMetadata &);
	   bool findKeyValueStoreEntry(std::string &,KeyValueStoreMetadata &);
	   void removeKeyValueStoreEntry(std::string &s);
	   void addKeyValueStoreInvList(std::string &s,std::string &attr_name,int);
	   bool findKeyValueStoreInvList(std::string &s,std::string &attr_name);
	   void removeKeyValueStoreInvList(std::string &s,std::string &attr_name);

	   data_server_client * get_rpc_client()
	   {
		return ds;
	
	   }
	   template<typename T,typename N>
	   void cacheflushInvList(struct kstream_args*k)
	   {
    		std::string s = k->tname;
    		std::string attr_name = k->attr_name;
    		KeyValueStoreAccessor* ka = tables->get_accessor(s);

    		int pos = ka->get_inverted_list_index(attr_name);

    		std::string type = ka->get_attribute_type(attr_name);

    		int tag = k->tid;

    		int send_v = 1;
    		std::vector<int> recv_v(numprocs);
    		std::fill(recv_v.begin(),recv_v.end(),0);

    		int nreq = 0;
   	 	MPI_Request *reqs = new MPI_Request[2*numprocs];

		bool end_loop = false;
		int rate = (k->ifreq < 50) ? 50 : k->ifreq;
		rate = (rate > 500) ? 500 : rate;
		int request_count=0;

		while(true)
   		{
      		  send_v = stream_flags[k->tid].load();
      		  std::fill(recv_v.begin(),recv_v.end(),0);
      		  nreq = 0;
      		  for(int i=0;i<numprocs;i++)
      		  {
        	     MPI_Isend(&send_v,1,MPI_INT,i,tag,MPI_COMM_WORLD,&reqs[nreq]);
        	     nreq++;
        	     MPI_Irecv(&recv_v[i],1,MPI_INT,i,tag,MPI_COMM_WORLD,&reqs[nreq]);
        	     nreq++;
      		  }

      		  MPI_Waitall(nreq,reqs,MPI_STATUS_IGNORE);

      		  int sum = 0;
      		  for(int i=0;i<numprocs;i++) sum += recv_v[i];

      		  if(sum==numprocs) 
		  {
		     end_loop = true;
		     rate = rate/2;
		  }

      		  auto t1 = std::chrono::high_resolution_clock::now();
		  if(myrank==0)
                  while(true)
                  {
        		auto t2 = std::chrono::high_resolution_clock::now();

        		double t = std::chrono::duration<double>(t2-t1).count();
        		if(t > (double)rate) break;
      		  }
		
		  nreq = 0;
		  if(myrank==0)
		  for(int i=0;i<numprocs;i++)
		  {
		    MPI_Isend(&send_v,1,MPI_INT,i,tag,MPI_COMM_WORLD,&reqs[nreq]);
		    nreq++;

		  }
		  
		  MPI_Irecv(&recv_v[0],1,MPI_INT,0,tag,MPI_COMM_WORLD,&reqs[nreq]);
		  nreq++;

		  MPI_Waitall(nreq,reqs,MPI_STATUS_IGNORE);

		  if(end_loop) 
		  ka->flush_invertedlist<T>(attr_name,true);
		  else 
		  {
		     bool c = false;
		     if(request_count%5==0) c = true;
		     ka->flush_invertedlist<T>(attr_name,c);
		  }
		  if(end_loop) break;
		  request_count++;
   	       }

   	       delete reqs;
          }


	   template<typename T,typename N>
           void prepare_inverted_list(struct kstream_args *k)
	   {
		std::string s = k->tname;
                std::string attr_name = k->attr_name;
                KeyValueStoreAccessor* ka = tables->get_accessor(s);
		int maxsize = k->maxsize;

                if(ka==nullptr)
                {
                   KeyValueStoreMetadata m;
                   if(!findKeyValueStoreEntry(s,m)) return;
                   if(!tables->add_accessor(s,m)) return;
                   ka = tables->get_accessor(s);
                }

              int pos = ka->get_inverted_list_index(attr_name);

               if(pos==-1)
               {
                  tables->create_invertedlist(s,attr_name,io_count,maxsize);
                  io_count++;
                  pos = ka->get_inverted_list_index(attr_name);
               }

               std::string type = ka->get_attribute_type(attr_name);
	       KeyValueStoreMetadata m = ka->get_metadata();
	       std::vector<std::string> metastring;
	       m.packmetadata(metastring);
	       int nloops = std::max(1,k->ffreq);
               int nde = (k->wdelay < 50) ? 50 : k->wdelay; 	
	       nde = (nde > 500) ? 500 : nde; 
	       bool b = if_q->CreateEmulatorStream(s,metastring,myrank,nloops,nde);
	   }

	   template<typename T,typename N>
	   void create_keyvalues_ordered(int s_id,std::vector<N> &keys,std::vector<std::string> &values,std::vector<int> &ops,int rate)
	   {
		std::string s = k_args[s_id].tname;
                std::string attr_name = k_args[s_id].attr_name;
                KeyValueStoreAccessor *ka = tables->get_accessor(s);
                bool b = false;
                int pos = ka->get_inverted_list_index(attr_name);
                std::string st = k_args[s_id].tname;
                KeyValueStoreMetadata m = ka->get_metadata();
                int datasize = m.value_size();
		std::vector<int> request_status(keys.size());
		std::fill(request_status.begin(),request_status.end(),0);
		std::unordered_map<N,int> pending_requests;

		while(true)
		{
		   std::vector<std::pair<int,std::string>> resp_ids = ka->Completed_Gets<T,N>(pos,st);

		   for(int i=0;i<resp_ids.size();i++)
		   {
			int id = resp_ids[i].first;
			N key = keys[id];
			auto r = pending_requests.find(key);
			if(r != pending_requests.end())
			{
			  pending_requests.erase(r);
			}
			if(ops[id]==2)
			{
			    int pos1 = values[id].find("=");
			    std::string fl = "field";
			    std::string substr1 = values[id].substr(0,pos1);
			    std::string substr2 = substr1.substr(fl.length());
			    int atr_id = std::stoi(substr2);
			    std::string data;
                            data.resize(datasize);
			    for(int j=0;j<datasize;j++)
				 data[j]=resp_ids[i].second[sizeof(uint64_t)+j];
			    int offset = atr_id*100;
			    for(int j=0;j<values[id].length();j++)
				data[sizeof(N)+offset+j] = values[id][j];
                            if(ka->Put<T,N,std::string>(pos,st,keys[id],data))
                            {


                            }
			    usleep(rate);

			}
			request_status[id] = -1;
		   }

		   int num_completed = 0;
		   for(int i=0;i<request_status.size();i++)
			if(request_status[i]==-1) num_completed++;
		   
		   if(num_completed == request_status.size()) break;

		   for(int i=0;i<keys.size();i++)
		   {
			if(request_status[i] != -1)
			{
			   N key = keys[i];
			   auto r = pending_requests.find(key);
			   if(r == pending_requests.end())
			   {
				if(ops[i]==0)
				{
				  std::string data;
                     		  data.resize(sizeof(N)+values[i].length());
                     		  char *keystr = (char *)(&keys[i]);
                     		  for(int j=0;j<sizeof(N);j++)
                        	    data[j] = keystr[j];
                     		  for(int j=0;j<values[i].length();j++)
                        	    data[sizeof(N)+j] = values[i][j];

                                  if(ka->Put<T,N,std::string>(pos,st,keys[i],data))
                                  {


                                  }
				  request_status[i] = -1;
				  usleep(rate);
				}
				else
				{
				   b = ka->Get_resp<T,N>(pos,st,keys[i],i);
				   std::pair<N,int> p;
			   	   p.first = keys[i];
				   p.second = ops[i];	   
				   pending_requests.insert(p);
				   usleep(rate);
				}
				
			   }

			}
		   }

		}

	   }
	   template<typename T,typename N>
	   void create_keyvalues(int s_id,std::vector<N> &keys,std::vector<std::string> &values,std::vector<int> &ops,int rate)
	   {
		std::string s = k_args[s_id].tname;
		std::string attr_name = k_args[s_id].attr_name;
		KeyValueStoreAccessor *ka = tables->get_accessor(s);
		bool b = false;
		int pos = ka->get_inverted_list_index(attr_name);
		std::string st = k_args[s_id].tname;
		KeyValueStoreMetadata m = ka->get_metadata();	
		int datasize = m.value_size();
		int ids = 0;

		for(int i=0;i<keys.size();i++)
		{
		   if(ops[i]==0)
		   {
		     std::string data;
		     data.resize(sizeof(N)+values[i].length());
		     char *key = (char *)(&keys[i]);
	     	     for(int j=0;j<sizeof(N);j++)
			data[j] = key[j];
		     for(int j=0;j<values[i].length();j++)
			data[sizeof(N)+j] = values[i][j];	     
			
		     if(ka->Put<T,N,std::string>(pos,st,keys[i],data))
		     {


		     }
		     ids++;
		   }
		   else if(ops[i]==1)
		   {
			b = ka->Get<T,N>(pos,st,keys[i],ids);
			ids++;

		   }
		   usleep(rate);
			
		}

	   }

	   template<typename T,typename N,typename M>
	   void create_keyvalues(int s_id,std::vector<N> &keys,std::vector<M> &values,std::vector<int> &ops,int rate)
	   {
	      std::string s = k_args[s_id].tname;
	      std::string attr_name = k_args[s_id].attr_name;
	      KeyValueStoreAccessor *ka = tables->get_accessor(s);
	      bool b = false;
	      int pos = ka->get_inverted_list_index(attr_name);
	      std::string st = k_args[s_id].tname;
	      KeyValueStoreMetadata m = ka->get_metadata();
	      int datasize = m.value_size();
	     
	      int ids = 0;

	      for(int i=0;i<keys.size();i++)
	      {
		if(ops[i]==0)
		{
	          std::string data;
	          data.resize(datasize);
		  char *key = (char*)(&(keys[i]));
		  char *value = (char*)(&(values[i]));
		  for(int j=0;j<sizeof(N);j++)
		    data[j] = key[j];
	          for(int j=0;j<sizeof(M);j++)
		    data[sizeof(N)+j] = value[j];	 
		  if(ka->Put<T,N,std::string>(pos,st,keys[i],data))
		  {


		  }
		
		}
		else
		{

		   b = ka->Get_resp<T,N> (pos,st,keys[i],ids);
		   ids++;

		}
		usleep(rate);

	      }

	      std::vector<std::pair<int,std::string>> resp_ids = ka->Completed_Gets<T,N>(pos,st);

	   }
	   template<typename T,typename N>
	   void create_keyvalues(int s_id,int nops,int rate)
	   {
		std::string s = k_args[s_id].tname;
   		std::string attr_name = k_args[s_id].attr_name;
   		KeyValueStoreAccessor* ka = tables->get_accessor(s);

		bool b = false;
		int pos = ka->get_inverted_list_index(attr_name);
		std::string st = k_args[s_id].tname;
		std::string data;
		KeyValueStoreMetadata m = ka->get_metadata();
		int datasize = m.value_size();
		data.resize(datasize);
		
		bool exit = false;
		int op = 0;
		N prevkey=0;
		int ids = 0;
		std::vector<N> keys;
		std::vector<N> keys_p;
		for(int i=0;i<nops;i++)
		{	
		    N key = random()%RAND_MAX; 
		    op = random()%2;
		    if(op==0)
		    { 
		      if(!ka->Put<T,N,std::string>(pos,st,key,data))
		      {
		      }
		      prevkey = key;
		      ids++;
		      if(ids < 100) keys_p.push_back(key);
		    }
		    else if(prevkey != 0) 
		    {
		      key = prevkey;
		      b = ka->Get<T,N> (pos,st,key,ids);
		      ids++;
		    }
		    
		    usleep(rate); 
		}

		for(int i=0;i<keys_p.size();i++)
		{
		   b = ka->Get<T,N> (pos,st,keys_p[i],ids);
		   ids++;

		}

	   }

           void get_testworkload(std::string &,std::vector<int>&,std::vector<uint64_t>&,int);
           void get_ycsb_timeseries_workload(std::string&,std::vector<uint64_t>&,std::vector<float>&,std::vector<int>&);
           void get_dataworld_workload(std::string&,std::vector<uint64_t>&,std::vector<uint64_t>&,std::vector<int>&);
           void get_ycsb_test(std::string&,std::vector<uint64_t>&,std::vector<std::string>&,std::vector<int>&);
	   void get_keyvaluestorestructs(std::string &,std::string&,KeyValueStoreAccessor*&,int &);

	   template<typename T,typename N>
	   int spawn_kvstream(std::string &s,std::string &a,int maxsize,int n1,int n2,int n3)
	   {

		int prev = nstreams.fetch_add(1);
		k_args[prev].tname = s;
		k_args[prev].attr_name = a;
		k_args[prev].tid = prev;
		k_args[prev].maxsize = maxsize;
		k_args[prev].ffreq = n1;
		k_args[prev].wdelay = n2;
		k_args[prev].ifreq = n3;
		std::string streamname = s+a;
		std::pair<std::string,int> p(streamname,prev);
		kvindex.insert(p);
		stream_flags[prev].store(0);

		prepare_inverted_list<T,N>(&k_args[prev]);

		std::function<void(struct kstream_args *)> 
		KVStream(std::bind(&KeyValueStore::cacheflushInvList<T,N>,this, std::placeholders::_1));
	
	 	std::thread t{KVStream,&k_args[prev]};	
		kstreams[prev] = std::move(t);

		return prev;
	   }

	   int start_session(std::string &name,std::string &attrname,KeyValueStoreMetadata &m,int maxsize,int n1,int n2,int n3)
	   {
		
		 createKeyValueStoreEntry(name,m);
		 addKeyValueStoreInvList(name,attrname,maxsize);
		 std::string type = m.get_type(attrname);
		 int session_index = -1;
		 if(type.compare("int")==0)
		 {
		   session_index = spawn_kvstream<integer_invlist,int>(name,attrname,maxsize,n1,n2,n3);
		 }
		 else if(type.compare("unsignedlong")==0)
		 {
		   session_index = spawn_kvstream<unsigned_long_invlist,unsigned long>(name,attrname,maxsize,n1,n2,n3);
		 }
		 else if(type.compare("float")==0)
		 {
		   session_index = spawn_kvstream<float_invlist,float>(name,attrname,maxsize,n1,n2,n3);
		 }
		 else if(type.compare("double")==0)
		 {
		    session_index = spawn_kvstream<double_invlist,double>(name,attrname,maxsize,n1,n2,n3);
		 }
		 return session_index;

	   }

	   void close_sessions()
	   {

		io_layer->query_service_end();

		std::string s = "endsession";
		bool b = if_q->EndEmulatorSession(s,myrank);
		
	        for(int i=0;i<nstreams.load();i++) stream_flags[i].store(1);
                for(int i=0;i<nstreams.load();i++) kstreams[i].join();
	
		io_layer->end_io();

		s = "shutdown";
		b = false;
		do
		{
		  b = false;
		  b = if_q->ShutDownEmulator(s,myrank);
		}while(!b);

	   }
	   ~KeyValueStore()
	   {

		delete tables;
		delete io_layer;
		delete mds;
		delete ds;
		delete if_q;
		std::free(stream_flags);
		H5close();

	   }




};



#endif
