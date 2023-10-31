#ifndef __QUERY_ENGINE_H_
#define __QUERY_ENGINE_H_

#include "query_request.h"
#include "query_response.h"
#include "distributed_queues.h"
#include "query_parser.h"
#include "rw.h"
//#include "external_sort.h"
#include <fstream>

struct thread_arg_q
{
 int tid;
};

class query_engine
{
   private:
	int numprocs;
	int myrank;
	distributed_queues *Q;
 	query_parser *S;  
        dsort *ds;	
	read_write_process *rwp;
	data_server_client *dsc;
	//hdf5_sort *hs;
 	std::vector<struct thread_arg_q> t_args;
	std::vector<std::thread> workers;
	int numthreads;
	std::atomic<int> end_session;
	std::atomic<int> end_request;
	std::atomic<int> query_number;
	std::unordered_map<std::string,std::pair<int,int>> buffer_names;
	std::vector<struct atomic_buffer*> sbuffers;
	std::mutex m1;
	std::unordered_map<int,struct query_response*> response_table; 
	std::vector<std::string> remoteshmaddrs;
	std::vector<std::string> remoteipaddrs;
	std::vector<std::string> remoteserveraddrs;

   public:
	query_engine(int n,int r,data_server_client *c,read_write_process *w) : numprocs(n), myrank(r), dsc(c), rwp(w)
	{

    	   Q = new distributed_queues(numprocs,myrank);
	   //hs = new hdf5_sort(n,r);
	   tl::engine *t_server = dsc->get_thallium_server();
           tl::engine *t_server_shm = dsc->get_thallium_shm_server();
           tl::engine *t_client = dsc->get_thallium_client();
           tl::engine *t_client_shm = dsc->get_thallium_shm_client();
           std::vector<tl::endpoint> server_addrs = dsc->get_serveraddrs();
           std::vector<std::string> ipaddrs = dsc->get_ipaddrs();
           std::vector<std::string> shmaddrs = dsc->get_shm_addrs();
	   Q->server_client_addrs(t_server,t_client,t_server_shm,t_client_shm,ipaddrs,shmaddrs,server_addrs);
   	   Q->bind_functions();
	   query_number.store(0);
	   int nreq = 0;
	   MPI_Request *reqs = new MPI_Request[2*numprocs];
	   int send_v = 1;
	   int tag = 10000;
	   std::vector<int> recvv(numprocs);
	   std::fill(recvv.begin(),recvv.end(),0);

	   for(int i=0;i<numprocs;i++)
	   {	
	     MPI_Isend(&send_v,1,MPI_INT,i,tag,MPI_COMM_WORLD,&reqs[nreq]);
	     nreq++;
	     MPI_Irecv(&recvv[i],1,MPI_INT,i,tag,MPI_COMM_WORLD,&reqs[nreq]);
	     nreq++;
	   }

	   MPI_Waitall(nreq,reqs,MPI_STATUS_IGNORE);
	   delete reqs;
	   S = new query_parser(numprocs,myrank);
	   ds = rwp->get_sorter();
	   end_session.store(0);
	   end_request.store(0);
	   numthreads = 1;
	   t_args.resize(numthreads);
	   workers.resize(numthreads);
	   std::function<void(struct thread_arg_q *)> QSFunc(
           std::bind(&query_engine::service_query,this, std::placeholders::_1));

	   std::function<void(struct thread_arg_q*)> QRFunc(
	   std::bind(&query_engine::service_response,this,std::placeholders::_1));

	   for(int i=0;i<numthreads;i++)
	   {
             t_args[i].tid = i;
	     if(i==0)
	     {
	        std::thread qe{QSFunc,&t_args[i]};
	        workers[i] = std::move(qe);
	     }
	     else
	     {
		std::thread qr{QRFunc,&t_args[i]};
		workers[i] = std::move(qr);
	     }
	   }

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

	~query_engine()
	{
	    for(int i=0;i<workers.size();i++) workers[i].join();
	    delete Q;
	    delete S;
	    //delete hs;
	}

	void end_session_flag()
	{
	   end_session.store(1);
	}
	void end_sessions()
	{
	   std::string s = "endsession";
	   //send_query(s);
	   for(int i=0;i<workers.size();i++) workers[i].join();
	}

	void end_service()
	{
	   for(int i=0;i<workers.size();i++)
		   workers[i].join();
	}

	void sort_file(std::string &s);
	void send_query(std::string &s);
	void query_point(std::string&,uint64_t);
	void sort_response(std::string&,int,std::vector<struct event>*,uint64_t&);
	void get_range(std::vector<struct event>*,std::vector<struct event>*,std::vector<struct event>*,uint64_t minkeys[3],uint64_t maxkeys[3],int);
	void service_query(struct thread_arg_q*);
	void service_response(struct thread_arg_q*);
	bool end_file_read(bool,int);
	int count_end_of_session();

};

#endif




