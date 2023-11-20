#include "KeyValueStoreIO.h"
#include "KeyValueStoreAccessor.h"
#include "invertedlist.h"


bool req_compare(struct sync_request*r1, struct sync_request*r2)
{
    std::string s1 = r1->name + r1->attr_name;
    std::string s2 = r2->name + r2->attr_name;

    if(s1.compare(s2) <= 0) return true;
    else return false;
}

void KeyValueStoreIO::io_function(struct thread_arg*t)
{

    std::vector <int> op_type;
    op_type.resize(3);

    std::vector <int> consensus;

    consensus.resize(nservers * 3);

    bool end_io = false;

    while(true)
    {

        std::fill(op_type.begin(), op_type.end(), 0);
        std::fill(consensus.begin(), consensus.end(), 0);

        op_type[0] = 0;

        op_type[1] = (sync_queue->empty() == true) ? 0 : 1;

        op_type[2] = service_queries.size();

        MPI_Allgather(op_type.data(), 3, MPI_INT, consensus.data(), 3, MPI_INT, MPI_COMM_WORLD);

        int numqueries = INT_MAX;

        for(int i = 0; i < nservers; i++)
            if(consensus[3 * i + 2] < numqueries) numqueries = consensus[3 * i + 2];

        int nprocs_sync = 0;
        for(int i = 0; i < nservers; i++)
            nprocs_sync += consensus[3 * i + 1];

        for(int i = 0; i < numqueries; i++)
        {
            if(service_queries[i].first == 0)
            {
                integer_invlist*invlist = reinterpret_cast<integer_invlist*>(service_queries[i].second);
                invlist->get_events();
            }
            else if(service_queries[i].first == 1)
            {
                unsigned_long_invlist*invlist = reinterpret_cast<unsigned_long_invlist*>(service_queries[i].second);
                invlist->get_events();
            }
            else if(service_queries[i].first == 2)
            {
                float_invlist*invlist = reinterpret_cast<float_invlist*>(service_queries[i].second);
                invlist->get_events();
            }
            else if(service_queries[i].first == 3)
            {
                double_invlist*invlist = reinterpret_cast<double_invlist*>(service_queries[i].second);
                invlist->get_events();

            }
        }

        if(end_io && nprocs_sync == 0) break;

        if(nprocs_sync == nservers)
        {

            std::vector <struct sync_request*> sync_reqs;
            while(!sync_queue->empty())
            {
                struct sync_request*r = nullptr;
                if(sync_queue->pop(r))
                {
                    sync_reqs.push_back(r);
                }
            }

            std::vector <struct sync_request*> common_reqs;
            get_common_requests(sync_reqs, common_reqs);

            std::vector <std::string> completed_reqs;

            for(int i = 0; i < common_reqs.size(); i++)
            {
                std::string newname = common_reqs[i]->name + common_reqs[i]->attr_name;
                if(std::find(completed_reqs.begin(), completed_reqs.end(), newname) != completed_reqs.end()) continue;
                if(common_reqs[i]->keytype == 0)
                {
                    if(common_reqs[i]->flush == true)
                    {
                        integer_invlist*invlist = reinterpret_cast<integer_invlist*>(common_reqs[i]->funcptr);
                        //invlist->flush_table_file(common_reqs[i]->offset,common_reqs[i]->persist);
                        invlist->flush_timestamps(common_reqs[i]->offset, common_reqs[i]->persist);
                        std::string name = common_reqs[i]->name + common_reqs[i]->attr_name;
                        completed_reqs.push_back(name);
                    }
                }
                else if(common_reqs[i]->keytype == 1)
                {
                    if(common_reqs[i]->flush == true)
                    {
                        unsigned_long_invlist*invlist = reinterpret_cast<unsigned_long_invlist*>(common_reqs[i]->funcptr);
                        //invlist->flush_table_file(common_reqs[i]->offset,common_reqs[i]->persist);
                        invlist->flush_timestamps(common_reqs[i]->offset, common_reqs[i]->persist);
                        std::string name = common_reqs[i]->name + common_reqs[i]->attr_name;
                        completed_reqs.push_back(name);
                    }
                }
                else if(common_reqs[i]->keytype == 2)
                {
                    if(common_reqs[i]->flush == true)
                    {
                        float_invlist*invlist = reinterpret_cast<float_invlist*>(common_reqs[i]->funcptr);
                        //invlist->flush_table_file(common_reqs[i]->offset,common_reqs[i]->persist);
                        invlist->flush_timestamps(common_reqs[i]->offset, common_reqs[i]->persist);
                        std::string name = common_reqs[i]->name + common_reqs[i]->attr_name;
                        completed_reqs.push_back(name);
                    }
                }
                else if(common_reqs[i]->keytype == 3)
                {
                    if(common_reqs[i]->flush == true)
                    {
                        double_invlist*invlist = reinterpret_cast<double_invlist*>(common_reqs[i]->funcptr);
                        //invlist->flush_table_file(common_reqs[i]->offset,common_reqs[i]->persist);
                        invlist->flush_timestamps(common_reqs[i]->offset, common_reqs[i]->persist);
                        std::string name = common_reqs[i]->name + common_reqs[i]->attr_name;
                        completed_reqs.push_back(name);
                    }
                }
                else if(common_reqs[i]->funcptr == nullptr)
                {
                    end_io = true;
                }
            }

            for(int i = 0; i < common_reqs.size(); i++) delete common_reqs[i];
            for(int i = 0; i < sync_reqs.size(); i++) sync_queue->push(sync_reqs[i]);

        }

    }
}

