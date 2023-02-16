#include "ClockSync.h"

template<typename Clocksource>
void ClockSynchronization<Clocksource>::SynchronizeClocks()
{

   if(myrank==0)
   {

      MPI_Status st;
      std::vector<int> requests;
      requests.resize(numprocs);
      std::fill(requests.begin(),requests.end(),0);

      std::vector<uint64_t> t;
      t.resize(3*numprocs);
      std::fill(t.begin(),t.end(),0);      

      for(int i=1;i<numprocs;i++)
      {
        	  
	MPI_Recv(&requests[i],1,MPI_INT,i,123,MPI_COMM_WORLD,&st);
        t[i*3+0] = Timestamp();
      }

      for(int i=1;i<numprocs;i++)
	      t[i*3+1] = Timestamp();

      for(int i=1;i<numprocs;i++)
      {
	t[i*3+2] = Timestamp();
	MPI_Send(&t[i*3],3,MPI_UINT64_T,i,123,MPI_COMM_WORLD);
      }
   }
   else 
   {
      uint64_t Treq = Timestamp();
      
      int req = 1;

      MPI_Send(&req,1,MPI_INT,0,123,MPI_COMM_WORLD);

      std::vector<uint64_t> resp;
      resp.resize(3);
      std::fill(resp.begin(),resp.end(),0);
      MPI_Status et;
      MPI_Recv(resp.data(),3,MPI_UINT64_T,0,123,MPI_COMM_WORLD,&et);
      
      uint64_t Tresp = Timestamp();
      uint64_t offset1 = resp[0]-Treq;
      uint64_t offset2 = Tresp-resp[2];
      myoffset = (offset1+offset2)/2;

   }

   MPI_Barrier(MPI_COMM_WORLD);

   //std::cout <<" rank = "<<myrank<<" offset = "<<myoffset<<" timestamp = "<<Timestamp()<<std::endl;

}

template<typename Clocksource>
void ClockSynchronization<Clocksource>::ComputeErrorInterval()
{


	if(myrank==0)
	{
	    MPI_Status st;

	    std::vector<uint64_t> tstamps,terr;
	    tstamps.resize(numprocs);
	    terr.resize(numprocs);
	    std::fill(tstamps.begin(),tstamps.end(),0);
	    std::fill(terr.begin(),terr.end(),0);

	    MPI_Barrier(MPI_COMM_WORLD);

	    uint64_t ts = Timestamp();
	    tstamps[myrank] = ts;

	    for(int i=1;i<numprocs;i++)
	    {
		MPI_Recv(&tstamps[i],1,MPI_UINT64_T,i,123,MPI_COMM_WORLD,&st);
	    }

	    for(int i=0;i<numprocs;i++)
	    {
		if(tstamps[i] > tstamps[myrank]) terr[i] = tstamps[i]-tstamps[myrank];
		else terr[i] = tstamps[myrank]-tstamps[i];
		if(maxError < terr[i]) maxError = terr[i];
	    }

	    for(int i=1;i<numprocs;i++)
	    {
		MPI_Send(&maxError,1,MPI_UINT64_T,i,123,MPI_COMM_WORLD);
	    }

	}
	else
	{
	    MPI_Barrier(MPI_COMM_WORLD);
		
	    uint64_t ts = Timestamp();

	    MPI_Send(&ts,1,MPI_UINT64_T,0,123,MPI_COMM_WORLD);

	    MPI_Status et;

	    MPI_Recv(&maxError,1,MPI_UINT64_T,0,123,MPI_COMM_WORLD,&et);
	}

        MPI_Barrier(MPI_COMM_WORLD);

	std::vector<uint64_t> times1,times2;
	times1.resize(numprocs); times2.resize(numprocs);
	std::fill(times1.begin(),times1.end(),0);
	std::fill(times2.begin(),times2.end(),0);
	times1[myrank] = Timestamp();

	MPI_Allreduce(times1.data(),times2.data(),numprocs,MPI_UINT64_T,MPI_SUM,MPI_COMM_WORLD);

	if(myrank==0) std::cout <<" maxError = "<<maxError<<std::endl;

	for(int i=0;i<numprocs;i++)
	{
	   uint64_t e;
	   if(times2[i]>times2[myrank]) e = times2[i]-times2[myrank];
	   else e = times2[myrank]-times2[i];
	   assert(e >= 0 && e < 2*maxError+epsilon);
	}
}
