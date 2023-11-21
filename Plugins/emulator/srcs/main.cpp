#include "process.h"
#include <cassert>
#include <thallium.hpp>
#include <chrono>
#include <sched.h>
#include <thread>
#include "rw_request.h"
#include "event_metadata.h"


namespace tl = thallium;
using namespace boost::interprocess;
using namespace std::chrono_literals;

int main(int argc, char**argv)
{

    int p;

    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &p);

    assert(p == MPI_THREAD_MULTIPLE);

    int size, rank;
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    cpu_set_t cpu;
    pthread_t self = pthread_self();

    pthread_getaffinity_np(self, sizeof(cpu_set_t), &cpu);

    int num_cores = 0;
    for(int i = 0; i < CPU_SETSIZE; i++)
    {
        if(CPU_ISSET(i, &cpu)) num_cores++;
    }

    //std::cout <<" rank = "<<rank<<" num_cores = "<<num_cores<<std::endl;

    auto t1 = std::chrono::high_resolution_clock::now();

    emu_process*np = new emu_process(size, rank, num_cores);

    np->bind_functions();

    np->synchronize();

    auto t2 = std::chrono::high_resolution_clock::now();

    double t = std::chrono::duration <double>(t2 - t1).count();

    int num_writer_threads = 4;

    int nbatches = 1;

    t1 = std::chrono::high_resolution_clock::now();

    //np->data_streams_s(story_names,total_events,nbatches);

    //np->generate_queries(story_names);

    //np->end_sessions_t();
    //
    /*std::string s = "endsessions";
    np->end_sessions(s);
  */
    while(np->process_end() == 0);

    t2 = std::chrono::high_resolution_clock::now();
    t = std::chrono::duration <double>(t2 - t1).count();

    double send_v = t;
    std::vector <double> recv_v(size);

    MPI_Request*reqs = new MPI_Request[2 * size];

    int nreq = 0;
    int tag = 300000;

    for(int i = 0; i < size; i++)
    {
        MPI_Isend(&send_v, 1, MPI_DOUBLE, i, tag, MPI_COMM_WORLD, &reqs[nreq]);
        nreq++;
        MPI_Irecv(&recv_v[i], 1, MPI_DOUBLE, i, tag, MPI_COMM_WORLD, &reqs[nreq]);
        nreq++;
    }

    MPI_Waitall(nreq, reqs, MPI_STATUS_IGNORE);

    delete reqs;

    if(rank == 0)
    {
        std::cout << " session flag " << std::endl;
    }

    delete np;
    MPI_Finalize();

}
