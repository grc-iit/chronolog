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

#include <cassert>
#include <boost/interprocess/ipc/message_queue.hpp>
#include <common.h>
#include <client.h>
#include <iostream>
#include <sys/wait.h>
#include <unistd.h>
#include "parse_commands.h"

int max_msg_len;
int num_processes;

void print_banner()
{

      std::cout <<" -connect -protocol <int> -hostname <string> -port <int>, protocol should be 0(sockets),1(tcp),2(verbs), connects all clients"<<std::endl;
      std::cout <<" Metadata operations : -c <string> -p <int>, create a chronicle with name <string> by client <int>  "<<std::endl
              <<" -s <string1> <string2> -p <int> , create a story with name string1+string2 : string1 = chronicle name, string2 = story name by client <int>"<<std::endl
              <<" -a -c <string> -p <int>, acquire chronicle with name <string> by client <int>"<<std::endl
              <<" -a -s <string1> <string2> -p <int>, acquire story with name string1+string2 : string1 = chronicle name, string2 = story name by client <int>"<<std::endl
              <<" -r -c <string> -p <int>, release chronicle with name <string> by client <int>"<<std::endl
              <<" -r -s <string1> <string2> -p <int>, release story with name string1+string2 : string1 = chronicle name, string2 = story name by client <int> "<<std::endl
              <<" -d -c <string> -p <int>, destroy chronicle with name <string> by client <int>"<<std::endl
              <<" -d -s <string1> <string2> -p <int>, destroy story with name string1+string2 : string1 = chronicle name, string2 = story name by client <int>"<<std::endl
              <<" -disconnect , disconnect all clients"<<std::endl;
}

using namespace boost::interprocess;

int main(int argc, char **argv) 
{

    std::string filename = "server_list";

    std::fstream fp(filename,std::ios::out);
    fp << "localhost"<<std::endl;

    if(argc != 2)
    {
	std::cout <<" ChronoAdmin_Sim usage : ./ChronoAdmin_Sim <int>, run simulation with <int> number of clients"<<std::endl
		<< " -h for help"<<std::endl;
    }
    assert(argc==2);

    num_processes = std::stoi(argv[1]);

    max_msg_len = 256;

    std::vector<std::string> queue_names;

    std::string header = "message_queue";

    for(int i=0;i<num_processes;i++)
    {
	std::string queue_name = header+":"+std::to_string(i);
	queue_names.push_back(queue_name);
    }	

    message_queue **queues = (message_queue**)std::malloc(num_processes*sizeof(message_queue*));

    for(int i=0;i<num_processes;i++)
    {
	message_queue::remove((char*)queue_names[i].c_str());
	queues[i] = new message_queue(create_only,(char*)queue_names[i].c_str(),100,256*sizeof(char));
    }

    int g_pid = getpid();
    int id;

    for(int i=0;i<num_processes;i++)
    {
	   if(getpid()==g_pid)
	   {
		id = i;
		int pid = fork();
		assert(pid != -1);
	   }
    }

    if(getpid()==g_pid)  // user interface
    {

      std::string str;
      const char *delim = " ";

      bool end_loop = false;

     while(true)
     {
        str.clear();
        std::getline(std::cin,str);

	std::vector<std::string> command_subs;

        char* s = std::strtok((char *)str.c_str(),delim);
        if(s != NULL) command_subs.push_back(s);

        while(s != NULL)
        {
          s = std::strtok(NULL,delim);
          if(s != NULL)
          command_subs.push_back(s);
        }

	if(command_subs[0].compare("-h")==0) print_banner();
	else
	parse_commands(queues,command_subs,end_loop);

	if(end_loop) break;
     }

        int wstatus;
        siginfo_t sinfo;

        wstatus = waitid(P_ALL,0,&sinfo,WEXITED|WSTOPPED|__WALL);
	assert(wstatus == 0);

	for(int i=0;i<num_processes;i++)
		if(queues[i] != nullptr) delete queues[i];
	std::free(queues);

    }
    else
    {
	    bool end_loop = false;
	    std::string queue_name = header+":"+std::to_string(id);
            message_queue myqueue = message_queue(open_only,(char*)queue_name.c_str());	
	    ChronoLogClient *client = nullptr;
	    std::string client_id = gen_random(8);
	    client_id += std::to_string(id);
	    int flags=0;
	    uint64_t offset=0;

	    while(true)
	    {
	       message_queue::size_type recvd=0;
	       char msg[256];
	       std::fill(msg,msg+256,0);
	       unsigned int priority = 1;
	       do
	       {
	          myqueue.receive(msg,256,recvd,priority);
	       }while(recvd==0);

	       std::string msg_string(msg);

	       run_command(client,msg_string,client_id,flags,offset,end_loop);

	       if(end_loop) break;
	    }
	    if(client != nullptr) delete client;
	    _exit(2);

    }
    

    return 0;
}
