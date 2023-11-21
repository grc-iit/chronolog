#ifndef __RW_REQUEST_H_
#define __RW_REQUEST_H_

#include <thread>
#include "rw.h"
#include "query_parser.h"

struct thread_arg
{
    int tid;
    read_write_process*np;
    int num_events;
    std::string name;
    std::pair <uint64_t, uint64_t> range;
    std::vector <hsize_t> total_records;
    std::vector <hsize_t> offsets;
    std::vector <hsize_t> numrecords;
};

void search_events(struct thread_arg*t);

void get_events_range(struct thread_arg*t);

#endif
