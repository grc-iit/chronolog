//
// Created by kfeng on 12/26/22.
//

#ifndef CHRONOLOG_RPCVISOR_H
#define CHRONOLOG_RPCVISOR_H

#include <iostream>
#include <unordered_map>
#include <functional>
#include <thallium.hpp>
#include <sys/types.h>
#include <unistd.h>
#include "macro.h"
#include "errcode.h"
#include "RPCFactory.h"
#include "ClientRegistryManager.h"
#include "ChronicleMetaDirectory.h"

class RPCVisor {
public:
    RPCVisor() {
        LOGD("%s constructor is called", typeid(*this).name());
        rpc = std::make_shared<ChronoLogRPC>();
        set_prefix("ChronoLog");
        LOGD("%s constructor finishes, object created@%p in thread PID=%d",
             typeid(*this).name(), this, getpid());
        clientManager = ChronoLog::Singleton<ClientRegistryManager>::GetInstance();
        chronicleMetaDirectory = ChronoLog::Singleton<ChronicleMetaDirectory>::GetInstance();
    }

    ~RPCVisor()
    {
    }

    void Visor_start() {
        rpc->start();}

    void set_acl_db(ACL_DB *ad)
    {
	    chronicleMetaDirectory->set_acl_db(ad);
    }
    /**
     * Admin APIs
     */
    int LocalConnect(const std::string &uri, std::string &client_id, std::string &group_id, uint32_t &role, int &flags, uint64_t &clock_offset) {
        LOGD("%s in ChronoLogAdminRPCProxy@%p called in PID=%d, with args: uri=%s",
             __FUNCTION__, this, getpid(), uri.c_str());
        ClientInfo record;
	int npos = uri.find("://");
	std::string substring = uri.substr(npos);
	npos = substring.rfind(":");
	std::string ip = substring.substr(3,npos-3);
        record.addr_ = ip;
	if(group_id.empty()) group_id = "DEFAULT";
	record.group_id_ = group_id;
	record.client_role_ = role;
        
        if (std::strtol(client_id.c_str(), nullptr, 10) < 0) {
            LOGE("client id is invalid");
            return CL_ERR_INVALID_ARG;
        }
	return clientManager->add_client_record(client_id,record);
    }

    int LocalDisconnect(const std::string &client_id, int &flags) {
        LOGD("%s is called in PID=%d, with args: client_id=%s, flags=%d",
             __FUNCTION__, getpid(), client_id.c_str(), flags);
        if (std::strtol(client_id.c_str(), nullptr, 10) < 0) {
            LOGE("client id is invalid");
            return CL_ERR_INVALID_ARG;
        }
        return clientManager->remove_client_record(client_id,flags);
    }

    /**
     * Metadata APIs
     */
    int LocalCreateChronicle(std::string& name,
		             std::string& client_id,
                             const std::unordered_map<std::string, std::string>& attrs,
                             int& flags) {
        LOGD("%s is called in PID=%d, with args: name=%s, attrs=", __FUNCTION__, getpid(), name.c_str());
        for (auto iter = attrs.begin(); iter != attrs.end(); ++iter) {
            LOGD("%s=%s", iter->first.c_str(), iter->second.c_str());
        }

	std::string group_id; uint32_t role;

	int ret = clientManager->get_client_group_and_role(client_id,group_id,role);

        if(ret == CL_SUCCESS)
	{	
	   bool b = clientManager->can_create_or_delete(role);	

	  if (!name.empty() && b)
	  {
            ret = chronicleMetaDirectory->create_chronicle(name,client_id,group_id,attrs);
          } 
	  else  
	 {
	    if(name.empty())
	    {
              LOGE("name is empty");
	      ret = CL_ERR_INVALID_ARG;
	    }
	    if(!b)
	    {
	      LOGE("Client role cannot create new chronicles");
              ret = CL_ERR_UNKNOWN;
	    }
         }
	}
	else
	{
           LOGE("client_id is incorrect");
	   ret = CL_ERR_UNKNOWN;
	}
	return ret;
    }

    int LocalDestroyChronicle(std::string& name, std::string &client_id, int& flags) {
        LOGD("%s is called in PID=%d, with args: name=%s, flags=%d", __FUNCTION__, getpid(), name.c_str(), flags);
	std::string group_id; uint32_t role;

	int ret = clientManager->get_client_group_and_role(client_id, group_id,role);

	if(ret == CL_SUCCESS)
	{
	   bool b = clientManager->can_create_or_delete(role);

           if (!name.empty() && b) 
	   {
              ret = chronicleMetaDirectory->destroy_chronicle(name,client_id,group_id,flags);
           } 
	   else 
	  {
	    if(name.empty())
	    {
              LOGE("name is empty");
	      ret = CL_ERR_INVALID_ARG;
	    }
	    if(!b)
	    {
	      LOGE("Client role cannot delete chronicles");
              ret = CL_ERR_UNKNOWN;
	    }
          }
	}
	else
	{
	   LOGE("client_id is incorrect");
	   ret = CL_ERR_UNKNOWN;
	}
	return ret;
    }

