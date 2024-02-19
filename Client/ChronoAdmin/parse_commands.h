#include <boost/interprocess/ipc/message_queue.hpp>
#include "log.h"

using namespace boost::interprocess;

extern int num_processes;

void parse_commands(message_queue**queues, std::vector <std::string> &command_subs, bool &end_loop)
{
    if(command_subs[0].compare("-disconnect") == 0) end_loop = true;

    int process_no;

    if(command_subs[0].compare("-connect") == 0 || command_subs[0].compare("-disconnect") == 0) process_no = -1;
    else
    {
        int numc = command_subs.size();
        assert(command_subs[numc - 2].compare("-p") == 0);
        process_no = std::stoi(command_subs[numc - 1]);
        assert(process_no >= 0 && process_no < num_processes);
    }

    if(process_no == -1)
    {
        std::string msg;
        for(size_t i = 0; i < command_subs.size(); i++)
        {
            msg += command_subs[i];
            if(i < command_subs.size() - 1) msg += " ";
        }
        msg += "\0";
        for(int i = 0; i < num_processes; i++)
        {
            queues[i]->send((char*)msg.c_str(), msg.length(), 1);

        }

    }
    else
    {
        std::string msg;
        int nc = command_subs.size() - 2;

        for(int i = 0; i < nc; i++)
        {
            msg += command_subs[i];
            if(i < nc - 1) msg += " ";
        }
        msg += "\0";
        queues[process_no]->send((char*)msg.c_str(), msg.length(), 1);
    }
}

