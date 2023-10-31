#include "process.h"


void emu_process::spawn_post_processing()
{

	/*MPI_Comm everyone;
	int psize;
	MPI_Comm_size(MPI_COMM_WORLD,&psize);
	std::string slave_program = "/home/asasidharan/FrontEnd/build/emu/post_processing";
	int nprocs = numprocs;
	if(myrank==0)
	{
	   std::cout <<" nprocs = "<<nprocs<<std::endl;
	   MPI_Comm_spawn(slave_program.c_str(),MPI_ARGV_NULL,nprocs,MPI_INFO_NULL,0,MPI_COMM_SELF,&everyone,MPI_ERRCODES_IGNORE);

	   int csize;
	   MPI_Comm_size(everyone,&csize);
	}*/

}
