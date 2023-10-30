#ifndef __QUERY_REQUEST_H_
#define __QUERY_REQUEST_H_


#include <string>

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


struct query_req
{
   
 std::string name;
 uint64_t minkey;
 uint64_t maxkey;
 int id;
 int sender;
 bool from_nvme;
 bool sorted;
 bool collective;
 bool single_point;
 bool output_file;
 int op;
};

template<typename A>
void serialize(A &ar,struct query_req &e)
{
        ar & e.name;
        ar & e.minkey;
        ar & e.maxkey;
        ar & e.id;
        ar & e.sender;
        ar & e.from_nvme;
        ar & e.sorted;
        ar & e.collective;
        ar & e.single_point;
        ar & e.output_file;
        ar & e.op;
}

#endif