    int LocalAcquireChronicle(std::string& name, std::string &client_id, int& flags) {
        LOGD("%s is called in PID=%d, with args: name=%s, flags=%d", __FUNCTION__, getpid(), name.c_str(), flags);
	std::string group_id; uint32_t role;
	int ret = clientManager->get_client_group_and_role(client_id,group_id,role);

	if(ret == CL_SUCCESS)
	{
	  enum ChronoLogOp op = (enum ChronoLogOp)flags;
	  bool b = false;

	  if(op == CHRONOLOG_READ) b = clientManager->can_read(role);
	  else if(op == CHRONOLOG_WRITE) b = clientManager->can_write(role);
	  else if(op == CHRONOLOG_FILEOP) b = clientManager->can_perform_fileops(role);

          if (!name.empty() && b) 
	  {
            ret = chronicleMetaDirectory->acquire_chronicle(name,client_id,group_id,flags);
          } 
	  else 
	  {
	    if(name.empty())
	    {
              LOGE("name is empty");
              ret = CL_ERR_INVALID_ARG;
	    }
	    if(!b)
	    {
	      LOGE("Role cannot perform this requested operation");
	      ret = CL_ERR_UNKNOWN;
	    }
         }
	}
	else
	{
	    LOGE("client_id is incorrect");
	    ret = CL_ERR_UNKNOWN;
	}
	return ret;
    }

    int LocalReleaseChronicle(std::string& name, std::string &client_id, int& flags) {
        LOGD("%s is called in PID=%d, with args: name=%s, flags=%d", __FUNCTION__, getpid(), name.c_str(), flags);
	std::string group_id; uint32_t role;
	int ret = clientManager->get_client_group_and_role(client_id,group_id,role);

	if(ret == CL_SUCCESS)
	{
          if (!name.empty()) 
	  {
            ret = chronicleMetaDirectory->release_chronicle(name,client_id,group_id,flags);
          } 
	  else 
	  {
            LOGE("name is empty");
            ret = CL_ERR_INVALID_ARG;
          }
	}
	else
	{
	   LOGE("client_id is incorrect");
	   ret = CL_ERR_UNKNOWN;
	}
	return ret;
    }
    int LocalUpdateChroniclePermissions(std::string &name,std::string &client_id,std::string &perm)
    {
	LOGD("%s is called in PID=%d with args: name=%s perm=%s",__FUNCTION__,getpid(),name.c_str(),perm.c_str());
	std::string group_id; uint32_t role;
	int ret = clientManager->get_client_group_and_role(client_id,group_id,role);
	if(ret == CL_SUCCESS)
	{
           bool b = clientManager->can_perform_fileops(role);
	   if(!name.empty() && b)
	   {
	       ret = chronicleMetaDirectory->update_chronicle_permissions(name,client_id,group_id,perm);
	   }
	   else
	   {
	       if(name.empty())
	       {
		  LOGE("name is empty");
		  ret = CL_ERR_INVALID_ARG;
	       }
	       if(!b)
	       {
		  LOGE("Role cannot perform the requested operation");
		  ret = CL_ERR_UNKNOWN;
	       }
	   }
	}
        else
	{
	  LOGE("client_id is incorrect");
          ret = CL_ERR_NOT_EXIST;
	}	  
	return ret;
    }
    int LocalCreateStory(std::string& chronicle_name,
                         std::string& story_name,
			 std::string& client_id,
                         const std::unordered_map<std::string, std::string>& attrs,
                         int& flags) {
        LOGD("%s is called in PID=%d, with args: chronicle_name=%s, story_name=%s, ,attrs=",
             __FUNCTION__, getpid(), chronicle_name.c_str(), story_name.c_str());
        for (auto iter = attrs.begin(); iter != attrs.end(); ++iter) {
            LOGD("%s=%s", iter->first.c_str(), iter->second.c_str());
        }
	std::string group_id; uint32_t role;
	int ret = clientManager->get_client_group_and_role(client_id,group_id,role);

	if(ret==CL_SUCCESS)
	{
	  bool b = clientManager->can_create_or_delete(role);

          if (!chronicle_name.empty() && !story_name.empty() && b) 
	  {
            ret = chronicleMetaDirectory->create_story(chronicle_name, story_name,client_id,group_id,attrs);
          } 
	  else 
	  {
            if (chronicle_name.empty() || story_name.empty())
	    {
                LOGE("chronicle name or story name is empty");
		ret = CL_ERR_INVALID_ARG;
	    }
	    if(!b)
	    {
		LOGE("Role does not support create or delete operations");
                ret = CL_ERR_UNKNOWN;
	    }
          }
	}
	else
	{
	    LOGE("client_id is incorrect");
	    ret = CL_ERR_UNKNOWN;
	}
	return ret;
    }

