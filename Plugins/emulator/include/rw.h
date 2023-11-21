#ifndef __RW_H_
#define __RW_H_

#include <abt.h>
#include <mpi.h>
#include "ClockSync.h"
#include <hdf5.h>
#include <boost/container_hash/hash.hpp>
#include "write_buffer.h"
#include "distributed_sort.h"
#include "data_server_client.h"
#include "event_metadata.h"
#include "nvme_buffer.h"
#include <boost/lockfree/queue.hpp>
//#include "h5_async_lib.h"
#include <thread>
#include <fstream>

using namespace boost;

struct thread_arg_w
{
    int tid;
    int num_events;
    bool endsession;
    std::string name;
};


struct io_request
{
    std::string name;
    bool from_nvme;
    bool read_op;
    int tid;
    uint64_t mints;
    uint64_t maxts;
    std::vector <struct event>*buf1;
    std::vector <struct event>*buf2;
    std::vector <struct event>*buf3;
    std::atomic <int> completed;
    bool for_query;
    hsize_t total_records;
};

struct ts_offset
{
    uint64_t ts;
    uint64_t offset;
};

class read_write_process
{

private:
    ClockSynchronization <ClocksourceCPPStyle>*CM;
    int myrank;
    int numprocs;
    int numcores;
    boost::hash <uint64_t> hasher;
    uint64_t seed = 1;
    databuffers*dm;
    nvme_buffers*nm;
    boost::mutex m1;
    boost::mutex m2;
    std::set <std::string> file_names;
    std::unordered_map <std::string, std::pair <int, event_metadata>> write_names;
    std::vector <std::pair <std::atomic <uint64_t>, std::atomic <uint64_t>>>*file_interval;
    std::vector <std::pair <std::atomic <uint64_t>, std::atomic <uint64_t>>>*write_interval;
    std::vector <struct atomic_buffer*> myevents;
    dsort*ds;
    data_server_client*dsc;
    std::vector <struct thread_arg_w> t_args;
    std::vector <std::thread> workers;
    boost::lockfree::queue <struct io_request*>*io_queue_async;
    boost::lockfree::queue <struct io_request*>*io_queue_sync;
    std::atomic <int> end_of_session;
    std::atomic <int> num_streams;
    int num_io_threads;
    std::vector <struct thread_arg_w> t_args_io;
    std::vector <std::thread> io_threads;
    std::atomic <int> sync_clock;
    std::vector <int> num_dropped;
    std::vector <int> batch_size;
    tl::engine*thallium_server;
    tl::engine*thallium_shm_server;
    tl::engine*thallium_client;
    tl::engine*thallium_shm_client;
    std::vector <tl::endpoint> serveraddrs;
    std::vector <std::string> ipaddrs;
    std::vector <std::string> shmaddrs;
    std::string myipaddr;
    int iters_per_batch;
    std::atomic <int>*enable_stream;
    std::atomic <int> cstream;
    std::atomic <int>*w_reqs_pending;
    std::atomic <int>*r_reqs_pending;
    std::vector <int> loopticks;
    std::vector <int> numloops;
    std::atomic <int> session_ended;
    std::atomic <int> qe_ended;
    std::atomic <int>*end_of_stream_session;
public:
    read_write_process(int r, int np, ClockSynchronization <ClocksourceCPPStyle>*C, int n, data_server_client*rc)
            : myrank(r), numprocs(np), numcores(n), dsc(rc)
    {
        H5open();
        //H5VLis_connector_registered_by_name("async");
        std::string unit = "microsecond";
        CM = C;
        sync_clock.store(0);
        iters_per_batch = 4;
        thallium_server = dsc->get_thallium_server();
        thallium_shm_server = dsc->get_thallium_shm_server();
        thallium_client = dsc->get_thallium_client();
        thallium_shm_client = dsc->get_thallium_shm_client();
        serveraddrs = dsc->get_serveraddrs();
        ipaddrs = dsc->get_ipaddrs();
        shmaddrs = dsc->get_shm_addrs();
        myipaddr = ipaddrs[myrank];
        dm = new databuffers(numprocs, myrank, numcores, CM);
        dm->server_client_addrs(thallium_server, thallium_client, thallium_shm_server, thallium_shm_client, ipaddrs
                                , shmaddrs, serveraddrs);
        CM->server_client_addrs(thallium_server, thallium_client, thallium_shm_server, thallium_shm_client, ipaddrs
                                , shmaddrs, serveraddrs);
        CM->bind_functions();
        ds = new dsort(numprocs, myrank);
        nm = new nvme_buffers(numprocs, myrank);
        io_queue_async = new boost::lockfree::queue <struct io_request*>(128);
        io_queue_sync = new boost::lockfree::queue <struct io_request*>(128);
        end_of_session.store(0);
        num_streams.store(0);
        qe_ended.store(0);
        session_ended.store(0);
        cstream.store(0);
        num_io_threads = 1;
        file_interval = new std::vector <std::pair <std::atomic <uint64_t>, std::atomic <uint64_t>>>(MAXSTREAMS);
        write_interval = new std::vector <std::pair <std::atomic <uint64_t>, std::atomic <uint64_t>>>(MAXSTREAMS);
        enable_stream = (std::atomic <int>*)std::malloc(MAXSTREAMS * sizeof(std::atomic <int>));
        w_reqs_pending = (std::atomic <int>*)std::malloc(MAXSTREAMS * sizeof(std::atomic <int>));
        r_reqs_pending = (std::atomic <int>*)std::malloc(MAXSTREAMS * sizeof(std::atomic <int>));
        end_of_stream_session = (std::atomic <int>*)std::malloc(MAXSTREAMS * sizeof(std::atomic <int>));
        t_args.resize(MAXSTREAMS);
        workers.resize(MAXSTREAMS);
        numloops.resize(MAXSTREAMS);
        loopticks.resize(MAXSTREAMS);

        for(int i = 0; i < MAXSTREAMS; i++)
        {
            (*file_interval)[i].first.store(UINT64_MAX);
            (*file_interval)[i].second.store(0);
            (*write_interval)[i].first.store(UINT64_MAX);
            (*write_interval)[i].second.store(0);
            enable_stream[i].store(0);
            w_reqs_pending[i].store(0);
            r_reqs_pending[i].store(0);
            end_of_stream_session[i].store(0);
        }
        std::function <void(struct thread_arg_w*)> IOFunc(
                std::bind(&read_write_process::io_polling, this, std::placeholders::_1));

        t_args_io.resize(num_io_threads);
        io_threads.resize(num_io_threads);
        t_args_io[0].tid = 0;
        std::thread iothread0{IOFunc, &t_args_io[0]};
        io_threads[0] = std::move(iothread0);

    }

