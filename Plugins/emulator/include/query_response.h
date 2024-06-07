#ifndef __QUERY_RESP_H_
#define __QUERY_RESP_H_

#include <string>
#include "event.h"

#include <thallium.hpp>
#include <thallium/serialization/proc_input_archive.hpp>
#include <thallium/serialization/proc_output_archive.hpp>
#include <thallium/serialization/serialize.hpp>
#include <thallium/serialization/stl/array.hpp>
#include <thallium/serialization/stl/complex.hpp>
#include <thallium/serialization/stl/deque.hpp>
#include <thallium/serialization/stl/forward_list.hpp>
#include <thallium/serialization/stl/list.hpp>
#include <thallium/serialization/stl/map.hpp>
#include <thallium/serialization/stl/multimap.hpp>
#include <thallium/serialization/stl/multiset.hpp>
#include <thallium/serialization/stl/pair.hpp>
#include <thallium/serialization/stl/set.hpp>
#include <thallium/serialization/stl/string.hpp>
#include <thallium/serialization/stl/tuple.hpp>
#include <thallium/serialization/stl/unordered_map.hpp>
#include <thallium/serialization/stl/unordered_multimap.hpp>
#include <thallium/serialization/stl/unordered_multiset.hpp>
#include <thallium/serialization/stl/unordered_set.hpp>
#include <thallium/serialization/stl/vector.hpp>
#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#define NOTFOUND 0


struct query_resp
{
    int id;
    int response_id;
    uint64_t minkey;
    uint64_t maxkey;
    int sender;
    std::string response;
    std::string output_file;
    bool complete;
    int error_code;
};

template <typename A>
void serialize(A &ar, struct query_resp &e)
{
    ar&e.id;
    ar&e.response_id;
    ar&e.minkey;
    ar&e.maxkey;
    ar&e.sender;
    ar&e.response;
    ar&e.output_file;
    ar&e.complete;
    ar&e.error_code;
}


#endif