    int LocalDestroyStory(std::string& chronicle_name, std::string& story_name, std::string &client_id, int& flags) {
        LOGD("%s is called in PID=%d, with args: chronicle_name=%s, story_name=%s, flags=%d",
             __FUNCTION__, getpid(), chronicle_name.c_str(), story_name.c_str(), flags);
	std::string group_id; uint32_t role;
	int ret = clientManager->get_client_group_and_role(client_id,group_id,role);

	if(ret == CL_SUCCESS)
	{
	  bool b = clientManager->can_create_or_delete(role);

          if (!chronicle_name.empty() && !story_name.empty() && b) {
            ret = chronicleMetaDirectory->destroy_story(chronicle_name, story_name, client_id,group_id,flags);
          } else {
            if (chronicle_name.empty() || story_name.empty())
	    {
                LOGE("chronicle name or story name is empty");
		ret = CL_ERR_INVALID_ARG;
	    }
	    if(!b)
	    {
	        LOGE("Role does not support create or delete operations");
                ret = CL_ERR_UNKNOWN;
	    }
         }
	}
	else
	{
	    LOGE("client_id is incorrect");
	    ret = CL_ERR_UNKNOWN;
	}
	return ret;
    }

    int LocalAcquireStory(std::string& chronicle_name, std::string& story_name, std::string &client_id, int& flags) {
        LOGD("%s is called in PID=%d, with args: chronicle_name=%s, story_name=%s, flags=%d",
             __FUNCTION__, getpid(), chronicle_name.c_str(), story_name.c_str(), flags);
	std::string group_id; uint32_t role;
	int ret = clientManager->get_client_group_and_role(client_id,group_id,role);
        
	if(ret == CL_SUCCESS)
	{
	  enum ChronoLogOp op = (enum ChronoLogOp)flags;

	  bool b = false;

	  if(op == CHRONOLOG_READ) b = clientManager->can_read(role);
	  else if(op == CHRONOLOG_WRITE) b = clientManager->can_write(role);

          if (!chronicle_name.empty() && !story_name.empty() && b) {
            ret = chronicleMetaDirectory->acquire_story(chronicle_name, story_name,client_id,group_id,flags);
          } else {
            if (chronicle_name.empty() || story_name.empty())
	    {
                LOGE("chronicle name or story name is empty");
		ret = CL_ERR_INVALID_ARG;
	    }
	    if(!b)
	    {
	       LOGE("Role cannot perform requested operation");
               ret = CL_ERR_UNKNOWN;
	    }
          }
	}
	else
	{
	   LOGE("client_id is incorrect");
	   ret = CL_ERR_UNKNOWN;
	}
	return ret;
    }

    int LocalReleaseStory(std::string& chronicle_name, std::string& story_name, std::string &client_id, int& flags) {
        LOGD("%s is called in PID=%d, with args: chronicle_name=%s, story_name=%s, flags=%d",
             __FUNCTION__, getpid(), chronicle_name.c_str(), story_name.c_str(), flags);
	std::string group_id; uint32_t role;
	int ret = clientManager->get_client_group_and_role(client_id,group_id,role);
	if(ret == CL_SUCCESS)
	{
          if (!chronicle_name.empty() && !story_name.empty()) {
            ret = chronicleMetaDirectory->release_story(chronicle_name, story_name,client_id,group_id,flags);
          } else {
            if (chronicle_name.empty())
                LOGE("chronicle name is empty");
            if (story_name.empty())
                LOGE("story name is empty");
              ret = CL_ERR_INVALID_ARG;
          }
	}
	else
	{
	   LOGE("client_id is incorrect");
	   ret = CL_ERR_UNKNOWN;
	}
	return ret;
    }
    int LocalUpdateStoryPermissions(std::string &chronicle_name,std::string &story_name,std::string &client_id,std::string &perm)
    {
	LOGD("%s is called in PID=%d with args:chronicle_name=%s story_name=%s perm=%s",__FUNCTION__,getpid(),chronicle_name.c_str(),story_name.c_str(),perm.c_str());
	std::string group_id;uint32_t role;
	int ret = clientManager->get_client_group_and_role(client_id,group_id,role);
	if(ret == CL_SUCCESS)
	{
	   bool b = clientManager->can_perform_fileops(role);
	   if(!chronicle_name.empty() && !story_name.empty() && b)
	   {
		ret = chronicleMetaDirectory->update_story_permissions(chronicle_name,story_name,client_id,group_id,perm);
	   }
	   else
	   {
		if(chronicle_name.empty() || story_name.empty())
		{
		   LOGE("chronicle name or story name is empty");
		   ret = CL_ERR_INVALID_ARG;
		}
		if(!b)
		{
		   LOGE("Role cannot perform requested operation");
		   ret = CL_ERR_UNKNOWN;
		}
	   }
	}
	else
	{
	   LOGE("client_id is incorrect");
	   ret = CL_ERR_NOT_EXIST;
	}
	return ret;
    }
    int LocalGetChronicleAttr(std::string& name, const std::string& key, std::string &client_id, std::string& value) {
        LOGD("%s is called in PID=%d, with args: name=%s, key=%s", __FUNCTION__, getpid(), name.c_str(), key.c_str());
	std::string group_id; uint32_t role;
	int ret = clientManager->get_client_group_and_role(client_id,group_id,role);
	if(ret == CL_SUCCESS)
	{
          if (!name.empty() && !key.empty()) {
            ret = chronicleMetaDirectory->get_chronicle_attr(name, key, client_id,group_id,value);
          } else {
            if (name.empty())
                LOGE("name is empty");
            if (key.empty())
                LOGE("key is empty");
            ret = CL_ERR_INVALID_ARG;
          }
	}
	else
	{
	   LOGE("client_id is incorrect");
	   ret = CL_ERR_UNKNOWN;
	}
	return ret;
    }

