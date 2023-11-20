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

    std::string sname1 = "table0";
    std::string sname2 = "table1";
    std::string sname3 = "table2";
    std::string sname4 = "table3";
    int n = 4;
    std::vector <std::string> types;
    types.push_back("int");
    types.push_back("int");
    types.push_back("int");
    types.push_back("int");
    std::vector <std::string> names;
    names.push_back("name1");
    names.push_back("name2");
    names.push_back("name3");
    names.push_back("name4");
    std::vector <int> lens;
    lens.push_back(sizeof(int));
    lens.push_back(sizeof(int));
    lens.push_back(sizeof(int));
    lens.push_back(sizeof(int));
    int len = names.size() * sizeof(int);
    KeyValueStoreMetadata m1(sname1, n, types, names, lens, len);
    KeyValueStoreMetadata m2(sname2, n, types, names, lens, len);
    KeyValueStoreMetadata m3(sname3, n, types, names, lens, len);
    KeyValueStoreMetadata m4(sname4, n, types, names, lens, len);

    std::string sname5 = "table1";
    n = 2;
    types.clear();
    types.push_back("double");
    names.clear();
    names.push_back("value1");
    lens.clear();
    lens.push_back(sizeof(double));
    types.push_back("char");
    names.push_back("value2");
    lens.push_back(100);
    len = sizeof(double) + 100;
    KeyValueStoreMetadata m5(sname5, n, types, names, lens, len);


    int id = k->start_session(sname5, names[0], m5, 32768);

    k->create_keyvalues <double_invlist, double>(id, 4096);

    std::vector <int> keys1;
    std::vector <uint64_t> ts1;

    std::vector <float> fkeys;
    std::vector <uint64_t> tsf;
    std::vector <int> op;
    //std::string tname = "timeseries.log";
    //k->get_ycsb_timeseries_workload(tname,fkeys,tsf,op);
    /*
    std::vector<uint64_t> dkeys;
    std::vector<uint64_t> tsk;
    std::vector<int> opd;
    std::string dname = "covid.h5";
    k->get_dataworld_workload(dname,dkeys,tsk,opd);*/
    //k->get_testworkload(sname1,keys1,ts1,0);

    /*std::vector<int> keys2;
    std::vector<uint64_t> ts2;
    k->get_testworkload(sname2,keys2,ts2,0);

    std::vector<int> keys3;
    std::vector<uint64_t> ts3;
    k->get_testworkload(sname3,keys3,ts3,0);

    std::vector<int> keys4;
    std::vector<uint64_t> ts4;
    k->get_testworkload(sname4,keys4,ts4,0);
 */
    /*k->spawn_kvstream<integer_invlist,int>(sname2,names[0],keys2,ts2);
    k->spawn_kvstream<integer_invlist,int>(sname3,names[0],keys3,ts3);
    k->spawn_kvstream<integer_invlist,int>(sname4,names[0],keys4,ts4);
 */
    k->close_sessions();


    MPI_Barrier(MPI_COMM_WORLD);

    delete k;

    MPI_Finalize();


}
