#include <iostream>
#include "KeyValueStore.h"


int main(int argc,char **argv)
{

  int prov;

   MPI_Init_thread(&argc,&argv,MPI_THREAD_MULTIPLE,&prov);

   int size,rank;

   MPI_Comm_size(MPI_COMM_WORLD,&size);
   MPI_Comm_rank(MPI_COMM_WORLD,&rank);

   KeyValueStore *k = new KeyValueStore(size,rank);

   std::string sname = "table3";
   int n = 2;
   std::vector<std::string> types;
   std::vector<std::string> names;
   std::vector<int> lens;
   types.push_back("float");
   names.push_back("value1");
   lens.push_back(sizeof(float));
   types.push_back("char");
   names.push_back("value2");
   lens.push_back(200);
   int len = sizeof(float)+200;
   KeyValueStoreMetadata m(sname,n,types,names,lens,len);

   int nloops = 2;
   int nticks = 100;
   int ifreq = 200; 
   int s = k->start_session(sname,names[0],m,32768,nloops,nticks,ifreq);

   k->create_keyvalues<float_invlist,float>(s,1024,200000);

   k->close_sessions();

   delete k;
   MPI_Finalize();

}
