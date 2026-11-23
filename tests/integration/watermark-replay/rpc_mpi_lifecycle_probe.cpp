// MPI-first public client and physical native-retirement regression probe.
#include <chronolog_client.h>
#include <abt.h>
#include <mpi.h>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <unistd.h>

namespace
{
int exerciseClient(char const* config, int rank)
{
    chronolog::ClientConfiguration configuration;
    if(!configuration.load_from_file(config))
        return 10;
    chronolog::Client client(configuration.PORTAL_CONF);
    int status = client.Connect();
    std::printf("rank=%d Connect=%d\n", rank, status);
    if(status != chronolog::CL_SUCCESS)
        return 11;
    auto chronicle = "mpi_lifecycle_" + std::to_string(getpid()) + "_" + std::to_string(rank);
    status = client.CreateChronicle(chronicle);
    if(status != chronolog::CL_SUCCESS)
        return 12;
    auto acquired = client.AcquireStory(chronicle, "story");
    std::printf("rank=%d Acquire=%d handle=%d\n", rank, acquired.first, acquired.second != nullptr);
    if(acquired.first != chronolog::CL_SUCCESS || acquired.second == nullptr)
        return 13;
    auto stamped = acquired.second->log_event("MPI-first native-lifetime proof");
    std::printf("rank=%d Write=%llu\n", rank, static_cast<unsigned long long>(stamped));
    if(stamped == 0)
        return 14;
    if(client.ReleaseStory(chronicle, "story") != chronolog::CL_SUCCESS ||
       client.DestroyStory(chronicle, "story") != chronolog::CL_SUCCESS ||
       client.DestroyChronicle(chronicle) != chronolog::CL_SUCCESS)
        return 15;
    status = client.Disconnect();
    std::printf("rank=%d Disconnect=%d\n", rank, status);
    return status == chronolog::CL_SUCCESS ? 0 : 16;
}
} // namespace

int main(int argc, char** argv)
{
    if(argc != 2)
    {
        std::fprintf(stderr, "usage: %s CLIENT_CONFIG\n", argv[0]);
        return 2;
    }
    const int before = ABT_initialized();
    if(MPI_Init(&argc, &argv) != MPI_SUCCESS)
        return 3;
    int rank = -1, size = 0;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    const int afterInit = ABT_initialized();
    const int input = rank + 1;
    int sum = -1;
    int status = MPI_Allreduce(&input, &sum, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
    if(status == MPI_SUCCESS)
        status = MPI_Barrier(MPI_COMM_WORLD);
    if(status == MPI_SUCCESS && sum != size * (size + 1) / 2)
        status = 4;
    if(before != ABT_ERR_UNINITIALIZED || afterInit != ABT_ERR_UNINITIALIZED)
        status = 5;
    if(status == MPI_SUCCESS)
        status = exerciseClient(argv[1], rank);

    // Client destruction queues cleanup. Observe actual physical retirement,
    // while MPI is still live, rather than treating Disconnect as that proof.
    auto expiry = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while(status == 0 && ABT_initialized() != ABT_ERR_UNINITIALIZED && std::chrono::steady_clock::now() < expiry)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const int afterClient = ABT_initialized();
    if(status == 0 && afterClient != ABT_ERR_UNINITIALIZED)
        status = 6;
    int combined = -1;
    int collective = MPI_Allreduce(&status, &combined, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
    const int finish = MPI_Finalize();
    const int afterFinalize = ABT_initialized();
    std::printf("rank=%d size=%d status=%d combined=%d collective=%d sum=%d expected=%d finalize=%d "
                "ABT_before=%d ABT_after_MPI_init=%d ABT_after_Client=%d ABT_after_MPI_finalize=%d uninitialized=%d\n",
                rank,
                size,
                status,
                combined,
                collective,
                sum,
                size * (size + 1) / 2,
                finish,
                before,
                afterInit,
                afterClient,
                afterFinalize,
                ABT_ERR_UNINITIALIZED);
    if(collective != MPI_SUCCESS || finish != MPI_SUCCESS || afterFinalize != ABT_ERR_UNINITIALIZED)
        return 7;
    return combined;
}
