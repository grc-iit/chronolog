#ifndef __DISTRIBUTED_SORT_H_
#define __DISTRIBUTED_SORT_H_

#include "event.h"
#include <mpi.h>
#include <vector>
#include <iostream>
#include <unordered_map>
#include "event_metadata.h"

class dsort
{

private:
    std::vector <std::vector <struct event>*> events;
    std::vector <std::vector <char>*> data;
    int numprocs;
    int myrank;

public:
    dsort(int n, int p): numprocs(n), myrank(p)
    {
    }

    ~dsort()
    {

    }

    int create_sort_buffer()
    {
        std::vector <struct event>*ev = nullptr;
        events.push_back(ev);
        std::vector <char>*ed = nullptr;
        data.push_back(ed);
        return events.size() - 1;
    }

    void get_unsorted_data(std::vector <struct event>*inp, std::vector <char>*inp_d, int index)
    {
        events[index] = inp;
        data[index] = inp_d;
    }

    bool sort_data(int, int, int, uint64_t &, uint64_t &, event_metadata &);

    std::vector <struct event>*get_sorted_data(int index)
    {
        return events[index];
    }

};

#endif
