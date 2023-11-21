#ifndef __KeyValueStoreIO_H_
#define __KeyValueStoreIO_H_

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
#include <cassert>
#include <boost/lockfree/queue.hpp>
#include "event.h"
#include <thread>
#include <mpi.h>

struct request
{
    std::string name;
    std::string attr_name;
    int id;
    int keytype;
    int intkey;
    uint64_t unsignedlongkey;
    float floatkey;
    double doublekey;
    int sender;
    bool flush;
    bool persist;
};

struct sync_request
{
    void*funcptr;
    std::string name;
    std::string attr_name;
    int keytype;
    int offset;
    bool flush;
    bool fill;
    bool persist;
};

struct response
{
    std::string name;
    std::string attr_name;
    int id;
    int response_id;
    std::string event;
    int sender;
    bool complete;
};

struct thread_arg
{
    int tid;

};

namespace tl = thallium;

template <typename A>
void serialize(A &ar, struct request &e)
{
    ar&e.name;
    ar&e.attr_name;
    ar&e.id;
    ar&e.keytype;
    ar&e.intkey;
    ar&e.unsignedlongkey;
    ar&e.floatkey;
    ar&e.doublekey;
    ar&e.sender;
    ar&e.flush;
}

template <typename A>
void serialize(A &ar, struct response &e)
{
    ar&e.name;
    ar&e.attr_name;
    ar&e.id;
    ar&e.response_id;
    ar&e.event;
    ar&e.sender;
    ar&e.complete;
}


class KeyValueStoreIO
{

private:
    int nservers;
    int serverid;
    boost::lockfree::queue <struct sync_request*>*sync_queue;
    tl::engine*thallium_server;
    tl::engine*thallium_shm_server;
    tl::engine*thallium_client;
    tl::engine*thallium_shm_client;
    std::vector <tl::endpoint> serveraddrs;
    std::vector <std::string> ipaddrs;
    std::vector <std::string> shmaddrs;
    std::string myipaddr;
    std::string myhostname;
    int num_io_threads;
    int tag;
    std::vector <std::pair <int, void*>> service_queries;
    std::atomic <int>*query_service_alive;
    std::vector <std::thread> io_threads;
    std::vector <struct thread_arg> t_args;
    std::atomic <int> request_count;
    std::atomic <uint32_t> synchronization_word;
    boost::mutex mutex_t;
    boost::condition_variable cv;
    int semaphore;

public:

    KeyValueStoreIO(int np, int p): nservers(np), serverid(p)
    {
        num_io_threads = 1;
        semaphore = 0;
        request_count.store(0);
        synchronization_word.store(0);
        query_service_alive = (std::atomic <int>*)std::malloc(MAXSTREAMS * sizeof(std::atomic <int>));
        for(int i = 0; i < MAXSTREAMS; i++)
            query_service_alive[i].store(0);
        sync_queue = new boost::lockfree::queue <struct sync_request*>(128);
        tag = 3000;
        t_args.resize(num_io_threads);
        for(int i = 0; i < num_io_threads; i++) t_args[i].tid = i;

        io_threads.resize(num_io_threads);

    }

    void end_io()
    {
        struct sync_request*r = new sync_request();
        r->name = "ZZZZ";
        r->attr_name = "ZZZZ";
        r->keytype = -1;
        r->funcptr = nullptr;

        sync_queue->push(r);

        for(int i = 0; i < num_io_threads; i++) io_threads[i].join();


    }

    void server_client_addrs(tl::engine*t_server, tl::engine*t_client, tl::engine*t_server_shm, tl::engine*t_client_shm
                             , std::vector <std::string> &ips, std::vector <std::string> &shm_addrs
                             , std::vector <tl::endpoint> &saddrs)
    {
        thallium_server = t_server;
        thallium_shm_server = t_server_shm;
        thallium_client = t_client;
        thallium_shm_client = t_client_shm;
        ipaddrs.assign(ips.begin(), ips.end());
        shmaddrs.assign(shm_addrs.begin(), shm_addrs.end());
        myipaddr = ipaddrs[serverid];
        serveraddrs.assign(saddrs.begin(), saddrs.end());
    }

    void add_query_service(int type, void*fptr)
    {
        std::pair <int, void*> p;
        p.first = type;
        p.second = fptr;
        service_queries.push_back(p);
        query_service_alive[service_queries.size() - 1].store(1);
    }

    void close_query_service(int id)
    {
        query_service_alive[id].store(0);

    }

    bool LocalPutSyncRequest(struct sync_request*r)
    {
        sync_queue->push(r);
        return true;
    }

    struct sync_request*GetSyncRequest()
    {
        struct sync_request*r = nullptr;
        bool b = sync_queue->pop(r);
        return r;
    }

    bool SyncRequestQueueEmpty()
    {
        return sync_queue->empty();
    }

    void get_common_requests(std::vector <struct sync_request*> &, std::vector <struct sync_request*> &);

    void io_function(struct thread_arg*);

    void io_service();

    void query_service_end();

    ~KeyValueStoreIO()
    {
        std::free(query_service_alive);
        delete sync_queue;
    }


};


#endif