void KeyValueStoreIO::get_common_requests(std::vector <struct sync_request*> &sync_reqs
                                          , std::vector <struct sync_request*> &common_reqs)
{

    int numreqs = sync_reqs.size();
    std::vector <int> send_count(nservers);
    std::vector <int> req_count(nservers);
    std::fill(send_count.begin(), send_count.end(), 0);
    std::fill(req_count.begin(), req_count.end(), 0);
    send_count[serverid] = numreqs;

    MPI_Allreduce(send_count.data(), req_count.data(), nservers, MPI_INT, MPI_SUM, MPI_COMM_WORLD);

    std::vector <int> send_lens(sync_reqs.size());

    for(int i = 0; i < sync_reqs.size(); i++)
    {
        send_lens[i] = sync_reqs[i]->name.length() + sync_reqs[i]->attr_name.length();
    }

    int total_req = 0;
    for(int i = 0; i < nservers; i++) total_req += req_count[i];

    std::vector <int> recv_lens;
    recv_lens.resize(total_req);

    int sendcount = sync_reqs.size();
    std::vector <int> displ(nservers);

    displ[0] = 0;
    for(int i = 1; i < nservers; i++) displ[i] = displ[i - 1] + req_count[i - 1];

    MPI_Allgatherv(send_lens.data(), sendcount, MPI_INT, recv_lens.data(), req_count.data(), displ.data(), MPI_INT
                   , MPI_COMM_WORLD);
    std::string send_name = sync_reqs[0]->name + sync_reqs[0]->attr_name;


    for(int i = 1; i < sync_reqs.size(); i++)
    {
        send_name += sync_reqs[i]->name + sync_reqs[i]->attr_name;
    }

    int total_length = 0;
    for(int i = 0; i < recv_lens.size(); i++)
        total_length += recv_lens[i];

    std::vector <char> recv_names(total_length);

    int l = send_name.length();

    std::vector <int> recv_counts(nservers);
    std::fill(recv_counts.begin(), recv_counts.end(), 0);

    for(int i = 0; i < nservers; i++)
    {
        for(int j = 0; j < req_count[i]; j++) recv_counts[i] += recv_lens[displ[i] + j];
    }

    std::vector <int> recv_displ(nservers);
    recv_displ[0] = 0;
    for(int i = 1; i < nservers; i++) recv_displ[i] = recv_displ[i - 1] + recv_counts[i - 1];


    MPI_Allgatherv(send_name.data(), l, MPI_CHAR, recv_names.data(), recv_counts.data(), recv_displ.data(), MPI_CHAR
                   , MPI_COMM_WORLD);
    std::vector <std::vector <std::string>> recv_reqs(nservers);

    for(int i = 0; i < nservers; i++)
    {
        for(int j = 0; j < req_count[i]; j++)
        {
            int len = recv_lens[displ[i] + j];
            std::string s;
            s.assign(recv_names.data() + recv_displ[i], recv_names.data() + recv_displ[i] + len);
            recv_displ[i] += len;
            recv_reqs[i].push_back(s);
        }
    }

    std::unordered_map <std::string, int> stringcount;

    for(int i = 0; i < nservers; i++)
    {
        for(int j = 0; j < recv_reqs[i].size(); j++)
        {
            std::string s = recv_reqs[i][j];
            auto r = stringcount.find(s);
            if(r == stringcount.end())
            {
                std::pair <std::string, int> p;
                p.first.assign(s);
                p.second = 1;
                stringcount.insert(p);
            }
            else r->second = r->second + 1;
        }
    }

    std::vector <struct sync_request*> pending_reqs;

    for(int i = 0; i < sync_reqs.size(); i++)
    {
        std::string s = sync_reqs[i]->name + sync_reqs[i]->attr_name;
        auto r = stringcount.find(s);
        if(r->second == nservers) common_reqs.push_back(sync_reqs[i]);
        else pending_reqs.push_back(sync_reqs[i]);
    }

    sync_reqs.clear();
    sync_reqs.assign(pending_reqs.begin(), pending_reqs.end());

    std::sort(common_reqs.begin(), common_reqs.end(), req_compare);

    if(serverid == 0)
        for(int i = 0; i < common_reqs.size(); i++)
            std::cout << " i = " << i << " req name = " << common_reqs[i]->name << " req attr name = "
                      << common_reqs[i]->attr_name << std::endl;

}