    ~read_write_process()
    {
        for(int i = 0; i < cstream.load(); i++) workers[i].join();
        for(int i = 0; i < num_io_threads; i++) io_threads[i].join();
        delete dm;
        delete ds;
        delete nm;
        delete io_queue_async;
        delete io_queue_sync;
        delete file_interval;
        delete write_interval;
        std::free(enable_stream);
        std::free(w_reqs_pending);
        std::free(r_reqs_pending);
        std::free(end_of_stream_session);
        H5close();

    }

    void bind_functions()
    {
        std::function <void(const tl::request &, int &, std::string &)> CreateEventBuffer(
                std::bind(&read_write_process::ThalliumCreateBuffer, this, std::placeholders::_1, std::placeholders::_2
                          , std::placeholders::_3));

        std::function <void(const tl::request &, std::string &, std::string &)> AddEventBuffer(
                std::bind(&read_write_process::ThalliumAddEvent, this, std::placeholders::_1, std::placeholders::_2
                          , std::placeholders::_3));
        std::function <void(const tl::request &, std::string &, uint64_t &)> GetEventProc(
                std::bind(&read_write_process::ThalliumGetEventProc, this, std::placeholders::_1, std::placeholders::_2
                          , std::placeholders::_3));

        std::function <void(const tl::request &, std::string &, uint64_t &)> GetEvent(
                std::bind(&read_write_process::ThalliumGetEvent, this, std::placeholders::_1, std::placeholders::_2
                          , std::placeholders::_3));

        std::function <void(const tl::request &, std::string &, uint64_t &)> GetNVMEProc(
                std::bind(&read_write_process::ThalliumGetNVMEProc, this, std::placeholders::_1, std::placeholders::_2
                          , std::placeholders::_3));

        std::function <void(const tl::request &, std::string &, uint64_t &)> GetNVMEEvent(
                std::bind(&read_write_process::ThalliumGetNVMEEvent, this, std::placeholders::_1, std::placeholders::_2
                          , std::placeholders::_3));
        std::function <void(const tl::request &, std::string &, uint64_t &)> FindEvent(
                std::bind(&read_write_process::ThalliumFindEvent, this, std::placeholders::_1, std::placeholders::_2
                          , std::placeholders::_3));

        std::function <void(const tl::request &, std::string &, uint64_t &)> FindEventFile(
                std::bind(&read_write_process::ThalliumFindEventFile, this, std::placeholders::_1, std::placeholders::_2
                          , std::placeholders::_3));

        std::function <void(const tl::request &, std::string &)> CheckFile(
                std::bind(&read_write_process::ThalliumCheckFile, this, std::placeholders::_1, std::placeholders::_2));

        std::function <void(const tl::request &, std::string &, std::vector <std::string> &, int &, int &)> AddService(
                std::bind(&read_write_process::ThalliumPrepareStream, this, std::placeholders::_1, std::placeholders::_2
                          , std::placeholders::_3, std::placeholders::_4, std::placeholders::_5));

        thallium_server->define("EmulatorPrepareStream", AddService);
        thallium_shm_server->define("EmulatorPrepareStream", AddService);
        thallium_server->define("EmulatorCheckFile", CheckFile);
        thallium_shm_server->define("EmulatorCheckFile", CheckFile);
        thallium_server->define("EmulatorFindEvent", FindEvent);
        thallium_shm_server->define("EmulatorFindEvent", FindEvent);
        thallium_server->define("EmulatorFindEventFile", FindEventFile);
        thallium_shm_server->define("EmulatorFindEventFile", FindEventFile);
        thallium_server->define("EmulatorGetNVMEEvent", GetNVMEEvent);
        thallium_shm_server->define("EmulatorGetNVMEEvent", GetNVMEEvent);
        thallium_server->define("EmulatorGetNVMEProc", GetNVMEProc);
        thallium_shm_server->define("EmulatorGetNVMEProc", GetNVMEProc);
        thallium_server->define("EmulatorGetEvent", GetEvent);
        thallium_shm_server->define("EmulatorGetEvent", GetEvent);
        thallium_server->define("EmulatorGetEventProc", GetEventProc);
        thallium_shm_server->define("EmulatorGetEventProc", GetEventProc);
        thallium_server->define("EmulatorCreateBuffer", CreateEventBuffer);
        thallium_shm_server->define("EmulatorCreateBuffer", CreateEventBuffer);
        thallium_server->define("EmulatorAddEvent", AddEventBuffer);
        thallium_shm_server->define("EmulatorAddEvent", AddEventBuffer);
    }

