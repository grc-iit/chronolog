#include <vector>
#include <iostream>
#include <sstream>
#include "hdf5.h"
#include "h5_async_lib.h"
#include <string>
#include <cfloat>
#include <cstdlib>
#include <fstream>
#include <cstring>
#include <chrono>
#include <ctime>
#include <mpi.h>
#include <algorithm>
#include <cfloat>
#include <cmath>

struct keydata
{
    uint64_t ts;
    char*data;
};

struct keyindex
{
    uint64_t ts;
    int pos;
};


int main(int argc, char**argv)
{
    std::string filename = "timeseries_ycsb.log";

    std::string hfile = "timeseries_ycsb.h5";

    int prov;

    MPI_Init_thread(&argc, &argv, MPI_THREAD_SINGLE, &prov);

    std::ifstream ist(filename.c_str(), std::ios_base::in);

    std::vector <std::string> lines;

    int valuesize = sizeof(double);

    std::vector <uint64_t> ts;
    std::vector <float> values;

    if(ist.is_open())
    {
        std::string line;
        while(getline(ist, line))
        {
            std::stringstream ss(line);
            std::string string1;
            ss >> string1;
            if(string1.compare("INSERT") == 0)
                lines.push_back(line);
        }

        std::cout << " numlines = " << lines.size() << std::endl;

        std::string str1 = "YCSBTS=";
        std::string str2 = "YCSBV=";
        for(int i = 0; i < lines.size(); i++)
        {
            int pos1 = lines[i].find(str1.c_str()) + str1.length();
            std::string substr1 = lines[i].substr(pos1);
            std::stringstream ss(substr1);
            std::string tss;
            ss >> tss;
            uint64_t tsu = std::stoul(tss, nullptr, 0);

            int pos2 = substr1.find(str2.c_str()) + str2.length();
            std::string substr2 = substr1.substr(pos2);

            std::stringstream ess(substr2);
            std::string valuestr;
            ess >> valuestr;
            float value = std::stof(valuestr, nullptr);
            ts.push_back(tsu);
            values.push_back(value);
        }

        ist.close();
    }

    hid_t async_fapl = H5Pcreate(H5P_FILE_ACCESS);
    hid_t async_dxpl = H5Pcreate(H5P_DATASET_XFER);

    H5Pset_fapl_mpio(async_fapl, MPI_COMM_WORLD, MPI_INFO_NULL);
    H5Pset_dxpl_mpio(async_dxpl, H5FD_MPIO_COLLECTIVE);

    int keyvaluesize = sizeof(uint64_t) + valuesize;
    std::vector <struct keyindex>*data_i = new std::vector <struct keyindex>();
    std::vector <char>*data_p = new std::vector <char>();

    data_p->resize(ts.size() * keyvaluesize);

    std::vector <std::pair <uint64_t, uint64_t>> ranges;
    std::vector <int> numrecords;

    for(int i = 0; i < ts.size(); i++)
    {
        struct keyindex k;
        k.ts = ts[i];
        k.pos = i;
        data_i->push_back(k);
    }

    auto comparekeydata = [](struct keyindex &k1, struct keyindex &k2)
    { return k1.ts < k2.ts; };

    std::sort(data_i->begin(), data_i->end(), comparekeydata);

    for(int i = 0; i < ts.size(); i++)
    {
        int pos = (*data_i)[i].pos;
        *(uint64_t*)(&(*data_p)[i * keyvaluesize]) = ts[pos];
        *(double*)(&(*data_p)[i * keyvaluesize + sizeof(uint64_t)]) = (double)values[pos];
    }

    int c = -1;
    int n = 0;
    uint64_t minkey = 0;
    uint64_t maxkey = 0;

    int nblocks = std::ceil(ts.size() / 8192);

    int block_id = 0;
    for(int i = 0; i < ts.size();)
    {
        minkey = (*data_i)[i].ts;
        int end = std::min(i + 8192, (int)ts.size());
        maxkey = (*data_i)[end - 1].ts;
        numrecords.push_back(end - i);
        std::pair <uint64_t, uint64_t> p;
        p.first = minkey;
        p.second = maxkey;
        ranges.push_back(p);
        i += 8192;
    }

    const char*attr_name[1];
    hsize_t adims[1];
    adims[0] = (hsize_t)valuesize;
    hid_t s1 = H5Tarray_create(H5T_NATIVE_CHAR, 1, adims);
    hid_t s2 = H5Tcreate(H5T_COMPOUND, keyvaluesize);
    H5Tinsert(s2, "key", HOFFSET(
    struct keydata,ts),H5T_NATIVE_UINT64);
    H5Tinsert(s2, "value", HOFFSET(
    struct keydata,data),s1);

    hsize_t attr_size[1];
    attr_size[0] = MAXBLOCKS * 4 + 4;
    hid_t attr_space[1];
    attr_name[0] = "Datasizes";
    attr_space[0] = H5Screate_simple(1, attr_size, NULL);

    hsize_t total_records = (hsize_t)ts.size();
    std::vector <uint64_t> attr_data;
    attr_data.resize(attr_size[0]);

    attr_data[0] = total_records;
    attr_data[1] = 8;
    attr_data[2] = valuesize;
    attr_data[3] = numrecords.size();

    int pos = 4;
    for(int i = 0; i < numrecords.size(); i++)
    {
        attr_data[pos + i * 4] = ranges[i].first;
        attr_data[pos + i * 4 + 1] = ranges[i].second;
        attr_data[pos + i * 4 + 2] = (uint64_t)(i + 1);
        attr_data[pos + i * 4 + 3] = (uint64_t)numrecords[i];
    }

    hsize_t chunkdims[1];
    chunkdims[0] = total_records;
    hsize_t maxdims[1];
    maxdims[0] = (hsize_t)H5S_UNLIMITED;

    hid_t dataset_pl = H5Pcreate(H5P_DATASET_CREATE);
    int ret = H5Pset_chunk(dataset_pl, 1, chunkdims);
    hid_t file_dataspace = H5Screate_simple(1, &total_records, maxdims);
    std::string dstring = "Data1";
    hid_t fid = H5Fcreate(hfile.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, async_fapl);

    hid_t dataset1 = H5Dcreate(fid, dstring.c_str(), s2, file_dataspace, H5P_DEFAULT, dataset_pl, H5P_DEFAULT);
    hid_t mem_dataspace = H5Screate_simple(1, &total_records, NULL);

    hsize_t offset_w = 0;

    ret = H5Sselect_hyperslab(file_dataspace, H5S_SELECT_SET, &offset_w, NULL, &total_records, NULL);
    ret = H5Dwrite(dataset1, s2, mem_dataspace, file_dataspace, async_dxpl, data_p->data());

    hid_t attr_id[1];
    attr_id[0] = H5Acreate(dataset1, attr_name[0], H5T_NATIVE_UINT64, attr_space[0], H5P_DEFAULT, H5P_DEFAULT);
    ret = H5Awrite(attr_id[0], H5T_NATIVE_UINT64, attr_data.data());
    ret = H5Aclose(attr_id[0]);

    delete data_p;
    delete data_i;
    H5Tclose(s2);
    H5Tclose(s1);
    H5Sclose(attr_space[0]);
    H5Pclose(dataset_pl);
    H5Sclose(file_dataspace);
    H5Sclose(mem_dataspace);
    H5Dclose(dataset1);
    H5Fclose(fid);

    MPI_Finalize();


}
