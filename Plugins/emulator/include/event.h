#ifndef __EVENT_H_
#define __EVENT_H_

#include <climits>
#include <string.h>
#include <iostream>
#include <string.h>
#include <vector>
#include <boost/thread/mutex.hpp>
#include <boost/thread/lock_types.hpp>
#include <boost/thread/shared_mutex.hpp>

using namespace boost;

struct event
{
    uint64_t ts;
    char*data;
};

struct atomic_buffer
{
    boost::shared_mutex m;
    std::atomic <int> buffer_size;
    std::vector <struct event>*buffer;
    std::vector <char>*datamem;
    std::vector <std::atomic <int>>*valid;
};

std::string pack_event(struct event*, int);

void unpack_event(struct event*, std::string &);

#endif