    inline int nearest_power_two(int n)
    {
        int c = 1;

        while(c < n)
        {
            c = 2 * c;
        }
        return c;
    }

    void end_session_flag()
    {
        end_of_session.store(1);

    }

    bool is_session_ended()
    {

        if(session_ended.load() == 1) return true;
        else return false;
    }

    void end_qe()
    {
        qe_ended.store(1);
    }

    void sync_queue_push(struct io_request*r)
    {
        io_queue_sync->push(r);
    }

    void spawn_write_streams(std::vector <std::string> &, std::vector <int> &, int);

    void spawn_write_stream(int, std::string &);

    dsort*get_sorter()
    {
        return ds;
    }

    void create_write_buffer(std::string &s, event_metadata &em, int maxsize)
    {
        m1.lock();
        if(write_names.find(s) == write_names.end())
        {
            struct atomic_buffer*ev = nullptr;
            ev = dm->create_write_buffer(maxsize, em.get_datasize());
            myevents.push_back(ev);
            int n = myevents.size() - 1;
            //numrecvevents[n].store(0);
            std::pair <std::string, std::pair <int, event_metadata>> p2;
            p2.first.assign(s);
            p2.second.first = myevents.size() - 1;
            p2.second.second = em;
            write_names.insert(p2);
            ds->create_sort_buffer();
            try
            {
                nm->create_nvme_buffer(s, em);
            }
            catch(const std::exception &except)
            {
                std::cout << except.what() << std::endl;
                exit(-1);
            }
            num_dropped.push_back(0);
            batch_size.push_back(maxsize);
        }
        m1.unlock();
    }

