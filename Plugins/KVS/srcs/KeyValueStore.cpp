#include "invertedlist.h"
#include "KeyValueStore.h"


void KeyValueStore::createKeyValueStoreEntry(std::string &s, KeyValueStoreMetadata &m)
{
    std::vector <std::string> pk;
    m.packmetadata(pk);
    if(!mds->Find(s))
    {
        bool b = mds->Insert(s, pk);
    }
    tables->add_accessor(s, m);
}

void
KeyValueStore::get_keyvaluestorestructs(std::string &s, std::string &attr_name, KeyValueStoreAccessor*&ka, int &pos)
{

    ka = nullptr;
    ka = tables->get_accessor(s);
    pos = ka->get_inverted_list_index(attr_name);
}

bool KeyValueStore::findKeyValueStoreEntry(std::string &s, KeyValueStoreMetadata &m)
{
    bool ret = false;
    std::vector <std::string> it;
    it = mds->Get(s);
    if(it.size() > 0)
    {
        m.unpackmetadata(it);
    }
    return ret;
}

void KeyValueStore::get_testworkload(std::string &s, std::vector <int> &keys, std::vector <uint64_t> &ts, int offset)
{

    create_integertestinput(s, numprocs, myrank, offset, keys, ts);
}

void KeyValueStore::get_ycsb_test(std::string &s, std::vector <uint64_t> &keys, std::vector <std::string> &values
                                  , std::vector <int> &op)
{
    create_ycsb_input(s, numprocs, myrank, keys, values, op);
}


void
KeyValueStore::get_ycsb_timeseries_workload(std::string &s, std::vector <uint64_t> &keys, std::vector <float> &values
                                            , std::vector <int> &op)
{

    create_timeseries_testinput(s, numprocs, myrank, keys, values, op);

}

void KeyValueStore::get_dataworld_workload(std::string &s, std::vector <uint64_t> &keys, std::vector <uint64_t> &ts
                                           , std::vector <int> &op)
{

    create_dataworld_testinput(s, numprocs, myrank, keys, ts, op);
}

void KeyValueStore::addKeyValueStoreInvList(std::string &s, std::string &attr_name, int maxsize)
{
    if(!tables->find_accessor(s))
    {
        KeyValueStoreMetadata m;
        if(!findKeyValueStoreEntry(s, m)) return;

        if(!tables->add_accessor(s, m)) return;
    }

    tables->create_invertedlist(s, attr_name, io_count, maxsize);
    io_count++;
}


bool KeyValueStore::findKeyValueStoreInvList(std::string &s, std::string &attr_name)
{


}

void KeyValueStore::removeKeyValueStoreInvList(std::string &s, std::string &attr_name)
{


}