    int LocalEditChronicleAttr(std::string& name, const std::string& key, std::string &client_id, const std::string& value) {
        LOGD("%s is called in PID=%d, with args: name=%s, key=%s, value=%s",
             __FUNCTION__, getpid(), name.c_str(), key.c_str(), value.c_str());
	std::string group_id; uint32_t role;
	int ret = clientManager->get_client_group_and_role(client_id,group_id,role);

	if(ret == CL_SUCCESS)
	{
           if (!name.empty() && !key.empty() && !value.empty()) {
            ret = chronicleMetaDirectory->edit_chronicle_attr(name, key, client_id,group_id,value);
           } else {
            if (name.empty() || key.empty())
	    {
                LOGE("name or key is empty");
                ret = CL_ERR_INVALID_ARG;
	    }
           }
	}
	else
	{
	   LOGE("client_id is incorrect");
	   ret = CL_ERR_UNKNOWN;
	}
	return ret;
    }

    int LocalRequestRoleChange(std::string &client_id,uint32_t &role)
    {
	LOGD("%s is called in PID=%d with args: client_id=%s role = %d",__FUNCTION__,getpid(),client_id.c_str(),role);
	std::string group_id;uint32_t prev_role;
	int ret = clientManager->get_client_group_and_role(client_id,group_id,prev_role);
	if(ret == CL_SUCCESS)
	{
	    ret =  clientManager->update_client_role(client_id,role);
	}
	else
	{
	   LOGE("client_id is incorrect");
	   ret = CL_ERR_UNKNOWN;
	}
	return ret;
    }
    
    int LocalAddGrouptoChronicle(std::string &chronicle_name,std::string &client_id,std::string &new_group_id,std::string &new_perm)
    {
	LOGD("%s is called in PID=%d with args: chronicle_name=%s client_id=%s new_group_id=%s",__FUNCTION__,getpid(),chronicle_name.c_str(),client_id.c_str(),new_group_id.c_str());
	std::string group_id; uint32_t role;
	int ret = clientManager->get_client_group_and_role(client_id,group_id,role);

        if(ret == CL_SUCCESS)
	{
	    bool b = clientManager->can_edit_ownership(role);

	    if(b)
	    {
	       ret = chronicleMetaDirectory->add_group_to_chronicle(chronicle_name,client_id,group_id,new_group_id,new_perm);
	    }
	    else
	    {
		LOGE("Role cannot perform requested operation");
		ret = CL_ERR_UNKNOWN;
	    }

	}
	else
	{
	    LOGE("client_id is incorrect");
	    ret = CL_ERR_UNKNOWN;
	}
	return ret;
    }

    int LocalRemoveGroupFromChronicle(std::string &chronicle_name,std::string &client_id,std::string &new_group_id)
    {
        LOGD("%s is called in PID=%d with args: chronicle_name=%s client_id=%s new_group_id=%s",__FUNCTION__,getpid(),chronicle_name.c_str(),client_id.c_str(),new_group_id.c_str());
        std::string group_id; uint32_t role;
        int ret = clientManager->get_client_group_and_role(client_id,group_id,role);

        if(ret == CL_SUCCESS)
        {
            bool b = clientManager->can_edit_ownership(role);

            if(b)
            {
               ret = chronicleMetaDirectory->remove_group_from_chronicle(chronicle_name,client_id,group_id,new_group_id);
            }
            else
            {
                LOGE("Role cannot perform requested operation");
                ret = CL_ERR_UNKNOWN;
            }

        }
        else
        {
            LOGE("client_id is incorrect");
            ret = CL_ERR_UNKNOWN;
        }
        return ret;
    }

