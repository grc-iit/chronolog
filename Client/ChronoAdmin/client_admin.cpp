//
// Created by kfeng on 7/11/22.
//

#include <client.h>
#include <common.h>
#include <cassert>
#include <iostream>
#include <fstream>
#include <sstream>
#include <sys/types.h>
#include <unistd.h>
#include <string>

int main() 
{

    std::string hostname;
    std::string portnum;
    std::string filename = "server_list";
    std::string protocol = "ofi";
    std::string provider; 

    std::cout <<" Enter protocol (ofi provider), choose from tcp, sockets, shm, udp "<<std::endl;
    std::cin >> provider;

    std::cout <<" Enter server hostname (localhost) : "<<std::endl;
    std::cin >> hostname;

    std::cout <<" Enter server portnumber (5555) : "<<std::endl;
    std::cin >> portnum;
    int portno = std::stoi(portnum);
    char ip_add[16];
    std::string host_ip;

    struct hostent *he = gethostbyname(hostname.c_str());
    if(he == 0) 
    {
	std::cout <<" hostname not found, Exiting"<<std::endl;
	exit(-1);
    }
    in_addr **addr_list = (struct in_addr **) he->h_addr_list;
    strcpy(ip_add, inet_ntoa(*addr_list[0]));
    host_ip = std::string(ip_add);

    ChronoLogCharStruct host_ip_struct(host_ip);
    ChronoLogCharStruct socket_struct(protocol+"+"+provider);

    CHRONOLOG_CONF->SOCKETS_CONF = socket_struct;
    CHRONOLOG_CONF->RPC_SERVER_IP = host_ip_struct;
    CHRONOLOG_CONF->RPC_BASE_SERVER_PORT = portno;

    ChronoLogClient client;

    std::string conf = protocol + "+"+ provider;    
    std::string conn = "://";
    std::string server_uri = conf + conn + host_ip + ":" + std::to_string(portno);

    int flags = 0;
    bool ret = false;

    std::string client_id = gen_random(8);

    ret = client.Connect(server_uri, client_id);

    std::cout <<" connected to server address : "<<server_uri<<std::endl;

    ret = client.Disconnect(client_id, flags);

    return 0;
}
