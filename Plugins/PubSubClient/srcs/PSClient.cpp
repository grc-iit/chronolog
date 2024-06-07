#include "PSClient.h"

void pubsubclient::create_pub_sub_service(std::string &s, std::vector <int> &p, std::vector <int> &sb, int num_messages
                                          , int msg_size)
{
    m.lock();
    auto r = table_roles.find(s);
    if(r == table_roles.end())
    {
        assert(subscribers.size() == publishers.size());
        int numtables = subscribers.size();

        std::pair <std::string, int> pn(s, numtables);

        subscribers.push_back(sb);
        publishers.push_back(p);

        table_roles.insert(pn);

        int role = -1;
        if(std::find(sb.begin(), sb.end(), myrank) != sb.end())
            role = 0;
        if(std::find(p.begin(), p.end(), myrank) != p.end())
            role = (role == -1) ? 1 : 2;
        std::pair <std::string, int> np(s, role);
        client_role.insert(np);
        if(myrank == 0) std::cout << " client role = " << role << std::endl;
    }
    else
    {
        int n = r->second;

        subscribers[n].assign(sb.begin(), sb.end());
        publishers[n].assign(p.begin(), p.end());
        int role = -1;
        if(std::find(sb.begin(), sb.end(), myrank) != sb.end())
            role = 0;
        if(std::find(p.begin(), p.end(), myrank) != p.end())
            role = (role == -1) ? 1 : 2;

        auto r1 = client_role.find(s);
        r1->second = role;
    }

    m.unlock();

    add_message_cache(s, num_messages, msg_size);

    barrier();
}

void pubsubclient::add_subs(std::string &s, std::vector <int> &sb)
{
    m.lock();
    auto r = table_roles.find(s);
    if(r != table_roles.end())
    {
        int n = r->second;

        for(int i = 0; i < sb.size(); i++)
        {
            if(std::find(subscribers[n].begin(), subscribers[n].end(), sb[i]) == subscribers[n].end())
                subscribers[n].push_back(sb[i]);
        }

        if(std::find(sb.begin(), sb.end(), myrank) != sb.end())
        {
            auto r1 = client_role.find(s);

            int role = r1->second;
            if(role == 1) role = 2;
            r1->second = role;
        }
    }
    m.unlock();
}

void pubsubclient::add_pubs(std::string &s, std::vector <int> &p)
{
    m.lock();
    auto r = table_roles.find(s);

    if(r != table_roles.end())
    {
        int n = r->second;

        for(int i = 0; i < p.size(); i++)
        {
            if(std::find(publishers[n].begin(), publishers[n].end(), p[i]) == publishers[n].end())
                publishers[n].push_back(p[i]);
        }

        if(std::find(p.begin(), p.end(), myrank) != p.end())
        {
            auto r1 = client_role.find(s);

            int role = r1->second;
            if(role == 0) role = 2;
            r1->second = role;
        }
    }
    m.unlock();
}

void pubsubclient::remove_pubs(std::string &s, std::vector <int> &p)
{
    m.lock();
    auto r = table_roles.find(s);

    if(r != table_roles.end())
    {
        int n = r->second;

        std::vector <int> temp;
        for(int i = 0; i < publishers[n].size(); i++)
        {
            if(std::find(p.begin(), p.end(), publishers[n][i]) == p.end())
                temp.push_back(publishers[n][i]);
        }

        publishers[n].assign(temp.begin(), temp.end());
    }

    if(std::find(p.begin(), p.end(), myrank) != p.end())
    {
        auto r1 = client_role.find(s);

        int role = r1->second;
        if(role == 2) role = 0;
        else if(role == 1) role = -1;
    }
    m.unlock();
}

void pubsubclient::remove_subs(std::string &s, std::vector <int> &sb)
{
    m.lock();
    auto r = table_roles.find(s);
    if(r != table_roles.end())
    {
        int n = r->second;
        std::vector <int> temp;
        for(int i = 0; i < subscribers[n].size(); i++)
        {
            if(std::find(sb.begin(), sb.end(), subscribers[n][i]) == sb.end())
                temp.push_back(subscribers[n][i]);
        }

        subscribers[n].assign(temp.begin(), temp.end());

        if(std::find(sb.begin(), sb.end(), myrank) != sb.end())
        {
            auto r1 = client_role.find(s);
            int role = r1->second;
            if(role == 2) role = 1;
            else if(role == 0) role = -1;
            r1->second = role;
        }
    }
    m.unlock();
}