    int LocalAddGrouptoStory(std::string &chronicle_name,std::string &story_name,std::string &client_id,std::string &new_group_id,std::string &new_perm)
    {
	LOGD("%s is called in PID=%d with args: chronicle_name=%s story_name=%s client_id=%s new_group_id=%s",__FUNCTION__,getpid(),chronicle_name.c_str(),story_name.c_str(),client_id.c_str(),new_group_id.c_str());
	std::string group_id; uint32_t role;
	int ret = clientManager->get_client_group_and_role(client_id,group_id,role);

	if(ret == CL_SUCCESS)
	{
	    bool b = clientManager->can_edit_ownership(role);

	    if(b)
	    {
		ret = chronicleMetaDirectory->add_group_to_story(chronicle_name,story_name,client_id,group_id,new_group_id,new_perm);
	    }
	    else
	    {
		    LOGE("Role cannot perform requested operation");
		    ret = CL_ERR_UNKNOWN;
	    }

	}
	else
	{
	    LOGE("client_id is incorrect");
	    ret = CL_ERR_UNKNOWN;
	}
	return ret;
    }
    int LocalRemoveGroupFromStory(std::string &chronicle_name,std::string &story_name,std::string &client_id,std::string &new_group_id)
    {
        LOGD("%s is called in PID=%d with args: chronicle_name=%s story_name=%s client_id=%s new_group_id=%s",__FUNCTION__,getpid(),chronicle_name.c_str(),story_name.c_str(),client_id.c_str(),new_group_id.c_str());
        std::string group_id; uint32_t role;
        int ret = clientManager->get_client_group_and_role(client_id,group_id,role);

        if(ret == CL_SUCCESS)
        {
            bool b = clientManager->can_edit_ownership(role);

            if(b)
            {
                ret = chronicleMetaDirectory->remove_group_from_story(chronicle_name,story_name,client_id,group_id,new_group_id);
            }
            else
            {
                    LOGE("Role cannot perform requested operation");
                    ret = CL_ERR_UNKNOWN;
            }

        }
        else
        {
            LOGE("client_id is incorrect");
            ret = CL_ERR_UNKNOWN;
        }
        return ret;
    }


