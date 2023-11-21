#include "write_buffer.h"
#include <boost/container_hash/hash.hpp>


atomic_buffer*databuffers::create_write_buffer(int maxsize, int datasize)
{
    int total_size = 65536 * 2;
    uint64_t maxkey = UINT64_MAX;
    dmap->create_table(total_size, maxkey);
    struct atomic_buffer*a = new struct atomic_buffer();
    a->buffer_size.store(0);
    try
    {
        a->buffer = new std::vector <struct event>(maxsize);
        a->datamem = new std::vector <char>(maxsize * datasize);
        a->valid = new std::vector <std::atomic <int>>(maxsize);
    }
    catch(const std::exception &except)
    {
        std::cout << except.what() << std::endl;
        exit(-1);
    }
    m1.lock();
    atomicbuffers.push_back(a);
    m1.unlock();
    return a;
}

void databuffers::clear_write_buffer(int index)
{
    boost::upgrade_lock <boost::shared_mutex> lk(atomicbuffers[index]->m);
    dmap->LocalClearMap(index);
    atomicbuffers[index]->buffer_size.store(0);
    atomicbuffers[index]->buffer->clear();
    atomicbuffers[index]->datamem->clear();
    for(int i = 0; i < atomicbuffers[index]->valid->size(); i++)
        (*atomicbuffers[index]->valid)[i].store(0);
}

void databuffers::clear_write_buffer_no_lock(int index)
{
    dmap->LocalClearMap(index);
    atomicbuffers[index]->buffer_size.store(0);
    atomicbuffers[index]->buffer->clear();
    atomicbuffers[index]->datamem->clear();
    for(int i = 0; i < atomicbuffers[index]->valid->size(); i++)
        (*atomicbuffers[index]->valid)[i].store(0);
}

void databuffers::set_valid_range(int index, uint64_t &n1, uint64_t &n2)
{
    dmap->set_valid_range(index, n1, n2);
}

int databuffers::add_event(int index, uint64_t ts, std::string &data, event_metadata &em)
{

    int datasize = em.get_datasize();
    int v = myrank;
    int b = dmap->Insert(ts, v, index);

    if(b == 1)
    {
        bool d = false;
        int prev = 0, next = 0;
        int maxsize = atomicbuffers[index]->buffer->size();

        do
        {
            d = false;
            prev = atomicbuffers[index]->buffer_size.load();
            if(prev == maxsize) break;
            next = prev + 1;
        } while(!(d = atomicbuffers[index]->buffer_size.compare_exchange_strong(prev, next)));

        if(d)
        {
            int ps = prev; //atomicbuffers[index]->buffer_size.fetch_add(1);
            (*atomicbuffers[index]->buffer)[ps].ts = ts;
            char*dest = &((*atomicbuffers[index]->datamem)[ps * datasize]);
            (*atomicbuffers[index]->buffer)[ps].data = dest;
            std::memcpy(dest, data.c_str(), datasize);
            (*atomicbuffers[index]->valid)[ps].store(1);
            return 1;
        }
        else
        {
            return 2;
        }
        //else b = false;
    }
    return b;


}

bool databuffers::add_event(event &e, int index, event_metadata &em)
{
    uint64_t key = e.ts;
    int v = myrank;

    auto t1 = std::chrono::high_resolution_clock::now();

    int b = dmap->Insert(key, v, index);

    auto t2 = std::chrono::high_resolution_clock::now();
    double t = std::chrono::duration <double>(t2 - t1).count();
    if(max_t < t) max_t = t;
    int datasize = em.get_datasize();

    if(b == 1)
    {
        int bc = 0;
        int numuints = std::ceil(datasize / sizeof(uint64_t));
        char data[datasize];
        for(int j = 0; j < numuints; j++)
        {
            std::size_t seed = 0;
            uint64_t key1 = key + j;
            boost::hash_combine(seed, key1);
            uint64_t mask = 127;
            bool end = false;
            for(int k = 0; k < sizeof(uint64_t); k++)
            {
                uint64_t v = mask&seed;
                char c = (char)v;
                seed = seed >> 8;
                data[bc] = c;
                bc++;
                if(bc == datasize)
                {
                    end = true;
                    break;
                }
            }
            if(end) break;
        }

        bool d = false;
        int prev = 0, next = 0;
        int maxsize = atomicbuffers[index]->buffer->size();

        do
        {
            d = false;
            prev = atomicbuffers[index]->buffer_size.load();
            if(prev == maxsize) break;
            next = prev + 1;
        } while(!(d = atomicbuffers[index]->buffer_size.compare_exchange_strong(prev, next)));

        if(d)
        {
            int ps = prev;
            (*atomicbuffers[index]->buffer)[ps].ts = e.ts;
            char*dest = &((*atomicbuffers[index]->datamem)[ps * datasize]);
            (*atomicbuffers[index]->buffer)[ps].data = dest;
            std::memcpy(dest, data, datasize);
            (*atomicbuffers[index]->valid)[ps].store(1);
            event_count++;
            return true;
        }
        else return false;
    }
    return false;
}

std::vector <struct event>*databuffers::get_write_buffer(int index)
{
    return atomicbuffers[index]->buffer;
}

atomic_buffer*databuffers::get_atomic_buffer(int index)
{
    return atomicbuffers[index];
}



