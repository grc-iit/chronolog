#include "ClockSync.h"
#include <iostream>

int main(int argc,char **argv)
{

  int provided;
  MPI_Init_thread(&argc,&argv,MPI_THREAD_SINGLE,&provided);

  int rank,size;
  MPI_Comm_size(MPI_COMM_WORLD,&size);
  MPI_Comm_rank(MPI_COMM_WORLD,&rank);

  std::string unit = "millisecond";
  ClockSynchronization<ClocksourceCPPStyle> *b = new ClockSynchronization<ClocksourceCPPStyle> (rank,size,unit);

  b->SynchronizeClocks();

  b->ComputeErrorInterval();

  MPI_Finalize();

  delete b;
}
