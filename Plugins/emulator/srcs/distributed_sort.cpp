#include "distributed_sort.h"
#include <string.h>
#include <algorithm>

bool compare_fn(struct event &e1, struct event &e2) 
{
    return e1.ts <= e2.ts;
}

bool dsort::sort_data(int index,int tag,int size,uint64_t& min_v,uint64_t &max_v,event_metadata &em)
{

  int datasize = em.get_datasize();

  int total_events = 0;

   int local_events = size;
  

   std::vector<uint64_t> mysplitters;
   if(local_events >= 2)
   {
     int r1 = random()%local_events;

     int r2 = r1;
   
     do
     {
	r2 = random()%local_events;
     }while(r2==r1);
   
     mysplitters.push_back((*events[index])[r1].ts);
     mysplitters.push_back((*events[index])[r2].ts);
   }
   else if(local_events==1) mysplitters.push_back((*events[index])[0].ts);

   std::vector<int> splitter_counts(numprocs);
   std::fill(splitter_counts.begin(),splitter_counts.end(),0);
   std::vector<int> splitter_counts_l(numprocs);

   splitter_counts_l[myrank] = mysplitters.size();

   MPI_Request *reqs = new MPI_Request[3*numprocs];
   assert(reqs != nullptr);

   int nreq = 0;
   for(int i=0;i<numprocs;i++)
   {
	MPI_Isend(&splitter_counts_l[myrank],1,MPI_INT,i,tag,MPI_COMM_WORLD,&reqs[nreq]);
	nreq++;
   }

   for(int i=0;i<numprocs;i++)
   {
	MPI_Irecv(&splitter_counts[i],1,MPI_INT,i,tag,MPI_COMM_WORLD,&reqs[nreq]);
	nreq++;
   }

   MPI_Waitall(nreq,reqs,MPI_STATUS_IGNORE);

   int num_splitters = 0;
   for(int i=0;i<numprocs;i++) num_splitters += splitter_counts[i];

   
   if(num_splitters==0) return false;

   if(num_splitters > 0)
   {
     std::vector<uint64_t> splitters;
     splitters.resize(num_splitters);

     std::vector<int> displ(numprocs);
     std::fill(displ.begin(),displ.end(),0);

     for(int i=1;i<numprocs;i++)
	   displ[i] = displ[i-1]+splitter_counts[i-1];

    nreq = 0;
    for(int i=0;i<numprocs;i++)
    {
	if(splitter_counts[myrank]>0)
	{
	  MPI_Isend(mysplitters.data(),splitter_counts[myrank],MPI_UINT64_T,i,tag,MPI_COMM_WORLD,&reqs[nreq]);
	  nreq++;
	}
   }

   for(int i=0;i<numprocs;i++)
   {
	if(splitter_counts[i] > 0)
	{
	  MPI_Irecv(&splitters[displ[i]],splitter_counts[i],MPI_UINT64_T,i,tag,MPI_COMM_WORLD,&reqs[nreq]);
	  nreq++;
	}
   }

   MPI_Waitall(nreq,reqs,MPI_STATUS_IGNORE);

   std::sort(splitters.begin(),splitters.end());

   int splitters_per_proc =  num_splitters/numprocs;
   int rem = num_splitters%numprocs;
   int offset = rem*(splitters_per_proc+1);

   mysplitters.clear();

   std::vector<int> procs;

   for(int i=0;i<splitters.size();i++)
   {
	int proc=-1;
	if(i < offset) 
	{
	   proc = i/(splitters_per_proc+1);
	}
	else proc = rem+((i-offset)/splitters_per_proc);
	procs.push_back(proc);
   }

   std::vector<int> send_counts(numprocs);
   std::vector<int> recv_counts(numprocs);
   std::vector<int> recv_displ(numprocs);
   std::vector<int> send_displ(numprocs);
   std::fill(send_counts.begin(),send_counts.end(),0);
   std::fill(send_displ.begin(),send_displ.end(),0);
   std::fill(recv_counts.begin(),recv_counts.end(),0);
   std::fill(recv_displ.begin(),recv_displ.end(),0);

   std::vector<int> event_dest;

   std::vector<int> event_count(numprocs);
   std::fill(event_count.begin(),event_count.end(),0);

   int keyvaluesize = sizeof(uint64_t)+datasize;
   for(int i=0;i<size;i++)
   {
	int dest = -1;
        uint64_t ts = (*events[index])[i].ts;
	for(int j=0;j<splitters.size();j++)
	{
	    if(ts <= splitters[j])
	    {
		 dest = procs[j]; break;
	    }
	}
	if(dest == -1) dest = procs[splitters.size()-1];
	send_counts[dest]++;
	event_dest.push_back(dest);	
   }

   for(int i=1;i<numprocs;i++)
	send_displ[i] = send_displ[i-1]+send_counts[i-1]*keyvaluesize;

   std::fill(recv_displ.begin(),recv_displ.end(),0);

   nreq = 0;
   for(int i=0;i<numprocs;i++)
   {
	MPI_Isend(&send_counts[i],1,MPI_INT,i,tag,MPI_COMM_WORLD,&reqs[nreq]);
	nreq++;
	MPI_Irecv(&recv_counts[i],1,MPI_INT,i,tag,MPI_COMM_WORLD,&reqs[nreq]);
	nreq++;
   }

   MPI_Waitall(nreq,reqs,MPI_STATUS_IGNORE);

   int total_recv_size = 0;
   for(int i=0;i<numprocs;i++)
	   total_recv_size += recv_counts[i];

   std::vector<char> send_buffer;
   std::vector<char> recv_buffer;

   send_buffer.resize(size*keyvaluesize);
   recv_buffer.resize(total_recv_size*keyvaluesize);

   for(int i=0;i<size;i++)
   {
	int dest = event_dest[i];
	int p = send_displ[dest];
	std::memcpy(&send_buffer[p],&((*events[index])[i].ts),sizeof(uint64_t));
	uint64_t sts = *(uint64_t*)(&(send_buffer[p]));
	p+=sizeof(uint64_t);
	std::memcpy(&send_buffer[p],(*events[index])[i].data,datasize);
	p+=datasize;
	send_displ[dest]+=keyvaluesize;
   }
   
   std::fill(send_displ.begin(),send_displ.end(),0);

   for(int i=1;i<numprocs;i++)
	   send_displ[i] = send_displ[i-1]+send_counts[i-1]*keyvaluesize;

   for(int i=1;i<numprocs;i++)
	   recv_displ[i] = recv_displ[i-1]+keyvaluesize*recv_counts[i-1];

   nreq = 0;
   for(int i=0;i<numprocs;i++)
   {
	if(send_counts[i]>0)
	{
	   MPI_Isend(&send_buffer[send_displ[i]],send_counts[i]*keyvaluesize,MPI_CHAR,i,tag,MPI_COMM_WORLD,&reqs[nreq]);
	   nreq++;
	}
	if(recv_counts[i]>0)
	{
	   MPI_Irecv(&recv_buffer[recv_displ[i]],recv_counts[i]*keyvaluesize,MPI_CHAR,i,tag,MPI_COMM_WORLD,&reqs[nreq]);
	   nreq++;
	}

   }

   MPI_Waitall(nreq,reqs,MPI_STATUS_IGNORE);

   events[index]->clear();
   try
   {
     data[index]->resize(total_recv_size*datasize);
   }
   catch(const std::exception &except)
   {
	std::cout<<except.what()<<std::endl;
	exit(-1);
   }

   int k=0;
   for(int i=0;i<numprocs;i++)
   {
	   for(int j=0,c=0;j<recv_counts[i]*keyvaluesize;j+=keyvaluesize,c++)
	   {
	        int p = recv_displ[i]+j;
	        uint64_t ts = *(uint64_t*)(&(recv_buffer[p]));
		p += sizeof(uint64_t);
		int q = k+c; 
		std::memcpy(&((*data[index])[q*datasize]),&recv_buffer[p],datasize);
		struct event e;
		e.ts = ts;
		e.data = &((*data[index])[q*datasize]);
		events[index]->push_back(e);
	   }
	   k+=recv_counts[i];
   }

   std::sort(events[index]->begin(),events[index]->end(),compare_fn);


   uint64_t min_ts, max_ts;

   min_ts = UINT64_MAX; max_ts = 0;

   if(events[index]->size()>0) 
   {
      min_ts = (*(events[index]))[0].ts;
      max_ts = (*(events[index]))[events[index]->size()-1].ts;
   }

   std::vector<uint64_t> send_ts;
   send_ts.resize(2);
   send_ts[0] = min_ts;
   send_ts[1] = max_ts;
   std::vector<uint64_t> recv_ts;

   recv_ts.resize(numprocs*2);

    nreq=0;
    for(int i=0;i<numprocs;i++)
    {
	 MPI_Isend(send_ts.data(),2,MPI_UINT64_T,i,tag,MPI_COMM_WORLD,&reqs[nreq]);
	 nreq++;
	 MPI_Irecv(&recv_ts[2*i],2,MPI_UINT64_T,i,tag,MPI_COMM_WORLD,&reqs[nreq]);
	 nreq++;
    }
    MPI_Waitall(nreq,reqs,MPI_STATUS_IGNORE);

    min_v = UINT64_MAX;
    max_v = 0;
    for(int i=0;i<numprocs;i+=2)
    {
	if(recv_ts[i] < min_v) min_v = recv_ts[i];
	if(recv_ts[i+1] > max_v) max_v = recv_ts[i+1];
    }
    return true;
   }
   delete reqs; 
}