void pubsubclient::add_message_cache(std::string &s, int nmessages, int msg_size)
{
    m.lock();
    auto r = table_roles.find(s);

    if(r != table_roles.end())
    {
        auto r1 = mcnum.find(s);

        if(r1 == mcnum.end())
        {
            message_cache*m = new message_cache(numprocs, myrank, s, ds, msg_size, nmessages);
            mcs.push_back(m);
            std::pair <std::string, int> p(s, mcs.size() - 1);
            mcnum.insert(p);
            if(myrank == 0)
                std::cout << " cache : message size = " << msg_size << " num msgs = " << nmessages << std::endl;
        }
    }
    m.unlock();
}

bool pubsubclient::broadcast_message(std::string &s, std::string &msg)
{
    auto r = mcnum.find(s);

    if(r == mcnum.end()) return false;
    int id = r->second;
    message_cache*c = mcs[id];

    return c->add_message(msg);
}

bool pubsubclient::publish_message(std::string &s, std::string &msg)
{

    auto r = client_role.find(s);
    auto r1 = table_roles.find(s);
    if(r == client_role.end() || r1 == table_roles.end()) return false;

    int role = r->second;

    if(role == 0) return false;

    int id = r1->second;

    bool b = false;

    for(int i = 0; i < subscribers[id].size(); i++)
    {

        int destid = subscribers[id][i];
        if(ipaddrs[destid].compare(myipaddr) == 0)
        {
            tl::endpoint ep = thallium_shm_client->lookup(shmaddrs[destid]);
            std::string fcnname = "BroadcastMessage";
            tl::remote_procedure rp = thallium_shm_client->define(fcnname.c_str());
            b = rp.on(ep)(s, msg);
        }
        else
        {
            std::string fcnname = "BroadcastMessage";
            tl::remote_procedure rp = thallium_client->define(fcnname.c_str());
            b = rp.on(serveraddrs[destid])(s, msg);
        }
    }
    return true;
}

void pubsubclient::barrier()
{
    MPI_Request*reqs = new MPI_Request[2 * numprocs];
    int nreq = 0;
    int sendv = 1;
    std::vector <int> recvv(numprocs);
    std::fill(recvv.begin(), recvv.end(), 0);

    for(int i = 0; i < numprocs; i++)
    {
        MPI_Isend(&sendv, 1, MPI_INT, i, tag, MPI_COMM_WORLD, &reqs[nreq]);
        nreq++;
        MPI_Irecv(&recvv[i], 1, MPI_INT, i, tag, MPI_COMM_WORLD, &reqs[nreq]);
        nreq++;
    }

    MPI_Waitall(nreq, reqs, MPI_STATUS_IGNORE);


    delete reqs;
}

void pubsubclient::CreatePubSubWorkload(std::string &s, std::string &attr_name, KeyValueStoreMetadata &m, int numrecords
                                        , int rate, int pub_rate, bool pub)
{
    std::vector <int> pubs, subs;

    for(int i = 0; i < numprocs; i++)
    {
        if(i % 2 == 0) pubs.push_back(i);
        else subs.push_back(i);
    }

    int len = m.value_size();
    int recordlen = sizeof(uint64_t) + len;
    create_pub_sub_service(s, pubs, subs, 100, recordlen);
    bind_functions();

    KeyValueStore*k = getkvs();

    int s1 = k->start_session(s, attr_name, m, 32768);

    KeyValueStoreAccessor*ka = nullptr;
    int id = 0;
    k->get_keyvaluestorestructs(s, attr_name, ka, id);

    std::string t = m.get_type(attr_name);

    if(t.compare("int") == 0)
        PutRecords <integer_invlist, int>(ka, id, s, len, numrecords, rate, pub_rate, pub);
    else if(t.compare("unsignedlong") == 0)
        PutRecords <unsigned_long_invlist, uint64_t>(ka, id, s, len, numrecords, rate, pub_rate, pub);
    else if(t.compare("float") == 0)
        PutRecords <float_invlist, float>(ka, id, s, len, numrecords, rate, pub_rate, pub);
    else if(t.compare("double") == 0)
        PutRecords <double_invlist, double>(ka, id, s, len, numrecords, rate, pub_rate, pub);
}