void KeyValueStoreIO::io_service()
{
    std::function <void(struct thread_arg*)> IOFunc(
    std::bind(&KeyValueStoreIO::io_function, this, std::placeholders::_1));

    std::thread t{IOFunc, &t_args[0]};
    io_threads[0] = std::move(t);
}

void KeyValueStoreIO::query_service_end()
{
    int reqc = 0;
    int sendv = 1;
    MPI_Request*reqs = new MPI_Request[2 * nservers];
    std::vector <int> recvv;
    recvv.resize(nservers);
    std::fill(recvv.begin(), recvv.end(), 0);

    for(int i = 0; i < nservers; i++)
    {
        MPI_Isend(&sendv, 1, MPI_INT, i, tag, MPI_COMM_WORLD, &reqs[reqc]);
        reqc++;
        MPI_Irecv(&recvv[i], 1, MPI_INT, i, tag, MPI_COMM_WORLD, &reqs[reqc]);
        reqc++;
    }

    MPI_Waitall(reqc, reqs, MPI_STATUS_IGNORE);

    for(int i = 0; i < service_queries.size(); i++)
    {
        if(service_queries[i].first == 0)
        {
            integer_invlist*invlist = reinterpret_cast<integer_invlist*>(service_queries[i].second);
            while(!invlist->EmptyPendingQueue());
        }
        else if(service_queries[i].first == 1)
        {
            unsigned_long_invlist*invlist = reinterpret_cast<unsigned_long_invlist*>(service_queries[i].second);
            while(!invlist->EmptyPendingQueue());
        }
        else if(service_queries[i].first == 2)
        {
            float_invlist*invlist = reinterpret_cast<float_invlist*>(service_queries[i].second);
            while(!invlist->EmptyPendingQueue());
        }
        else if(service_queries[i].first == 3)
        {
            double_invlist*invlist = reinterpret_cast<double_invlist*>(service_queries[i].second);
            while(!invlist->EmptyPendingQueue());

        }
    }

    reqc = 0;
    std::fill(recvv.begin(), recvv.end(), 0);

    for(int i = 0; i < nservers; i++)
    {
        MPI_Isend(&sendv, 1, MPI_INT, i, tag, MPI_COMM_WORLD, &reqs[reqc]);
        reqc++;
        MPI_Irecv(&recvv[i], 1, MPI_INT, i, tag, MPI_COMM_WORLD, &reqs[reqc]);
        reqc++;
    }

    MPI_Waitall(reqc, reqs, MPI_STATUS_IGNORE);

    delete reqs;
}