    void bind_functions() {
        switch (CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.RPC_IMPLEMENTATION) {
            case CHRONOLOG_THALLIUM_SOCKETS:
            case CHRONOLOG_THALLIUM_TCP:
            case CHRONOLOG_THALLIUM_ROCE: {
                std::function<void(const tl::request &,
                                   const std::string &,
                                   std::string &,
				   std::string &,
				   uint32_t &,
                                   int &,
                                   uint64_t &)> connectFunc(
                        [this](auto && PH1,
                               auto && PH2,
                               auto && PH3,
                               auto && PH4,
                               auto && PH5,
			       auto && PH6,
			       auto && PH7) {
                            ThalliumLocalConnect(std::forward<decltype(PH1)>(PH1),
                                                 std::forward<decltype(PH2)>(PH2),
                                                 std::forward<decltype(PH3)>(PH3),
                                                 std::forward<decltype(PH4)>(PH4),
                                                 std::forward<decltype(PH5)>(PH5),
						 std::forward<decltype(PH6)>(PH6),
						 std::forward<decltype(PH7)>(PH7));
                        }
                );
                std::function<void(const tl::request &,
                                   std::string &,
                                   int &)> disconnectFunc(
                        [this](auto && PH1,
                               auto && PH2,
                               auto && PH3) {
                            ThalliumLocalDisconnect(std::forward<decltype(PH1)>(PH1),
                                                    std::forward<decltype(PH2)>(PH2),
                                                    std::forward<decltype(PH3)>(PH3));
                        }
                );
                std::function<void(const tl::request &,
                                   std::string &,
				   std::string &,
                                   const std::unordered_map<std::string, std::string> &,
                                   int &)> createChronicleFunc(
                        [this](auto && PH1,
                                auto && PH2,
                                auto && PH3,
                                auto && PH4,
				auto && PH5) {
                            ThalliumLocalCreateChronicle(std::forward<decltype(PH1)>(PH1),
                                                         std::forward<decltype(PH2)>(PH2),
                                                         std::forward<decltype(PH3)>(PH3),
                                                         std::forward<decltype(PH4)>(PH4),
							 std::forward<decltype(PH5)>(PH5));
                        }
                );
                std::function<void(const tl::request &,
                                   std::string &,
				   std::string &,
                                   int &)> destroyChronicleFunc(
                        [this](auto && PH1,
                                auto && PH2,
                                auto && PH3,
				auto && PH4) {
                            ThalliumLocalDestroyChronicle(std::forward<decltype(PH1)>(PH1),
                                                          std::forward<decltype(PH2)>(PH2),
                                                          std::forward<decltype(PH3)>(PH3),
							  std::forward<decltype(PH4)>(PH4)); }
                );
                std::function<void(const tl::request &,
                                   std::string &,
                                   std::string &,
                                   int &)> acquireChronicleFunc(
                        [this](auto && PH1,
                                auto && PH2,
				auto && PH3,
				auto && PH4) {
                            ThalliumLocalAcquireChronicle(std::forward<decltype(PH1)>(PH1),
                                                          std::forward<decltype(PH2)>(PH2),
							  std::forward<decltype(PH3)>(PH3),
							  std::forward<decltype(PH4)>(PH4)); }
                );
                std::function<void(const tl::request &,
				   std::string &,
				   std::string &,
                                   int &)> releaseChronicleFunc(
                        [this](auto && PH1,
                                auto && PH2,
                                auto && PH3,
				auto && PH4) {
                            ThalliumLocalReleaseChronicle(std::forward<decltype(PH1)>(PH1),
                                                          std::forward<decltype(PH2)>(PH2),
                                                          std::forward<decltype(PH3)>(PH3),
							  std::forward<decltype(PH4)>(PH4)); }
                );
                std::function<void(const tl::request &,
                                   std::string &,
                                   std::string &,
                                   std::string &)> updateChroniclePermFunc(
                        [this](auto && PH1,
                                auto && PH2,
                                auto && PH3,
                                auto && PH4) {
                            ThalliumLocalUpdateChroniclePermissions(std::forward<decltype(PH1)>(PH1),
                                                                    std::forward<decltype(PH2)>(PH2),
                                                                    std::forward<decltype(PH3)>(PH3),
                                                                    std::forward<decltype(PH4)>(PH4)); }
                );

                std::function<void(const tl::request &,
                                   std::string &,
                                   std::string &,
				   std::string &,
                                   const std::unordered_map<std::string, std::string> &,
                                   int &)> createStoryFunc(
                        [this](auto && PH1,
                                auto && PH2,
                                auto && PH3,
                                auto && PH4,
                                auto && PH5,
				auto && PH6) {
                            ThalliumLocalCreateStory(std::forward<decltype(PH1)>(PH1),
                                                     std::forward<decltype(PH2)>(PH2),
                                                     std::forward<decltype(PH3)>(PH3),
                                                     std::forward<decltype(PH4)>(PH4),
                                                     std::forward<decltype(PH5)>(PH5),
						     std::forward<decltype(PH6)>(PH6)); }
                );
                std::function<void(const tl::request &,
                                   std::string &,
                                   std::string &,
				   std::string &,
                                   int &)> destroyStoryFunc(
                        [this](auto && PH1,
                                auto && PH2,
                                auto && PH3,
                                auto && PH4,
				auto && PH5) {
                            ThalliumLocalDestroyStory(std::forward<decltype(PH1)>(PH1),
                                                      std::forward<decltype(PH2)>(PH2),
                                                      std::forward<decltype(PH3)>(PH3),
                                                      std::forward<decltype(PH4)>(PH4),
						      std::forward<decltype(PH5)>(PH5));
                        }
                );
                std::function<void(const tl::request &,
				                   std::string &,
                                   std::string &,
				   std::string &,
                                   int &)> acquireStoryFunc(
                        [this](auto && PH1,
                                auto && PH2,
                                auto && PH3,
                                auto && PH4,
				auto && PH5) {
                            ThalliumLocalAcquireStory(std::forward<decltype(PH1)>(PH1),
                                                      std::forward<decltype(PH2)>(PH2),
                                                      std::forward<decltype(PH3)>(PH3),
                                                      std::forward<decltype(PH4)>(PH4),
						      std::forward<decltype(PH5)>(PH5)); }
                );
                std::function<void(const tl::request &,
				                   std::string &,
                                   std::string &,
				   std::string &,
                                   int &)> releaseStoryFunc(
                        [this](auto && PH1,
                                auto && PH2,
                                auto && PH3,
                                auto && PH4,
				auto && PH5) {
                            ThalliumLocalReleaseStory(std::forward<decltype(PH1)>(PH1),
                                                      std::forward<decltype(PH2)>(PH2),
                                                      std::forward<decltype(PH3)>(PH3),
                                                      std::forward<decltype(PH4)>(PH4),
						      std::forward<decltype(PH5)>(PH5));
                        }
                );
                std::function<void(const tl::request &,
                                   std::string &,
                                   std::string &,
                                   std::string &,
                                   std::string &)> updateStoryPermFunc(
                        [this](auto && PH1,
                                auto && PH2,
                                auto && PH3,
                                auto && PH4,
                                auto && PH5) {
                            ThalliumLocalUpdateStoryPermissions(std::forward<decltype(PH1)>(PH1),
                                                                std::forward<decltype(PH2)>(PH2),
                                                                std::forward<decltype(PH3)>(PH3),
                                                                std::forward<decltype(PH4)>(PH4),
                                                                std::forward<decltype(PH5)>(PH5));
                        }
                );

                std::function<void(const tl::request &,
                                   std::string &name,
                                   const std::string &,
				   std::string &client_id,
                                   std::string &)> getChronicleAttrFunc(
                        [this](auto && PH1,
                                auto && PH2,
                                auto && PH3,
                                auto && PH4,
				auto && PH5) {
                            ThalliumLocalGetChronicleAttr(std::forward<decltype(PH1)>(PH1),
                                                          std::forward<decltype(PH2)>(PH2),
                                                          std::forward<decltype(PH3)>(PH3),
                                                          std::forward<decltype(PH4)>(PH4),
							  std::forward<decltype(PH5)>(PH5)); }
                );
                std::function<void(const tl::request &,
                                   std::string &name,
                                   const std::string &,
				   std::string &client_id,
                                   const std::string &)> editChronicleAttrFunc(
                        [this](auto && PH1,
                                auto && PH2,
                                auto && PH3,
                                auto && PH4,
				auto && PH5) {
                            ThalliumLocalEditChronicleAttr(std::forward<decltype(PH1)>(PH1),
                                                           std::forward<decltype(PH2)>(PH2),
                                                           std::forward<decltype(PH3)>(PH3),
                                                           std::forward<decltype(PH4)>(PH4),
							   std::forward<decltype(PH5)>(PH5)); }
                );

		 std::function<void(const tl::request &,
                                   std::string &,
                                   uint32_t &)> requestRoleChangeFunc(
                        [this](auto && PH1,
                               auto && PH2,
                               auto && PH3) {
                            ThalliumLocalRequestRoleChange(std::forward<decltype(PH1)>(PH1),
                                                 std::forward<decltype(PH2)>(PH2),
                                                 std::forward<decltype(PH3)>(PH3));
                        }
                );

		 std::function<void(const tl::request &,
                                   std::string &,
				   std::string &,
				   std::string &,
                                   std::string &)> addGrouptoChronicleFunc(
                        [this](auto && PH1,
                               auto && PH2,
			       auto && PH3,
			       auto && PH4,
                               auto && PH5) {
                            ThalliumLocalAddGrouptoChronicle(std::forward<decltype(PH1)>(PH1),
                                                 std::forward<decltype(PH2)>(PH2),
						 std::forward<decltype(PH3)>(PH3),
						 std::forward<decltype(PH4)>(PH4),
                                                 std::forward<decltype(PH5)>(PH5));
                        }
                );

		 std::function<void(const tl::request &,
                                   std::string &,
                                   std::string &,
                                   std::string &)> removeGroupFromChronicleFunc(
                        [this](auto && PH1,
                               auto && PH2,
                               auto && PH3,
                               auto && PH4) {
                            ThalliumLocalRemoveGroupFromChronicle(std::forward<decltype(PH1)>(PH1),
                                                 std::forward<decltype(PH2)>(PH2),
                                                 std::forward<decltype(PH3)>(PH3),
                                                 std::forward<decltype(PH4)>(PH4));
                        }
                );

		  std::function<void(const tl::request &,
                                   std::string &,
				   std::string &,
                                   std::string &,
				   std::string &,
                                   std::string &)> addGrouptoStoryFunc(
                        [this](auto && PH1,
                               auto && PH2,
                               auto && PH3,
			       auto && PH4,
			       auto && PH5,
                               auto && PH6) {
                            ThalliumLocalAddGrouptoStory(std::forward<decltype(PH1)>(PH1),
                                                 std::forward<decltype(PH2)>(PH2),
                                                 std::forward<decltype(PH3)>(PH3),
						 std::forward<decltype(PH4)>(PH4),
						 std::forward<decltype(PH5)>(PH5),
                                                 std::forward<decltype(PH6)>(PH6));
                        }
                );

		   std::function<void(const tl::request &,
                                   std::string &,
                                   std::string &,
                                   std::string &,
                                   std::string &)> removeGroupFromStoryFunc(
                        [this](auto && PH1,
                               auto && PH2,
                               auto && PH3,
                               auto && PH4,
                               auto && PH5) {
                            ThalliumLocalRemoveGroupFromStory(std::forward<decltype(PH1)>(PH1),
                                                 std::forward<decltype(PH2)>(PH2),
                                                 std::forward<decltype(PH3)>(PH3),
                                                 std::forward<decltype(PH4)>(PH4),
                                                 std::forward<decltype(PH5)>(PH5));
                        }
                );

    		rpc->bind("ChronoLogThalliumConnect", connectFunc);
                rpc->bind("ChronoLogThalliumDisconnect", disconnectFunc);

                rpc->bind("ChronoLogThalliumCreateChronicle", createChronicleFunc);
                rpc->bind("ChronoLogThalliumDestroyChronicle", destroyChronicleFunc);
                rpc->bind("ChronoLogThalliumAcquireChronicle", acquireChronicleFunc);
                rpc->bind("ChronoLogThalliumReleaseChronicle", releaseChronicleFunc);
		rpc->bind("ChronoLogThalliumUpdateChroniclePermissions",updateChroniclePermFunc);
                rpc->bind("ChronoLogThalliumCreateStory", createStoryFunc);
                rpc->bind("ChronoLogThalliumDestroyStory", destroyStoryFunc);
                rpc->bind("ChronoLogThalliumAcquireStory", acquireStoryFunc);
                rpc->bind("ChronoLogThalliumReleaseStory", releaseStoryFunc);
		rpc->bind("ChronoLogThalliumUpdateStoryPermission",updateStoryPermFunc);
                rpc->bind("ChronoLogThalliumGetChronicleAttr", getChronicleAttrFunc);
                rpc->bind("ChronoLogThalliumEditChronicleAttr", editChronicleAttrFunc);
		rpc->bind("ChronoLogThalliumRequestRoleChange", requestRoleChangeFunc);
		rpc->bind("ChronoLogThalliumAddGrouptoChronicle",addGrouptoChronicleFunc);
		rpc->bind("ChronoLogThalliumAddGrouptoStory",addGrouptoStoryFunc);
		rpc->bind("ChronoLogThalliumRemoveGroupFromChronicle",removeGroupFromChronicleFunc);
		rpc->bind("ChronoLogThalliumRemoveGroupFromStory",removeGroupFromStoryFunc);
            }
        }
    }