    void get_events_from_map(std::string &s)
    {
        int index = -1;
        m1.lock();
        auto r = write_names.find(s);
        if(r != write_names.end())
        {
            index = (r->second).first;
        }
        m1.unlock();
        if(index != -1)
            myevents[index] = dm->get_atomic_buffer(index);
    }

    atomic_buffer*get_write_buffer(std::string &s)
    {
        m1.lock();
        int index = -1;
        auto r = write_names.find(s);
        if(r != write_names.end()) index = (r->second).first;
        m1.unlock();

        if(index == -1) return nullptr;
        else return dm->get_atomic_buffer(index);
    }

    void sort_events(std::string &);

    int get_event_proc(int index, uint64_t &ts)
    {
        int pid = dm->GetValue(ts, index);
        return pid;
    }

    int get_event_proc(std::string &s, uint64_t &ts)
    {
        int index = -1;
        m1.lock();
        auto r = write_names.find(s);
        if(r != write_names.end())
            index = (r->second).first;
        m1.unlock();
        if(index == -1) return index;
        int pid = dm->GetValue(ts, index);
        return pid;
    }

    int get_nvme_proc(std::string &s, uint64_t ts)
    {
        int index_r = nm->get_proc(s, ts);
        return index_r;
    }

    bool find_nvme_event(std::string &s, uint64_t ts, struct event*e)
    {
        return nm->find_event(s, ts, e);
    }

    std::string find_nvme_event(std::string &s, uint64_t &ts)
    {
        int index = -1;
        event_metadata em;
        m1.lock();
        auto r = write_names.find(s);
        if(r != write_names.end())
        {
            index = (r->second).first;
            em = (r->second).second;
        }
        m1.unlock();
        std::string eventstring;

        if(index == -1) return eventstring;

        struct event e;
        char data[em.get_datasize()];
        e.data = data;

        bool b = find_nvme_event(s, ts, &e);

        eventstring = pack_event(&e, em.get_datasize());
        return eventstring;
    }

    bool find_event(int index, uint64_t ts, struct event*e, int length)
    {

        boost::shared_lock <boost::shared_mutex> lk(myevents[index]->m);

        e->ts = UINT64_MAX;

        for(int i = 0; i < myevents[index]->buffer_size.load(); i++)
        {
            if((*myevents[index]->valid)[i].load() == 1 && (*myevents[index]->buffer)[i].ts == ts)
            {
                e->ts = (*myevents[index]->buffer)[i].ts;
                std::memcpy(e->data, &(*myevents[index]->buffer)[i].data, length);
                return true;
                break;
            }
        }
        return false;
    }

    std::string find_event(std::string &s, uint64_t &ts)
    {
        int index = -1;
        event_metadata em;
        m1.lock();
        auto r = write_names.find(s);
        if(r != write_names.end())
        {
            index = (r->second).first;
            em = (r->second).second;
        }
        m1.unlock();

        std::string eventstring;
        if(index == -1) return eventstring;

        struct event e;
        char data[em.get_datasize()];
        e.ts = UINT64_MAX;
        e.data = data;
        bool b = find_event(index, ts, &e, em.get_datasize());
        if(b)
        {
            eventstring = pack_event(&e, em.get_datasize());
        }
        return eventstring;
    }

