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
#include <cassert>

int main(int argc, char**argv)
{

    std::string hostname;
    std::string portnum;
    std::string filename = "server_list";
    ChronoLogRPCImplementation protocol;
    std::string protocol_string;
    std::string server_uri;
    std::vector <std::string> args;

    if(argc != 7)
    {
        std::cout << " ChronoAdmin usage : ./ChronoAdmin -protocol p -hostname h -port n" << std::endl;
        exit(-1);
    }

    for(int i = 1; i < 7; i++)
    {
        std::string s(argv[i]);
        args.push_back(s);
    }

    bool end_program = false;


    int portno = -1;
    protocol = (ChronoLogRPCImplementation) - 1;

    for(int i = 0; i < args.size(); i++)
    {
        if(i % 2 == 0)
        {
            if(args[i].compare("-protocol") == 0)
            {
                protocol = (ChronoLogRPCImplementation)std::stoi(args[i + 1]);
            }
            else if(args[i].compare("-hostname") == 0)
            {
                hostname = args[i + 1];
            }
            else if(args[i].compare("-port") == 0)
            {
                portno = std::stoi(args[i + 1]);
            }
            else
            {
                end_program = true;
                break;
            }
        }
    }

    if(end_program || protocol == -1 || hostname.empty() || portno == -1) exit(-1);

    if(protocol < 0 || protocol > 2)
    {
        std::cout << " protocol not supported : valid values are 0 (sockets), 1 (tcp) and 2 (verbs)" << std::endl;
        end_program = true;
    }

    if(end_program) exit(-1);

    char ip_add[16];
    std::string host_ip;

    struct hostent*he = gethostbyname(hostname.c_str());
    if(he == 0)
    {
        std::cout << " hostname not found, Exiting" << std::endl;
        exit(-1);
    }
    in_addr**addr_list = (struct in_addr**)he->h_addr_list;
    strcpy(ip_add, inet_ntoa(*addr_list[0]));
    host_ip = std::string(ip_add);

    std::string protocolstring;

    if(protocol == 0)
    {
        protocolstring = "ofi+sockets";
        ChronoLogCharStruct prot_struct(protocolstring);
        CHRONOLOG_CONF->SOCKETS_CONF = prot_struct;
    }
    else if(protocol == 1)
    {
        protocolstring = "ofi+tcp";
        ChronoLogCharStruct prot_struct(protocolstring);
        CHRONOLOG_CONF->SOCKETS_CONF = prot_struct;
    }
    else if(protocol == 2) protocolstring = "verbs";

    server_uri = protocolstring + "://" + host_ip + ":" + std::to_string(portno);

    std::fstream fp(filename, std::ios::out);
    fp << "localhost" << std::endl;

    ChronoLogClient client(protocol, host_ip, portno);

    int flags = 0;
    int ret;
    uint64_t offset = 0;

    std::string client_id = gen_random(8);

    try
    {
        ret = client.Connect(server_uri, client_id, flags, offset);
    }
    catch(const thallium::exception &e)
    {
        std::cerr << " Failed to connect" << e.what() << std::endl;
        exit(-1);
    };


    std::cout << " connected to server address : " << server_uri << std::endl;

    std::cout << " Metadata operations : -c <string> , create a chronicle with name <string>  " << std::endl
              << " -s <string1> <string2>, create a story with name string1+string2 : string1 = chronicle name, string2 = story name "
              << std::endl << " -a -c <string>, acquire chronicle with name <string>" << std::endl
              << " -a -s <string1> <string2>, acquire story with name string1+string2 : string1 = chronicle name, string2 = story name"
              << std::endl << " -r -c <string>, release chronicle with name <string>" << std::endl
              << " -r -s <string1> <string2>, release story with name string1+string2 : string1 = chronicle name, string2 = story name"
              << std::endl << " -d -c <string>, destroy chronicle with name <string>" << std::endl
              << " -d -s <string1> <string2>, destroy story with name string1+string2 : string1 = chronicle name, string2 = story name"
              << std::endl << " -disconnect " << std::endl;


    std::vector <std::string> commands;

    std::string command_line;

    const char*delim = " ";

    std::string str;

    flags = 0;

    while(true)
    {
        str.clear();
        std::getline(std::cin, str);
        if(str.compare("-disconnect") == 0) break;

        std::vector <std::string> command_subs;

        char*s = std::strtok((char*)str.c_str(), delim);
        command_subs.push_back(s);

        while(s != NULL)
        {
            s = std::strtok(NULL, delim);
            if(s != NULL)
                command_subs.push_back(s);
        }

        if(command_subs[0].compare("-c") == 0)
        {
            assert(command_subs.size() == 2);
            std::string chronicle_name = command_subs[1];
            std::unordered_map <std::string, std::string> chronicle_attrs;
            chronicle_attrs.emplace("Priority", "High");
            chronicle_attrs.emplace("IndexGranularity", "Millisecond");
            chronicle_attrs.emplace("TieringPolicy", "Hot");
            ret = client.CreateChronicle(chronicle_name, chronicle_attrs, flags);
            assert(ret == CL_SUCCESS || ret == CL_ERR_CHRONICLE_EXISTS);
        }
        else if(command_subs[0].compare("-s") == 0)
        {
            assert(command_subs.size() == 3);
            std::string chronicle_name = command_subs[1];
            std::string story_name = command_subs[2];
            std::unordered_map <std::string, std::string> story_attrs;
            story_attrs.emplace("Priority", "High");
            story_attrs.emplace("IndexGranularity", "Millisecond");
            story_attrs.emplace("TieringPolicy", "Hot");
            ret = client.CreateStory(chronicle_name, story_name, story_attrs, flags);
            assert(ret == CL_SUCCESS || ret == CL_ERR_STORY_EXISTS);

        }
        else if(command_subs[0].compare("-a") == 0)
        {
            if(command_subs[1].compare("-c") == 0)
            {
                assert(command_subs.size() == 3);
                std::string chronicle_name = command_subs[2];
                ret = client.AcquireChronicle(chronicle_name, flags);
            }
            else if(command_subs[1].compare("-s") == 0)
            {
                assert(command_subs.size() == 4);
                std::string chronicle_name = command_subs[2];
                std::string story_name = command_subs[3];
                ret = client.AcquireStory(chronicle_name, story_name, flags);
            }
        }
        else if(command_subs[0].compare("-r") == 0)
        {
            if(command_subs[1].compare("-c") == 0)
            {
                assert(command_subs.size() == 3);
                std::string chronicle_name = command_subs[2];
                ret = client.ReleaseChronicle(chronicle_name, flags);
            }
            else if(command_subs[1].compare("-s") == 0)
            {
                assert(command_subs.size() == 4);
                std::string chronicle_name = command_subs[2];
                std::string story_name = command_subs[3];
                ret = client.ReleaseStory(chronicle_name, story_name, flags);
            }

        }
        else if(command_subs[0].compare("-d") == 0)
        {
            if(command_subs[1].compare("-c") == 0)
            {
                assert(command_subs.size() == 3);
                std::string chronicle_name = command_subs[2];
                ret = client.DestroyChronicle(chronicle_name, flags);
            }
            else if(command_subs[1].compare("-s") == 0)
            {
                assert(command_subs.size() == 4);
                std::string chronicle_name = command_subs[2];
                std::string story_name = command_subs[3];
                ret = client.DestroyStory(chronicle_name, story_name, flags);
            }

        }


    }


    ret = client.Disconnect(client_id, flags);

    return 0;
}
