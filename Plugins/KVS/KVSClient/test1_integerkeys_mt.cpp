#include <iostream>
#include "KeyValueStore.h"

struct mthread_arg
{
  KeyValueStore *k;
  int index;
  int nreq;
  int rate;
};

void wthread(struct mthread_arg *m)
{

   m->k->create_keyvalues<integer_invlist,int>(m->index,m->nreq,m->rate);

}

int main(int argc,char **argv)
{

  int prov;

   MPI_Init_thread(&argc,&argv,MPI_THREAD_MULTIPLE,&prov);

   int size,rank;

   MPI_Comm_size(MPI_COMM_WORLD,&size);
   MPI_Comm_rank(MPI_COMM_WORLD,&rank);

   KeyValueStore *k = new KeyValueStore(size,rank);

   std::string sname = "table2";
   int n = 3;
   std::vector<std::string> types;
   std::vector<std::string> names;
   std::vector<int> lens;
   types.push_back("int");
   names.push_back("value1");
   lens.push_back(sizeof(int));
   types.push_back("int");
   names.push_back("value2");
   lens.push_back(sizeof(int));
   types.push_back("char");
   names.push_back("value3");
   lens.push_back(200);
   int len = sizeof(int)*2+200;
   KeyValueStoreMetadata m(sname,n,types,names,lens,len);

   std::string sname1 = "table3";
   n = 3;
   KeyValueStoreMetadata m1(sname1,n,types,names,lens,len);

   std::string sname2 = "table4";
   KeyValueStoreMetadata m2(sname2,n,types,names,lens,len);

   std::string sname3 = "table5";
   KeyValueStoreMetadata m3(sname3,n,types,names,lens,len);
  
   auto t1 = std::chrono::high_resolution_clock::now();
   int nloops = 1;
   int nticks = 50;
   int ifreq = 200;
   int s1 = k->start_session(sname,names[0],m,32768,nloops,nticks,ifreq);

   nloops = 1;
   nticks = 50;
   int s2 = k->start_session(sname1,names[1],m1,32768,nloops,nticks,ifreq);
   
   nloops = 1;
   nticks = 50;
   int s3 = k->start_session(sname2,names[0],m2,32768,nloops,nticks,ifreq);
   nloops = 1;
   nticks = 50;
   int s4 = k->start_session(sname3,names[0],m3,32768,nloops,nticks,ifreq);

   int tdw = 65536*8;
   int td = tdw/size;
   int numthreads = 4;

   std::vector<struct mthread_arg> args(numthreads);
   std::vector<std::thread> workers(numthreads);

   for(int i=0;i<numthreads;i++)
   {
	args[i].k = k;
	args[i].index = i;
	args[i].nreq = td;
	args[i].rate = 10000;

	std::thread t{wthread,&args[i]};
	workers[i] = std::move(t);
   }

   for(int i=0;i<numthreads;i++) workers[i].join();

   k->close_sessions();

   delete k;
   auto t2 = std::chrono::high_resolution_clock::now();
   double t = std::chrono::duration<double>(t2-t1).count();
   double total_time = 0;
   MPI_Allreduce(&t,&total_time,1,MPI_DOUBLE,MPI_MAX,MPI_COMM_WORLD);
   if(rank==0) 
   {
	   std::cout <<" num put-get = "<<tdw*numthreads<<std::endl;
	   std::cout <<" Total time = "<<total_time<<std::endl;
   }
   MPI_Finalize();

}