    event_metadata get_metadata(std::string &s)
    {
        event_metadata em;
        m1.lock();
        auto r = write_names.find(s);
        if(r != write_names.end())
        {
            em = (r->second).second;
        }
        m1.unlock();
        return em;
    }

    bool add_metadata_buffers(std::string &s, std::vector <std::string> &m, int &nloops, int &nticks)
    {
        if(cstream.load() == MAXSTREAMS) return false;

        event_metadata em;
        int i = 0;
        std::string tname = m[0];
        i++;
        std::size_t num;
        int numattrs = std::stoi(m[i].c_str(), &num);
        i++;
        em.set_numattrs(numattrs);
        std::vector <std::string> attrtypes;
        std::vector <std::string> attrnames;
        std::vector <int> attrsizes;
        for(int j = 0; j < numattrs; j++)
        {
            std::string st = m[i];
            attrtypes.push_back(st);
            i++;
        }
        for(int j = 0; j < numattrs; j++)
        {
            std::string st = m[i];
            attrnames.push_back(st);
            i++;
        }
        for(int j = 0; j < numattrs; j++)
        {
            std::string st = m[i];
            int size = std::stoi(st.c_str(), &num);
            attrsizes.push_back(size);
            i++;
        }
        int datalen = std::stoi(m[i].c_str(), &num);
        for(int j = 0; j < numattrs; j++)
        {
            bool is_signed = false;
            bool is_big_endian = true;
            em.add_attr(attrnames[j], attrsizes[j], is_signed, is_big_endian);
        }
        em.set_invlist(false);
        create_write_buffer(s, em, 8192);
        int streamid = cstream.fetch_add(1);
        numloops[streamid] = nloops;
        loopticks[streamid] = (nticks < 50) ? 50 : nticks;
        loopticks[streamid] = (loopticks[streamid] > 500) ? 500 : loopticks[streamid];
        enable_stream[streamid].store(1);
        spawn_write_stream(streamid, s);
        return true;
    }

    int get_stream_index(std::string &s)
    {
        int index = -1;
        m1.lock();
        auto r = write_names.find(s);
        if(r != write_names.end())
            index = (r->second).first;
        m1.unlock();
        return index;
    }

    void get_file_minmax(std::string &s, uint64_t &min_v, uint64_t &max_v)
    {
        min_v = UINT64_MAX;
        max_v = 0;

        int index = -1;
        m1.lock();
        auto r = std::find(file_names.begin(), file_names.end(), s);
        if(r != file_names.end()) index = std::distance(file_names.begin(), r);
        m1.unlock();

        if(index != -1)
        {
            min_v = (*file_interval)[index].first.load();
            max_v = (*file_interval)[index].second.load();
        }

    }

    bool file_exists(std::string &s)
    {
        bool found = false;
        m1.lock();
        auto r = std::find(file_names.begin(), file_names.end(), s);
        if(r != file_names.end()) found = true;
        m1.unlock();
        return found;
    }

    bool get_range_in_write_buffers(std::string &s, uint64_t &min_v, uint64_t &max_v)
    {
        min_v = UINT64_MAX;
        max_v = 0;
        bool err = false;
        int index = -1;
        m1.lock();
        auto r = write_names.find(s);
        if(r != write_names.end()) index = r->second.first;
        m1.unlock();

        if(index != -1)
        {
            uint64_t minv = (*write_interval)[index].first.load();
            uint64_t maxv = (*write_interval)[index].second.load();

            min_v = (min_v < minv) ? min_v : minv;
            max_v = (max_v > maxv) ? max_v : maxv;
            err = true;
        }
        return err;
    }

    int num_write_events(std::string &s)
    {
        int index = -1;
        m1.lock();
        auto r = write_names.find(s);
        if(r != write_names.end())
        {
            index = (r->second).first;
        }
        m1.unlock();
        if(index != -1)
        {
            return myevents[index]->buffer_size.load();
        }
        return 0;
    }

    int dropped_events()
    {
        return dm->num_dropped_events();
    }

    void ThalliumCheckFile(const tl::request &req, std::string &s)
    {
        req.respond(file_exists(s));
    }

