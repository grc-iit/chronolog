#include "ClockSync.h"
#include <cmath>
#include <thread>

template <typename Clocksource>
std::pair <uint64_t, uint64_t> ClockSynchronization <Clocksource>::SynchronizeClocks()
{

    int nreq = 0;
    int tag = 50000;
    MPI_Request*reqs = (MPI_Request*)std::malloc(2 * numprocs * sizeof(MPI_Request));

    int send_v = 1;
    std::vector <int> recv_v(numprocs);
    std::fill(recv_v.begin(), recv_v.end(), 0);

    for(int i = 0; i < numprocs; i++)
    {
        MPI_Isend(&send_v, 1, MPI_INT, i, tag, MPI_COMM_WORLD, &reqs[nreq]);
        nreq++;
        MPI_Irecv(&recv_v[i], 1, MPI_INT, i, tag, MPI_COMM_WORLD, &reqs[nreq]);
        nreq++;
    }
    MPI_Waitall(nreq, reqs, MPI_STATUS_IGNORE);

    uint64_t offset_l = 0;
    nreq = 0;

    if(myrank == 0)
    {
        std::vector <int> requests;
        requests.resize(numprocs);
        std::fill(requests.begin(), requests.end(), 0);

        std::vector <uint64_t> t;
        t.resize(3 * numprocs);
        std::fill(t.begin(), t.end(), 0);

        for(int i = 1; i < numprocs; i++)
        {

            MPI_Status s;
            MPI_Recv(&requests[i], 1, MPI_INT, i, tag, MPI_COMM_WORLD, &s);
            nreq++;
            t[i * 3 + 0] = clock->getTimestamp() / unit.load();
        }

        for(int i = 1; i < numprocs; i++)
            t[i * 3 + 1] = clock->getTimestamp() / unit.load();

        nreq = 0;
        for(int i = 1; i < numprocs; i++)
        {
            t[i * 3 + 2] = clock->getTimestamp() / unit.load();
            MPI_Send(&t[i * 3], 3, MPI_UINT64_T, i, tag, MPI_COMM_WORLD);
            nreq++;
        }

    }
    else
    {
        uint64_t Treq = clock->getTimestamp() / unit.load();

        int nreq = 0;
        int req = 1;

        MPI_Send(&req, 1, MPI_INT, 0, tag, MPI_COMM_WORLD);
        nreq++;


        std::vector <uint64_t> resp;
        resp.resize(3);
        std::fill(resp.begin(), resp.end(), 0);
        MPI_Status et;

        nreq = 0;
        MPI_Recv(resp.data(), 3, MPI_UINT64_T, 0, tag, MPI_COMM_WORLD, &et);
        nreq++;

        uint64_t Tresp = clock->getTimestamp() / unit.load();
        int64_t offset1 = resp[0] - Treq;
        int64_t offset2 = resp[2] - Tresp;
        double diff = (double)(offset1 + offset2) / (double)2;
        int64_t m_offset = std::ceil(diff);
        bool isless = false;
        if(m_offset < 0)
        {
            m_offset = -1 * m_offset;
            isless = true;
        }
        uint64_t mbit = 0;
        if(isless) mbit = 1;
        mbit = mbit << 63;
        mbit = mbit|m_offset;
        is_less.store(isless);
        offset_l = mbit;
    }

    uint64_t msb = offset_l;
    msb = msb >> 63;
    uint64_t offsetv = offset_l;
    offsetv = offsetv << 1;
    offsetv = offsetv >> 1;
    std::free(reqs);
    std::pair <uint64_t, uint64_t> p(msb, offsetv);
    return p;

}

template <typename Clocksource>
void ClockSynchronization <Clocksource>::ComputeErrorInterval(uint64_t msb, uint64_t offset)
{
    MPI_Status st;
    MPI_Request*reqs = (MPI_Request*)std::malloc(2 * numprocs * sizeof(MPI_Request));
    int nreq = 0;

    int sreq = 1;
    std::vector <int> recvreq(numprocs);
    std::fill(recvreq.begin(), recvreq.end(), 0);
    uint64_t merror;
    int tag = 50000;

    nreq = 0;
    for(int i = 0; i < numprocs; i++)
    {
        MPI_Isend(&sreq, 1, MPI_INT, i, tag, MPI_COMM_WORLD, &reqs[nreq]);
        nreq++;
        MPI_Irecv(&recvreq[i], 1, MPI_INT, i, tag, MPI_COMM_WORLD, &reqs[nreq]);
        nreq++;
    }

    MPI_Waitall(nreq, reqs, MPI_STATUS_IGNORE);

    nreq = 0;

    if(myrank == 0)
    {
        std::vector <uint64_t> tstamps, terr;
        tstamps.resize(numprocs);
        terr.resize(numprocs);
        std::fill(tstamps.begin(), tstamps.end(), 0);
        std::fill(terr.begin(), terr.end(), 0);

        uint64_t ts;
        for(int i = 1; i < numprocs; i++)
        {
            MPI_Status s;
            MPI_Recv(&tstamps[i], 1, MPI_UINT64_T, i, tag, MPI_COMM_WORLD, &s);
            nreq++;
            tstamps[i] += delay.load();
            ts = clock->getTimestamp() / unit.load();
            if(msb == 0) ts += offset; else ts -= offset;
            if(ts > tstamps[i]) terr[i] = ts - tstamps[i];
            else terr[i] = tstamps[i] - ts;
        }

        merror = 0;

        for(int i = 0; i < numprocs; i++)
        {
            if(merror < terr[i]) merror = terr[i];
        }

        nreq = 0;
        for(int i = 1; i < numprocs; i++)
        {
            MPI_Send(&merror, 1, MPI_UINT64_T, i, tag, MPI_COMM_WORLD);
            nreq++;
        }

    }
    else
    {
        uint64_t ts = clock->getTimestamp() / unit.load();
        if(msb == 0) ts += offset; else ts -= offset;

        MPI_Send(&ts, 1, MPI_UINT64_T, 0, tag, MPI_COMM_WORLD);

        MPI_Status et;
        nreq = 0;
        MPI_Recv(&merror, 1, MPI_UINT64_T, 0, tag, MPI_COMM_WORLD, &et);
        nreq++;
    }

    uint64_t mbit = msb;
    mbit << 63;
    mbit = mbit|offset;
    myoffset.store(mbit);
    maxError.store(merror);
    boost::uint128_type eoffset;
    eoffset = (boost::uint128_type)myoffset.load();
    eoffset = eoffset << 64;
    eoffset = eoffset|(boost::uint128_type)maxError.load();
    offset_error.store(eoffset);

    std::vector <uint64_t> times1, times2;
    times1.resize(numprocs);
    times2.resize(numprocs);
    std::fill(times1.begin(), times1.end(), 0);
    std::fill(times2.begin(), times2.end(), 0);
    times1[myrank] = Timestamp();

    nreq = 0;

    uint64_t send_v = 1;

    for(int i = 0; i < numprocs; i++)
    {
        MPI_Isend(&send_v, 1, MPI_UINT64_T, i, tag, MPI_COMM_WORLD, &reqs[nreq]);
        nreq++;
        MPI_Irecv(&times1[i], 1, MPI_UINT64_T, i, tag, MPI_COMM_WORLD, &reqs[nreq]);
        nreq++;
    }

    MPI_Waitall(nreq, reqs, MPI_STATUS_IGNORE);

    if(myrank == 0) std::cout << " maxError = " << maxError.load() << std::endl;

    std::free(reqs);
}
