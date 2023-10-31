#include "post_processing.h"



int main(int argc,char **argv)
{
   int numprocs,rank;

   int prov;

   MPI_Comm parent;

   MPI_Init_thread(&argc,&argv,MPI_THREAD_MULTIPLE,&prov);

   //MPI_Comm_get_parent(&parent);

   int psize = 0;
   //MPI_Comm_remote_size(parent,&psize);


   MPI_Comm_size(MPI_COMM_WORLD,&numprocs);
   MPI_Comm_rank(MPI_COMM_WORLD,&rank);

   file_post_processing *fp = new file_post_processing(numprocs,rank);

   auto t1 = std::chrono::high_resolution_clock::now();

   //fp->sort_on_secondary_key(); 

   char processor_name[1024];
   int len = 0;
   MPI_Get_processor_name(processor_name, &len);

   std::string procname(processor_name);

   std::string str = "table0";
   int maxtablesize = 8192;
   int offset = 0;
   fp->create_invlist(str,maxtablesize,0,offset);

   auto t2 = std::chrono::high_resolution_clock::now();

   double total_time = std::chrono::duration<double> (t2-t1).count();

   double max_time = 0;

   MPI_Allreduce(&total_time,&max_time,1,MPI_DOUBLE,MPI_MAX,MPI_COMM_WORLD);

   if(rank==0) std::cout <<" total_time = "<<max_time<<std::endl;

   delete fp;

   MPI_Finalize();

}