    void ThalliumCreateBuffer(const tl::request &req, int &num_events, std::string &s)
    {
        req.respond(create_buffer(num_events, s));
    }

    void ThalliumPrepareStream(const tl::request &req, std::string &s, std::vector <std::string> &m, int &n1, int &n2)
    {
        req.respond(add_metadata_buffers(s, m, n1, n2));

    }

    void ThalliumAddEvent(const tl::request &req, std::string &s, std::string &data)
    {
        req.respond(add_event(s, data));
    }

    void ThalliumGetEventProc(const tl::request &req, std::string &s, uint64_t &ts)
    {
        req.respond(get_event_proc(s, ts));
    }

    void ThalliumGetEvent(const tl::request &req, std::string &s, uint64_t &ts)
    {
        req.respond(find_event(s, ts));
    }

    void ThalliumGetNVMEProc(const tl::request &req, std::string &s, uint64_t &ts)
    {
        req.respond(get_nvme_proc(s, ts));
    }

    void ThalliumGetNVMEEvent(const tl::request &req, std::string &s, uint64_t &ts)
    {
        req.respond(find_nvme_event(s, ts));
    }

    void ThalliumFindEvent(const tl::request &req, std::string &s, uint64_t &ts)
    {
        req.respond(FindEvent(s, ts));
    }

    void ThalliumFindEventFile(const tl::request &req, std::string &s, uint64_t &ts)
    {
        req.respond(FindEventFile(s, ts));
    }


    std::string FindEventFile(std::string &, uint64_t &);

    std::string FindEvent(std::string &, uint64_t &);

    std::string GetEvent(std::string &, uint64_t &, int);

    std::string GetNVMEEvent(std::string &, uint64_t &, int);

    bool get_events_in_range_from_read_buffers(std::string &s, std::pair <uint64_t, uint64_t> &range
                                               , std::vector <struct event> &oup);

    void create_events(int num_events, std::string &s, double);

    void clear_write_events(int, uint64_t &, uint64_t &);

    void get_range(std::string &s);

    void pwrite_extend_files(std::vector <std::string> &, std::vector <hsize_t> &, std::vector <hsize_t> &
                             , std::vector <std::pair <std::vector <struct event>*, std::vector <char>*>> &
                             , std::vector <uint64_t> &, std::vector <uint64_t> &, bool, std::vector <int> &
                             , std::vector <std::vector <std::vector <int>>> &);

    void pwrite(std::vector <std::string> &, std::vector <hsize_t> &, std::vector <hsize_t> &
                , std::vector <std::pair <std::vector <struct event>*, std::vector <char>*>> &, std::vector <uint64_t> &
                , std::vector <uint64_t> &, bool, std::vector <int> &, std::vector <std::vector <std::vector <int>>> &);

    void pwrite_files(std::vector <std::string> &, std::vector <hsize_t> &, std::vector <hsize_t> &
                      , std::vector <std::pair <std::vector <struct event>*, std::vector <char>*>> &
                      , std::vector <uint64_t> &, std::vector <uint64_t> &, bool, std::vector <int> &
                      , std::vector <std::vector <std::vector <int>>> &);

    bool pread(std::vector <std::vector <io_request*>> &, int);

    std::pair <std::vector <struct event>*, std::vector <char>*>
    create_data_spaces(std::string &, hsize_t &, hsize_t &, uint64_t &, uint64_t &, bool, int &
                       , std::vector <std::vector <int>> &);

    void io_polling(struct thread_arg_w*);

    void data_stream(struct thread_arg_w*);

    bool create_buffer(int &, std::string &);

    std::vector <uint64_t> add_event(std::string &, std::string &);

    int endsessioncount(int);

    void create_inverted_list(std::string &, std::vector <std::vector <int>> &
                              , std::pair <std::vector <struct event>*, std::vector <char>*> &, hid_t &, hid_t &
                              , hid_t &);

    void merge_inverted_list(std::string &, std::vector <std::vector <int>> &
                             , std::pair <std::vector <struct event>*, std::vector <char>*> &, hid_t &, hid_t &);
};

#endif
