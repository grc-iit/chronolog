//
// Created by kfeng on 7/19/22.
//

#include "ChronoVisorServer2.h"
#include "macro.h"
#include "ACL_List.h"
#include <iostream>

namespace ChronoVisor {
    ChronoVisorServer2::ChronoVisorServer2(const std::string &conf_file_path) {
        if (!conf_file_path.empty())
            CHRONOLOG_CONF->LoadConfFromJSONFile(conf_file_path);
        init();
    }

    ChronoVisorServer2::ChronoVisorServer2(const ChronoLog::ConfigurationManager &conf_manager) {
        CHRONOLOG_CONF->SetConfiguration(conf_manager);
        init();
    }

    int ChronoVisorServer2::start() {
        LOGI("ChronoVisor server starting, listen on %d ports starting from %d ...", numPorts_, basePorts_);

	std::string filename = "clients.json";
	//std::shared_ptr<ClientRegistryManager> clientRegistry = rpcVisor_->GetClientRegistry();
	//std::shared_ptr<ChronicleMetaDirectory> chronicleManager = rpcVisor_->GetChronicleMetaDirectory();

	//CreateACLDB(filename,clientRegistry,chronicleManager);
        // bind functions first (defining RPC routines on engines)
        rpcVisor_->bind_functions();

        // start engines (listening for incoming requests)
        rpcVisor_->Visor_start();

        return 0;
    }

    void ChronoVisorServer2::CreateACLDB(std::string &json_filename, std::shared_ptr<ClientRegistryManager> clientRegistry, std::shared_ptr<ChronicleMetaDirectory> chronicleManager)
{

   struct json_object *config = nullptr;


   config = json_object_from_file(json_filename.c_str());
   struct json_object *clients_vec = json_object_object_get(config,"client");
   int num_clients = 0;
   if(clients_vec != NULL) num_clients = json_object_array_length(clients_vec);

   std::set<std::string> groups;

   for(int i=0;i<num_clients;i++)
   {
        struct json_object *c = json_object_array_get_idx(clients_vec,i);

        std::string name(json_object_get_string(json_object_object_get(c, "name")));
        std::string ipaddress(json_object_get_string(json_object_object_get(c,"ipaddress")));
        std::string group(json_object_get_string(json_object_object_get(c,"group")));
        std::string role = (json_object_get_string(json_object_object_get(c,"role")));
        uint32_t role_num;
	if(role.compare("CHRONOLOG_CLIENT_RESTRICTED_USER")==0) role_num = (uint32_t)CHRONOLOG_CLIENT_RESTRICTED_USER;
	else if(role.compare("CHRONOLOG_CLIENT_REGULAR_USER")==0) role_num = (uint32_t)CHRONOLOG_CLIENT_REGULAR_USER;
	else if(role.compare("CHRONOLOG_CLIENT_PRIVILEGED_USER")==0) role_num = (uint32_t)CHRONOLOG_CLIENT_PRIVILEGED_USER;
	else if(role.compare("CHRONOLOG_CLIENT_CLUSTER_ADMIN")==0) role_num = (uint32_t)CHRONOLOG_CLIENT_CLUSTER_ADMIN;
	else role_num = (uint32_t) CHRONOLOG_CLIENT_RESTRICTED_USER;
     
	ClientInfo cinfo;
	cinfo.addr_ = ipaddress;
	cinfo.group_id_ = group;
	cinfo.client_role_ = role_num;
	
	clientRegistry->add_client_record(name,cinfo);
	groups.insert(group);
   }

        struct json_object *chron_vec = json_object_object_get(config,"chronicles");
        int num_chronicles = 0;
        if(chron_vec != NULL)
          num_chronicles = json_object_array_length(chron_vec);

	for(int i=0;i<num_chronicles;i++)
        {
                struct json_object *chron = json_object_array_get_idx(chron_vec,i);
                std::string chronicle_name(json_object_get_string(json_object_object_get(chron,"chronicle_name")));
		struct json_object *group_vec = json_object_object_get(chron,"group");
		int num_groups = json_object_array_length(group_vec);
		std::string owner;
		for(int j=0;j<num_groups;j++)
		{
		    struct json_object *group_obj = json_object_array_get_idx(group_vec,j);
		    std::string group_id(json_object_get_string(json_object_object_get(group_obj,"group_id")));
		    std::string permission(json_object_get_string(json_object_object_get(group_obj,"permission")));
	
		    if(std::find(groups.begin(),groups.end(),group_id)==groups.end()) continue;

		    std::unordered_map<std::string, std::string> chronicle_attrs;
        	    chronicle_attrs.emplace("Priority", "High");
        	    chronicle_attrs.emplace("IndexGranularity", "Millisecond");
        	    chronicle_attrs.emplace("TieringPolicy", "Hot");
        	    chronicle_attrs.emplace("Permissions",permission);
		
		    std::string name = "default";
		    int res = chronicleManager->create_chronicle(chronicle_name,name,group_id,chronicle_attrs);
		    if(res==CL_SUCCESS)
		    {
		       owner = group_id;
		    }
		    else if(res==CL_ERR_CHRONICLE_EXISTS && !owner.empty())
		   {
			chronicleManager->add_group_to_chronicle(chronicle_name,name,owner,group_id,permission);
		   }
		}
		struct json_object *story_vec = json_object_object_get(chron,"story");
		int num_stories = 0;
		if(story_vec != NULL) num_stories = json_object_array_length(story_vec);

                for(int j=0;j<num_stories;j++)
                {
                   struct json_object *story = json_object_array_get_idx(story_vec,j);
                   std::string story_name(json_object_get_string(json_object_object_get(story,"story_name")));
		   struct json_object *group_vec = json_object_object_get(story,"group");
		   int num_groups = json_object_array_length(group_vec);
		   std::string owner;
		   for(int k=0;k<num_groups;k++)
		   {
			 struct json_object *group_obj = json_object_array_get_idx(group_vec,k);
			 std::string group_id(json_object_get_string(json_object_object_get(group_obj,"group_id")));
			 std::string permission(json_object_get_string(json_object_object_get(group_obj,"permission")));
   
			 if(std::find(groups.begin(),groups.end(),group_id)==groups.end()) continue;

		   	  std::unordered_map<std::string,std::string> story_attrs;
		          story_attrs.emplace("Priority","High");
		          story_attrs.emplace("IndexGranularity","Millisecond");
		          story_attrs.emplace("TieringPolicy","Hot");
		          story_attrs.emplace("Permissions",permission);
			  std::string name = "default";
		          int res = chronicleManager->create_story(chronicle_name,story_name,name,group_id,story_attrs);
			  if(res == CL_SUCCESS) owner = group_id;
		          if(res == CL_ERR_STORY_EXISTS && !owner.empty())
		          {
			    chronicleManager->add_group_to_story(chronicle_name,story_name,name,owner,group_id,permission);
		          }
		  }
		}
        }

}