    CHRONOLOG_THALLIUM_DEFINE(LocalConnect, (uri, client_id, group_id, role, flags, clock_offset),
                              const std::string &uri, std::string &client_id, std::string &group_id, uint32_t &role, int &flags, uint64_t &clock_offset)
    CHRONOLOG_THALLIUM_DEFINE(LocalDisconnect, (client_id, flags), std::string &client_id, int &flags)

    CHRONOLOG_THALLIUM_DEFINE(LocalCreateChronicle, (name, client_id, attrs, flags),
                              std::string &name, std::string &client_id,const std::unordered_map<std::string, std::string> &attrs, int &flags)
    CHRONOLOG_THALLIUM_DEFINE(LocalDestroyChronicle, (name, client_id, flags), std::string &name, std::string & client_id, int &flags)
    CHRONOLOG_THALLIUM_DEFINE(LocalAcquireChronicle, (name, client_id, flags), std::string &name, std::string &client_id,int &flags)
    CHRONOLOG_THALLIUM_DEFINE(LocalReleaseChronicle, (name, client_id, flags), std::string &name, std::string &client_id, int &flags)
    CHRONOLOG_THALLIUM_DEFINE(LocalUpdateChroniclePermissions, (name,client_id,perm), std::string &name,std::string &client_id,std::string &perm)
    CHRONOLOG_THALLIUM_DEFINE(LocalCreateStory, (chronicle_name, story_name, client_id, attrs, flags),
                              std::string &chronicle_name,
                              std::string &story_name,
			      std::string &client_id,
                              const std::unordered_map<std::string, std::string> &attrs,
                              int &flags)
    CHRONOLOG_THALLIUM_DEFINE(LocalDestroyStory, (chronicle_name, story_name, client_id, flags),
                              std::string &chronicle_name, std::string &story_name, std::string &client_id, int &flags)
    CHRONOLOG_THALLIUM_DEFINE(LocalAcquireStory, (chronicle_name, story_name, client_id,flags),
                              std::string &chronicle_name, std::string &story_name, std::string &client_id,int &flags)
    CHRONOLOG_THALLIUM_DEFINE(LocalReleaseStory, (chronicle_name, story_name, client_id, flags),
                              std::string &chronicle_name, std::string &story_name, std::string &client_id, int &flags)
    CHRONOLOG_THALLIUM_DEFINE(LocalUpdateStoryPermissions, (chronicle_name,story_name,client_id,perm), std::string &chronicle_name,std::string &story_name,std::string &client_id,std::string &perm)
    CHRONOLOG_THALLIUM_DEFINE(LocalGetChronicleAttr, (name, key, client_id, value),
                              std::string &name, const std::string &key, std::string &client_id, std::string &value)
    CHRONOLOG_THALLIUM_DEFINE(LocalEditChronicleAttr, (name, key, client_id, value),
                              std::string &name, const std::string &key, std::string &client_id, const std::string &value)
    CHRONOLOG_THALLIUM_DEFINE(LocalRequestRoleChange, (client_id,role),
		    	      std::string &client_id,uint32_t role)
    CHRONOLOG_THALLIUM_DEFINE(LocalAddGrouptoChronicle,(name,client_id,new_group_id,new_perm),
		    	      std::string &name,std::string &client_id,std::string &new_group_id,std::string &new_perm)
    CHRONOLOG_THALLIUM_DEFINE(LocalAddGrouptoStory, (chronicle_name,story_name,client_id,new_group_id,new_perm),
		             std::string &chronicle_name,std::string &story_name,std::string &client_id,std::string &new_group_id,std::string &new_perm)
    CHRONOLOG_THALLIUM_DEFINE(LocalRemoveGroupFromChronicle,(name,client_id,new_group_id),
		    	     std::string &name,std::string &client_id,std::string &new_group_id)
    CHRONOLOG_THALLIUM_DEFINE(LocalRemoveGroupFromStory, (chronicle_name,story_name,client_id,new_group_id),
		             std::string &chronicle_name,std::string &story_name,std::string &client_id,std::string &new_group_id)
private:
    void set_prefix(std::string prefix) {
        func_prefix = prefix;
        switch (CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.RPC_IMPLEMENTATION) {
            case CHRONOLOG_THALLIUM_SOCKETS:
            case CHRONOLOG_THALLIUM_TCP:
            case CHRONOLOG_THALLIUM_ROCE:
                func_prefix += "Thallium";
                break;
        }
    }

    ChronoLogCharStruct func_prefix;
    std::shared_ptr<ChronoLogRPC> rpc;
    std::shared_ptr<ClientRegistryManager> clientManager;
    std::shared_ptr<ChronicleMetaDirectory> chronicleMetaDirectory;
};


#endif //CHRONOLOG_RPCVISOR_H
