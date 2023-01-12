/*BSD 2-Clause License

Copyright (c) 2022, Scalable Computing Software Laboratory
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

Created by Aparna on 01/12/2023
*/

#include <client.h>
#include <common.h>

int main() 
{

    std::string hostname;
    std::string portnum;
    std::string filename = "server_list";
    ChronoLogRPCImplementation protocol;
    std::string protocol_string; 
    std::string server_uri;
    std::cout <<" Enter protocol 0 for sockets, 1 for TCP, 2 for verbs "<<std::endl;
    std::cin >> protocol_string;
    protocol = (ChronoLogRPCImplementation)std::stoi(protocol_string);
    if(protocol < 0 || protocol > 2) 
    {
	 std::cout <<" protocol not supported, Exiting"<<std::endl;
	 exit(-1);
    }

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

    std::string protocolstring;

    if(protocol==0)
    {
       protocolstring = "ofi+sockets";
       ChronoLogCharStruct prot_struct(protocolstring);
       CHRONOLOG_CONF->SOCKETS_CONF = prot_struct;
    }
    else if(protocol==1)
    {
	protocolstring = "ofi+tcp";
	ChronoLogCharStruct prot_struct(protocolstring);
	CHRONOLOG_CONF->SOCKETS_CONF = prot_struct;
    }
    else protocolstring = "verbs";

    server_uri = protocolstring+"://"+host_ip+":"+portnum;

    std::fstream fp(filename,std::ios::out);
    fp << "localhost"<<std::endl;

    ChronoLogClient client(protocol,host_ip,portno);
     
    int flags = 0;
    int ret;
    uint64_t offset = 0;

    std::string client_id = gen_random(8);

    ret = client.Connect(server_uri, client_id,flags,offset);

    std::cout <<" connected to server address : "<<server_uri<<std::endl;

    ret = client.Disconnect(client_id, flags);

    return 0;
}
