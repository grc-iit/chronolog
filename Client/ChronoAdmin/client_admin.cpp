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

#include <chronolog_client.h>
#include <cmd_arg_parse.h>
#include <common.h>
#include <cassert>
#include <unistd.h>
#include <pwd.h>

int main(int argc, char **argv)
{
    std::string default_conf_file_path = "./default_conf.json";
    std::string conf_file_path;
    conf_file_path = parse_conf_path_arg(argc, argv);
    if (conf_file_path.empty())
    {
        conf_file_path = default_conf_file_path;
    }
    ChronoLog::ConfigurationManager confManager(conf_file_path);
    chronolog::Client client(confManager);

    int flags = 0;
    int ret;

    std::string client_id = gen_random(8);

    std::string server_ip = confManager.CLIENT_CONF.VISOR_CLIENT_PORTAL_SERVICE_CONF.RPC_CONF.IP;
    int base_port = confManager.CLIENT_CONF.VISOR_CLIENT_PORTAL_SERVICE_CONF.RPC_CONF.BASE_PORT;
    std::string server_uri = confManager.CLIENT_CONF.VISOR_CLIENT_PORTAL_SERVICE_CONF.RPC_CONF.PROTO_CONF;
    server_uri += "://" + server_ip + ":" + std::to_string(base_port);

    std::string username = getpwuid(getuid())->pw_name;
    client.Connect(server_uri, username, flags);

    std::cout << " connected to server address : " << server_uri << std::endl;

    std::cout << "Metadata operations: \n"
              << "\t-c <chronicle_name> , create a Chronicle <chronicle_name> \n"
              << "\t-a -s <chronicle_name> <story_name>, acquire Story <story_name> in Chronicle <chronicle_name> \n"
              << "\t-r -s <chronicle_name> <story_name>, release Story <story_name> in Chronicle <chronicle_name> \n"
              << "\t-d -s <chronicle_name> <story_name>, destroy Story <story_name> in Chronicle <chronicle_name> \n"
              << "\t-d -c <chronicle_name>, destroy Chronicle <chronicle_name> \n"
              << "\t-disconnect \n" << std::endl;


    std::vector<std::string> commands;

    std::string command_line;

    const char *delim = " ";

    std::string str;

    flags = 0;

    while (true)
    {
        str.clear();
        std::getline(std::cin, str);
        if (str == "-disconnect") break;

        std::vector<std::string> command_subs;

        char *s = std::strtok((char *) str.c_str(), delim);
        command_subs.emplace_back(s);

        while (s != nullptr)
        {
            s = std::strtok(nullptr, delim);
            if (s != nullptr)
                command_subs.emplace_back(s);
        }

        if (command_subs[0] == "-c")
        {
            assert(command_subs.size() == 2);
            std::string chronicle_name = command_subs[1];
            std::unordered_map<std::string, std::string> chronicle_attrs;
            chronicle_attrs.emplace("Priority", "High");
            chronicle_attrs.emplace("IndexGranularity", "Millisecond");
            chronicle_attrs.emplace("TieringPolicy", "Hot");
            ret = client.CreateChronicle(chronicle_name, chronicle_attrs, flags);
            assert(ret == CL_SUCCESS || ret == CL_ERR_CHRONICLE_EXISTS);
        }
        else if (command_subs[0] == "-a")
        {
            if (command_subs[1] == "-s")
            {
                assert(command_subs.size() == 4);
                std::string chronicle_name = command_subs[2];
                std::string story_name = command_subs[3];
                std::unordered_map<std::string, std::string> story_acquisition_attrs;
                story_acquisition_attrs.emplace("Priority", "High");
                story_acquisition_attrs.emplace("IndexGranularity", "Millisecond");
                story_acquisition_attrs.emplace("TieringPolicy", "Hot");
                std::pair<int, chronolog::StoryHandle *> acq_ret = client.AcquireStory(chronicle_name, story_name,
                                                                                       story_acquisition_attrs,
                                                                                       flags);
                assert(acq_ret.first == CL_SUCCESS || acq_ret.first == CL_ERR_ACQUIRED);
            }
        }
        else if (command_subs[0] == "-r")
        {
            if (command_subs[1] == "-s")
            {
                assert(command_subs.size() == 4);
                std::string chronicle_name = command_subs[2];
                std::string story_name = command_subs[3];
                ret = client.ReleaseStory(chronicle_name, story_name);
                assert(ret == CL_SUCCESS || ret == CL_ERR_NOT_EXIST);
            }

        }
        else if (command_subs[0] == "-d")
        {
            if (command_subs[1] == "-c")
            {
                assert(command_subs.size() == 3);
                std::string chronicle_name = command_subs[2];
                ret = client.DestroyChronicle(chronicle_name);
                assert(ret == CL_SUCCESS || ret == CL_ERR_NOT_EXIST || ret == CL_ERR_ACQUIRED);
            }
            else if (command_subs[1] == "-s")
            {
                assert(command_subs.size() == 4);
                std::string chronicle_name = command_subs[2];
                std::string story_name = command_subs[3];
                ret = client.DestroyStory(chronicle_name, story_name);
                assert(ret == CL_SUCCESS || ret == CL_ERR_NOT_EXIST || ret == CL_ERR_ACQUIRED);
            }

        }


    }

    ret = client.Disconnect();
    assert(ret == CL_SUCCESS);

    return 0;
}