void run_command(ChronoLogClient*&client, std::string &msg_string, std::string &client_id, int &flags, uint64_t &offset
                 , bool &end_loop)
{

    std::vector <std::string> command_subs;

    const char*delim = " ";
    if(msg_string.length() > 0)
    {
        char*sub = std::strtok((char*)msg_string.c_str(), delim);
        if(sub != NULL) command_subs.push_back(sub);

        while(sub != NULL)
        {
            sub = std::strtok(NULL, delim);
            if(sub != NULL) command_subs.push_back(sub);
        }
    }

    int ret;
    bool incorrect = false;
    if(command_subs[0].compare("-connect") == 0)
    {
        int protocol = -1;
        std::string hostname;
        int portno = -1;
        incorrect = false;
        if(command_subs.size() != 7 ||
           (command_subs[1].compare("-protocol") != 0 && command_subs[3].compare("-hostname") != 0 &&
            command_subs[5].compare("-port") != 0))
            incorrect = true;
        if(incorrect)
        {
            LOG_ERROR("incorrect command, retry");
            /*std::cout << " incorrect command, retry" << std::endl;*/
            return;
        }

        protocol = std::stoi(command_subs[2]);
        hostname = command_subs[4];
        portno = std::stoi(command_subs[6]);

        if(protocol >= 0 && protocol < 3 && !hostname.empty() && portno != -1)
        {
            std::string server_uri;
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
            else protocolstring = "verbs";

            char ip_add[16];
            std::string host_ip;

            struct hostent*he = gethostbyname(hostname.c_str());
            if(he == 0)
            {
                LOG_ERROR("hostname not found");
                /*std::cout << " hostname not found" << std::endl;*/
                return;
            }

            in_addr**addr_list = (struct in_addr**)he->h_addr_list;
            strcpy(ip_add, inet_ntoa(*addr_list[0]));
            host_ip = std::string(ip_add);

            server_uri = protocolstring + "://" + host_ip + ":" + std::to_string(portno);
            LOG_INFO("server_uri = {}", server_uri);
            /*std::cout << " server_uri = " << server_uri << std::endl;*/

            if(client == nullptr)
            {
                client = new ChronoLogClient((ChronoLogRPCImplementation)protocol, host_ip, portno);
                if(client_id.empty())
                    client_id = gen_random(8);
                flags = 0;
                offset = 0;
                ret = client->Connect(server_uri, client_id, flags, offset);
                assert(ret == chronolog::CL_SUCCESS);
            }
            else
                LOG_ERROR("client connected, Incorrect command, retry");
            /*std::cout << " client connected, Incorrect command, retry" << std::endl;*/

        }
        else
        {
            incorrect = true;
            LOG_ERROR("incorrect command, retry");
            /*std::cout << " incorrect command, retry" << std::endl;*/
            return;
        }
    }
    else if(command_subs[0].compare("-disconnect") == 0)
    {
        assert (client != nullptr && !client_id.empty());
        ret = client->Disconnect(client_id, flags);
        end_loop = true;
    }
    else if(command_subs[0].compare("-c") == 0)
    {
        if(command_subs.size() != 2) incorrect = true;
        if(incorrect)
        {
            LOG_ERROR("incorrect command, retry");
            /*std::cout << " incorrect command, retry" << std::endl;*/
            return;
        }
        std::string chronicle_name = command_subs[1];
        std::unordered_map <std::string, std::string> chronicle_attrs;
        chronicle_attrs.emplace("Priority", "High");
        chronicle_attrs.emplace("IndexGranularity", "Millisecond");
        chronicle_attrs.emplace("TieringPolicy", "Hot");
        ret = client->CreateChronicle(chronicle_name, chronicle_attrs, flags);
        assert(ret == chronolog::CL_SUCCESS || ret == chronolog::CL_ERR_CHRONICLE_EXISTS);
    }
    else if(command_subs[0].compare("-s") == 0)
    {
        if(command_subs.size() != 3) incorrect = true;
        if(incorrect)
        {
            LOG_ERROR("incorrect command, retry");
            /*std::cout << " incorrect command, retry" << std::endl;*/
            return;
        }
        std::string chronicle_name = command_subs[1];
        std::string story_name = command_subs[2];
        std::unordered_map <std::string, std::string> story_attrs;
        story_attrs.emplace("Priority", "High");
        story_attrs.emplace("IndexGranularity", "Millisecond");
        story_attrs.emplace("TieringPolicy", "Hot");
        ret = client->CreateStory(chronicle_name, story_name, story_attrs, flags);
        assert(ret == chronolog::CL_SUCCESS || ret == chronolog::CL_ERR_STORY_EXISTS);
    }
    else if(command_subs[0].compare("-a") == 0)
    {
        if(command_subs.size() < 3 || (command_subs[1].compare("-c") != 0 && command_subs[1].compare("-s") != 0))
            incorrect = true;
        if(incorrect)
        {
            LOG_ERROR("incorrect command, retry");
            /*std::cout << " incorrect command, retry" << std::endl;*/
            return;
        }
        if(command_subs[1].compare("-c") == 0)
        {
            std::string chronicle_name = command_subs[2];
            ret = client->AcquireChronicle(chronicle_name, flags);

        }
        else if(command_subs[1].compare("-s") == 0)
        {
            if(command_subs.size() != 4) incorrect = true;
            if(incorrect)
            {
                LOG_ERROR("incorrect command, retry");
                /*std::cout << " incorrect command, retry" << std::endl;*/
                return;
            }
            std::string chronicle_name = command_subs[2];
            std::string story_name = command_subs[3];
            ret = client->AcquireStory(chronicle_name, story_name, flags);
        }
    }
    else if(command_subs[0].compare("-r") == 0)
    {
        if(command_subs.size() < 3 || (command_subs[1].compare("-c") != 0 && command_subs[1].compare("-s") != 0))
            incorrect = true;
        if(incorrect)
        {
            LOG_ERROR("incorrect command, retry");
            /*std::cout << " incorrect command, retry" << std::endl;*/
            return;
        }
        if(command_subs[1].compare("-c") == 0)
        {
            std::string chronicle_name = command_subs[2];
            ret = client->ReleaseChronicle(chronicle_name, flags);
        }
        else if(command_subs[1].compare("-s") == 0)
        {
            if(command_subs.size() != 4) incorrect = true;
            if(incorrect)
            {
                LOG_ERROR("incorrect command, retry");
                /*std::cout << " incorrect command, retry" << std::endl;*/
                return;
            }
            std::string chronicle_name = command_subs[2];
            std::string story_name = command_subs[3];
            ret = client->ReleaseStory(chronicle_name, story_name, flags);
        }
    }
    else if(command_subs[0].compare("-d") == 0)
    {
        if(command_subs.size() < 3 || (command_subs[1].compare("-c") != 0 && command_subs[1].compare("-s") != 0))
            incorrect = true;
        if(incorrect)
        {
            LOG_ERROR("incorrect command, retry");
            /*std::cout << " incorrect command, retry" << std::endl;*/
            return;
        }
        if(command_subs[1].compare("-c") == 0)
        {
            std::string chronicle_name = command_subs[2];
            ret = client->DestroyChronicle(chronicle_name, flags);
        }
        else if(command_subs[1].compare("-s") == 0)
        {
            if(command_subs.size() != 4) incorrect = true;
            if(incorrect)
            {
                LOG_ERROR("incorrect command, retry");
                /*std::cout << " incorrect command, retry" << std::endl;*/
                return;
            }
            std::string chronicle_name = command_subs[2];
            std::string story_name = command_subs[3];
            ret = client->DestroyStory(chronicle_name, story_name, flags);
        }
    }
    return;
}
