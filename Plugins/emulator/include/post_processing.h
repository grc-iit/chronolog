#ifndef __POST_PROCESSING_H_
#define __POST_PROCESSING_H_


#include "external_sort.h"
#include "inverted_list.h"
#include <mpi.h>
#include <iostream>


struct hashfcn
{

    uint64_t operator()(int k)
    {
        uint64_t key = std::hash <int>()(k);
        std::size_t seed = 0;
        boost::hash_combine(seed, key);
        key = seed;
        return key;
    }

};

class file_post_processing
{

private :
    int myrank;
    int numprocs;
    hdf5_sort*hs;
    hdf5_invlist <int, uint64_t, hashfcn, std::equal_to <int>>*iv;

public :
    file_post_processing(int n, int p): numprocs(n), myrank(p)
    {

        H5open();
        hs = new hdf5_sort(numprocs, myrank);
        iv = new hdf5_invlist <int, uint64_t, hashfcn, std::equal_to <int>>(numprocs, myrank);
    }

    void sort_on_secondary_key()
    {

        std::string s = "table0";

        std::string attrname = "attr" + std::to_string(0);
        std::string attr_type = "integer";
        std::string outputfile = hs->sort_on_secondary_key <int>(s, attrname, 0, 0, UINT64_MAX, attr_type);

        hs->merge_tree <int>(s, 0);
    }

    void create_invlist(std::string &s, int maxsize, int kt, int offset)
    {
        if(kt == 0)
        {
            iv->create_invlist(s, maxsize);
            iv->fill_invlist_from_file(s, offset);
        }
        else if(kt == 1)
        {
            /*iv->create_invlist<float>(s,maxsize);
            iv->fill_invlist_from_file<float>(s,offset);*/
        }
        else if(kt == 2)
        {
            /*iv->create_invlist<double>(s,maxsize);
            iv->fill_invlist_from_file<double>(s,offset);*/
        }
    }

    ~file_post_processing()
    {
        delete hs;
        delete iv;
        H5close();
    }


};

#endif