    void ChronoVisorServer2::init() {
        pClocksourceManager_ = ClocksourceManager::getInstance();
        pClocksourceManager_->setClocksourceType(CHRONOLOG_CONF->CLOCKSOURCE_TYPE);
        CHRONOLOG_CONF->ROLE = CHRONOLOG_VISOR;
        switch (CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.RPC_IMPLEMENTATION) {
            CHRONOLOG_RPC_CALL_WRAPPER_THALLIUM_SOCKETS()
                [[fallthrough]];
            CHRONOLOG_RPC_CALL_WRAPPER_THALLIUM_TCP() {
                protocol_ = CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.PROTO_CONF.string();
                break;
            }
            CHRONOLOG_RPC_CALL_WRAPPER_THALLIUM_ROCE() {
                protocol_ = CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.PROTO_CONF.string();
                break;
            }
        }
        baseIP_ = CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.VISOR_END_CONF.VISOR_IP.string();
        basePorts_ = CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.VISOR_END_CONF.VISOR_BASE_PORT;
        numPorts_ = CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.VISOR_END_CONF.VISOR_PORTS;
        numStreams_ = CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.VISOR_END_CONF.VISOR_SERVICE_THREADS;
        serverAddrVec_.reserve(CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.VISOR_END_CONF.VISOR_PORTS);
        for (int i = 0; i < numPorts_; i++) {
            std::string server_addr = protocol_ + "://" +
                                      baseIP_ + ":" +
                                      std::to_string(basePorts_ + i);
            serverAddrVec_.emplace_back(std::move(server_addr));
        }
        engineVec_.reserve(numPorts_);
        midVec_.reserve(numPorts_);
        rpcVisor_ = ChronoLog::Singleton<RPCVisor>::GetInstance();
    }
}
