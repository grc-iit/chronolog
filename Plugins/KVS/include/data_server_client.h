#ifndef __DATA_SERVER_CLIENT_H_
#define __DATA_SERVER_CLIENT_H_

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
#include <mpi.h>
#include <fstream>


namespace tl = thallium;


class data_server_client
{

private:
    tl::engine*thallium_server;
    tl::engine*thallium_shm_server;
    tl::engine*thallium_client;
    tl::engine*thallium_shm_client;
    int nservers;
    int serverid;
    std::vector <tl::endpoint> serveraddrs;
    std::vector <std::string> ipaddrs;
    std::vector <std::string> shmaddrs;
    std::string myipaddr;
    std::string myhostname;
    int base_port;
    std::string serveraddrsfile;

public:

    data_server_client(int n, int p, int b): nservers(n), serverid(p), base_port(b)
    {

        serveraddrsfile = "kvsaddrs";
        char processor_name[1024];
        int len = 0;
        MPI_Get_processor_name(processor_name, &len);
        myhostname.assign(processor_name);
        char ip[16];
        struct hostent*he = gethostbyname(myhostname.c_str());
        auto**addr_list = (struct in_addr**)he->h_addr_list;
        strcpy(ip, inet_ntoa(*addr_list[0]));
        myipaddr.assign(ip);

        //std::cout <<" nservers = "<<nservers<<" server_id = "<<serverid<<" ip = "<<myipaddr<<" name = "<<myhostname<<std::endl;

        std::vector <int> strlens;
        strlens.resize(nservers);
        int l = myipaddr.length();
        MPI_Allgather(&l, 1, MPI_INT, strlens.data(), 1, MPI_INT, MPI_COMM_WORLD);
        std::vector <char> ipstrings;
        int total_length = 0;
        for(int i = 0; i < strlens.size(); i++)
            total_length += strlens[i];
        ipstrings.resize(total_length);
        std::vector <int> recv_counts;
        recv_counts.assign(strlens.begin(), strlens.end());
        std::vector <int> recv_displ;
        recv_displ.resize(nservers);
        std::fill(recv_displ.begin(), recv_displ.end(), 0);

        for(int i = 1; i < nservers; i++)
            recv_displ[i] = recv_displ[i - 1] + recv_counts[i - 1];

        MPI_Allgatherv(myipaddr.data(), l, MPI_CHAR, ipstrings.data(), recv_counts.data(), recv_displ.data(), MPI_CHAR
                       , MPI_COMM_WORLD);
        std::set <std::string> node_ips;

        for(int i = 0; i < nservers; i++)
        {
            std::string s;
            s.assign(ipstrings.data() + recv_displ[i], ipstrings.data() + recv_displ[i] + recv_counts[i]);
            ipaddrs.push_back(s);
            node_ips.insert(s);
        }

        int pos = std::distance(ipaddrs.begin(), std::find(ipaddrs.begin(), ipaddrs.end(), myipaddr));

        int port_addr = base_port + serverid - pos;
        std::string server_addr = "na+sm://";

        //std::cout <<" serverid = "<<serverid<<" port_addr = "<<port_addr<<std::endl;

        thallium_shm_server = new tl::engine(server_addr.c_str(), THALLIUM_SERVER_MODE, true, 4);
        std::string server_shm_addr = thallium_shm_server->self();
        l = server_shm_addr.length();
        strlens.clear();
        strlens.resize(nservers);
        MPI_Allgather(&l, 1, MPI_INT, strlens.data(), 1, MPI_INT, MPI_COMM_WORLD);
        recv_counts.assign(strlens.begin(), strlens.end());
        total_length = 0;
        for(int i = 0; i < nservers; i++) total_length += strlens[i];
        ipstrings.resize(total_length);
        recv_displ[0] = 0;
        for(int i = 1; i < nservers; i++) recv_displ[i] = recv_displ[i - 1] + recv_counts[i - 1];

        MPI_Allgatherv(server_shm_addr.data(), l, MPI_CHAR, ipstrings.data(), recv_counts.data(), recv_displ.data()
                       , MPI_CHAR, MPI_COMM_WORLD);
        for(int i = 0; i < nservers; i++)
        {
            std::string addr;
            addr.assign(ipstrings.data() + recv_displ[i], ipstrings.data() + recv_displ[i] + recv_counts[i]);
            shmaddrs.push_back(addr);
        }

        server_addr = "ofi+sockets://";
        server_addr += myipaddr;
        server_addr = server_addr + ":" + std::to_string(port_addr);
        thallium_server = new tl::engine(server_addr.c_str(), THALLIUM_SERVER_MODE, true, 4);
        //std::cout <<" server_addr = "<<server_addr<<std::endl;
        MPI_Barrier(MPI_COMM_WORLD);

        thallium_client = new tl::engine("ofi+sockets", THALLIUM_CLIENT_MODE, true, 4);
        thallium_shm_client = new tl::engine("na+sm", THALLIUM_CLIENT_MODE, true, 4);

        std::vector <std::string> serverstrings;

        for(int i = 0; i < nservers; i++)
        {
            int portno = base_port;
            std::string serveraddr_1 = "ofi+sockets://";
            serveraddr_1 += ipaddrs[i];
            serveraddr_1 += ":";
            int spos = std::distance(ipaddrs.begin(), std::find(ipaddrs.begin(), ipaddrs.end(), ipaddrs[i]));
            serveraddr_1 += std::to_string(portno + i - spos);
            serverstrings.push_back(serveraddr_1);
            tl::endpoint ep = thallium_client->lookup(serveraddr_1.c_str());
            serveraddrs.push_back(ep);
        }

        if(serverid == 0)
        {
            std::ofstream filest(serveraddrsfile.c_str(), std::ios_base::out);

            filest << std::to_string(nservers) << std::endl;

            for(int i = 0; i < nservers; i++)
                filest << shmaddrs[i] << std::endl;

            for(int i = 0; i < nservers; i++)
                filest << ipaddrs[i] << std::endl;

            for(int i = 0; i < nservers; i++)
                filest << serverstrings[i] << std::endl;

            filest.close();
        }

        MPI_Barrier(MPI_COMM_WORLD);

    }

    ~data_server_client()
    {
        thallium_server->finalize();
        thallium_shm_server->finalize();
        delete thallium_server;
        delete thallium_shm_server;
        delete thallium_client;
        delete thallium_shm_client;
    }

    tl::engine*get_thallium_server()
    {
        return thallium_server;
    }

    tl::engine*get_thallium_shm_server()
    {
        return thallium_shm_server;
    }

    tl::engine*get_thallium_client()
    {
        return thallium_client;
    }

    tl::engine*get_thallium_shm_client()
    {
        return thallium_shm_client;
    }

    std::vector <tl::endpoint> &get_serveraddrs()
    {
        return serveraddrs;
    }

    std::vector <std::string> &get_ipaddrs()
    {
        return ipaddrs;
    }

    std::vector <std::string> &get_shm_addrs()
    {
        return shmaddrs;
    }

};

#endif
