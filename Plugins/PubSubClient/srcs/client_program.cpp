#include "PSClient.h"


int main(int argc,char **argv)
{
   int prov;

   MPI_Init_thread(&argc,&argv,MPI_THREAD_MULTIPLE,&prov);
   assert (prov == MPI_THREAD_MULTIPLE);

   int size,rank;
   MPI_Comm_size(MPI_COMM_WORLD,&size);
   MPI_Comm_rank(MPI_COMM_WORLD,&rank);

   pubsubclient *p = new pubsubclient(size,rank);

   std::string sname = "table1";
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

   int total_size = 4096;
   int size_per_proc = total_size/size;
   int rate = 20000;
   int pub_rate = 100000;
   bool bcast = true;

   p->CreatePubSubWorkload(sname,names[0],m,size_per_proc,rate,pub_rate,bcast);

  delete p;


  MPI_Finalize();

}
