#include <iostream>
#include "KeyValueStore.h"


int main(int argc, char**argv)
{

    int prov;

    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &prov);

    int size, rank;

    MPI_Comm_size(MPI_COMM_WORLD, &size);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    KeyValueStore*k = new KeyValueStore(size, rank);

    std::string sname = "timeseriesrun";
    int n = 2;
    std::vector <std::string> types;
    std::vector <std::string> names;
    std::vector <int> lens;
    types.push_back("unsignedlong");
    names.push_back("value1");
    lens.push_back(sizeof(uint64_t));
    types.push_back("float");
    names.push_back("value2");
    lens.push_back(sizeof(float));
    int len = sizeof(uint64_t) + sizeof(float);
    KeyValueStoreMetadata m(sname, n, types, names, lens, len);

    std::vector <uint64_t> keys;
    std::vector <float> values;
    std::vector <int> op;
    std::string filename = sname + ".log";
    k->get_ycsb_timeseries_workload(filename, keys, values, op);

    MPI_Barrier(MPI_COMM_WORLD);

    int nloops = 2;
    int nticks = 50;
    int ifreq = 200;
    int s = k->start_session(sname, names[0], m, 32768, nloops, nticks, ifreq);

    k->create_keyvalues <unsigned_long_invlist, uint64_t, float>(s, keys, values, op, 20000);

    k->close_sessions();

    delete k;
    MPI_Finalize();

}
